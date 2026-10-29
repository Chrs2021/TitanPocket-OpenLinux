/*
 * mnlspy: log how mnld drives libmnl.so (a reverse-engineering aid).
 *
 * LD_PRELOADed into stock mnld (in /opt/android-gps), it interposes every
 * libmnl entry point mnld imports and every callback mnld registers with
 * mtk_gps_sys_function_register(), logging arguments (and the config structs
 * passed to mtk_gps_mnl_run) to /tmp/mnlspy.log inside the chroot, then
 * calls through. This is the ground truth for titan-mnld, the open
 * replacement for mnld.
 *
 * Arguments are forwarded as six ints and the result as 64 bits: on
 * armeabi-v7a (softfp) that passes r0-r3 plus two stack words and returns
 * r0:r1 unchanged, whatever the real prototype.
 *
 *   tools/gnss/build-mnlspy.sh
 */
typedef unsigned int size_t;
typedef long long i64;

extern void *dlsym(void *handle, const char *name);
extern int open(const char *path, int flags, ...);
extern int write(int fd, const void *buf, size_t n);
extern int snprintf(char *s, size_t n, const char *fmt, ...);
extern int gettid(void);
extern int clock_gettime(int clk, void *ts);

#define RTLD_NEXT ((void *)0xfffffffe)	/* bionic, 32-bit */
#define O_WRONLY 01
#define O_CREAT 0100
#define O_APPEND 02000

static int logfd = -1;

