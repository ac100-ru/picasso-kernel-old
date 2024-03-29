/*
 * drivers/usb/otg/tegra-otg.c
 *
 * OTG transceiver driver for Tegra UTMI phy
 *
 * Copyright (C) 2010-2012 NVIDIA CORPORATION. All rights reserved.
 * Copyright (C) 2010 Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/otg.h>
#include <linux/usb/gadget.h>
#include <linux/platform_device.h>
#include <linux/platform_data/tegra_usb.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/dma-mapping.h>
#include <linux/of_device.h>

#define USB_PHY_WAKEUP		0x408
#define  USB_ID_INT_EN		(1 << 0)
#define  USB_ID_INT_STATUS	(1 << 1)
#define  USB_ID_STATUS		(1 << 2)
#define  USB_ID_PIN_WAKEUP_EN	(1 << 6)
#define  USB_VBUS_WAKEUP_EN	(1 << 30)
#define  USB_VBUS_INT_EN	(1 << 8)
#define  USB_VBUS_INT_STATUS	(1 << 9)
#define  USB_VBUS_STATUS	(1 << 10)
#define  USB_INT_EN		(USB_VBUS_INT_EN | USB_ID_INT_EN | \
						USB_VBUS_WAKEUP_EN | USB_ID_PIN_WAKEUP_EN)

#ifdef DEBUG
#define DBG(stuff...)	pr_info("tegra-otg: " stuff)
#else
#define DBG(stuff...)	do {} while (0)
#endif

struct tegra_otg_data {
	struct usb_phy phy;
	unsigned long int_status;
	spinlock_t lock;
	struct mutex irq_work_mutex;
	void __iomem *regs;
	struct clk *clk;
	int irq;
	struct platform_device *pdev;
	struct work_struct work;
	unsigned int intr_reg_data;
	bool clk_prepare_enabled;
	bool interrupt_mode;
	bool suspended;
};

static struct tegra_otg_data *tegra_clone;

static inline unsigned long otg_readl(struct tegra_otg_data *tegra,
				      unsigned int offset)
{
	return readl(tegra->regs + offset);
}

static inline void otg_writel(struct tegra_otg_data *tegra, unsigned long val,
			      unsigned int offset)
{
	writel(val, tegra->regs + offset);
}

static const char *tegra_state_name(enum usb_otg_state state)
{
	switch (state) {
		case OTG_STATE_A_HOST:
			return "HOST";
		case OTG_STATE_B_PERIPHERAL:
			return "PERIPHERAL";
		case OTG_STATE_A_SUSPEND:
			return "SUSPEND";
		case OTG_STATE_UNDEFINED:
			return "UNDEFINED";
		default:
			return "INVALID";
	}
}

static unsigned long enable_interrupt(struct tegra_otg_data *tegra, bool en)
{
	unsigned long val;

	clk_prepare_enable(tegra->clk);
	val = otg_readl(tegra, USB_PHY_WAKEUP);
	if (en)
		val |= USB_VBUS_INT_EN | USB_VBUS_WAKEUP_EN | USB_ID_PIN_WAKEUP_EN;
	else
		val &= ~USB_INT_EN;
	otg_writel(tegra, val, USB_PHY_WAKEUP);
	/* Add delay to make sure register is updated */
	udelay(1);
	clk_disable_unprepare(tegra->clk);

	return val;
}

static void tegra_start_host(struct tegra_otg_data *tegra)
{
	struct tegra_ehci_platform_data *pdata = tegra->phy.dev->platform_data;
	struct platform_device *pdev;
	int val;
	DBG("%s(%d) Begin\n", __func__, __LINE__);

	if (tegra->pdev)
		return ;

	/* prepare device structure for registering host*/
	pdev = platform_device_alloc("tegra-ehci", 0);
	if (!pdev)
		return ;

	val = platform_device_add_resources(pdev, tegra->pdev->resource,
					    tegra->pdev->num_resources);
	if (val)
		goto error;

	pdev->dev.dma_mask = tegra->phy.dev->dma_mask;
	pdev->dev.coherent_dma_mask = tegra->phy.dev->coherent_dma_mask;
	pdev->dev.of_node = tegra->phy.dev->of_node;

	pdev->dev.platform_data = pdata;
	val = platform_device_add(pdev);
	if (val)
		goto error;

	tegra->pdev = pdev;
	DBG("%s(%d) End\n", __func__, __LINE__);
	return ;

error:
	pr_err("%s: failed to add the host controller device\n", __func__);
	platform_device_put(pdev);
	tegra->pdev = NULL;
}

