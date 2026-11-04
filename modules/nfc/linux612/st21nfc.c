// SPDX-License-Identifier: GPL-2.0
/*
 * ST21NFC NFC controller (NCI over I2C) as /dev/st21nfc, the character
 * device ST's driver gives userspace (titan-nfc speaks NCI through it):
 * write() sends one NCI packet, read() returns the controller's packet
 * while its IRQ line is high (0x7e filler otherwise), poll() waits for the
 * line, and the ioctls pulse the reset line or set the IRQ mode.
 *
 * Power: the controller runs only while the device is open. On open the
 * driver turns on its 26 MHz reference, the MT6358's XO buffer 3 (the
 * vendor's CLK_BUF_NFC, software controlled), and raises VEN; on close VEN
 * goes low (hibernate) and the buffer off. Left alone, VEN stays where the
 * bootloader put it (high), and the controller sits awake with an unread
 * CORE_RESET_NTF and its IRQ raised.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/regmap.h>
#include <linux/wait.h>

#define ST21NFC_MAGIC			0xEA
#define ST21NFC_PULSE_RESET		_IO(ST21NFC_MAGIC, 0x02)
#define ST21NFC_SET_POLARITY_RISING	_IO(ST21NFC_MAGIC, 0x03)
#define ST21NFC_SET_POLARITY_HIGH	_IO(ST21NFC_MAGIC, 0x05)

#define MAX_BUFFER_SIZE		260
#define IDLE_CHARACTER		0x7e

/* MT6358 DCXO: write-1-to-set and -clear views of CW00; XO_EXTBUF3_EN_M */
#define MT6358_DCXO_CW00_SET	0x78a
#define MT6358_DCXO_CW00_CLR	0x78c
#define XO_EXTBUF3_EN_M		BIT(8)

struct st21nfc {
	struct i2c_client *client;
	struct miscdevice misc;
	struct gpio_desc *ven, *irq_gpio;
	struct regmap *pmic;
	wait_queue_head_t read_wq;
	struct mutex lock;
	bool open, irq_enabled;
	u8 buf[MAX_BUFFER_SIZE];
};

static void st21nfc_power(struct st21nfc *nfc, bool on)
{
	if (on) {
		if (nfc->pmic)
			regmap_write(nfc->pmic, MT6358_DCXO_CW00_SET,
				     XO_EXTBUF3_EN_M);
		gpiod_set_value_cansleep(nfc->ven, 1);
		usleep_range(10000, 11000);
	} else {
		gpiod_set_value_cansleep(nfc->ven, 0);
		if (nfc->pmic)
			regmap_write(nfc->pmic, MT6358_DCXO_CW00_CLR,
				     XO_EXTBUF3_EN_M);
	}
}

static irqreturn_t st21nfc_irq(int irq, void *data)
{
	struct st21nfc *nfc = data;

	/* level triggered: quiet until poll() sees the line low again */
	disable_irq_nosync(irq);
	nfc->irq_enabled = false;
	wake_up(&nfc->read_wq);
	return IRQ_HANDLED;
}

static struct st21nfc *to_nfc(struct file *f)
{
	return container_of(f->private_data, struct st21nfc, misc);
}

static ssize_t st21nfc_read(struct file *f, char __user *ubuf, size_t count,
			    loff_t *ppos)
{
	struct st21nfc *nfc = to_nfc(f);
	int ret;

	count = min_t(size_t, count, MAX_BUFFER_SIZE);
	if (!count)
		return 0;
	mutex_lock(&nfc->lock);
	if (!gpiod_get_value_cansleep(nfc->irq_gpio)) {
		memset(nfc->buf, IDLE_CHARACTER, count);
		ret = count;
	} else {
		ret = i2c_master_recv(nfc->client, nfc->buf, count);
	}
	if (ret > 0 && copy_to_user(ubuf, nfc->buf, ret))
		ret = -EFAULT;
	mutex_unlock(&nfc->lock);
	return ret;
}

static ssize_t st21nfc_write(struct file *f, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct st21nfc *nfc = to_nfc(f);
	int ret;

	if (count > MAX_BUFFER_SIZE)
		return -EINVAL;
	mutex_lock(&nfc->lock);
	if (copy_from_user(nfc->buf, ubuf, count))
		ret = -EFAULT;
	else
		ret = i2c_master_send(nfc->client, nfc->buf, count);
	mutex_unlock(&nfc->lock);
	return ret;
}

static __poll_t st21nfc_poll(struct file *f, poll_table *wait)
{
	struct st21nfc *nfc = to_nfc(f);

	poll_wait(f, &nfc->read_wq, wait);
	if (gpiod_get_value_cansleep(nfc->irq_gpio))
		return EPOLLIN | EPOLLRDNORM;
	if (!nfc->irq_enabled) {
		nfc->irq_enabled = true;
		enable_irq(nfc->client->irq);
	}
	return 0;
}

