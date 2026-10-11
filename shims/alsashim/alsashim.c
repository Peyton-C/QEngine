/* Makes an emulated QEMU sound card usable as Engine's audio device: gets it
 * past Engine's compiled-in card-name allowlist, routes Engine's PCM opens
 * through ALSA's format-converting `plug` layer, and gives the PCM ring
 * enough depth to survive an emulated card's service interval.
 *
 * Four independent gates, in the order Engine hits them. The first three are
 * what stand between silence and audio at all; the fourth is what stands
 * between audio and *clean* audio.
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
 * (shims/midisurface/) necessarily is.
 *
 * Engine drives MIDI entirely through the sequencer API — it imports no
 * snd_rawmidi_* symbols at all — so the card number is only ever used to
 * identify/qualify the device, never to open one. Reporting a card number
 * for card-less clients is therefore safe: it can't send Engine looking for
 * a rawmidi node that doesn't exist. Only clients that report no card at all
 * are touched, so real USB controllers keep their true card numbers.
 *
 * 4. THE RING DEPTH (glitchy/distorted playback under emulation)
 *
 * Engine configures playback for a 256-frame buffer of 128-frame periods at
 * 44100Hz: 5.8ms of ring, refilled every 2.9ms. That's a reasonable ask of a
 * real codec on a real DMA engine, and hopeless against an emulated one.
 * QEMU's audio subsystem moves samples on a timer whose default period is
 * 10ms (`-audiodev ...,timer-period`) — longer than the guest's *entire* ring
 * buffer — so the emulated HDA controller repeatedly drains past what Engine
 * has written and re-reads stale ring content. That is heard as continuous
 * glitching/distortion rather than as clean dropouts, and shows up in the
 * guest as a steady stream of playback XRUNs.
 *
 * Neither side is misbehaving; the buffer is just sized for hardware that
 * isn't there. So this scales the ring's *depth* without touching its
 * *granularity*: buffer_size, buffer_time and the period *count* are
 * multiplied by ALSASHIM_BUFFER_SCALE (default 8 — 2048 frames, 46ms, in 16
 * periods), while period_size and period_time are passed through untouched.
 * Engine's audio callback keeps firing every 128 frames exactly as it does
 * now; only the amount of audio queued ahead of the emulated card changes.
 *
 * Scaling that particular set keeps every hw constraint mutually consistent
 * (buffer = periods x period_size, buffer_time = periods x period_time), so
 * it doesn't matter which subset of them Engine actually pins. That
 * consistency is the whole correctness argument here, and it's why the
 * multiplier is applied verbatim with no per-quantity latency ceiling: a
 * ceiling binds at a different ratio for each quantity (clamping frames at
 * one factor while the period count moves by another), which silently breaks
 * the invariant and gets the *combination* rejected even though each value
 * looked reasonable on its own. Where the ring and period size are already
 * pinned on the params object, the period count is derived from them as
 * buffer/period rather than multiplied, which keeps it exact regardless of
 * what the card granted.
 *
 * Degradation against a card with less headroom than the scale asks for
 * differs by setter, and neither case ends worse than today:
 *
 *   - the `_near` setters (what Engine appears to use) clamp, so the ring
 *     simply grows as far as the card allows — a 1024-frame ceiling yields
 *     1024/8 periods, still at a 128-frame period size.
 *   - the exact setters can't clamp, so they retry with Engine's original
 *     value and the configuration comes out byte-identical to an unscaled
 *     one. It's all-or-nothing at the requested factor rather than a backoff
 *     search, because a partial backoff is what breaks the invariant above.
 *
 * Two software params have to follow the hardware one, or the deeper ring
 * buys nothing:
 *
 *   - stop_threshold is conventionally the buffer size, and means "declare an
 *     XRUN once this much of the ring is empty". Left at Engine's 256 against
 *     a 2048-frame ring it would fire almost continuously — strictly worse
 *     than not scaling at all — so it's raised to the real buffer size.
 *   - start_threshold decides how full the ring is at the moment the stream
 *     starts. A period-driven write loop (write one period per period
 *     interrupt) then holds the ring at roughly whatever depth it started
 *     with, so a low threshold would hand back all the headroom. It's raised
 *     to the real buffer size for playback — ALSA's own default — so the
 *     stream starts full. Capture is left alone: there the threshold gates
 *     the first read rather than a fill level.
 *
 * Both are raised toward the buffer size actually granted by the hardware
 * (read back with snd_pcm_hw_params_current), never past it, and never
 * lowered below what Engine asked for — so they stay correct even on the
 * fallback path, where the ring didn't grow at all.
 *
 * Env vars:
 *   ALSASHIM_AS     card name to report (default "RMZ2", System One's real
 *                   simple-audio-card name, which is what its allowlist
 *                   contains). RK3288 products need this set: there the name
 *                   is the ASoC card name their vendor machine driver
 *                   registers ("JP07", "JP11", ...), and it is not always the
 *                   product code -- JP08 shares JP07's devicetree compatible
 *                   and so comes up as "JP07". scripts/build_scripts/
 *                   detect_audio_card.sh works it out per firmware.
 *   ALSASHIM_CARD   only spoof this card index; default -1 = every card
 *   ALSASHIM_NOPLUG non-empty to leave PCM device names alone (skip the
 *                   plughw rewrite), e.g. against a card that already
 *                   advertises the needed formats natively
 *   ALSASHIM_BUFFER_SCALE  multiplier for the PCM ring depth (default 8);
 *                   1 disables the resizing and the sw_params follow-up
 *                   entirely, leaving Engine's own buffering untouched
 *   ALSASHIM_XRUN_FREE_RUN  non-empty (and not "0") to set playback's
 *                   stop_threshold to the ring boundary, so an underrun leaves
 *                   the stream running instead of stopping it. Engine never
 *                   recovers from an XRUN -- every later write returns -EPIPE
 *                   and it calls no snd_pcm_prepare, so under emulation one
 *                   stall silences audio permanently. Off by default; it turns
 *                   a permanent silence into an audible gap, which is only the
 *                   better trade where stalls are expected.
 *   ALSASHIM_MIDI_CARD  card number to report for card-less MIDI sequencer
 *                   clients (default 0); -1 disables this substitution
 *   ALSASHIM_DEBUG  non-empty to log each substitution to stderr
 *
 * RK3288 output layout. The vendor codec is S32_LE / 4 channels / 44100, but
 * what the two stereo pairs *mean* differs by product:
 *   Prime GO   ch 0,1 = cue (headphone), ch 2,3 = master
 *   SC6000     ch 0,1 = Layer A, ch 2,3 = Layer B -- two independent RCA pairs,
 *              because the deck can play two layers at once
 * A card that cannot carry four channels (QEMU's usb-audio, or virtio-sound
 * with one stream) still gets Engine's native layout, bridged in userspace:
 *   ALSASHIM_CHANNELS         channels advertised to Engine (default 4)
 *   ALSASHIM_HW_CHANNELS      channels on the real device (default 2)
 *   ALSASHIM_MASTER_CHANNELS  source indices of the master pair (default 2,3);
 *                             this is what Prime GO needs on a stereo card
 *   ALSASHIM_LAYER_MIX        non-empty sums both pairs (ch 0,1 + 2,3), for
 *                             SC6000, where either layer may carry the track
 *   ALSASHIM_ALLOW_32BIT      leave the real format mask alone (Engine uses S32)
 *   ALSASHIM_RATE             pin the advertised rate (44100); without it Engine
 *                             can take its 96 kHz upsampling path
 *   ALSASHIM_ENUM_CAPTURE     present the card as duplex
 *   ALSASHIM_FAKE_CAPTURE     open capture on the "null" PCM (reads silence)
 *   ALSASHIM_PLAYBACK_DEV / ALSASHIM_CAPTURE_DEV  pick the card's PCM device
 *                             per direction, for SoC cards that are not device 0
 */
#define _GNU_SOURCE
#include <time.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Opaque to us — we only ever hand it back to the real libasound getters,
 * so there's no need to pull in <alsa/asoundlib.h> (and no need to link
 * against libasound: the real symbols are resolved out of the copy Engine
 * has already loaded). */
