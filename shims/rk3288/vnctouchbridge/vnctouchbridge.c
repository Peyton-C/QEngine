#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <netdb.h>
#include <linux/uinput.h>

/* ---- uinput virtual touchscreen (same protocol as touchsim) ---- */

static int uifd = -1;
static int screen_w = 1024, screen_h = 768;
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
    usetup.id.product = 0x5679;
    strcpy(usetup.name, "VncTouchBridge Virtual Touchscreen");
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

/* ---- socket helpers ---- */

static int read_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, (const char *)buf + sent, n - sent);
        if (w <= 0) return -1;
        sent += w;
    }
    return 0;
}

static int connect_upstream(const char *host, int port) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

/* ---- server->client raw relay thread (no parsing needed) ---- */

struct relay_args { int from_fd; int to_fd; };

static void *relay_thread(void *arg) {
    struct relay_args *a = (struct relay_args *)arg;
    char buf[65536];
    for (;;) {
        ssize_t r = read(a->from_fd, buf, sizeof(buf));
        if (r <= 0) break;
        if (write_full(a->to_fd, buf, (size_t)r) < 0) break;
    }
    shutdown(a->to_fd, SHUT_WR);
    free(a);
    return NULL;
}

/* ---- RFB 3.3 handshake pass-through (server=upstream Engine VNC, client=downstream real VNC client) ---- */

static int do_handshake(int client_fd, int upstream_fd, int *out_w, int *out_h) {
    char version[12];

    /* 1. server -> client: protocol version */
    if (read_full(upstream_fd, version, 12) < 0) return -1;
    if (write_full(client_fd, version, 12) < 0) return -1;

    /* 2. client -> server: protocol version */
    if (read_full(client_fd, version, 12) < 0) return -1;
    if (write_full(upstream_fd, version, 12) < 0) return -1;

    /* 3. server -> client: security type (RFB 3.3: unilateral u32) */
    unsigned char sectype[4];
    if (read_full(upstream_fd, sectype, 4) < 0) return -1;
    if (write_full(client_fd, sectype, 4) < 0) return -1;
    /* assume security = None (1); this build always uses None */

    /* 4. client -> server: ClientInit (1 byte shared-flag) */
    unsigned char clientinit;
    if (read_full(client_fd, &clientinit, 1) < 0) return -1;
    if (write_full(upstream_fd, &clientinit, 1) < 0) return -1;

    /* 5. server -> client: ServerInit (parse width/height, relay whole message raw) */
    unsigned char head[24];
    if (read_full(upstream_fd, head, 24) < 0) return -1;
    int w = (head[0] << 8) | head[1];
    int h = (head[2] << 8) | head[3];
    unsigned int namelen = ((unsigned int)head[20] << 24) | (head[21] << 16) | (head[22] << 8) | head[23];
    if (write_full(client_fd, head, 24) < 0) return -1;
    if (namelen > 0) {
        char *name = malloc(namelen);
        if (read_full(upstream_fd, name, namelen) < 0) { free(name); return -1; }
        if (write_full(client_fd, name, namelen) < 0) { free(name); return -1; }
        free(name);
    }

    *out_w = w;
    *out_h = h;
    fprintf(stderr, "Handshake complete. Framebuffer %dx%d.\n", w, h);
    return 0;
}

/* ---- client->server message loop: intercept PointerEvent (type 5), forward everything else ---- */

