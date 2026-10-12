/*
 * Hold MediaTek's hostless speaker link (Speaker_PLayback) open so the
 * FS1860 codec stays powered while audio plays on another PCM.
 * prepare -> codec unmute (amp on); close on SIGTERM/SIGINT -> amp off.
 * The stream is only prepared, never started: starting an empty playback
 * stream underruns at once, and the link carries no data anyway.
 * Usage: spk-hold [card] [device] [rate]
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <tinyalsa/pcm.h>

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

int main(int argc, char **argv)
{
	unsigned int card = argc > 1 ? atoi(argv[1]) : 0;
	unsigned int device = argc > 2 ? atoi(argv[2]) : 30;
	struct pcm_config cfg = {
		.channels = 2,
		.rate = argc > 3 ? atoi(argv[3]) : 48000,
		.format = PCM_FORMAT_S16_LE,
		.period_size = 1024,
		.period_count = 4,
	};
	struct pcm *pcm = pcm_open(card, device, PCM_OUT, &cfg);

	if (!pcm || !pcm_is_ready(pcm)) {
		fprintf(stderr, "spk-hold: %s\n", pcm ? pcm_get_error(pcm) : "open failed");
		return 1;
	}
	if (pcm_prepare(pcm)) {
		fprintf(stderr, "spk-hold: prepare: %s\n", pcm_get_error(pcm));
		pcm_close(pcm);
		return 1;
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	while (!stop)
		pause();

	pcm_close(pcm);
	return 0;
}
