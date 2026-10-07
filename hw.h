/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * hw.h -- the hardware tposd and tposd-panel read and change (FreeBSD)
 *
 * All functions return -1 when the value is not available (no backlight,
 * no acpi_ibm, no such mixer channel); callers just skip that item.
 */
#ifndef TPOSD_HW_H
#define TPOSD_HW_H

#define HW_BACKLIGHT_DEV	"/dev/backlight/backlight0"

#ifndef MUTE_LED_HELPER
#define MUTE_LED_HELPER		"/usr/local/libexec/tposd-mute-led"
#endif

/* Screen brightness 0..100, backlight(9) */
int	hw_get_brightness(void);
int	hw_set_brightness(int value);

/*
 * Speaker mute done by the ThinkPad embedded controller: 0/1, acpi_ibm(4).
 * Setting it goes through the root helper (hw_set_led("speaker"): ACPI
 * method SSMS, which mutes and sets the LED together).
 */
int	hw_get_hwmute(void);

/*
 * Default mixer, mixer(3): OSS "pcm" level 0..100.  pulseaudio (module-oss)
 * sets "vol" but does not reliably manage "pcm", which limits what you hear.
 */
int	hw_get_pcm(void);
int	hw_set_pcm(int value);

/*
 * Default mixer, read only: unit N of /dev/mixerN and the OSS
 * levels of "vol" and "pcm", 0.0..1.0 as mixer(8) shows them (-1 when the
 * channel is missing).  pulseaudio (module-oss) sets "vol"; "pcm" it does
 * not reliably manage.
 */
int	hw_get_oss_levels(int *unit, float *vol, float *pcm);

/*
 * Microphone mute 0/1: the recording level ("rec") of every mixer that has
 * one, so that all inputs are cut, whichever device a program records from
 * (e.g. the internal microphone is often a device of its own, such as
 * "pcm4: Internal Analog Mic").  Muted only when all of them are muted.
 */
int	hw_get_micmute(void);
int	hw_set_micmute(int on);

/*
 * Set the speaker or microphone mute LED through the root helper
 * (pkexec + polkit policy org.tposd.mute-led).  which: "speaker" or "mic".
 * Returns 0 on success.
 */
int	hw_set_led(const char *which, int on);

#endif
