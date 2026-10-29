/*
 * titan-mnld: an open replacement for MediaTek's mnld (the GNSS daemon).
 *
 * MediaTek's navigation engine, libmnl.so, stays closed: it drives the
 * receiver over /dev/stpgps (firmware patch, tracking, measurements) and
 * computes the fixes. mnld, the stock daemon around it, needs 20 Android
 * libraries (HIDL, sensors, curl, OpenSSL, nvram...). This replaces it with
 * just what libmnl needs from its host:
 *
 *   - /dev/stpgps opened and probed (chip, clock type, LNA pin)
 *   - the two config structs for mtk_gps_mnl_run()
 *   - the 17 porting-layer callbacks (mtk_gps_sys_function_register)
 *   - property_get() and mtk_gps_sys_agps_disaptcher_callback(), which
 *     libmnl imports from its host executable
 *
 * Control and NMEA use mnld's socket protocol with the GNSS HAL, so
 * titan-gnss drives this exactly as it drove mnld:
 *   to us    "\0mtk_hal2mnl"  [i32 1][i32 cmd]  101 init, 102 start, 103 stop,
 *                                               108 set_position_mode
 *   from us  "\0mtk_mnl2hal"  [i32 1][i32 104][i64 ts][u8 1][i32 len][nmea\0][i32 len]
 *
 * libmnl is a 32-bit bionic library, so this is built for bionic (clang
 * --target=armv7a-linux-androideabi, no libc headers) and runs under the
 * stock linker in a minimal chroot: linker, ld-android, libc, libc++, libdl,
 * libm and libmnl.so. See docs/gnss.md and userspace/titan-mnld/build.sh.
 *
 * Struct layouts and the call sequence were recovered from the stock mnld
 * (Ghidra) and checked by interposing it (tools/gnss/mnlspy.c).
 */
typedef unsigned int size_t;
typedef int ssize_t;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef long long i64;

/* ---- bionic (no headers) ---- */
extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern ssize_t read(int fd, void *buf, size_t n);
extern ssize_t write(int fd, const void *buf, size_t n);
extern int ioctl(int fd, int req, ...);
extern int socket(int domain, int type, int proto);
extern int bind(int fd, const void *addr, u32 len);
extern ssize_t sendto(int fd, const void *buf, size_t n, int flags, const void *addr, u32 len);
extern ssize_t recv(int fd, void *buf, size_t n, int flags);
extern int setsockopt(int fd, int level, int name, const void *val, u32 len);
extern int snprintf(char *s, size_t n, const char *fmt, ...);
extern int vsnprintf(char *s, size_t n, const char *fmt, __builtin_va_list ap);
extern void *memset(void *d, int c, size_t n);
extern void *memcpy(void *d, const void *s, size_t n);
extern int memcmp(const void *a, const void *b, size_t n);
extern size_t strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern int mkdir(const char *path, u32 mode);
extern void (*signal(int sig, void (*fn)(int)))(int);
extern int clock_gettime(int clk, void *ts);
extern int *__errno(void);
extern char *strerror(int e);
extern void exit(int code) __attribute__((noreturn));
extern int usleep(u32 us);
extern char *getenv(const char *name);
extern long strtol(const char *s, char **end, int base);
extern unsigned long strtoul(const char *s, char **end, int base);
extern char *strstr(const char *h, const char *n);
extern long syscall(long n, ...);

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define AF_UNIX 1
#define SOCK_DGRAM 2
#define SOL_SOCKET 1
#define SO_RCVTIMEO 20
#define SIGTERM 15
#define SIGINT 2
#define SIGPIPE 13
#define SIG_IGN ((void (*)(int))1)

/* ---- libmnl ---- */
extern int mtk_gps_sys_function_register(void *table);
extern int mtk_gps_mnl_run(const void *init_cfg, const void *driver_cfg);
extern int mtk_gps_mnl_stop(void);
extern int mtk_gps_set_param(int key, void *value);
/* Android's raw GNSS measurements (stock mnld's mnl2hal_gnss_measurements):
 * six arrays of 0xa4-byte GnssMeasurement-like entries (GPS, GLONASS,
 * BeiDou, Galileo, then two more: QZSS and SBAS?), then six small per-SV
 * byte arrays (35, 20, 63, 36, 19 and 14 bytes, cleared by the call) */
