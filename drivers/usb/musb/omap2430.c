/*
 * Copyright (C) 2005-2007 by Texas Instruments
 * Some code has been taken from tusb6010.c
 * Copyrights for that are attributable to:
 * Copyright (C) 2006 Nokia Corporation
 * Tony Lindgren <tony@atomide.com>
 *
 * This file is part of the Inventra Controller Driver for Linux.
 *
 * The Inventra Controller Driver for Linux is free software; you
 * can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 2 as published by the Free Software
 * Foundation.
 *
 * The Inventra Controller Driver for Linux is distributed in
 * the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with The Inventra Controller Driver for Linux ; if not,
 * write to the Free Software Foundation, Inc., 59 Temple Place,
 * Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/pm_runtime.h>
#include <linux/err.h>
#ifdef CONFIG_USB_SAMSUNG_OMAP_NORPM
#include <plat/omap_device.h>
#endif

#include "musb_core.h"
#include "omap2430.h"

struct omap2430_glue {
	struct device		*dev;
	struct platform_device	*musb;
};
#define glue_to_musb(g)		platform_get_drvdata(g->musb)

static struct timer_list musb_idle_timer;

static void musb_do_idle(unsigned long _musb)
{
	struct musb	*musb = (void *)_musb;
	unsigned long	flags;
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	u8	power;
#endif
	u8	devctl;

	spin_lock_irqsave(&musb->lock, flags);

	switch (musb->xceiv->state) {
	case OTG_STATE_A_WAIT_BCON:

		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		if (devctl & MUSB_DEVCTL_BDEVICE) {
			musb->xceiv->state = OTG_STATE_B_IDLE;
			MUSB_DEV_MODE(musb);
		} else {
			musb->xceiv->state = OTG_STATE_A_IDLE;
			MUSB_HST_MODE(musb);
		}
		break;
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	case OTG_STATE_A_SUSPEND:
		/* finish RESUME signaling? */
		if (musb->port1_status & MUSB_PORT_STAT_RESUME) {
			power = musb_readb(musb->mregs, MUSB_POWER);
			power &= ~MUSB_POWER_RESUME;
			dev_dbg(musb->controller, "root port resume stopped, power %02x\n", power);
			musb_writeb(musb->mregs, MUSB_POWER, power);
			musb->is_active = 1;
			musb->port1_status &= ~(USB_PORT_STAT_SUSPEND
						| MUSB_PORT_STAT_RESUME);
			musb->port1_status |= USB_PORT_STAT_C_SUSPEND << 16;
			usb_hcd_poll_rh_status(musb_to_hcd(musb));
			/* NOTE: it might really be A_WAIT_BCON ... */
			musb->xceiv->state = OTG_STATE_A_HOST;
		}
		break;
#endif
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	case OTG_STATE_A_HOST:
		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		if (devctl &  MUSB_DEVCTL_BDEVICE)
			musb->xceiv->state = OTG_STATE_B_IDLE;
		else
			musb->xceiv->state = OTG_STATE_A_WAIT_BCON;
#endif
	default:
		break;
	}
	spin_unlock_irqrestore(&musb->lock, flags);
}


static void omap2430_musb_try_idle(struct musb *musb, unsigned long timeout)
{
	unsigned long		default_timeout = jiffies + msecs_to_jiffies(3);
	static unsigned long	last_timer;

	if (timeout == 0)
		timeout = default_timeout;

	/* Never idle if active, or when VBUS timeout is not set as host */
	if (musb->is_active || ((musb->a_wait_bcon == 0)
			&& (musb->xceiv->state == OTG_STATE_A_WAIT_BCON))) {
		dev_dbg(musb->controller, "%s active, deleting timer\n",
			otg_state_string(musb->xceiv->state));
		del_timer(&musb_idle_timer);
		last_timer = jiffies;
		return;
	}

	if (time_after(last_timer, timeout)) {
		if (!timer_pending(&musb_idle_timer))
			last_timer = timeout;
		else {
			dev_dbg(musb->controller, "Longer idle timer already pending, ignoring\n");
			return;
		}
	}
	last_timer = timeout;

	dev_dbg(musb->controller, "%s inactive, for idle timer for %lu ms\n",
		otg_state_string(musb->xceiv->state),
		(unsigned long)jiffies_to_msecs(timeout - jiffies));
	mod_timer(&musb_idle_timer, timeout);
}