static void tegra_stop_host(struct tegra_otg_data *tegra)
{
	struct platform_device *pdev = tegra->pdev;

	DBG("%s(%d) Begin\n", __func__, __LINE__);

	if (pdev) {
		/* unregister host from otg */
		pdev->dev.platform_data = NULL;
		platform_device_unregister(pdev);
		tegra->pdev = NULL;
	}

	DBG("%s(%d) End\n", __func__, __LINE__);
}

static void tegra_otg_notify_event(struct usb_phy *phy,
					enum usb_phy_events event)
{
	phy->last_event = event;
	atomic_notifier_call_chain(&phy->notifier, event, NULL);
}

static void tegra_change_otg_state(struct tegra_otg_data *tegra,
				enum usb_otg_state to)
{
	struct usb_phy *phy = &tegra->phy;
	enum usb_otg_state from = phy->state;

	if (!tegra->interrupt_mode) {
		DBG("OTG: Vbus detection is disabled");
		return;
	}

	DBG("%s(%d) requested otg state %s-->%s\n", __func__,
		__LINE__, tegra_state_name(from), tegra_state_name(to));

	if (to != OTG_STATE_UNDEFINED && from != to) {
		phy->state = to;
		dev_info(tegra->phy.dev, "%s --> %s\n", tegra_state_name(from),
					      tegra_state_name(to));

		if (from == OTG_STATE_A_SUSPEND) {
			if (to == OTG_STATE_B_PERIPHERAL && phy->otg->gadget) {
				usb_gadget_vbus_connect(phy->otg->gadget);
				tegra_otg_notify_event(phy, USB_EVENT_VBUS);
			}
			else if (to == OTG_STATE_A_HOST) {
				tegra_start_host(tegra);
				tegra_otg_notify_event(phy, USB_EVENT_ID);
			}
		} else if (from == OTG_STATE_A_HOST) {
			if (to == OTG_STATE_A_SUSPEND) {
				tegra_stop_host(tegra);
				tegra_otg_notify_event(phy, USB_EVENT_NONE);
			}
		} else if (from == OTG_STATE_B_PERIPHERAL && phy->otg->gadget) {
			if (to == OTG_STATE_A_SUSPEND) {
				usb_gadget_vbus_disconnect(phy->otg->gadget);
				tegra_otg_notify_event(phy, USB_EVENT_NONE);
			}
		}
	}
}

static void irq_work(struct work_struct *work)
{
	struct tegra_otg_data *tegra =
		container_of(work, struct tegra_otg_data, work);
	struct usb_phy *phy = &tegra->phy;
	enum usb_otg_state from;
	enum usb_otg_state to = OTG_STATE_UNDEFINED;
	unsigned long flags;
	unsigned long status;

	mutex_lock(&tegra->irq_work_mutex);

	spin_lock_irqsave(&tegra->lock, flags);
	from = phy->state;
	status = tegra->int_status;

	/* Debug prints */
	DBG("%s(%d) status = 0x%lx\n", __func__, __LINE__, status);
	if ((status & USB_ID_INT_STATUS) &&
			(status & USB_VBUS_INT_STATUS))
		DBG("%s(%d) got vbus & id interrupt\n", __func__, __LINE__);
	else {
		if (status & USB_ID_INT_STATUS)
			DBG("%s(%d) got id interrupt\n", __func__, __LINE__);
		if (status & USB_VBUS_INT_STATUS)
			DBG("%s(%d) got vbus interrupt\n", __func__, __LINE__);
	}

	if (!(status & USB_ID_STATUS) && (status & USB_ID_INT_EN))
		to = OTG_STATE_A_HOST;
	else if (status & USB_VBUS_STATUS && from != OTG_STATE_A_HOST)
		to = OTG_STATE_B_PERIPHERAL;
	else
		to = OTG_STATE_A_SUSPEND;

	spin_unlock_irqrestore(&tegra->lock, flags);
	tegra_change_otg_state(tegra, to);
	mutex_unlock(&tegra->irq_work_mutex);
}

