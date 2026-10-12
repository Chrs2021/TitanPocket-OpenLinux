/*
 * titan-wlan-cmd: send a MediaTek gen3 Wi-Fi "driver command" (the private
 * commands Android's wpa_supplicant passes with "DRIVER ...").
 *
 *   titan-wlan-cmd [-i wlan0] SETSUSPENDMODE 1
 *
 * Interface: ioctl SIOCDEVPRIVATE + 1 on the net device, ifr_data pointing
 * to struct priv_driver_cmd_s (wlan_gen3/os/linux/gl_wext_priv.c). The reply
 * comes back in the same buffer and is printed.
 */
#include <errno.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define PRIV_CMD_SIZE 512

struct priv_driver_cmd {
	char buf[PRIV_CMD_SIZE];
	int used_len;
	int total_len;
};

int main(int argc, char **argv)
{
	const char *iface = "wlan0";
	struct priv_driver_cmd cmd;
	struct ifreq ifr;
	int i, fd;
	size_t n = 0;

	if (argc > 2 && !strcmp(argv[1], "-i")) {
		iface = argv[2];
		argv += 2;
		argc -= 2;
	}
	if (argc < 2) {
		fprintf(stderr, "usage: titan-wlan-cmd [-i IFACE] COMMAND [ARGS...]\n");
		return 2;
	}

	memset(&cmd, 0, sizeof(cmd));
	for (i = 1; i < argc; i++) {
		int w = snprintf(cmd.buf + n, sizeof(cmd.buf) - n, "%s%s", i > 1 ? " " : "", argv[i]);

		if (w < 0 || (size_t)w >= sizeof(cmd.buf) - n) {
			fprintf(stderr, "titan-wlan-cmd: command too long\n");
			return 2;
		}
		n += w;
	}
	cmd.used_len = n + 1;
	cmd.total_len = sizeof(cmd.buf);

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
	ifr.ifr_data = (void *)&cmd;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0 || ioctl(fd, SIOCDEVPRIVATE + 1, &ifr) < 0) {
		fprintf(stderr, "titan-wlan-cmd: %s: %s\n", iface, strerror(errno));
		return 1;
	}
	close(fd);
	cmd.buf[PRIV_CMD_SIZE - 1] = 0;
	if (cmd.buf[0])
		printf("%s\n", cmd.buf);
	return 0;
}