static void omap2430_musb_set_vbus(struct musb *musb, int is_on)
{
	u8		devctl;
	unsigned long timeout = 1000;
	int ret = 1;
	/* HDRC controls CPEN, but beware current surges during device
	 * connect.  They can trigger transient overcurrent conditions
	 * that must be ignored.
	 */

	if (!otg_is_active(musb->xceiv) && !is_on) {
		dev_info(musb->controller, "otg is not active.\n");
		return;
	}

	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	if (is_on) {
		if (musb->xceiv->state == OTG_STATE_A_IDLE) {
			/* start the session */
			devctl |= MUSB_DEVCTL_SESSION;
			musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
			/*
			 * Wait for the musb to set as A device to enable the
			 * VBUS
			 */
			while (musb_readb(musb->mregs, MUSB_DEVCTL) & 0x80) {

				cpu_relax();

				if (!timeout) {
					dev_err(musb->controller,
					"configured as A device timeout");
					ret = -EINVAL;
					break;
				}
				mdelay(1);
				timeout--;
			}

			if (ret && musb->xceiv->set_vbus)
				otg_set_vbus(musb->xceiv, 1);
			musb->xceiv->default_a = 1;
			musb->vbus_reset_count = 0;
			MUSB_HST_MODE(musb);
		}
	} else {
		musb->is_active = 0;

		/* NOTE:  we're skipping A_WAIT_VFALL -> A_IDLE and
		 * jumping right to B_IDLE...
		 */

		musb->xceiv->default_a = 0;
		musb->xceiv->state = OTG_STATE_B_IDLE;
		devctl &= ~MUSB_DEVCTL_SESSION;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		MUSB_DEV_MODE(musb);
	}

	dev_dbg(musb->controller, "VBUS %s, devctl %02x "
		/* otg %3x conf %08x prcm %08x */ "\n",
		otg_state_string(musb->xceiv->state),
		musb_readb(musb->mregs, MUSB_DEVCTL));
}

static int omap2430_musb_set_mode(struct musb *musb, u8 musb_mode)
{
	u8	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	devctl |= MUSB_DEVCTL_SESSION;
	musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);

	return 0;
}

static inline void omap2430_low_level_exit(struct musb *musb)
{
	u32 l;

	/* in any role */
	l = musb_readl(musb->mregs, OTG_FORCESTDBY);
	l |= ENABLEFORCE;	/* enable MSTANDBY */
	musb_writel(musb->mregs, OTG_FORCESTDBY, l);
}

static inline void omap2430_low_level_init(struct musb *musb)
{
	u32 l;

	l = musb_readl(musb->mregs, OTG_FORCESTDBY);
	l &= ~ENABLEFORCE;	/* disable MSTANDBY */
	musb_writel(musb->mregs, OTG_FORCESTDBY, l);
}

/* blocking notifier support */
static void musb_otg_notifier_work(struct work_struct *data_notifier_work);

static int musb_otg_notifications(struct notifier_block *nb,
		unsigned long event, void *gadget)
{
	struct musb	*musb = container_of(nb, struct musb, nb);
	struct musb_otg_work *otg_work;

	if (gadget == NULL) {
		pr_err("%s gadget is NULL\n", __func__);
		return 0;
	}
	otg_work = kmalloc(sizeof(struct musb_otg_work), GFP_ATOMIC);
	if (!otg_work)
		return notifier_from_errno(-ENOMEM);
	INIT_WORK(&otg_work->work, musb_otg_notifier_work);
	otg_work->xceiv_event = event;
	otg_work->musb = musb;
	queue_work(musb->otg_notifier_wq, &otg_work->work);
	return 0;
}

static int omap2430_musb_otg_notifications
			(struct musb *musb, unsigned long event)
{
	struct musb_otg_work *otg_work;

	otg_work = kmalloc(sizeof(struct musb_otg_work), GFP_ATOMIC);
	if (!otg_work)
		return notifier_from_errno(-ENOMEM);
	INIT_WORK(&otg_work->work, musb_otg_notifier_work);
	otg_work->xceiv_event = event;
	otg_work->musb = musb;
	pr_info("%s recheck event=%lu\n", __func__, event);
	queue_work(musb->otg_notifier_wq, &otg_work->work);
	return 0;
}

