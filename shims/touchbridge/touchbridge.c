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
 * across device-list changes the same way dtshim.c's dynamic
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
 *
 * One instance per display. JP22 has three screens, and a single tablet cannot
 * serve them: every window would drive the same absolute device and the guest
 * could not tell which screen a click landed on. The launcher instead gives each
 * head its own usb-tablet (QEMU's display=/head= properties, see
 * scripts/qemu/arch_devices.sh), so this runs once per head — `--head N` picks
 * the Nth tablet, sizes itself from that head's DRM connector, and publishes
 * /dev/input/qengine-touchN for the matching output's touchDevice to resolve
 * through. Single-screen devices are just the N=1 case and behave as before.
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
static const char *opt_symlink = NULL;
static const char *opt_connector = NULL;
static int opt_index = 0;
static int opt_check = 0;
static int opt_wait = 0;

/* Defined below main()'s helpers but called from the uinput setup above them. */
static void publish_symlink(const char *link_path);

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
/* want_connector selects a specific connector by the tail of its sysfs name
 * ("Virtual-2" matches "card0-Virtual-2"); NULL keeps the original behaviour of
 * taking the first connected one. One instance runs per head and each needs its
 * own screen's geometry, which need not match the others'. */
static int detect_screen_size_from_sysfs(const char *want_connector, int *out_w, int *out_h) {
    DIR *d = opendir("/sys/class/drm");
    if (!d) return -1;

    int found = -1;
    struct dirent *entry;
    while (found != 0 && (entry = readdir(d)) != NULL) {
        if (!strchr(entry->d_name, '-')) continue;
        if (want_connector) {
            size_t nlen = strlen(entry->d_name), wlen = strlen(want_connector);
            if (wlen > nlen || strcmp(entry->d_name + (nlen - wlen), want_connector) != 0)
                continue;
        }

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
/* Picks the `want`th (0-based) matching tablet, in ascending /dev/input/eventN
 * order.
 *
 * With GPU_MAX_OUTPUTS>1 the launcher adds one usb-tablet per head, each bound
 * to its own scanout via QEMU's display=/head= properties — that binding is what
 * lets the guest tell which window a click came from, since otherwise all three
 * windows drive one absolute device and arrive indistinguishable. All of them
 * report the same EVIOCGNAME, so they can only be told apart by position, and
 * QEMU registers them in command-line order. readdir() does not return entries
 * in any particular order, so the matches are sorted by event number before
 * indexing or "the second tablet" would vary run to run. */
static char *find_tablet_device(int want) {
    DIR *d = opendir("/dev/input");
    if (!d) {
        fprintf(stderr, "opendir /dev/input failed: %s\n", strerror(errno));
        return NULL;
    }

    int matches[64];
    char names[64][256];
    int nmatches = 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && nmatches < (int)(sizeof(matches) / sizeof(matches[0]))) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;

        char path[sizeof("/dev/input/") + sizeof(entry->d_name)];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;

        char name[256] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) name[0] = '\0';

        if (device_looks_like_tablet(fd, name)) {
            matches[nmatches] = atoi(entry->d_name + 5);
            snprintf(names[nmatches], sizeof(names[0]), "%s", name);
            nmatches++;
        }
        close(fd);
    }
    closedir(d);

    if (nmatches == 0) {
        fprintf(stderr, "No device matching \"USB Tablet\" with ABS_X/ABS_Y found under /dev/input/\n");
        return NULL;
    }

    /* Insertion sort on event number, carrying the names along. n is at most the
     * number of input devices, so anything cleverer would be noise. */
    for (int i = 1; i < nmatches; i++) {
        int kn = matches[i];
        char kname[256];
        snprintf(kname, sizeof(kname), "%s", names[i]);
        int j = i - 1;
        while (j >= 0 && matches[j] > kn) {
            matches[j + 1] = matches[j];
            snprintf(names[j + 1], sizeof(names[0]), "%s", names[j]);
            j--;
        }
        matches[j + 1] = kn;
        snprintf(names[j + 1], sizeof(names[0]), "%s", kname);
    }

    for (int i = 0; i < nmatches; i++)
        fprintf(stderr, "Candidate tablet device: /dev/input/event%d (\"%s\")%s\n",
                matches[i], names[i], i == want ? " [selected]" : "");

    if (want >= nmatches) {
        fprintf(stderr, "Requested tablet index %d but only %d candidate(s) exist. "
                        "Is GPU_MAX_OUTPUTS set high enough on the launcher?\n",
                want, nmatches);
        return NULL;
    }

    char path[64];
    snprintf(path, sizeof(path), "/dev/input/event%d", matches[want]);
    return strdup(path);
}