static void out(const char *s, int n)
{
	if (logfd < 0)
		logfd = open("/tmp/mnlspy.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (logfd >= 0)
		write(logfd, s, n);
}

static int stamp(char *b, int n)
{
	struct { long s, ns; } ts;

	clock_gettime(1, &ts);
	return snprintf(b, n, "%5ld.%03ld [%d] ", ts.s % 100000, ts.ns / 1000000, gettid());
}

/* printable text, or "" */
static void text(char *dst, int dn, const char *src, int max)
{
	int i, k = 0;

	dst[0] = 0;
	if ((unsigned)src < 0x10000)
		return;
	for (i = 0; i < max && src[i] && k < dn - 4; i++) {
		unsigned char c = src[i];
		if (c == '\n') { dst[k++] = '\\'; dst[k++] = 'n'; }
		else if (c == '\r') continue;
		else if (c >= 32 && c < 127) dst[k++] = c;
		else return (void)(dst[0] = 0);
	}
	dst[k] = 0;
}

static void log_call(const char *what, const char *name, int a, int b, int c, int d)
{
	char line[512], s[300];
	int n = stamp(line, sizeof line);

	text(s, sizeof s, (const char *)a, 280);
	if (!s[0])
		text(s, sizeof s, (const char *)b, 280);
	n += snprintf(line + n, sizeof line - n, "%s %s(0x%x, 0x%x, 0x%x, 0x%x)%s%s%s\n",
		      what, name, a, b, c, d, s[0] ? " \"" : "", s, s[0] ? "\"" : "");
	out(line, n);
}

static void log_ret(const char *name, i64 r)
{
	char line[128];
	int n = stamp(line, sizeof line);

	n += snprintf(line + n, sizeof line - n, "  %s -> 0x%x\n", name, (int)r);
	out(line, n);
}

static void hexdump(const char *title, const unsigned char *p, int len)
{
	char line[200];
	int i, j, n;

	n = snprintf(line, sizeof line, "  %s @%p, %d bytes:\n", title, p, len);
	out(line, n);
	if ((unsigned)p < 0x10000)
		return;
	for (i = 0; i < len; i += 16) {
		n = snprintf(line, sizeof line, "    %04x:", i);
		for (j = i; j < i + 16 && j < len; j++)
			n += snprintf(line + n, sizeof line - n, " %02x", p[j]);
		n += snprintf(line + n, sizeof line - n, "  ");
		for (j = i; j < i + 16 && j < len; j++)
			line[n++] = p[j] >= 32 && p[j] < 127 ? p[j] : '.';
		line[n++] = '\n';
		out(line, n);
	}
}

/* ---- libmnl entry points mnld imports ---- */

typedef i64 (*fn6)(int, int, int, int, int, int);

#define WRAP(name)							\
i64 name(int a, int b, int c, int d, int e, int f)			\
{									\
	static fn6 real;						\
	i64 r;								\
	if (!real)							\
		real = (fn6)dlsym(RTLD_NEXT, #name);			\
	log_call("call", #name, a, b, c, d);				\
	r = real(a, b, c, d, e, f);					\
	log_ret(#name, r);						\
	return r;							\
}

WRAP(mtk_agps_set_param_with_payload_len)
WRAP(mtk_gps_get_MNL_Config_XML_param)
WRAP(mtk_gps_inject_nlp_location)
WRAP(mtk_gps_inject_ntp_time)
WRAP(mtk_gps_mnl_create_dbg_port)
WRAP(mtk_agps_agent_epo_file_update)
WRAP(mtk_agps_agent_mtknav_file_update)
WRAP(mtk_agps_agent_qepo_file_update)
WRAP(mtk_agps_agent_qepo_get_rqst_time)
WRAP(mtk_agps_set_param)
WRAP(mtk_gnss_get_clock)
WRAP(mtk_gnss_get_measurement)
WRAP(mtk_gnss_get_navigation_event)
WRAP(mtk_gps_delete_nv_data)
WRAP(mtk_gps_get_mnl_info)
WRAP(mtk_gps_get_position_accuracy)
WRAP(mtk_gps_get_rtc_info)
WRAP(mtk_gps_mnl_stop)
WRAP(mtk_gps_set_debug_type)
WRAP(mtk_agps_agent_qepo_bd_file_update)
WRAP(mtk_agps_agent_qepo_ga_file_update)
WRAP(mtk_gps_mnl_get_sensor_info)
WRAP(mtk_gps_mnl_mpe_callback_reg)
WRAP(mtk_gps_set_mpe_info)

i64 mtk_gps_mnl_run(int a, int b, int c, int d, int e, int f)
{
	static fn6 real;
	i64 r;

	if (!real)
		real = (fn6)dlsym(RTLD_NEXT, "mtk_gps_mnl_run");
	log_call("call", "mtk_gps_mnl_run", a, b, c, d);
	hexdump("arg0 (init cfg)", (const unsigned char *)a, 0x60);
	hexdump("arg1 (driver cfg?)", (const unsigned char *)b, 0x434);
	r = real(a, b, c, d, e, f);
	log_ret("mtk_gps_mnl_run", r);
	return r;
}

/* the value argument is a pointer; show what it points at */
i64 mtk_gps_set_param(int a, int b, int c, int d, int e, int f)
{
	static fn6 real;
	i64 r;

	if (!real)
		real = (fn6)dlsym(RTLD_NEXT, "mtk_gps_set_param");
	log_call("call", "mtk_gps_set_param", a, b, c, d);
	hexdump("value", (const unsigned char *)b, 8);
	r = real(a, b, c, d, e, f);
	log_ret("mtk_gps_set_param", r);
	return r;
}

/* ---- the callbacks mnld registers ---- */

#define NCB 17
static const char *cb_name[NCB] = {
	"gps_mnl_callback", "nmea_output_to_app", "nmea_output_to_mnld",
	"frame_sync_enable_sleep_mode", "frame_sync_meas_req_by_network",
	"frame_sync_meas_req", "agps_disaptcher_callback", "pmtk_cmd_cb(null)",
	"cb8", "cb9", "ofl_sys_rst_stpgps_req", "ofl_sys_submit_flp_data",
	"dbg2file_mnld", "ofl_sys_mnl_offload_callback",
	"data2mnld_callback", "do_hw_suspend_resume", "suspend_extention_set",
};
static fn6 cb_real[NCB];
static fn6 cb_table[NCB + 8];

#define CB(i)								\
static i64 cb##i(int a, int b, int c, int d, int e, int f)		\
{									\
	i64 r;								\
	if (i != 12) /* dbg2file: mnld's own debug log, very chatty */	\
		log_call("cb", cb_name[i], a, b, c, d);			\
	r = cb_real[i](a, b, c, d, e, f);				\
	return r;							\
}
CB(0) CB(1) CB(2) CB(3) CB(4) CB(5) CB(6) CB(7) CB(8)
CB(9) CB(10) CB(11) CB(12) CB(13) CB(14) CB(15) CB(16)

static fn6 cb_wrap[NCB] = {
	cb0, cb1, cb2, cb3, cb4, cb5, cb6, cb7, cb8,
	cb9, cb10, cb11, cb12, cb13, cb14, cb15, cb16,
};

i64 mtk_gps_sys_function_register(int a, int b, int c, int d, int e, int f)
{
	static fn6 real;
	fn6 *tbl = (fn6 *)a;
	int i;

	if (!real)
		real = (fn6)dlsym(RTLD_NEXT, "mtk_gps_sys_function_register");
	log_call("call", "mtk_gps_sys_function_register", a, b, c, d);
	hexdump("callback table", (const unsigned char *)a, 4 * (NCB + 8));
	for (i = 0; i < NCB + 8; i++)
		cb_table[i] = tbl[i];
	for (i = 0; i < NCB; i++) {
		cb_real[i] = tbl[i];
		if (tbl[i])
			cb_table[i] = cb_wrap[i];
	}
	return real((int)cb_table, b, c, d, e, f);
}
