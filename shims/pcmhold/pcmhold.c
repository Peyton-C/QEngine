/* pcmhold — gives a snd-aloop loopback card the channel counts of the card it
 * stands in for, by holding the loopback's partner device open.
 *
 *   pcmhold <card> <playback ch> <capture ch> [rate]
 *
 * An app that insists on its own sound card gets the kernel's loopback card,
 * renamed to that card's id (see qengine-hv01.service in
 * build_armv7_engine_rootfs.sh). A bare loopback offers 1 to 32 channels each
 * way, and HeadRush Prime 5.2.0's Evil takes all 32: it reported
 * "IN: 0 [hw:HV01,0] ch: 32 / OUT: ... ch: 32" and then segfaulted in its own
 * audio thread, in a per-channel copy loop overrunning buffers sized for the
 * real card. HG06's devicetree gives that card's I2S port 8 playback and 2
 * capture channels.
 *
 * snd-aloop pairs device 0 with device 1: what is played on one is captured on
 * the other, and once one end of a pair has hw_params set, the other end can
 * only open with the same channels, rate and format. So this opens device 1
 * with the counts swapped -- capture with the app's playback count, playback
 * with its capture count -- and device 0, where the app opens hw:<card>,0, then
 * offers exactly the real card's shape. It then keeps both running, reading
 * and discarding what the app plays and feeding it silence to capture, so the
 * pair stays configured for as long as this runs.
 *
 * S32_LE because RtAudio, which Evil uses, picks the widest integer format a
 * device offers, and the Rockchip I2S offers S32_LE. The rate defaults to the
 * 96000 Evil logs ("RtAudio stream created, sample rate: 96000").
 */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERIOD 96

static snd_pcm_t *open_pcm(const char *dev, snd_pcm_stream_t dir,
                           unsigned channels, unsigned rate) {
    snd_pcm_t *pcm;
    int err = snd_pcm_open(&pcm, dev, dir, 0);
    if (err < 0) {
        fprintf(stderr, "pcmhold: open %s %s: %s\n", dev,
                dir == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture",
                snd_strerror(err));
        exit(1);
    }
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S32_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED, channels, rate,
                             0 /* no resampling */, 100000 /* 100 ms */);
    if (err < 0) {
        fprintf(stderr, "pcmhold: %s %u ch at %u Hz: %s\n", dev, channels, rate,
                snd_strerror(err));
        exit(1);
    }
    return pcm;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: pcmhold <card> <playback ch> <capture ch> [rate]\n");
        return 2;
    }
    unsigned play_ch = (unsigned)atoi(argv[2]);
    unsigned cap_ch = (unsigned)atoi(argv[3]);
    unsigned rate = argc > 4 ? (unsigned)atoi(argv[4]) : 96000;
    char dev[64];
    snprintf(dev, sizeof(dev), "hw:%s,1", argv[1]);

    /* Swapped: device 1 captures what the app plays on device 0, and plays what
     * the app captures there. */
    snd_pcm_t *cap = open_pcm(dev, SND_PCM_STREAM_CAPTURE, play_ch, rate);
    snd_pcm_t *play = open_pcm(dev, SND_PCM_STREAM_PLAYBACK, cap_ch, rate);
    fprintf(stderr, "pcmhold: %s held, app side offers %u out / %u in at %u Hz\n",
            dev, play_ch, cap_ch, rate);

    int32_t *in = calloc((size_t)PERIOD * play_ch, sizeof(int32_t));
    int32_t *silence = calloc((size_t)PERIOD * cap_ch, sizeof(int32_t));
    if (!in || !silence) return 1;

    for (;;) {
        snd_pcm_sframes_t n = snd_pcm_writei(play, silence, PERIOD);
        if (n < 0) snd_pcm_recover(play, (int)n, 1);
        n = snd_pcm_readi(cap, in, PERIOD);
        if (n < 0) snd_pcm_recover(cap, (int)n, 1);
    }
}
