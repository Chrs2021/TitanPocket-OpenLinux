# SPDX-License-Identifier: GPL-2.0-or-later
# The hardware's userspace (README.md): static binaries, then install.
#   make [CC=...] [CARGO_TARGET=...]
#   make install [DESTDIR=...] [PREFIX=/usr/local]
CC ?= gcc
CFLAGS ?= -O2
LDFLAGS ?= -static -s
CARGO ?= cargo
CARGO_TARGET ?=
PREFIX ?= /usr/local
DESTDIR ?=

C_PROGS = titan-wmt titan-btbridge titan-fm
HP = audio/titan-headphones
HP_BIN = $(HP)/target/$(if $(CARGO_TARGET),$(CARGO_TARGET)/)release/titan-headphones

all: $(addprefix build/,$(C_PROGS)) build/titan-headphones

build/%: connectivity/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

build/titan-headphones: $(HP)/src/main.rs $(HP)/Cargo.toml
	@mkdir -p build
	cd $(HP) && $(CARGO) build --release $(if $(CARGO_TARGET),--target $(CARGO_TARGET))
	cp $(HP_BIN) $@

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/sbin \
		$(DESTDIR)/etc/systemd/system $(DESTDIR)/etc/udev/rules.d \
		$(DESTDIR)/etc/pipewire/pipewire.conf.d $(DESTDIR)/etc/wireplumber/main.lua.d
	install -m 755 build/titan-wmt build/titan-btbridge build/titan-fm $(DESTDIR)$(PREFIX)/bin/
	install -m 755 build/titan-headphones connectivity/titan-wifi-on connectivity/titan-bt-start \
		connectivity/titan-nvram-copy $(DESTDIR)$(PREFIX)/sbin/
	install -m 644 connectivity/systemd/*.service audio/titan-headphones.service \
		$(DESTDIR)/etc/systemd/system/
	install -m 644 audio/91-titan-audio.rules $(DESTDIR)/etc/udev/rules.d/
	install -m 644 audio/pipewire/*.conf $(DESTDIR)/etc/pipewire/pipewire.conf.d/
	install -m 644 audio/wireplumber/*.lua $(DESTDIR)/etc/wireplumber/main.lua.d/
	cd audio/debian-root-session && for d in *.d; do \
		install -d $(DESTDIR)/etc/systemd/user/$$d && \
		install -m 644 $$d/titan-root.conf $(DESTDIR)/etc/systemd/user/$$d/; done

clean:
	rm -rf build $(HP)/target

.PHONY: all install clean