/* find_tablet_device() with a bounded retry.
 *
 * These units start before USB enumeration has finished, so a single lookup at
 * startup routinely finds nothing — the tablets appear a second or two later.
 * Polling here rather than letting the process die and be restarted keeps the
 * failure modes distinct: exhausting the wait means "this head has no tablet"
 * (a single-screen guest running a per-head unit), which is a clean exit rather
 * than something to restart. */
static char *find_tablet_device_waiting(int want, int wait_secs) {
    const int poll_ms = 200;
    int tries = wait_secs > 0 ? (wait_secs * 1000) / poll_ms : 0;
    for (int i = 0;; i++) {
        char *p = find_tablet_device(want);
        if (p) return p;
        if (i >= tries) return NULL;
        if (i == 0)
            fprintf(stderr, "Tablet %d not present yet; waiting up to %ds for USB enumeration.\n",
                    want, wait_secs);
        usleep(poll_ms * 1000);
    }
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
    /* Distinct per instance, so neither udev nor a human reading
     * /proc/bus/input/devices has to guess which screen a node belongs to.
     *
     * Head 0 deliberately keeps the original unsuffixed name: it is the only one
     * that existed before multi-head, and anything out there matching on it — a
     * udev rule, a debugging habit — should not break just because the program
     * learned to run more than one copy of itself. */
    if (opt_index == 0)
        snprintf(usetup.name, sizeof(usetup.name), "TouchBridge Virtual Touchscreen");
    else
        snprintf(usetup.name, sizeof(usetup.name),
                 "TouchBridge Virtual Touchscreen %d", opt_index);
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
    if (opt_symlink) publish_symlink(opt_symlink);
    fprintf(stderr, "Virtual touchscreen created (%dx%d).\n", screen_w, screen_h);
}

/* Publishes a stable path for the uinput device just created.
 *
 * Engine's Hardware::updateTouchDevicePathsInConfig() resolves each output's
 * touchDevice as a symlink and substitutes the target before handing the config
 * to Qt, so the value in ScreenConfiguration.json has to *be* a symlink — given a
 * plain device node it logs "Could not resolve symlink for:" and drops the
 * binding, which is exactly what leaves every touch going to the primary screen.
 * The kernel's own eventN number depends on registration order, so the link is
 * built from the sysfs name uinput hands back rather than guessed. */
static void publish_symlink(const char *link_path) {
    char sysname[64] = {0};
    if (ioctl(uifd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0) {
        fprintf(stderr, "UI_GET_SYSNAME failed (%s); not publishing %s\n",
                strerror(errno), link_path);
        return;
    }

    char dirpath[192];
    snprintf(dirpath, sizeof(dirpath), "/sys/class/input/%s", sysname);
    DIR *d = opendir(dirpath);
    if (!d) {
        fprintf(stderr, "opendir %s failed (%s); not publishing %s\n",
                dirpath, strerror(errno), link_path);
        return;
    }
    char evname[256] = {0};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "event", 5) == 0) {
            snprintf(evname, sizeof(evname), "%s", e->d_name);
            break;
        }
    }
    closedir(d);
    if (!evname[0]) {
        fprintf(stderr, "no eventN under %s; not publishing %s\n", dirpath, link_path);
        return;
    }

    char target[96];
    snprintf(target, sizeof(target), "/dev/input/%s", evname);
    /* Stale link from a previous run of this instance: /dev is a tmpfs that does
     * not survive reboot, but the service can restart within one boot. */
    unlink(link_path);
    if (symlink(target, link_path) < 0)
        fprintf(stderr, "symlink %s -> %s failed: %s\n", link_path, target, strerror(errno));
    else
        fprintf(stderr, "Published %s -> %s\n", link_path, target);
}

