/* Translates the real QEMU USB Tablet (absolute ABS_X/ABS_Y + BTN_LEFT,
 * classified by udev as ID_INPUT_MOUSE) into a synthetic multitouch device
 * via uinput (ABS_MT_SLOT/TRACKING_ID/POSITION_X/Y protocol B + BTN_TOUCH,
 * INPUT_PROP_DIRECT so udev tags it ID_INPUT_TOUCHSCREEN). Reuses the
 * uinput setup pattern from shims/vnctouchbridge/vnctouchbridge.c, but with
 * a plain evdev source instead of parsing Engine's own VNC server protocol
 * — this Qt6/eglfs build has no VNC QPA plugin at all (see BUILDING.md's
 * arm64/RK3588 section), so there's no "Engine's own VNC server" to proxy;
 * QEMU's own -vnc server already injects clicks into the real USB tablet,
 * which the guest kernel sees as normal evdev events. What's missing is
 * only the mouse-vs-touch translation this file provides.
 *
 * Which /dev/input/eventN node the tablet lands on isn't stable — it's
 * assigned in device-registration order, so it shifts whenever the QEMU
 * device list changes (e.g. removing a passed-through USB controller
 * shifted it from event2 down to event0 during this project's own
 * bring-up). Rather than hardcode a number, find_tablet_device() scans
 * /dev/input/event* at startup and picks the one whose EVIOCGNAME matches
 * QEMU's tablet and which actually reports ABS_X/ABS_Y — self-healing
 * across device-list changes the same way dtshim_rmz2.c's dynamic
 * /proc/interrupts generation is. An explicit path can still be passed as
 * an override (see main()'s argument handling) for testing against
 * something else.
 *
 * Grabs the source device (EVIOCGRAB) so Qt's own evdevmouse handler for
 * it goes quiet — only genuine touch events should reach Engine, since a
 * DJ touchscreen UI's QML may only wire up TapHandler/MouseArea for touch
 * semantics rather than a real mouse pointer.
 *
 * Screen resolution is auto-detected from /sys/class/drm at startup
 * (detect_screen_size_from_sysfs()) rather than hardcoded, so it doesn't
 * need to be kept in sync by hand with whatever xres/yres a qemu launch
 * script passes to virtio-gpu-pci — same self-healing motivation as
 * find_tablet_device() above. An explicit <width> <height> can still be
 * passed to override it (see main()'s argument handling).
 *
 * The source device's ABS_X/ABS_Y range is read from the device itself via
 * EVIOCGABS at startup, not assumed/hardcoded — QEMU's usb-tablet reports
 * 0..32767 today, but this works unchanged if that ever changes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <linux/input.h>

static int uifd = -1;
static int screen_w = 1280, screen_h = 800;
static int touch_active = 0;

#define BITS_PER_LONG (8 * (int)sizeof(long))
#define ABS_BITS_LEN ((ABS_MAX / BITS_PER_LONG) + 1)

static int bit_is_set(const unsigned long *bits, int bit) {
    return (bits[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1;
}

/* Auto-detects the current display resolution from sysfs instead of
 * assuming a fixed default that has to be hand-kept in sync with whatever
 * xres/yres a qemu launch script passes to virtio-gpu-pci. Each DRM
 * connector directory under /sys/class/drm (e.g. "card0-Virtual-1") has a
 * "modes" file listing every mode it advertises, with the driver's actual
 * currently configured mode always first: confirmed directly, with
 * -device virtio-gpu-pci,...,xres=1280,yres=800 on the qemu command line,
 * the first line reads exactly "1280x800", followed by a long list of
 * generic fallback resolutions no display was ever set to. Only
 * "connected" connectors are considered, and the connector's directory
 * name itself isn't assumed — only that it lives under /sys/class/drm and
 * contains a dash (cardN, renderDxxx, and version don't). */
