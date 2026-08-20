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
 *   ALSASHIM_MIDI_CARD  card number to report for card-less MIDI sequencer
 *                   clients (default 0); -1 disables this substitution
 *   ALSASHIM_DEBUG  non-empty to log each substitution to stderr
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Opaque to us — we only ever hand it back to the real libasound getters,
 * so there's no need to pull in <alsa/asoundlib.h> (and no need to link
 * against libasound: the real symbols are resolved out of the copy Engine
 * has already loaded). */
typedef struct _snd_ctl_card_info snd_ctl_card_info_t;

typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef struct _snd_pcm_sw_params snd_pcm_sw_params_t;
typedef struct _snd_seq_client_info snd_seq_client_info_t;

/* Matches alsa-lib's own definition; frame counts are unsigned long there. */
typedef unsigned long snd_pcm_uframes_t;

/* SND_PCM_STREAM_PLAYBACK, as returned by snd_pcm_stream(). */
#define ALSASHIM_STREAM_PLAYBACK 0

typedef const char *(*get_name_t)(const snd_ctl_card_info_t *);
typedef int (*get_card_t)(const snd_ctl_card_info_t *);
typedef int (*pcm_open_t)(snd_pcm_t **, const char *, int, int);
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

static get_name_t real_get_name = NULL;
static get_card_t real_get_card = NULL;
static pcm_open_t real_pcm_open = NULL;
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