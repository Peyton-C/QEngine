/* midibridge: MIDI between a QEMU guest and its host, over virtio-serial.
 *
 * On the Pi, Engine runs in a chroot on the host's own kernel, so a controller
 * plugged into the Pi is already a sequencer client Engine and midisurface can
 * see. A QEMU guest has no such thing. usb-host passthrough is the only other
 * way in, and it needs a Linux host, takes the device away from the host, and
 * hands the guest a whole USB device for one controller. QEMU has no emulated
 * USB MIDI device to offer instead.
 *
 * This makes the QEMU guest look like the Pi from the inside. run_qemu.sh gives
 * an Engine guest one virtio-serial port, named org.qengine.midi, whose host end
 * is a localhost TCP socket. This program opens the guest end and presents it as
 * a sequencer client called "QEngine MIDI", with one port that reads and writes:
 *
 *   host controller/DAW <-> host side <-> TCP <-> virtio-serial <-> midibridge
 *       <-> sequencer <-> midisurface --forward "QEngine MIDI" <-> Engine
 *
 * The wire format is a plain MIDI 1.0 byte stream in both directions, no
 * framing. MIDI frames itself: every status byte has the top bit set and every
 * data byte does not, so a reader can always find where a message starts, and
 * SysEx (which the identity handshake is) passes through as F0 ... F7.
 * Running status is accepted from the host but never sent to it, so the host
 * side can treat every message as self-contained.
 *
 * The port presents as MIDI_GENERIC|HARDWARE, the same as a controller on the
 * Pi does, so Engine enumerates it the same way. Engine will not bind it unless
 * whatever is on the host answers its identity inquiry as an inMusic device,
 * which is midisurface's job; midisurface forwards from it as it forwards from a
 * USB controller on the Pi.
 *
 * Found by name under /sys/class/virtio-ports rather than at
 * /dev/virtio-ports/<name>, because that link is made by a systemd udev rule
 * and the kernel only guarantees /dev/vportNpM. The driver, virtio_console, is
 * built into both of get_kernel.sh's Debian kernels.
 *
 * With no such port -- an instance launched before run_qemu.sh added it, or a
 * QEMU without virtio-serial -- this exits 0, so the unit does not restart-loop.
 *
 * Usage: midibridge [-v] [--device /dev/vportNpM]
 */
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VPORT_NAME "org.qengine.midi"
#define CLIENT_NAME "QEngine MIDI"

static int verbose;
static snd_seq_t *seq;
static int my_port;
static int vport = -1;
static int host_up;
static snd_midi_event_t *enc, *dec;
static unsigned long dropped;