static int detect_screen_size_from_sysfs(int *out_w, int *out_h) {
    DIR *d = opendir("/sys/class/drm");
    if (!d) return -1;

    int found = -1;
    struct dirent *entry;
    while (found != 0 && (entry = readdir(d)) != NULL) {
        if (!strchr(entry->d_name, '-')) continue;

        char status_path[320];
        snprintf(status_path, sizeof(status_path), "/sys/class/drm/%s/status", entry->d_name);
        FILE *sf = fopen(status_path, "r");
        if (!sf) continue;
        char status[32] = {0};
        char *got = fgets(status, sizeof(status), sf);
        fclose(sf);
        if (!got || strncmp(status, "connected", 9) != 0) continue;

        char modes_path[320];
        snprintf(modes_path, sizeof(modes_path), "/sys/class/drm/%s/modes", entry->d_name);
        FILE *mf = fopen(modes_path, "r");
        if (!mf) continue;
        int w = 0, h = 0;
        int matched = fscanf(mf, "%dx%d", &w, &h);
        fclose(mf);
        if (matched == 2 && w > 0 && h > 0) {
            *out_w = w;
            *out_h = h;
            found = 0;
        }
    }
    closedir(d);
    return found;
}

static int device_looks_like_tablet(int fd, const char *name) {
    /* "USB Tablet" on machines with USB; "Virtio Tablet" on QEMU's 32-bit virt,
     * which has no PCI and therefore no reachable USB controller. */
    if (!strstr(name, "USB Tablet") && !strstr(name, "Virtio Tablet")) return 0;

    unsigned long absbits[ABS_BITS_LEN];
    memset(absbits, 0, sizeof(absbits));
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0) return 0;

    return bit_is_set(absbits, ABS_X) && bit_is_set(absbits, ABS_Y);
}

/* Scans /dev/input/event* for the device QEMU's usb-tablet actually is
 * right now, rather than assuming a fixed number. Returns a malloc'd path
 * on success (caller frees), NULL if nothing matched. */
static char *find_tablet_device(void) {
    DIR *d = opendir("/dev/input");
    if (!d) {
        fprintf(stderr, "opendir /dev/input failed: %s\n", strerror(errno));
        return NULL;
    }

    char *found_path = NULL;
    char found_name[256] = {0};
    int nmatches = 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;

        char path[sizeof("/dev/input/") + sizeof(entry->d_name)];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;

        char name[256] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) name[0] = '\0';

        if (device_looks_like_tablet(fd, name)) {
            nmatches++;
            fprintf(stderr, "Candidate tablet device: %s (\"%s\")%s\n", path, name,
                    nmatches > 1 ? " [extra match, keeping first]" : "");
            if (nmatches == 1) {
                found_path = strdup(path);
                strncpy(found_name, name, sizeof(found_name) - 1);
            }
        }
        close(fd);
    }
    closedir(d);

    if (!found_path) {
        fprintf(stderr, "No device matching \"USB Tablet\" with ABS_X/ABS_Y found under /dev/input/\n");
        return NULL;
    }
    if (nmatches > 1) {
        fprintf(stderr, "Warning: %d candidate tablet devices found, using %s (\"%s\")\n",
                nmatches, found_path, found_name);
    }
    return found_path;
}

static void emit(int type, int code, int val) {
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    ie.type = type;
    ie.code = code;
    ie.value = val;
    if (write(uifd, &ie, sizeof(ie)) < 0) {
        fprintf(stderr, "uinput write failed: %s\n", strerror(errno));
    }
}

static void sync_report(void) { emit(EV_SYN, SYN_REPORT, 0); }

static void touch_down(int x, int y) {
    emit(EV_ABS, ABS_MT_SLOT, 0);
    emit(EV_ABS, ABS_MT_TRACKING_ID, 1);
    emit(EV_KEY, BTN_TOUCH, 1);
    emit(EV_ABS, ABS_MT_POSITION_X, x);
    emit(EV_ABS, ABS_MT_POSITION_Y, y);
    emit(EV_ABS, ABS_X, x);
    emit(EV_ABS, ABS_Y, y);
    sync_report();
}

