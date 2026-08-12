/* A virtual MIDI control surface for the emulated RANE SYSTEM ONE.
 *
 * SYSTEM ONE's transport controls (play, cue, load, sync, ...) are physical
 * buttons on the unit, not touchscreen elements — Engine's on-screen UI has
 * no way to start a deck playing. Under emulation that leaves no way to
 * trigger playback at all: touch works (see touchbridge_rmz2) but only
 * reaches the browse/deck views, never the transport. This creates an ALSA
 * sequencer client that Engine's MIDI device enumerator can discover and
 * bind to a preset assignment file, so the emulated unit can be driven
 * exactly the way the real control surface drives it.
 *
 * The MIDI vocabulary comes from the rootfs's own preset assignment files
 * (/usr/Engine/AssignmentFiles/PresetAssignmentFiles/RMZ2/), which are
 * plain QML and readable directly — no reverse engineering needed:
 *
 *   RMZ2_Controller_Assignments.qml
 *     Left deck  -> midiChannel 0x04, load note 0x1A
 *     Right deck -> midiChannel 0x05, load note 0x1B
 *     PlayCue    -> playNote 0x01, cueNote 0x02
 *     Sync 0x14, Shift 0x5D, Censor 0x0B, Slip 0x20, ...
 *   RMZ2_Controller_Device.qml
 *     SysEx identity: F0 00 00 17 <deviceId 7F> <productId 27> ... F7
 *     (manufacturer 00 00 17 = inMusic, product 0x27 = SYSTEM ONE)
 *
 * Engine does NOT bind an assignment file by device name — it identifies
 * control surfaces with a MIDI Device Inquiry handshake, driven by a
 * KnownDevices table it logs at startup (air.deviceidentifier):
 *
 *   <IdRequest message="7E 7F 06 01"/>
 *   <Device type="USB" realName="Controller">
 *     <property name="DeviceInquiryResponse"
 *               value="7E ?? 06 02 00 00 17 27 ?? ?? ?? ?? ?? ?? 7F"/>
 *     <property name="AssignmentFileName" value="RMZ2 Controller"/>
 *
 * So Engine broadcasts the universal identity request F0 7E 7F 06 01 F7 and
 * waits for a reply carrying inMusic's manufacturer id (00 00 17) and
 * product 0x27; only then does it load the assignment file and start
 * treating incoming notes as control-surface input. A device that never
 * answers is enumerated and connected but its MIDI is ignored — which looks
 * exactly like "the buttons do nothing". This program therefore answers the
 * inquiry automatically (see ID_RESPONSE), impersonating SYSTEM ONE's own
 * control surface. The client name is cosmetic by comparison; it defaults to
 * "RMZ2_Controller" and can be overridden with argv[1].
 *
 * Commands are read from stdin, one per line, so this can be driven
 * interactively or fed from a script/fifo:
 *
 *   press <ch> <note> [ms]   note-on then note-off (a button tap; default 80ms)
 *   on    <ch> <note> [vel]  note-on
 *   off   <ch> <note>        note-off
 *   cc    <ch> <cc> <val>    control change
 *   sysex <hex bytes...>     raw sysex, e.g. sysex F0 00 00 17 7F 27 ... F7
 *   play  left|right         convenience for the deck play button
 *   cue   left|right         convenience for the deck cue button
 *   load  left|right         convenience for the deck load button
 *   motor left|right         toggle the deck's motorized-platter mode
 *                            (shift + slip). REQUIRED ONCE PER ENGINE START
 *                            before play will do anything — see below.
 *   ports                    list sequencer clients/ports and our own id
 *   quit
 *
 * Events are sent to whatever has subscribed to our output port (Engine
 * subscribes when it binds the device), so no explicit routing is needed in
 * the normal case.
 */
#include <alsa/asoundlib.h>
#include <ctype.h>
#include <time.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Deck identities straight out of RMZ2_Controller_Assignments.qml. */
#define DECK_LEFT_CH 0x04
#define DECK_RIGHT_CH 0x05
#define NOTE_PLAY 0x01
#define NOTE_CUE 0x02
#define LOAD_NOTE_LEFT 0x1A
#define LOAD_NOTE_RIGHT 0x1B
/* Shift + Slip is Action.ToggleMotor in RMZ2_Controller_Assignments.qml. */
#define NOTE_SHIFT 0x5D
#define NOTE_SLIP 0x20


