/*
 * titan-wmt: bring up the MT6771 CONNSYS (Wi-Fi/BT/GPS/FM) on plain Linux.
 *
 * Replaces MediaTek's Android-only wmt_loader + wmt_launcher, which were
 * reverse-engineered for this (see docs/drivers/connectivity.md):
 *
 *   loader   /dev/wmtdetect: SoC has no external chip, read the chip id,
 *            hand it back, run the WMT kernel-module init.
 *   launcher /dev/stpwmt:   set config name + STP mode (BTIF), power the
 *            chip on, then serve the kernel's "srh_patch" requests by
 *            registering the ROM patch files.
 *
 * Needs wmt_drv.ko loaded and the firmware in /lib/firmware. Runs in the
 * foreground (the kernel asks for patches on every chip power-on) and
 * creates READY_FILE once the chip may be switched on.
 *
 *   titan-wmt [-p firmware-dir] [-1]   (-1: init + power on, then exit)
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

/* common_detect/wmt_detect.h */
#define WMT_DETECT_IOC_MAGIC		'w'
#define COMBO_IOCTL_GET_CHIP_ID		_IOR(WMT_DETECT_IOC_MAGIC, 0, int)
#define COMBO_IOCTL_SET_CHIP_ID		_IOW(WMT_DETECT_IOC_MAGIC, 1, int)
#define COMBO_IOCTL_GET_SOC_CHIP_ID	_IOR(WMT_DETECT_IOC_MAGIC, 3, int)
#define COMBO_IOCTL_DO_MODULE_INIT	_IOR(WMT_DETECT_IOC_MAGIC, 4, int)
#define COMBO_IOCTL_MODULE_CLEANUP	_IOR(WMT_DETECT_IOC_MAGIC, 5, int)
#define COMBO_IOCTL_EXT_CHIP_PWR_ON	_IOR(WMT_DETECT_IOC_MAGIC, 6, int)
#define COMBO_IOCTL_GET_ADIE_CHIP_ID	_IOR(WMT_DETECT_IOC_MAGIC, 9, int)

/* common_main/linux/wmt_dev.c */
#define WMT_IOC_MAGIC			0xa0
#define WMT_IOCTL_SET_STP_MODE		_IOW(WMT_IOC_MAGIC, 5, int)
#define WMT_IOCTL_LPBK_POWER_CTRL	_IOW(WMT_IOC_MAGIC, 7, int)
#define WMT_IOCTL_SET_LAUNCHER_KILL	_IOW(WMT_IOC_MAGIC, 13, int)
#define WMT_IOCTL_SET_PATCH_NUM		_IOW(WMT_IOC_MAGIC, 14, int)
#define WMT_IOCTL_SET_PATCH_INFO	_IOW(WMT_IOC_MAGIC, 15, char *)
#define WMT_IOCTL_WMT_CFG_NAME		_IOWR(WMT_IOC_MAGIC, 21, char *)
#define WMT_IOCTL_WMT_QUERY_CHIPID	_IOR(WMT_IOC_MAGIC, 22, int)

#define STP_BTIF_FULL	0x03	/* linux/include/wmt_dev.h */
#define WMT_FM_COMM	2	/* core/include/wmt_core.h */

struct wmt_patch_info {		/* WMT_PATCH_INFO, core/include/wmt_lib.h */
	uint32_t seq;		/* 1-based download order */
	uint8_t addr[4];
	char name[256];		/* request_firmware() name */
};

/* MT6771 (CONNSYS chip 0x788): ROMv4 patches, "ROMv4_be_patch_<v>_<n>_hdr.bin" */
#define PATCH_PREFIX	"ROMv4_be_patch"
#define WMT_CFG_NAME	"WMT_SOC.cfg"
#define READY_FILE	"/run/titan-wmt.ready"

static const char *fwdir = "/lib/firmware";

#define log(...) do { fprintf(stderr, "titan-wmt: " __VA_ARGS__); fputc('\n', stderr); } while (0)

static int open_retry(const char *path, int tries)
{
	int fd;

	while ((fd = open(path, O_RDWR | O_NOCTTY)) < 0 && --tries > 0)
		usleep(300000);
	if (fd < 0)
		log("open %s: %s", path, strerror(errno));
	return fd;
}

/* what wmt_loader does for a SoC chip */
static int loader(void)
{
	int fd = open_retry("/dev/wmtdetect", 20), chip, adie;

	if (fd < 0)
		return -1;
	/* an SoC has no external combo chip: ask the platform code instead */
	if (ioctl(fd, COMBO_IOCTL_EXT_CHIP_PWR_ON) != 0)
		chip = ioctl(fd, COMBO_IOCTL_GET_SOC_CHIP_ID);
	else
		chip = ioctl(fd, COMBO_IOCTL_GET_CHIP_ID);
	log("chip id 0x%04x", chip);
	if (chip <= 0) {
		close(fd);
		return -1;
	}
	ioctl(fd, COMBO_IOCTL_SET_CHIP_ID, chip);
	ioctl(fd, COMBO_IOCTL_MODULE_CLEANUP, chip);
	if (ioctl(fd, COMBO_IOCTL_DO_MODULE_INIT, chip) != 0) {
		log("kernel module init failed");
		close(fd);
		return -1;
	}
	adie = ioctl(fd, COMBO_IOCTL_GET_ADIE_CHIP_ID);
	log("WMT driver ready, A-die 0x%04x", adie);
	close(fd);
	return 0;
}

