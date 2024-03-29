/*
 * tegra_wm8903.c - Tegra machine ASoC driver for boards using WM8903 codec.
 *
 * Author: Stephen Warren <swarren@nvidia.com>
 * Copyright (C) 2010-2012 - NVIDIA, Inc.
 *
 * Based on code copyright/by:
 *
 * (c) 2009, 2010 Nvidia Graphics Pvt. Ltd.
 *
 * Copyright 2007 Wolfson Microelectronics PLC.
 * Author: Graeme Gregory
 *         graeme.gregory@wolfsonmicro.com or linux@wolfsonmicro.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#ifdef CONFIG_SWITCH
#include <linux/switch.h>
#endif

#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tegra_wm8903.h>

#include "../codecs/wm8903.h"

#include "tegra_asoc_utils.h"

#define DRV_NAME "tegra-snd-wm8903"

struct tegra_wm8903 {
	struct tegra_wm8903_platform_data pdata;
	struct tegra_asoc_utils_data util_data;
#ifdef CONFIG_SWITCH
	int jack_status;
#endif
	struct regulator *dmic_reg;
};

static int tegra_wm8903_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_codec *codec = codec_dai->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	int srate, mclk;
	int err;

	srate = params_rate(params);
	switch (srate) {
	case 64000:
	case 88200:
	case 96000:
		mclk = 128 * srate;
		break;
	default:
		mclk = 256 * srate;
		break;
	}
	/* FIXME: Codec only requires >= 3MHz if OSR==0 */
	while (mclk < 6000000)
		mclk *= 2;

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		dev_err(card->dev, "Can't configure clocks\n");
		return err;
	}

	err = snd_soc_dai_set_sysclk(codec_dai, 0, mclk,
					SND_SOC_CLOCK_IN);
	if (err < 0) {
		dev_err(card->dev, "codec_dai clock not set\n");
		return err;
	}

	return 0;
}

static int tegra_spdif_hw_params(struct snd_pcm_substream *substream,
                                 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	int srate, mclk, min_mclk;
	int err;

	srate = params_rate(params);
	switch (srate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
		mclk = 11289600;
		break;
	case 8000:
	case 16000:
	case 32000:
	case 48000:
	case 64000:
	case 96000:
		mclk = 12288000;
		break;
	default:
		return -EINVAL;
	}
	min_mclk = 128 * srate;

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % min_mclk))
			mclk = machine->util_data.set_mclk;
		else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	return 0;
}

static struct snd_soc_ops tegra_wm8903_ops = {
	.hw_params = tegra_wm8903_hw_params,
};

static struct snd_soc_ops tegra_spdif_ops = {
	.hw_params = tegra_spdif_hw_params,
};

static struct snd_soc_jack tegra_wm8903_hp_jack;

#ifdef CONFIG_SWITCH
/* These values are copied from Android WiredAccessoryObserver */
enum headset_state {
	BIT_NO_HEADSET = 0,
	BIT_HEADSET = (1 << 0),
	BIT_HEADSET_NO_MIC = (1 << 1),
};

static struct switch_dev tegra_wm8903_hp_switch = {
	.name = "h2w",
};

static int tegra_wm8903_jack_notifier(struct notifier_block *self,
                                      unsigned long action, void *dev)
{
	struct snd_soc_jack *jack = dev;
	struct snd_soc_codec *codec = jack->codec;
	struct snd_soc_card *card = codec->card;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	enum headset_state state = BIT_NO_HEADSET;

	if (jack == &tegra_wm8903_hp_jack) {
		machine->jack_status &= ~SND_JACK_HEADPHONE;
		machine->jack_status |= (action & SND_JACK_HEADPHONE);
	} else {
		machine->jack_status &= ~SND_JACK_MICROPHONE;
		machine->jack_status |= (action & SND_JACK_MICROPHONE);
	}

	switch (machine->jack_status) {
	case SND_JACK_HEADPHONE:
		snd_soc_dapm_enable_pin(dapm, "Headphone Jack");
		snd_soc_dapm_disable_pin(dapm, "Int Spk");
		state = BIT_HEADSET_NO_MIC;
		break;
	case SND_JACK_HEADSET:
		snd_soc_dapm_enable_pin(dapm, "Headphone Jack");
		snd_soc_dapm_disable_pin(dapm, "Int Spk");
		state = BIT_HEADSET;
		break;
	case SND_JACK_MICROPHONE:
		/* mic: would not report */
	default:
		snd_soc_dapm_disable_pin(dapm, "Headphone Jack");
		snd_soc_dapm_enable_pin(dapm, "Int Spk");
		state = BIT_NO_HEADSET;
	}

	switch_set_state(&tegra_wm8903_hp_switch, state);

	return NOTIFY_OK;
}