/* Universal MIDI device inquiry, as Engine's KnownDevices IdRequest sends it. */
static const unsigned char ID_REQUEST[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};

/* The reply Engine's DeviceInquiryResponse pattern accepts:
 *   7E ?? 06 02 00 00 17 27 ?? ?? ?? ?? ?? ?? 7F
 * i.e. a standard identity reply — 7E <device id> 06 02, inMusic's
 * manufacturer id 00 00 17, family LSB 0x27 (SYSTEM ONE, the same product id
 * RMZ2_Controller_Device.qml uses in its own sysex) — with family/member and
 * the software-revision bytes left free apart from a trailing 0x7F.
 *
 * The revision bytes matter: Engine compares them against
 * /usr/Engine/Firmware/RMZ2 Controller/firmware.json ("version": "1.0.0.27")
 * and, on a mismatch, flashes UpdateImage.rbin — putting the unit into a
 * full-screen "UPDATING... PLAYER WILL REBOOT AFTER UPDATE" state that a
 * virtual surface can never complete. The comparison is for *equality*, not
 * "older than": Engine OS supports official downgrades, where an older
 * release's control-surface firmware must be flashed back over a newer one.
 * Reporting all-0x7F (the highest value 7-bit MIDI data bytes can hold) was
 * tried and still triggered the update, confirming that.
 *
 * The revision is the four bytes at index 11 of the reply (Engine parses them
 * via FUN_0080bd84(response, 0xb)), counting the leading F0, each rendered in
 * *decimal*. That was pinned down empirically: sending 01 00 00 27 made
 * Engine's Settings > About/Update screen report "Controller Version:
 * 1.0.0.39" — 0x27 = 39 — so the bytes below encode 1.0.0.27 as 01 00 00 1B,
 * matching firmware.json exactly and leaving the unit alone. */
static const unsigned char ID_RESPONSE_DEFAULT[] = {
    0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x00, 0x17, 0x27,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x1B, 0x7F, 0xF7};

/* Overridable via MIDISURFACE_ID_RESPONSE (space-separated hex), since the
 * exact placement of the revision field inside the reply is inferred rather
 * than documented — being able to try an encoding without a rebuild/redeploy
 * cycle is worth the few lines. */
static unsigned char id_response[64];
static size_t id_response_len = 0;

static void init_id_response(void) {
    const char *env = getenv("MIDISURFACE_ID_RESPONSE");
    if (env && *env) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", env);
        char *tok = strtok(buf, " \t,");
        while (tok && id_response_len < sizeof(id_response)) {
            id_response[id_response_len++] =
                (unsigned char)strtol(tok, NULL, 16);
            tok = strtok(NULL, " \t,");
        }
        if (id_response_len) return;
    }
    memcpy(id_response, ID_RESPONSE_DEFAULT, sizeof(ID_RESPONSE_DEFAULT));
    id_response_len = sizeof(ID_RESPONSE_DEFAULT);
}

static snd_seq_t *seq = NULL;
static int my_port = -1;
static int verbose = 0;

/* --forward: relay a real controller's MIDI into Engine.
 *
 * Engine will only bind a device that answers its inMusic identity inquiry,
 * which no third-party controller does — so a generic controller cannot drive
 * Engine directly, no matter how it is mapped. This program is already the
 * thing Engine trusts, so it can carry the controller's events through: we
 * subscribe to the controller's port and re-emit whatever arrives to our own
 * subscribers (i.e. Engine). Events are relayed unchanged; translating a
 * controller's notes/CCs to what Engine expects is done in the assignment
 * QML instead (see controllermap.sh), which costs nothing at runtime and
 * needs no per-controller code here. */
static int forward_client = -1;
static int forward_port = -1;

/* Auto motor-off. SYSTEM ONE's decks wait on platter timecode that cannot
 * exist under emulation, so play does nothing until motorized mode is toggled
 * off, and Engine does not persist that setting — it starts motorized every
 * time. Firing on Engine's identity inquiry is the right trigger because that
 * is precisely when Engine has (re)bound us, so it re-arms naturally across
 * Engine restarts without this process restarting.
 *
 * It must fire exactly once per binding, though: the control is a *toggle*,
 * and Engine sends the inquiry more than once per startup (two is typical),
 * so acting on each one would turn the motor straight back on. Hence a
 * debounce — each inquiry re-arms a timer, and only the quiet period after
 * the last one triggers the toggles. */
