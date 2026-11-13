/* A virtual control surface for an emulated Akai MPC, Force or MPC Sample, preloaded into the app.
 *
 * MPC's pads, buttons and encoders are a separate microcontroller that the
 * application reaches as a USB MIDI device. With none there MPC still starts,
 * but it can only be driven by touch. This stands in for that device so the
 * emulated unit can be driven the way its panel drives it.
 *
 * Engine's surface is a sequencer client, midisurface, because Engine binds any
 * client that answers its identity inquiry. MPC does not look at the sequencer
 * for its own panel. It walks the sound cards (3.10.0 polls about twenty times a
 * second, so the panel can appear late), reads each card's components string,
 * which for a USB card is "USB<vid>:<pid>", and takes the one whose ids are in
 * its hardware table. On that card it wants a rawmidi device with more than one
 * subdevice, and opens hw:<card>,<dev>,1, the private port, and then
 * hw:<card>,<dev>,0, the public one. So the surface has to be a card, and this
 * is a preload rather than a program.
 *
 * A real card was tried first: a MIDI gadget on dummy_hcd, which gives MPC a
 * genuine USB MIDI device inside the guest. MPC found it, and then failed to
 * open it with EBUSY, because its own MIDI device list had already taken both
 * ports through the sequencer like any keyboard plugged into the unit. It also
 * needs kernel modules a chroot's host may not have. A card that exists only
 * inside MPC has neither problem.
 *
 * What is interposed is deliberately not alsa-lib. MPC calls some thirty
 * snd_ctl_* and snd_rawmidi_* functions, and a made-up handle has to survive
 * every one of them, including the ones the audio code calls on every card it
 * finds. Instead this answers at the kernel's interface, which is small and
 * fixed: open() of /dev/snd/controlC<n> and /dev/snd/midiC<n>D0, and the
 * handful of ioctls alsa-lib sends them. alsa-lib then builds real handles on
 * top, and everything else MPC does with them works unmodified. The rawmidi
 * descriptor is one end of a socketpair, so read, write and poll need nothing
 * from us either. A thread started on first use holds the other end and is the
 * panel.
 *
 * Descriptors are recognised at ioctl time by number and inode rather than by
 * tracking close(), which would mean interposing it for the whole process.
 *
 * Which panel to be comes from the guest's product code, the same file dtshim
 * serves. The USB product id and the SysEx product byte are the same number,
 * from the hardware table in /usr/bin/MPC (see PRODUCTS).
 *
 * Commands are read from a fifo, one per line, /run/mpcsurface.fifo unless
 * MPCSURFACE_FIFO names another. The fifo is the preload's own, so nothing has
 * to be started before MPC:
 *
 *   button <name> [ms]       press and release a named button (default 80 ms)
 *   hold <name>              press a named button and keep it down, as for shift
 *   release <name>           let it go
 *   pad <n> [velocity] [ms]  strike pad n, 1 to 16 with pad 1 at the bottom left,
 *                            or on a Force 1 to 64 with pad 1 at the top left
 *   turn <name> <steps>      turn dial or q1 to q4, q8 on a Force, negative for
 *                            anticlockwise
 *   touch <name> [ms]        touch a Q-Link without turning it
 *   set <pot> <0-127>        MPC Sample only: k1 to k3, volume or fader
 *   list                     log the button names this product has
 *   press <ch> <note> [ms]   raw note on, then off
 *   on    <ch> <note> [vel]  raw note on
 *   off   <ch> <note>        raw note off
 *   cc    <ch> <cc> <val>    raw control change
 *   sysex <hex bytes...>     raw SysEx
 *
 * Button names are known only for the products in BUTTONS, where each was
 * pressed and seen to do what its name says, except most of the Force's, which
 * come from IamForce (see FORCE_BUTTONS). The raw commands reach anything
 * else, and can also send what no real panel would: on ACVA3, 3.10.0, a note on
 * for 0x58, one past its fourth Q-Link's touch note, stops MPC listening to the
 * panel until it is restarted.
 *
 * MPCSURFACE_VOLUME sets where MPC Sample's MAIN VOLUME starts, 100 unless
 * given, the level the Sample's panel reports when the app asks.
 *
 * MPCSURFACE_VERBOSE=1 logs what MPC sends and what it is sent, leaving out pad
 * colours and pings, and 2 logs those too. That log is how the lamp or note for
 * a control this does not name yet is found. MPCSURFACE_LOG names a file to log
 * to in place of stderr.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>

/* To MPC's stderr, which is the journal under systemd, or to the file
 * MPCSURFACE_LOG names. MPC's stderr reaches the journal late and in bursts,
 * and the journal drops lines once a lit pad starts animating, so the file is
 * the one to read when the order or timing of messages matters. */