extern int mtk_gnss_get_measurement(void *gps, void *glonass, void *beidou, void *galileo,
				    void *m5, void *m6, void *f1, void *f2, void *f3,
				    void *f4, void *f5, void *f6);
extern int mtk_gnss_get_clock(void *clock);
extern int mtk_gps_set_debug_type(u8 type);

/* ---- logging (stderr: the journal) ---- */
static void say(const char *fmt, ...)
{
	char b[512];
	__builtin_va_list ap;
	int n;

	__builtin_va_start(ap, fmt);
	n = vsnprintf(b, sizeof b - 1, fmt, ap);
	__builtin_va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof b - 2)
		n = sizeof b - 2;
	b[n++] = '\n';
	write(2, b, n);
}

/* ---- Android properties ----
 * libmnl reads one (ro.build.type, for its logging) through libcutils'
 * property_get(), which it expects from its host. There is no property
 * service here: every key reads as its default. */
#define PROP_VALUE_MAX 92

int property_get(const char *key, char *value, const char *def)
{
	int n = 0;

	if (def)
		while (def[n] && n < PROP_VALUE_MAX - 1) {
			value[n] = def[n];
			n++;
		}
	value[n] = 0;
	if (getenv("TITAN_MNLD_DEBUG"))
		say("titan-mnld: property %s -> \"%s\"", key, value);
	return n;
}

/* AGPS requests (EPO, NTP, network location...): no AGPS daemon here */
int mtk_gps_sys_agps_disaptcher_callback(u16 type, u16 len, char *buf)
{
	static u32 seen;	/* log each request type once */

	(void)len; (void)buf;
	if (type < 32 && !(seen & 1u << type)) {
		seen |= 1u << type;
		say("titan-mnld: AGPS request %u ignored (no AGPS)", type);
	}
	return 0;
}

/* ---- HAL socket ---- */
struct sockaddr_un { u16 family; char path[108]; };
static int hal_tx = -1;
static struct sockaddr_un hal_addr;

static void abstract_addr(struct sockaddr_un *a, const char *name)
{
	memset(a, 0, sizeof *a);
	a->family = AF_UNIX;
	memcpy(a->path + 1, name, strlen(name));	/* "\0name", NUL padded */
}

static void hal_nmea(const char *text, u32 len)
{
	static u8 msg[4096];
	struct { long s, ns; } ts;
	i64 ms;
	u32 n = 0, l;

	if (hal_tx < 0 || !len)
		return;
	if (len > sizeof msg - 32)
		len = sizeof msg - 32;
	clock_gettime(0, &ts);		/* CLOCK_REALTIME */
	ms = (i64)ts.s * 1000 + ts.ns / 1000000;
	l = len + 1;			/* incl. NUL */
	*(int *)(msg + n) = 1; n += 4;
	*(int *)(msg + n) = 104; n += 4;
	memcpy(msg + n, &ms, 8); n += 8;
	msg[n++] = 1;
	memcpy(msg + n, &l, 4); n += 4;
	memcpy(msg + n, text, len); n += len;
	msg[n++] = 0;
	memcpy(msg + n, &l, 4); n += 4;
	sendto(hal_tx, msg, n, 0, &hal_addr, sizeof hal_addr);
}

/* ---- porting-layer callbacks ---- */
#define MSG_FIX_READY 0
#define MSG_FIX_PROHIBITED 7
#define MSG_NEED_RESTART 13