static void run_message_loop(int client_fd, int upstream_fd) {
    unsigned char last_mask = 0;

    for (;;) {
        unsigned char msg_type;
        if (read_full(client_fd, &msg_type, 1) < 0) break;

        switch (msg_type) {
            case 0: { /* SetPixelFormat: 1+3pad+16 = 20 bytes total (19 more) */
                unsigned char rest[19];
                if (read_full(client_fd, rest, 19) < 0) goto done;
                if (write_full(upstream_fd, &msg_type, 1) < 0) goto done;
                if (write_full(upstream_fd, rest, 19) < 0) goto done;
                break;
            }
            case 2: { /* SetEncodings: 1+1pad+2count+4*count */
                unsigned char hdr[3];
                if (read_full(client_fd, hdr, 3) < 0) goto done;
                int count = (hdr[1] << 8) | hdr[2];
                size_t enc_bytes = (size_t)count * 4;
                unsigned char *enc = malloc(enc_bytes > 0 ? enc_bytes : 1);
                if (enc_bytes > 0 && read_full(client_fd, enc, enc_bytes) < 0) { free(enc); goto done; }
                if (write_full(upstream_fd, &msg_type, 1) < 0 ||
                    write_full(upstream_fd, hdr, 3) < 0 ||
                    (enc_bytes > 0 && write_full(upstream_fd, enc, enc_bytes) < 0)) {
                    free(enc);
                    goto done;
                }
                free(enc);
                break;
            }
            case 3: { /* FramebufferUpdateRequest: 1+1+2+2+2+2 = 10 bytes total (9 more) */
                unsigned char rest[9];
                if (read_full(client_fd, rest, 9) < 0) goto done;
                if (write_full(upstream_fd, &msg_type, 1) < 0) goto done;
                if (write_full(upstream_fd, rest, 9) < 0) goto done;
                break;
            }
            case 4: { /* KeyEvent: 1+1+2pad+4 = 8 bytes total (7 more) — forward as-is, keyboard already works */
                unsigned char rest[7];
                if (read_full(client_fd, rest, 7) < 0) goto done;
                if (write_full(upstream_fd, &msg_type, 1) < 0) goto done;
                if (write_full(upstream_fd, rest, 7) < 0) goto done;
                break;
            }
            case 5: { /* PointerEvent: 1+1+2+2 = 6 bytes total (5 more) — INTERCEPT, convert to touch */
                unsigned char rest[5];
                if (read_full(client_fd, rest, 5) < 0) goto done;
                unsigned char mask = rest[0];
                int x = (rest[1] << 8) | rest[2];
                int y = (rest[3] << 8) | rest[4];

                if (!touch_active && mask != 0) {
                    touch_down(x, y);
                    touch_active = 1;
                } else if (touch_active && mask != 0) {
                    touch_move(x, y);
                } else if (touch_active && mask == 0) {
                    touch_up();
                    touch_active = 0;
                }
                last_mask = mask;
                (void)last_mask;
                /* not forwarded upstream */
                break;
            }
            case 6: { /* ClientCutText: 1+3pad+4len+len */
                unsigned char hdr[7];
                if (read_full(client_fd, hdr, 7) < 0) goto done;
                unsigned int len = ((unsigned int)hdr[3] << 24) | (hdr[4] << 16) | (hdr[5] << 8) | hdr[6];
                unsigned char *text = malloc(len > 0 ? len : 1);
                if (len > 0 && read_full(client_fd, text, len) < 0) { free(text); goto done; }
                if (write_full(upstream_fd, &msg_type, 1) < 0 ||
                    write_full(upstream_fd, hdr, 7) < 0 ||
                    (len > 0 && write_full(upstream_fd, text, len) < 0)) {
                    free(text);
                    goto done;
                }
                free(text);
                break;
            }
            default:
                fprintf(stderr, "unknown client message type %d, aborting connection\n", msg_type);
                goto done;
        }
    }
done:
    if (touch_active) {
        touch_up();
        touch_active = 0;
    }
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <listen_port> <upstream_host> <upstream_port> [width] [height]\n", argv[0]);
        return 1;
    }
    int listen_port = atoi(argv[1]);
    const char *upstream_host = argv[2];
    int upstream_port = atoi(argv[3]);
    if (argc >= 6) {
        screen_w = atoi(argv[4]);
        screen_h = atoi(argv[5]);
    }

    signal(SIGPIPE, SIG_IGN);

    /* Create the uinput device upfront, at a known/fixed size, so its
     * /dev/input/eventN path is stable and known before Engine ever starts
     * (Engine needs QT_QPA_GENERIC_PLUGINS=evdevtouch:<path> at launch time,
     * well before any real VNC client ever connects to us). */
    setup_uinput_device();

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listen_port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind failed: %s\n", strerror(errno));
        return 1;
    }
    listen(lfd, 1);
    fprintf(stderr, "vnctouchbridge listening on %d, upstream %s:%d\n", listen_port, upstream_host, upstream_port);

    for (;;) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int client_fd = accept(lfd, (struct sockaddr *)&cliaddr, &clilen);
        if (client_fd < 0) continue;
        int one2 = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one2, sizeof(one2));
        fprintf(stderr, "client connected\n");

        int upstream_fd = connect_upstream(upstream_host, upstream_port);
        if (upstream_fd < 0) {
            fprintf(stderr, "failed to connect upstream: %s\n", strerror(errno));
            close(client_fd);
            continue;
        }

        int w = 0, h = 0;
        if (do_handshake(client_fd, upstream_fd, &w, &h) < 0) {
            fprintf(stderr, "handshake failed\n");
            close(client_fd);
            close(upstream_fd);
            continue;
        }
        if (w != screen_w || h != screen_h) {
            fprintf(stderr, "warning: upstream framebuffer is %dx%d, uinput device set up for %dx%d\n",
                    w, h, screen_w, screen_h);
        }

        struct relay_args *ra = malloc(sizeof(*ra));
        ra->from_fd = upstream_fd;
        ra->to_fd = client_fd;
        pthread_t tid;
        pthread_create(&tid, NULL, relay_thread, ra);
        pthread_detach(tid);

        run_message_loop(client_fd, upstream_fd);

        fprintf(stderr, "client disconnected\n");
        close(client_fd);
        close(upstream_fd);
    }

    return 0;
}