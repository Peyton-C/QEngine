/* A virtual MIDI control surface for an emulated inMusic player.
 *
 * Transport controls (play, cue, load, sync, ...) on these units are physical
 * buttons, not touchscreen elements — Engine's on-screen UI has no way to start
 * a deck playing. Under emulation that leaves no way to trigger playback at
 * all: touch works (see touchbridge_rmz2) but only reaches the browse/deck
 * views, never the transport. This creates an ALSA sequencer client that
 * Engine's MIDI device enumerator can discover and bind to a preset assignment
 * file, so the emulated unit can be driven the way the real control surface
 * drives it.
 *
 * One binary serves every product, on both SoCs -- the RK3588 and RK3288 rootfs
 * builders compile this same source -- which is why it lives outside shims/
 * rk3588 and shims/rk3288 and why nothing here is named for a device or an
 * architecture. Only dtshim is genuinely per-SoC. Engine does NOT bind an assignment file by device name — it
 * identifies control surfaces with a MIDI Device Inquiry handshake, driven by
 * a KnownDevices table it logs at startup (air.deviceidentifier). For RMZ2:
 *
 *   <IdRequest message="7E 7F 06 01"/>
 *   <Device type="USB" realName="Controller">
 *     <property name="DeviceInquiryResponse"
 *               value="7E ?? 06 02 00 00 17 27 ?? ?? ?? ?? ?? ?? 7F"/>
 *     <property name="AssignmentFileName" value="RMZ2 Controller"/>
 *
 * So Engine broadcasts the universal identity request F0 7E 7F 06 01 F7 and
 * waits for a reply matching the pattern for some device it knows; only then
 * does it load that device's assignment file and start treating incoming notes
 * as control-surface input. A device that never answers is enumerated and
 * connected but its MIDI is ignored — which looks exactly like "the buttons do
 * nothing". This program answers the inquiry automatically, choosing which
 * device to answer as from the guest's own product code (see
 * DEVICE_IDENTITIES). The ALSA client name is cosmetic by comparison; it
 * is named after the guest's own product code (RMZ2_Controller, JP14_Controller,
 * ...) and can be overridden with argv[1].
 *
 * The transport vocabulary is per-product too, and for the same reason: Engine's
 * assignment files disagree about which channel and note mean "play". play, cue
 * and load read their numbers from DEVICE_CONTROLS below, so `play left` sends
 * the right thing on any product in that table, and single-deck players take
 * `play` with no deck at all. The numbers come from each product's own
 * <CODE>_Controller_Assignments.qml, which every rootfs ships for every product
 * under /usr/Engine/AssignmentFiles/PresetAssignmentFiles/<CODE>/ -- plain QML,
 * no reverse engineering. RMZ2's, for reference:
 *
 *   RMZ2_Controller_Assignments.qml
 *     Left deck  -> deckMidiChannel 0x04, loadNote 0x1A
 *     Right deck -> deckMidiChannel 0x05, loadNote 0x1B
 *     PlayCue    -> playNote 0x01, cueNote 0x02
 *     Sync 0x14, Shift 0x5D, Censor 0x0B, Slip 0x20, ...
 *   RMZ2_Controller_Device.qml
 *     SysEx identity: F0 00 00 17 <deviceId 7F> <productId 27> ... F7
 *     (manufacturer 00 00 17 = inMusic, product 0x27 = SYSTEM ONE)
 *
 * WHAT IS STILL NOT GENERIC: motor. It is a shift-chord rather than a note, and
 * only RMZ2 spells it Shift + Slip; the other motorized products (JP08 and JP14,
 * the SC5000M and SC6000M) do something structurally different. It refuses to
 * fire on anything but RMZ2 -- see --motor-off. The `sysex`, `on`, `off`, `cc`
 * and `press` commands take raw numbers and are unaffected by any of this.
 *
 * Commands are read from stdin, one per line, so this can be driven
 * interactively or fed from a script/fifo:
 *
 *   press <ch> <note> [ms]   note-on then note-off (a button tap; default 80ms)
 *   on    <ch> <note> [vel]  note-on
 *   off   <ch> <note>        note-off
 *   cc    <ch> <cc> <val>    control change
 *   sysex <hex bytes...>     raw sysex, e.g. sysex F0 00 00 17 7F 27 ... F7
 *   play  [deck]             convenience for the deck play button
 *   cue   [deck]             convenience for the deck cue button
 *   load  [deck]             convenience for the deck load button
 *                            <deck> is a name as the product's own assignment
 *                            file spells it (left/right, or deck on the
 *                            single-deck players) or a 1-based index, and may be
 *                            omitted where the product has only one deck.
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


/* Universal MIDI device inquiry, as Engine's KnownDevices IdRequest sends it. */
static const unsigned char ID_REQUEST[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};