static void musb_otg_core_reset(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct musb_hdrc_platform_data *pdata = dev->platform_data;
	struct omap_musb_board_data *data = pdata->board_data;
	struct platform_device *pdev
		= to_platform_device(musb->controller->parent);
	unsigned long flags = 0;
	u32 val = 0;
	unsigned long timeout = 1000;

	dev_info(&pdev->dev, "%s +\n", __func__);
	mutex_lock(&musb->async_musb_lock);
	spin_lock_irqsave(&musb->lock, flags);
#ifndef CONFIG_USB_SAMSUNG_OMAP_NORPM
	musb_async_resume(musb);
#endif
	musb_async_suspend(musb);
	val = musb_readl(musb->mregs, OTG_SYSCONFIG);
	val |= SOFTRST;
	musb_writel(musb->mregs, OTG_SYSCONFIG, val);
	spin_unlock_irqrestore(&musb->lock, flags);
	msleep(20);
	while (!(musb_readb(musb->mregs, OTG_SYSSTATUS) & RESETDONE)) {
			cpu_relax();
			if (!timeout) {
				dev_err(musb->controller,
				"otg core reset timeout");
				break;
			}
			mdelay(1);
			timeout--;
	}
	spin_lock_irqsave(&musb->lock, flags);
	val = musb_readl(musb->mregs, OTG_INTERFSEL);
	if (data->interface_type ==
		MUSB_INTERFACE_UTMI) {
		val &= ~ULPI_12PIN;
		val |= UTMI_8BIT;
	} else
		val |= ULPI_12PIN;
	musb_writel(musb->mregs, OTG_INTERFSEL, val);
	musb_async_resume(musb);
	spin_unlock_irqrestore(&musb->lock, flags);
	mutex_unlock(&musb->async_musb_lock);
	dev_info(&pdev->dev, "%s -\n", __func__);
}

static void musb_otg_init(struct musb *musb)
{
	pm_runtime_get_sync(musb->controller);

	/* reset musb controller */
#ifndef CONFIG_USB_SAMSUNG_OMAP_NORPM
	musb_otg_core_reset(musb);
#endif

	if (otg_is_active(musb->xceiv))
		otg_set_suspend(musb->xceiv, 1);

	otg_set_suspend(musb->xceiv, 0);

	otg_init(musb->xceiv);
	msleep(1000);
	omap2430_musb_set_vbus(musb, 1);
}

#ifdef CONFIG_PM
#ifdef CONFIG_USB_SAMSUNG_OMAP_NORPM
int omap2430_async_suspend(struct musb *musb)
{
	struct platform_device *pdev
		= to_platform_device(musb->controller->parent);
	unsigned long flags = 0;
	int ret = 0;
	if (!pdev) {
		pr_err("%s pdev is null error\n", __func__);
		return -ENODEV;
	}

	dev_info(&pdev->dev, "%s async_resume %d +\n",
		__func__, musb->async_resume);

	mutex_lock(&musb->async_musb_lock);

	do {
		musb->async_resume--;
	} while (musb->reserve_async_suspend-- > 0);
	musb->reserve_async_suspend = 0;

	if (musb->async_resume > 0)
		;
	else if (musb->async_resume < 0) {
		musb->async_resume++;
		dev_err(&pdev->dev, "%s async_resume is fault\n", __func__);
	} else {
		spin_lock_irqsave(&musb->lock, flags);
		musb_async_suspend(musb);
		omap2430_low_level_exit(musb);
		otg_set_suspend(musb->xceiv, 1);
		spin_unlock_irqrestore(&musb->lock, flags);
		ret = omap_device_idle(pdev);
		if (ret < 0) {
			dev_err(&pdev->dev, "%s omap_device_idle error ret=%d\n",
				__func__, ret);
			mutex_unlock(&musb->async_musb_lock);
			return ret;
		}
	}
	mutex_unlock(&musb->async_musb_lock);
	dev_info(&pdev->dev, "%s async_resume %d -\n",
		__func__, musb->async_resume);
	return 0;
}

