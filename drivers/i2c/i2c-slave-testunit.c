// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C slave mode testunit
 *
 * Copyright (C) 2020 by Wolfram Sang, Sang Engineering <wsa@sang-engineering.com>
 * Copyright (C) 2020 by Renesas Electronics Corporation
 */

#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/workqueue.h> // FIXME: own workqueue?

enum testunit_cmds {
	TU_CMD_READ_BYTES,
	TU_CMD_HOST_NOTIFY,
};

enum testunit_regs {
	TU_REG_CMD,
	TU_REG_DATAL,
	TU_REG_DATAH,
	TU_REG_DELAY,
	TU_NUM_REGS
};

struct testunit_data {
	u8 regs[TU_NUM_REGS];
	u8 reg_idx;
	struct i2c_client *client;
	struct delayed_work worker;
	struct i2c_msg msg;
	u8 msgbuf[256];
};

static void i2c_slave_testunit_work(struct work_struct *work)
{
	struct testunit_data *tu = container_of(work, struct testunit_data, worker.work);
	int ret;

	switch (tu->regs[TU_REG_CMD]) {

	case TU_CMD_READ_BYTES:
		tu->msg.addr = tu->regs[TU_REG_DATAL];
		tu->msg.flags = I2C_M_RD;
		tu->msg.len = tu->regs[TU_REG_DATAH];
		break;

	case TU_CMD_HOST_NOTIFY:
		tu->msg.addr = 0x08;
		tu->msg.flags = 0;
		tu->msg.len = 3;
		tu->msgbuf[0] = tu->client->addr;
		tu->msgbuf[1] = tu->regs[TU_REG_DATAL];
		tu->msgbuf[2] = tu->regs[TU_REG_DATAH];
		break;
	}

	ret = i2c_transfer(tu->client->adapter, &tu->msg, 1);
	if (ret != 1)
		dev_err(&tu->client->dev, "CMD%02X failed (%d)\n", tu->regs[TU_REG_CMD], ret);
}

static int i2c_slave_testunit_slave_cb(struct i2c_client *client,
				     enum i2c_slave_event event, u8 *val)
{
	struct testunit_data *tu = i2c_get_clientdata(client);
	int ret = 0;

	switch (event) {
	case I2C_SLAVE_WRITE_REQUESTED:
		break;

	case I2C_SLAVE_WRITE_RECEIVED:
		if (tu->reg_idx < TU_NUM_REGS)
			tu->regs[tu->reg_idx] = *val;
		else
			ret = -EMSGSIZE;

		if (tu->reg_idx <= TU_NUM_REGS)
			tu->reg_idx++;

		break;

	case I2C_SLAVE_STOP:
		if (tu->reg_idx == TU_NUM_REGS)
			queue_delayed_work(system_long_wq, &tu->worker,
					   msecs_to_jiffies(100 * tu->regs[TU_REG_DELAY]));
		tu->reg_idx = 0;
		break;

	case I2C_SLAVE_READ_REQUESTED:
	case I2C_SLAVE_READ_PROCESSED:
		*val = 0xff;
		break;
	}

	return ret;
}

static int i2c_slave_testunit_probe(struct i2c_client *client)
{
	struct testunit_data *tu;

	tu = devm_kzalloc(&client->dev, sizeof(struct testunit_data), GFP_KERNEL);
	if (!tu)
		return -ENOMEM;

	tu->msg.buf = tu->msgbuf;
	tu->client = client;
	i2c_set_clientdata(client, tu);
	INIT_DELAYED_WORK(&tu->worker, i2c_slave_testunit_work);

	return i2c_slave_register(client, i2c_slave_testunit_slave_cb);
};

static int i2c_slave_testunit_remove(struct i2c_client *client)
{
	struct testunit_data *tu = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&tu->worker);
	i2c_slave_unregister(client);
	return 0;
}

static const struct i2c_device_id i2c_slave_testunit_id[] = {
	{ "slave-testunit", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, i2c_slave_testunit_id);

static struct i2c_driver i2c_slave_testunit_driver = {
	.driver = {
		.name = "i2c-slave-testunit",
	},
	.probe_new = i2c_slave_testunit_probe,
	.remove = i2c_slave_testunit_remove,
	.id_table = i2c_slave_testunit_id,
};
module_i2c_driver(i2c_slave_testunit_driver);

MODULE_AUTHOR("Wolfram Sang <wsa@sang-engineering.com>");
MODULE_DESCRIPTION("I2C slave mode test unit");
MODULE_LICENSE("GPL v2");
