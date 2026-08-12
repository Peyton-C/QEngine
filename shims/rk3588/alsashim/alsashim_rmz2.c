/* Makes an emulated QEMU sound card usable as Engine's audio device: gets it
 * past Engine's compiled-in card-name allowlist, and routes Engine's PCM
 * opens through ALSA's format-converting `plug` layer.
 *
 * Two independent gates, in the order Engine hits them.
 *
 * 1. THE CARD-NAME ALLOWLIST
 *
 * ALSADeviceEnumerator::scanDevices() walks every ALSA card via
 * snd_card_next()/snd_ctl_open()/snd_ctl_card_info(), and — before it ever
 * looks at a card's PCM devices, formats or channel counts — does a plain
 * std::find() of snd_ctl_card_info_get_name()'s result in a vector<string>
 * of accepted card names. That vector is built from the "AudioDevices" key
 * of Engine's per-product config map, which is compiled into Engine.bin (no
 * file on the rootfs contains it — confirmed by grep). If the name isn't in
 * the list, the card is snd_ctl_close()d immediately and skipped; an empty
 * list means accept-everything, but this product's list is not empty.
 *
 * Under QEMU that check is the entire reason audio never played. The
 * emulated ich9-intel-hda card reports its name as "HDA Intel", which isn't
 * in RANE SYSTEM ONE's allowlist, so Engine never enumerated it, never
 * constructed an ALSADevice for it, and therefore had no audio device to
 * select — surfacing much further downstream as
 * airHost::updateAudioDeviceChanged's "Failed to fetch the audio device """
 * warning, which is what made this look like a device-*selection* bug for a
 * long time rather than an enumeration one. Confirmed directly: with
 * QT_LOGGING_RULES=air.devicemanager.*=true, hw:0 logs "Get card info for
 * hw:0" and then nothing further, while a card whose name does match goes on
 * to log "Query device 0"/"Device name hw:N".
 *
 * A previous attempt renamed the emulated card with `modprobe snd_hda_intel
 * id=RMZ2` and concluded name-matching wasn't the mechanism. That test was
 * checking the right idea against the wrong field: `id=` sets the card's
 * short *id* (the bracketed token in /proc/asound/cards), whereas
 * snd_ctl_card_info_get_name() returns the card's *shortname* — a separate
 * string the HDA driver derives from the codec and which no module parameter
 * exposes. Hence this shim: intercept the getter itself rather than trying
 * to influence what the driver puts in it.
 *
 * Spoofing the name is safe beyond the allowlist check because Engine takes
 * the *device* name it actually opens from snd_pcm_info_get_name()/"hw:%d"
 * instead, which snd_ctl_card_info_get_name() doesn't feed.
 *
 * 2. THE HARDWARE FORMAT NEGOTIATION
 *
 * Getting past the allowlist is necessary but not sufficient. Engine then
 * calls ALSADevice::ConfigureHwParams() on both directions, and on the
 * emulated ich9-intel-hda card the PLAYBACK direction configures fine
 * (44100Hz, 256-frame buffer, 128-frame periods) while CAPTURE dies at
 * snd_pcm_hw_params_set_format with EINVAL — which aborts the whole device
 * (`ALSACombinedDevice::start() Input Device does not initialize correctly`),
 * playback included, since Engine drives input and output as one combined
 * device.
 *
 * The cause is a genuine capability gap, not a bug: `aplay/arecord
 * --dump-hw-params` reports the emulated HDA card supports exactly S16_LE /
 * 2ch / 16000-96000Hz in both directions, whereas real System One hardware
 * is a 6-channel codec advertising S16+S24+S32 (see the reimplemented
 * az04-codec in shims/rk3588/az04-audio/, which does satisfy this step —
 * confirming the requested capture format is one the HDA card simply
 * doesn't offer).
 *
 * Rather than force a format behind Engine's back — which would leave it
 * writing frames in one layout while the card interprets them as another,
 * i.e. garbage — this rewrites the PCM device name Engine opens from
 * "hw:N" to "plughw:N". ALSA's `plug` plugin is purpose-built for exactly
 * this: it accepts whatever format/rate/channel count the caller asks for
 * and converts transparently to what the hardware actually supports, so
 * Engine negotiates successfully and still gets correctly-formed audio.
 * Engine builds these names itself from a hardcoded "hw:%d", so there's no
 * configuration route to the same result.
 *
 * 3. THE MIDI CLIENT'S CARD NUMBER
 *
 * Separately, Engine's MIDI device enumerator rejects any ALSA sequencer
 * client that isn't backed by a sound card: for a userspace client it logs
 * `client id: N - card number unavailable` followed by `The port isn't'
 * opened for Midi::Out::<name>` (the same warning ENGINEOS.md has recorded
 * as unresolved), and never opens the device. snd_seq_client_info_get_card()
 * returns -1 for any client created with snd_seq_open() rather than by a
 * card driver, which is what a virtual control surface
 * (shims/rk3588/midisurface_rmz2/) necessarily is.
 *
 * Engine drives MIDI entirely through the sequencer API — it imports no
 * snd_rawmidi_* symbols at all — so the card number is only ever used to
 * identify/qualify the device, never to open one. Reporting a card number
 * for card-less clients is therefore safe: it can't send Engine looking for
 * a rawmidi node that doesn't exist. Only clients that report no card at all
 * are touched, so real USB controllers keep their true card numbers.
 *
 * Env vars:
 *   ALSASHIM_AS     card name to report (default "RMZ2", System One's real
 *                   simple-audio-card name, which is what its allowlist
 *                   contains)
 *   ALSASHIM_CARD   only spoof this card index; default -1 = every card
 *   ALSASHIM_NOPLUG non-empty to leave PCM device names alone (skip the
 *                   plughw rewrite), e.g. against a card that already
 *                   advertises the needed formats natively
 *   ALSASHIM_MIDI_CARD  card number to report for card-less MIDI sequencer
 *                   clients (default 0); -1 disables this substitution
 *   ALSASHIM_DEBUG  non-empty to log each substitution to stderr
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Opaque to us — we only ever hand it back to the real libasound getters,
 * so there's no need to pull in <alsa/asoundlib.h> (and no need to link
 * against libasound: the real symbols are resolved out of the copy Engine
 * has already loaded). */
