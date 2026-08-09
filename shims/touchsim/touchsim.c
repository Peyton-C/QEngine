#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/uinput.h>

#define SCREEN_W 1024
#define SCREEN_H 768

static int fd;

static void emit(int type, int code, int val) {
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    ie.type = type;
    ie.code = code;
    ie.value = val;
    if (write(fd, &ie, sizeof(ie)) < 0) {
        fprintf(stderr, "write failed: %s\n", strerror(errno));
    }
}

static void sync_report(void) {
    emit(EV_SYN, SYN_REPORT, 0);
}

static void touch_down(int slot, int tracking_id, int x, int y) {
    emit(EV_ABS, ABS_MT_SLOT, slot);
    emit(EV_ABS, ABS_MT_TRACKING_ID, tracking_id);
    emit(EV_KEY, BTN_TOUCH, 1);
    emit(EV_ABS, ABS_MT_POSITION_X, x);
    emit(EV_ABS, ABS_MT_POSITION_Y, y);
    emit(EV_ABS, ABS_X, x);
    emit(EV_ABS, ABS_Y, y);
    sync_report();
}

static void touch_move(int slot, int x, int y) {
    emit(EV_ABS, ABS_MT_SLOT, slot);
    emit(EV_ABS, ABS_MT_POSITION_X, x);
    emit(EV_ABS, ABS_MT_POSITION_Y, y);
    emit(EV_ABS, ABS_X, x);
    emit(EV_ABS, ABS_Y, y);
    sync_report();
}

static void touch_up(int slot) {
    emit(EV_ABS, ABS_MT_SLOT, slot);
    emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(EV_KEY, BTN_TOUCH, 0);
    sync_report();
}

static void setup_device(void) {
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "open /dev/uinput failed: %s\n", strerror(errno));
        exit(1);
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);

    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);

    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5678;
    strcpy(usetup.name, "TouchSim Virtual Touchscreen");
    ioctl(fd, UI_DEV_SETUP, &usetup);

    struct uinput_abs_setup abs_x = { .code = ABS_X, .absinfo = { .minimum = 0, .maximum = SCREEN_W, .resolution = 0 } };
    struct uinput_abs_setup abs_y = { .code = ABS_Y, .absinfo = { .minimum = 0, .maximum = SCREEN_H, .resolution = 0 } };
    struct uinput_abs_setup abs_mtx = { .code = ABS_MT_POSITION_X, .absinfo = { .minimum = 0, .maximum = SCREEN_W, .resolution = 0 } };
    struct uinput_abs_setup abs_mty = { .code = ABS_MT_POSITION_Y, .absinfo = { .minimum = 0, .maximum = SCREEN_H, .resolution = 0 } };
    struct uinput_abs_setup abs_slot = { .code = ABS_MT_SLOT, .absinfo = { .minimum = 0, .maximum = 9, .resolution = 0 } };
    struct uinput_abs_setup abs_id = { .code = ABS_MT_TRACKING_ID, .absinfo = { .minimum = 0, .maximum = 65535, .resolution = 0 } };

    ioctl(fd, UI_ABS_SETUP, &abs_x);
    ioctl(fd, UI_ABS_SETUP, &abs_y);
    ioctl(fd, UI_ABS_SETUP, &abs_mtx);
    ioctl(fd, UI_ABS_SETUP, &abs_mty);
    ioctl(fd, UI_ABS_SETUP, &abs_slot);
    ioctl(fd, UI_ABS_SETUP, &abs_id);

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "UI_DEV_CREATE failed: %s\n", strerror(errno));
        exit(1);
    }
    usleep(300000);
    fprintf(stderr, "Virtual touchscreen created.\n");
}

int main(int argc, char **argv) {
    setup_device();

    fprintf(stderr, "Ready. Commands: 'tap X Y HOLD_MS', 'drag X1 Y1 X2 Y2 DURATION_MS [STEPS]', or 'quit'\n");
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        int x, y, hold_ms;
        int x1, y1, x2, y2, duration_ms, steps;
        if (strncmp(line, "quit", 4) == 0) break;
        if (sscanf(line, "tap %d %d %d", &x, &y, &hold_ms) == 3) {
            fprintf(stderr, "tap at (%d,%d) hold %dms\n", x, y, hold_ms);
            touch_down(0, 1, x, y);
            usleep((useconds_t)hold_ms * 1000);
            touch_up(0);
            fprintf(stderr, "done\n");
        } else if ((steps = 0, sscanf(line, "drag %d %d %d %d %d %d", &x1, &y1, &x2, &y2, &duration_ms, &steps)) >= 5) {
            if (steps <= 0) steps = 20;
            fprintf(stderr, "drag (%d,%d) -> (%d,%d) over %dms, %d steps\n", x1, y1, x2, y2, duration_ms, steps);
            touch_down(0, 1, x1, y1);
            useconds_t step_delay = (useconds_t)duration_ms * 1000 / steps;
            for (int i = 1; i <= steps; i++) {
                int ix = x1 + (x2 - x1) * i / steps;
                int iy = y1 + (y2 - y1) * i / steps;
                usleep(step_delay);
                touch_move(0, ix, iy);
            }
            usleep(100000);
            touch_up(0);
            fprintf(stderr, "done\n");
        } else {
            fprintf(stderr, "bad command: %s", line);
        }
    }

    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}