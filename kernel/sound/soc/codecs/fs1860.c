// SPDX-License-Identifier: GPL-2.0
/*
 * FourSemi FS1860 smart speaker amplifier (Unihertz Titan Pocket).
 *
 * On stock Android this amp is driven entirely from the vendor HAL
 * (libfs1860.so) through a raw I2C pass-through device. The sequences here
 * were recovered from that library, and the steady-state configuration was
 * captured from the running phone (see fs1860_config below).
 *
 * Registers are 16-bit big-endian. The register map closely follows the
 * NXP TFA98xx family: 0x00 status, 0x04 I2S control, 0x06 volume,
 * 0x09 system control (bit0 power-down, bit3 amp enable).
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#include "fs1860.h"

#define FS1860_DRV_NAME		"fs1860"
#define FS1860_PRESET_FW	"fs16xx_01s_mono.preset"

#define FS1860_REG_STATUS	0x00
#define FS1860_STATUS_PLLS	BIT(1)
#define FS1860_REG_REVID	0x03
#define FS1860_REVID		0x0703
#define FS1860_REG_I2SCTRL	0x04
#define FS1860_REG_AUDIOCTRL	0x06
#define FS1860_REG_SYSCTRL	0x09
#define FS1860_SYSCTRL_PWDN	BIT(0)
#define FS1860_SYSCTRL_AMPE	BIT(3)
#define FS1860_REG_ACCKEY	0x0b
#define FS1860_ACCKEY_UNLOCK	0xca91
#define FS1860_REG_DSPCOEF	0x82
#define FS1860_REG_COEFADDR	0x86
#define FS1860_REG_DSPRAM	0xa2
#define FS1860_REG_DSPADDR	0xa6
#define FS1860_REG_TEMPPROT	0xaf
#define FS1860_REG_DSPSTATUS	0xbd
#define FS1860_REG_PWRKEY	0xc4

/* Preset file layout (libfs1860 v0x0703): 468-byte header, then coefficients */
#define FS1860_PRESET_VERSION	0x0703
#define FS1860_PRESET_HDR_LEN	468
#define FS1860_PRESET_NCOEF	42	/* u16 count field offset */
#define FS1860_PRESET_RAM	144	/* 81 x u32 DSP RAM words */
#define FS1860_PRESET_RAM_WORDS	81

struct fs1860_reg {
	u8 reg;
	u16 val;
};

/* Fs16xx_reg_defaults from libfs1860.so, written first by fs16xx_init() */
static const struct fs1860_reg fs1860_defaults[] = {
	{ 0x0b, 0xca91 }, { 0x04, 0x800b }, { 0x06, 0xff00 }, { 0xc0, 0xb54e },
	{ 0xc4, 0x000f }, { 0xae, 0x0010 }, { 0xb9, 0xffff }, { 0x09, 0x0009 },
	{ 0xd7, 0x60a0 }, { 0xa1, 0x5802 }, { 0x99, 0x0000 }, { 0x0b, 0x0000 },
};

/*
 * Configuration registers as left by the stock HAL after init (speaker
 * idle), read back from this phone. Covers the 44.1/48k preset selection,
 * mono channel select, mute/volume and protection thresholds.
 * 0x62-0x69 are speaker-protection coefficients the HAL derives from the
 * factory R25 calibration in OTP (0xe8); they are specific to this unit.
 */
