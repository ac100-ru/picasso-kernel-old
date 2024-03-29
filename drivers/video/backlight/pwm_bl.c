/*
 * linux/drivers/video/backlight/pwm_bl.c
 *
 * simple PWM based backlight control, board code has to setup
 * 1) pin configuration so PWM waveforms can output
 * 2) platform_data being correctly configured
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/pwm.h>
#include <linux/pwm_backlight.h>
#include <linux/slab.h>

struct pwm_bl_data {
	struct pwm_device	*pwm;
	struct device		*dev;
	unsigned int		period;
	unsigned int		lth_brightness;
	unsigned int		*levels;
	bool			enabled;
	struct power_seq_set	power_seqs;
	struct power_seq	*power_on_seq;
	struct power_seq	*power_off_seq;

	/* Legacy callbacks */
	int			(*notify)(struct device *,
					  int brightness);
	void			(*notify_after)(struct device *,
					int brightness);
	int			(*check_fb)(struct device *, struct fb_info *);
	void			(*exit)(struct device *);
};

static void pwm_backlight_on(struct backlight_device *bl)
{
	struct pwm_bl_data *pb = dev_get_drvdata(&bl->dev);
	int ret;

	if (pb->enabled)
		return;

	if (pb->power_on_seq) {
		ret = power_seq_run(pb->power_on_seq);
		if (ret < 0) {
			dev_err(&bl->dev, "cannot run power on sequence\n");
			return;
		}
	} else {
		/* legacy framework */
		pwm_enable(pb->pwm);
	}

	pb->enabled = true;
}

static void pwm_backlight_off(struct backlight_device *bl)
{
	struct pwm_bl_data *pb = dev_get_drvdata(&bl->dev);
	int ret;

	if (!pb->enabled)
		return;

	if (pb->power_off_seq) {
		ret = power_seq_run(pb->power_off_seq);
		if (ret < 0) {
			dev_err(&bl->dev, "cannot run power off sequence\n");
			return;
		}
	} else {
		/* legacy framework */
		pwm_config(pb->pwm, 0, pb->period);
		pwm_disable(pb->pwm);
	}

	pb->enabled = false;
}

static int pwm_backlight_update_status(struct backlight_device *bl)
{
	struct pwm_bl_data *pb = dev_get_drvdata(&bl->dev);
	int brightness = bl->props.brightness;
	int max = bl->props.max_brightness;

	if (bl->props.power != FB_BLANK_UNBLANK)
		brightness = 0;

	if (bl->props.fb_blank != FB_BLANK_UNBLANK)
		brightness = 0;

	if (pb->notify)
		brightness = pb->notify(pb->dev, brightness);

	if (brightness == 0) {
		pwm_backlight_off(bl);
	} else {
		int duty_cycle;

		if (pb->levels) {
			duty_cycle = pb->levels[brightness];
			max = pb->levels[max];
		} else {
			duty_cycle = brightness;
		}

		duty_cycle = pb->lth_brightness +
		     (duty_cycle * (pb->period - pb->lth_brightness) / max);
		pwm_config(pb->pwm, duty_cycle, pb->period);
		pwm_backlight_on(bl);
	}

	if (pb->notify_after)
		pb->notify_after(pb->dev, brightness);

	return 0;
}

static int pwm_backlight_get_brightness(struct backlight_device *bl)
{
	return bl->props.brightness;
}

static int pwm_backlight_check_fb(struct backlight_device *bl,
				  struct fb_info *info)
{
	struct pwm_bl_data *pb = dev_get_drvdata(&bl->dev);

	return !pb->check_fb || pb->check_fb(pb->dev, info);
}

static const struct backlight_ops pwm_backlight_ops = {
	.update_status	= pwm_backlight_update_status,
	.get_brightness	= pwm_backlight_get_brightness,
	.check_fb	= pwm_backlight_check_fb,
};