static void touch_move(int x, int y) {
    emit(EV_ABS, ABS_MT_SLOT, 0);
    emit(EV_ABS, ABS_MT_POSITION_X, x);
    emit(EV_ABS, ABS_MT_POSITION_Y, y);
    emit(EV_ABS, ABS_X, x);
    emit(EV_ABS, ABS_Y, y);
    sync_report();
}

static void touch_up(void) {
    emit(EV_ABS, ABS_MT_SLOT, 0);
    emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(EV_KEY, BTN_TOUCH, 0);
    sync_report();
}

static void setup_uinput_device(void) {
    uifd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uifd < 0) {
        fprintf(stderr, "open /dev/uinput failed: %s\n", strerror(errno));
        exit(1);
    }

    ioctl(uifd, UI_SET_EVBIT, EV_KEY);
    ioctl(uifd, UI_SET_KEYBIT, BTN_TOUCH);

    ioctl(uifd, UI_SET_EVBIT, EV_ABS);
    ioctl(uifd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(uifd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(uifd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(uifd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(uifd, UI_SET_ABSBIT, ABS_X);
    ioctl(uifd, UI_SET_ABSBIT, ABS_Y);
    ioctl(uifd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5680;
    strcpy(usetup.name, "RMZ2TouchBridge Virtual Touchscreen");
    ioctl(uifd, UI_DEV_SETUP, &usetup);

    struct uinput_abs_setup abs_x = { .code = ABS_X, .absinfo = { .minimum = 0, .maximum = screen_w } };
    struct uinput_abs_setup abs_y = { .code = ABS_Y, .absinfo = { .minimum = 0, .maximum = screen_h } };
    struct uinput_abs_setup abs_mtx = { .code = ABS_MT_POSITION_X, .absinfo = { .minimum = 0, .maximum = screen_w } };
    struct uinput_abs_setup abs_mty = { .code = ABS_MT_POSITION_Y, .absinfo = { .minimum = 0, .maximum = screen_h } };
    struct uinput_abs_setup abs_slot = { .code = ABS_MT_SLOT, .absinfo = { .minimum = 0, .maximum = 9 } };
    struct uinput_abs_setup abs_id = { .code = ABS_MT_TRACKING_ID, .absinfo = { .minimum = 0, .maximum = 65535 } };

    ioctl(uifd, UI_ABS_SETUP, &abs_x);
    ioctl(uifd, UI_ABS_SETUP, &abs_y);
    ioctl(uifd, UI_ABS_SETUP, &abs_mtx);
    ioctl(uifd, UI_ABS_SETUP, &abs_mty);
    ioctl(uifd, UI_ABS_SETUP, &abs_slot);
    ioctl(uifd, UI_ABS_SETUP, &abs_id);

    if (ioctl(uifd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "UI_DEV_CREATE failed: %s\n", strerror(errno));
        exit(1);
    }
    usleep(300000);
    fprintf(stderr, "Virtual touchscreen created (%dx%d).\n", screen_w, screen_h);
}

int main(int argc, char **argv) {
    /* argc==1: auto-discover device, auto-detect screen size from sysfs
     *          (falls back to the compiled-in default if that fails).
     * argc==3: <width> <height> — auto-discover device, explicit size.
     * argc==4: <event-device> <width> <height> — fully explicit override,
     *          e.g. for testing against a device that isn't QEMU's
     *          usb-tablet, or a host where /sys/class/drm doesn't apply. */
    const char *src_path = NULL;
    char *discovered = NULL;

    if (argc == 1) {
        if (detect_screen_size_from_sysfs(&screen_w, &screen_h) == 0) {
            fprintf(stderr, "Auto-detected screen resolution: %dx%d\n", screen_w, screen_h);
        } else {
            fprintf(stderr, "Could not auto-detect screen resolution from /sys/class/drm; "
                             "falling back to default %dx%d. Pass <width> <height> "
                             "explicitly to override.\n", screen_w, screen_h);
        }
    } else if (argc == 3) {
        screen_w = atoi(argv[1]);
        screen_h = atoi(argv[2]);
    } else if (argc == 4) {
        src_path = argv[1];
        screen_w = atoi(argv[2]);
        screen_h = atoi(argv[3]);
    } else {
        fprintf(stderr, "usage: %s [<event-device>] [<width> <height>]\n", argv[0]);
        return 1;
    }

    if (!src_path) {
        discovered = find_tablet_device();
        if (!discovered) {
            fprintf(stderr, "Could not auto-discover the QEMU USB Tablet device; "
                             "pass its /dev/input/eventN path explicitly as the "
                             "first of three arguments if this persists.\n");
            return 1;
        }
        src_path = discovered;
    }
    fprintf(stderr, "Using source device: %s\n", src_path);

    /* discovered (if non-NULL) is deliberately never freed: src_path keeps
     * pointing into it for the rest of main(), and this is a long-running
     * daemon — the OS reclaims it on exit either way. */
    int srcfd = open(src_path, O_RDONLY);
    if (srcfd < 0) {
        fprintf(stderr, "open %s failed: %s\n", src_path, strerror(errno));
        return 1;
    }

    struct input_absinfo abs_x_info, abs_y_info;
    if (ioctl(srcfd, EVIOCGABS(ABS_X), &abs_x_info) < 0 ||
        ioctl(srcfd, EVIOCGABS(ABS_Y), &abs_y_info) < 0) {
        fprintf(stderr, "EVIOCGABS on %s failed: %s\n", src_path, strerror(errno));
        return 1;
    }
    fprintf(stderr, "Source %s: ABS_X [%d,%d] ABS_Y [%d,%d]\n", src_path,
            abs_x_info.minimum, abs_x_info.maximum, abs_y_info.minimum, abs_y_info.maximum);

    if (ioctl(srcfd, EVIOCGRAB, 1) < 0) {
        fprintf(stderr, "EVIOCGRAB on %s failed (continuing ungrabbed): %s\n", src_path, strerror(errno));
    }

    setup_uinput_device();

    struct input_event ev;
    int have_x = 0, have_y = 0;
    int raw_x = 0, raw_y = 0;
    int btn_down = 0;
    int cur_x = 0, cur_y = 0;

    for (;;) {
        ssize_t n = read(srcfd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) {
            if (n < 0 && errno == EINTR) continue;
            fprintf(stderr, "source read ended: %s\n", n < 0 ? strerror(errno) : "EOF");
            break;
        }

        if (ev.type == EV_ABS && ev.code == ABS_X) {
            raw_x = ev.value;
            have_x = 1;
        } else if (ev.type == EV_ABS && ev.code == ABS_Y) {
            raw_y = ev.value;
            have_y = 1;
        } else if (ev.type == EV_KEY && ev.code == BTN_LEFT) {
            btn_down = ev.value;
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            if (have_x) {
                long long range = abs_x_info.maximum - abs_x_info.minimum;
                cur_x = range > 0 ? (int)(((long long)(raw_x - abs_x_info.minimum) * screen_w) / range) : 0;
                if (cur_x < 0) cur_x = 0;
                if (cur_x >= screen_w) cur_x = screen_w - 1;
            }
            if (have_y) {
                long long range = abs_y_info.maximum - abs_y_info.minimum;
                cur_y = range > 0 ? (int)(((long long)(raw_y - abs_y_info.minimum) * screen_h) / range) : 0;
                if (cur_y < 0) cur_y = 0;
                if (cur_y >= screen_h) cur_y = screen_h - 1;
            }

            if (btn_down && !touch_active) {
                touch_down(cur_x, cur_y);
                touch_active = 1;
            } else if (btn_down && touch_active) {
                touch_move(cur_x, cur_y);
            } else if (!btn_down && touch_active) {
                touch_up();
                touch_active = 0;
            }
        }
    }

    if (touch_active) touch_up();
    return 0;
}
