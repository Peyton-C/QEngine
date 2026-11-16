/* alsa: Engine's path from "is there a sound card" to audio leaving it, which
 * is alsashim's four gates in the order its header describes them.
 *
 *   1. walk the cards and take the one whose name is on the product's list
 *   2. ask it for a playback device, and for a capture one
 *   3. open hw:N and configure 44100 Hz, 128-frame periods, a 256-frame ring
 *   4. write, and never recover from an underrun
 *
 * The list of accepted names is compiled into Engine. The builder already
 * worked out which name this product's list holds and put it in ALSASHIM_AS,
 * so that is the name looked for here.
 *
 * It plays one second of a quiet 440 Hz tone, which a booted instance's wav
 * capture will show. In a chroot the cards are the host's, and a real one will
 * not grant a 128-frame period, so check.sh loads snd-dummy there and names it
 * in FAKE_ALSA_CARD: only that card is then considered. */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fake.h"

#define RATE 44100
#define PERIOD 128
#define RING 256

static long env_long(const char *name, long fallback) {
    const char *v = getenv(name);
    return v && *v ? strtol(v, NULL, 10) : fallback;
}

void probe_alsa(void) {
    if (!shim_exporting("snd_pcm_open")) {
        t_skip("alsa", "no alsashim preloaded");
        return;
    }
    const char *want = getenv("ALSASHIM_AS");
    if (!want || !*want) want = "RMZ2"; /* alsashim's own default */
    if (strcmp(want, "off") == 0) want = NULL;

    /* 1. The card. */
    snd_ctl_card_info_t *info;
    snd_ctl_card_info_alloca(&info);
    snd_ctl_t *ctl = NULL;
    int card = -1, accepted = -1, cards = 0;
    long only = env_long("FAKE_ALSA_CARD", -1);
    while (snd_card_next(&card) == 0 && card >= 0) {
        char dev[32];
        if (only >= 0 && card != only) continue;
        snd_ctl_t *c;
        snprintf(dev, sizeof(dev), "hw:%d", card);
        if (snd_ctl_open(&c, dev, 0) < 0) continue;
        cards++;
        const char *name = snd_ctl_card_info(c, info) == 0 ? snd_ctl_card_info_get_name(info) : "";
        t_note("alsa card %d reports \"%s\"", card, name);
        if (accepted < 0 && (!want || strcmp(name, want) == 0)) {
            accepted = card;
            ctl = c;
        } else {
            snd_ctl_close(c);
        }
    }
    if (cards == 0) {
        t_skip("alsa", "no sound card on this machine");
        return;
    }
    if (accepted < 0) {
        t_fail("alsa.card-name", "no card reports \"%s\", so Engine skips them all", want);
        return;
    }
    if (want) t_ok("alsa.card-name", "card %d reports \"%s\"", accepted, want);
    else t_skip("alsa.card-name", "ALSASHIM_AS=off, taking card %d as it is", accepted);

    /* 2. Its streams. */
    snd_pcm_info_t *pinfo;
    snd_pcm_info_alloca(&pinfo);
    snd_pcm_info_set_device(pinfo, 0);
    snd_pcm_info_set_subdevice(pinfo, 0);
    snd_pcm_info_set_stream(pinfo, SND_PCM_STREAM_PLAYBACK);
    int err = snd_ctl_pcm_info(ctl, pinfo);
    if (err < 0) t_fail("alsa.playback-stream", "%s", snd_strerror(err));
    else t_ok("alsa.playback-stream", "device 0 plays");
    snd_pcm_info_set_stream(pinfo, SND_PCM_STREAM_CAPTURE);
    int capture = snd_ctl_pcm_info(ctl, pinfo);
    if (getenv("ALSASHIM_NO_CAPTURE") && *getenv("ALSASHIM_NO_CAPTURE")) {
        if (capture < 0) t_ok("alsa.capture-hidden", "no capture stream offered");
        else t_fail("alsa.capture-hidden", "ALSASHIM_NO_CAPTURE is set and capture shows");
    } else {
        t_note("alsa capture stream: %s", capture < 0 ? "none" : "present");
    }
    snd_ctl_close(ctl);
    if (err < 0) return;

    /* 3. Open and configure, as Engine does: the maximum channel count, then
     * the ring. */
    char dev[32];
    snd_pcm_t *pcm;
    snprintf(dev, sizeof(dev), "hw:%d", accepted);
    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        t_fail("alsa.open", "%s: %s", dev, snd_strerror(err));
        return;
    }
    t_ok("alsa.open", "%s opened as %s", dev, snd_pcm_name(pcm));

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    unsigned channels = 0, rate = RATE;
    snd_pcm_hw_params_get_channels_max(hw, &channels);
    /* The plug layer's nominal maximum is 10000, and Engine renders them all. */
    if (channels == 0 || channels > 64) {
        t_fail("alsa.channels", "%u channels offered", channels);
        snd_pcm_close(pcm);
        return;
    }
    t_ok("alsa.channels", "%u channels offered", channels);

    snd_pcm_uframes_t period = PERIOD, ring = RING;
    int step = 0;
    if ((step = 1, err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (step = 2, err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE)) < 0 ||
        (step = 3, err = snd_pcm_hw_params_set_channels(pcm, hw, channels)) < 0 ||
        (step = 4, err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL)) < 0 ||
        (step = 5, err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL)) < 0 ||
        (step = 6, err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &ring)) < 0 ||
        (step = 7, err = snd_pcm_hw_params(pcm, hw)) < 0) {
        static const char *const steps[] = {"", "access", "format", "channels", "rate",
                                            "period size", "buffer size", "hw_params"};
        t_fail("alsa.configure", "%s: %s", steps[step], snd_strerror(err));
        snd_pcm_close(pcm);
        return;
    }
    snd_pcm_hw_params_get_period_size(hw, &period, NULL);
    snd_pcm_hw_params_get_buffer_size(hw, &ring);
    if (rate != RATE) t_fail("alsa.configure", "%u Hz granted, not %d", rate, RATE);
    else t_ok("alsa.configure", "%u Hz, S32_LE, %u channels", rate, channels);

    long scale = env_long("ALSASHIM_BUFFER_SCALE", 8);
    if (period != PERIOD)
        t_fail("alsa.period", "%lu frames, Engine asked for %d", (unsigned long)period, PERIOD);
    else t_ok("alsa.period", "%d frames", PERIOD);
    if (ring == (snd_pcm_uframes_t)(RING * scale))
        t_ok("alsa.ring", "%lu frames, %d x %ld", (unsigned long)ring, RING, scale);
    else if (scale > 1 && ring > RING)
        t_ok("alsa.ring", "%lu frames: the card stopped short of %d x %ld",
             (unsigned long)ring, RING, scale);
    else t_fail("alsa.ring", "%lu frames, expected %ld", (unsigned long)ring, RING * scale);

    /* The thresholds Engine sets follow its own 256-frame ring. */
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_uframes_t start = 0, stop = 0;
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, PERIOD);
    snd_pcm_sw_params_set_stop_threshold(pcm, sw, RING);
    err = snd_pcm_sw_params(pcm, sw);
    snd_pcm_sw_params_get_start_threshold(sw, &start);
    snd_pcm_sw_params_get_stop_threshold(sw, &stop);
    const char *free_run = getenv("ALSASHIM_XRUN_FREE_RUN");
    if (err < 0) t_fail("alsa.thresholds", "%s", snd_strerror(err));
    else if (scale > 1 && (start != ring || (stop != ring && !(free_run && *free_run != '0'))))
        t_fail("alsa.thresholds", "start %lu, stop %lu, with a ring of %lu",
               (unsigned long)start, (unsigned long)stop, (unsigned long)ring);
    else t_ok("alsa.thresholds", "start %lu, stop %lu", (unsigned long)start, (unsigned long)stop);

    /* 4. Play. */
    int32_t *frames = calloc((size_t)PERIOD * channels, sizeof(*frames));
    long written = 0, total = RATE / PERIOD;
    double phase = 0;
    for (long i = 0; frames && i < total; i++, written++) {
        for (int f = 0; f < PERIOD; f++, phase += 2 * M_PI * 440 / RATE)
            for (unsigned c = 0; c < channels; c++)
                frames[f * channels + c] = (int32_t)(sin(phase) * 0.1 * INT32_MAX);
        snd_pcm_sframes_t done = snd_pcm_writei(pcm, frames, PERIOD);
        if (done != PERIOD) {
            t_fail("alsa.play", "write %ld of %ld: %s", i + 1, total,
                   done < 0 ? snd_strerror((int)done) : "short");
            break;
        }
    }
    if (written == total) t_ok("alsa.play", "%ld periods written without an underrun", total);
    free(frames);
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
}
