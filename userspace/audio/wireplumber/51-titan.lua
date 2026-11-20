-- SPDX-License-Identifier: GPL-2.0-or-later
-- Titan Pocket: the MediaTek AFE card exposes ~40 PCMs without a UCM
-- profile, so automatic ALSA device setup produces nothing usable. The
-- loudspeaker sink is defined in /etc/pipewire/pipewire.conf.d/50-titan-speaker.conf.
alsa_monitor.enabled = false