int omap2430_async_resume(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct musb_hdrc_platform_data *pdata = dev->platform_data;
	struct omap_musb_board_data *data = pdata->board_data;
	struct platform_device *pdev
		= to_platform_device(musb->controller->parent);
	unsigned long flags = 0;
	u32 val = 0;
	int ret = 0;
	if (!pdev) {
		pr_err("%s pdev is null error\n", __func__);
		return -ENODEV;
	}

	dev_info(&pdev->dev, "%s async_resume=%d +\n",
		__func__, musb->async_resume);

	mutex_lock(&musb->async_musb_lock);
	if (musb->async_resume > 0)
		musb->async_resume++;
	else {
		ret = omap_device_enable(pdev);
		if (ret < 0) {
			dev_err(&pdev->dev, "%s omap_device_enable error ret=%d\n",
				__func__, ret);
			mutex_unlock(&musb->async_musb_lock);
			return ret;
		}
		spin_lock_irqsave(&musb->lock, flags);
		otg_set_suspend(musb->xceiv, 0);
		omap2430_low_level_init(musb);
		val = musb_readl(musb->mregs, OTG_INTERFSEL);
		if (data->interface_type ==
			MUSB_INTERFACE_UTMI) {
			val &= ~ULPI_12PIN;
			val |= UTMI_8BIT;
		} else {
			val |= ULPI_12PIN;
		}
		musb_writel(musb->mregs, OTG_INTERFSEL, val);
		musb_async_resume(musb);
		spin_unlock_irqrestore(&musb->lock, flags);
		musb->async_resume++;
	}
	mutex_unlock(&musb->async_musb_lock);
	dev_info(&pdev->dev, "%s async_resume %d -\n",
		__func__, musb->async_resume);
	return 0;
}
#endif
#endif

static void musb_otg_notifier_work(struct work_struct *data_notifier_work)
{
	struct musb_otg_work *otg_work =
		container_of(data_notifier_work, struct musb_otg_work, work);
	struct musb *musb = otg_work->musb;
	struct device *dev = musb->controller;
	struct musb_hdrc_platform_data *pdata = dev->platform_data;
	struct omap_musb_board_data *data = pdata->board_data;
	enum usb_xceiv_events xceiv_event = otg_work->xceiv_event;
	unsigned long	flags;
#ifdef CONFIG_USB_SAMSUNG_OMAP_NORPM
	int ret = 0;
#endif

	kfree(otg_work);

	switch (xceiv_event) {
	case USB_EVENT_ID:
		dev_info(musb->controller, "ID GND\n");
		musb->xceiv->state = OTG_STATE_A_IDLE;
#ifdef CONFIG_USB_SAMSUNG_OMAP_NORPM
		ret = omap2430_async_resume(musb);
		if (ret < 0)
			return;
#endif
		if (is_otg_enabled(musb)) {
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
			if (musb->gadget_driver) {
				musb_otg_init(musb);
			}
#endif
		} else {
			musb_otg_init(musb);
		}
#ifdef CONFIG_USB_SAMSUNG_OMAP_NORPM
		musb_add_hcd(musb);
#endif
		break;
	case USB_EVENT_VBUS_CHARGER:
		dev_info(musb->controller, "USB/TA Connect\n");
		/*  This event received from ta_connect_irq
		 * when a usb cable is connected. Logic has still
		 * not identified whether this is a usb cable or TA.
		 *  So just break here.
		 */
		break;
	case USB_EVENT_VBUS:
		dev_info(musb->controller, "VBUS Connect\n");
#ifdef CONFIG_USB_SAMSUNG_OMAP_NORPM
		ret = omap2430_async_resume(musb);
		if (ret < 0)
			return;
#endif
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
		if (musb->gadget_driver)
			pm_runtime_get_sync(musb->controller);
#endif
		otg_init(musb->xceiv);
#ifdef CONFIG_USB_SAMSUNG_OMAP_NORPM
		musb_start(musb);
		musb_platform_pullup(musb, 1);
#endif
		break;

	case USB_EVENT_CHARGER:
		dev_info(musb->controller, "Dedicated charger connect\n");
		musb->is_ac_charger = true;
		break;
	case USB_EVENT_HOST_NONE:
#ifdef CONFIG_USB_SAMSUNG_OMAP_NORPM
		dev_info(musb->controller, "USB host Disconnect. ID float\n");
		musb_stop(musb);
		musb_remove_hcd(musb);
		if (data->interface_type == MUSB_INTERFACE_UTMI) {
			omap2430_musb_set_vbus(musb, 0);
			if (musb->xceiv->set_vbus)
				otg_set_vbus(musb->xceiv, 0);
		}
		otg_shutdown(musb->xceiv);
		musb_otg_core_reset(musb);
		ret = omap2430_async_suspend(musb);
		if (ret < 0)
			return;
		break;
#endif
	case USB_EVENT_NONE:
		if (musb->is_ac_charger) {
			dev_info(musb->controller,
				"Dedicated charger disconnect\n");
			musb->is_ac_charger = false;
			break;
		}

		dev_info(musb->controller, "VBUS Disconnect\n");
#ifndef CONFIG_USB_SAMSUNG_OMAP_NORPM

		spin_lock_irqsave(&musb->lock, flags);
		musb_g_disconnect(musb);
		spin_unlock_irqrestore(&musb->lock, flags);
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
		if (is_otg_enabled(musb) || is_peripheral_enabled(musb))
			if (musb->gadget_driver)
#endif
			{
				pm_runtime_mark_last_busy(musb->controller);
				pm_runtime_put_autosuspend(musb->controller);
			}

		if (data->interface_type == MUSB_INTERFACE_UTMI) {
			omap2430_musb_set_vbus(musb, 0);
			if (musb->xceiv->set_vbus)
				otg_set_vbus(musb->xceiv, 0);
		}
		otg_shutdown(musb->xceiv);
#else
		musb_platform_pullup(musb, 0);
		spin_lock_irqsave(&musb->lock, flags);
		musb_stop(musb);
		musb_all_ep_flush(musb);
		musb_g_disconnect(musb);
		spin_unlock_irqrestore(&musb->lock, flags);
		if (data->interface_type == MUSB_INTERFACE_UTMI)
			omap2430_musb_set_vbus(musb, 0);
		otg_shutdown(musb->xceiv);
		musb_otg_core_reset(musb);
		ret = omap2430_async_suspend(musb);
		if (ret < 0)
			return;
#endif
		break;
	default:
		dev_info(musb->controller, "ID float\n");
	}
}