typedef struct _snd_ctl_card_info snd_ctl_card_info_t;
typedef struct _snd_ctl snd_ctl_t;
typedef struct _snd_pcm_info snd_pcm_info_t;

typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef struct _snd_pcm_sw_params snd_pcm_sw_params_t;
typedef struct _snd_seq_client_info snd_seq_client_info_t;

/* Matches alsa-lib's own definition; frame counts are unsigned long there. */
typedef unsigned long snd_pcm_uframes_t;

/* SND_PCM_STREAM_PLAYBACK, as returned by snd_pcm_stream(). */
#define ALSASHIM_STREAM_PLAYBACK 0

typedef const char *(*get_name_t)(const snd_ctl_card_info_t *);
typedef const char *(*get_id_t)(const snd_ctl_card_info_t *);
typedef int (*get_card_t)(const snd_ctl_card_info_t *);
typedef int (*pcm_open_t)(snd_pcm_t **, const char *, int, int);
typedef int (*ctl_pcm_info_t)(snd_ctl_t *, snd_pcm_info_t *);
typedef unsigned int (*pcm_info_get_device_t)(const snd_pcm_info_t *);
typedef ssize_t (*pcm_readi_t)(snd_pcm_t *, void *, snd_pcm_uframes_t);
typedef int (*seq_get_card_t)(const snd_seq_client_info_t *);
typedef int (*seq_get_client_t)(const snd_seq_client_info_t *);

typedef int (*pcm_stream_t)(snd_pcm_t *);
typedef int (*hw_frames_t)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t);
typedef int (*hw_frames_p_t)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t *);
typedef int (*hw_uint_t)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int, int);
typedef int (*hw_uint_p_t)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *, int *);
typedef int (*hw_malloc_t)(snd_pcm_hw_params_t **);
typedef void (*hw_free_t)(snd_pcm_hw_params_t *);
typedef int (*hw_current_t)(snd_pcm_t *, snd_pcm_hw_params_t *);
typedef int (*hw_get_frames_t)(const snd_pcm_hw_params_t *, snd_pcm_uframes_t *);
typedef int (*hw_get_frames_d_t)(const snd_pcm_hw_params_t *,
                                 snd_pcm_uframes_t *, int *);
typedef int (*sw_frames_t)(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t);
typedef int (*sw_get_frames_t)(const snd_pcm_sw_params_t *, snd_pcm_uframes_t *);

static get_name_t real_get_name = NULL;
static get_id_t real_get_id = NULL;
static get_card_t real_get_card = NULL;
static pcm_open_t real_pcm_open = NULL;
static ctl_pcm_info_t real_ctl_pcm_info = NULL;
static pcm_info_get_device_t real_pcm_info_get_device = NULL;
static pcm_readi_t real_pcm_readi = NULL;
static seq_get_card_t real_seq_get_card = NULL;
static seq_get_client_t real_seq_get_client = NULL;

static pcm_stream_t real_pcm_stream = NULL;
static hw_frames_t real_set_bufsz = NULL;
static hw_frames_p_t real_set_bufsz_near = NULL;
static hw_frames_p_t real_set_bufsz_min = NULL;
static hw_frames_p_t real_set_bufsz_max = NULL;
static hw_uint_t real_set_buftime = NULL;
static hw_uint_p_t real_set_buftime_near = NULL;
static hw_uint_t real_set_periods = NULL;
static hw_uint_p_t real_set_periods_near = NULL;
static hw_malloc_t real_hw_malloc = NULL;
static hw_free_t real_hw_free = NULL;
static hw_current_t real_hw_current = NULL;
static hw_get_frames_t real_hw_get_bufsz = NULL;
static hw_get_frames_t real_get_bufsz_min = NULL;
static hw_get_frames_t real_get_bufsz_max = NULL;
static hw_get_frames_d_t real_get_persz_min = NULL;
static hw_get_frames_d_t real_get_persz_max = NULL;
static sw_frames_t real_set_start = NULL;
static sw_frames_t real_set_stop = NULL;
static sw_get_frames_t real_get_boundary = NULL;

/* Lazy resolution, one dlsym per symbol. Racing threads compute the same
 * pointer, so the unsynchronised write is benign. */
#define RESOLVE(var, sym) \
    do { \
        if (!(var)) (var) = (__typeof__(var))dlsym(RTLD_NEXT, sym); \
    } while (0)

static const char *spoof_name(void) {
    const char *v = getenv("ALSASHIM_AS");
    return (v && *v) ? v : "RMZ2";
}

/* The card *id* is a separate getter from the name. Denon's own build reports
 * the same string for both, but some ALSA enumerators match the id, so spoof
 * it too (ALSASHIM_ID overrides, else the same string as the name). */