#ifdef CONFIG_OF
static int pwm_backlight_parse_dt(struct device *dev,
				  struct platform_pwm_backlight_data *data)
{
	struct device_node *node = dev->of_node;
	struct property *prop;
	int length;
	u32 value;
	int ret;

	if (!node)
		return -ENODEV;

	memset(data, 0, sizeof(*data));

	/* determine the number of brightness levels */
	prop = of_find_property(node, "brightness-levels", &length);
	if (!prop)
		return -EINVAL;

	data->max_brightness = length / sizeof(u32);

	/* read brightness levels from DT property */
	if (data->max_brightness > 0) {
		size_t size = sizeof(*data->levels) * data->max_brightness;

		data->levels = devm_kzalloc(dev, size, GFP_KERNEL);
		if (!data->levels)
			return -ENOMEM;

		ret = of_property_read_u32_array(node, "brightness-levels",
						 data->levels,
						 data->max_brightness);
		if (ret < 0)
			return ret;

		ret = of_property_read_u32(node, "default-brightness-level",
					   &value);
		if (ret < 0)
			return ret;

		if (value >= data->max_brightness) {
			dev_warn(dev, "invalid default brightness level: %u, using %u\n",
				 value, data->max_brightness - 1);
			value = data->max_brightness - 1;
		}

		data->dft_brightness = value;
		data->max_brightness--;
	}

	/* read power sequences */
	data->power_seqs = devm_of_parse_power_seq_set(dev);
	if (IS_ERR(data->power_seqs))
		return PTR_ERR(data->power_seqs);

	return 0;
}

static struct of_device_id pwm_backlight_of_match[] = {
	{ .compatible = "pwm-backlight" },
	{ }
};

MODULE_DEVICE_TABLE(of, pwm_backlight_of_match);
#else
static int pwm_backlight_parse_dt(struct device *dev,
				  struct platform_pwm_backlight_data *data)
{
	return -ENODEV;
}
#endif