static const struct fs1860_reg fs1860_config[] = {
	{ 0x04, 0x801b }, { 0x06, 0xff00 }, { 0x08, 0x01fe }, { 0x0a, 0x0400 },
	{ 0x62, 0x7f10 }, { 0x63, 0x8e4f }, { 0x64, 0x8c27 }, { 0x65, 0x8c27 },
	{ 0x66, 0x8c27 }, { 0x67, 0x8c27 }, { 0x68, 0x89a6 }, { 0x69, 0x873c },
	{ 0x6a, 0x0a05 }, { 0x6b, 0x2015 }, { 0x6c, 0x402a },
	{ 0x72, 0x0a53 }, { 0x73, 0xeb59 }, { 0x74, 0x0a53 },
	{ 0x77, 0x000b }, { 0x78, 0x0016 }, { 0x79, 0x000b },
	{ 0x7e, 0x1000 }, { 0x7f, 0x0000 },
	{ 0x89, 0x9080 }, { 0x8a, 0x0345 }, { 0x8b, 0x18bc }, { 0x8c, 0x14c7 },
	{ 0xa1, 0x5802 }, { 0xa7, 0x0004 }, { 0xa8, 0x00e5 }, { 0xa9, 0x02d8 },
	{ 0xaa, 0x0035 }, { 0xad, 0x00a3 }, { 0xae, 0x0010 }, { 0xb9, 0xffff },
	{ 0xba, 0x0031 }, { 0xc0, 0x4aae }, { 0xc1, 0x0168 }, { 0xc2, 0x00f0 },
	{ 0xc3, 0x0006 },
};

struct fs1860 {
	struct i2c_client *client;
	struct mutex lock;
	bool initialized;
	bool on;
};

static int fs1860_read(struct fs1860 *fs, u8 reg)
{
	return i2c_smbus_read_word_swapped(fs->client, reg);
}

static int fs1860_write(struct fs1860 *fs, u8 reg, u16 val)
{
	return i2c_smbus_write_word_swapped(fs->client, reg, val);
}