static const char *spoof_id(void) {
    const char *v = getenv("ALSASHIM_ID");
    return (v && *v) ? v : spoof_name();
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

/* Whether to make playback free-running rather than let an underrun stop the
 * stream. Independent of ALSASHIM_BUFFER_SCALE on purpose: the case it exists
 * for is a *shallow* ring that would otherwise die on its first stall. */
static int free_run_on(void) {
    const char *v = getenv("ALSASHIM_XRUN_FREE_RUN");
    return v && *v && *v != '0';
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

typedef const char *(*get_driver_t)(const snd_ctl_card_info_t *);
static get_driver_t real_get_driver = NULL;

/* ALSADeviceEnumerator also reads the card *driver* (snd_ctl_card_info_get_driver),
 * which on this board is the ASoC driver "mt8183-sound". Spoof it too. */
const char *snd_ctl_card_info_get_driver(const snd_ctl_card_info_t *obj) {
    if (!real_get_driver)
        real_get_driver = (get_driver_t)dlsym(RTLD_NEXT, "snd_ctl_card_info_get_driver");
    if (!real_get_driver) return spoof_name();
    const char *real = real_get_driver(obj);
    if (debug_on())
        fprintf(stderr, "[alsashim] get_driver \"%s\"\n", real ? real : "(null)");
    const char *v = getenv("ALSASHIM_DRIVER");
    return (v && *v) ? v : real;
}

const char *snd_ctl_card_info_get_id(const snd_ctl_card_info_t *obj) {
    if (!real_get_id)
        real_get_id = (get_id_t)dlsym(RTLD_NEXT, "snd_ctl_card_info_get_id");
    if (!real_get_id) return spoof_id();

    const char *real = real_get_id(obj);

    int want = spoof_card();
    if (want >= 0) {
        if (!real_get_card)
            real_get_card = (get_card_t)dlsym(RTLD_NEXT, "snd_ctl_card_info_get_card");
        if (!real_get_card || real_get_card(obj) != want) return real;
    }

    const char *as = spoof_id();
    if (debug_on())
        fprintf(stderr, "[alsashim] reporting card id \"%s\" as \"%s\"\n",
                real ? real : "(null)", as);
    return as;
}

/* Rewrite "hw:..." -> "plughw:..." so ALSA's plug layer does format/rate/
 * channel conversion on Engine's behalf. Anything not starting with "hw:"
 * (including a name already routed through a plugin) is passed through
 * untouched.
 *
 * Engine drives one card as a *combined* playback+capture device opened as
 * "hw:N" (device 0). Real inmusic codecs expose both directions on device 0;
 * the tablet's SoC card does not — playback is PCM device 0, capture device 3
 * — so a bare "plughw:N" gives ENOENT on capture and EBUSY on the second
 * playback open. Pick the device per direction (ALSASHIM_PLAYBACK_DEV /
 * ALSASHIM_CAPTURE_DEV override the defaults). */
static int stream_device(int stream) {
    const char *v = getenv(stream == ALSASHIM_STREAM_PLAYBACK
                           ? "ALSASHIM_PLAYBACK_DEV" : "ALSASHIM_CAPTURE_DEV");
    if (v && *v) return atoi(v);
    return stream == ALSASHIM_STREAM_PLAYBACK ? 0 : 3;
}

/* Engine drives one card as a *combined* playback+capture device. This board's
 * capture PCM cannot be opened ("no backend DAIs enabled for Capture_1"), so
 * with ALSASHIM_FAKE_CAPTURE the capture direction is opened as a second, real
 * *playback* PCM (device ALSASHIM_FAKE_CAPTURE_DEV, default 1) and reads on it
 * return silence. Engine only needs the handle to exist and configure. */
#define ALSASHIM_MAX_FAKE_CAPTURE 8
static snd_pcm_t *fake_capture[ALSASHIM_MAX_FAKE_CAPTURE];
static int fake_capture_n = 0;

static int is_fake_capture(snd_pcm_t *p) {
    for (int i = 0; i < fake_capture_n; i++)
        if (fake_capture[i] == p) return 1;
    return 0;
}

int snd_pcm_open(snd_pcm_t **pcmp, const char *name, int stream, int mode) {
    if (!real_pcm_open)
        real_pcm_open = (pcm_open_t)dlsym(RTLD_NEXT, "snd_pcm_open");
    if (!real_pcm_open) return -1;

    if (name && strncmp(name, "hw:", 3) == 0 && !getenv("ALSASHIM_NOPLUG")) {
        int card = 0, dev = -1;
        if (sscanf(name, "hw:%d,%d", &card, &dev) < 1)
            return real_pcm_open(pcmp, name, stream, mode);
        int open_stream = stream;
        const char *literal = NULL; /* PCM name used verbatim (fake capture) */
        if (stream == 1 /* CAPTURE */ && getenv("ALSASHIM_FAKE_CAPTURE")) {
            const char *cd = getenv("ALSASHIM_FAKE_CAPTURE_DEV");
            if (!cd || !*cd) cd = "null";
            if (cd[0] < '0' || cd[0] > '9') {
                /* Non-numeric: a PCM name that really supports capture on this
                 * board ("null" returns silence). Devices 1/2 exist but their
                 * playback open fails with EINVAL, which is what made Engine
                 * report "Could not open device hw:N ... Invalid argument". */
                literal = cd;
                open_stream = stream;
            } else {
                dev = atoi(cd);
                open_stream = ALSASHIM_STREAM_PLAYBACK;
            }
        } else if (dev < 0) {
            dev = stream_device(stream);
        }
        char plugged[128];
        int n = literal ? snprintf(plugged, sizeof(plugged), "%s", literal)
                        : snprintf(plugged, sizeof(plugged), "plughw:%d,%d", card, dev);
        if (n > 0 && (size_t)n < sizeof(plugged)) {
            if (debug_on())
                fprintf(stderr, "[alsashim] opening \"%s\" (stream %d) as \"%s\" (open stream %d)\n",
                        name, stream, plugged, open_stream);
            int rc = real_pcm_open(pcmp, plugged, open_stream, mode);
            if (rc == 0 && *pcmp && stream == 1 &&
                getenv("ALSASHIM_FAKE_CAPTURE") &&
                fake_capture_n < ALSASHIM_MAX_FAKE_CAPTURE)
                fake_capture[fake_capture_n++] = *pcmp;
            return rc;
        }
    }
    return real_pcm_open(pcmp, name, stream, mode);
}

/* Report only PCM device 0 of each card. Engine otherwise opens the same
 * "hw:N" once per real PCM device and the exclusive hw PCM returns EBUSY. */
int snd_ctl_pcm_info(snd_ctl_t *ctl, snd_pcm_info_t *info) {
    if (!real_ctl_pcm_info)
        real_ctl_pcm_info = (ctl_pcm_info_t)dlsym(RTLD_NEXT, "snd_ctl_pcm_info");
    if (!real_pcm_info_get_device)
        real_pcm_info_get_device = (pcm_info_get_device_t)dlsym(RTLD_NEXT, "snd_pcm_info_get_device");
    if (!real_ctl_pcm_info) return -1;
    if (real_pcm_info_get_device && real_pcm_info_get_device(info) != 0) return -2;
    /* Only ever report the real answer. Faking success for the capture stream
     * here makes Engine enumerate a bogus capture device for hw:0, which then
     * wins the default-device pick (Playback: false) and leaves the combined
     * device's playback slot null -- the "frozen Audio_probe" symptom. Capture
     * is faked at snd_pcm_open() time instead, which Engine reaches anyway
     * while opening the combined device. */
    int rc = real_ctl_pcm_info(ctl, info);
    if (rc < 0 && getenv("ALSASHIM_ENUM_CAPTURE")) {
        /* ALSACombinedDevice wants a duplex card.  Refill the info from the
         * playback direction (device 0 really has one) and then flip its
         * stream field to capture, so the entry is fully formed rather than
         * garbage.  Combined with ALSASHIM_FAKE_CAPTURE the open also works. */
        typedef unsigned int (*info_get_stream_t)(const snd_pcm_info_t *);
        typedef void (*info_set_stream_t)(snd_pcm_info_t *, int);
        static info_get_stream_t real_info_get_stream = NULL;
        static info_set_stream_t real_info_set_stream = NULL;
        if (!real_info_get_stream)
            real_info_get_stream = (info_get_stream_t)dlsym(RTLD_NEXT, "snd_pcm_info_get_stream");
        if (!real_info_set_stream)
            real_info_set_stream = (info_set_stream_t)dlsym(RTLD_NEXT, "snd_pcm_info_set_stream");
        if (real_info_get_stream && real_info_set_stream &&
            real_info_get_stream(info) == 1) {
            real_info_set_stream(info, 0);
            rc = real_ctl_pcm_info(ctl, info);
            real_info_set_stream(info, 1);
            if (rc == 0) return 0;
        }
    }
    return rc;
}

/* Reads on the faked capture handle return silence, paced to the stream rate.
 * The `null` PCM returns instantly, so without the sleep Engine's full-duplex
 * thread spins at 100% CPU and starves the UI thread (observed as a frozen
 * panel and load average > 10). */
static unsigned int alsashim_target_rate(void);
ssize_t snd_pcm_readi(snd_pcm_t *pcm, void *buf, snd_pcm_uframes_t count) {
    if (is_fake_capture(pcm)) {
        unsigned int rate = alsashim_target_rate();
        if (rate && count) {
            long long ns = (long long)count * 1000000000LL / (long long)rate;
            struct timespec ts;
            ts.tv_sec = (time_t)(ns / 1000000000LL);
            ts.tv_nsec = (long)(ns % 1000000000LL);
            nanosleep(&ts, NULL);
        }
        if (buf) memset(buf, 0, (size_t)count * 8);
        return (ssize_t)count;
    }
    if (!real_pcm_readi)
        real_pcm_readi = (pcm_readi_t)dlsym(RTLD_NEXT, "snd_pcm_readi");
    return real_pcm_readi ? real_pcm_readi(pcm, buf, count) : -1;
}

/* --- PCM ring depth --------------------------------------------------- */

static unsigned int buffer_scale(void) {
    const char *v = getenv("ALSASHIM_BUFFER_SCALE");
    long n = (v && *v) ? strtol(v, NULL, 10) : 8;
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    return (unsigned int)n;
}

/* Apply the multiplier verbatim — every scaled quantity has to move by the
 * *same* factor or `buffer = periods x period_size` stops holding and the
 * card rejects the combination. `type_max` is the maximum of the argument's
 * own type, and only guards the multiplication against overflow; it is not a
 * latency ceiling, since a ceiling would bind at different ratios for
 * different quantities and reintroduce exactly that inconsistency. */
static unsigned long scale_val(unsigned long v, unsigned long type_max) {
    unsigned int n = buffer_scale();
    if (n <= 1 || v == 0 || v > type_max / n) return v;
    return v * n;
}

/* When the ring size and the period size are both already pinned on this
 * params object, the only period count that can satisfy them is their ratio.
 * Deriving it rather than multiplying keeps `periods` consistent with the
 * buffer the card actually granted — including the case where the scaled
 * buffer was refused and Engine's original was used instead. */
static int pinned_periods(snd_pcm_hw_params_t *p, unsigned int *out) {
    RESOLVE(real_get_bufsz_min, "snd_pcm_hw_params_get_buffer_size_min");
    RESOLVE(real_get_bufsz_max, "snd_pcm_hw_params_get_buffer_size_max");
    RESOLVE(real_get_persz_min, "snd_pcm_hw_params_get_period_size_min");
    RESOLVE(real_get_persz_max, "snd_pcm_hw_params_get_period_size_max");
    if (!real_get_bufsz_min || !real_get_bufsz_max || !real_get_persz_min ||
        !real_get_persz_max)
        return -ENOSYS;

    snd_pcm_uframes_t bmin, bmax, pmin, pmax;
    if (real_get_bufsz_min(p, &bmin) < 0 || real_get_bufsz_max(p, &bmax) < 0)
        return -EINVAL;
    if (bmin != bmax) return -EINVAL; /* ring not pinned yet */
    if (real_get_persz_min(p, &pmin, NULL) < 0 ||
        real_get_persz_max(p, &pmax, NULL) < 0)
        return -EINVAL;
    if (pmin != pmax || pmin == 0) return -EINVAL;

    *out = (unsigned int)(bmin / pmin);
    return 0;
}

/* The period count that keeps this params object consistent: derived from the
 * pinned ring when there is one, otherwise Engine's own count scaled. */
static unsigned int target_periods(snd_pcm_hw_params_t *p, unsigned int val) {
    unsigned int derived;
    if (buffer_scale() > 1 && pinned_periods(p, &derived) == 0) return derived;
    return (unsigned int)scale_val(val, UINT_MAX);
}

static int stream_of(snd_pcm_t *pcm) {
    RESOLVE(real_pcm_stream, "snd_pcm_stream");
    return real_pcm_stream ? real_pcm_stream(pcm) : -1;
}

static void log_scaled(snd_pcm_t *pcm, const char *what,
                       unsigned long from, unsigned long to) {
    if (!debug_on()) return;
    int dir = stream_of(pcm);
    fprintf(stderr, "[alsashim] %s %s %lu -> %lu\n",
            dir == ALSASHIM_STREAM_PLAYBACK ? "playback"
                                            : (dir < 0 ? "pcm" : "capture"),
            what, from, to);
}

/* The buffer size the hardware actually granted, i.e. after our scaling and
 * after ALSA's own rounding — the only figure the sw_params below should be
 * measured against. */
static int granted_buffer_size(snd_pcm_t *pcm, snd_pcm_uframes_t *out) {
    RESOLVE(real_hw_malloc, "snd_pcm_hw_params_malloc");
    RESOLVE(real_hw_free, "snd_pcm_hw_params_free");
    RESOLVE(real_hw_current, "snd_pcm_hw_params_current");
    RESOLVE(real_hw_get_bufsz, "snd_pcm_hw_params_get_buffer_size");
    if (!real_hw_malloc || !real_hw_free || !real_hw_current ||
        !real_hw_get_bufsz)
        return -ENOSYS;

    snd_pcm_hw_params_t *params = NULL;
    if (real_hw_malloc(&params) < 0 || !params) return -ENOMEM;

    int rc = real_hw_current(pcm, params);
    if (rc == 0) rc = real_hw_get_bufsz(params, out);
    real_hw_free(params);
    return rc;
}

int snd_pcm_hw_params_set_buffer_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                      snd_pcm_uframes_t val) {
    RESOLVE(real_set_bufsz, "snd_pcm_hw_params_set_buffer_size");
    if (!real_set_bufsz) return -ENOSYS;

    snd_pcm_uframes_t want = scale_val(val, ULONG_MAX);
    if (want != val && real_set_bufsz(pcm, p, want) == 0) {
        log_scaled(pcm, "buffer_size", val, want);
        return 0;
    }
    return real_set_bufsz(pcm, p, val);
}

int snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *pcm,
                                           snd_pcm_hw_params_t *p,
                                           snd_pcm_uframes_t *val) {
    RESOLVE(real_set_bufsz_near, "snd_pcm_hw_params_set_buffer_size_near");
    if (!real_set_bufsz_near) return -ENOSYS;
    if (!val) return real_set_bufsz_near(pcm, p, val);

    snd_pcm_uframes_t orig = *val;
    snd_pcm_uframes_t want = scale_val(orig, ULONG_MAX);
    if (want != orig) {
        *val = want;
        if (real_set_bufsz_near(pcm, p, val) == 0) {
            log_scaled(pcm, "buffer_size", orig, *val);
            return 0;
        }
        *val = orig;
    }
    return real_set_bufsz_near(pcm, p, val);
}