static int motor_off_enabled = 0;
static long long motor_off_due_ms = 0;   /* 0 = disarmed */
#define MOTOR_OFF_DEBOUNCE_MS 4000

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void send_event(snd_seq_event_t *ev) {
    snd_seq_ev_set_source(ev, my_port);
    snd_seq_ev_set_subs(ev);   /* deliver to all subscribers */
    snd_seq_ev_set_direct(ev);
    int err = snd_seq_event_output_direct(seq, ev);
    if (err < 0)
        fprintf(stderr, "send failed: %s\n", snd_strerror(err));
}

static void note_on(int ch, int note, int vel) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_noteon(&ev, ch, note, vel);
    send_event(&ev);
}

static void note_off(int ch, int note) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_noteoff(&ev, ch, note, 0);
    send_event(&ev);
}

static void control_change(int ch, int param, int val) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_controller(&ev, ch, param, val);
    send_event(&ev);
}

/* A button tap: press, hold briefly, release. Engine distinguishes taps from
 * holds for several controls (cue-hold, shift, sync-hold => instant double),
 * so the hold duration is caller-controllable. */
static void press(int ch, int note, int ms) {
    note_on(ch, note, 0x7F);
    usleep((useconds_t)ms * 1000);
    note_off(ch, note);
}

/* SYSTEM ONE has motorized platters, and its deck assignment ends in
 * MotorizedTimecode { } — the platter reports its position as a timecode
 * signal carried on the codec's capture channels, DVS-style, not over MIDI.
 * Under emulation no platter exists, so with the motor engaged a deck accepts
 * the play command and then sits there waiting for platter timecode that will
 * never arrive: play appears to do nothing at all, while cue still previews
 * audio normally (it bypasses the platter). Toggling motorized mode off makes
 * it behave like an ordinary non-motorized deck and play works.
 *
 * This has to be re-sent after every Engine start. The mode lives at
 * /Client/Preferences/Profile/Application/PlatterMode but is not persisted —
 * confirmed by stopping engine.service cleanly (so Qt flushes its settings)
 * and diffing rmz2.user.settings/Engine.conf: no key is written, and nothing
 * platter-related appears anywhere under /data. Because Engine therefore
 * always starts motorized, a single toggle is deterministic rather than a
 * coin flip on unknown state.
 *
 * Two alternatives were tried against the assignment file and both failed
 * (each reverted): deleting MotorizedTimecode { } outright — other controls
 * kept working, so the file loaded fine, but play was still dead, i.e. that
 * component wires the timecode input rather than selecting the mode — and
 * replacing it with a MIDI JogWheel { } as the non-motorized products
 * (JC11/JP11) use. The mode is the gate, not the platter source. */
static void toggle_motor(int deck_channel) {
    note_on(deck_channel, NOTE_SHIFT, 0x7F);
    usleep(150 * 1000);
    press(deck_channel, NOTE_SLIP, 80);
    usleep(150 * 1000);
    note_off(deck_channel, NOTE_SHIFT);
}

static void send_sysex(unsigned char *buf, size_t len) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_sysex(&ev, len, buf);
    send_event(&ev);
}

static void list_ports(void) {
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    printf("our client id: %d, port: %d\n", snd_seq_client_id(seq), my_port);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        printf("client %3d: \"%s\"\n", client,
               snd_seq_client_info_get_name(cinfo));
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            printf("    port %3d: \"%s\" caps=0x%x\n",
                   snd_seq_port_info_get_port(pinfo),
                   snd_seq_port_info_get_name(pinfo),
                   snd_seq_port_info_get_capability(pinfo));
        }
    }
    fflush(stdout);
}

/* Drain and act on anything Engine sends us. Two reasons this must run even
 * when we have nothing to say: the identity handshake above is mandatory
 * before Engine will honour any input, and an unread port fills its input
 * pool (Engine streams LED/display updates continuously once bound). */
