/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SND_SOC_CODEC_FS1860_H_
#define _SND_SOC_CODEC_FS1860_H_

#include <linux/i2c.h>

int fs1860_speaker_amp_probe(struct i2c_client *client,
			     const struct i2c_device_id *id);
int fs1860_speaker_amp_remove(struct i2c_client *client);
void fs1860_speaker_amp_shutdown(struct i2c_client *client);

#endif /* _SND_SOC_CODEC_FS1860_H_ */