static FILE *log_file;
static void log_line(const char *fmt, ...) {
    FILE *f = log_file ? log_file : stderr;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    va_list ap;
    va_start(ap, fmt);
    flockfile(f);
    if (log_file) fprintf(f, "%ld.%03ld ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    else fputs("[mpcsurface] ", f);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
    funlockfile(f);
    va_end(ap);
}
#define LOG(...) log_line(__VA_ARGS__)

/* ---- which panel ---------------------------------------------------------- */

/* From the hardware table in /usr/bin/MPC 3.10.0 (Gen 2), which carries every
 * product of both generations: 0x70-byte rows of code, product byte, names and
 * panel geometry. The product byte is the low byte of the USB product id, with
 * Akai's vendor id 09e8, and the product byte of the panel's SysEx. Gen 2
 * restarted the numbering from zero.
 *
 * ACV5S, the MPC X SE, shares the X's panel and its id.
 *
 * MPC Sample is not in that table and its panel is not USB. AC50's panel MCU
 * sits on a UART, fe670000, under snd-serial-generic, which inMusic's kernel
 * names after the devicetree label: a card "Control Surface" with the driver
 * "SerialMIDI" and one rawmidi port. "MPC Sample" 1.3.0 walks the cards for
 * that name and driver and opens hw:<card>,<dev>. The protocol is MIDI with no
 * handshake: the app sends F0 47 7F 5C 42 00 00 F7 once it starts, and the
 * panel answers with its pot positions. 0x5C is the product byte in AC50's
 * identity reply. */
struct product {
    const char *code;
    unsigned char id;
    const char *name;
    int pads;                  /* 16 on an MPC, 64 on a Force */
    int uart;                  /* 1: MPC Sample's serial panel, 0: a USB one */
};

static const struct product PRODUCTS[] = {
    {"ACV5",  0x3A, "MPC X",        16, 0},
    {"ACV5S", 0x3A, "MPC X SE",     16, 0},
    {"ACV8",  0x3B, "MPC Live",     16, 0},
    {"ACVA",  0x46, "MPC One",      16, 0},
    {"ACVA2", 0x56, "MPC One+",     16, 0},
    {"ACVA3", 0x00, "MPC One",      16, 0},
    {"ACVB",  0x47, "MPC Live II",  16, 0},
    {"ACVC",  0x5A, "MPC XL",       16, 0},
    {"ACVG",  0x58, "MPC Live III", 16, 0},
    {"ACVM",  0x4B, "MPC Key 61",   16, 0},
    {"ACVM2", 0x02, "MPC Key 61",   16, 0},
    {"ACVR",  0x57, "MPC Key 37",   16, 0},
    {"ACVR2", 0x01, "MPC Key 37",   16, 0},
    {"ADA2",  0x40, "Force",        64, 0},
    {"ADA2S", 0x04, "Force",        64, 0},
    {"AC50",  0x5C, "MPC Sample",   16, 1},
};
#define AKAI_VENDOR 0x09E8

/* Where the guest records which device it is pretending to be. dtshim serves
 * this file to MPC in place of the devicetree node; it is a plain file, so it
 * is read directly, as midisurface does. */
#define PRODUCT_CODE_PATH "/root/fake-dt/inmusic,product-code"

static const struct product *product;   /* NULL: unknown code, shim inert */

static void pick_product(void) {
    char code[32] = "";
    const char *env = getenv("MPCSURFACE_PRODUCT_CODE");
    if (env && *env) {
        snprintf(code, sizeof code, "%s", env);
    } else {
        FILE *f = fopen(PRODUCT_CODE_PATH, "r");
        if (f) {
            if (!fgets(code, sizeof code, f)) code[0] = '\0';
            fclose(f);
        }
    }
    code[strcspn(code, "\r\n")] = '\0';
    for (size_t i = 0; i < sizeof PRODUCTS / sizeof PRODUCTS[0]; i++)
        if (strcmp(PRODUCTS[i].code, code) == 0) product = &PRODUCTS[i];
    if (product && product->uart)
        LOG("panel for %s (%s), serial", product->code, product->name);
    else if (product)
        LOG("panel for %s (%s), USB %04x:%04x", product->code, product->name,
            AKAI_VENDOR, product->id);
    else
        /* No guessing: a panel with another product's id makes MPC run as that
         * product, with its pad count and its screens. */
        LOG("no panel known for product code '%s'; MPC runs without one",
            *code ? code : "<unset>");
}

/* ---- the card -------------------------------------------------------------- */

/* A card number nothing real will have. alsa-lib finds cards by trying to open
 * controlC0 to controlC31, so answering for this one is all it takes to be
 * enumerated. */
static int fake_card = 27;
static char ctl_path[40], midi_path[40];

static int verbose;
static pthread_once_t once = PTHREAD_ONCE_INIT;

/* Our descriptors, by number. A slot is live only while the descriptor still
 * has the inode it was made with, so one MPC has closed, and whose number has
 * gone to something else, is not answered for. */
enum { FD_NONE, FD_CTL, FD_MIDI };
#define MAX_FDS 4096
static struct { ino_t ino; char kind; char subdevice; size_t buffer; } fds[MAX_FDS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void remember(int fd, int kind, int subdevice) {
    struct stat st;
    if (fd < 0 || fd >= MAX_FDS || fstat(fd, &st) < 0) return;
    pthread_mutex_lock(&lock);
    fds[fd].ino = st.st_ino;
    fds[fd].kind = (char)kind;
    fds[fd].subdevice = (char)subdevice;
    fds[fd].buffer = 4096;     /* the kernel's default */
    pthread_mutex_unlock(&lock);
}

static int kind_of(int fd, int *subdevice) {
    struct stat st;
    if (fd < 0 || fd >= MAX_FDS || fds[fd].kind == FD_NONE) return FD_NONE;
    if (fstat(fd, &st) < 0 || st.st_ino != fds[fd].ino) return FD_NONE;
    if (subdevice) *subdevice = fds[fd].subdevice;
    return fds[fd].kind;
}

/* The two ports, and the panel's end of each open of them. MPC opens each port
 * once, but nothing here depends on that. */
#define PORT_PUBLIC 0
#define PORT_PRIVATE 1
/* The port the panel speaks on: the private one on a USB panel, the only one,
 * 0, on MPC Sample's. */
#define PANEL_PORT (product->uart ? PORT_PUBLIC : PORT_PRIVATE)
#define MAX_CONNS 8
static struct conn {
    int fd;                    /* -1: free */
    int port;
    int input;                 /* MPC reads from it */
    unsigned char buf[4096];   /* a message MPC has not finished sending */
    size_t len;
    unsigned char running;     /* its running status */
    int app_fd;                /* MPC's end, which its PARAMS ioctl names */
    unsigned int mode;         /* the framing and clock MPC asked for there */
} conns[MAX_CONNS];
static int wake_pipe[2] = {-1, -1};

/* The kernel remembers the subdevice a task asked for and gives it to that
 * task's next open. alsa-lib asks and opens on the same thread. */
static __thread int preferred_subdevice = -1;

static void *surface_thread(void *);

static void start(void) {
    const char *env = getenv("MPCSURFACE_CARD");
    if (env && *env) fake_card = atoi(env);
    snprintf(ctl_path, sizeof ctl_path, "/dev/snd/controlC%d", fake_card);
    snprintf(midi_path, sizeof midi_path, "/dev/snd/midiC%dD0", fake_card);
    env = getenv("MPCSURFACE_VERBOSE");
    verbose = env && *env ? atoi(env) : 0;
    env = getenv("MPCSURFACE_LOG");
    if (env && *env) log_file = fopen(env, "ae");
    for (int i = 0; i < MAX_CONNS; i++) conns[i].fd = -1;
    pick_product();
    if (!product) return;
    pthread_t t;
    if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK) < 0 ||
        pthread_create(&t, NULL, surface_thread, NULL) != 0) {
        LOG("could not start the panel: %s", strerror(errno));
        product = NULL;
        return;
    }
    pthread_detach(t);
}

static int open_ctl(int flags) {
    /* Any descriptor with an inode of its own will do; nothing is read from or
     * written to a control device, it only takes ioctls. */
    int fd = socket(AF_UNIX, SOCK_DGRAM | (flags & O_CLOEXEC ? SOCK_CLOEXEC : 0), 0);
    if (fd >= 0) remember(fd, FD_CTL, 0);
    return fd;
}