static int cb_mnl(int type)
{
	if (type == MSG_NEED_RESTART) {
		/* stock mnld exits here too; systemd restarts us and
		 * titan-gnss starts a new session */
		say("titan-mnld: libmnl asks for a restart");
		exit(1);
	}
	return 0;
}
static int cb_nmea_app(const char *s, u32 n) { (void)s; (void)n; return 0; }
static int cb_nmea_mnld(const char *s, u32 n) { hal_nmea(s, n); return 0; }
static int cb_zero(void) { return 0; }
static void cb_void(void) { }
static int cb_offload(void) { return 0; }
/* SUPL_encrypt/decrypt as in stock mnld: byte-wise rotate and XOR 0x63 */
static int cb_encrypt(const u8 *plain, u8 *cipher, u32 n)
{
	u32 i;
	for (i = 0; i < n; i++)
		cipher[i] = (u8)((plain[i] >> 2 | plain[i] << 6) ^ 0x63);
	return 0;
}
static int cb_decrypt(u8 *plain, const u8 *cipher, u32 n)
{
	u32 i;
	for (i = 0; i < n; i++) {
		u8 x = cipher[i] ^ 0x63;
		plain[i] = (u8)(x << 2 | x >> 6);
	}
	return 0;
}
static int cb_suspend_resume(u32 mask) { (void)mask; return -1; }	/* no hw suspend */
static void cb_suspend_ext(int en, int timeout) { (void)en; (void)timeout; }
static void cb_data2mnld(const void *p, u32 n) { (void)p; (void)n; }
/* libmnl's debug log (TITAN_MNLD_DBG=/path, debug type 0x11 as stock mnld's
 * "debug to file"): its per-satellite tracking lines are a reference for
 * decoding the chip's reports */
static int dbg_fd = -1;
static int cb_dbg2file(const void *p, u32 n)
{
	if (dbg_fd >= 0)
		write(dbg_fd, p, n);
	return 0;
}

static void *callbacks[17] = {
	cb_mnl,			/*  0 gps_mnl_callback */
	cb_nmea_app,		/*  1 nmea_output_to_app (stock mnld: log only) */
	cb_nmea_mnld,		/*  2 nmea_output_to_mnld: the NMEA, once a second */
	cb_zero,		/*  3 frame_sync_enable_sleep_mode */
	cb_zero,		/*  4 frame_sync_meas_req_by_network */
	cb_zero,		/*  5 frame_sync_meas_req */
	mtk_gps_sys_agps_disaptcher_callback, /* 6 */
	cb_void,		/*  7 pmtk_cmd_cb */
	cb_encrypt,		/*  8 */
	cb_decrypt,		/*  9 */
	cb_zero,		/* 10 ofl_sys_rst_stpgps_req */
	cb_zero,		/* 11 ofl_sys_submit_flp_data */
	cb_dbg2file,		/* 12 debug log to file */
	cb_offload,		/* 13 ofl_sys_mnl_offload_callback */
	cb_data2mnld,		/* 14 data2mnld_callback */
	cb_suspend_resume,	/* 15 do_hw_suspend_resume */
	cb_suspend_ext,		/* 16 suspend_extention_set */
};

/* ---- the engine ---- */
#include "config.h"	/* init_cfg / driver_cfg defaults, recovered from mnld */

static int dsp_fd = -1;
static int running;
static void drop_setup(void);

static int engine_start(void)
{
	u8 init[INIT_CFG_SIZE], drv[DRIVER_CFG_SIZE];
	int r;

	if (running)
		return 0;
	dsp_fd = open("/dev/stpgps", O_RDWR);
	if (dsp_fd < 0) {
		say("titan-mnld: /dev/stpgps: %s", strerror(*__errno()));
		return -1;
	}
	memcpy(init, init_cfg_default, sizeof init);
	memcpy(drv, driver_cfg_default, sizeof drv);
	config_probe(dsp_fd, init, drv);
	drop_setup();
	r = mtk_gps_mnl_run(init, drv);
	if (r != MNL_RUN_OK) {
		say("titan-mnld: mtk_gps_mnl_run failed (%d)", r);
		close(dsp_fd);
		dsp_fd = -1;
		return -1;
	}
	running = 1;
	/* the settings stock mnld applies right after starting (one byte each;
	 * from mnl.prop / MNL_Config.xml defaults on this phone) */
	{
		static u8 off = 0, on = 1;
		static u32 zero = 0;
		mtk_gps_set_param(0x3b, &off);
		mtk_gps_set_param(0x49, &off);
		mtk_gps_set_param(0x3c, &on);
		mtk_gps_set_param(0x4a, &zero);
	}
	if (dbg_fd >= 0) {
		const char *t = getenv("TITAN_MNLD_DBG_TYPE");
		u8 type = t ? (u8)strtol(t, 0, 0) : 0x11;
		say("titan-mnld: debug type 0x%x: %d", type, mtk_gps_set_debug_type(type));
	}
	if (getenv("TITAN_MNLD_RAW"))	/* MTK_MSG_RAW_MEAS, as stock raw_debug_mode */
		say("titan-mnld: raw measurements: %d", mtk_gps_set_param(0x33, 0));
	say("titan-mnld: receiver started");
	return 0;
}

