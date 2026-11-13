/* A virtual control surface for an emulated HeadRush VX5: its encoders,
 * switches, phantom-power key and volume knob, and the MIDI port it shows a
 * computer over USB.
 *
 * VX5's panel is not MIDI, unlike MPC's or Engine's. HV03 1.2.2 is JUCE on
 * inMusic's sasc framework, and reads its controls as Linux input devices at
 * fixed udev paths:
 *
 *   /dev/input/by-path/platform-fe620000.spi-cs-0-event   inmusic,cs-spi panel
 *   /dev/input/by-path/platform-gpio-keys-event           phantom-power key
 *   /dev/input/by-path/platform-adc-joystick-event        volume knob
 *
 * It retries a missing one about four times a second, so they can appear after
 * it starts. This creates one uinput device for each, with the event codes the
 * HV03 devicetree gives the real ones, and the udev rule build_mpc_rootfs.sh
 * installs links each to its path by name. QEMU's virt machine already has a
 * gpio-keys device for its power button, which takes the gpio-keys path first;
 * the rule gives ours a higher link priority.
 *
 *   panel (cs-spi)     REL_X, REL_Y, REL_Z     encoders 1 to 3, one event per detent
 *                      KEY_SELECT              encoder 1's push
 *                      BTN_0, BTN_1            footswitches 1 and 2
 *                      BTN_2 to BTN_6          preset, key, auto-tune, harmony, fx
 *   gpio-keys          BTN_7                   phantom power
 *   adc-joystick       ABS_VOLUME, 12 to 1020  volume knob
 *
 * HV03 reads the knob's position when it opens the device, and plays nothing
 * at all while it is at zero, which is where it is left without one. It
 * starts at 80 percent here.
 * The MIDI port is a sequencer client named f_midi, which is what the USB
 * gadget's MIDI function is called on the real unit. HV03 subscribes to it by
 * that name, both ways, and takes program changes as presets (PC 4 is preset
 * 05). What it sends back is logged.
 *
 * Commands are read from stdin, one per line; the unit feeds it from
 * /run/vx5surface.fifo:
 *
 *   press <name> [ms]        press and release a switch (default 80 ms)
 *   hold <name>              press a switch and keep it down
 *   release <name>           let it go
 *   turn <1|2|3> <steps>     turn an encoder, negative for anticlockwise
 *   volume <0-100>           turn the volume knob, as a percentage
 *   pc <program> [ch]        MIDI program change, channel 1 to 16 (default 1)
 *   cc <cc> <value> [ch]     MIDI control change
 *   note <note> <vel> [ch]   MIDI note on, or off with velocity 0
 *   list                     log the switch names
 *
 * Switch names: ft1, ft2, preset, key, autotune, harmony, fx, select, phantom.
 */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The names the udev rule matches. */
#define PANEL_NAME "QEngine VX5 panel"
#define PHANTOM_NAME "QEngine VX5 phantom power"
#define VOLUME_NAME "QEngine VX5 volume"

enum { PANEL, PHANTOM, VOLUME, NDEV };
static int fds[NDEV] = {-1, -1, -1};

static const struct {
    const char *name;
    int dev, code;
} switches[] = {
    {"ft1", PANEL, BTN_0},     {"ft2", PANEL, BTN_1},      {"preset", PANEL, BTN_2},
    {"key", PANEL, BTN_3},     {"autotune", PANEL, BTN_4}, {"harmony", PANEL, BTN_5},
    {"fx", PANEL, BTN_6},      {"select", PANEL, KEY_SELECT},
    {"phantom", PHANTOM, BTN_7},
};
#define NSWITCH (int)(sizeof(switches) / sizeof(switches[0]))

/* From the HV03 devicetree's adc-joystick axis: abs-range 12 to 1020. */
#define VOLUME_MIN 12
#define VOLUME_MAX 1020
#define VOLUME_START (VOLUME_MIN + (VOLUME_MAX - VOLUME_MIN) * 80 / 100)

static snd_seq_t *seq;
static int seq_port;