static int fs1860_write_table(struct fs1860 *fs, const struct fs1860_reg *t,
			      int n)
{
	int i, ret;

	for (i = 0; i < n; i++) {
		ret = fs1860_write(fs, t[i].reg, t[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

static int fs1860_bulk_write(struct fs1860 *fs, u8 reg, const u8 data[4])
{
	u8 buf[5] = { reg, data[0], data[1], data[2], data[3] };
	int ret = i2c_master_send(fs->client, buf, sizeof(buf));

	return ret == sizeof(buf) ? 0 : (ret < 0 ? ret : -EIO);
}

/* Poll until (reg & mask) == want, up to tries x 1ms */
static int fs1860_wait(struct fs1860 *fs, u8 reg, u16 mask, u16 want, int tries)
{
	while (tries--) {
		int v = fs1860_read(fs, reg);

		if (v >= 0 && (v & mask) == want)
			return 0;
		usleep_range(1000, 1200);
	}
	return -ETIMEDOUT;
}

/* fs16xx_load_presets_to_dev(): DSP RAM and coefficient upload */
static int fs1860_load_preset(struct fs1860 *fs, const struct firmware *fw)
{
	const u8 *p = fw->data;
	u16 ncoef = p[FS1860_PRESET_NCOEF] | p[FS1860_PRESET_NCOEF + 1] << 8;
	int sysctrl, i, ret;
	u8 b[4];

	if (fw->size < FS1860_PRESET_HDR_LEN ||
	    (p[0] | p[1] << 8) != FS1860_PRESET_VERSION ||
	    fw->size != FS1860_PRESET_HDR_LEN + 2 * ncoef || ncoef & 1) {
		dev_err(&fs->client->dev, "unexpected preset file layout\n");
		return -EINVAL;
	}

	sysctrl = fs1860_read(fs, FS1860_REG_SYSCTRL);
	if (sysctrl < 0)
		return sysctrl;
	fs1860_write(fs, FS1860_REG_SYSCTRL, sysctrl & ~FS1860_SYSCTRL_AMPE);
	fs1860_write(fs, FS1860_REG_ACCKEY, FS1860_ACCKEY_UNLOCK);
	if (fs1860_wait(fs, FS1860_REG_DSPSTATUS, 0x3, 0, 50))
		dev_warn(&fs->client->dev, "DSP did not stop\n");

	ret = fs1860_write(fs, FS1860_REG_DSPADDR, 0);
	for (i = 0; !ret && i < FS1860_PRESET_RAM_WORDS; i++) {
		const u8 *w = p + FS1860_PRESET_RAM + 4 * i;	/* LE u32 */

		b[0] = w[1];
		b[1] = w[0];
		b[2] = 0;
		b[3] = w[2];
		ret = fs1860_bulk_write(fs, FS1860_REG_DSPRAM, b);
	}

	if (!ret)
		ret = fs1860_write(fs, FS1860_REG_COEFADDR, 0);
	for (i = 0; !ret && i < ncoef / 2; i++) {
		const u8 *c = p + FS1860_PRESET_HDR_LEN + 4 * i;	/* 2 x LE u16 */

		ret = fs1860_write(fs, FS1860_REG_COEFADDR, i);
		b[0] = c[1];
		b[1] = c[0];
		b[2] = c[3];
		b[3] = c[2];
		if (!ret)
			ret = fs1860_bulk_write(fs, FS1860_REG_DSPCOEF, b);
	}

	fs1860_write(fs, FS1860_REG_ACCKEY, 0);
	fs1860_write(fs, FS1860_REG_SYSCTRL, sysctrl);
	return ret;
}

static int fs1860_init_chip(struct fs1860 *fs)
{
	const struct firmware *fw;
	int ret;

	ret = fs1860_write_table(fs, fs1860_defaults,
				 ARRAY_SIZE(fs1860_defaults));
	if (ret)
		return ret;

	ret = request_firmware(&fw, FS1860_PRESET_FW, &fs->client->dev);
	if (ret) {
		dev_err(&fs->client->dev, "missing %s: %d\n",
			FS1860_PRESET_FW, ret);
		return ret;
	}
	ret = fs1860_load_preset(fs, fw);
	release_firmware(fw);
	if (ret)
		return ret;

	fs1860_write(fs, FS1860_REG_ACCKEY, FS1860_ACCKEY_UNLOCK);
	ret = fs1860_write_table(fs, fs1860_config, ARRAY_SIZE(fs1860_config));
	fs1860_write(fs, FS1860_REG_ACCKEY, 0);
	if (ret)
		return ret;

	dev_info(&fs->client->dev, "initialized with %s\n", FS1860_PRESET_FW);
	return 0;
}

static int fs1860_ensure_init(struct fs1860 *fs)
{
	int ret = 0;

	if (!fs->initialized) {
		ret = fs1860_init_chip(fs);
		fs->initialized = !ret;
	}
	return ret;
}

/*
 * fs16xx_powerdown(dev, 0) + the AMPE cycle from fs16xx_set_preset() +
 * fs16xx_set_tempr_protection(dev, 1). The DSP only starts on an AMPE rising
 * edge while the I2S clock is present, so power up with AMPE clear, wait for
 * the PLL to lock, then enable the amplifier.
 */
static int fs1860_speaker_on(struct fs1860 *fs)
{
	int v, ret;

	fs1860_write(fs, FS1860_REG_PWRKEY, 0x000f);
	v = fs1860_read(fs, FS1860_REG_SYSCTRL);
	if (v < 0)
		return v;
	v &= ~(FS1860_SYSCTRL_PWDN | FS1860_SYSCTRL_AMPE);
	ret = fs1860_write(fs, FS1860_REG_SYSCTRL, v);
	if (ret)
		return ret;

	/* The PLL locks to the I2S bit clock; no clock means no audio */
	if (fs1860_wait(fs, FS1860_REG_STATUS, FS1860_STATUS_PLLS,
			FS1860_STATUS_PLLS, 100))
		dev_warn(&fs->client->dev, "PLL not locked (no I2S clock?)\n");
	fs1860_wait(fs, FS1860_REG_DSPSTATUS, 0x3, 0, 50);

	ret = fs1860_write(fs, FS1860_REG_SYSCTRL, v | FS1860_SYSCTRL_AMPE);
	if (ret)
		return ret;
	usleep_range(5000, 6000);	/* boost soft-start */

	ret = fs1860_write(fs, FS1860_REG_TEMPPROT, 0x362b);
	fs->on = !ret;
	dev_dbg(&fs->client->dev, "speaker on: status=0x%04x dsp=0x%04x\n",
		fs1860_read(fs, FS1860_REG_STATUS),
		fs1860_read(fs, FS1860_REG_DSPSTATUS));
	return ret;
}

/* fs16xx_powerdown(dev, 1) */
static int fs1860_speaker_off(struct fs1860 *fs)
{
	int v;

	fs1860_write(fs, FS1860_REG_TEMPPROT, 0x7623);
	v = fs1860_read(fs, FS1860_REG_SYSCTRL);
	if (v >= 0)
		fs1860_write(fs, FS1860_REG_SYSCTRL, v | FS1860_SYSCTRL_PWDN);
	if (fs1860_wait(fs, FS1860_REG_DSPSTATUS, 0x3, 0, 50))
		dev_warn(&fs->client->dev, "DSP did not stop\n");
	fs1860_write(fs, FS1860_REG_PWRKEY, 0x0000);
	fs->on = false;
	return 0;
}

/* fs16xx_set_samplerate(): rate code in I2SCTRL[15:12], clear [7:6] */
static int fs1860_rate_code(unsigned int rate)
{
	switch (rate) {
	case 8000:	return 0x0;
	case 16000:	return 0x3;
	case 32000:	return 0x6;
	case 44100:	return 0x7;
	case 48000:	return 0x8;
	default:	return -EINVAL;
	}
}

static int fs1860_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	struct fs1860 *fs = snd_soc_codec_get_drvdata(dai->codec);
	int code = fs1860_rate_code(params_rate(params));
	int v, ret;

	dev_dbg(&fs->client->dev, "%s: rate=%u ch=%u\n", __func__,
		 params_rate(params), params_channels(params));
	if (code < 0)
		return code;

	mutex_lock(&fs->lock);
	ret = fs1860_ensure_init(fs);
	if (!ret) {
		v = fs1860_read(fs, FS1860_REG_I2SCTRL);
		ret = v < 0 ? v : fs1860_write(fs, FS1860_REG_I2SCTRL,
					       (v & 0x0f3f) | code << 12);
	}
	mutex_unlock(&fs->lock);
	return ret;
}

static int fs1860_digital_mute(struct snd_soc_dai *dai, int mute)
{
	struct fs1860 *fs = snd_soc_codec_get_drvdata(dai->codec);
	int sysctrl, ret = 0;

	mutex_lock(&fs->lock);
	/* Trust the chip, not our flag: it may have been powered down behind us */
	sysctrl = fs1860_read(fs, FS1860_REG_SYSCTRL);
	if (sysctrl >= 0)
		fs->on = !(sysctrl & FS1860_SYSCTRL_PWDN);
	dev_dbg(&fs->client->dev, "%s: mute=%d sysctrl=0x%04x\n", __func__,
		 mute, sysctrl);

	if (!mute && !fs->on) {
		ret = fs1860_ensure_init(fs);
		if (!ret)
			ret = fs1860_speaker_on(fs);
	} else if (mute && fs->on) {
		ret = fs1860_speaker_off(fs);
	}
	if (ret)
		dev_err(&fs->client->dev, "speaker %s failed: %d\n",
			mute ? "off" : "on", ret);
	mutex_unlock(&fs->lock);
	return ret;
}

static int fs1860_prepare(struct snd_pcm_substream *substream,
			  struct snd_soc_dai *dai)
{
	dev_dbg(dai->dev, "%s\n", __func__);
	/* Power up here as well: prepare is sleepable and precedes any data */
	return fs1860_digital_mute(dai, 0);
}

static int fs1860_trigger(struct snd_pcm_substream *substream, int cmd,
			  struct snd_soc_dai *dai)
{
	dev_dbg(dai->dev, "%s: cmd=%d\n", __func__, cmd);
	return 0;
}

static const struct snd_soc_dai_ops fs1860_dai_ops = {
	.hw_params	= fs1860_hw_params,
	.prepare	= fs1860_prepare,
	.trigger	= fs1860_trigger,
	.digital_mute	= fs1860_digital_mute,
};

static struct snd_soc_dai_driver fs1860_dai = {
	.name = "fs1860-aif",
	.playback = {
		.stream_name	= "Playback",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
				  SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
				  SNDRV_PCM_RATE_48000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S24_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &fs1860_dai_ops,
};

/* Volume: AUDIOCTRL[15:8], 0xff is the HAL's normal (full) level, -0.5dB steps */
static int fs1860_volume_get(struct snd_kcontrol *kc,
			     struct snd_ctl_elem_value *uc)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kc);
	struct fs1860 *fs = snd_soc_codec_get_drvdata(codec);
	int v = fs1860_read(fs, FS1860_REG_AUDIOCTRL);

	if (v < 0)
		return v;
	uc->value.integer.value[0] = v >> 8;
	return 0;
}

static int fs1860_volume_put(struct snd_kcontrol *kc,
			     struct snd_ctl_elem_value *uc)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kc);
	struct fs1860 *fs = snd_soc_codec_get_drvdata(codec);
	long vol = clamp_t(long, uc->value.integer.value[0], 0, 0xff);
	int v, ret;

	mutex_lock(&fs->lock);
	v = fs1860_read(fs, FS1860_REG_AUDIOCTRL);
	ret = v < 0 ? v : fs1860_write(fs, FS1860_REG_AUDIOCTRL,
				       (v & 0xff) | vol << 8);
	mutex_unlock(&fs->lock);
	return ret ? ret : 1;
}