static int omap2430_musb_init(struct musb *musb)
{
	u32 l;
	int status = 0;
	struct device *dev = musb->controller;
	struct musb_hdrc_platform_data *plat = dev->platform_data;
	struct omap_musb_board_data *data = plat->board_data;

	/* We require some kind of external transceiver, hooked
	 * up through ULPI.  TWL4030-family PMICs include one,
	 * which needs a driver, drivers aren't always needed.
	 */
	musb->xceiv = otg_get_transceiver();
	if (!musb->xceiv) {
		pr_err("HS USB OTG: no transceiver configured\n");
		return -ENODEV;
	}

	musb->otg_notifier_wq = create_singlethread_workqueue("musb-otg");
	if (!musb->otg_notifier_wq) {
		pr_err("HS USB OTG: cannot allocate otg event wq\n");
		status = -ENOMEM;
		goto err1;
	}
#ifdef CONFIG_USB_SAMSUNG_OMAP_NORPM
	omap2430_async_resume(musb);
#endif
	status = pm_runtime_get_sync(dev);
#ifndef CONFIG_USB_SAMSUNG_OMAP_NORPM
	if (status < 0) {
		dev_err(dev, "pm_runtime_get_sync FAILED");
		goto err2;
	}
#endif

	l = musb_readl(musb->mregs, OTG_INTERFSEL);

	if (data->interface_type == MUSB_INTERFACE_UTMI) {
		/* OMAP4 uses Internal PHY GS70 which uses UTMI interface */
		l &= ~ULPI_12PIN;       /* Disable ULPI */
		l |= UTMI_8BIT;         /* Enable UTMI  */
	} else {
		l |= ULPI_12PIN;
	}

	musb_writel(musb->mregs, OTG_INTERFSEL, l);

	pr_debug("HS USB OTG: revision 0x%x, sysconfig 0x%02x, "
			"sysstatus 0x%x, intrfsel 0x%x, simenable  0x%x\n",
			musb_readl(musb->mregs, OTG_REVISION),
			musb_readl(musb->mregs, OTG_SYSCONFIG),
			musb_readl(musb->mregs, OTG_SYSSTATUS),
			musb_readl(musb->mregs, OTG_INTERFSEL),
			musb_readl(musb->mregs, OTG_SIMENABLE));

	musb->nb.notifier_call = musb_otg_notifications;
	status = otg_register_notifier(musb->xceiv, &musb->nb);

	if (status)
		dev_dbg(musb->controller, "notification register failed\n");

	setup_timer(&musb_idle_timer, musb_do_idle, (unsigned long) musb);

	pm_runtime_put_noidle(musb->controller);
#ifdef CONFIG_USB_SAMSUNG_OMAP_NORPM
	musb->reserve_async_suspend++;
#endif
	return 0;

err2:
	destroy_workqueue(musb->otg_notifier_wq);
err1:
	otg_put_transceiver(musb->xceiv);
	pm_runtime_disable(dev);
	return status;
}