/* The reply Engine's DeviceInquiryResponse pattern accepts, per device.
 *
 * Engine does not bind a control surface by name. It broadcasts a MIDI Device
 * Inquiry and matches the answer against a table it ships in
 * Content/KnownDevices.vfsb, then loads the assignment file that table names.
 * So the surface has to answer as the device the guest is pretending to be, or
 * Engine ignores it -- or worse, binds it to the wrong control map.
 *
 * Generated from that file. RMZ2 is the odd one out: inMusic's manufacturer id
 * 00 00 17 and a trailing 7F, where the RK3288 family uses 00 02 0B and the
 * Numark units 00 01 3f, both trailing 00.
 *
 * Every one of those patterns wildcards the six bytes before the trailing one
 * -- the software revision -- so the zeros below cannot affect *binding*, and
 * nothing here fills them in. Engine does compare the revision separately, to
 * decide whether the unit needs flashing, but that comparison is switched off
 * upstream of this program: both rootfs builders launch Engine with
 * -skipFirmwareUpdate, which answers "no update needed" before Engine reads
 * firmware.json at all. See the flag's note in either builder for why matching
 * the version here was abandoned.
 *
 * The visible cost is cosmetic: Settings > About reports a controller version
 * of all zeros. Nothing acts on it.
 *
 * Several devices share a reply -- JC11/JC11S, JP11/JP11S, NH08/NH08S. That is
 * Engine's business, not ours: we answer for the code the guest claims, and
 * Engine resolves the rest from its own per-product configuration. */
struct device_identity {
    const char *code;
    unsigned char response[17];
};

static const struct device_identity DEVICE_IDENTITIES[] = {
    {"JC11", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x02, 0x0B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7}},   /* 00 02 0B 08 */
    {"JC11S", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x02, 0x0B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7}},   /* 00 02 0B 08 */
    {"JC16", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x02, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7}},   /* 00 02 0B 0B */
    {"JP07", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x02, 0x0B, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7}},   /* 00 02 0B 06 */
    {"JP08", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x02, 0x0B, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7}},   /* 00 02 0B 0A */
    {"JP11", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x02, 0x0B, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7}},   /* 00 02 0B 0C */
    {"JP11S", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x02, 0x0B, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7}},   /* 00 02 0B 0C */
    {"JP13", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x02, 0x0B, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7}},   /* 00 02 0B 0D */
    {"JP14", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x02, 0x0B, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7}},   /* 00 02 0B 0E */
    {"JP20", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x02, 0x0B, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7}},   /* 00 02 0B 11  (prefix pattern) */
    {"JP21", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x02, 0x0B, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7}},   /* 00 02 0B 12  (prefix pattern) */
    {"NH08", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x01, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7}},   /* 00 01 3f 3f */
    {"NH08S", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x01, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7}},   /* 00 01 3f 3f */
    {"NH10", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x01, 0x3F, 0x59, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7}},   /* 00 01 3f 59 */
    {"RMZ2", {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x00, 0x17, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xF7}},   /* 00 00 17 27 */
};

/* Where the guest records which device it is pretending to be. dtshim serves
 * this file to Engine in place of the real devicetree node, but it is a plain
 * world-readable file and we are not preloaded, so we read it directly. */
#define PRODUCT_CODE_PATH "/root/fake-dt/inmusic,product-code"

/* Which device is this guest? Empty string if it will not say.
 *
 * MIDISURFACE_PRODUCT_CODE overrides, which is how this gets tested off a real
 * rootfs -- and how an instance whose fake-dt says one thing can be made to
 * present as another without a rebuild. */
