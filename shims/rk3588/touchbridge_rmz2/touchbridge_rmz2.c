/* Translates the real QEMU USB Tablet (/dev/input/event2 — absolute
 * ABS_X/ABS_Y + BTN_LEFT, classified by udev as ID_INPUT_MOUSE) into a
 * synthetic multitouch device via uinput (ABS_MT_SLOT/TRACKING_ID/
 * POSITION_X/Y protocol B + BTN_TOUCH, INPUT_PROP_DIRECT so udev tags it
 * ID_INPUT_TOUCHSCREEN). Reuses the uinput setup pattern from
 * shims/vnctouchbridge/vnctouchbridge.c, but with a plain evdev source
 * instead of parsing Engine's own VNC server protocol — this Qt6/eglfs
 * build has no VNC QPA plugin at all (see BUILDING.md's arm64/RK3588
 * section), so there's no "Engine's own VNC server" to proxy; QEMU's own
 * -vnc server already injects clicks into the real USB tablet, which the
 * guest kernel sees as normal evdev events on /dev/input/event2. What's
 * missing is only the mouse-vs-touch translation this file provides.
 *
 * Grabs the source device (EVIOCGRAB) so Qt's own evdevmouse handler for
 * it goes quiet — only genuine touch events should reach Engine, since a
 * DJ touchscreen UI's QML may only wire up TapHandler/MouseArea for touch
 * semantics rather than a real mouse pointer.
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
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <linux/input.h>

static int uifd = -1;
static int screen_w = 1280, screen_h = 800;
static int touch_active = 0;

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
    const char *src_path = argc > 1 ? argv[1] : "/dev/input/event2";
    if (argc > 3) {
        screen_w = atoi(argv[2]);
        screen_h = atoi(argv[3]);
    }

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
