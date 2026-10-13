# Titan Pocket userspace for the connectivity drivers and the power keys.
#
#   make                                  native build on the phone / arm64
#   make CC=aarch64-linux-gnu-gcc         cross build
#   make LDFLAGS=-static                  static binaries
#   sudo make install                     binaries, scripts, systemd units
#   sudo systemctl enable titan-speaker titan-bt titan-powerkey   (and titan-wifi)
#
# The systemd units and scripts expect PREFIX=/usr/local.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall
LDFLAGS ?=
PREFIX  ?= /usr/local
UNITDIR ?= /etc/systemd/system

TINYALSA     := third_party/tinyalsa
TINYALSA_SRC := $(addprefix $(TINYALSA)/src/,pcm.c pcm_hw.c pcm_plugin.c \
                  mixer.c mixer_hw.c mixer_plugin.c limits.c snd_card_plugin.c)

TOOLS        := titan-wmt titan-btbridge titan-fm titan-wlan-cmd
ALSA_TOOLS   := spk-hold titan-speakerd tinymix
SBIN_SCRIPTS := titan-bt-start titan-nvram-copy titan-wifi-on titan-powerkey
BIN_SCRIPTS  := titan-radio
UNITS        := titan-wmt.service titan-wifi.service titan-bt.service \
                titan-powerkey.service titan-speaker.service

all: $(addprefix build/,$(TOOLS) $(ALSA_TOOLS))

build:
	mkdir -p build

build/spk-hold: src/spk-hold.c $(TINYALSA_SRC) | build
	$(CC) $(CFLAGS) -I$(TINYALSA)/include $(LDFLAGS) -o $@ $^ -ldl

build/titan-speakerd: src/titan-speakerd.c $(TINYALSA_SRC) | build
	$(CC) $(CFLAGS) -I$(TINYALSA)/include $(LDFLAGS) -o $@ $^ -ldl

build/tinymix: $(TINYALSA)/utils/tinymix.c $(TINYALSA_SRC) | build
	$(CC) $(CFLAGS) -I$(TINYALSA)/include $(LDFLAGS) -o $@ $^ -ldl

build/%: src/%.c | build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/sbin $(DESTDIR)$(UNITDIR)
	install -m 755 $(addprefix build/,$(TOOLS) $(ALSA_TOOLS)) $(DESTDIR)$(PREFIX)/bin/
	install -m 755 $(addprefix bin/,$(BIN_SCRIPTS)) $(DESTDIR)$(PREFIX)/bin/
	install -m 755 $(addprefix sbin/,$(SBIN_SCRIPTS)) $(DESTDIR)$(PREFIX)/sbin/
	install -m 644 $(addprefix systemd/,$(UNITS)) $(DESTDIR)$(UNITDIR)/

clean:
	rm -rf build

.PHONY: all install clean