/* A minimum is a floor on the ring, a maximum a ceiling on it; raising either
 * moves it the way we want, and a rejected floor falls back to Engine's. */
int snd_pcm_hw_params_set_buffer_size_min(snd_pcm_t *pcm,
                                          snd_pcm_hw_params_t *p,
                                          snd_pcm_uframes_t *val) {
    RESOLVE(real_set_bufsz_min, "snd_pcm_hw_params_set_buffer_size_min");
    if (!real_set_bufsz_min) return -ENOSYS;
    if (!val) return real_set_bufsz_min(pcm, p, val);

    snd_pcm_uframes_t orig = *val;
    snd_pcm_uframes_t want = scale_val(orig, ULONG_MAX);
    if (want != orig) {
        *val = want;
        if (real_set_bufsz_min(pcm, p, val) == 0) {
            log_scaled(pcm, "buffer_size_min", orig, *val);
            return 0;
        }
        *val = orig;
    }
    return real_set_bufsz_min(pcm, p, val);
}

int snd_pcm_hw_params_set_buffer_size_max(snd_pcm_t *pcm,
                                          snd_pcm_hw_params_t *p,
                                          snd_pcm_uframes_t *val) {
    RESOLVE(real_set_bufsz_max, "snd_pcm_hw_params_set_buffer_size_max");
    if (!real_set_bufsz_max) return -ENOSYS;
    if (!val) return real_set_bufsz_max(pcm, p, val);

    snd_pcm_uframes_t orig = *val;
    snd_pcm_uframes_t want = scale_val(orig, ULONG_MAX);
    if (want != orig) {
        *val = want;
        if (real_set_bufsz_max(pcm, p, val) == 0) {
            log_scaled(pcm, "buffer_size_max", orig, *val);
            return 0;
        }
        *val = orig;
    }
    return real_set_bufsz_max(pcm, p, val);
}

int snd_pcm_hw_params_set_buffer_time(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                      unsigned int val, int dir) {
    RESOLVE(real_set_buftime, "snd_pcm_hw_params_set_buffer_time");
    if (!real_set_buftime) return -ENOSYS;

    unsigned int want = (unsigned int)scale_val(val, UINT_MAX);
    if (want != val && real_set_buftime(pcm, p, want, dir) == 0) {
        log_scaled(pcm, "buffer_time", val, want);
        return 0;
    }
    return real_set_buftime(pcm, p, val, dir);
}

int snd_pcm_hw_params_set_buffer_time_near(snd_pcm_t *pcm,
                                           snd_pcm_hw_params_t *p,
                                           unsigned int *val, int *dir) {
    RESOLVE(real_set_buftime_near, "snd_pcm_hw_params_set_buffer_time_near");
    if (!real_set_buftime_near) return -ENOSYS;
    if (!val) return real_set_buftime_near(pcm, p, val, dir);

    unsigned int orig = *val;
    unsigned int want = (unsigned int)scale_val(orig, UINT_MAX);
    if (want != orig) {
        *val = want;
        if (real_set_buftime_near(pcm, p, val, dir) == 0) {
            log_scaled(pcm, "buffer_time", orig, *val);
            return 0;
        }
        *val = orig;
    }
    return real_set_buftime_near(pcm, p, val, dir);
}

/* Scaling the period *count* alongside the buffer is what keeps the period
 * *size* — Engine's callback granularity — where Engine put it. */
