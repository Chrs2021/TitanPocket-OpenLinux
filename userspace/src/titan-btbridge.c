/*
 * titan-btbridge: connect the MT6771 CONNSYS Bluetooth controller to the
 * Linux Bluetooth stack (BlueZ).
 *
 * MediaTek's bt_drv exposes the controller as /dev/stpbt: raw HCI with H4
 * framing (one type byte per packet) carried over WMT/STP. Opening it powers
 * the BT function on. The kernel has no HCI driver for it, so this daemon
 * relays packets between /dev/stpbt and /dev/vhci, which makes the
 * controller appear as hci0.
 *
 *   titan-btbridge [-a BD:AD:DR:..]       relay (foreground)
 *   titan-btbridge [-a BD:AD:DR:..] -t    test: Reset, Read Local Version,
 *                                         Read BD_ADDR
 *
 * -a sets the public address first (MediaTek vendor command 0xFC1A, as the
 * stock HAL does from the factory NVRAM record APCFG/APRDEB/BT_Addr);
 * otherwise the controller uses its default 00:00:46:xx:xx:xx.
 *
 * The controller's Read Local Supported Commands reply leaves out every LE
 * command (octets 25-28 are zero) although it implements them. The kernel
 * enables LE events from that list, so it would mask LE Connection Complete
 * and LE Advertising Report: advertising works, but no LE connection or scan
 * result ever reaches BlueZ. The relay fills in the Bluetooth 4.0 LE command
 * set (see fix_supported_commands).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define H4_CMD	0x01
#define H4_ACL	0x02
#define H4_SCO	0x03
#define H4_EVT	0x04
#define H4_ISO	0x05
#define H4_VENDOR 0xff	/* vhci control packet */

#define log(...) do { fprintf(stderr, "titan-btbridge: " __VA_ARGS__); fputc('\n', stderr); } while (0)

static int write_all(int fd, const uint8_t *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {	/* STP queue full */
				usleep(1000);
				continue;
			}
			return -1;
		}
		p += w;
		n -= w;
	}
	return 0;
}

/* length of the complete H4 packet at p, 0 if more bytes are needed, -1 if bad */
static int h4_len(const uint8_t *p, size_t n)
{
	size_t hdr, plen;

	if (n < 1)
		return 0;
	switch (p[0]) {
	case H4_EVT: hdr = 2; if (n < 1 + hdr) return 0; plen = p[2]; break;
	case H4_ACL: hdr = 4; if (n < 1 + hdr) return 0; plen = p[3] | p[4] << 8; break;
	case H4_SCO: hdr = 3; if (n < 1 + hdr) return 0; plen = p[3]; break;
	case H4_ISO: hdr = 4; if (n < 1 + hdr) return 0; plen = (p[3] | p[4] << 8) & 0x3fff; break;
	default: return -1;
	}
	return 1 + hdr + plen <= n ? (int)(1 + hdr + plen) : 0;
}

static void dump(const char *tag, const uint8_t *p, int n)
{
	int i;

	fprintf(stderr, "%s", tag);
	for (i = 0; i < n; i++)
		fprintf(stderr, " %02x", p[i]);
	fputc('\n', stderr);
}

/* read one complete H4 packet from /dev/stpbt (buffering partial reads) */
static uint8_t rxbuf[16384];
static size_t rxlen;

static int stp_next_packet(int fd, uint8_t *out, int timeout_ms)
{
	for (;;) {
		int n = h4_len(rxbuf, rxlen);

		if (n < 0) {
			log("lost H4 framing (byte %02x), resyncing", rxbuf[0]);
			memmove(rxbuf, rxbuf + 1, --rxlen);
			continue;
		}
		if (n > 0) {
			memcpy(out, rxbuf, n);
			memmove(rxbuf, rxbuf + n, rxlen - n);
			rxlen -= n;
			return n;
		}
		if (timeout_ms >= 0) {
			struct pollfd p = { .fd = fd, .events = POLLIN };

			if (poll(&p, 1, timeout_ms) <= 0)
				return 0;
		}
		ssize_t r = read(fd, rxbuf + rxlen, sizeof(rxbuf) - rxlen);

		if (r < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -1;
		}
		rxlen += r;
		if (timeout_ms < 0)	/* relay mode: one read per poll wakeup */
			return h4_len(rxbuf, rxlen) > 0 ? stp_next_packet(fd, out, 0) : 0;
	}
}

/*
 * Read Local Supported Commands (0x1002) Command Complete: add the LE
 * commands of Bluetooth 4.0 (mandatory for an LE controller) that the
 * firmware leaves out. Octet 25: LE Set Event Mask .. LE Set Advertising Data
 * (bit 3 reserved); 26: LE Set Scan Response Data .. LE Clear White List;
 * 27: LE Add Device To White List .. LE Rand; 28: LE Start Encryption ..
 * LE Test End.
 */
static void fix_supported_commands(uint8_t *p, int n)
{
	static const uint8_t le40[4] = { 0xf7, 0xff, 0xff, 0x7f };
	static int logged;
	uint8_t *cmds = p + 7;		/* H4 type, event, plen, ncmd, opcode[2], status */
	int i, changed = 0;

	if (n < 7 + 29 || p[0] != H4_EVT || p[1] != 0x0e ||
	    (p[4] | p[5] << 8) != 0x1002 || p[6] != 0)
		return;
	for (i = 0; i < 4; i++) {
		changed |= le40[i] & ~cmds[25 + i];
		cmds[25 + i] |= le40[i];
	}
	if (changed && !logged++)
		log("added the LE commands missing from Read Local Supported Commands");
}