static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static void emit(int dev, int type, int code, int value) {
    struct input_event ev[2];
    memset(ev, 0, sizeof(ev));
    ev[0].type = (unsigned short)type;
    ev[0].code = (unsigned short)code;
    ev[0].value = value;
    ev[1].type = EV_SYN;
    ev[1].code = SYN_REPORT;
    if (write(fds[dev], ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        fprintf(stderr, "uinput write: %s\n", strerror(errno));
}

static int make_device(const char *name, void (*caps)(int fd)) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "/dev/uinput: %s\n", strerror(errno));
        return -1;
    }
    caps(fd);
    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_HOST;
    snprintf(us.name, sizeof(us.name), "%s", name);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "%s: %s\n", name, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void panel_caps(int fd) {
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_Z);
    for (int i = 0; i < NSWITCH; i++)
        if (switches[i].dev == PANEL) ioctl(fd, UI_SET_KEYBIT, switches[i].code);
}

static void phantom_caps(int fd) {
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_7);
}

static void volume_caps(int fd) {
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_ABSBIT, ABS_VOLUME);
    struct uinput_abs_setup abs;
    memset(&abs, 0, sizeof(abs));
    abs.code = ABS_VOLUME;
    abs.absinfo.minimum = VOLUME_MIN;
    abs.absinfo.maximum = VOLUME_MAX;
    abs.absinfo.fuzz = 5;
    abs.absinfo.value = VOLUME_START;
    ioctl(fd, UI_ABS_SETUP, &abs);
}

static int find_switch(const char *name) {
    for (int i = 0; i < NSWITCH; i++)
        if (strcmp(switches[i].name, name) == 0) return i;
    fprintf(stderr, "unknown switch \"%s\" (try list)\n", name);
    return -1;
}

static void midi_send(snd_seq_event_t *ev) {
    snd_seq_ev_set_source(ev, seq_port);
    snd_seq_ev_set_subs(ev);
    snd_seq_ev_set_direct(ev);
    snd_seq_event_output_direct(seq, ev);
}

/* MIDI channels are 1 to 16 here, as a player would count them. */
static int channel_arg(const char *s) {
    int ch = s ? atoi(s) : 1;
    return (ch >= 1 && ch <= 16) ? ch - 1 : 0;
}

static void command(char *line) {
    char *argv[4] = {0};
    int argc = 0;
    for (char *tok = strtok(line, " \t\r\n"); tok && argc < 4; tok = strtok(NULL, " \t\r\n"))
        argv[argc++] = tok;
    if (argc == 0) return;
    const char *cmd = argv[0];
    int s;

    if (strcmp(cmd, "press") == 0 && argc >= 2 && (s = find_switch(argv[1])) >= 0) {
        emit(switches[s].dev, EV_KEY, switches[s].code, 1);
        sleep_ms(argc >= 3 ? atoi(argv[2]) : 80);
        emit(switches[s].dev, EV_KEY, switches[s].code, 0);
    } else if (strcmp(cmd, "hold") == 0 && argc >= 2 && (s = find_switch(argv[1])) >= 0) {
        emit(switches[s].dev, EV_KEY, switches[s].code, 1);
    } else if (strcmp(cmd, "release") == 0 && argc >= 2 &&
               (s = find_switch(argv[1])) >= 0) {
        emit(switches[s].dev, EV_KEY, switches[s].code, 0);
    } else if (strcmp(cmd, "turn") == 0 && argc >= 3) {
        int enc = atoi(argv[1]), steps = atoi(argv[2]);
        if (enc < 1 || enc > 3) {
            fprintf(stderr, "turn wants encoder 1, 2 or 3\n");
            return;
        }
        /* One event per detent, spaced as a hand would turn it, so the app
         * sees each step rather than one large jump. */
        for (int i = 0; i < abs(steps); i++) {
            emit(PANEL, EV_REL, REL_X + enc - 1, steps < 0 ? -1 : 1);
            sleep_ms(20);
        }
    } else if (strcmp(cmd, "volume") == 0 && argc >= 2) {
        int pct = atoi(argv[1]);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        emit(VOLUME, EV_ABS, ABS_VOLUME, VOLUME_MIN + (VOLUME_MAX - VOLUME_MIN) * pct / 100);
    } else if (strcmp(cmd, "pc") == 0 && argc >= 2) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_pgmchange(&ev, channel_arg(argv[2]), atoi(argv[1]) & 0x7f);
        midi_send(&ev);
    } else if (strcmp(cmd, "cc") == 0 && argc >= 3) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_controller(&ev, channel_arg(argv[3]), atoi(argv[1]) & 0x7f,
                                  atoi(argv[2]) & 0x7f);
        midi_send(&ev);
    } else if (strcmp(cmd, "note") == 0 && argc >= 3) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        int vel = atoi(argv[2]) & 0x7f;
        if (vel)
            snd_seq_ev_set_noteon(&ev, channel_arg(argv[3]), atoi(argv[1]) & 0x7f, vel);
        else
            snd_seq_ev_set_noteoff(&ev, channel_arg(argv[3]), atoi(argv[1]) & 0x7f, 0);
        midi_send(&ev);
    } else if (strcmp(cmd, "list") == 0) {
        for (int i = 0; i < NSWITCH; i++) fprintf(stderr, "%s ", switches[i].name);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "unknown or incomplete command \"%s\"\n", cmd);
    }
}