int snd_pcm_hw_params_set_periods(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                  unsigned int val, int dir) {
    RESOLVE(real_set_periods, "snd_pcm_hw_params_set_periods");
    if (!real_set_periods) return -ENOSYS;

    unsigned int want = target_periods(p, val);
    if (want != val && real_set_periods(pcm, p, want, dir) == 0) {
        log_scaled(pcm, "periods", val, want);
        return 0;
    }
    return real_set_periods(pcm, p, val, dir);
}

int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                       unsigned int *val, int *dir) {
    RESOLVE(real_set_periods_near, "snd_pcm_hw_params_set_periods_near");
    if (!real_set_periods_near) return -ENOSYS;
    if (!val) return real_set_periods_near(pcm, p, val, dir);

    unsigned int orig = *val;
    unsigned int want = target_periods(p, orig);
    if (want != orig) {
        *val = want;
        if (real_set_periods_near(pcm, p, val, dir) == 0) {
            log_scaled(pcm, "periods", orig, *val);
            return 0;
        }
        *val = orig;
    }
    return real_set_periods_near(pcm, p, val, dir);
}

/* Start playback with the ring full rather than one period deep, so the extra
 * depth is actually carried. Capture is left alone: there start_threshold
 * gates the first read, not a fill level. */
int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm,
                                          snd_pcm_sw_params_t *p,
                                          snd_pcm_uframes_t val) {
    RESOLVE(real_set_start, "snd_pcm_sw_params_set_start_threshold");
    if (!real_set_start) return -ENOSYS;

    snd_pcm_uframes_t bufsz;
    if (buffer_scale() > 1 && stream_of(pcm) == ALSASHIM_STREAM_PLAYBACK &&
        granted_buffer_size(pcm, &bufsz) == 0 && bufsz > val &&
        real_set_start(pcm, p, bufsz) == 0) {
        log_scaled(pcm, "start_threshold", val, bufsz);
        return 0;
    }
    return real_set_start(pcm, p, val);
}

/* Engine sizes stop_threshold for the ring it asked for. Against the deeper
 * one it would report an XRUN while most of the ring was still legitimately
 * unwritten, so it has to grow with the buffer. */
int snd_pcm_sw_params_set_stop_threshold(snd_pcm_t *pcm,
                                         snd_pcm_sw_params_t *p,
                                         snd_pcm_uframes_t val) {
    RESOLVE(real_set_stop, "snd_pcm_sw_params_set_stop_threshold");
    if (!real_set_stop) return -ENOSYS;

    /* Free-running playback (from upstream's universal-audio branch):
     * stop_threshold at the ring's boundary -- a value the available count can
     * never reach -- is ALSA's own idiom for "never stop this stream on an
     * underrun". Engine does not recover from an XRUN: every later
     * snd_pcm_writei returns -EPIPE and it calls no snd_pcm_prepare, so under
     * emulation one stall is silence forever. Checked before the depth-derived
     * value below so this holds at any ALSASHIM_BUFFER_SCALE, scale 1 included. */
    if (free_run_on()) {
        RESOLVE(real_get_boundary, "snd_pcm_sw_params_get_boundary");
        snd_pcm_uframes_t boundary;
        if (real_get_boundary && real_get_boundary(p, &boundary) == 0 &&
            boundary > val && real_set_stop(pcm, p, boundary) == 0) {
            log_scaled(pcm, "stop_threshold (free-run)", val, boundary);
            return 0;
        }
        if (debug_on())
            fprintf(stderr, "[alsashim] free-run stop_threshold refused; "
                            "leaving it at %lu\n", (unsigned long)val);
    }

    snd_pcm_uframes_t bufsz;
    if (buffer_scale() > 1 && granted_buffer_size(pcm, &bufsz) == 0 &&
        bufsz > val && real_set_stop(pcm, p, bufsz) == 0) {
        log_scaled(pcm, "stop_threshold", val, bufsz);
        return 0;
    }
    return real_set_stop(pcm, p, val);
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

/* --- Format restriction ---------------------------------------------------
 * Engine enumerates the formats a PCM advertises and selects the highest
 * quality one.  Through ALSA's `plug` layer that is S32_LE, but plug's rate
 * converter cannot produce 44100 Hz into a 32-bit format (snd_pcm_hw_params
 * returns EINVAL), which is exactly what happens against QEMU's usb-audio
 * card: natively 48000 Hz only.  Advertising S16_LE alone makes Engine pick
 * S16_LE, and 44100 + S16_LE converts cleanly.
 *
 * ALSASHIM_ALLOW_32BIT  non-empty to leave the real format mask alone. */
typedef int snd_pcm_format_t;
typedef struct _snd_pcm_format_mask snd_pcm_format_mask_t;
#define ALSASHIM_FMT_S16_LE 2 /* SND_PCM_FORMAT_S16_LE */
typedef void (*fmt_mask_none_t)(snd_pcm_format_mask_t *);
typedef int (*fmt_mask_set_t)(snd_pcm_format_mask_t *, snd_pcm_format_t);
typedef void (*get_fmt_mask_t)(const snd_pcm_hw_params_t *,
                               snd_pcm_format_mask_t *);
typedef int (*test_fmt_t)(snd_pcm_t *, snd_pcm_hw_params_t *,
                          snd_pcm_format_t);
static get_fmt_mask_t real_get_fmt_mask = NULL;
static test_fmt_t real_test_fmt = NULL;
static fmt_mask_none_t real_mask_none = NULL;
static fmt_mask_set_t real_mask_set = NULL;

static int restrict_formats(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("ALSASHIM_ALLOW_32BIT");
        v = (e && *e) ? 0 : 1;
    }
    return v;
}

/* Single rate advertised to Engine (real inmusic codecs are 44100-only).
 * ALSASHIM_RATE overrides; "0" or empty disables the clamp. */
static unsigned int alsashim_target_rate(void) {
    const char *v = getenv("ALSASHIM_RATE");
    if (!v || !*v) return 44100;
    long n = strtol(v, NULL, 10);
    return (n > 0) ? (unsigned int)n : 44100;
}

void snd_pcm_hw_params_get_format_mask(const snd_pcm_hw_params_t *params,
                                       snd_pcm_format_mask_t *mask) {
    if (!real_get_fmt_mask)
        real_get_fmt_mask = (get_fmt_mask_t)dlsym(
            RTLD_NEXT, "snd_pcm_hw_params_get_format_mask");
    if (real_get_fmt_mask) real_get_fmt_mask(params, mask);
    if (!restrict_formats() || !mask) return;
    if (!real_mask_none)
        real_mask_none =
            (fmt_mask_none_t)dlsym(RTLD_NEXT, "snd_pcm_format_mask_none");
    if (!real_mask_set)
        real_mask_set =
            (fmt_mask_set_t)dlsym(RTLD_NEXT, "snd_pcm_format_mask_set");
    if (real_mask_none) real_mask_none(mask);
    if (real_mask_set) real_mask_set(mask, ALSASHIM_FMT_S16_LE);
}

int snd_pcm_hw_params_test_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                  snd_pcm_format_t format) {
    if (!real_test_fmt)
        real_test_fmt = (test_fmt_t)dlsym(RTLD_NEXT,
                                          "snd_pcm_hw_params_test_format");
    if (restrict_formats() && format != ALSASHIM_FMT_S16_LE) return -EINVAL;
    return real_test_fmt ? real_test_fmt(pcm, params, format) : 0;
}

/* --- Debug: log the hw-params sequence Engine builds (ALSASHIM_DEBUG) ----- */
typedef int (*hw_set_fmt_t)(snd_pcm_t *, snd_pcm_hw_params_t *,
                            snd_pcm_format_t);
typedef int (*hw_rate_t)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int, int);
typedef int (*hw_rate_p_t)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *,
                           int *);
typedef int (*hw_ch_t)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int);
typedef int (*hw_persz_t)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t,
                          int);
typedef int (*hw_persz_p_t)(snd_pcm_t *, snd_pcm_hw_params_t *,
                            snd_pcm_uframes_t *, int *);