/* send one HCI command and wait for its Command Complete/Status */
static int hci_cmd(int stp, uint16_t opcode, const uint8_t *param, uint8_t plen)
{
	uint8_t buf[260], ev[2048];
	int n, tries;

	buf[0] = H4_CMD;
	buf[1] = opcode & 0xff;
	buf[2] = opcode >> 8;
	buf[3] = plen;
	memcpy(buf + 4, param, plen);
	if (write_all(stp, buf, 4 + plen))
		return -1;
	for (tries = 0; tries < 10; tries++) {
		n = stp_next_packet(stp, ev, 2000);
		if (n <= 0)
			return -1;
		if (ev[0] == H4_EVT && ev[1] == 0x0e && n >= 7 &&
		    (ev[4] | ev[5] << 8) == opcode)
			return ev[6];		/* status */
		if (ev[0] == H4_EVT && ev[1] == 0x0f && n >= 7 &&
		    (ev[5] | ev[6] << 8) == opcode)
			return ev[3];
	}
	return -1;
}

static int set_bdaddr(int stp, const char *str)
{
	unsigned a[6];
	uint8_t le[6];
	int i, st;

	if (sscanf(str, "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
		log("bad address %s", str);
		return -1;
	}
	for (i = 0; i < 6; i++)
		le[i] = a[5 - i];
	if ((st = hci_cmd(stp, 0x0c03, NULL, 0)) != 0) {	/* HCI_Reset */
		log("reset failed (%d)", st);
		return -1;
	}
	st = hci_cmd(stp, 0xfc1a, le, 6);
	if (st != 0) {
		log("set address failed (%d)", st);
		return -1;
	}
	log("public address %s", str);
	return 0;
}

static int test(int stp)
{
	static const uint8_t cmds[][4] = {
		{ H4_CMD, 0x03, 0x0c, 0x00 },	/* HCI_Reset */
		{ H4_CMD, 0x01, 0x10, 0x00 },	/* Read_Local_Version_Information */
		{ H4_CMD, 0x09, 0x10, 0x00 },	/* Read_BD_ADDR */
	};
	uint8_t pkt[2048];
	unsigned i;

	for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		int n;

		dump("cmd:", cmds[i], 4);
		if (write_all(stp, cmds[i], 4))
			return 1;
		n = stp_next_packet(stp, pkt, 2000);
		if (n <= 0) {
			log("no reply");
			return 1;
		}
		dump("evt:", pkt, n);
		if (i == 2 && n >= 13 && pkt[6] == 0)
			printf("BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x\n",
			       pkt[12], pkt[11], pkt[10], pkt[9], pkt[8], pkt[7]);
	}
	return 0;
}

int main(int argc, char **argv)
{
	uint8_t pkt[4096];
	int stp, vhci;

	signal(SIGPIPE, SIG_IGN);
	const char *addr = NULL;
	int opt, testmode = 0;

	while ((opt = getopt(argc, argv, "a:t")) != -1) {
		if (opt == 'a')
			addr = optarg;
		else if (opt == 't')
			testmode = 1;
		else
			return 2;
	}

	stp = open("/dev/stpbt", O_RDWR | O_NOCTTY);
	if (stp < 0) {
		log("open /dev/stpbt: %s (is bt_drv loaded?)", strerror(errno));
		return 1;
	}
	log("BT powered on");
	if (addr && set_bdaddr(stp, addr))
		log("continuing with the default address");
	if (testmode)
		return test(stp);

	vhci = open("/dev/vhci", O_RDWR);
	if (vhci < 0) {
		log("open /dev/vhci: %s", strerror(errno));
		return 1;
	}
	/* create the controller now (HCI_PRIMARY) instead of after vhci's 1 s timeout */
	pkt[0] = H4_VENDOR;
	pkt[1] = 0x00;
	if (write_all(vhci, pkt, 2))
		log("vhci create: %s", strerror(errno));

	for (;;) {
		struct pollfd p[2] = {
			{ .fd = stp, .events = POLLIN },
			{ .fd = vhci, .events = POLLIN },
		};
		int n;

		if (poll(p, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (p[1].revents & POLLIN) {	/* host -> controller */
			n = read(vhci, pkt, sizeof(pkt));
			if (n <= 0)
				break;
			if (pkt[0] != H4_VENDOR && write_all(stp, pkt, n)) {
				log("write stpbt: %s", strerror(errno));
				break;
			}
		}
		if (p[0].revents & POLLIN) {	/* controller -> host */
			while ((n = stp_next_packet(stp, pkt, -1)) > 0) {
				fix_supported_commands(pkt, n);
				if (write_all(vhci, pkt, n)) {
					log("write vhci: %s", strerror(errno));
					return 1;
				}
				if (h4_len(rxbuf, rxlen) <= 0)
					break;
			}
			if (n < 0) {
				log("read stpbt: %s", strerror(errno));
				break;
			}
		}
		if ((p[0].revents | p[1].revents) & (POLLERR | POLLHUP)) {
			log("device gone");
			break;
		}
	}
	return 1;
}