static const char *product_code(void) {
    static char code[32];
    static int done = 0;
    if (done) return code;
    done = 1;

    const char *env = getenv("MIDISURFACE_PRODUCT_CODE");
    if (env && *env) {
        snprintf(code, sizeof(code), "%s", env);
        return code;
    }
    FILE *f = fopen(PRODUCT_CODE_PATH, "r");
    if (!f) return code;                 /* stays empty */
    if (!fgets(code, sizeof(code), f)) code[0] = '\0';
    fclose(f);
    /* The file is written with printf and carries no newline, but tolerate one
     * rather than answer an inquiry as "JP13\n". */
    for (char *c = code; *c; c++) {
        if (*c == '\n' || *c == '\r') { *c = '\0'; break; }
    }
    return code;
}

static const struct device_identity *identity_for(const char *code) {
    if (!code || !*code) return NULL;
    for (size_t i = 0; i < sizeof(DEVICE_IDENTITIES) / sizeof(DEVICE_IDENTITIES[0]); i++)
        if (strcmp(DEVICE_IDENTITIES[i].code, code) == 0)
            return &DEVICE_IDENTITIES[i];
    return NULL;
}

/* Per-device deck vocabulary.
 *
 * Scraped from each product's own <CODE>_Controller_Assignments.qml, which every
 * rootfs ships for every product under
 * /usr/Engine/AssignmentFiles/PresetAssignmentFiles/<CODE>/. Nothing here was
 * reverse engineered; it is four numbers per device read out of plain QML. Scraped
 * from version 5.0.4.
 *
 * A channel argument alone would not have done: play/cue note and deck channel
 * vary independently. JC11 and RMZ2 both put their decks on channels 4 and 5,
 * and JC11 plays with note 10 where RMZ2 plays with note 1 -- so knowing the
 * channel tells you nothing about the note, and a command that took only a
 * channel would send one product's note to another product's deck and look
 * portable while doing it.
 *
 * Two conventions run through the range. The standalone players (JP07, JP08,
 * JP13, JP14 -- SC5000/SC6000 and relatives) are single-deck, address channel 0,
 * use play 1 / cue 2, and have no load button at all: a track arrives from the
 * browser rather than from a deck control. Everything else is a two-deck
 * controller with play 10 / cue 9 and per-deck load notes 1 and 2. RMZ2 is the
 * odd one out, pairing the players' play/cue notes with two decks of its own.
 *
 * A product absent from this table can still be driven with the raw press/on/
 * off/cc commands; only the named convenience commands need to know it.*/
#define MAX_DECKS 2

struct deck_controls {
    const char *name;          /* as the assignment file's deckName spells it */
    unsigned char channel;
    int load_note;             /* -1 where the product has no load button */
};

struct device_controls {
    const char *code;
    unsigned char play_note;
    unsigned char cue_note;
    int deck_count;
    struct deck_controls decks[MAX_DECKS];
};

static const struct device_controls DEVICE_CONTROLS[] = {
    {"JC11",  10, 9, 2, {{"Left", 0x04, 0x01}, {"Right", 0x05, 0x02}}},
    {"JC11S", 10, 9, 2, {{"Left", 0x04, 0x01}, {"Right", 0x05, 0x02}}},
    {"JC16",  10, 9, 2, {{"Left", 0x02, 0x01}, {"Right", 0x03, 0x02}}},
    {"JP07",   1, 2, 1, {{"Deck", 0x00,   -1}}},
    {"JP08",   1, 2, 1, {{"Deck", 0x00,   -1}}},
    {"JP11",  10, 9, 2, {{"Left", 0x02, 0x01}, {"Right", 0x03, 0x02}}},
    {"JP11S", 10, 9, 2, {{"Left", 0x02, 0x01}, {"Right", 0x03, 0x02}}},
    {"JP13",   1, 2, 1, {{"Deck", 0x00,   -1}}},
    {"JP14",   1, 2, 1, {{"Deck", 0x00,   -1}}},
    {"JP20",  10, 9, 2, {{"Left", 0x02, 0x01}, {"Right", 0x03, 0x02}}},
    {"JP21",  10, 9, 2, {{"Left", 0x04, 0x01}, {"Right", 0x05, 0x02}}},
    {"NH08",  10, 9, 2, {{"Left", 0x02, 0x01}, {"Right", 0x03, 0x02}}},
    {"NH08S", 10, 9, 2, {{"Left", 0x02, 0x01}, {"Right", 0x03, 0x02}}},
    {"NH10",  10, 9, 2, {{"Left", 0x02, 0x01}, {"Right", 0x03, 0x02}}},
    {"RMZ2",   1, 2, 2, {{"Left", 0x04, 0x1A}, {"Right", 0x05, 0x1B}}},
};