typedef int (*hw_apply_t)(snd_pcm_t *, snd_pcm_hw_params_t *);
static hw_set_fmt_t real_set_fmt = NULL;
static hw_rate_t real_set_rate = NULL;
static hw_rate_p_t real_set_rate_near = NULL;
static hw_ch_t real_set_channels = NULL;
static hw_persz_t real_set_persz = NULL;
static hw_persz_p_t real_set_persz_near = NULL;
static hw_apply_t real_apply = NULL;

int snd_pcm_hw_params_set_format(snd_pcm_t *p, snd_pcm_hw_params_t *h,
                                 snd_pcm_format_t f) {
    RESOLVE(real_set_fmt, "snd_pcm_hw_params_set_format");
    if (!real_set_fmt) return -ENOSYS;
    /* Engine sets S32_LE regardless of what the mask advertises, and plug's
     * rate converter cannot resample 44100 into a 32-bit format.  Substitute
     * S16_LE so the conversion is possible; Engine reads the negotiated
     * format back from hw_params, as it must for a plug-routed device. */
    snd_pcm_format_t use = f;
    if (restrict_formats() && f != ALSASHIM_FMT_S16_LE)
        use = ALSASHIM_FMT_S16_LE;
    int r = real_set_fmt(p, h, use);
    if (debug_on())
        fprintf(stderr, "[alsashim] set_format(%d)->(%d) -> %d\n", (int)f,
                (int)use, r);
    return r;
}

int snd_pcm_hw_params_set_rate(snd_pcm_t *p, snd_pcm_hw_params_t *h,
                               unsigned int r, int d) {
    RESOLVE(real_set_rate, "snd_pcm_hw_params_set_rate");
    unsigned int want = alsashim_target_rate();
    int rc = real_set_rate ? real_set_rate(p, h, want, d) : -ENOSYS;
    if (debug_on()) fprintf(stderr, "[alsashim] set_rate(%u)->(%u,%d) -> %d\n", r, want, d, rc);
    return rc;
}

int snd_pcm_hw_params_set_rate_near(snd_pcm_t *p, snd_pcm_hw_params_t *h,
                                    unsigned int *r, int *d) {
    RESOLVE(real_set_rate_near, "snd_pcm_hw_params_set_rate_near");
    unsigned int req = r ? *r : 0;
    unsigned int want = alsashim_target_rate();
    if (r) *r = want;
    int rc = real_set_rate_near ? real_set_rate_near(p, h, r, d) : -ENOSYS;
    if (debug_on())
        fprintf(stderr, "[alsashim] set_rate_near(%u)->(%u) -> %d got %u\n", req, want, rc,
                r ? *r : 0);
    return rc;
}

/* Engine enumerates the rates the PCM advertises and upsamples to the highest
 * one it finds; through `plug` on a 48000-only board that is 96000, and that
 * upsampling path is what it then fails on (`ALSACombinedDevice::
 * setUpsamplingRate` warning) before the deck render callback runs.  Real
 * inmusic hardware is 44100-only, so advertise exactly one rate. */
typedef int (*hw_test_rate_t)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int, int);
typedef int (*hw_get_rate_t)(const snd_pcm_hw_params_t *, unsigned int *, int *);
static hw_test_rate_t real_test_rate = NULL;
static hw_get_rate_t real_get_ratemin = NULL;
static hw_get_rate_t real_get_ratemax = NULL;

int snd_pcm_hw_params_test_rate(snd_pcm_t *p, snd_pcm_hw_params_t *h,
                                unsigned int r, int d) {
    RESOLVE(real_test_rate, "snd_pcm_hw_params_test_rate");
    if (r != alsashim_target_rate()) return -EINVAL;
    return real_test_rate ? real_test_rate(p, h, r, d) : 0;
}

int snd_pcm_hw_params_get_rate_min(const snd_pcm_hw_params_t *h, unsigned int *v,
                                   int *d) {
    RESOLVE(real_get_ratemin, "snd_pcm_hw_params_get_rate_min");
    int rc = real_get_ratemin ? real_get_ratemin(h, v, d) : -ENOSYS;
    if (v) *v = alsashim_target_rate();
    return rc;
}

int snd_pcm_hw_params_get_rate_max(const snd_pcm_hw_params_t *h, unsigned int *v,
                                   int *d) {
    RESOLVE(real_get_ratemax, "snd_pcm_hw_params_get_rate_max");
    int rc = real_get_ratemax ? real_get_ratemax(h, v, d) : -ENOSYS;
    if (v) *v = alsashim_target_rate();
    return rc;
}

/* --- Virtual channel layout bridged onto a 2-channel device --------------
 * The real JP11 codec presents S32_LE / 4 channels / 44100.  Engine's first
 * channel pair is the cue (headphone) bus and the second pair is the master
 * bus, so a 2-channel device only ever receives cue.  QEMU's usb-audio cannot
 * carry more than 2 channels cleanly, so instead of truncating the layout we
 * let Engine negotiate its native 4-channel one, negotiate the underlying
 * (plug) device at 2 channels, and copy the master pair out of each virtual
 * frame in snd_pcm_writei.  plug then does the S32->S16 / 44100->48000
 * conversion, which is correct at 2 channels.
 *
 *   ALSASHIM_CHANNELS         channels advertised to Engine (default 4)
 *   ALSASHIM_HW_CHANNELS      channels on the real device (default 2)
 *   ALSASHIM_MASTER_CHANNELS  source indices of the master pair (default 2,3)
 *
 * With ALSASHIM_CHANNELS == ALSASHIM_HW_CHANNELS the bridge is disabled and
 * the shim is a pass-through. */
static unsigned int env_uint(const char *name, unsigned int dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    long n = strtol(v, NULL, 10);
    return (n > 0 && n < 1024) ? (unsigned int)n : dflt;
}

static unsigned int virtual_channels(void) {
    return env_uint("ALSASHIM_CHANNELS", 4);
}
static unsigned int hw_channels(void) {
    return env_uint("ALSASHIM_HW_CHANNELS", 2);
}

static void master_pair(unsigned int *l, unsigned int *r) {
    static int done = 0;
    static unsigned int a = 2, b = 3;
    if (!done) {
        const char *v = getenv("ALSASHIM_MASTER_CHANNELS");
        unsigned x, y;
        if (v && *v && sscanf(v, "%u,%u", &x, &y) == 2) { a = x; b = y; }
        done = 1;
    }
    *l = a;
    *r = b;
}

/* Set once Engine has negotiated the virtual channel count. */
static int bridge_active = 0;

int snd_pcm_hw_params_set_channels(snd_pcm_t *p, snd_pcm_hw_params_t *h,
                                   unsigned int c) {
    RESOLVE(real_set_channels, "snd_pcm_hw_params_set_channels");
    if (!real_set_channels) return -ENOSYS;

    unsigned int vc = virtual_channels(), hc = hw_channels();
    unsigned int use = c;
    if (vc != hc && c == vc) {
        use = hc;
        bridge_active = 1;
    } else if (c != vc) {
        bridge_active = 0;
    }
    int rc = real_set_channels(p, h, use);
    if (debug_on())
        fprintf(stderr, "[alsashim] set_channels(%u)->(%u) -> %d (virtual %u, hw %u)\n",
                c, use, rc, vc, hc);
    return rc;
}

typedef int (*hw_ch_near_t)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *);
static hw_ch_near_t real_set_channels_near = NULL;

int snd_pcm_hw_params_set_channels_near(snd_pcm_t *p, snd_pcm_hw_params_t *h,
                                        unsigned int *c) {
    RESOLVE(real_set_channels_near, "snd_pcm_hw_params_set_channels_near");
    if (!real_set_channels_near || !c) return -ENOSYS;

    unsigned int want = *c;
    unsigned int vc = virtual_channels(), hc = hw_channels();
    unsigned int use = want;
    if (vc != hc && want >= vc) use = hc;
    int rc = real_set_channels_near(p, h, &use);
    if (rc == 0) {
        if (vc != hc && want >= vc) { *c = vc; bridge_active = 1; }
        else { *c = use; bridge_active = 0; }
    }
    if (debug_on())
        fprintf(stderr, "[alsashim] set_channels_near(%u) -> %d got %u\n",
                want, rc, *c);
    return rc;
}