typedef struct _snd_ctl_card_info snd_ctl_card_info_t;

typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_seq_client_info snd_seq_client_info_t;

typedef const char *(*get_name_t)(const snd_ctl_card_info_t *);
typedef int (*get_card_t)(const snd_ctl_card_info_t *);
typedef int (*pcm_open_t)(snd_pcm_t **, const char *, int, int);
typedef int (*seq_get_card_t)(const snd_seq_client_info_t *);
typedef int (*seq_get_client_t)(const snd_seq_client_info_t *);

static get_name_t real_get_name = NULL;
static get_card_t real_get_card = NULL;
static pcm_open_t real_pcm_open = NULL;
static seq_get_card_t real_seq_get_card = NULL;
static seq_get_client_t real_seq_get_client = NULL;

static const char *spoof_name(void) {
    const char *v = getenv("ALSASHIM_AS");
    return (v && *v) ? v : "RMZ2";
}

/* Which card index to spoof, or -1 for all of them. Parsed per call rather
 * than cached: this is called a handful of times per device scan, not in any
 * audio hot path. */
static int spoof_card(void) {
    const char *v = getenv("ALSASHIM_CARD");
    return (v && *v) ? atoi(v) : -1;
}

static int debug_on(void) {
    const char *v = getenv("ALSASHIM_DEBUG");
    return v && *v;
}

const char *snd_ctl_card_info_get_name(const snd_ctl_card_info_t *obj) {
    if (!real_get_name)
        real_get_name = (get_name_t)dlsym(RTLD_NEXT, "snd_ctl_card_info_get_name");
    if (!real_get_name) return spoof_name();

    const char *real = real_get_name(obj);

    int want = spoof_card();
    if (want >= 0) {
        if (!real_get_card)
            real_get_card = (get_card_t)dlsym(RTLD_NEXT, "snd_ctl_card_info_get_card");
        /* If the index can't be resolved, fail closed (leave the real name
         * alone) rather than spoofing a card the caller didn't ask for. */
        if (!real_get_card || real_get_card(obj) != want) return real;
    }

    const char *as = spoof_name();
    if (debug_on())
        fprintf(stderr, "[alsashim] reporting card name \"%s\" as \"%s\"\n",
                real ? real : "(null)", as);
    return as;
}

/* Rewrite "hw:..." -> "plughw:..." so ALSA's plug layer does format/rate/
 * channel conversion on Engine's behalf. Anything not starting with "hw:"
 * (including a name already routed through a plugin) is passed through
 * untouched. */
int snd_pcm_open(snd_pcm_t **pcmp, const char *name, int stream, int mode) {
    if (!real_pcm_open)
        real_pcm_open = (pcm_open_t)dlsym(RTLD_NEXT, "snd_pcm_open");
    if (!real_pcm_open) return -1;

    if (name && strncmp(name, "hw:", 3) == 0 && !getenv("ALSASHIM_NOPLUG")) {
        char plugged[128];
        int n = snprintf(plugged, sizeof(plugged), "plug%s", name);
        if (n > 0 && (size_t)n < sizeof(plugged)) {
            if (debug_on())
                fprintf(stderr, "[alsashim] opening \"%s\" as \"%s\"\n",
                        name, plugged);
            return real_pcm_open(pcmp, plugged, stream, mode);
        }
    }
    return real_pcm_open(pcmp, name, stream, mode);
}

/* Report a card number for sequencer clients that have none, so a purely
 * virtual control surface qualifies as a real MIDI device. Clients that
 * already report a card are left untouched. */
int snd_seq_client_info_get_card(const snd_seq_client_info_t *info) {
    if (!real_seq_get_card)
        real_seq_get_card =
            (seq_get_card_t)dlsym(RTLD_NEXT, "snd_seq_client_info_get_card");
    if (!real_seq_get_card) return -1;

    int card = real_seq_get_card(info);
    if (card >= 0) return card;

    const char *v = getenv("ALSASHIM_MIDI_CARD");
    int as = (v && *v) ? atoi(v) : 0;
    if (as < 0) return card;

    if (debug_on()) {
        if (!real_seq_get_client)
            real_seq_get_client = (seq_get_client_t)dlsym(
                RTLD_NEXT, "snd_seq_client_info_get_client");
        fprintf(stderr,
                "[alsashim] seq client %d: reporting card %d (was %d)\n",
                real_seq_get_client ? real_seq_get_client(info) : -1, as, card);
    }
    return as;
}