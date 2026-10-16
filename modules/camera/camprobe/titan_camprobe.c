// SPDX-License-Identifier: GPL-2.0
/*
 * titan_camprobe: power one camera sensor of the Titan Pocket the way the
 * stock kernel did and read its chip ID over I2C (camera bring-up, phase 0).
 *
 *   insmod titan_camprobe.ko cam=0   rear  OV16880 (i2c2)
 *   insmod titan_camprobe.ko cam=1   front OV8865  (i2c4)
 *   rmmod titan_camprobe             power it off again
 *
 * Board wiring, from the stock kernel's imgsensor tables and the dtbo
 * (see docs/camera.md):
 *   cam0: MCLK = CMMCLK0 (GPIO99 f1) <- topckgen camtg_sel, 24 MHz
 *         DOVDD vcamio 1.8 V, AVDD external LDO enabled by GPIO160,
 *         DVDD vcamd 1.2 V, PDN GPIO97, RST GPIO101
 *   cam1: MCLK = CMMCLK1 (GPIO100 f1) <- camtg2_sel, 24 MHz
 *         DOVDD vcamio 1.8 V, AVDD vcama1 2.8 V, DVDD vcamd 1.2 V,
 *         PDN GPIO98, RST GPIO102
 * Pins are driven through the pinctrl states the BSP's imgsensor node
 * (kd_camera_hw1) already defines.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#include <dt-bindings/clock/mt6771-clk.h>

static int cam;
module_param(cam, int, 0444);
MODULE_PARM_DESC(cam, "0 = rear OV16880, 1 = front OV8865");

struct cam_desc {
	const char *name;
	int i2c_bus;
	int camtg;			/* topckgen mux feeding this MCLK pin */
	const char *avdd;		/* regulator, or NULL: GPIO LDO */
	u8 id_hi, id_lo;		/* expected 0x300b, 0x300c */
	unsigned int step_ms;		/* delay after each power step */
};

static const struct cam_desc cams[2] = {
	{ "OV16880", 2, TOP_MUX_CAMTG,  NULL,     0x68, 0x80, 1 },
	{ "OV8865",  4, TOP_MUX_CAMTG2, "vcama1", 0x88, 0x65, 5 },
};

static const struct cam_desc *c;
static struct pinctrl *pctl;
static struct clk *mclk, *mclk_parent;
static struct regulator *dovdd, *avdd, *dvdd;
static bool mclk_on;

static int pin(const char *fmt, ...)
{
	struct pinctrl_state *st;
	char name[32];
	va_list ap;
	int ret;

	va_start(ap, fmt);
	vsnprintf(name, sizeof(name), fmt, ap);
	va_end(ap);
	st = pinctrl_lookup_state(pctl, name);
	if (IS_ERR(st)) {
		pr_err("titan_camprobe: no pin state %s\n", name);
		return PTR_ERR(st);
	}
	ret = pinctrl_select_state(pctl, st);
	if (ret)
		pr_err("titan_camprobe: pin state %s: %d\n", name, ret);
	return ret;
}

static struct clk *topck(int idx)
{
	struct of_phandle_args args = { .args_count = 1, .args = { idx } };
	struct clk *clk;

	args.np = of_find_compatible_node(NULL, NULL, "mediatek,topckgen");
	if (!args.np)
		return ERR_PTR(-ENODEV);
	clk = of_clk_get_from_provider(&args);
	of_node_put(args.np);
	return clk;
}

static int reg_on(struct regulator **r, const char *name, int uv)
{
	int ret;

	*r = regulator_get(NULL, name);
	if (IS_ERR(*r)) {
		ret = PTR_ERR(*r);
		*r = NULL;
		pr_err("titan_camprobe: regulator %s: %d\n", name, ret);
		return ret;
	}
	ret = regulator_set_voltage(*r, uv, uv);
	if (!ret)
		ret = regulator_enable(*r);
	if (ret) {
		pr_err("titan_camprobe: %s %d uV: %d\n", name, uv, ret);
		regulator_put(*r);
		*r = NULL;
	}
	return ret;
}

static void reg_off(struct regulator **r)
{
	if (*r) {
		regulator_disable(*r);
		regulator_put(*r);
		*r = NULL;
	}
}