static irqreturn_t tegra_otg_irq(int irq, void *data)
{
	struct tegra_otg_data *tegra = data;
	unsigned long flags;
	unsigned long val;

	spin_lock_irqsave(&tegra->lock, flags);
	val = otg_readl(tegra, USB_PHY_WAKEUP);
	DBG("%s(%d) interrupt val = 0x%lx\n", __func__, __LINE__, val);

	if (val & (USB_VBUS_INT_EN | USB_ID_INT_EN)) {
		DBG("%s(%d) PHY_WAKEUP = 0x%lx\n", __func__, __LINE__, val);
		otg_writel(tegra, val, USB_PHY_WAKEUP);
		if ((val & USB_ID_INT_STATUS) || (val & USB_VBUS_INT_STATUS)) {
			tegra->int_status = val;
			schedule_work(&tegra->work);
		}
	}
	spin_unlock_irqrestore(&tegra->lock, flags);

	return IRQ_HANDLED;
}


static int tegra_otg_set_peripheral(struct usb_otg *otg,
				struct usb_gadget *gadget)
{
	struct tegra_otg_data *tegra;
	unsigned long val;
	DBG("%s(%d) BEGIN\n", __func__, __LINE__);

	tegra = container_of(otg->phy, struct tegra_otg_data, phy);
	otg->gadget = gadget;

	val = enable_interrupt(tegra, true);

	if ((val & USB_ID_STATUS) && (val & USB_VBUS_STATUS))
		val |= USB_VBUS_INT_STATUS;
	else if (!(val & USB_ID_STATUS))
		val &= ~USB_ID_INT_STATUS;
	else
		val &= ~(USB_ID_INT_STATUS | USB_VBUS_INT_STATUS);

	if ((val & USB_ID_INT_STATUS) || (val & USB_VBUS_INT_STATUS)) {
		tegra->int_status = val;
		schedule_work(&tegra->work);
	}

	DBG("%s(%d) END\n", __func__, __LINE__);
	return 0;
}

static int tegra_otg_set_host(struct usb_otg *otg,
				struct usb_bus *host)
{
	struct tegra_otg_data *tegra;
	unsigned long val;
	DBG("%s(%d) BEGIN\n", __func__, __LINE__);

	tegra = container_of(otg->phy, struct tegra_otg_data, phy);
	otg->host = host;

	clk_prepare_enable(tegra->clk);
	val = otg_readl(tegra, USB_PHY_WAKEUP);
	val &= ~(USB_VBUS_INT_STATUS | USB_ID_INT_STATUS);
	val |= (USB_ID_INT_EN | USB_ID_PIN_WAKEUP_EN);
	otg_writel(tegra, val, USB_PHY_WAKEUP);
	clk_disable_unprepare(tegra->clk);

	DBG("%s(%d) END\n", __func__, __LINE__);
	return 0;
}

static int tegra_otg_set_power(struct usb_phy *phy, unsigned mA)
{
	return 0;
}

static int tegra_otg_set_suspend(struct usb_phy *phy, int suspend)
{
	return 0;
}

static ssize_t show_host_en(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct tegra_otg_data *tegra = platform_get_drvdata(pdev);

	*buf = tegra->interrupt_mode ? '0': '1';
	strcat(buf, "\n");
	return strlen(buf);
}