/* Logs what HV03 sends back on f_midi, as bytes. */
static void drain_midi(void) {
    snd_seq_event_t *ev;
    static snd_midi_event_t *dec;
    if (!dec && snd_midi_event_new(256, &dec) < 0) return;
    while (snd_seq_event_input(seq, &ev) >= 0 && ev) {
        unsigned char buf[256];
        long n = snd_midi_event_decode(dec, buf, sizeof(buf), ev);
        if (n <= 0) continue;
        fprintf(stderr, "midi from VX5:");
        for (long i = 0; i < n; i++) fprintf(stderr, " %02X", buf[i]);
        fprintf(stderr, "\n");
        if (snd_seq_event_input_pending(seq, 0) == 0) break;
    }
}

int main(void) {
    fds[PANEL] = make_device(PANEL_NAME, panel_caps);
    fds[PHANTOM] = make_device(PHANTOM_NAME, phantom_caps);
    fds[VOLUME] = make_device(VOLUME_NAME, volume_caps);
    if (fds[PANEL] < 0 || fds[PHANTOM] < 0 || fds[VOLUME] < 0) return 1;

    int err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
    if (err < 0) {
        fprintf(stderr, "snd_seq_open: %s\n", snd_strerror(err));
        return 1;
    }
    snd_seq_set_client_name(seq, "f_midi");
    seq_port = snd_seq_create_simple_port(
        seq, "f_midi",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_WRITE |
            SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE);
    if (seq_port < 0) {
        fprintf(stderr, "create_simple_port: %s\n", snd_strerror(seq_port));
        return 1;
    }
    fprintf(stderr, "VX5 panel, phantom key, volume knob and f_midi up\n");

    int npfd = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd *pfds = calloc((size_t)npfd + 1, sizeof(*pfds));
    if (!pfds) return 1;
    char line[256];
    size_t have = 0;
    for (;;) {
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        snd_seq_poll_descriptors(seq, pfds + 1, (unsigned int)npfd, POLLIN);
        if (poll(pfds, (nfds_t)npfd + 1, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 1; i <= npfd; i++)
            if (pfds[i].revents & POLLIN) drain_midi();
        if (!(pfds[0].revents & (POLLIN | POLLHUP))) continue;
        ssize_t got = read(STDIN_FILENO, line + have, sizeof(line) - 1 - have);
        if (got <= 0) break;
        have += (size_t)got;
        line[have] = '\0';
        char *nl;
        while ((nl = strchr(line, '\n'))) {
            *nl = '\0';
            command(line);
            have -= (size_t)(nl + 1 - line);
            memmove(line, nl + 1, have + 1);
        }
        if (have == sizeof(line) - 1) have = 0; /* an overlong line is dropped */
    }
    return 0;
}