static void handle_incoming(void) {
    snd_seq_event_t *ev;
    while (snd_seq_event_input(seq, &ev) >= 0) {
        /* Anything from the forwarded controller goes straight out to Engine.
         * Engine's own messages to us (LEDs, pad displays, the inquiry) come
         * from a different client and must not be echoed back. */
        if (forward_client >= 0 && ev->source.client == forward_client) {
            snd_seq_event_t out = *ev;
            if (verbose)
                fprintf(stderr, "[surface] forwarding type %d from %d:%d\n",
                        ev->type, ev->source.client, ev->source.port);
            send_event(&out);
            if (snd_seq_event_input_pending(seq, 0) <= 0) break;
            continue;
        }
        if (ev->type == SND_SEQ_EVENT_SYSEX) {
            const unsigned char *d = (const unsigned char *)ev->data.ext.ptr;
            unsigned int len = ev->data.ext.len;
            if (verbose) {
                fprintf(stderr, "[surface] sysex in (%u):", len);
                for (unsigned int i = 0; i < len && i < 24; i++)
                    fprintf(stderr, " %02X", d[i]);
                fprintf(stderr, "\n");
            }
            /* Identity request: F0 7E <dev> 06 01 F7, device id wildcarded. */
            if (len >= sizeof(ID_REQUEST) && d[0] == 0xF0 && d[1] == 0x7E &&
                d[3] == 0x06 && d[4] == 0x01) {
                snd_seq_event_t out;
                snd_seq_ev_clear(&out);
                snd_seq_ev_set_sysex(&out, id_response_len,
                                     (void *)id_response);
                send_event(&out);
                printf("answered device inquiry (identifying as inMusic 0x27)\n");
                fflush(stdout);
                if (motor_off_enabled) motor_off_due_ms = now_ms() + MOTOR_OFF_DEBOUNCE_MS;
            }
        }
        if (snd_seq_event_input_pending(seq, 0) <= 0) break;
    }
}

static int deck_channel(const char *which) {
    if (!which) return -1;
    if (strcasecmp(which, "left") == 0 || strcmp(which, "1") == 0)
        return DECK_LEFT_CH;
    if (strcasecmp(which, "right") == 0 || strcmp(which, "2") == 0)
        return DECK_RIGHT_CH;
    return -1;
}

/* Subscribe to the first sequencer port whose client name contains `match`
 * and that can be read from, skipping our own client and Engine's. Matching on
 * a substring rather than an exact name keeps this usable against whatever
 * name a given controller's USB descriptor produces. */
static int connect_forward_source(const char *match) {
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    int me = snd_seq_client_id(seq);

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        const char *cname = snd_seq_client_info_get_name(cinfo);
        if (client == me || client == SND_SEQ_CLIENT_SYSTEM) continue;
        if (!cname || !strstr(cname, match)) continue;

        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned int caps = snd_seq_port_info_get_capability(pinfo);
            if ((caps & SND_SEQ_PORT_CAP_READ) &&
                (caps & SND_SEQ_PORT_CAP_SUBS_READ)) {
                int port = snd_seq_port_info_get_port(pinfo);
                if (snd_seq_connect_from(seq, my_port, client, port) < 0) {
                    fprintf(stderr, "could not subscribe to %d:%d\n",
                            client, port);
                    continue;
                }
                forward_client = client;
                forward_port = port;
                printf("forwarding from \"%s\" (%d:%d)\n", cname, client, port);
                fflush(stdout);
                return 0;
            }
        }
    }
    fprintf(stderr, "no readable port found matching \"%s\"\n", match);
    return -1;
}