static void omap2430_musb_enable(struct musb *musb)
{
	u8		devctl;
	unsigned long timeout = jiffies + msecs_to_jiffies(1000);
	struct device *dev = musb->controller;
	struct musb_hdrc_platform_data *pdata = dev->platform_data;
	struct omap_musb_board_data *data = pdata->board_data;
	u32 val;

	switch (musb->xceiv->last_event) {

	case USB_EVENT_ID:
		val = musb_readl(musb->mregs, OTG_INTERFSEL);
		if (data->interface_type == MUSB_INTERFACE_UTMI) {
			val &= ~ULPI_12PIN;
			val |= UTMI_8BIT;
		} else {
			val |= ULPI_12PIN;
		}

		musb_writel(musb->mregs, OTG_INTERFSEL, val);
		otg_init(musb->xceiv);
		omap2430_musb_set_vbus(musb, 1);
		break;

	case USB_EVENT_VBUS:
		val = musb_readl(musb->mregs, OTG_INTERFSEL);
		if (data->interface_type ==
			MUSB_INTERFACE_UTMI) {
			val &= ~ULPI_12PIN;
			val |= UTMI_8BIT;
		} else {
			val |= ULPI_12PIN;
		}
		musb_writel(musb->mregs, OTG_INTERFSEL, val);
		otg_init(musb->xceiv);
		break;

	case USB_EVENT_CHARGER:
		dev_dbg(musb->controller, "Dedicated charger connect\n");
		musb->is_ac_charger = true;
		break;

	default:
		break;
	}
}

static void omap2430_musb_disable(struct musb *musb)
{
	if (musb->xceiv->last_event)
		otg_shutdown(musb->xceiv);
}

static int omap2430_musb_exit(struct musb *musb)
{
	del_timer_sync(&musb_idle_timer);

	otg_unregister_notifier(musb->xceiv, &musb->nb);
	destroy_workqueue(musb->otg_notifier_wq);
	omap2430_low_level_exit(musb);
	otg_put_transceiver(musb->xceiv);

	return 0;
}

static int omap2430_musb_vbus_reset(struct musb *musb)
{
	int ret = -1;

	dev_info(musb->controller, "%s count=%d\n", __func__,
					musb->vbus_reset_count);
	if (musb->vbus_reset_count < 5) {
		if (musb->xceiv->start_hnp)
			ret = otg_start_hnp(musb->xceiv);
	}

	return ret;
}

static const struct musb_platform_ops omap2430_ops = {
	.init		= omap2430_musb_init,
	.exit		= omap2430_musb_exit,

	.set_mode	= omap2430_musb_set_mode,
	.try_idle	= omap2430_musb_try_idle,

	.set_vbus	= omap2430_musb_set_vbus,

	.enable		= omap2430_musb_enable,
	.disable	= omap2430_musb_disable,
	.vbus_reset = omap2430_musb_vbus_reset,
	.otg_notifications = omap2430_musb_otg_notifications,
#ifdef CONFIG_USB_SAMSUNG_OMAP_NORPM
	.async_suspend = omap2430_async_suspend,
	.async_resume = omap2430_async_resume,
#endif
};

static u64 omap2430_dmamask = DMA_BIT_MASK(32);

