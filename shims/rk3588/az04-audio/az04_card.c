// SPDX-License-Identifier: GPL-2.0
/*
 * Experimental machine driver pairing with az04_codec.c. On real RMZ2
 * hardware the machine driver is mainline `simple-audio-card` binding
 * `inmusic,az04-codec` to `rockchip,rk3588-i2s-tdm` — but that CPU DAI is
 * real RK3588 silicon QEMU's virt machine doesn't emulate at all (see
 * docs/BUILDING.md's "az04-codec driver exists, and is unusable here").
 * This binds the same codec to ASoC's built-in dummy CPU DAI/platform
 * (`snd_soc_dummy_dlc`, exported by soc-core) instead — a real,
 * fully-probing ALSA card and PCM device with nothing behind it, entirely
 * in software. No real hardware, no devicetree node, is involved on
 * either side of the link.
 *
 * Card name is literally "RMZ2", matching the real hardware's
 * simple-audio-card node name (`build/systemone_linux_debive_tree.dtb`),
 * on the chance Engine's device manager string-matches against it —
 * already tested and ruled out once before with a renamed HDA ALSA id
 * (see BUILDING.md), but cheap to also cover here.
 *
 * The CPU DAI role uses `snd_soc_dummy_dlc` (exported by soc-core) fine.
 * The PLATFORM role does not: soc-utils.c's dummy_dma_open() has a guard
 * ("if there are other components associated with rtd, we shouldn't
 * override their hwparams") that walks rtd's components looking for one
 * whose driver == &dummy_platform — which, when snd_soc_dummy_dlc itself
 * *is* the platform (our case), always finds itself and returns 0
 * immediately, before ever calling snd_soc_set_runtime_hwparams(). That
 * leaves runtime->hw.info at 0 — no SNDRV_PCM_INFO_INTERLEAVED bit set —
 * so ALSA core's snd_pcm_hw_constraints_complete() computes an empty
 * ACCESS mask and open() fails with EINVAL before ever reaching hw_params
 * (confirmed directly via a kretprobe on snd_pcm_hw_constraint_mask():
 * exactly one call, on ACCESS, returning the failure — format/channels/
 * rate constraints downstream are never even reached). snd-soc-dummy is
 * built for DPCM/no-PCM backend roles, not as a directly-openable
 * platform in a plain non-dynamic link like this one. Fixed by providing
 * our own trivial platform component below instead.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <sound/pcm.h>

static const struct snd_pcm_hardware az04_pcm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.buffer_bytes_max = 128 * 1024,
	.period_bytes_min = 4096,
	.period_bytes_max = 8192,
	.periods_min = 2,
	.periods_max = 128,
};

static int az04_platform_open(struct snd_soc_component *component,
			      struct snd_pcm_substream *substream)
{
	return snd_soc_set_runtime_hwparams(substream, &az04_pcm_hardware);
}

static const struct snd_soc_component_driver az04_platform_component_driver = {
	.name = "az04-platform",
	.open = az04_platform_open,
};

static struct snd_soc_dai_link_component az04_codec_component[] = {
	{ .name = "az04-codec", .dai_name = "az04-hifi" },
};

static struct snd_soc_dai_link_component az04_platform_component[] = {
	{ .name = "az04-card" },
};

static struct snd_soc_dai_link az04_dai_link = {
	.name = "az04-link",
	.stream_name = "az04 HiFi",
	.cpus = &snd_soc_dummy_dlc,
	.num_cpus = 1,
	.codecs = az04_codec_component,
	.num_codecs = ARRAY_SIZE(az04_codec_component),
	.platforms = az04_platform_component,
	.num_platforms = ARRAY_SIZE(az04_platform_component),
};

static struct snd_soc_card az04_card = {
	.name = "RMZ2",
	.owner = THIS_MODULE,
	.dai_link = &az04_dai_link,
	.num_links = 1,
};

static int az04_card_probe(struct platform_device *pdev)
{
	int ret;

	/* Registered on this same device, so az04_platform_component[]'s
	 * .name = "az04-card" above resolves to it. */
	ret = devm_snd_soc_register_component(&pdev->dev, &az04_platform_component_driver,
					      NULL, 0);
	if (ret)
		return ret;

	az04_card.dev = &pdev->dev;
	return devm_snd_soc_register_card(&pdev->dev, &az04_card);
}

static struct platform_driver az04_card_driver = {
	.driver = {
		.name = "az04-card",
	},
	.probe = az04_card_probe,
};

static struct platform_device *az04_card_pdev;

static int __init az04_card_init(void)
{
	int ret;

	az04_card_pdev = platform_device_register_simple("az04-card", -1, NULL, 0);
	if (IS_ERR(az04_card_pdev))
		return PTR_ERR(az04_card_pdev);

	ret = platform_driver_register(&az04_card_driver);
	if (ret)
		platform_device_unregister(az04_card_pdev);

	return ret;
}

static void __exit az04_card_exit(void)
{
	platform_driver_unregister(&az04_card_driver);
	platform_device_unregister(az04_card_pdev);
}

module_init(az04_card_init);
module_exit(az04_card_exit);

MODULE_DESCRIPTION("Experimental az04 machine driver, dummy CPU DAI (no real I2S/TDM)");
MODULE_AUTHOR("QEngine project");
MODULE_LICENSE("GPL");