static int pwm_backlight_probe(struct platform_device *pdev)
{
	struct platform_pwm_backlight_data *data = pdev->dev.platform_data;
	struct platform_pwm_backlight_data defdata;
	struct power_seq_resource *res;
	struct backlight_properties props;
	struct backlight_device *bl;
	struct pwm_bl_data *pb;
	unsigned int max;
	int ret;

	if (!data) {
		ret = pwm_backlight_parse_dt(&pdev->dev, &defdata);
		if (ret == -EPROBE_DEFER) {
			return ret;
		} else if (ret < 0) {
			dev_err(&pdev->dev, "failed to find platform data\n");
			return ret;
		}

		data = &defdata;
	}

	if (data->init) {
		ret = data->init(&pdev->dev);
		if (ret < 0)
			return ret;
	}

	pb = devm_kzalloc(&pdev->dev, sizeof(*pb), GFP_KERNEL);
	if (!pb) {
		dev_err(&pdev->dev, "no memory for state\n");
		ret = -ENOMEM;
		goto err_alloc;
	}

	if (data->power_seqs) {
		/* use power sequences */
		struct power_seq_set *seqs = &pb->power_seqs;

		power_seq_set_init(seqs, &pdev->dev);
		power_seq_set_add_sequences(seqs, data->power_seqs);

		/* Check that the required sequences are here */
		pb->power_on_seq = power_seq_lookup(seqs, "power-on");
		if (!pb->power_on_seq) {
			dev_err(&pdev->dev, "missing power-on sequence\n");
			return -EINVAL;
		}
		pb->power_off_seq = power_seq_lookup(seqs, "power-off");
		if (!pb->power_off_seq) {
			dev_err(&pdev->dev, "missing power-off sequence\n");
			return -EINVAL;
		}

		/* we must have exactly one PWM resource for this driver */
		power_seq_for_each_resource(res, seqs) {
			if (res->type != POWER_SEQ_PWM)
				continue;
			if (pb->pwm) {
				dev_err(&pdev->dev, "more than one PWM used\n");
				return -EINVAL;
			}
			/* keep the pwm at hand */
			pb->pwm = res->pwm.pwm;
		}
		/* from here we should have a PWM */
		if (!pb->pwm) {
			dev_err(&pdev->dev, "no PWM defined!\n");
			return -EINVAL;
		}
	} else {
		/* use legacy interface */
		pb->pwm = devm_pwm_get(&pdev->dev, NULL);
		if (IS_ERR(pb->pwm)) {
			dev_err(&pdev->dev,
				"unable to request PWM, trying legacy API\n");

			pb->pwm = pwm_request(data->pwm_id, "pwm-backlight");
			if (IS_ERR(pb->pwm)) {
				dev_err(&pdev->dev,
					"unable to request legacy PWM\n");
				ret = PTR_ERR(pb->pwm);
				goto err_alloc;
			}
		}

		dev_dbg(&pdev->dev, "got pwm for backlight\n");

		/*
		* The DT case will set the pwm_period_ns field to 0 and store
		* the period, parsed from the DT, in the PWM device. For the
		* non-DT case, set the period from platform data.
		*/
		if (data->pwm_period_ns > 0)
			pwm_set_period(pb->pwm, data->pwm_period_ns);
	}

	if (data->levels) {
		max = data->levels[data->max_brightness];
		pb->levels = data->levels;
	} else
		max = data->max_brightness;

	pb->notify = data->notify;
	pb->notify_after = data->notify_after;
	pb->check_fb = data->check_fb;
	pb->exit = data->exit;
	pb->dev = &pdev->dev;

	pb->period = pwm_get_period(pb->pwm);
	pb->lth_brightness = data->lth_brightness * (pb->period / max);

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = data->max_brightness;
	bl = backlight_device_register(dev_name(&pdev->dev), &pdev->dev, pb,
				       &pwm_backlight_ops, &props);
	if (IS_ERR(bl)) {
		dev_err(&pdev->dev, "failed to register backlight\n");
		ret = PTR_ERR(bl);
		goto err_alloc;
	}

	bl->props.brightness = data->dft_brightness;
	backlight_update_status(bl);

	platform_set_drvdata(pdev, bl);
	return 0;

err_alloc:
	if (data->exit)
		data->exit(&pdev->dev);
	return ret;
}

static int pwm_backlight_remove(struct platform_device *pdev)
{
	struct backlight_device *bl = platform_get_drvdata(pdev);
	struct pwm_bl_data *pb = dev_get_drvdata(&bl->dev);

	backlight_device_unregister(bl);
	pwm_backlight_off(bl);
	if (pb->exit)
		pb->exit(&pdev->dev);
	return 0;
}

#ifdef CONFIG_PM
static int pwm_backlight_suspend(struct device *dev)
{
	struct backlight_device *bl = dev_get_drvdata(dev);
	struct pwm_bl_data *pb = dev_get_drvdata(&bl->dev);

	if (pb->notify)
		pb->notify(pb->dev, 0);
	pwm_backlight_off(bl);
	if (pb->notify_after)
		pb->notify_after(pb->dev, 0);
	return 0;
}

static int pwm_backlight_resume(struct device *dev)
{
	struct backlight_device *bl = dev_get_drvdata(dev);

	backlight_update_status(bl);
	return 0;
}

static SIMPLE_DEV_PM_OPS(pwm_backlight_pm_ops, pwm_backlight_suspend,
			 pwm_backlight_resume);

#endif

static struct platform_driver pwm_backlight_driver = {
	.driver		= {
		.name		= "pwm-backlight",
		.owner		= THIS_MODULE,
#ifdef CONFIG_PM
		.pm		= &pwm_backlight_pm_ops,
#endif
		.of_match_table	= of_match_ptr(pwm_backlight_of_match),
	},
	.probe		= pwm_backlight_probe,
	.remove		= pwm_backlight_remove,
};

module_platform_driver(pwm_backlight_driver);

MODULE_DESCRIPTION("PWM based Backlight Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:pwm-backlight");