static ssize_t store_host_en(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct tegra_otg_data *tegra = platform_get_drvdata(pdev);
	unsigned int host;

	if (sscanf(buf, "%d", &host) != 1 || host < 0 || host > 1)
		return -EINVAL;

	if (host) {
		enable_interrupt(tegra, false);
		tegra_change_otg_state(tegra, OTG_STATE_A_SUSPEND);
		tegra_change_otg_state(tegra, OTG_STATE_A_HOST);
		tegra->interrupt_mode = false;
	} else {
		tegra->interrupt_mode = true;
		tegra_change_otg_state(tegra, OTG_STATE_A_SUSPEND);
		enable_interrupt(tegra, true);
	}

	return count;
}

static DEVICE_ATTR(enable_host, 0644, show_host_en, store_host_en);

static u64 tegra_ehci_dma_mask = DMA_BIT_MASK(32);

static int tegra_otg_probe(struct platform_device *pdev)
{
	struct tegra_otg_data *tegra;
	struct resource *res;
	struct tegra_ehci_platform_data *pdata = dev_get_platdata(&pdev->dev);
	int err;

	if (!pdata) {
		dev_err(&pdev->dev, "Platform data missing\n");
		return -EINVAL;
	}

	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &tegra_ehci_dma_mask;

	if (!pdev->dev.coherent_dma_mask)
		pdev->dev.coherent_dma_mask = tegra_ehci_dma_mask;

	tegra = devm_kzalloc(&pdev->dev, sizeof(*tegra), GFP_KERNEL);
	if (!tegra)
		return -ENOMEM;

	tegra->phy.otg = devm_kzalloc(&pdev->dev,
				      sizeof(struct usb_otg), GFP_KERNEL);
	if (!tegra->phy.otg)
		return -ENOMEM;

	tegra->phy.dev = &pdev->dev;
	tegra->phy.dev->platform_data = pdata;
	tegra->phy.label = "tegra-otg";
	tegra->phy.state = OTG_STATE_UNDEFINED;
	tegra->phy.otg->set_host = tegra_otg_set_host;
	tegra->phy.otg->set_peripheral = tegra_otg_set_peripheral;
	tegra->phy.otg->phy = &tegra->phy;
	tegra->phy.set_suspend = tegra_otg_set_suspend;
	tegra->phy.set_power = tegra_otg_set_power;
	spin_lock_init(&tegra->lock);
	mutex_init(&tegra->irq_work_mutex);

	platform_set_drvdata(pdev, tegra);
	tegra_clone = tegra;
	tegra->interrupt_mode = true;
	tegra->suspended = false;

	tegra->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(tegra->clk)) {
		dev_err(&pdev->dev, "Can't get otg clock\n");
		err = PTR_ERR(tegra->clk);
		goto err_probe;
	}

	err = clk_prepare_enable(tegra->clk);
	if (err)
		goto err_probe;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "Failed to get I/O memory\n");
		err = -ENXIO;
		goto err_probe;
	}
	tegra->regs = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!tegra->regs) {
		err = -EADDRNOTAVAIL;
		goto err_probe;
	}

	tegra->phy.state = OTG_STATE_A_SUSPEND;

	err = usb_add_phy(&tegra->phy, USB_PHY_TYPE_USB2);
	if (err) {
		dev_err(&pdev->dev, "can't register transceiver (%d)\n", err);
		goto err_probe;
	}

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res) {
		dev_err(&pdev->dev, "Failed to get IRQ\n");
		err = -ENXIO;
		goto err_phy;
	}
	tegra->irq = res->start;

	INIT_WORK(&tegra->work, irq_work);

	err = devm_request_threaded_irq(&pdev->dev, tegra->irq, tegra_otg_irq,
					NULL, IRQF_SHARED | IRQF_TRIGGER_HIGH,
					"tegra-otg", tegra);
	if (err) {
		dev_err(&pdev->dev, "Failed to register IRQ\n");
		goto err_phy;
	}

	err = enable_irq_wake(tegra->irq);
	if (err < 0)
		dev_warn(&pdev->dev, "Couldn't enable USB otg wakeup\n");

	err = device_create_file(&pdev->dev, &dev_attr_enable_host);
	if (err) {
		dev_warn(&pdev->dev, "Can't register sysfs attribute\n");
		goto err_phy;
	}

	clk_disable_unprepare(tegra->clk);

	dev_info(&pdev->dev, "otg transceiver registered\n");

	return 0;