static void engine_stop(void)
{
	if (!running)
		return;
	mtk_gps_mnl_stop();
	mtk_gps_set_param(0, 0);	/* as stock mnld does after stopping */
	running = 0;
	if (dsp_fd >= 0)
		close(dsp_fd);
	dsp_fd = -1;
	say("titan-mnld: stopped");
}

/* ---- dropping chip reports (TITAN_MNLD_DROP=29,3b  TITAN_MNLD_DROP_AFTER=s) ----
 * A reverse-engineering experiment: which of the chip's reports does libmnl
 * need for which measurements? libmnl imports read() from its host, so this
 * read() is the one it calls. For /dev/stpgps it passes the stream through
 * but, once DROP_AFTER seconds have passed, breaks the FE marker of each
 * frame whose type is listed, so libmnl discards it. */
#define __NR_read 3
static u8 drop_type[256];
static int drop_any;
static i64 drop_from;	/* CLOCK_MONOTONIC ms */
static int drop_state;	/* 0 idle, 1 AA seen, 2-3 length, 4 type, 5 FE, 6 payload */
static int drop_esc, drop_this, drop_pos;
static int drop_b2 = -1;	/* TITAN_MNLD_DROP_B2: only 29 records with this byte 2 */
static int drop_id = -1;	/* TITAN_MNLD_DROP_ID: and this satellite field (bits 11-16) */
static u8 drop_b1;
static u32 drop_count;

static i64 mono_ms(void)
{
	struct { long s, ns; } ts;
	clock_gettime(1, &ts);
	return (i64)ts.s * 1000 + ts.ns / 1000000;
}

static void drop_filter(u8 *p, ssize_t n)
{
	ssize_t i;

	for (i = 0; i < n; i++) {
		u8 b = p[i];
		switch (drop_state) {
		case 0:
			drop_state = b == 0xAA;
			break;
		case 1:
			drop_state = b == 0xF0 ? 2 : b == 0xAA;
			break;
		case 2:
		case 3:			/* the length, escaped like the body */
			if (b == 0xDE && !drop_esc) {
				drop_esc = 1;
				break;
			}
			drop_esc = 0;
			drop_state++;
			break;
		case 4:
			drop_this = drop_type[b] && mono_ms() >= drop_from;
			drop_state = drop_this && drop_b2 >= 0 && b == 0x29 ? 6 : 5;
			drop_pos = 0;
			break;
		case 5:
			if (drop_this && b == 0xFE) {
				p[i] = 0x00;
				drop_count++;
			}
			drop_state = 0;
			break;
		case 6:			/* FE, then payload bytes (escaped, XOR 0xCA) */
			if (b == 0xDE && !drop_esc) {
				drop_esc = 1;
				break;
			}
			if (drop_esc)
				b = b == 0xDF ? 0xAA : 0xDE;
			drop_esc = 0;
			drop_pos++;
			if (drop_pos == 3)
				drop_b1 = b ^ 0xCA;
			if (drop_pos == 4 && ((b ^ 0xCA) != drop_b2 || (drop_id >= 0 &&
			    ((drop_b1 >> 3) | ((b ^ 0xCA) & 1) << 5) != drop_id)))
				drop_state = 0;
			else if (drop_pos == 5) {
				/* break the frame: a bit of this payload byte (not
				 * an escape) fails libmnl's checksum */
				if (p[i] != 0xAA && p[i] != 0xDE && (p[i] ^ 1) != 0xAA &&
				    (p[i] ^ 1) != 0xDE) {
					p[i] ^= 1;
					drop_count++;
					drop_state = 0;
				} else
					drop_pos--;
			}
			break;
		}
	}
}

ssize_t read(int fd, void *buf, size_t n)
{
	ssize_t r = syscall(__NR_read, fd, buf, n);

	if (drop_any && fd == dsp_fd && r > 0)
		drop_filter(buf, r);
	return r;
}

