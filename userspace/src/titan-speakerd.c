/*
 * titan-speakerd: power the loudspeaker path while something plays.
 *
 * The speaker is fed by AFE playback devices that only carry samples; the
 * path itself has to be switched on around them (docs/audio.md):
 *   1. I2S clock generation + amp pin mux: Audio_i2s0_SideGen_Switch On48000
 *   2. the FS1860 amp: device 30 (Speaker_PLayback), a hostless link that
 *      is opened and prepared but never started; close powers the amp off
 * Clock first: the amp's DSP only starts on an enable edge with the I2S
 * clock running, so the clock gets CLOCK_SETTLE_MS before the amp is
 * enabled. Even then the DSP occasionally stays idle (status register 0xBD
 * = 0x0803 instead of 0x5803/0x3803: speaker silent although everything
 * else runs), so after switching on, the status is checked over I2C and the
 * amp is toggled again (a new enable edge) if the DSP hasn't started.
 *
 * Watched devices (any of them open -> speaker on):
 *   7   I2S0DL1_PLayback  PCM samples (PipeWire's sink, tinyplay)
 *   21  FM_I2S_Playback   FM radio (titan-radio)
 * After they are all closed for LINGER_MS the amp and clock go off again.
 * PipeWire closes its device after 5 s of silence, so the amp is off when
 * nothing plays.
 *
 * The speaker level (the amp's gain, /var/lib/titan-audio/volume, shared with
 * the volume keys, the panel indicator and the mixer) is re-applied each time
 * the amp is switched on: the codec driver's one-time chip init on the first
 * stream after boot writes full gain (0xff), and a power loss resets it too.
 *
 * Only 48 kHz: the clock rate is fixed, PipeWire's sink runs at 48 kHz.
 */
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <tinyalsa/asoundlib.h>

#define CARD		0
#define AMP_DEVICE	30
#define POLL_MS		200
#define LINGER_MS	2000
#define RETRY_MS	2000	/* after a failed switch-on */
#define CLOCK_SETTLE_MS	50
#define DSP_START_MS	1500	/* DSP must be running this long after enable */
#define DSP_RETRIES	3
#define AMP_I2C		"/dev/i2c-3"	/* FS1860 at 3-0034 */
#define AMP_ADDR	0x34
#define AMP_DSP_STATUS	0xBD
#define VOL_FILE	"/var/lib/titan-audio/volume"
#define VOL_CONTROL	"Speaker Amp Volume"
#define VOL_MAX		224	/* same cap as titan-powerkey and titan-mixer */
#define CLOCK_CTL	"Audio_i2s0_SideGen_Switch"
#define CLOCK_ON	"On48000"

static const unsigned int watched[] = { 7, 21 };

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	stop = 1;
}

/* a playback substream is in use unless its status file says "closed" */
static int device_open(unsigned int device)
{
	char path[64], buf[16] = "";
	FILE *f;

	snprintf(path, sizeof path, "/proc/asound/card%d/pcm%up/sub0/status",
		 CARD, device);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (!fgets(buf, sizeof buf, f))
		buf[0] = '\0';
	fclose(f);
	return strncmp(buf, "closed", 6) != 0;
}

static int any_open(void)
{
	unsigned int i;

	for (i = 0; i < sizeof watched / sizeof watched[0]; i++)
		if (device_open(watched[i]))
			return 1;
	return 0;
}

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/*
 * 1: the amp's DSP is processing, 0: idle, -1: can't tell. Reads one
 * register beside the kernel driver (the I2C core serializes transfers).
 */
static int amp_dsp_running(void)
{
	union i2c_smbus_data data;
	struct i2c_smbus_ioctl_data args = {
		.read_write = I2C_SMBUS_READ,
		.command = AMP_DSP_STATUS,
		.size = I2C_SMBUS_WORD_DATA,
		.data = &data,
	};
	unsigned int status;
	int fd, ret;

	fd = open(AMP_I2C, O_RDWR);
	if (fd < 0)
		return -1;
	ret = ioctl(fd, I2C_SLAVE_FORCE, AMP_ADDR);
	if (ret == 0)
		ret = ioctl(fd, I2C_SMBUS, &args);
	close(fd);
	if (ret < 0)
		return -1;
	/* SMBus words are little-endian, the amp's registers big-endian */
	status = ((data.word & 0xff) << 8) | (data.word >> 8);
	return (status & 0xf000) != 0;	/* 0x0803 idle, 0x5803/0x3803 running */
}