static int open_midi(int flags) {
    int sub = preferred_subdevice;
    preferred_subdevice = -1;
    if (sub != PORT_PUBLIC && sub != PORT_PRIVATE) sub = PORT_PUBLIC;
    if (product->uart) sub = PORT_PUBLIC;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) return -1;
    pthread_mutex_lock(&lock);
    int slot = -1;
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].fd < 0) { slot = i; break; }
    if (slot >= 0) {
        conns[slot].fd = sv[1];
        conns[slot].port = sub;
        conns[slot].input = (flags & O_ACCMODE) != O_WRONLY;
        conns[slot].len = 0;
        conns[slot].running = 0;
        conns[slot].app_fd = sv[0];
        conns[slot].mode = 0;
    }
    pthread_mutex_unlock(&lock);
    if (slot < 0) {
        close(sv[0]); close(sv[1]);
        errno = EBUSY;
        return -1;
    }
    if (!(flags & O_CLOEXEC)) fcntl(sv[0], F_SETFD, 0);
    if (flags & O_NONBLOCK) fcntl(sv[0], F_SETFL, O_NONBLOCK);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    remember(sv[0], FD_MIDI, sub);
    LOG("MPC opened the %s port for %s",
        product->uart ? "panel" : sub == PORT_PRIVATE ? "private" : "public",
        (flags & O_ACCMODE) == O_RDONLY ? "input"
        : (flags & O_ACCMODE) == O_WRONLY ? "output" : "both");
    if (write(wake_pipe[1], "o", 1) < 0) { /* the thread polls again soon anyway */ }
    return sv[0];
}

/* -2 when the path is not ours. */
static int open_ours(const char *path, int flags) {
    if (!path || strncmp(path, "/dev/snd/", 9) != 0) { errno = ENOENT; return -2; }
    pthread_once(&once, start);
    if (!product) { errno = ENOENT; return -2; }
    if (strcmp(path, ctl_path) == 0) return open_ctl(flags);
    if (strcmp(path, midi_path) == 0) return open_midi(flags);
    errno = ENOENT;
    return -2;
}

