// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simple logic analyzer using GPIOs
 *
 * Copyright (C) 2020 Wolfram Sang <wsa@sang-engineering.com>
 * Copyright (C) 2020 Renesas Electronics Corporation
 */

#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched/clock.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#define GPIO_LA_DEFAULT_BUF_SIZE SZ_256K

struct gpio_la_priv {
	struct debugfs_blob_wrapper blob;
	struct gpio_descs *descs;
	bool capture_running;
	u32 buf_idx;
	struct dentry *debug_dir, *blob_dent;
	struct debugfs_blob_wrapper meta;
};

struct gpio_la_irq_data {
	unsigned int channel;
	struct gpio_la_priv *priv;
};

static struct dentry *gpio_la_debug_dir;

static __always_inline void gpio_la_sample_data(struct gpio_la_priv *priv, unsigned int channel)
{
	u64 *la_buf = priv->blob.data;
	int state = gpiod_get_value(priv->descs->desc[channel]);

	la_buf[priv->buf_idx++] = local_clock() << 8 | channel << 1 | state;
}

static irqreturn_t gpio_la_irq(int irq, void *dev_id)
{
	struct gpio_la_irq_data *idata = dev_id;
	struct gpio_la_priv *priv = idata->priv;

	//FIXME: locking
	if (priv->capture_running) {
		gpio_la_sample_data(priv, idata->channel);
#if 0
		if (priv->buf_idx >= priv->blob.size)
			priv->capture_running = false;
			//FIXME: completion to notify main thread
#endif
	}

	return IRQ_HANDLED;
}

static int fops_capture_get(void *data, u64 *val)
{
	struct gpio_la_priv *priv = data;

	*val = priv->capture_running;

	return 0;
}

static int fops_capture_set(void *data, u64 val)
{
	struct gpio_la_priv *priv = data;
	int i;

	if (!!val == priv->capture_running)
		return 0;

	if (val) {
		//spinlock
		if (priv->blob_dent)
			debugfs_remove(priv->blob_dent);

		priv->buf_idx = 0;

		for (i = 0; i < priv->descs->ndescs; i++)
			gpio_la_sample_data(priv, i);

		priv->capture_running = true;

	} else {
		priv->capture_running = false;

		for (i = 0; i < priv->descs->ndescs; i++)
			gpio_la_sample_data(priv, i);

		priv->blob.size = priv->buf_idx * sizeof(u64);
		priv->blob_dent = debugfs_create_blob("sample_data", 0400, priv->debug_dir, &priv->blob);
	}

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(fops_capture, fops_capture_get, fops_capture_set, "%llu\n");

#if 0
static int fops_buf_size_get(void *data, u64 *val)
{
	*val = priv->blob.size;

	return 0;
}
#endif

static int fops_buf_size_set(void *data, u64 val)
{
	struct gpio_la_priv *priv = data;
	void *p;

	//FIXME: locking!
	//FIXME: sanity checks on val
	vfree(priv->blob.data);
	p = vzalloc(val);
	if (!p)
		return -ENOMEM;

	memset(p, 0xbd, val); // FIXME: remove 0xbd

	priv->blob.data = p;

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(fops_buf_size, NULL, fops_buf_size_set, "%llu\n");

static int gpio_la_probe(struct platform_device *pdev)
{
	struct gpio_la_priv *priv;
	struct device *dev = &pdev->dev;
	char *meta = NULL;
	int ret, i;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	fops_buf_size_set(priv, GPIO_LA_DEFAULT_BUF_SIZE);

	priv->descs = devm_gpiod_get_array(dev, "probe", GPIOD_IN);
	if (IS_ERR(priv->descs))
		return PTR_ERR(priv->descs);

	for (i = 0; i < priv->descs->ndescs; i++) {
		struct gpio_la_irq_data *idata;
		const char *str, *old_meta;
		int irq;

		ret = of_property_read_string_index(pdev->dev.of_node, "probe-names",
						    i, &str);
		if (ret == 0) {
			gpiod_set_consumer_name(priv->descs->desc[i], str);

			old_meta = meta;
			meta = devm_kasprintf(dev, GFP_KERNEL, "%sChannel %d: %s\n",
					      old_meta ?: "", i, str);
			if (!meta)
				return -ENOMEM;

			devm_kfree(dev, old_meta);
		}

		irq = gpiod_to_irq(priv->descs->desc[i]);
		if (irq < 0)
			return irq;

		idata = devm_kmalloc(dev, sizeof(*idata), GFP_KERNEL);
		if (!idata)
			return -ENOMEM;

		idata->channel = i;
		idata->priv = priv;

		ret = devm_request_irq(dev, irq, gpio_la_irq,
				       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_NO_THREAD,
				       "simple_logic_analyzer", idata);
		if (ret)
			return ret;
	}

	priv->debug_dir = debugfs_create_dir(dev_name(dev), gpio_la_debug_dir);
	if (IS_ERR(priv->debug_dir))
		return PTR_ERR(priv->debug_dir);

	debugfs_create_file_unsafe("buf_size", 0600, priv->debug_dir, priv, &fops_buf_size);
	debugfs_create_file_unsafe("capture", 0600, priv->debug_dir, priv, &fops_capture);

	priv->meta.data = meta;
	priv->meta.size = strlen(meta);
	debugfs_create_blob("meta_data", 0400, priv->debug_dir, &priv->meta);

	return 0;
}

static const struct of_device_id gpio_la_of_match[] = {
	{ .compatible = "logic-analyzer-irq", },
	{ },
};
MODULE_DEVICE_TABLE(of, gpio_la_of_match);

static struct platform_driver gpio_la_device_driver = {
	.probe		= gpio_la_probe,
	//.shutdown	= gpio_la_shutdown,
	.driver		= {
		.name	= "logic-analyzer",
		.of_match_table = gpio_la_of_match,
	}
};

static int __init gpio_la_init(void)
{
	gpio_la_debug_dir = debugfs_create_dir("simple_logic_analyzer", NULL);
	if (IS_ERR(gpio_la_debug_dir))
		return PTR_ERR(gpio_la_debug_dir);

	return platform_driver_register(&gpio_la_device_driver);
}
late_initcall(gpio_la_init);

static void __exit gpio_la_exit(void)
{
	platform_driver_unregister(&gpio_la_device_driver);
	debugfs_remove_recursive(gpio_la_debug_dir);
}
module_exit(gpio_la_exit);

MODULE_AUTHOR("Wolfram Sang <wsa@sang-engineering.com>");
MODULE_DESCRIPTION("Simple logic analyzer using GPIOs");
MODULE_LICENSE("GPL v2");