static int set_clock(const char *value)
{
	struct mixer *mixer = mixer_open(CARD);
	struct mixer_ctl *ctl;
	int ret = -1;

	if (!mixer) {
		fprintf(stderr, "titan-speakerd: cannot open mixer\n");
		return -1;
	}
	ctl = mixer_get_ctl_by_name(mixer, CLOCK_CTL);
	if (ctl)
		ret = mixer_ctl_set_enum_by_string(ctl, value);
	if (ret)
		fprintf(stderr, "titan-speakerd: %s = %s failed\n", CLOCK_CTL, value);
	mixer_close(mixer);
	return ret;
}

/* set the amp to the shared speaker level */
static void apply_level(void)
{
	struct mixer *mixer;
	struct mixer_ctl *ctl;
	FILE *f = fopen(VOL_FILE, "r");
	int level;

	if (!f)
		return;
	if (fscanf(f, "%d", &level) != 1 || level < 0 || level > VOL_MAX) {
		fclose(f);
		return;
	}
	fclose(f);
	mixer = mixer_open(CARD);
	if (!mixer)
		return;
	ctl = mixer_get_ctl_by_name(mixer, VOL_CONTROL);
	if (!ctl || mixer_ctl_set_value(ctl, 0, level))
		fprintf(stderr, "titan-speakerd: cannot set %s\n", VOL_CONTROL);
	mixer_close(mixer);
}

/* open + prepare the amp link: the FS1860 powers up with an enable edge */
static struct pcm *amp_link_open(void)
{
	struct pcm_config cfg = {
		.channels = 2,
		.rate = 48000,
		.format = PCM_FORMAT_S16_LE,
		.period_size = 1024,
		.period_count = 4,
	};
	struct pcm *pcm = pcm_open(CARD, AMP_DEVICE, PCM_OUT, &cfg);

	if (!pcm || !pcm_is_ready(pcm) || pcm_prepare(pcm)) {
		fprintf(stderr, "titan-speakerd: amp link: %s\n",
			pcm ? pcm_get_error(pcm) : "open failed");
		if (pcm)
			pcm_close(pcm);
		return NULL;
	}
	apply_level();	/* after the driver's chip init, which writes full gain */
	return pcm;
}

static struct pcm *amp_on(void)
{
	struct pcm *pcm;

	if (set_clock(CLOCK_ON))
		return NULL;
	sleep_ms(CLOCK_SETTLE_MS);
	pcm = amp_link_open();
	if (!pcm) {
		set_clock("Off");
		return NULL;
	}
	fprintf(stderr, "titan-speakerd: speaker on\n");
	return pcm;
}

static void amp_off(struct pcm *pcm)
{
	pcm_close(pcm);
	set_clock("Off");
	fprintf(stderr, "titan-speakerd: speaker off\n");
}

int main(void)
{
	struct timespec tick = { 0, POLL_MS * 1000000L };
	struct pcm *amp = NULL;
	int idle_ms = 0, backoff_ms = 0;
	int check_ms = -1, retries = 0;	/* DSP start check, -1 = done */

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	setvbuf(stderr, NULL, _IOLBF, 0);

	while (!stop) {
		if (backoff_ms > 0) {
			backoff_ms -= POLL_MS;
		} else if (any_open()) {
			idle_ms = 0;
			if (!amp) {
				amp = amp_on();
				if (!amp)
					backoff_ms = RETRY_MS;
				check_ms = 0;
				retries = 0;
			}
		} else if (amp && (idle_ms += POLL_MS) >= LINGER_MS) {
			amp_off(amp);
			amp = NULL;
		}

		/* the DSP must have started; else give the amp a new enable edge */
		if (amp && check_ms >= 0) {
			int running = amp_dsp_running();

			if (running != 0) {
				check_ms = -1;	/* running, or no way to tell */
			} else if ((check_ms += POLL_MS) >= DSP_START_MS) {
				if (retries++ == DSP_RETRIES) {
					fprintf(stderr, "titan-speakerd: amp DSP did not start\n");
					check_ms = -1;
				} else {
					fprintf(stderr, "titan-speakerd: amp DSP idle, re-enabling\n");
					pcm_close(amp);
					sleep_ms(CLOCK_SETTLE_MS);
					amp = amp_link_open();
					check_ms = 0;
					if (!amp) {
						set_clock("Off");
						backoff_ms = RETRY_MS;
					}
				}
			}
		}
		nanosleep(&tick, NULL);
	}
	if (amp)
		amp_off(amp);
	return 0;
}