/* Shift + Slip is Action.ToggleMotor in RMZ2_Controller_Assignments.qml, and
 * only there -- see the motor command and --motor-off, both of which refuse to
 * send these anywhere else. */
#define NOTE_SHIFT 0x5D
#define NOTE_SLIP 0x20

static const struct device_controls *controls_for(const char *code) {
    if (!code || !*code) return NULL;
    for (size_t i = 0; i < sizeof(DEVICE_CONTROLS) / sizeof(DEVICE_CONTROLS[0]); i++)
        if (strcmp(DEVICE_CONTROLS[i].code, code) == 0)
            return &DEVICE_CONTROLS[i];
    return NULL;
}

/* The controls for whichever device this guest claims to be. */
static const struct device_controls *my_controls(void) {
    return controls_for(product_code());
}

/* The ALSA client name. Cosmetic: Engine matches a control surface by its
 * inquiry reply and nothing else -- across all 74 device entries in
 * KnownDevices.vfsb there is no port- or client-name property, only
 * DeviceInquiryResponse, AssignmentFileName and USB descriptor fields. Naming
 * it after the product anyway keeps `aconnect -l` and Engine's own log lines
 * ("Midi::Out::<name>") readable, and follows Engine's own AssignmentFileName
 * spelling. An RMZ2 guest gets RMZ2_Controller, exactly the name this was
 * hardcoded to before, so nothing moves for the product it was written for. */
static const char *default_client_name(void) {
    static char name[64];
    const char *code = product_code();
    if (!*code) return "Control Surface";
    snprintf(name, sizeof(name), "%s_Controller", code);
    return name;
}

/* Resolve a deck argument against this device: a name as its assignment file
 * spells it ("left", "right", "deck"), or a 1-based index. An empty argument
 * means the only deck, which is unambiguous on the single-deck players and an
 * error anywhere else. Returns NULL and explains itself on stderr. */
static const struct deck_controls *resolve_deck(const struct device_controls *dc,
                                                const char *which) {
    if (!dc) return NULL;
    if (!which || !*which) {
        if (dc->deck_count == 1) return &dc->decks[0];
        fprintf(stderr, "%s has %d decks; name one:", dc->code, dc->deck_count);
        for (int i = 0; i < dc->deck_count; i++)
            fprintf(stderr, " %s", dc->decks[i].name);
        fprintf(stderr, "\n");
        return NULL;
    }
    for (int i = 0; i < dc->deck_count; i++) {
        char idx[2] = {(char)('1' + i), '\0'};
        if (strcasecmp(which, dc->decks[i].name) == 0 || strcmp(which, idx) == 0)
            return &dc->decks[i];
    }
    fprintf(stderr, "no deck '%s' on %s; it has:", which, dc->code);
    for (int i = 0; i < dc->deck_count; i++)
        fprintf(stderr, " %s", dc->decks[i].name);
    fprintf(stderr, "\n");
    return NULL;
}

/* Overridable via MIDISURFACE_ID_RESPONSE (space-separated hex): an escape
 * hatch for answering as a device this table does not carry, without a
 * rebuild. */
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
    const char *code = product_code();
    const struct device_identity *dev = identity_for(code);
    if (!dev) {
        /* Refusing to guess. Answering with the wrong device's identity is
         * worse than not answering: Engine would bind the surface and drive it
         * with another product's control map, which looks like a wiring fault
         * rather than a configuration one. */
        fprintf(stderr,
                "WARNING: no identity known for product code '%s' (from %s). "
                "The surface will not answer Engine's inquiry and will not be "
                "bound. Set MIDISURFACE_ID_RESPONSE to override.\n",
                *code ? code : "<unset>", PRODUCT_CODE_PATH);
        return;
    }
    memcpy(id_response, dev->response, sizeof(dev->response));
    id_response_len = sizeof(dev->response);
    printf("identifying as %s\n", dev->code);
    fflush(stdout);
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