err_phy:
	usb_remove_phy(&tegra->phy);
err_probe:
	mutex_destroy(&tegra->irq_work_mutex);

	return err;
}

static int tegra_otg_remove(struct platform_device *pdev)
{
	struct tegra_otg_data *tegra = platform_get_drvdata(pdev);

	usb_remove_phy(&tegra->phy);
	mutex_destroy(&tegra->irq_work_mutex);

	return 0;
}

#ifdef CONFIG_PM
static int tegra_otg_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct tegra_otg_data *tegra = platform_get_drvdata(pdev);
	struct usb_phy *phy = &tegra->phy;
	int val;

	mutex_lock(&tegra->irq_work_mutex);
	DBG("%s(%d) BEGIN state : %s\n", __func__, __LINE__,
					tegra_state_name(phy->state));

	clk_prepare_enable(tegra->clk);
	val = otg_readl(tegra, USB_PHY_WAKEUP);
	val &= ~(USB_ID_INT_EN | USB_VBUS_INT_EN);
	otg_writel(tegra, val, USB_PHY_WAKEUP);
	clk_disable_unprepare(tegra->clk);

	/* Suspend peripheral mode, host mode is taken care by host driver */
	if (phy->state == OTG_STATE_B_PERIPHERAL)
		tegra_change_otg_state(tegra, OTG_STATE_A_SUSPEND);

	tegra->suspended = true;

	DBG("%s(%d) END\n", __func__, __LINE__);
	mutex_unlock(&tegra->irq_work_mutex);
	return 0;
}

static void tegra_otg_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct tegra_otg_data *tegra = platform_get_drvdata(pdev);
	int val;
	unsigned long flags;
	DBG("%s(%d) BEGIN\n", __func__, __LINE__);

	mutex_lock(&tegra->irq_work_mutex);
	if (!tegra->suspended) {
		mutex_unlock(&tegra->irq_work_mutex);
		return;
	}

	/* Clear pending interrupts */
	clk_prepare_enable(tegra->clk);
	val = otg_readl(tegra, USB_PHY_WAKEUP);
	otg_writel(tegra, val, USB_PHY_WAKEUP);
	DBG("%s(%d) PHY WAKEUP register : 0x%x\n", __func__, __LINE__, val);
	clk_disable_unprepare(tegra->clk);

	/* Enable interrupt and call work to set to appropriate state */
	spin_lock_irqsave(&tegra->lock, flags);
	tegra->int_status = val | USB_VBUS_INT_EN | USB_VBUS_WAKEUP_EN |
		USB_ID_PIN_WAKEUP_EN;

	spin_unlock_irqrestore(&tegra->lock, flags);
	schedule_work(&tegra->work);
	enable_interrupt(tegra, true);

	tegra->suspended = false;

	DBG("%s(%d) END\n", __func__, __LINE__);
	mutex_unlock(&tegra->irq_work_mutex);
}

static const struct dev_pm_ops tegra_otg_pm_ops = {
	.complete = tegra_otg_resume,
	.suspend = tegra_otg_suspend,
};
#endif

static const struct of_device_id tegra_otg_of_match[] = {
	{ .compatible = "nvidia,tegra20-otg", },
	{ },
};
MODULE_DEVICE_TABLE(of, tegra_otg_of_match);

static struct platform_driver tegra_otg_driver = {
	.driver = {
		.name  = "tegra-otg",
		.of_match_table = of_match_ptr(tegra_otg_of_match),
#ifdef CONFIG_PM
		.pm    = &tegra_otg_pm_ops,
#endif
	},
	.remove  = tegra_otg_remove,
	.probe   = tegra_otg_probe,
};

module_platform_driver(tegra_otg_driver);
