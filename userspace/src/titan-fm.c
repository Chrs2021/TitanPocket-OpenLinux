/*
 * titan-fm: FM radio tuner for the MT6631 (MediaTek fmradio_drv, /dev/fm).
 *
 *   titan-fm scan            list stations (87.5-108 MHz, 100 kHz steps)
 *   titan-fm FREQ            power up, tune (e.g. 101.1) and stay tuned;
 *                            stdin: a frequency, "u"/"d" step, "su"/"sd"
 *                            seek, "q" quit. RSSI/stereo printed on change.
 *
 * The driver powers FM down when the last file handle closes, so the tuner
 * has to keep running while you listen. Sound comes out through the AFE's
 * FM path (see docs/drivers/fm.md for the mixer settings).
 *
 * Frequencies are in units of 10 kHz in the ioctls (10110 = 101.1 MHz).
 * "signal" is the chip's raw RSSI; higher is stronger, the station threshold
 * is -296 (about -4 dBuV, FM_RX_RSSI_TH_LONG in fm_cust.cfg).
 * The ioctl numbers encode sizeof(pointer), as in the driver's fm_ioctl.h.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct fm_tune_parm {
	unsigned char err, band, space, hilo, deemphasis;
	unsigned short freq;
};

struct fm_softmute_tune_t {
	int rssi;
	unsigned short freq;
	int valid;
};

#define FM_IOC_MAGIC		0xf5
#define FM_IOCTL_POWERUP	_IOWR(FM_IOC_MAGIC, 0, struct fm_tune_parm *)
#define FM_IOCTL_TUNE		_IOWR(FM_IOC_MAGIC, 2, struct fm_tune_parm *)
#define FM_IOCTL_GETRSSI	_IOWR(FM_IOC_MAGIC, 7, int *)
#define FM_IOCTL_GETCHIPID	_IOWR(FM_IOC_MAGIC, 10, unsigned short *)
#define FM_IOCTL_GETMONOSTERO	_IOWR(FM_IOC_MAGIC, 13, unsigned short *)
#define FM_IOCTL_PRE_SEARCH	_IOWR(FM_IOC_MAGIC, 45, int)
#define FM_IOCTL_RESTORE_SEARCH	_IOWR(FM_IOC_MAGIC, 46, int)
#define FM_IOCTL_SOFT_MUTE_TUNE	_IOWR(FM_IOC_MAGIC, 63, struct fm_softmute_tune_t *)

#define FM_BAND_UE	1
#define FM_SPACE_100K	1
#define FREQ_MIN	8750
#define FREQ_MAX	10800
#define STEP		10

static int fd;

static int tune(unsigned short freq, int powerup)
{
	struct fm_tune_parm p = {
		.band = FM_BAND_UE, .space = FM_SPACE_100K, .freq = freq,
	};

	if (ioctl(fd, powerup ? FM_IOCTL_POWERUP : FM_IOCTL_TUNE, &p) < 0) {
		fprintf(stderr, "titan-fm: %s %u: %s\n", powerup ? "power up" : "tune",
			freq, strerror(errno));
		return -1;
	}
	return 0;
}

/* soft-mute tune: measure a channel without unmuting; 1 = station */
static int probe(unsigned short freq, int *rssi)
{
	struct fm_softmute_tune_t p = { .freq = freq };

	if (ioctl(fd, FM_IOCTL_SOFT_MUTE_TUNE, &p) < 0)
		return -1;
	*rssi = p.rssi;
	return p.valid != 0;
}

static unsigned short wrap(int f)
{
	if (f > FREQ_MAX)
		return FREQ_MIN;
	if (f < FREQ_MIN)
		return FREQ_MAX;
	return f;
}

static int seek(unsigned short from, int dir)
{
	int f = from, i, rssi;

	ioctl(fd, FM_IOCTL_PRE_SEARCH, 0);
	for (i = 0; i < (FREQ_MAX - FREQ_MIN) / STEP; i++) {
		f = wrap(f + dir * STEP);
		if (probe(f, &rssi) == 1)
			break;
	}
	ioctl(fd, FM_IOCTL_RESTORE_SEARCH, 0);
	return f;
}

static int scan(void)
{
	int f, rssi, n = 0;

	if (tune(FREQ_MIN, 1))
		return 1;
	ioctl(fd, FM_IOCTL_PRE_SEARCH, 0);
	for (f = FREQ_MIN; f <= FREQ_MAX; f += STEP) {
		if (probe(f, &rssi) == 1) {
			printf("%3d.%d MHz  signal %d\n", f / 100, f / 10 % 10, rssi);
			n++;
		}
	}
	ioctl(fd, FM_IOCTL_RESTORE_SEARCH, 0);
	fprintf(stderr, "titan-fm: %d stations\n", n);
	return 0;
}

static int parse_mhz(const char *s)
{
	double mhz = strtod(s, NULL);
	int f = (int)(mhz * 10 + 0.5) * 10;

	return f >= FREQ_MIN && f <= FREQ_MAX ? f : -1;
}

static void status(unsigned short freq)
{
	unsigned short stereo = 0;
	int rssi = 0;

	ioctl(fd, FM_IOCTL_GETRSSI, &rssi);
	ioctl(fd, FM_IOCTL_GETMONOSTERO, &stereo);
	printf("%3d.%d MHz  signal %d  %s\n", freq / 100, freq / 10 % 10, rssi,
	       stereo ? "stereo" : "mono");
	fflush(stdout);
}

int main(int argc, char **argv)
{
	unsigned short chip = 0;
	char line[64];
	int freq;

	if (argc != 2) {
		fprintf(stderr, "usage: titan-fm scan | titan-fm MHZ\n");
		return 2;
	}
	fd = open("/dev/fm", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "titan-fm: /dev/fm: %s (is fmradio_drv loaded?)\n", strerror(errno));
		return 1;
	}
	if (!strcmp(argv[1], "scan"))
		return scan();

	if ((freq = parse_mhz(argv[1])) < 0) {
		fprintf(stderr, "titan-fm: %s is outside 87.5-108 MHz\n", argv[1]);
		return 2;
	}
	if (tune(freq, 1))
		return 1;
	if (!ioctl(fd, FM_IOCTL_GETCHIPID, &chip))
		fprintf(stderr, "titan-fm: chip %04x powered up\n", chip);
	status(freq);

	for (;;) {
		struct pollfd p = { .fd = 0, .events = POLLIN };
		int r = poll(&p, 1, 5000), f;

		if (r == 0) {
			status(freq);
			continue;
		}
		if (!fgets(line, sizeof(line), stdin) || line[0] == 'q')
			break;
		if (line[0] == 'u' || line[0] == 'd') {
			f = wrap(freq + (line[0] == 'u' ? STEP : -STEP));
		} else if (line[0] == 's' && (line[1] == 'u' || line[1] == 'd')) {
			f = seek(freq, line[1] == 'u' ? 1 : -1);
		} else if ((f = parse_mhz(line)) < 0) {
			fprintf(stderr, "titan-fm: ? (MHz, u, d, su, sd, q)\n");
			continue;
		}
		if (!tune(f, 0))
			freq = f;
		status(freq);
	}
	close(fd);	/* last close powers FM down */
	return 0;
}