static struct notifier_block tegra_wm8903_jack_detect_nb = {
	.notifier_call = tegra_wm8903_jack_notifier,
};
#else
static struct snd_soc_jack_pin tegra_wm8903_hp_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};
#endif

static struct snd_soc_jack_gpio tegra_wm8903_hp_jack_gpio = {
	.name = "headphone detect",
	.report = SND_JACK_HEADPHONE,
	.debounce_time = 150,
	.invert = 1,
};

static struct snd_soc_jack tegra_wm8903_mic_jack;

static struct snd_soc_jack_pin tegra_wm8903_mic_jack_pins[] = {
	{
		.pin = "Mic Jack",
		.mask = SND_JACK_MICROPHONE,
	},
};

static int tegra_wm8903_event_int_spk(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_wm8903_platform_data *pdata = &machine->pdata;

	if (!gpio_is_valid(pdata->gpio_spkr_en))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_spkr_en,
				SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static int tegra_wm8903_event_hp(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_wm8903_platform_data *pdata = &machine->pdata;

	if (!gpio_is_valid(pdata->gpio_hp_mute))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_hp_mute,
				!SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static int tegra_wm8903_event_int_mic(struct snd_soc_dapm_widget *w,
                                        struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_wm8903_platform_data *pdata = &machine->pdata;

	if (machine->dmic_reg) {
		if (SND_SOC_DAPM_EVENT_ON(event))
			regulator_enable(machine->dmic_reg);
		else
			regulator_disable(machine->dmic_reg);
	}

	if (gpio_is_valid(pdata->gpio_int_mic_en))
		gpio_set_value_cansleep(pdata->gpio_int_mic_en,
					SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static const struct snd_soc_dapm_widget tegra_wm8903_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Int Spk", tegra_wm8903_event_int_spk),
	SND_SOC_DAPM_HP("Headphone Jack", tegra_wm8903_event_hp),
	SND_SOC_DAPM_MIC("Int Mic", tegra_wm8903_event_int_mic),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
};

static const struct snd_soc_dapm_route harmony_audio_map[] = {
	{"Headphone Jack", NULL, "HPOUTR"},
	{"Headphone Jack", NULL, "HPOUTL"},
	{"Int Spk", NULL, "ROP"},
	{"Int Spk", NULL, "RON"},
	{"Int Spk", NULL, "LOP"},
	{"Int Spk", NULL, "LON"},
	{"Mic Jack", NULL, "MICBIAS"},
	{"IN1L", NULL, "Mic Jack"},
};

static const struct snd_kcontrol_new tegra_wm8903_controls[] = {
	SOC_DAPM_PIN_SWITCH("Int Spk"),
	SOC_DAPM_PIN_SWITCH("Mic Jack"),
};

static int tegra_wm8903_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_codec *codec = codec_dai->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_card *card = codec->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_wm8903_platform_data *pdata = &machine->pdata;

	if (gpio_is_valid(pdata->gpio_hp_det)) {
		tegra_wm8903_hp_jack_gpio.gpio = pdata->gpio_hp_det;
		snd_soc_jack_new(codec, "Headphone Jack", SND_JACK_HEADPHONE,
						&tegra_wm8903_hp_jack);
#ifdef CONFIG_SWITCH
		snd_soc_jack_notifier_register(&tegra_wm8903_hp_jack,
						&tegra_wm8903_jack_detect_nb);
#else
		snd_soc_jack_add_pins(&tegra_wm8903_hp_jack,
					ARRAY_SIZE(tegra_wm8903_hp_jack_pins),
					tegra_wm8903_hp_jack_pins);
#endif
		snd_soc_jack_add_gpios(&tegra_wm8903_hp_jack,
					1,
					&tegra_wm8903_hp_jack_gpio);
	}

	snd_soc_jack_new(codec, "Mic Jack", SND_JACK_MICROPHONE,
			 &tegra_wm8903_mic_jack);
	snd_soc_jack_add_pins(&tegra_wm8903_mic_jack,
			      ARRAY_SIZE(tegra_wm8903_mic_jack_pins),
			      tegra_wm8903_mic_jack_pins);
	wm8903_mic_detect(codec, &tegra_wm8903_mic_jack, SND_JACK_MICROPHONE,
				SND_JACK_MICROPHONE);

	snd_soc_dapm_force_enable_pin(dapm, "MICBIAS");

	return 0;
}

static int tegra_wm8903_remove(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd = &(card->rtd[0]);
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_codec *codec = codec_dai->codec;

	wm8903_mic_detect(codec, NULL, 0, 0);

	return 0;
}

static struct snd_soc_dai_link tegra_wm8903_dai[] = {
	{
		.name = "WM8903",
		.stream_name = "WM8903 PCM",
		.codec_name = "wm8903.0-001a",
		.platform_name = "tegra20-i2s.0",
		.cpu_dai_name = "tegra20-i2s.0",
		.codec_dai_name = "wm8903-hifi",
		.init = tegra_wm8903_init,
		.ops = &tegra_wm8903_ops,
		.dai_fmt = SND_SOC_DAIFMT_I2S |
			SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBS_CFS,
	},
	{
		.name = "SPDIF",
		.stream_name = "SPDIF PCM",
		.codec_name = "spdif-dit.0",
		.platform_name = "tegra20-i2s.0",
		.cpu_dai_name = "tegra20-spdif",
		.codec_dai_name = "dit-hifi",
		.ops = &tegra_spdif_ops,
	},
};

static int tegra_wm8903_suspend_post(struct snd_soc_card *card)
{
	struct snd_soc_jack_gpio *gpio = &tegra_wm8903_hp_jack_gpio;

	if (gpio_is_valid(gpio->gpio))
		disable_irq(gpio_to_irq(gpio->gpio));

	return 0;
}

static int tegra_wm8903_resume_pre(struct snd_soc_card *card)
{
	struct snd_soc_jack_gpio *gpio = &tegra_wm8903_hp_jack_gpio;
	int val;

	if (gpio_is_valid(gpio->gpio)) {
		val = gpio_get_value(gpio->gpio);
		val = gpio->invert ? !val : val;
		snd_soc_jack_report(gpio->jack, val, gpio->report);
		enable_irq(gpio_to_irq(gpio->gpio));
	}

	return 0;
}

static struct snd_soc_card snd_soc_tegra_wm8903 = {
	.name = "tegra-wm8903",
	.owner = THIS_MODULE,
	.dai_link = tegra_wm8903_dai,
	.num_links = ARRAY_SIZE(tegra_wm8903_dai),

	.remove = tegra_wm8903_remove,

	.controls = tegra_wm8903_controls,
	.num_controls = ARRAY_SIZE(tegra_wm8903_controls),
	.dapm_widgets = tegra_wm8903_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tegra_wm8903_dapm_widgets),
	.fully_routed = true,

	.suspend_post = tegra_wm8903_suspend_post,
	.resume_pre = tegra_wm8903_resume_pre,
};

static int tegra_wm8903_driver_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct snd_soc_card *card = &snd_soc_tegra_wm8903;
	struct tegra_wm8903 *machine;
	struct tegra_wm8903_platform_data *pdata;
	int ret;

	if (!pdev->dev.platform_data && !pdev->dev.of_node) {
		dev_err(&pdev->dev, "No platform data supplied\n");
		return -EINVAL;
	}

	machine = devm_kzalloc(&pdev->dev, sizeof(struct tegra_wm8903),
			       GFP_KERNEL);
	if (!machine) {
		dev_err(&pdev->dev, "Can't allocate tegra_wm8903 struct\n");
		ret = -ENOMEM;
		goto err;
	}
	pdata = &machine->pdata;

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, machine);

	if (pdev->dev.platform_data) {
		memcpy(pdata, card->dev->platform_data, sizeof(*pdata));
	} else if (np) {
		pdata->gpio_spkr_en = of_get_named_gpio(np,
						"nvidia,spkr-en-gpios", 0);
		if (pdata->gpio_spkr_en == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		pdata->gpio_hp_mute = of_get_named_gpio(np,
						"nvidia,hp-mute-gpios", 0);
		if (pdata->gpio_hp_mute == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		pdata->gpio_hp_det = of_get_named_gpio(np,
						"nvidia,hp-det-gpios", 0);
		if (pdata->gpio_hp_det == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		pdata->gpio_int_mic_en = of_get_named_gpio(np,
						"nvidia,int-mic-en-gpios", 0);
		if (pdata->gpio_int_mic_en == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		pdata->gpio_ext_mic_en = of_get_named_gpio(np,
						"nvidia,ext-mic-en-gpios", 0);
		if (pdata->gpio_ext_mic_en == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		machine->dmic_reg = devm_regulator_get(&pdev->dev, "vdd_dmic");
		if (IS_ERR(machine->dmic_reg)) {
			dev_info(&pdev->dev, "No digital mic"
						" regulator found\n");
			machine->dmic_reg = 0;
		}
	}

	if (np) {
		ret = snd_soc_of_parse_card_name(card, "nvidia,model");
		if (ret)
			goto err;

		ret = snd_soc_of_parse_audio_routing(card,
						     "nvidia,audio-routing");
		if (ret)
			goto err;

		tegra_wm8903_dai[0].codec_name = NULL;
		tegra_wm8903_dai[0].codec_of_node = of_parse_phandle(np,
				"nvidia,audio-codec", 0);
		if (!tegra_wm8903_dai[0].codec_of_node) {
			dev_err(&pdev->dev,
				"Property 'nvidia,audio-codec' missing or invalid\n");
			ret = -EINVAL;
			goto err;
		}

		tegra_wm8903_dai[0].cpu_dai_name = NULL;
		tegra_wm8903_dai[0].cpu_of_node = of_parse_phandle(np,
				"nvidia,i2s-controller", 0);
		if (!tegra_wm8903_dai[0].cpu_of_node) {
			dev_err(&pdev->dev,
				"Property 'nvidia,i2s-controller' missing or invalid\n");
			ret = -EINVAL;
			goto err;
		}

		tegra_wm8903_dai[0].platform_name = NULL;
		tegra_wm8903_dai[0].platform_of_node =
					tegra_wm8903_dai[0].cpu_of_node;
	} else {
		card->dapm_routes = harmony_audio_map;
		card->num_dapm_routes = ARRAY_SIZE(harmony_audio_map);
	}

	if (gpio_is_valid(pdata->gpio_spkr_en)) {
		ret = devm_gpio_request_one(&pdev->dev, pdata->gpio_spkr_en,
					    GPIOF_OUT_INIT_LOW, "spkr_en");
		if (ret) {
			dev_err(card->dev, "cannot get spkr_en gpio\n");
			return ret;
		}
	}

	if (gpio_is_valid(pdata->gpio_hp_mute)) {
		ret = devm_gpio_request_one(&pdev->dev, pdata->gpio_hp_mute,
					    GPIOF_OUT_INIT_HIGH, "hp_mute");
		if (ret) {
			dev_err(card->dev, "cannot get hp_mute gpio\n");
			return ret;
		}
	}

	if (gpio_is_valid(pdata->gpio_int_mic_en)) {
		/* Disable int mic; enable signal is active-high */
		ret = devm_gpio_request_one(&pdev->dev, pdata->gpio_int_mic_en,
					    GPIOF_OUT_INIT_LOW, "int_mic_en");
		if (ret) {
			dev_err(card->dev, "cannot get int_mic_en gpio\n");
			return ret;
		}
	}

	if (gpio_is_valid(pdata->gpio_ext_mic_en)) {
		/* Enable ext mic; enable signal is active-low */
		ret = devm_gpio_request_one(&pdev->dev, pdata->gpio_ext_mic_en,
					    GPIOF_OUT_INIT_LOW, "ext_mic_en");
		if (ret) {
			dev_err(card->dev, "cannot get ext_mic_en gpio\n");
			return ret;
		}
	}

	ret = tegra_asoc_utils_init(&machine->util_data, &pdev->dev);
	if (ret)
		goto err;

#ifdef CONFIG_SWITCH
	/* Add h2w swith class support */
	ret = switch_dev_register(&tegra_wm8903_hp_switch);
	if (ret < 0)
		goto err_fini_utils;
#endif

	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",
			ret);
		goto err_unregister_switch;
	}

	return 0;

err_unregister_switch:
#ifdef CONFIG_SWITCH
	switch_dev_unregister(&tegra_wm8903_hp_switch);
#endif
err_fini_utils:
	tegra_asoc_utils_fini(&machine->util_data);
err:
	return ret;
}

static int tegra_wm8903_driver_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);

	snd_soc_jack_free_gpios(&tegra_wm8903_hp_jack, 1,
				&tegra_wm8903_hp_jack_gpio);

	snd_soc_unregister_card(card);

	tegra_asoc_utils_fini(&machine->util_data);

#ifdef CONFIG_SWITCH
	switch_dev_unregister(&tegra_wm8903_hp_switch);
#endif

	return 0;
}

static const struct of_device_id tegra_wm8903_of_match[] = {
	{ .compatible = "nvidia,tegra-audio-wm8903", },
	{},
};

static struct platform_driver tegra_wm8903_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = tegra_wm8903_of_match,
	},
	.probe = tegra_wm8903_driver_probe,
	.remove = tegra_wm8903_driver_remove,
};
module_platform_driver(tegra_wm8903_driver);

MODULE_AUTHOR("Stephen Warren <swarren@nvidia.com>");
MODULE_DESCRIPTION("Tegra+WM8903 machine ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
MODULE_DEVICE_TABLE(of, tegra_wm8903_of_match);
