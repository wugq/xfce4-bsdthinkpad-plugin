/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * ctl.h -- the logic behind bsdthinkpad-plugin, without any UI (GLib only)
 *
 * A Ctl keeps the state of the controls, reads it from the hardware (hw.h)
 * and carries out requests.  Requests that need root (the speaker mute and
 * the mute LEDs, through pkexec) run asynchronously, so the caller never
 * waits; while such a request is pending, the state shows the requested
 * value.  Whenever the state changes, the "changed" callback is called; a UI
 * only renders ctl_state() and forwards user actions to the ctl_set_*()
 * functions.
 */
#ifndef BSDTHINKPAD_CTL_H
#define BSDTHINKPAD_CTL_H

#include <glib.h>

/* Every int is -1 when the control is not available */
typedef struct {
	int	brightness;	/* 0..100, backlight(9) */
	int	speaker_mute;	/* 0/1, ThinkPad hardware mute */
	int	mic_mute;	/* 0/1, recording level of all mixers muted */
} CtlState;

typedef struct Ctl Ctl;
typedef void (*CtlChanged)(const CtlState *state, gpointer data);

Ctl		*ctl_new(CtlChanged changed, gpointer data);
void		 ctl_free(Ctl *ctl);

const CtlState	*ctl_state(Ctl *ctl);

/* Re-read the hardware now; calls "changed" if anything differs */
void		 ctl_refresh(Ctl *ctl);

/* Poll the hardware every interval_ms (0 stops polling) */
void		 ctl_set_polling(Ctl *ctl, guint interval_ms);

void		 ctl_set_brightness(Ctl *ctl, int value);
void		 ctl_step_brightness(Ctl *ctl, int step);
void		 ctl_set_speaker_mute(Ctl *ctl, int mute);
void		 ctl_set_mic_mute(Ctl *ctl, int mute);

#endif