int main(int argc, char **argv) {
    const char *client_name = "RMZ2_Controller";
    const char *forward_match = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--motor-off") == 0) {
            motor_off_enabled = 1;
        } else if (strcmp(argv[i], "--forward") == 0 && i + 1 < argc) {
            forward_match = argv[++i];
        } else if (argv[i][0] != '-') {
            client_name = argv[i];
        } else {
            fprintf(stderr,
                    "usage: %s [client-name] [-v] [--motor-off] "
                    "[--forward <controller-name-substring>]\n", argv[0]);
            return 1;
        }
    }

    init_id_response();

    int err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (err < 0) {
        fprintf(stderr, "snd_seq_open: %s\n", snd_strerror(err));
        return 1;
    }
    snd_seq_set_client_name(seq, client_name);

    /* Advertise both directions, and present as MIDI_GENERIC|HARDWARE rather
     * than APPLICATION: Engine's enumerator reads snd_seq_port_info_get_type()
     * and is looking for something that presents like a real control surface
     * (an APPLICATION-typed port is the conventional marker for "another
     * program", which hosts routinely skip). Bidirectional because Engine
     * opens an output side too, for LED/display feedback. */
    my_port = snd_seq_create_simple_port(
        seq, client_name,
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE);
    if (my_port < 0) {
        fprintf(stderr, "create_simple_port: %s\n", snd_strerror(my_port));
        return 1;
    }

    printf("virtual control surface \"%s\" up as client %d port %d\n",
           client_name, snd_seq_client_id(seq), my_port);
    fflush(stdout);

    if (forward_match) connect_forward_source(forward_match);

    /* Poll stdin and the sequencer together: commands can arrive at any time,
     * but so can Engine's identity request, and missing that means the
     * surface is never bound. */
    int seq_npfd = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd *pfds = calloc(seq_npfd + 1, sizeof(*pfds));
    if (!pfds) return 1;

    char line[1024];
    for (;;) {
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        snd_seq_poll_descriptors(seq, pfds + 1, seq_npfd, POLLIN);

        if (poll(pfds, seq_npfd + 1, motor_off_due_ms ? 250 : -1) < 0) break;

        if (motor_off_due_ms && now_ms() >= motor_off_due_ms) {
            motor_off_due_ms = 0;
            toggle_motor(DECK_LEFT_CH);
            toggle_motor(DECK_RIGHT_CH);
            printf("auto motor-off: toggled both decks out of motorized mode\n");
            fflush(stdout);
        }

        for (int i = 1; i <= seq_npfd; i++) {
            if (pfds[i].revents & POLLIN) {
                handle_incoming();
                break;
            }
        }

        if (!(pfds[0].revents & POLLIN)) continue;
        if (!fgets(line, sizeof(line), stdin)) {
            /* A fifo writer closing gives EOF; keep serving MIDI rather than
             * exiting, so the surface survives between command bursts. */
            clearerr(stdin);
            continue;
        }

        char cmd[64] = {0};
        int n = 0;
        if (sscanf(line, "%63s%n", cmd, &n) != 1) continue;
        char *rest = line + n;

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) break;

        if (strcmp(cmd, "ports") == 0) {
            list_ports();
        } else if (strcmp(cmd, "press") == 0) {
            int ch, note, ms = 80;
            if (sscanf(rest, "%i %i %i", &ch, &note, &ms) >= 2)
                press(ch, note, ms);
            else
                fprintf(stderr, "usage: press <ch> <note> [ms]\n");
        } else if (strcmp(cmd, "on") == 0) {
            int ch, note, vel = 0x7F;
            if (sscanf(rest, "%i %i %i", &ch, &note, &vel) >= 2)
                note_on(ch, note, vel);
        } else if (strcmp(cmd, "off") == 0) {
            int ch, note;
            if (sscanf(rest, "%i %i", &ch, &note) == 2) note_off(ch, note);
        } else if (strcmp(cmd, "cc") == 0) {
            int ch, param, val;
            if (sscanf(rest, "%i %i %i", &ch, &param, &val) == 3)
                control_change(ch, param, val);
        } else if (strcmp(cmd, "play") == 0 || strcmp(cmd, "cue") == 0 ||
                   strcmp(cmd, "load") == 0) {
            char which[32] = {0};
            sscanf(rest, "%31s", which);
            int ch = deck_channel(which);
            if (ch < 0) {
                fprintf(stderr, "usage: %s left|right\n", cmd);
            } else {
                int note = (strcmp(cmd, "play") == 0)  ? NOTE_PLAY
                           : (strcmp(cmd, "cue") == 0) ? NOTE_CUE
                           : (ch == DECK_LEFT_CH)      ? LOAD_NOTE_LEFT
                                                       : LOAD_NOTE_RIGHT;
                press(ch, note, 80);
                printf("sent %s -> ch %#04x note %#04x\n", cmd, ch, note);
                fflush(stdout);
            }
        } else if (strcmp(cmd, "motor") == 0) {
            char which[32] = {0};
            sscanf(rest, "%31s", which);
            int ch = deck_channel(which);
            if (ch < 0) {
                fprintf(stderr, "usage: motor left|right\n");
            } else {
                toggle_motor(ch);
                printf("toggled motorized-platter mode on deck ch %#04x\n", ch);
                fflush(stdout);
            }
        } else if (strcmp(cmd, "sysex") == 0) {
            unsigned char buf[256];
            size_t len = 0;
            char *tok = strtok(rest, " \t\r\n");
            while (tok && len < sizeof(buf)) {
                buf[len++] = (unsigned char)strtol(tok, NULL, 16);
                tok = strtok(NULL, " \t\r\n");
            }
            if (len) send_sysex(buf, len);
        } else {
            fprintf(stderr, "unknown command: %s\n", cmd);
        }
    }

    snd_seq_close(seq);
    return 0;
}