int main(int argc, char **argv) {
    /* Flags first, then the historical positional forms:
     *   (none)                       auto-discover device, auto-detect size
     *   <width> <height>             auto-discover device, explicit size
     *   <event-device> <w> <h>       fully explicit
     *
     * Flags, all optional and all needed only for the multi-head case:
     *   --head N         shorthand for --index N with the connector and symlink
     *                    conventions that go with head N; what the per-head
     *                    systemd template passes
     *   --check          exit 0 if that tablet exists, 1 if it does not, without
     *                    creating anything — for systemd ExecCondition=
     *   --wait SECS      keep looking for that tablet for up to SECS before
     *                    giving up (--head defaults it to 20)
     *   --index N        use the Nth matching tablet (0-based, event order)
     *   --connector NAME take the size from that DRM connector ("Virtual-2")
     *   --symlink PATH   publish a stable path for Engine's touchDevice
     */
    const char *src_path = NULL;
    char *discovered = NULL;
    static char head_connector[32];
    static char head_symlink[64];

    int argi = 1;
    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strcmp(argv[argi], "--index") != 0 &&
            strcmp(argv[argi], "--head") != 0 &&
            strcmp(argv[argi], "--connector") != 0 &&
            strcmp(argv[argi], "--wait") != 0 &&
            strcmp(argv[argi], "--symlink") != 0) {
            /* --check takes no value, so it is handled before the pairing rule. */
            if (strcmp(argv[argi], "--check") == 0) { opt_check = 1; argi++; continue; }
            fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[argi]);
            return 1;
        }
        if (argi + 1 >= argc) {
            fprintf(stderr, "%s: %s needs a value\n", argv[0], argv[argi]);
            return 1;
        }
        if (strcmp(argv[argi], "--index") == 0 || strcmp(argv[argi], "--head") == 0) {
            opt_index = atoi(argv[argi + 1]);
            if (opt_index < 0) {
                fprintf(stderr, "%s: %s must not be negative\n", argv[0], argv[argi]);
                return 1;
            }
            /* --head is --index plus the two conventions that follow from it, so a
             * systemd template can pass just "--head %i". virtio-gpu names its
             * connectors Virtual-1..Virtual-N in head order, and each instance
             * needs a distinct symlink for its output's touchDevice. Both remain
             * overridable by giving --connector/--symlink after --head. */
            if (strcmp(argv[argi], "--head") == 0) {
                snprintf(head_connector, sizeof(head_connector), "Virtual-%d", opt_index + 1);
                snprintf(head_symlink, sizeof(head_symlink), "/dev/input/qengine-touch%d", opt_index);
                opt_connector = head_connector;
                opt_symlink = head_symlink;
                if (opt_wait == 0) opt_wait = 20;
            }
        } else if (strcmp(argv[argi], "--wait") == 0) {
            opt_wait = atoi(argv[argi + 1]);
        } else if (strcmp(argv[argi], "--connector") == 0) {
            opt_connector = argv[argi + 1];
        } else {
            opt_symlink = argv[argi + 1];
        }
        argi += 2;
    }

    int npos = argc - argi;
    if (npos == 0) {
        if (detect_screen_size_from_sysfs(opt_connector, &screen_w, &screen_h) == 0) {
            fprintf(stderr, "Auto-detected screen resolution: %dx%d\n", screen_w, screen_h);
        } else {
            fprintf(stderr, "Could not auto-detect screen resolution from /sys/class/drm%s%s; "
                             "falling back to default %dx%d. Pass <width> <height> "
                             "explicitly to override.\n",
                    opt_connector ? " for connector " : "",
                    opt_connector ? opt_connector : "", screen_w, screen_h);
        }
    } else if (npos == 2) {
        screen_w = atoi(argv[argi]);
        screen_h = atoi(argv[argi + 1]);
    } else if (npos == 3) {
        src_path = argv[argi];
        screen_w = atoi(argv[argi + 1]);
        screen_h = atoi(argv[argi + 2]);
    } else {
        fprintf(stderr, "usage: %s [--head N | --index N] [--connector NAME] "
                        "[--symlink PATH] [<event-device>] [<width> <height>]\n", argv[0]);
        return 1;
    }

    /* Probe only. A template instance for a head this launch did not create
     * should stand down quietly rather than restart-loop, and ExecCondition=
     * treats a non-zero exit as "skip this unit", not as a failure. */
    if (opt_check) {
        char *probe = find_tablet_device_waiting(opt_index, opt_wait);
        if (!probe) return 1;
        free(probe);
        return 0;
    }

    if (!src_path) {
        discovered = find_tablet_device_waiting(opt_index, opt_wait);
        if (!discovered) {
            /* A per-head unit on a guest with fewer heads than instances: report
             * it and exit *successfully*, so `Restart=on-failure` leaves it alone
             * instead of looping forever on hardware that will never appear. */
            if (opt_wait > 0) {
                fprintf(stderr, "No tablet for head %d after %ds — this guest has fewer "
                                "displays than that. Nothing to bridge; exiting.\n",
                        opt_index, opt_wait);
                return 0;
            }
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