static const DECLARE_TLV_DB_SCALE(fs1860_vol_tlv, -12750, 50, 1);

static const struct snd_kcontrol_new fs1860_controls[] = {
	SOC_SINGLE_EXT_TLV("Speaker Amp Volume", SND_SOC_NOPM, 0, 0xff, 0,
			   fs1860_volume_get, fs1860_volume_put,
			   fs1860_vol_tlv),
};

static struct snd_soc_codec_driver fs1860_codec = {
	.component_driver = {
		.controls	= fs1860_controls,
		.num_controls	= ARRAY_SIZE(fs1860_controls),
	},
};

int fs1860_speaker_amp_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct fs1860 *fs;
	int rev;

	rev = i2c_smbus_read_word_swapped(client, FS1860_REG_REVID);
	if (rev != FS1860_REVID)
		return -ENODEV;

	fs = devm_kzalloc(&client->dev, sizeof(*fs), GFP_KERNEL);
	if (!fs)
		return -ENOMEM;
	fs->client = client;
	mutex_init(&fs->lock);
	i2c_set_clientdata(client, fs);

	/* Keep the amp powered down until a stream starts */
	fs1860_write(fs, FS1860_REG_SYSCTRL,
		     FS1860_SYSCTRL_PWDN | FS1860_SYSCTRL_AMPE);

	/* The MTK machine driver binds the speaker DAI link by this name */
	dev_set_name(&client->dev, "%s", FS1860_DRV_NAME);
	dev_info(&client->dev, "FS1860 rev 0x%04x\n", rev);

	return snd_soc_register_codec(&client->dev, &fs1860_codec,
				      &fs1860_dai, 1);
}
EXPORT_SYMBOL(fs1860_speaker_amp_probe);

int fs1860_speaker_amp_remove(struct i2c_client *client)
{
	snd_soc_unregister_codec(&client->dev);
	return 0;
}
EXPORT_SYMBOL(fs1860_speaker_amp_remove);

void fs1860_speaker_amp_shutdown(struct i2c_client *client)
{
	struct fs1860 *fs = i2c_get_clientdata(client);

	if (fs && fs->on) {
		mutex_lock(&fs->lock);
		fs1860_speaker_off(fs);
		mutex_unlock(&fs->lock);
	}
}
EXPORT_SYMBOL(fs1860_speaker_amp_shutdown);

MODULE_DESCRIPTION("FourSemi FS1860 smart amplifier (Titan Pocket)");
MODULE_LICENSE("GPL v2");
