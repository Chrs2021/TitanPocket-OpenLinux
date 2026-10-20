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
 *
 * Headphones: the same samples also reach the MT6358 codec's DAC. With
 * headphones (or a line-out cable) in the jack, as reported by the PMIC's
 * accessory detection (input device "ACCDET", SW_HEADPHONE_INSERT /
 * SW_LINEOUT_INSERT), the speaker stays off and the codec's headphone amps
 * (Audio_Amp_L/R_Switch, the stock HAL's headphone_output path) are switched
 * on instead, HP_DELAY_MS after the stream opened: switched on before the
 * stream runs, they stay silent. Plugging in or out while something plays
 * moves the sound over. Each output keeps its own level: on a jack change
 * the shared level file is saved for the old output (volume.speaker,
 * volume.headphones) and loaded with the new one's, so the volume keys, the
 * mixer and the panel indicator always work on the output in use. For the
 * headphones the level is applied here whenever the file changes.
 *
 * Headphone loudness: full scale is HP_MAX_DB (-8 dB, the bottom tenth of
 * the codec's -10..+8 dB headphone gain, which is loud enough), and the
 * level spreads HP_RANGE_DB below it, 1.5 dB per volume-key step. The
 * codec's gain only has 1 dB steps down to -10 dB, so it is set to the
 * step at or above the target and PipeWire's sink volume (software, the
 * speaker is off meanwhile) takes the rest; back on the speaker the sink
 * returns to 100 %. FM bypasses PipeWire: its finer level is the Radio
 * app's own (AFE) gain. Level 0 mutes (-40 dB, sink 0).
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>
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
#define VOL_DEFAULT	160
#define CLOCK_CTL	"Audio_i2s0_SideGen_Switch"
#define CLOCK_ON	"On48000"
#define LEVEL_DIR	"/var/lib/titan-audio"
#define VOL_SPEAKER	LEVEL_DIR "/volume.speaker"
#define VOL_HEADPHONES	LEVEL_DIR "/volume.headphones"
#define VOL_OWNER	LEVEL_DIR "/volume.output"	/* whose level VOL_FILE holds */
#define HP_DEFAULT	107	/* about -30 dB */
#define HP_DELAY_MS	300
#define HP_MAX_DB	-8.0	/* level VOL_MAX */
#define HP_RANGE_DB	42.0	/* level 0+ .. VOL_MAX: 1.5 dB per key step of 8 */
#define HP_PGA_MIN	-10	/* the codec's lowest step above -40 dB */
#define PACTL_ENV	"XDG_RUNTIME_DIR=/run/user/0"	/* PipeWire runs in root's session */
#define JACK_NAME	"ACCDET"

enum output { OUT_NONE, OUT_SPEAKER, OUT_HEADPHONES };

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

static int read_level(const char *path, int def)
{
	FILE *f = fopen(path, "r");
	int level;

	if (!f)
		return def;
	if (fscanf(f, "%d", &level) != 1 || level < 0 || level > VOL_MAX)
		level = def;
	fclose(f);
	return level;
}

/* atomically, so the panel indicator (watching for renames) sees it */
static void write_text(const char *path, const char *text)
{
	char tmp[128];
	FILE *f;

	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fputs(text, f);
	if (fclose(f) == 0)
		rename(tmp, path);
}

static void write_level(const char *path, int level)
{
	char buf[16];

	snprintf(buf, sizeof buf, "%d\n", level);
	write_text(path, buf);
}

/* the jack: "ACCDET" among the input devices; -1 if absent */
static int jack_open(void)
{
	struct dirent *e;
	DIR *d = opendir("/dev/input");
	int fd = -1;

	while (d && (e = readdir(d))) {
		char path[300], name[64] = "";

		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) >= 0 &&
		    !strcmp(name, JACK_NAME))
			break;
		close(fd);
		fd = -1;
	}
	if (d)
		closedir(d);
	return fd;
}

/* headphones or a line-out cable in the jack */
static int headphones_in(void)
{
	static int fd = -1;
	unsigned long sw[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };

	if (fd < 0 && (fd = jack_open()) < 0)
		return 0;
	if (ioctl(fd, EVIOCGSW(sizeof sw), sw) < 0) {
		close(fd);
		fd = -1;
		return 0;
	}
	return (sw[0] >> SW_HEADPHONE_INSERT & 1) || (sw[0] >> SW_LINEOUT_INSERT & 1);
}

/* the shared level file follows the output: save the old one's, load the new one's */
static void switch_levels(int to_headphones)
{
	char owner[32] = "";
	FILE *f = fopen(VOL_OWNER, "r");
	int from_headphones, cur;

	if (f) {
		if (!fgets(owner, sizeof owner, f))
			owner[0] = '\0';
		fclose(f);
	}
	/* no owner yet: the level file always belonged to the speaker */
	from_headphones = !strncmp(owner, "headphones", 10);
	mkdir(LEVEL_DIR, 0755);
	if (from_headphones == to_headphones) {
		if (!owner[0])
			write_text(VOL_OWNER, "speaker\n");
		return;
	}
	cur = read_level(VOL_FILE, -1);
	if (cur >= 0)
		write_level(from_headphones ? VOL_HEADPHONES : VOL_SPEAKER, cur);
	if (to_headphones)
		write_level(VOL_FILE, read_level(VOL_HEADPHONES, HP_DEFAULT));
	else
		write_level(VOL_FILE, read_level(VOL_SPEAKER, cur >= 0 ? cur : VOL_DEFAULT));
	write_text(VOL_OWNER, to_headphones ? "headphones\n" : "speaker\n");
	fprintf(stderr, "titan-speakerd: %s, level %d\n",
		to_headphones ? "headphones in" : "headphones out", read_level(VOL_FILE, -1));
}