int open(const char *path, int flags, ...) {
    static int (*real)(const char *, int, ...);
    if (!real) real = dlsym(RTLD_NEXT, "open");
    va_list ap; va_start(ap, flags);
    mode_t mode = (flags & (O_CREAT | O_TMPFILE)) ? va_arg(ap, mode_t) : 0;
    va_end(ap);
    int fd = open_ours(path, flags);
    return fd != -2 ? fd : real(path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    static int (*real)(const char *, int, ...);
    if (!real) real = dlsym(RTLD_NEXT, "open64");
    va_list ap; va_start(ap, flags);
    mode_t mode = (flags & (O_CREAT | O_TMPFILE)) ? va_arg(ap, mode_t) : 0;
    va_end(ap);
    int fd = open_ours(path, flags);
    return fd != -2 ? fd : real(path, flags, mode);
}

/* What a fortified build calls when the flags are not a constant, which is how
 * alsa-lib 1.2.11 in MPC 3.10.0 opens every device but its card probe. */
int __open_2(const char *path, int flags) {
    static int (*real)(const char *, int);
    if (!real) real = dlsym(RTLD_NEXT, "__open_2");
    int fd = open_ours(path, flags);
    return fd != -2 ? fd : real(path, flags);
}

int __open64_2(const char *path, int flags) {
    static int (*real)(const char *, int);
    if (!real) real = dlsym(RTLD_NEXT, "__open64_2");
    int fd = open_ours(path, flags);
    return fd != -2 ? fd : real(path, flags);
}

static void fill_midi_info(struct snd_rawmidi_info *info, int subdevice) {
    int stream = info->stream;
    memset(info, 0, sizeof *info);
    info->device = 0;
    info->subdevice = subdevice;
    info->stream = stream;
    info->card = fake_card;
    info->flags = SNDRV_RAWMIDI_INFO_OUTPUT | SNDRV_RAWMIDI_INFO_INPUT |
                  SNDRV_RAWMIDI_INFO_DUPLEX;
    if (product->uart) {
        /* As snd-serial-generic names its one port under inMusic's patch. */
        snprintf((char *)info->id, sizeof info->id, "SerialMIDI");
        snprintf((char *)info->name, sizeof info->name, "Control Surface");
        snprintf((char *)info->subname, sizeof info->subname, "Control Surface");
        info->subdevices_count = 1;
        info->subdevices_avail = 1;
        return;
    }
    snprintf((char *)info->id, sizeof info->id, "USB MIDI");
    snprintf((char *)info->name, sizeof info->name, "%s Controller", product->name);
    snprintf((char *)info->subname, sizeof info->subname, "%s %s", product->name,
             subdevice == PORT_PRIVATE ? "Private" : "Public");
    info->subdevices_count = 2;
    info->subdevices_avail = 2;
}

/* The control device. Anything not answered here is refused the way a card
 * with no mixer, PCM or hwdep would refuse it, which alsa-lib passes up as an
 * error; MPC's audio code looks at every card and moves on from this one. */
static int ctl_ioctl(unsigned long req, void *arg) {
    switch (_IOC_NR(req)) {
    case _IOC_NR(SNDRV_CTL_IOCTL_PVERSION):
        *(int *)arg = SNDRV_CTL_VERSION;
        return 0;
    case _IOC_NR(SNDRV_CTL_IOCTL_CARD_INFO): {
        struct snd_ctl_card_info *ci = arg;
        memset(ci, 0, sizeof *ci);
        ci->card = fake_card;
        if (product->uart) {
            /* What MPC Sample matches the panel by: the name and the driver.
             * The id is what alsa-lib would make of that name. */
            snprintf((char *)ci->id, sizeof ci->id, "ControlSurface");
            snprintf((char *)ci->driver, sizeof ci->driver, "SerialMIDI");
            snprintf((char *)ci->name, sizeof ci->name, "Control Surface");
            snprintf((char *)ci->longname, sizeof ci->longname,
                     "Control Surface at serial2 (QEngine)");
            return 0;
        }
        snprintf((char *)ci->id, sizeof ci->id, "Controller");
        snprintf((char *)ci->driver, sizeof ci->driver, "USB-Audio");
        snprintf((char *)ci->name, sizeof ci->name, "%s Controller", product->name);
        snprintf((char *)ci->longname, sizeof ci->longname,
                 "Akai %s Controller (QEngine)", product->name);
        snprintf((char *)ci->mixername, sizeof ci->mixername, "USB Mixer");
        /* What MPC matches the panel by. */
        snprintf((char *)ci->components, sizeof ci->components, "USB%04x:%04x",
                 AKAI_VENDOR, product->id);
        return 0;
    }
    case _IOC_NR(SNDRV_CTL_IOCTL_RAWMIDI_NEXT_DEVICE):
        *(int *)arg = *(int *)arg < 0 ? 0 : -1;
        return 0;
    case _IOC_NR(SNDRV_CTL_IOCTL_HWDEP_NEXT_DEVICE):
    case _IOC_NR(SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE):
        *(int *)arg = -1;
        return 0;
    case _IOC_NR(SNDRV_CTL_IOCTL_RAWMIDI_INFO): {
        struct snd_rawmidi_info *info = arg;
        if (info->device != 0 || info->subdevice > (product->uart ? 0 : 1)) {
            errno = ENXIO;
            return -1;
        }
        fill_midi_info(info, (int)info->subdevice);
        return 0;
    }
    case _IOC_NR(SNDRV_CTL_IOCTL_RAWMIDI_PREFER_SUBDEVICE):
        preferred_subdevice = *(int *)arg;
        return 0;
    case _IOC_NR(SNDRV_CTL_IOCTL_ELEM_LIST): {
        struct snd_ctl_elem_list *list = arg;
        list->used = 0;
        list->count = 0;
        return 0;
    }
    case _IOC_NR(SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS):
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

static int midi_ioctl(int fd, int subdevice, unsigned long req, void *arg) {
    switch (_IOC_NR(req)) {
    case _IOC_NR(SNDRV_RAWMIDI_IOCTL_PVERSION):
        *(int *)arg = SNDRV_RAWMIDI_VERSION;
        return 0;
    case _IOC_NR(SNDRV_RAWMIDI_IOCTL_USER_PVERSION):
        return 0;
    case _IOC_NR(SNDRV_RAWMIDI_IOCTL_INFO):
        fill_midi_info(arg, subdevice);
        return 0;
    case _IOC_NR(SNDRV_RAWMIDI_IOCTL_PARAMS): {
        /* Buffer sizes and wake-up thresholds. The socket has its own, but the
         * size is kept so that status can report all of it free.
         *
         * The mode matters. MPC Sample 1.3.0 reads its panel with
         * snd_rawmidi_tread, which sets FRAMING_TSTAMP here, and then each read
         * is whole 32-byte records with a timestamp, not MIDI bytes. Raw bytes
         * on that port are read as garbage records and the panel does nothing.
         * Gen 2 MPC asks for no framing. */
        struct snd_rawmidi_params *params = arg;
        fds[fd].buffer = params->buffer_size;
        if (params->stream == SNDRV_RAWMIDI_STREAM_INPUT) {
            pthread_mutex_lock(&lock);
            for (int i = 0; i < MAX_CONNS; i++)
                if (conns[i].fd >= 0 && conns[i].app_fd == fd) conns[i].mode = params->mode;
            pthread_mutex_unlock(&lock);
            if (params->mode & SNDRV_RAWMIDI_MODE_FRAMING_MASK)
                LOG("MPC reads the panel in timestamped records");
        }
        return 0;
    }
    case _IOC_NR(SNDRV_RAWMIDI_IOCTL_STATUS): {
        /* Nothing is ever lost, and there is always room to write. MPC asks
         * for this to decide whether its output is backing up. */
        struct snd_rawmidi_status *st = arg;
        int stream = st->stream, queued = 0;
        memset(st, 0, sizeof *st);
        st->stream = stream;
        if (stream == SNDRV_RAWMIDI_STREAM_INPUT) {
            static int (*real)(int, unsigned long, ...);
            if (!real) real = dlsym(RTLD_NEXT, "ioctl");
            real(fd, FIONREAD, &queued);
            st->avail = (size_t)queued;
        } else {
            st->avail = fds[fd].buffer;
        }
        return 0;
    }
    case _IOC_NR(SNDRV_RAWMIDI_IOCTL_DROP):
    case _IOC_NR(SNDRV_RAWMIDI_IOCTL_DRAIN):
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

static int ioctl_ours(int fd, unsigned long req, void *arg, int *ret) {
    /* 'U' is the control device's ioctl type and 'W' rawmidi's. MPC makes a
     * great many ioctls, nearly all of them DRM's, and this turns them away on
     * one comparison. */
    if (_IOC_TYPE(req) != 'U' && _IOC_TYPE(req) != 'W') return 0;
    int subdevice = 0;
    int kind = kind_of(fd, &subdevice);
    if (kind == FD_CTL && _IOC_TYPE(req) == 'U') { *ret = ctl_ioctl(req, arg); return 1; }
    if (kind == FD_MIDI && _IOC_TYPE(req) == 'W') { *ret = midi_ioctl(fd, subdevice, req, arg); return 1; }
    return 0;
}

int ioctl(int fd, unsigned long req, ...) {
    static int (*real)(int, unsigned long, ...);
    if (!real) real = dlsym(RTLD_NEXT, "ioctl");
    va_list ap; va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    int ret;
    return ioctl_ours(fd, req, arg, &ret) ? ret : real(fd, req, arg);
}

#if __SIZEOF_POINTER__ == 4
/* What a 32-bit glibc with 64-bit time calls instead, as Gen 1's does. */
int __ioctl_time64(int fd, unsigned long req, ...) {
    static int (*real)(int, unsigned long, ...);
    if (!real) real = dlsym(RTLD_NEXT, "__ioctl_time64");
    va_list ap; va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    int ret;
    return ioctl_ours(fd, req, arg, &ret) ? ret : real(fd, req, arg);
}
#endif

/* ---- the panel -------------------------------------------------------------- */

/* 1 for the messages that drown the rest out: pad colours, which MPC sends by
 * the hundred a second while a pad animates, and the ping and its answer. */
static int quiet(const unsigned char *m, size_t n) {
    return n > 4 && m[0] == 0xF0 && m[1] == 0x47 &&
           (m[4] == 0x65 || m[4] == 0x52 || m[4] == 0x53);
}

static void log_bytes(const char *what, int port, const unsigned char *b, size_t n) {
    char line[3 * 48 + 8];
    size_t o = 0;
    for (size_t i = 0; i < n && i < 48; i++)
        o += (size_t)snprintf(line + o, sizeof line - o, " %02X", b[i]);
    LOG("%s %s (%zu):%s%s", what, port == PORT_PRIVATE ? "private" : "public", n,
        line, n > 48 ? " ..." : "");
}

/* The bytes as the kernel frames them for a port in FRAMING_TSTAMP mode: a
 * record of up to sixteen bytes each, stamped from the clock MPC chose. Returns
 * the length written to out. */
static size_t frame(unsigned int mode, const unsigned char *buf, size_t len,
                    unsigned char *out, size_t room) {
    clockid_t clock;
    switch (mode & SNDRV_RAWMIDI_MODE_CLOCK_MASK) {
    case SNDRV_RAWMIDI_MODE_CLOCK_REALTIME: clock = CLOCK_REALTIME; break;
    case SNDRV_RAWMIDI_MODE_CLOCK_MONOTONIC_RAW: clock = CLOCK_MONOTONIC_RAW; break;
    default: clock = CLOCK_MONOTONIC; break;
    }
    struct timespec ts = {0, 0};
    if ((mode & SNDRV_RAWMIDI_MODE_CLOCK_MASK) != SNDRV_RAWMIDI_MODE_CLOCK_NONE)
        clock_gettime(clock, &ts);
    size_t o = 0;
    for (size_t i = 0; i < len && o + sizeof(struct snd_rawmidi_framing_tstamp) <= room;) {
        struct snd_rawmidi_framing_tstamp r;
        memset(&r, 0, sizeof r);
        size_t n = len - i < SNDRV_RAWMIDI_FRAMING_DATA_LENGTH
                   ? len - i : SNDRV_RAWMIDI_FRAMING_DATA_LENGTH;
        r.length = (unsigned char)n;
        r.tv_sec = (__u64)ts.tv_sec;
        r.tv_nsec = (__u32)ts.tv_nsec;
        memcpy(r.data, buf + i, n);
        memcpy(out + o, &r, sizeof r);
        o += sizeof r;
        i += n;
    }
    return o;
}

static void send_bytes(int port, const unsigned char *buf, size_t len) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CONNS; i++) {
        /* Not to a descriptor MPC only writes to: nothing would drain it, and
         * once its buffer filled every send after would fail. */
        if (conns[i].fd < 0 || conns[i].port != port || !conns[i].input) continue;
        const unsigned char *out = buf;
        size_t out_len = len;
        unsigned char framed[1024];
        if (conns[i].mode & SNDRV_RAWMIDI_MODE_FRAMING_MASK) {
            out_len = frame(conns[i].mode, buf, len, framed, sizeof framed);
            out = framed;
        }
        if (send(conns[i].fd, out, out_len, MSG_NOSIGNAL) < 0)
            LOG("send to MPC failed: %s", strerror(errno));
        else if (verbose && quiet(buf, len) < verbose)
            log_bytes("MPC <-", port, buf, len);
    }
    pthread_mutex_unlock(&lock);
}

static void send3(int port, int status, int d1, int d2) {
    unsigned char m[3] = {(unsigned char)status, (unsigned char)(d1 & 0x7F),
                          (unsigned char)(d2 & 0x7F)};
    send_bytes(port, m, 3);
}

/* The panel's serial number, fifteen letters and digits. MPC reads it out of
 * the identity reply, and where it is blank it makes one up, programs it into
 * the panel and asks again. It asks six times over before it reads the first
 * answer, so a blank one is programmed six times. This one is in the shape of
 * those MPC 3.10.0 makes up, and it leaves it alone. */
static char serial[16] = "99QENG2702QEMU1";

/* Whether MPC has put the panel into its own mode yet. A real panel powers up
 * as a class-compliant controller and MPC switches it over. */
static int mpc_mode;

static void akai(int command, const unsigned char *data, size_t len) {
    unsigned char m[64] = {0xF0, 0x47, 0x00, product->id, (unsigned char)command,
                           (unsigned char)(len >> 7), (unsigned char)(len & 0x7F)};
    if (len > sizeof m - 8) return;
    memcpy(m + 7, data, len);
    m[7 + len] = 0xF7;
    send_bytes(PANEL_PORT, m, len + 8);
}

/* MPC Sample's pots, K1 to K3, MAIN VOLUME and the fader, as CC 0x10 to 0x14.
 * They are absolute, and the app knows where one is only from what the panel
 * last sent. MAIN VOLUME is the Sample's software volume, so a panel that never
 * reports it leaves the app silent. */
#define CC_POT 0x10
enum { POT_K1, POT_K2, POT_K3, POT_VOLUME, POT_FADER, POTS };
static unsigned char pots[POTS] = {64, 64, 64, 100, 64};

/* When to report MAIN VOLUME again, in CLOCK_MONOTONIC ms, 0 for never. */
static long long report_due;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Only MAIN VOLUME: the app takes K1 to K3 or the fader arriving as a move on
 * whatever page is open, so reporting them at start would edit the project. */
static void report_pots(void) {
    send3(PANEL_PORT, 0xB0, CC_POT + POT_VOLUME, pots[POT_VOLUME]);
    if (verbose) LOG("reported MAIN VOLUME %d", pots[POT_VOLUME]);
}

/* One complete message from MPC.
 *
 * MPC 3.10.0 opens the private port for input and output and the public port
 * for output only. It sends its SysEx on the public one and the button lamps,
 * as control changes on the button's own number, on the private one, and
 * listens only on the private one. So everything this sends goes there.
 *
 * Akai's own messages are F0 47 <device> <product> <command> <length, two
 * bytes of seven bits> <data> F7. */
static void from_mpc(int port, const unsigned char *m, size_t n) {
    if (verbose && quiet(m, n) < verbose) log_bytes("MPC ->", port, m, n);
    if (n < 6 || m[0] != 0xF0) return;

    /* The universal identity request, F0 7E <device> 06 01 F7. */
    if (m[1] == 0x7E && m[3] == 0x06 && m[4] == 0x01 && product->uart) {
        /* AC50's MCU: product 5C, then firmware 1.0.0.051 as four bytes, the
         * version 1.3.0's UpdateImage.rbin carries, so az0x-firmware-tool
         * finds nothing to update. */
        static const unsigned char reply[] = {
            0xF0, 0x7E, 0x00, 0x06, 0x02, 0x47, 0x5C, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x33, 0xF7};
        send_bytes(PANEL_PORT, reply, sizeof reply);
        if (verbose) LOG("answered the identity request");
        return;
    }
    if (m[1] == 0x7E && m[3] == 0x06 && m[4] == 0x01) {
        /* Manufacturer 47 is Akai, then the product byte, which is how MPC
         * tells which panel this is. MPC reads the firmware version from bytes
         * 9 to 12, as two two-digit numbers on most products and as four
         * numbers on the Live III and XL, whose product bytes are 58 and 5A.
         * Nines, so no shipped firmware is newer: MPC halts a Live III whose
         * panel is older than 1.3.0.1 and asks for an update. The serial is the
         * fifteen characters from byte 18, with 7F for one never set. */
        unsigned char reply[35] = {
            0xF0, 0x7E, 0x00, 0x06, 0x02, 0x47, product->id, 0x00, 0x19,
            0x09, 0x09, 0x09, 0x09, 0x00, 0x7F, 0x7F, 0x7F, 0x7F};
        if ((product->id & ~2) != 0x58) reply[9] = reply[11] = 0x00;   /* 9.9 */
        memset(reply + 18, 0x7F, 16);
        if (serial[0]) memcpy(reply + 18, serial, 15);
        reply[34] = 0xF7;
        send_bytes(PANEL_PORT, reply, sizeof reply);
        if (verbose) LOG("answered MPC's identity request");
        return;
    }
    if (m[1] != 0x47 || n < 8) return;
    const unsigned char *data = m + 7;
    size_t len = n - 8;
    if (product->uart) {
        /* The Sample's request for the pots, sent as it starts. The reference
         * panel found an answer sent at once was lost, as the app writes this
         * before it reads, so the report goes again a moment later. */
        if (m[4] == 0x42) {
            report_pots();
            report_due = now_ms() + 1500;
        }
        return;
    }
    switch (m[4]) {
    case 0x52: {
        /* A ping, every few seconds, carrying four bytes to send back. The
         * byte after them is the panel's mode. 2 is the mode it powers up in,
         * and once MPC is running that answer makes it set the panel up again,
         * which is what a panel that had reset would need. */
        unsigned char pong[5] = {0, 0, 0, 0, (unsigned char)(mpc_mode ? 1 : 2)};
        if (len < 4) break;
        memcpy(pong, data, 4);
        akai(0x53, pong, sizeof pong);
        break;
    }
    case 0x62:
        mpc_mode = 1;
        break;
    case 0x79:
        if (len >= 15) {
            memcpy(serial, data, 15);
            serial[15] = '\0';
            LOG("MPC set the panel's serial number to %s", serial);
        }
        break;
    }
}

/* Split what has arrived into messages. Running status is honoured because the
 * rawmidi interface allows it, though MPC 3.10.0 was not seen to use it. */
static void parse(struct conn *c) {
    size_t i = 0;
    while (i < c->len) {
        unsigned char b = c->buf[i];
        if (b == 0xF0) {
            unsigned char *end = memchr(c->buf + i, 0xF7, c->len - i);
            if (!end) break;
            from_mpc(c->port, c->buf + i, (size_t)(end - (c->buf + i)) + 1);
            i = (size_t)(end - c->buf) + 1;
        } else if (b >= 0xF8) {
            i++;                              /* realtime */
        } else if (b >= 0x80) {
            size_t need = (b >= 0xC0 && b <= 0xDF) || b == 0xF1 || b == 0xF3 ? 2
                          : b >= 0xF4 ? 1 : 3;
            if (c->len - i < need) break;
            if (b < 0xF0) c->running = b;
            from_mpc(c->port, c->buf + i, need);
            i += need;
        } else if (c->running) {
            size_t need = (c->running >= 0xC0 && c->running <= 0xDF) ? 1 : 2;
            if (c->len - i < need) break;
            unsigned char m[3] = {c->running, c->buf[i], need == 2 ? c->buf[i + 1] : 0};
            from_mpc(c->port, m, need + 1);
            i += need;
        } else {
            i++;
        }
    }
    memmove(c->buf, c->buf + i, c->len - i);
    c->len -= i;
    if (c->len == sizeof c->buf) c->len = 0;   /* an unterminated SysEx; drop it */
}

static void command(char *line);

static void *surface_thread(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(), "mpcsurface");
    const char *volume = getenv("MPCSURFACE_VOLUME");
    if (volume && *volume) pots[POT_VOLUME] = (unsigned char)(atoi(volume) & 0x7F);
    const char *fifo = getenv("MPCSURFACE_FIFO");
    if (!fifo || !*fifo) fifo = "/run/mpcsurface.fifo";
    /* Ours to make: nothing else has to run before MPC for the panel to work.
     * Opened read-write so a writer going away is not an end of file. */
    unlink(fifo);
    int cmd_fd = -1;
    if (mkfifo(fifo, 0600) < 0 || (cmd_fd = open(fifo, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0)
        LOG("no command fifo at %s: %s", fifo, strerror(errno));
    else
        LOG("commands on %s", fifo);

    char line[1024];
    size_t line_len = 0;
    for (;;) {
        struct pollfd p[MAX_CONNS + 2];
        int who[MAX_CONNS + 2], n = 0;
        p[n].fd = wake_pipe[0]; p[n].events = POLLIN; who[n++] = -1;
        if (cmd_fd >= 0) { p[n].fd = cmd_fd; p[n].events = POLLIN; who[n++] = -2; }
        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_CONNS; i++)
            if (conns[i].fd >= 0) { p[n].fd = conns[i].fd; p[n].events = POLLIN; who[n++] = i; }
        pthread_mutex_unlock(&lock);
        int timeout = -1;
        if (report_due) {
            long long left = report_due - now_ms();
            timeout = left > 0 ? (int)left : 0;
        }
        int ready = poll(p, (nfds_t)n, timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (report_due && now_ms() >= report_due) {
            report_due = 0;
            report_pots();
        }
        for (int k = 0; k < n; k++) {
            if (!p[k].revents) continue;
            if (who[k] == -1) {
                char drain[64];
                while (read(wake_pipe[0], drain, sizeof drain) > 0) { }
            } else if (who[k] == -2) {
                ssize_t r = read(cmd_fd, line + line_len, sizeof line - 1 - line_len);
                if (r <= 0) continue;
                line_len += (size_t)r;
                line[line_len] = '\0';
                char *start = line, *nl;
                while ((nl = strchr(start, '\n'))) {
                    *nl = '\0';
                    command(start);
                    start = nl + 1;
                }
                line_len = strlen(start);
                memmove(line, start, line_len + 1);
                if (line_len == sizeof line - 1) line_len = 0;
            } else {
                struct conn *c = &conns[who[k]];
                ssize_t r = recv(c->fd, c->buf + c->len, sizeof c->buf - c->len, 0);
                if (r > 0) {
                    c->len += (size_t)r;
                    parse(c);
                } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                    LOG("MPC closed the %s port",
                        c->port == PORT_PRIVATE ? "private" : "public");
                    pthread_mutex_lock(&lock);
                    close(c->fd);
                    c->fd = -1;
                    pthread_mutex_unlock(&lock);
                }
            }
        }
    }
    return NULL;
}

/* ---- commands ---------------------------------------------------------------- */

/* A button is a note on channel 1, velocity 7F down and 0 up, and its lamp is
 * the control change of the same number coming back. */
struct button { const char *name; unsigned char note; };

/* ACVA3, the MPC One (Gen 2), on MPC 3.10.0. Found by pressing every note and
 * watching the screen and the lamps, with no panel to compare against, so a
 * name is what MPC did, in the words of its own screens, and a button that
 * changed nothing visible is not here. The numbers are not all Gen 1's: MENU
 * is 0x34 on this panel. */
static const struct button ACVA3_BUTTONS[] = {
    {"play", 0x52}, {"stop", 0x51}, {"rec", 0x49}, {"overdub", 0x50},
    {"play-start", 0x53}, {"locate", 0x46},
    {"menu", 0x34}, {"main", 0x29}, {"shift", 0x31}, {"undo", 0x7B},
    {"copy", 0x7A}, {"erase", 0x09}, {"note-repeat", 0x0B},
    {"full-level", 0x27}, {"16-level", 0x28}, {"tc", 0x43},
    {"automation", 0x4B}, {"plus", 0x36}, {"minus", 0x37},
    {"bank-a", 0x23}, {"bank-b", 0x24}, {"bank-c", 0x25}, {"bank-d", 0x26},
    {"sample-edit", 0x02}, {"pad-mixer", 0x03}, {"program-edit", 0x06},
    {"song", 0x08}, {"arrange", 0x0C}, {"next-seq", 0x2A},
    {"track-mute", 0x2B}, {"pad-color", 0x70}, {"track-mixer", 0x74},
    {"grid", 0x7D}, {"xyfx", 0x7E}, {"pad-perform", 0x7F},
    {"qlink-project", 0x3B}, {"qlink-track", 0x3C}, {"qlink-pad-scene", 0x3D},
    {"qlink-pad-param", 0x3E}, {"qlink-screen", 0x3F},
    {"dial", 0x6F},
    {NULL, 0},
};

/* AC50, MPC Sample, on 1.3.0. From the control map virtual-mpc-sample checked
 * against a real panel. The three screen buttons have no lamp; every other
 * lamp is the button's own number. The dial's push is 0x6F as on the MPCs. */
static const struct button AC50_BUTTONS[] = {
    {"sample", 0x06}, {"seq", 0x07}, {"pad-fx", 0x01}, {"knob-fx", 0x00},
    {"shift", 0x31}, {"pad-bank", 0x23}, {"erase", 0x09}, {"note-repeat", 0x0B},
    {"screen-1", 0x0C}, {"screen-2", 0x0D}, {"screen-3", 0x0E},
    {"chop", 0x4B}, {"mute", 0x2B}, {"loop", 0x4A}, {"16-levels", 0x28},
    {"sample-select", 0x24}, {"tap-tempo", 0x35}, {"minus", 0x37}, {"plus", 0x36},
    {"sample-rec", 0x49}, {"seq-rec", 0x50}, {"stop", 0x51}, {"play", 0x52},
    {"dial", 0x6F},
    {NULL, 0},
};

/* The Force, ADA2 and ADA2S. From IamForce (TheKikGen/MPCLiveXplore-libs,
 * src/tkgl_midimapper.h), which runs the Force app on an MPC's panel and so had
 * to send it the Force's own numbers. Those are the Gen 1 Force's. On ADA2S,
 * 3.10.0, matrix, note, mixer, menu, play and stop were pressed and did what the
 * name says; the rest are IamForce's. The mute row is the eight buttons under
 * the pads, which the mute, solo, rec-arm and clip-stop buttons repurpose, and
 * the column row the eight above them. */
static const struct button FORCE_BUTTONS[] = {
    {"navigate", 0}, {"knobs", 1}, {"menu", 2}, {"matrix", 3}, {"note", 4},
    {"master", 5}, {"clip", 9}, {"mixer", 11},
    {"load", 35}, {"save", 36}, {"edit", 37}, {"delete", 38},
    {"shift", 0x31}, {"select", 52}, {"tap-tempo", 0x35},
    {"plus", 54}, {"minus", 55}, {"undo", 67}, {"copy", 122},
    {"rec", 73}, {"stop", 81}, {"play", 82}, {"stop-all", 95},
    {"up", 112}, {"down", 113}, {"left", 114}, {"right", 115},
    {"launch", 116}, {"step-seq", 117}, {"arp", 118},
    {"mute", 91}, {"solo", 92}, {"rec-arm", 93}, {"clip-stop", 94},
    {"assign-a", 123}, {"assign-b", 124},
    {"launch-1", 56}, {"launch-2", 57}, {"launch-3", 58}, {"launch-4", 59},
    {"launch-5", 60}, {"launch-6", 61}, {"launch-7", 62}, {"launch-8", 63},
    {"column-1", 96}, {"column-2", 97}, {"column-3", 98}, {"column-4", 99},
    {"column-5", 100}, {"column-6", 101}, {"column-7", 102}, {"column-8", 103},
    {"mute-1", 41}, {"mute-2", 42}, {"mute-3", 43}, {"mute-4", 44},
    {"mute-5", 45}, {"mute-6", 46}, {"mute-7", 47}, {"mute-8", 48},
    {"dial", 0x6F},
    {NULL, 0},
};

static const struct { const char *code; const struct button *buttons; } BUTTONS[] = {
    {"ACVA3", ACVA3_BUTTONS},
    {"AC50", AC50_BUTTONS},
    {"ADA2", FORCE_BUTTONS},
    {"ADA2S", FORCE_BUTTONS},
};

static const struct button *buttons(void) {
    for (size_t i = 0; i < sizeof BUTTONS / sizeof BUTTONS[0]; i++)
        if (strcmp(BUTTONS[i].code, product->code) == 0) return BUTTONS[i].buttons;
    return NULL;
}

static int button_note(const char *name) {
    const struct button *b = buttons();
    if (!b) {
        LOG("no button names known for %s; use press <ch> <note>", product->code);
        return -1;
    }
    for (; b->name; b++)
        if (strcasecmp(b->name, name) == 0) return b->note;
    LOG("no button '%s' on %s; list shows the ones it has", name, product->code);
    return -1;
}

/* The pads are notes on channel 10, with the velocity struck, in the order the
 * MPC has always numbered them rather than in a run. Pad 1 is bottom left.
 * The Force's 64 are a run from FORCE_PAD_NOTE, top left along each row, which
 * is the order pad numbers here follow. A Force starts with its pads launching
 * clips, so they make no sound until note mode is chosen with the note button. */
#define FORCE_PAD_NOTE 0x36
static const unsigned char PAD_NOTES[16] = {
    0x25, 0x24, 0x2A, 0x52, 0x28, 0x26, 0x2E, 0x2C,
    0x30, 0x2F, 0x2D, 0x2B, 0x31, 0x37, 0x33, 0x35};

/* An encoder sends a control change on channel 1 whose value is the number of
 * detents turned since the last one, in seven bits two's complement. The Q-Links
 * are 0x10 up, from the top, and send the note 0x54 up while a finger is on the
 * cap, which is what brings their overlay onto the screen. Only the four every
 * panel has are named: see the header for what the fifth did. */
#define CC_DIAL 0x64
#define CC_QLINK 0x10
#define NOTE_QLINK_TOUCH 0x54
/* The Force has eight, with the same control changes and touch notes one lower,
 * from IamForce. */
#define NOTE_FORCE_QLINK_TOUCH 0x53

static int qlinks(void) { return product->pads == 64 ? 8 : 4; }
static int qlink_touch(int qlink) {
    return (product->pads == 64 ? NOTE_FORCE_QLINK_TOUCH : NOTE_QLINK_TOUCH) + qlink;
}

/* -1, or the control change for an encoder's name. */
static int encoder(const char *name, int *qlink) {
    *qlink = -1;
    if (strcasecmp(name, "dial") == 0) return CC_DIAL;
    if (product->uart) {
        LOG("MPC Sample has only the dial; its knobs take set");
        return -1;
    }
    if ((name[0] == 'q' || name[0] == 'Q') && name[1] >= '1' && name[1] < '1' + qlinks() &&
        !name[2]) {
        *qlink = name[1] - '1';
        return CC_QLINK + *qlink;
    }
    LOG("no encoder '%s'; there are dial and q1 to q%d", name, qlinks());
    return -1;
}

/* MPC Sample's absolute pots, by name, or -1. */
static int pot(const char *name) {
    static const char *const NAMES[POTS] = {"k1", "k2", "k3", "volume", "fader"};
    for (int i = 0; i < POTS; i++)
        if (strcasecmp(name, NAMES[i]) == 0) return i;
    LOG("no pot '%s'; there are k1 k2 k3 volume fader", name);
    return -1;
}

/* A button lets go with a note off, as the Sample's panel sends, and a USB
 * panel with a note on of velocity 0, as MPC's always has. */
static void tap(int status_channel, int note, int velocity, int ms) {
    send3(PANEL_PORT, 0x90 | status_channel, note, velocity);
    usleep((useconds_t)ms * 1000);
    if (product->uart) send3(PANEL_PORT, 0x80 | status_channel, note, 0);
    else send3(PANEL_PORT, 0x90 | status_channel, note, 0);
}

static void command(char *line) {
    char cmd[32] = "", name[32] = "";
    int n = 0;
    if (sscanf(line, "%31s%n", cmd, &n) != 1) return;
    char *rest = line + n;
    if (verbose) LOG("command: %s", line);

    if (strcmp(cmd, "button") == 0 || strcmp(cmd, "hold") == 0 ||
        strcmp(cmd, "release") == 0) {
        int ms = 80, note;
        if (sscanf(rest, "%31s %i", name, &ms) < 1 || (note = button_note(name)) < 0) return;
        if (cmd[0] == 'b') tap(0, note, 0x7F, ms);
        else if (cmd[0] == 'h') send3(PANEL_PORT, 0x90, note, 0x7F);
        else send3(PANEL_PORT, product->uart ? 0x80 : 0x90, note, 0);
    } else if (strcmp(cmd, "pad") == 0) {
        int pad, velocity = 100, ms = 80;
        if (sscanf(rest, "%i %i %i", &pad, &velocity, &ms) < 1) return;
        if (pad < 1 || pad > product->pads || velocity < 1 || velocity > 127) {
            LOG("usage: pad <1-%d> [velocity 1-127] [ms]", product->pads);
            return;
        }
        tap(9, product->pads == 64 ? FORCE_PAD_NOTE + pad - 1 : PAD_NOTES[pad - 1],
            velocity, ms);
    } else if (strcmp(cmd, "turn") == 0) {
        int steps, qlink, cc;
        if (sscanf(rest, "%31s %i", name, &steps) != 2 || (cc = encoder(name, &qlink)) < 0)
            return;
        /* One detent a message. A Q-Link is touched for as long as it is
         * turned, as a real one cannot be turned without. */
        if (qlink >= 0) send3(PANEL_PORT, 0x90, qlink_touch(qlink), 0x7F);
        for (int i = 0; i < abs(steps); i++) {
            send3(PANEL_PORT, 0xB0, cc, steps > 0 ? 1 : 0x7F);
            usleep(5000);
        }
        if (qlink >= 0) send3(PANEL_PORT, 0x90, qlink_touch(qlink), 0);
    } else if (strcmp(cmd, "touch") == 0) {
        int ms = 500, qlink;
        if (sscanf(rest, "%31s %i", name, &ms) < 1 || encoder(name, &qlink) < 0) return;
        if (qlink < 0) {
            LOG("the dial has no touch sensor");
            return;
        }
        tap(0, qlink_touch(qlink), 0x7F, ms);
    } else if (strcmp(cmd, "set") == 0) {
        int value, which;
        if (!product->uart) {
            LOG("set is for MPC Sample's knobs; this panel's turn");
            return;
        }
        if (sscanf(rest, "%31s %i", name, &value) != 2 || (which = pot(name)) < 0) return;
        if (value < 0 || value > 127) {
            LOG("usage: set <k1|k2|k3|volume|fader> <0-127>");
            return;
        }
        pots[which] = (unsigned char)value;
        send3(PANEL_PORT, 0xB0, CC_POT + which, value);
    } else if (strcmp(cmd, "list") == 0) {
        char names[1024] = "";
        size_t o = 0;
        for (const struct button *b = buttons(); b && b->name && o < sizeof names; b++)
            o += (size_t)snprintf(names + o, sizeof names - o, " %s", b->name);
        LOG("%s buttons:%s", product->code, *names ? names : " none named, use press");
    } else if (strcmp(cmd, "press") == 0) {
        int ch, note, ms = 80;
        if (sscanf(rest, "%i %i %i", &ch, &note, &ms) < 2) return;
        tap(ch & 15, note, 0x7F, ms);
    } else if (strcmp(cmd, "on") == 0) {
        int ch, note, vel = 0x7F;
        if (sscanf(rest, "%i %i %i", &ch, &note, &vel) >= 2)
            send3(PANEL_PORT, 0x90 | (ch & 15), note, vel);
    } else if (strcmp(cmd, "off") == 0) {
        int ch, note;
        if (sscanf(rest, "%i %i", &ch, &note) == 2)
            send3(PANEL_PORT, 0x80 | (ch & 15), note, 0);
    } else if (strcmp(cmd, "cc") == 0) {
        int ch, cc, val;
        if (sscanf(rest, "%i %i %i", &ch, &cc, &val) == 3)
            send3(PANEL_PORT, 0xB0 | (ch & 15), cc, val);
    } else if (strcmp(cmd, "sysex") == 0) {
        unsigned char buf[512];
        size_t len = 0;
        for (char *tok = strtok(rest, " \t\r"); tok && len < sizeof buf;
             tok = strtok(NULL, " \t\r"))
            buf[len++] = (unsigned char)strtol(tok, NULL, 16);
        if (len) send_bytes(PANEL_PORT, buf, len);
    } else {
        LOG("unknown command: %s", cmd);
    }
}