static void drop_setup(void)
{
	const char *t = getenv("TITAN_MNLD_DROP");
	const char *a = getenv("TITAN_MNLD_DROP_AFTER");

	while (t && *t) {
		char *end;
		long v = strtol(t, &end, 16);
		if (end == t)
			break;
		drop_type[v & 0xff] = 1;
		drop_any = 1;
		t = *end ? end + 1 : end;
	}
	drop_from = mono_ms() + (a ? strtol(a, 0, 10) * 1000 : 0);
	if (getenv("TITAN_MNLD_DROP_B2"))
		drop_b2 = strtol(getenv("TITAN_MNLD_DROP_B2"), 0, 16);
	if (getenv("TITAN_MNLD_DROP_ID"))
		drop_id = strtol(getenv("TITAN_MNLD_DROP_ID"), 0, 10);
	if (drop_any)
		say("titan-mnld: dropping chip reports %s after %s s", getenv("TITAN_MNLD_DROP"),
		    a ? a : "0");
}

/* ---- the measurement log (TITAN_MNLD_MEAS=/path) ----
 * Once a second while running: libmnl's raw measurements and clock, as hex,
 * for reverse engineering the chip's measurement records against them
 * (docs/gnss.md, "Replacing libmnl"). Lines:
 *   M <realtime ms> <constellation array> <index> <0xa4 bytes hex>
 *   C <realtime ms> <0x58 bytes hex>
 * Entry layout (from stock mnld's update_gnss_measurement): +8 svid (i16, 0 =
 * empty), +10 constellation, +0x14 state, +0x18 received SV time (ns),
 * +0x28 C/N0 (double), +0x30 pseudorange rate, +0x44 ADR, +0x54 carrier Hz. */
#define MEAS_SIZE 0xa4
/* libmnl stores doubles into these with VFP stores: word-align them */
#define MEAS_BUF(name, n) static u8 name[n] __attribute__((aligned(8)))
MEAS_BUF(meas_gps, 0x18fc); MEAS_BUF(meas_glo, 0xf60); MEAS_BUF(meas_bds, 0x285c);
MEAS_BUF(meas_gal, 0x1710); MEAS_BUF(meas_m5, 0xc2c); MEAS_BUF(meas_m6, 0x8f8);
MEAS_BUF(meas_flags[6], 64); MEAS_BUF(meas_clock, 0x58);
static int meas_fd = -1;

static void hex_line(char tag, i64 ms, int arr, int idx, const u8 *p, u32 n)
{
	static char line[64 + 2 * 0x200];
	static const char hx[] = "0123456789abcdef";
	u32 k = snprintf(line, 64, arr < 0 ? "%c %lld " : "%c %lld %d %d ", tag, ms, arr, idx);
	u32 i;

	for (i = 0; i < n && k + 3 < sizeof line; i++) {
		line[k++] = hx[p[i] >> 4];
		line[k++] = hx[p[i] & 15];
	}
	line[k++] = '\n';
	write(meas_fd, line, k);
}

/* TITAN_MNLD_MEMDUMP=/dir: every 20 s, the process's writable memory
 * (libmnl's data and bss, the heap, the threads' stacks) to /dir/<ms>.bin,
 * each region as [u32 start][u32 length][bytes]: for finding libmnl's own
 * per-satellite state beside the measurement log */
static void mem_dump(i64 ms)
{
	static char maps[16384];
	char path[128];
	int fd, out, n = 0, k;
	char *line, *nl;
	const char *dir = getenv("TITAN_MNLD_MEMDUMP");

	if (!dir)
		return;
	fd = open("/proc/self/maps", O_RDONLY);
	if (fd < 0)
		return;
	while (n < (int)sizeof maps - 1 && (k = read(fd, maps + n, sizeof maps - 1 - n)) > 0)
		n += k;
	close(fd);
	maps[n] = 0;
	snprintf(path, sizeof path, "%s/%lld.bin", dir, ms);
	out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0)
		return;
	for (line = maps; *line; line = nl + 1) {
		u32 start, end;

		nl = strstr(line, "\n");
		if (!nl)
			break;
		*nl = 0;
		start = strtoul(line, 0, 16);
		end = strtoul(strstr(line, "-") + 1, 0, 16);
		if (strstr(line, " rw") && !strstr(line, "signal stack")) {
			u32 hdr[2] = { start, end - start };
			write(out, hdr, sizeof hdr);
			write(out, (void *)start, end - start);
		}
	}
	close(out);
}

