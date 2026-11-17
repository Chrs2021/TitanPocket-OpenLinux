/*
 * avref: decode an Annex B H.264 file with the phone's libavcodec (FFmpeg
 * 5.1, dlopened: Debian ships no headers without the -dev package) and
 * write I420 frames, as the reference for titan_vdec (docs/video-decoder.md).
 *   avref in.h264 out.yuv [threads]
 * Build (aarch64): gcc -O2 -o avref avref.c -ldl
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* FFmpeg 5.1 (libavutil 57): the leading fields of AVFrame */
struct frame_head {
	uint8_t *data[8];
	int linesize[8];
	uint8_t **extended_data;
	int width, height;
};
struct packet_head {		/* AVPacket, libavcodec 59 */
	void *buf;
	int64_t pts, dts;
	uint8_t *data;
	int size;
};

#define AV_NOPTS_VALUE ((int64_t)0x8000000000000000ULL)

static void *sym(void *h, const char *n)
{
	void *p = dlsym(h, n);

	if (!p) {
		fprintf(stderr, "%s missing\n", n);
		exit(1);
	}
	return p;
}

int main(int argc, char **argv)
{
	void *lc = dlopen("libavcodec.so.59", RTLD_NOW), *lu = dlopen("libavutil.so.57", RTLD_NOW);
	void *(*find)(const char *), *(*alloc_ctx)(void *), *(*frame_alloc)(void);
	void *(*packet_alloc)(void), *(*parser_init)(int), *(*opt_set)(void *, const char *,
								    const char *, int);
	int (*open2)(void *, void *, void *), (*send)(void *, void *), (*recv)(void *, void *);
	int (*parse2)(void *, void *, uint8_t **, int *, const uint8_t *, int, int64_t, int64_t,
		      int64_t);
	FILE *in, *out;
	uint8_t *buf;
	long len, pos = 0;
	void *codec, *ctx, *fr, *pkt, *parser;
	int n = 0;
	double fps = argc > 4 ? atof(argv[4]) : 0;
	struct timespec t0;

	if (argc < 3 || !lc || !lu) {
		fprintf(stderr, "usage: avref in.h264 out.yuv [threads] (%s)\n", dlerror());
		return 1;
	}
	find = sym(lc, "avcodec_find_decoder_by_name");
	alloc_ctx = sym(lc, "avcodec_alloc_context3");
	open2 = sym(lc, "avcodec_open2");
	send = sym(lc, "avcodec_send_packet");
	recv = sym(lc, "avcodec_receive_frame");
	packet_alloc = sym(lc, "av_packet_alloc");
	parser_init = sym(lc, "av_parser_init");
	parse2 = sym(lc, "av_parser_parse2");
	frame_alloc = sym(lu, "av_frame_alloc");
	opt_set = sym(lu, "av_opt_set");

	in = fopen(argv[1], "rb");
	out = strcmp(argv[2], "-") ? fopen(argv[2], "wb") : fopen("/dev/null", "wb");
	if (!in || !out)
		return 1;
	fseek(in, 0, SEEK_END);
	len = ftell(in);
	rewind(in);
	buf = calloc(1, len + 64);
	if (fread(buf, 1, len, in) != (size_t)len)
		return 1;
	codec = find("h264");
	ctx = alloc_ctx(codec);
	opt_set(ctx, "threads", argc > 3 ? argv[3] : "1", 0);
	if (open2(ctx, codec, NULL) < 0)
		return 1;
	parser = parser_init(27);	/* AV_CODEC_ID_H264 */
	fr = frame_alloc();
	pkt = packet_alloc();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int flushed = 0;;) {
		struct packet_head *p = pkt;
		uint8_t *d;
		int sz, used;

		used = parse2(parser, ctx, &d, &sz, buf + pos, (int)(len - pos),
			      AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
		pos += used;
		p->data = d;
		p->size = sz;
		if (sz || (pos >= len && flushed++)) {
			send(ctx, sz ? pkt : NULL);
			while (!recv(ctx, fr)) {
				struct frame_head *f = fr;
				int pl, y;

				if (fps > 0) {
					long long ns = t0.tv_nsec + (long long)(n * 1e9 / fps);
					struct timespec t = { t0.tv_sec + ns / 1000000000, ns % 1000000000 };

					clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
				}
				for (pl = 0; pl < 3 && strcmp(argv[2], "-"); pl++) {
					int w = pl ? f->width / 2 : f->width;
					int h = pl ? f->height / 2 : f->height;

					for (y = 0; y < h; y++)
						fwrite(f->data[pl] + (long)y * f->linesize[pl], 1, w, out);
				}
				n++;
			}
		}
		if (pos >= len && !sz && flushed > 1)
			break;
	}
	fclose(out);
	fprintf(stderr, "%d frames\n", n);
	return 0;
}