int snd_pcm_hw_params_set_period_size(snd_pcm_t *p, snd_pcm_hw_params_t *h,
                                      snd_pcm_uframes_t v, int d) {
    RESOLVE(real_set_persz, "snd_pcm_hw_params_set_period_size");
    int rc = real_set_persz ? real_set_persz(p, h, v, d) : -ENOSYS;
    if (debug_on())
        fprintf(stderr, "[alsashim] set_period_size(%lu,%d) -> %d\n",
                (unsigned long)v, d, rc);
    return rc;
}

int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *p,
                                           snd_pcm_hw_params_t *h,
                                           snd_pcm_uframes_t *v, int *d) {
    RESOLVE(real_set_persz_near, "snd_pcm_hw_params_set_period_size_near");
    snd_pcm_uframes_t req = v ? *v : 0;
    int rc = real_set_persz_near ? real_set_persz_near(p, h, v, d) : -ENOSYS;
    if (debug_on())
        fprintf(stderr, "[alsashim] set_period_size_near(%lu) -> %d got %lu\n",
                (unsigned long)req, rc, (unsigned long)(v ? *v : 0));
    return rc;
}

int snd_pcm_hw_params(snd_pcm_t *p, snd_pcm_hw_params_t *h) {
    RESOLVE(real_apply, "snd_pcm_hw_params");
    int rc = real_apply ? real_apply(p, h) : -ENOSYS;
    if (debug_on()) fprintf(stderr, "[alsashim] hw_params -> %d\n", rc);
    return rc;
}

/* Engine reads the plug layer's channel maximum (10000 - plug can route any
 * count) and uses it as the channel count, which no real device can
 * realise, so hw_params then fails.  Report the count the real JP11 codec
 * has (4), so Engine negotiates its native layout. */
typedef int (*hw_ch_get_t)(const snd_pcm_hw_params_t *, unsigned int *);
static hw_ch_get_t real_get_chmax = NULL;
static hw_ch_get_t real_get_chmin = NULL;
static hw_ch_get_t real_get_chcur = NULL;

int snd_pcm_hw_params_get_channels_max(const snd_pcm_hw_params_t *h,
                                       unsigned int *val) {
    RESOLVE(real_get_chmax, "snd_pcm_hw_params_get_channels_max");
    int r = real_get_chmax ? real_get_chmax(h, val) : -ENOSYS;
    unsigned int vc = virtual_channels();
    if (val && *val > vc) {
        if (debug_on())
            fprintf(stderr, "[alsashim] channels_max %u -> %u\n", *val, vc);
        *val = vc;
    }
    return r;
}

int snd_pcm_hw_params_get_channels_min(const snd_pcm_hw_params_t *h,
                                       unsigned int *val) {
    RESOLVE(real_get_chmin, "snd_pcm_hw_params_get_channels_min");
    int r = real_get_chmin ? real_get_chmin(h, val) : -ENOSYS;
    if (val && *val > virtual_channels()) *val = virtual_channels();
    return r;
}

/* After the bridge is armed the real device is 2-channel, but Engine must
 * size its frames for the 4-channel layout it negotiated. */
int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *h,
                                   unsigned int *val) {
    RESOLVE(real_get_chcur, "snd_pcm_hw_params_get_channels");
    int r = real_get_chcur ? real_get_chcur(h, val) : -ENOSYS;
    if (r == 0 && bridge_active && val) {
        if (debug_on())
            fprintf(stderr, "[alsashim] channels %u -> %u\n", *val,
                    virtual_channels());
        *val = virtual_channels();
    }
    return r;
}
/* --- Force the RW path (safety net) --------------------------------------
 * Engine asks for SND_PCM_ACCESS_RW_INTERLEAVED itself and imports only
 * snd_pcm_writei/snd_pcm_readi (confirmed from Engine.bin's dynamic symbols),
 * so this is a no-op in practice.  It exists because only the read/write path
 * can carry a format conversion through `plug`: an mmap buffer is a window
 * onto the *slave* ring, so an application format that differs from the
 * hardware's would be written without conversion.  Forcing RW keeps a caller
 * that did negotiate mmap on the converting path instead.
 * ALSASHIM_FORCE_RW non-empty enables this (SND_PCM_ACCESS_RW_INTERLEAVED=3). */
typedef int (*hw_access_t)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
static hw_access_t real_set_access = NULL;

int snd_pcm_hw_params_set_access(snd_pcm_t *p, snd_pcm_hw_params_t *h, int a) {
    RESOLVE(real_set_access, "snd_pcm_hw_params_set_access");
    int use = a;
    const char *e = getenv("ALSASHIM_FORCE_RW");
    if (e && *e) use = 3; /* SND_PCM_ACCESS_RW_INTERLEAVED */
    int r = real_set_access ? real_set_access(p, h, use) : -ENOSYS;
    if (debug_on())
        fprintf(stderr, "[alsashim] set_access(%d)->(%d) -> %d\n", a, use, r);
    return r;
}

/* --- Master-pair extraction onto the 2-channel device --------------------
 * Engine writes its negotiated 4-channel S32 frames here.  When the bridge is
 * armed, copy the master pair (default indices 2,3) into a 2-channel S32
 * buffer and hand that to plug, which converts it to the hw's S16 @ 48000.
 * Any extra hw channels are zeroed.  With ALSASHIM_DUMP set, also report the
 * peak sample every 64 calls (on the virtual frame, pre-extraction). */
typedef long (*writei_t)(snd_pcm_t *, const void *, unsigned long);
static writei_t real_writei = NULL;
static int32_t *bridge_buf = NULL;
static size_t bridge_cap = 0;

long snd_pcm_writei(snd_pcm_t *p, const void *b, unsigned long f) {
    RESOLVE(real_writei, "snd_pcm_writei");
    if (!real_writei) return -ENOSYS;

    if (getenv("ALSASHIM_WBT")) {
        static int wbt_done = 0;
        if (!wbt_done) {
            wbt_done = 1;
            void *bt[64];
            int n = backtrace(bt, 64);
            fprintf(stderr, "\n[alsashim] === writei backtrace (%d frames) ===\n", n);
            backtrace_symbols_fd(bt, n, 2);
            fprintf(stderr, "[alsashim] === end backtrace ===\n");
        }
    }

    if (getenv("ALSASHIM_DUMP") && b && f) {
        static unsigned long calls = 0;
        const int32_t *s = (const int32_t *)b;
        unsigned int nc = bridge_active ? virtual_channels() : 2;
        unsigned long n = f;
        if (nc > 16) nc = 16;
        if (n > 2048) n = 2048;
        long pk[16] = {0};
        for (unsigned long i = 0; i < n; i++)
            for (unsigned int c = 0; c < nc; c++) {
                long v = s[i * nc + c];
                if (v < 0) v = -v;
                if (v > pk[c]) pk[c] = v;
            }
        if ((calls++ % 64) == 0) {
            fprintf(stderr, "[alsashim] writei frames=%lu ch:", f);
            for (unsigned int c = 0; c < nc; c++) fprintf(stderr, " %ld", pk[c]);
            fprintf(stderr, "\n");
        }
    }

    unsigned int vc = virtual_channels(), hc = hw_channels();
    if (!(bridge_active && vc != hc && b && f))
        return real_writei(p, b, f);

    size_t need = (size_t)f * hc * sizeof(int32_t);
    if (need > bridge_cap) {
        int32_t *nb = realloc(bridge_buf, need);
        if (!nb) return real_writei(p, b, f);
        bridge_buf = nb;
        bridge_cap = need;
    }

    unsigned int ml, mr;
    master_pair(&ml, &mr);
    const int32_t *src = (const int32_t *)b;
    int32_t *dst = bridge_buf;
    /* The SC6000/Prime GO codec exposes TWO stereo outputs, not one: Layer A /
     * RCA pair 1 on channels 0,1 and Layer B / RCA pair 2 on 2,3.  A 2-channel
     * board therefore has to sum both layers, or a track playing on the second
     * layer is silent.  ALSASHIM_LAYER_MIX=1 enables the sum (saturating). */
    const char *mixv = getenv("ALSASHIM_LAYER_MIX");
    int mix = (mixv && *mixv && *mixv != '0');
    for (unsigned long i = 0; i < f; i++) {
        if (mix && vc >= 4 && hc == 2) {
            long l = (long)src[i * vc + 0] + (long)src[i * vc + 2];
            long r = (long)src[i * vc + 1] + (long)src[i * vc + 3];
            if (l > 2147483647L) l = 2147483647L;
            if (l < -2147483648L) l = -2147483648L;
            if (r > 2147483647L) r = 2147483647L;
            if (r < -2147483648L) r = -2147483648L;
            dst[i * hc + 0] = (int32_t)l;
            dst[i * hc + 1] = (int32_t)r;
            continue;
        }
        dst[i * hc + 0] = src[i * vc + (ml < vc ? ml : 0)];
        if (hc > 1) dst[i * hc + 1] = src[i * vc + (mr < vc ? mr : 1)];
        for (unsigned int c = 2; c < hc; c++) dst[i * hc + c] = 0;
    }
    return real_writei(p, dst, f);
}