static long st21nfc_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct st21nfc *nfc = to_nfc(f);

	switch (cmd) {
	case ST21NFC_PULSE_RESET:
		/* two 20 ms low pulses on VEN, as ST's driver */
		gpiod_set_value_cansleep(nfc->ven, 0);
		msleep(20);
		gpiod_set_value_cansleep(nfc->ven, 1);
		usleep_range(10000, 11000);
		gpiod_set_value_cansleep(nfc->ven, 0);
		msleep(20);
		gpiod_set_value_cansleep(nfc->ven, 1);
		return 0;
	case ST21NFC_SET_POLARITY_RISING:
	case ST21NFC_SET_POLARITY_HIGH:
		/* the interrupt is level triggered either way */
		return 0;
	default:
		return -ENOTTY;
	}
}

static int st21nfc_open(struct inode *inode, struct file *f)
{
	struct st21nfc *nfc = container_of(f->private_data, struct st21nfc,
					   misc);
	int ret = 0;

	mutex_lock(&nfc->lock);
	if (nfc->open) {
		ret = -EBUSY;
	} else {
		nfc->open = true;
		st21nfc_power(nfc, true);
	}
	mutex_unlock(&nfc->lock);
	return ret;
}

static int st21nfc_release(struct inode *inode, struct file *f)
{
	struct st21nfc *nfc = to_nfc(f);

	mutex_lock(&nfc->lock);
	st21nfc_power(nfc, false);
	nfc->open = false;
	mutex_unlock(&nfc->lock);
	return 0;
}

static const struct file_operations st21nfc_fops = {
	.owner = THIS_MODULE,
	.open = st21nfc_open,
	.release = st21nfc_release,
	.read = st21nfc_read,
	.write = st21nfc_write,
	.poll = st21nfc_poll,
	.unlocked_ioctl = st21nfc_ioctl,
	.llseek = noop_llseek,
};

/* the PMIC's regmap, for the clock buffer: the node "mediatek,pmic" names */
static struct regmap *st21nfc_pmic(struct device *dev)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct mt6397_chip *chip;

	np = of_parse_phandle(dev->of_node, "mediatek,pmic", 0);
	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return NULL;
	chip = platform_get_drvdata(pdev);
	put_device(&pdev->dev);
	return chip ? chip->regmap : NULL;
}

static int st21nfc_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct st21nfc *nfc;
	int ret;

	nfc = devm_kzalloc(dev, sizeof(*nfc), GFP_KERNEL);
	if (!nfc)
		return -ENOMEM;
	nfc->client = client;
	mutex_init(&nfc->lock);
	init_waitqueue_head(&nfc->read_wq);
	i2c_set_clientdata(client, nfc);

	/* hibernate until opened */
	nfc->ven = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(nfc->ven))
		return dev_err_probe(dev, PTR_ERR(nfc->ven), "reset gpio\n");
	nfc->irq_gpio = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(nfc->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(nfc->irq_gpio), "irq gpio\n");
	nfc->pmic = st21nfc_pmic(dev);
	if (!nfc->pmic)
		dev_warn(dev, "no PMIC: the NFC clock buffer stays as it is\n");
	else
		regmap_write(nfc->pmic, MT6358_DCXO_CW00_CLR, XO_EXTBUF3_EN_M);

	if (!client->irq)
		client->irq = gpiod_to_irq(nfc->irq_gpio);
	ret = devm_request_irq(dev, client->irq, st21nfc_irq,
			       IRQF_TRIGGER_HIGH | IRQF_NO_AUTOEN, "st21nfc",
			       nfc);
	if (ret)
		return ret;

	nfc->misc.minor = MISC_DYNAMIC_MINOR;
	nfc->misc.name = "st21nfc";
	nfc->misc.fops = &st21nfc_fops;
	nfc->misc.mode = 0660;
	ret = misc_register(&nfc->misc);
	if (ret)
		return ret;
	dev_info(dev, "ST21NFC, hibernating until opened\n");
	return 0;
}

static void st21nfc_remove(struct i2c_client *client)
{
	struct st21nfc *nfc = i2c_get_clientdata(client);

	misc_deregister(&nfc->misc);
	st21nfc_power(nfc, false);
}

static const struct of_device_id st21nfc_of_match[] = {
	{ .compatible = "st,st21nfc" },
	{ }
};
MODULE_DEVICE_TABLE(of, st21nfc_of_match);

static struct i2c_driver st21nfc_driver = {
	.driver = {
		.name = "st21nfc",
		.of_match_table = st21nfc_of_match,
	},
	.probe = st21nfc_probe,
	.remove = st21nfc_remove,
};
module_i2c_driver(st21nfc_driver);

MODULE_DESCRIPTION("ST21NFC NCI controller as /dev/st21nfc");
MODULE_LICENSE("GPL");