/*
 * "srh_patch": register every <PATCH_PREFIX>* file. Header byte 0x18 holds
 * patch count (high nibble) and sequence (low nibble); 0x19..0x1b are the
 * load address bytes (byte 0 is always 0), as wmt_launcher reads them.
 */
static int search_patches(int fd)
{
	DIR *d = opendir(fwdir);
	struct dirent *e;
	int found = 0, num_set = 0;

	if (!d) {
		log("opendir %s: %s", fwdir, strerror(errno));
		return -1;
	}
	while ((e = readdir(d))) {
		struct wmt_patch_info pi;
		uint8_t hdr[0x1c];
		char path[512];
		int pf;

		if (strncmp(e->d_name, PATCH_PREFIX, strlen(PATCH_PREFIX)))
			continue;
		snprintf(path, sizeof(path), "%s/%s", fwdir, e->d_name);
		pf = open(path, O_RDONLY);
		if (pf < 0 || read(pf, hdr, sizeof(hdr)) != sizeof(hdr)) {
			log("read %s failed", path);
			if (pf >= 0)
				close(pf);
			continue;
		}
		close(pf);

		if (!num_set) {
			ioctl(fd, WMT_IOCTL_SET_PATCH_NUM, hdr[0x18] >> 4);
			num_set = 1;
		}
		memset(&pi, 0, sizeof(pi));
		pi.seq = hdr[0x18] & 0xf;
		pi.addr[1] = hdr[0x19];
		pi.addr[2] = hdr[0x1a];
		pi.addr[3] = hdr[0x1b];
		snprintf(pi.name, sizeof(pi.name), "%s", e->d_name);
		log("patch %s: %d/%d addr %02x%02x%02x", pi.name, pi.seq, hdr[0x18] >> 4,
		    pi.addr[1], pi.addr[2], pi.addr[3]);
		if (ioctl(fd, WMT_IOCTL_SET_PATCH_INFO, &pi) != 0)
			log("SET_PATCH_INFO %s: %s", pi.name, strerror(errno));
		found++;
	}
	closedir(d);
	return found ? 0 : -1;
}

static int power_on(int fd)
{
	int retry;

	for (retry = 0; retry < 5; retry++) {
		if (ioctl(fd, WMT_IOCTL_LPBK_POWER_CTRL, 1) == 0) {
			log("CONNSYS powered on");
			return 0;
		}
		ioctl(fd, WMT_IOCTL_LPBK_POWER_CTRL, 0);
		log("power on failed, retrying (%d)", retry);
		sleep(1);
	}
	return -1;
}

int main(int argc, char **argv)
{
	char cfg[256] = WMT_CFG_NAME;
	int fd, opt, once = 0, powered = 0;

	while ((opt = getopt(argc, argv, "p:1")) != -1) {
		if (opt == 'p')
			fwdir = optarg;
		else if (opt == '1')
			once = 1;
		else {
			fprintf(stderr, "usage: %s [-p firmware-dir] [-1]\n", argv[0]);
			return 2;
		}
	}

	/* /dev/stpwmt only exists once the WMT core has been initialised */
	if (access("/dev/stpwmt", F_OK) && loader())
		return 1;

	fd = open_retry("/dev/stpwmt", 20);
	if (fd < 0)
		return 1;
	log("consys chip 0x%04x", ioctl(fd, WMT_IOCTL_WMT_QUERY_CHIPID));
	ioctl(fd, WMT_IOCTL_WMT_CFG_NAME, cfg);
	if (ioctl(fd, WMT_IOCTL_SET_STP_MODE, STP_BTIF_FULL | WMT_FM_COMM << 4))
		log("SET_STP_MODE: %s", strerror(errno));
	ioctl(fd, WMT_IOCTL_SET_LAUNCHER_KILL, 0);

	/*
	 * The power on blocks in the kernel until the patches are downloaded,
	 * and the patch search is a request back to us, so serve requests
	 * while a child does the power on.
	 */
	if (fork() == 0)
		_exit(power_on(fd) ? 1 : 0);
	close(open(READY_FILE, O_WRONLY | O_CREAT, 0644));

	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		char cmd[256], resp[32];
		ssize_t n;

		if (poll(&p, 1, once && powered ? 0 : 2000) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (!(p.revents & POLLIN)) {
			int st;

			if (waitpid(-1, &st, WNOHANG) > 0) {
				powered = 1;
				if (once)
					break;
			}
			continue;
		}
		n = read(fd, cmd, sizeof(cmd) - 1);
		if (n <= 0)
			continue;
		cmd[n] = 0;
		log("kernel request: %s", cmd);
		if (!strcmp(cmd, "srh_patch"))
			snprintf(resp, sizeof(resp), "%s", search_patches(fd) ? "fail" : "ok");
		else if (!strcmp(cmd, "srh_rom_patch"))
			snprintf(resp, sizeof(resp), "ok");	/* not used by ROMv4 chips */
		else
			snprintf(resp, sizeof(resp), "cmd not found");
		if (write(fd, resp, strlen(resp)) < 0)
			log("write resp: %s", strerror(errno));
	}
	unlink(READY_FILE);
	close(fd);
	return 0;
}