static int read_reg(struct i2c_adapter *adap, u16 addr, u16 reg, u8 *val)
{
	u8 buf[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{ .addr = addr, .flags = 0, .len = 2, .buf = buf },
		{ .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};

	return i2c_transfer(adap, msgs, 2) == 2 ? 0 : -EIO;
}

static void power_off(void)
{
	pin("cam%d_rst0", cam);
	pin("cam%d_pnd0", cam);
	reg_off(&dvdd);
	if (c->avdd)
		reg_off(&avdd);
	else
		pin("cam0_ldo_vcama_0");
	reg_off(&dovdd);
	pin("cam%d_mclk_off", cam);
	if (mclk_on) {
		clk_disable_unprepare(mclk);
		mclk_on = false;
	}
}

static int power_on(void)
{
	int ret;

	/* MCLK: 24 MHz from univpll_192m_d8 out of the CMMCLK pin */
	ret = clk_set_parent(mclk, mclk_parent);
	if (!ret)
		ret = clk_prepare_enable(mclk);
	if (ret) {
		pr_err("titan_camprobe: MCLK: %d\n", ret);
		return ret;
	}
	mclk_on = true;
	pr_info("titan_camprobe: MCLK %lu Hz\n", clk_get_rate(mclk));
	ret = pin("cam%d_mclk_4mA", cam);
	if (ret)
		return ret;

	pin("cam%d_pnd0", cam);
	pin("cam%d_rst0", cam);
	mdelay(c->step_ms);

	ret = reg_on(&dovdd, "vcamio", 1800000);
	if (ret)
		return ret;
	mdelay(c->step_ms);
	if (c->avdd)
		ret = reg_on(&avdd, c->avdd, 2800000);
	else
		ret = pin("cam0_ldo_vcama_1");
	if (ret)
		return ret;
	mdelay(c->step_ms);
	ret = reg_on(&dvdd, "vcamd", 1200000);
	if (ret)
		return ret;
	mdelay(5);

	pin("cam%d_pnd1", cam);
	mdelay(c->step_ms);
	pin("cam%d_rst1", cam);
	mdelay(10);	/* > 8192 MCLK cycles before the first SCCB access */
	return 0;
}

static int __init camprobe_init(void)
{
	static const u16 addrs[] = { 0x36, 0x10 };
	struct platform_device *pdev;
	struct device_node *np;
	struct i2c_adapter *adap;
	int ret, i;

	if (cam < 0 || cam > 1)
		return -EINVAL;
	c = &cams[cam];

	np = of_find_compatible_node(NULL, NULL, "mediatek,imgsensor");
	pdev = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	if (!pdev)
		return -ENODEV;
	pctl = pinctrl_get(&pdev->dev);
	put_device(&pdev->dev);
	if (IS_ERR(pctl))
		return PTR_ERR(pctl);

	mclk = topck(c->camtg);
	mclk_parent = topck(TOP_UNIVP_192M_D8);
	if (IS_ERR(mclk) || IS_ERR(mclk_parent)) {
		pr_err("titan_camprobe: camtg clocks not found\n");
		ret = -ENODEV;
		goto err_clk;
	}

	adap = i2c_get_adapter(c->i2c_bus);
	if (!adap) {
		ret = -ENODEV;
		goto err_clk;
	}

	ret = power_on();
	if (ret) {
		i2c_put_adapter(adap);
		goto err_power;
	}

	ret = -ENODEV;
	for (i = 0; i < ARRAY_SIZE(addrs); i++) {
		u8 a = 0, b = 0, d = 0;

		if (read_reg(adap, addrs[i], 0x300b, &a) ||
		    read_reg(adap, addrs[i], 0x300c, &b)) {
			pr_info("titan_camprobe: %s: i2c%d 0x%02x: no answer\n",
				c->name, c->i2c_bus, addrs[i]);
			continue;
		}
		read_reg(adap, addrs[i], 0x300a, &d);
		pr_info("titan_camprobe: %s: i2c%d 0x%02x: chip id %02x %02x %02x%s\n",
			c->name, c->i2c_bus, addrs[i], d, a, b,
			a == c->id_hi && b == c->id_lo ? " (match)" : " (unexpected)");
		if (a == c->id_hi && b == c->id_lo)
			ret = 0;
	}
	i2c_put_adapter(adap);
	if (ret)
		goto err_power;
	pr_info("titan_camprobe: %s powered; rmmod to power it off\n", c->name);
	return 0;

err_power:
	power_off();	/* stay loaded only when the sensor answered */
err_clk:
	if (!IS_ERR_OR_NULL(mclk))
		clk_put(mclk);
	if (!IS_ERR_OR_NULL(mclk_parent))
		clk_put(mclk_parent);
	pinctrl_put(pctl);
	return ret;
}

static void __exit camprobe_exit(void)
{
	power_off();
	clk_put(mclk);
	clk_put(mclk_parent);
	pinctrl_put(pctl);
	pr_info("titan_camprobe: %s powered off\n", c->name);
}

module_init(camprobe_init);
module_exit(camprobe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Titan Pocket camera power-up and chip ID probe");
