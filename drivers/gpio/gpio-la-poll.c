// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simple logic analyzer using GPIOs
 *
 * Copyright (C) 2020 Wolfram Sang <wsa@sang-engineering.com>
 * Copyright (C) 2020 Renesas Electronics Corporation
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/timekeeping.h>
#include <linux/vmalloc.h>

#define GPIO_LA_DEFAULT_BUF_SIZE SZ_256K

struct gpio_la_poll_priv {
	unsigned long ndelay;
	u32 buf_idx;
	struct debugfs_blob_wrapper blob;
	struct gpio_descs *descs;
	struct dentry *debug_dir, *blob_dent;
	struct debugfs_blob_wrapper meta;
	unsigned long gpio_delay;
};

static struct dentry *gpio_la_poll_debug_dir;

static int fops_capture_set(void *data, u64 val)
{
	struct gpio_la_poll_priv *priv = data;
	u8 *la_buf = priv->blob.data;
	unsigned long state = 0;
	int ret;

	if (val) {
		if (priv->blob_dent) {
			debugfs_remove(priv->blob_dent);
			priv->blob_dent = NULL;
		}

		priv->buf_idx = 0;

		local_irq_disable();
		preempt_disable_notrace();
		while (priv->buf_idx < priv->blob.size && ret == 0) {
			ret = gpiod_get_array_value(priv->descs->ndescs, priv->descs->desc,
					      priv->descs->info, &state);
			la_buf[priv->buf_idx++] = state;
			//la_buf[priv->buf_idx++] = gpiod_get_value(priv->descs->desc[0]);
			ndelay(priv->ndelay);
		}
		preempt_enable_notrace();
		local_irq_enable();
		if (ret)
			pr_err("%s: couldn't read GPIOs: %d\n", __func__, ret);

		priv->blob_dent = debugfs_create_blob("sample_data", 0400, priv->debug_dir, &priv->blob);
	}

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(fops_capture, NULL, fops_capture_set, "%llu\n");

static int fops_buf_size_get(void *data, u64 *val)
{
	struct gpio_la_poll_priv *priv = data;

	*val = priv->blob.size;

	return 0;
}

static int fops_buf_size_set(void *data, u64 val)
{
	struct gpio_la_poll_priv *priv = data;
	void *p;

	//FIXME: locking?
	//FIXME: sanity checks on val
	vfree(priv->blob.data);
	p = vzalloc(val);
	if (!p)
		return -ENOMEM;

	priv->blob.data = p;
	priv->blob.size = val;

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(fops_buf_size, fops_buf_size_get, fops_buf_size_set, "%llu\n");

static int gpio_la_poll_probe(struct platform_device *pdev)
{
	struct gpio_la_poll_priv *priv;
	struct device *dev = &pdev->dev;
	char *meta = NULL;
	unsigned long state;
	ktime_t start_time, end_time;
	int ret, i;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	fops_buf_size_set(priv, GPIO_LA_DEFAULT_BUF_SIZE);

	priv->descs = devm_gpiod_get_array(dev, "probe", GPIOD_IN);
	if (IS_ERR(priv->descs))
		return PTR_ERR(priv->descs);

	/* artificial limit to keep 1 byte per sample */
	if (priv->descs->ndescs > 8)
		return -ERANGE;

	for (i = 0; i < priv->descs->ndescs; i++) {
		const char *str, *old_meta;

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
	}

	priv->debug_dir = debugfs_create_dir(dev_name(dev), gpio_la_poll_debug_dir);
	if (IS_ERR(priv->debug_dir))
		return PTR_ERR(priv->debug_dir);

	debugfs_create_file_unsafe("buf_size", 0600, priv->debug_dir, priv, &fops_buf_size);
	debugfs_create_file_unsafe("capture", 0600, priv->debug_dir, priv, &fops_capture);
	debugfs_create_ulong("delay_ns_user", 0600, priv->debug_dir, &priv->ndelay);

	priv->meta.data = meta;
	priv->meta.size = strlen(meta);
	debugfs_create_blob("meta_data", 0400, priv->debug_dir, &priv->meta);

	local_irq_disable();
	preempt_disable_notrace();
	start_time = ktime_get();
	for (i = 0; i < 1024 && ret == 0; i++)
		ret = gpiod_get_array_value(priv->descs->ndescs, priv->descs->desc,
				      priv->descs->info, &state);
	end_time = ktime_get();
	preempt_enable_notrace();
	local_irq_enable();
	if (ret) {
		dev_err(dev, "couldn't read GPIOs: %d\n", ret);
		return ret;
	}

	priv->gpio_delay = ktime_sub(end_time, start_time) / 1024;
	debugfs_create_ulong("delay_ns_acquisition", 0400, priv->debug_dir, &priv->gpio_delay);

	return 0;
}

static const struct of_device_id gpio_la_poll_of_match[] = {
	{ .compatible = "logic-analyzer-poll", },
	{ },
};
MODULE_DEVICE_TABLE(of, gpio_la_poll_of_match);

static struct platform_driver gpio_la_poll_device_driver = {
	.probe		= gpio_la_poll_probe,
	//.remove	= FIXME
	.driver		= {
		.name	= "logic-analyzer-poll",
		.of_match_table = gpio_la_poll_of_match,
	}
};

static int __init gpio_la_poll_init(void)
{
	gpio_la_poll_debug_dir = debugfs_create_dir("simple_logic_analyzer-poll", NULL);
	if (IS_ERR(gpio_la_poll_debug_dir))
		return PTR_ERR(gpio_la_poll_debug_dir);

	return platform_driver_register(&gpio_la_poll_device_driver);
}
late_initcall(gpio_la_poll_init);

static void __exit gpio_la_poll_exit(void)
{
	platform_driver_unregister(&gpio_la_poll_device_driver);
	debugfs_remove_recursive(gpio_la_poll_debug_dir);
}
module_exit(gpio_la_poll_exit);

MODULE_AUTHOR("Wolfram Sang <wsa@sang-engineering.com>");
MODULE_DESCRIPTION("Simple logic analyzer using GPIOs");
MODULE_LICENSE("GPL v2");