static int set_enum(const char *control, const char *value)
{
	struct mixer *mixer = mixer_open(CARD);
	struct mixer_ctl *ctl;
	int ret = -1;

	if (!mixer)
		return -1;
	ctl = mixer_get_ctl_by_name(mixer, control);
	if (ctl)
		ret = mixer_ctl_set_enum_by_string(ctl, value);
	if (ret)
		fprintf(stderr, "titan-speakerd: %s = %s failed\n", control, value);
	mixer_close(mixer);
	return ret;
}

/* PipeWire's default sink volume (linear amplitude); remembered to skip repeats */
static double sink_volume = -1;

static void set_sink_volume(double v)
{
	char cmd[160];

	if (fabs(v - sink_volume) < 1e-6)
		return;
	snprintf(cmd, sizeof cmd, PACTL_ENV " pactl set-sink-volume @DEFAULT_SINK@ %.6f "
		 ">/dev/null 2>&1", v);
	if (system(cmd) == 0)
		sink_volume = v;
}

/* the shared level (0..VOL_MAX) as codec gain + sink volume */
static void headphones_level(void)
{
	int level = read_level(VOL_FILE, HP_DEFAULT);
	double target;
	int pga;
	char db[16];

	if (level == 0) {
		set_enum("Headset_PGAL_GAIN", "-40Db");
		set_enum("Headset_PGAR_GAIN", "-40Db");
		set_sink_volume(0);
		return;
	}
	target = HP_MAX_DB - HP_RANGE_DB * (VOL_MAX - level) / VOL_MAX;
	pga = (int)ceil(target - 1e-9);
	if (pga < HP_PGA_MIN)
		pga = HP_PGA_MIN;
	snprintf(db, sizeof db, "%dDb", pga);
	set_enum("Headset_PGAL_GAIN", db);
	set_enum("Headset_PGAR_GAIN", db);
	set_sink_volume(pow(10, (target - pga) / 20));
}

static void headphones_on(void)
{
	headphones_level();
	sleep_ms(HP_DELAY_MS);	/* amps switched on before the stream runs stay silent */
	set_enum("Audio_Amp_L_Switch", "On");
	set_enum("Audio_Amp_R_Switch", "On");
	fprintf(stderr, "titan-speakerd: headphones on\n");
}

static void headphones_off(void)
{
	set_enum("Audio_Amp_L_Switch", "Off");
	set_enum("Audio_Amp_R_Switch", "Off");
	if (!headphones_in())
		set_sink_volume(1.0);	/* the speaker's level is the amp gain alone */
	fprintf(stderr, "titan-speakerd: headphones off\n");
}

static long long file_mtime(const char *path)
{
	struct stat st;

	if (stat(path, &st))
		return 0;
	return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
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
	if (pcm)
		pcm_close(pcm);
	set_clock("Off");
	fprintf(stderr, "titan-speakerd: speaker off\n");
}

int main(void)
{
	struct timespec tick = { 0, POLL_MS * 1000000L };
	struct pcm *amp = NULL;
	enum output out = OUT_NONE;
	int idle_ms = 0, backoff_ms = 0;
	int check_ms = -1, retries = 0;	/* DSP start check, -1 = done */
	int hp, last_hp;
	long long level_mtime = 0;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	setvbuf(stderr, NULL, _IOLBF, 0);

	last_hp = headphones_in();
	switch_levels(last_hp);		/* the level file may belong to the other output */

	while (!stop) {
		hp = headphones_in();
		if (hp != last_hp) {
			switch_levels(hp);
			last_hp = hp;
			if (!hp)
				set_sink_volume(1.0);
		}
		if (backoff_ms > 0) {
			backoff_ms -= POLL_MS;
		} else if (any_open()) {
			enum output want = hp ? OUT_HEADPHONES : OUT_SPEAKER;

			idle_ms = 0;
			if (out != want) {
				if (out == OUT_SPEAKER) {
					amp_off(amp);
					amp = NULL;
				} else if (out == OUT_HEADPHONES) {
					headphones_off();
				}
				out = OUT_NONE;
				if (want == OUT_SPEAKER) {
					amp = amp_on();
					if (amp)
						out = OUT_SPEAKER;
					else
						backoff_ms = RETRY_MS;
					check_ms = 0;
					retries = 0;
				} else {
					headphones_on();
					level_mtime = file_mtime(VOL_FILE);
					out = OUT_HEADPHONES;
				}
			}
		} else if (out != OUT_NONE && (idle_ms += POLL_MS) >= LINGER_MS) {
			if (out == OUT_SPEAKER) {
				amp_off(amp);
				amp = NULL;
			} else {
				headphones_off();
			}
			out = OUT_NONE;
		}

		/* volume keys / mixer changed the level: the headphones follow */
		if (out == OUT_HEADPHONES && file_mtime(VOL_FILE) != level_mtime) {
			level_mtime = file_mtime(VOL_FILE);
			headphones_level();
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
						out = OUT_NONE;
					}
				}
			}
		}
		nanosleep(&tick, NULL);
	}
	if (amp)
		amp_off(amp);
	if (out == OUT_HEADPHONES)
		headphones_off();
	return 0;
}
