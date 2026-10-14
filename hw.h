/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * hw.h -- the hardware bsdthinkpad and the panel plugin read and change
 * (FreeBSD)
 *
 * All functions return -1 when the value is not available (no backlight,
 * no acpi_ibm, no such mixer channel); callers just skip that item.
 */
#ifndef BSDTHINKPAD_HW_H
#define BSDTHINKPAD_HW_H

#define HW_BACKLIGHT_DEV	"/dev/backlight/backlight0"

#ifndef MUTE_LED_HELPER
#define MUTE_LED_HELPER		"/usr/local/libexec/bsdthinkpad-mute-led"
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
 * Default mixer, mixer(3): OSS "pcm" level 0..100.  PulseAudio's volume
 * (module-oss) is "vol"; the card's "pcm" stays as it is (measured on
 * FreeBSD 15.1, where PulseAudio's PCM write does not reach it), so it is set
 * by hand: the panel's PCM slider, "bsdthinkpad pcm".
 */
int	hw_get_pcm(void);
int	hw_set_pcm(int value);

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
 * (pkexec + polkit policy org.bsdthinkpad.mute-led).
 * which: "speaker" or "mic".  on: 0 or 1; for "speaker" also -1: set the LED
 * to the mute the controller has when the helper runs (read with GSMS right
 * before SSMS, which sets the mute too; see bsdthinkpad-mute-led.c).
 * Returns 0 on success, -1 on failure; with -1, the state the LED was set
 * to (0 or 1).
 */
int	hw_set_led(const char *which, int on);

/*
 * Tell this user's bsdthinkpad watcher that the brightness or the microphone
 * mute was just changed (SIGWINCH), so it looks at once instead of at its
 * next poll, which can be 2 s away when tpmute is loaded.  Does nothing if
 * no watcher runs.
 */
void	hw_nudge_watcher(void);

/* The tpmute kernel module is loaded (it reports the speaker mute to devd) */
int	hw_have_tpmute(void);

#endif