/* Auto motor-off. RMZ2's decks wait on platter timecode that cannot
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

/* RMZ2 has motorized platters, and its deck assignment ends in
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
                /* The bytes actually sent, not a product name: this answers for
                 * fifteen devices now, and a message naming the wrong one is how
                 * a mis-set product code stays invisible. */
                if (id_response_len >= 9)
                    printf("answered device inquiry (manufacturer %02X %02X %02X,"
                           " product %02X)\n", id_response[5], id_response[6],
                           id_response[7], id_response[8]);
                else
                    printf("answered device inquiry (%zu bytes)\n",
                           id_response_len);
                fflush(stdout);
                if (motor_off_enabled) motor_off_due_ms = now_ms() + MOTOR_OFF_DEBOUNCE_MS;
            }
        }
        if (snd_seq_event_input_pending(seq, 0) <= 0) break;
    }
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
    const char *client_name = NULL;   /* argv[1], else named after the product */
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

    /* The motor toggle is RMZ2's, not a generic control. It sends Shift + Slip
     * on RMZ2's deck channels, which is Action.ToggleMotor only in
     * RMZ2_Controller_Assignments.qml -- on any other product those same notes
     * land on whatever that product's assignment file maps them to, and the
     * unit acts on it. Firing it unbidden on the wrong device is worse than not
     * firing it at all, so the guest's own product code decides, not the flag.
     *
     * Other motorized products exist (JP08 and JP14 are the SC5000M and
     * SC6000M) and presumably need the same treatment eventually, but with
     * their own note numbers read from their own assignment files. Answering
     * that is a separate job from not misfiring here. */
    if (motor_off_enabled) {
        const char *code = product_code();
        if (strcmp(code, "RMZ2") != 0) {
            fprintf(stderr,
                    "WARNING: --motor-off ignored: this guest reports product code "
                    "'%s', and the toggle it sends is RMZ2's Shift+Slip. Sending it "
                    "here would trigger whatever %s maps those notes to.\n",
                    *code ? code : "<unset>", *code ? code : "this device");
            motor_off_enabled = 0;
        }
    }

    if (!client_name) client_name = default_client_name();

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
            const struct device_controls *dc = my_controls();
            for (int i = 0; dc && i < dc->deck_count; i++)
                toggle_motor(dc->decks[i].channel);
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
            const struct device_controls *dc = my_controls();
            const struct deck_controls *dk = resolve_deck(dc, which);
            if (!dc) {
                fprintf(stderr,
                        "no deck vocabulary known for product code '%s'; use "
                        "press <ch> <note>\n", product_code());
            } else if (dk) {
                int note = (strcmp(cmd, "play") == 0)  ? dc->play_note
                           : (strcmp(cmd, "cue") == 0) ? dc->cue_note
                                                       : dk->load_note;
                if (note < 0)
                    fprintf(stderr,
                            "%s has no load button -- it loads from the browser\n",
                            dc->code);
                else {
                    press(dk->channel, (unsigned char)note, 80);
                    printf("sent %s -> %s ch 0x%02X note 0x%02X\n",
                           cmd, dk->name, dk->channel, note);
                    fflush(stdout);
                }
            }
        } else if (strcmp(cmd, "motor") == 0) {
            char which[32] = {0};
            sscanf(rest, "%31s", which);
            const struct deck_controls *dk = resolve_deck(my_controls(), which);
            if (dk) {
                int ch = dk->channel;
                const char *pc = product_code();
                if (strcmp(pc, "RMZ2") != 0)
                    fprintf(stderr,
                            "WARNING: these are RMZ2's Shift+Slip notes and this guest "
                            "is '%s'; sending anyway because you asked.\n",
                            *pc ? pc : "<unset>");
                toggle_motor(ch);
                printf("toggled motorized-platter mode on deck ch 0x%02X\n", ch);
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