// SPDX-License-Identifier: GPL-2.0
/*
 * Experimental reimplementation of the real onboard az04-codec ASoC codec
 * driver (RANE SYSTEM ONE / RMZ2), reverse-engineered from the real
 * snd-soc-inmusic-az04.ko (unstripped, John Keeping
 * <jkeeping@inmusicbrands.com>, kernel 6.18.9-imb-2026-02-06-rt3) via
 * objdump — see docs/BUILDING.md. The real driver has no register map, no
 * I2C/SPI: it's purely two optional GPIO lines (mute/reset) plus
 * devicetree-driven PCM capability negotiation via
 * device_property_read_u32_array() on inmusic,playback-channels /
 * inmusic,capture-channels / inmusic,pcm-rates.
 *
 * This version drops the devicetree dependency entirely (hardcoded
 * capabilities, no GPIO lookup table) and self-registers its own
 * platform_device instead of being instantiated from a devicetree node —
 * lets it bind under QEMU with no real devicetree support at all. See
 * az04_card.c for the matching machine driver, which binds this to
 * ASoC's built-in dummy CPU DAI/platform instead of the real (and, under
 * QEMU's virt machine, nonexistent) rockchip,rk3588-i2s-tdm controller.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/err.h>
#include <sound/soc.h>
#include <sound/pcm.h>

struct az04_codec {
	struct gpio_desc *mute_gpio;
	struct gpio_desc *reset_gpio;
};

static struct snd_soc_dai_driver az04_codec_dai = {
	.name = "az04-hifi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 6,
		.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 2,
		.channels_max = 6,
		.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
};

static const struct snd_soc_component_driver az04_codec_component_driver = {
	.name = "az04-codec",
};

static int az04_codec_probe(struct platform_device *pdev)
{
	struct az04_codec *priv;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	platform_set_drvdata(pdev, priv);

	/* No gpiod_lookup_table registered for this synthetic platform_device,
	 * so these always resolve to NULL (not an error) — matches the real
	 * driver's tolerance of a board with the lines left unconnected. */
	priv->mute_gpio = devm_gpiod_get_optional(&pdev->dev, "mute", GPIOD_OUT_LOW);
	if (IS_ERR(priv->mute_gpio))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->mute_gpio),
				      "Failed to get mute GPIO\n");

	priv->reset_gpio = devm_gpiod_get_optional(&pdev->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset_gpio))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->reset_gpio),
				      "Failed to get reset GPIO\n");

	dev_info(&pdev->dev, "az04-codec (experimental QEngine reimplementation) probed\n");

	return devm_snd_soc_register_component(&pdev->dev, &az04_codec_component_driver,
						&az04_codec_dai, 1);
}

static struct platform_driver az04_codec_driver = {
	.driver = {
		.name = "az04-codec",
	},
	.probe = az04_codec_probe,
};

static struct platform_device *az04_codec_pdev;

static int __init az04_codec_init(void)
{
	int ret;

	az04_codec_pdev = platform_device_register_simple("az04-codec", -1, NULL, 0);
	if (IS_ERR(az04_codec_pdev))
		return PTR_ERR(az04_codec_pdev);

	ret = platform_driver_register(&az04_codec_driver);
	if (ret)
		platform_device_unregister(az04_codec_pdev);

	return ret;
}

static void __exit az04_codec_exit(void)
{
	platform_driver_unregister(&az04_codec_driver);
	platform_device_unregister(az04_codec_pdev);
}

module_init(az04_codec_init);
module_exit(az04_codec_exit);

MODULE_DESCRIPTION("Experimental reimplementation of the inMusic AZ04 ASoC codec driver");
MODULE_AUTHOR("QEngine project");
MODULE_LICENSE("GPL");