static void meas_log(void)
{
	static i64 last;
	u8 *arrays[6] = { meas_gps, meas_glo, meas_bds, meas_gal, meas_m5, meas_m6 };
	u32 sizes[6] = { sizeof meas_gps, sizeof meas_glo, sizeof meas_bds, sizeof meas_gal,
			 sizeof meas_m5, sizeof meas_m6 };
	struct { long s, ns; } ts;
	i64 ms;
	int a;
	u32 i;

	if (meas_fd < 0 || !running)
		return;
	clock_gettime(0, &ts);		/* CLOCK_REALTIME, as strace -ttt */
	ms = (i64)ts.s * 1000 + ts.ns / 1000000;
	if (ms - last < 1000)
		return;
	last = ms;
	{
		static i64 last_dump;
		if (ms - last_dump >= 20000) {
			last_dump = ms;
			mem_dump(ms);
		}
	}
	for (a = 0; a < 6; a++)
		memset(arrays[a], 0, sizes[a]);
	memset(meas_clock, 0, sizeof meas_clock);
	mtk_gnss_get_measurement(meas_gps, meas_glo, meas_bds, meas_gal, meas_m5, meas_m6,
				 meas_flags[0], meas_flags[1], meas_flags[2], meas_flags[3],
				 meas_flags[4], meas_flags[5]);
	if (mtk_gnss_get_clock(meas_clock))
		hex_line('C', ms, -1, 0, meas_clock, sizeof meas_clock);
	for (a = 0; a < 6; a++)
		for (i = 0; i + MEAS_SIZE <= sizes[a]; i += MEAS_SIZE)
			if (*(short *)(arrays[a] + i + 8))
				hex_line('M', ms, a, i / MEAS_SIZE, arrays[a] + i, MEAS_SIZE);
}

static volatile int quit;
static void on_signal(int sig) { (void)sig; quit = 1; }

int main(void)
{
	struct sockaddr_un rx_addr;
	struct { long s, us; } tv = { 1, 0 };
	int rx, cmd;
	u8 buf[1024];

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	mkdir("/data/vendor/gps", 0770);

	if (mtk_gps_sys_function_register(callbacks) != 0) {
		say("titan-mnld: mtk_gps_sys_function_register failed");
		return 1;
	}
	hal_tx = socket(AF_UNIX, SOCK_DGRAM, 0);
	abstract_addr(&hal_addr, "mtk_mnl2hal");
	rx = socket(AF_UNIX, SOCK_DGRAM, 0);
	abstract_addr(&rx_addr, "mtk_hal2mnl");
	if (rx < 0 || bind(rx, &rx_addr, sizeof rx_addr) < 0) {
		say("titan-mnld: bind mtk_hal2mnl: %s", strerror(*__errno()));
		return 1;
	}
	setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	if (getenv("TITAN_MNLD_DBG"))
		dbg_fd = open(getenv("TITAN_MNLD_DBG"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (getenv("TITAN_MNLD_MEAS")) {
		meas_fd = open(getenv("TITAN_MNLD_MEAS"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		say("titan-mnld: measurement log %s%s", getenv("TITAN_MNLD_MEAS"),
		    meas_fd < 0 ? " (cannot open)" : "");
	}
	say("titan-mnld: ready");

	while (!quit) {
		ssize_t n = recv(rx, buf, sizeof buf, 0);
		meas_log();
		if (n < 8)
			continue;
		memcpy(&cmd, buf + 4, 4);
		switch (cmd) {
		case 101:	/* init */
			break;
		case 102:	/* start */
			if (engine_start() < 0)
				say("titan-mnld: start failed");
			break;
		case 103:	/* stop */
			engine_stop();
			break;
		case 108:	/* set_position_mode: 1 Hz is all we do */
			break;
		default:
			say("titan-mnld: ignoring HAL command %d", cmd);
		}
	}
	engine_stop();
	return 0;
}