/* --- allowlist reader (diagnostic, shim-only) -----------------------------
 *
 * The card-name allowlist is a std::vector<std::string> built from Engine's
 * compiled-in config map, so there is no file to edit and no exported symbol to
 * hook. The vector-copy constructor FUN_0038116c copies each element with
 * memcpy (call site 0x00381264 in the unrebased image). Interposing memcpy and
 * logging the source of calls that return there dumps the entries, which lets
 * ALSASHIM_AS be set to the real card name instead of patching Engine.
 * Enable with ALSASHIM_DUMP_ALLOWLIST=1. */
#include <stdint.h>

static int g_dump_allowlist = 0;

__attribute__((constructor))
static void alsashim_dump_ctor(void) {
    const char *v = getenv("ALSASHIM_DUMP_ALLOWLIST");
    g_dump_allowlist = (v && *v) ? 1 : 0;
}

typedef void *(*memcpy_t)(void *, const void *, size_t);
void *memcpy(void *dest, const void *src, size_t n) {
    static memcpy_t real_memcpy = NULL;
    if (!real_memcpy) real_memcpy = (memcpy_t)dlsym(RTLD_NEXT, "memcpy");
    if (g_dump_allowlist && n >= 2 && n <= 24) {
        const unsigned char *s = (const unsigned char *)src;
        int ok = 1;
        for (size_t i = 0; i < n; i++) { if (s[i] < 0x20 || s[i] > 0x7e) { ok = 0; break; } }
        if (ok) {
            static long cnt = 0;
            if (cnt < 40000) {
                fprintf(stderr, "[alsashim] mcp n=%zu ra=0x%lx \"%.*s\"\n",
                        n, (unsigned long)(uintptr_t)__builtin_return_address(0),
                        (int)n, (const char *)src);
                cnt++;
            }
        }
    }
    return real_memcpy(dest, src, n);
}

/* Log both operands of the card-name comparisons in scanDevices. The allowlist
 * loop compares the (spoofed) card name against each entry with memcmp; the
 * comparison is length-gated, so set ALSASHIM_AS to a string of the target
 * length. Enable with ALSASHIM_DUMP_ALLOWLIST=1. */
typedef int (*memcmp_t)(const void *, const void *, size_t);
int memcmp(const void *a, const void *b, size_t n) {
    static memcmp_t real_memcmp = NULL;
    if (!real_memcmp) real_memcmp = (memcmp_t)dlsym(RTLD_NEXT, "memcmp");
    if (g_dump_allowlist && n >= 2 && n <= 40) {
        const char *nm = getenv("ALSASHIM_AS");
        if (nm && strlen(nm) == n) {
            if (real_memcmp(a, nm, n) == 0)
                fprintf(stderr, "[alsashim] AL n=%zu entry=\"%.*s\"\n", n, (int)n, (const char *)b);
            else if (real_memcmp(b, nm, n) == 0)
                fprintf(stderr, "[alsashim] AL n=%zu entry=\"%.*s\"\n", n, (int)n, (const char *)a);
        }
    }
    return real_memcmp(a, b, n);
}

/* --- Crash backtrace (diagnostic) ----------------------------------------
 * Engine SEGV/ABRTs shortly after ALSADevice::start() once the card is
 * accepted.  With ALSASHIM_BT=1 install SA_SIGINFO handlers that print the
 * faulting address, PC/LR/SP and the containing object + offset, so the crash
 * can be mapped back through Ghidra (file offset = ghidra_addr - 0x10000).
 * backtrace() itself returns 0 frames here (no frame pointers), so the
 * register context is the useful part. */
#include <execinfo.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>

static void puthex(const char *tag, unsigned long v) {
    char buf[64];
    static const char hx[] = "0123456789abcdef";
    int n = 0;
    for (const char *t = tag; *t; t++) buf[n++] = *t;
    buf[n++] = '0'; buf[n++] = 'x';
    int started = 0;
    for (int sh = 28; sh >= 0; sh -= 4) {
        int d = (v >> sh) & 0xf;
        if (d || started || sh == 0) { buf[n++] = hx[d]; started = 1; }
    }
    buf[n++] = '\n';
    ssize_t r = write(2, buf, (size_t)n); (void)r;
}

static volatile sig_atomic_t in_bt = 0;
static void alsashim_bt_handler(int sig, siginfo_t *si, void *uc_) {
    if (in_bt) _exit(3);
    in_bt = 1;
    signal(sig, SIG_DFL); /* recursive fault kills instead of looping */
    ucontext_t *uc = (ucontext_t *)uc_;
    unsigned long pc = 0, lr = 0, sp = 0, fp = 0;
    if (uc) {
        pc = (unsigned long)uc->uc_mcontext.arm_pc;
        lr = (unsigned long)uc->uc_mcontext.arm_lr;
        sp = (unsigned long)uc->uc_mcontext.arm_sp;
        fp = (unsigned long)uc->uc_mcontext.arm_fp;
    }
    ssize_t r;
    r = write(2, "\n[alsashim] caught signal\n", 26); (void)r;
    puthex("si_addr=", (unsigned long)(si ? si->si_addr : 0));
    puthex("pc=", pc);
    puthex("lr=", lr);
    puthex("sp=", sp);
    puthex("fp=", fp);
    {
        void *bt[96];
        int nb = backtrace(bt, 96);
        r = write(2, "--- crash backtrace ---\n", 25); (void)r;
        backtrace_symbols_fd(bt, nb, 2);
        r = write(2, "--- end ---\n", 12); (void)r;
    }
    r = write(2, "--- fp chain ---\n", 17); (void)r;
    unsigned long f = fp;
    for (int i = 0; i < 48 && f; i++) {
        unsigned long prev = *(volatile unsigned long *)f;
        unsigned long ra = *(volatile unsigned long *)(f + 4);
        puthex("fp=", f); puthex("  ra=", ra);
        if (prev <= f) break;
        f = prev;
    }
    r = write(2, "--- stack ---\n", 14); (void)r;
    unsigned long *st = (unsigned long *)sp;
    for (int i = 0; i < 160; i++) puthex("stk=", st[i]);
    {
        int fd = open("/proc/self/maps", O_RDONLY);
        if (fd >= 0) {
            char mb[4096]; ssize_t k;
            r = write(2, "--- self maps ---\n", 18); (void)r;
            while ((k = read(fd, mb, sizeof(mb))) > 0) { r = write(2, mb, (size_t)k); (void)r; }
            close(fd);
        }
    }
    _exit(128 + sig);
}

__attribute__((constructor))
static void alsashim_bt_ctor(void) {
    if (!getenv("ALSASHIM_BT")) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = alsashim_bt_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    fprintf(stderr, "[alsashim] backtrace handlers installed\n");
}