static int find_vport(char *path, size_t n) {
    DIR *d = opendir("/sys/class/virtio-ports");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[300], name[128] = "";
        snprintf(p, sizeof p, "/sys/class/virtio-ports/%s/name", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
        fclose(f);
        if (strcmp(name, VPORT_NAME) == 0) {
            snprintf(path, n, "/dev/%s", e->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

/* virtio_console reports POLLHUP for as long as nothing is connected on the
 * host end, and a read then returns 0 rather than blocking. So the port cannot
 * be left in the main poll while the host is away, or the loop spins; it is
 * asked again on a timer instead, until the HUP goes. */
static int host_connected(void) {
    struct pollfd p = { .fd = vport, .events = POLLIN };
    if (poll(&p, 1, 0) < 0) return 0;
    return !(p.revents & POLLHUP);
}

static void set_host_up(int up) {
    if (up == host_up) return;
    host_up = up;
    /* Half a message left over from the last connection would corrupt the
     * first one of the next. */
    snd_midi_event_reset_encode(enc);
    printf("host %s\n", up ? "connected" : "disconnected");
    fflush(stdout);
}

/* Bytes from the host, out to whoever subscribes to our port. */
static void from_host(void) {
    unsigned char buf[1024];
    ssize_t n = read(vport, buf, sizeof buf);
    if (n == 0) { set_host_up(0); return; }
    if (n < 0) {
        if (errno != EAGAIN && errno != EINTR) set_host_up(0);
        return;
    }
    long off = 0;
    while (off < n) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        long used = snd_midi_event_encode(enc, buf + off, n - off, &ev);
        if (used <= 0) break;
        off += used;
        if (ev.type == SND_SEQ_EVENT_NONE) continue;
        if (verbose) fprintf(stderr, "[bridge] host -> guest type %d\n", ev.type);
        snd_seq_ev_set_source(&ev, my_port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct(seq, &ev);
    }
}

/* Non-blocking, and dropped rather than queued when the host is not reading:
 * blocking here would stall the sequencer side, and Engine streams lamp
 * updates continuously once it has a surface. Running status is off, so a
 * dropped message never changes the meaning of the ones after it. */
static void to_host(const unsigned char *b, long n) {
    if (!host_up) return;
    while (n > 0) {
        ssize_t w = write(vport, b, (size_t)n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN) set_host_up(0);
            if (++dropped == 1 || dropped % 1000 == 0)
                fprintf(stderr, "host not reading; %lu messages dropped\n", dropped);
            return;
        }
        b += w;
        n -= w;
    }
}

/* Events sent to our port, out to the host as bytes. */
static void from_guest(void) {
    static unsigned char buf[65536];
    snd_seq_event_t *ev;
    int r;
    while ((r = snd_seq_event_input(seq, &ev)) >= 0 || r == -ENOSPC) {
        if (r < 0) {
            fprintf(stderr, "sequencer input overrun\n");
            continue;
        }
        /* Subscription notices and the like, not MIDI. */
        if (ev->source.client == SND_SEQ_CLIENT_SYSTEM) continue;
        long n = snd_midi_event_decode(dec, buf, sizeof buf, ev);
        if (n <= 0) continue;
        if (verbose) fprintf(stderr, "[bridge] guest -> host %ld bytes\n", n);
        to_host(buf, n);
    }
}

int main(int argc, char **argv) {
    char path[300] = "";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            snprintf(path, sizeof path, "%s", argv[++i]);
        } else {
            fprintf(stderr, "usage: %s [-v] [--device /dev/vportNpM]\n", argv[0]);
            return 1;
        }
    }
    if (!*path && find_vport(path, sizeof path) < 0) {
        printf("no virtio-serial port named %s; nothing to bridge\n", VPORT_NAME);
        return 0;
    }
    vport = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (vport < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return 1;
    }

    int err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
    if (err < 0) {
        fprintf(stderr, "snd_seq_open: %s\n", snd_strerror(err));
        return 1;
    }
    snd_seq_set_client_name(seq, CLIENT_NAME);
    my_port = snd_seq_create_simple_port(
        seq, CLIENT_NAME,
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE);
    if (my_port < 0) {
        fprintf(stderr, "create_simple_port: %s\n", snd_strerror(my_port));
        return 1;
    }

    /* SysEx longer than the encoder's buffer goes out in pieces, which the
     * sequencer allows; 4096 holds every inMusic message seen so far whole. */
    if (snd_midi_event_new(4096, &enc) < 0 || snd_midi_event_new(4096, &dec) < 0) {
        fprintf(stderr, "snd_midi_event_new failed\n");
        return 1;
    }
    snd_midi_event_no_status(dec, 1);

    printf("bridging %s as \"%s\" (client %d port %d)\n",
           path, CLIENT_NAME, snd_seq_client_id(seq), my_port);
    fflush(stdout);
    set_host_up(host_connected());

    int nseq = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd *pfds = calloc((size_t)nseq + 1, sizeof *pfds);
    if (!pfds) return 1;

    for (;;) {
        snd_seq_poll_descriptors(seq, pfds, (unsigned)nseq, POLLIN);
        int n = nseq;
        if (host_up) {
            pfds[n].fd = vport;
            pfds[n].events = POLLIN;
            pfds[n].revents = 0;
            n++;
        }
        if (poll(pfds, (nfds_t)n, host_up ? -1 : 250) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            return 1;
        }
        if (host_up && (pfds[nseq].revents & POLLHUP)) set_host_up(0);
        else if (host_up && (pfds[nseq].revents & POLLIN)) from_host();
        if (!host_up && host_connected()) set_host_up(1);
        from_guest();
    }
}