static int __init omap2430_probe(struct platform_device *pdev)
{
	struct musb_hdrc_platform_data	*pdata = pdev->dev.platform_data;
	struct platform_device		*musb;
	struct omap2430_glue		*glue;
	int				ret = -ENOMEM;

	glue = kzalloc(sizeof(*glue), GFP_KERNEL);
	if (!glue) {
		dev_err(&pdev->dev, "failed to allocate glue context\n");
		goto err0;
	}

	musb = platform_device_alloc("musb-hdrc", -1);
	if (!musb) {
		dev_err(&pdev->dev, "failed to allocate musb device\n");
		goto err1;
	}

	musb->dev.parent		= &pdev->dev;
	musb->dev.dma_mask		= &omap2430_dmamask;
	musb->dev.coherent_dma_mask	= omap2430_dmamask;

	glue->dev			= &pdev->dev;
	glue->musb			= musb;

	pdata->platform_ops		= &omap2430_ops;

	platform_set_drvdata(pdev, glue);

	ret = platform_device_add_resources(musb, pdev->resource,
			pdev->num_resources);
	if (ret) {
		dev_err(&pdev->dev, "failed to add resources\n");
		goto err2;
	}

	ret = platform_device_add_data(musb, pdata, sizeof(*pdata));
	if (ret) {
		dev_err(&pdev->dev, "failed to add platform_data\n");
		goto err2;
	}

#ifndef CONFIG_USB_SAMSUNG_OMAP_NORPM
	pm_runtime_enable(&pdev->dev);
#endif

	ret = platform_device_add(musb);
	if (ret) {
		dev_err(&pdev->dev, "failed to register musb device\n");
		goto err2;
	}

	return 0;

err2:
	platform_device_put(musb);

err1:
	kfree(glue);

err0:
	return ret;
}

static int __exit omap2430_remove(struct platform_device *pdev)
{
	struct omap2430_glue		*glue = platform_get_drvdata(pdev);

	platform_device_del(glue->musb);
	platform_device_put(glue->musb);
	pm_runtime_disable(&pdev->dev);
	kfree(glue);

	return 0;
}

#ifdef CONFIG_PM

static int omap2430_runtime_suspend(struct device *dev)
{
	struct omap2430_glue		*glue = dev_get_drvdata(dev);
	struct musb			*musb = glue_to_musb(glue);

	if (mutex_trylock(&musb->musb_lock)) {
		dev_info(dev, "runtime suspend\n");
		musb->context.otg_interfsel =
				musb_readl(musb->mregs,
						OTG_INTERFSEL);

		omap2430_low_level_exit(musb);
		otg_set_suspend(musb->xceiv, 1);
		mutex_unlock(&musb->musb_lock);
		return 0;
	}
	return -EBUSY;
}

static int omap2430_runtime_resume(struct device *dev)
{
	struct omap2430_glue		*glue = dev_get_drvdata(dev);
	struct musb			*musb = glue_to_musb(glue);

	dev_info(dev, "runtime resume\n");
	omap2430_low_level_init(musb);
	musb_writel(musb->mregs, OTG_INTERFSEL,
					musb->context.otg_interfsel);

	otg_set_suspend(musb->xceiv, 0);

	return 0;
}

static struct dev_pm_ops omap2430_pm_ops = {
	.runtime_suspend = omap2430_runtime_suspend,
	.runtime_resume = omap2430_runtime_resume,
};

#define DEV_PM_OPS	(&omap2430_pm_ops)
#else
#define DEV_PM_OPS	NULL
#endif

static struct platform_driver omap2430_driver = {
	.remove		= __exit_p(omap2430_remove),
	.driver		= {
		.name	= "musb-omap2430",
		.pm	= DEV_PM_OPS,
	},
};

MODULE_DESCRIPTION("OMAP2PLUS MUSB Glue Layer");
MODULE_AUTHOR("Felipe Balbi <balbi@ti.com>");
MODULE_LICENSE("GPL v2");

static int __init omap2430_init(void)
{
	return platform_driver_probe(&omap2430_driver, omap2430_probe);
}
subsys_initcall(omap2430_init);

static void __exit omap2430_exit(void)
{
	platform_driver_unregister(&omap2430_driver);
}
module_exit(omap2430_exit);
