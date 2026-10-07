/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * ctl.c -- the logic behind tposd-panel, without any UI (GLib only)
 */

#include <string.h>

#include "ctl.h"
#include "hw.h"

struct Ctl {
	CtlState	 hw;		/* last read from the hardware */
	CtlState	 state;		/* hw with pending requests applied */
	gboolean	 valid;		/* state has been reported once */
	int		 want_speaker;	/* pending speaker request, or -1 */
	guint		 poll_id;
	GSList		*runs;		/* helpers still running */
	CtlChanged	 changed;
	gpointer	 data;
};

/* A running "pkexec tposd-mute-led" */
typedef struct {
	Ctl	*ctl;
	guint	 watch_id;
	int	 speaker;	/* the speaker request: clears want_speaker */
} Run;

/* ---- state ------------------------------------------------------------------ */

static void
read_hw(Ctl *ctl)
{
	CtlState *s = &ctl->hw;

	s->brightness = hw_get_brightness();
	s->pcm = hw_get_pcm();
	s->speaker_mute = hw_get_hwmute();
	s->mic_mute = hw_get_micmute();
	if (hw_get_oss_levels(&s->oss_unit, &s->oss_vol, &s->oss_pcm) != 0) {
		s->oss_unit = -1;
		s->oss_vol = s->oss_pcm = -1;
	}
}

/* Combine the hardware state with pending requests; report a change */
static void
update(Ctl *ctl)
{
	CtlState s = ctl->hw;

	if (ctl->want_speaker >= 0)
		s.speaker_mute = ctl->want_speaker;
	if (ctl->valid && memcmp(&s, &ctl->state, sizeof(s)) == 0)
		return;
	ctl->state = s;
	ctl->valid = TRUE;
	if (ctl->changed != NULL)
		ctl->changed(&ctl->state, ctl->data);
}

void
ctl_refresh(Ctl *ctl)
{
	read_hw(ctl);
	update(ctl);
}

static gboolean
poll_tick(gpointer data)
{
	ctl_refresh(data);
	return G_SOURCE_CONTINUE;
}

void
ctl_set_polling(Ctl *ctl, guint interval_ms)
{
	if (ctl->poll_id != 0) {
		g_source_remove(ctl->poll_id);
		ctl->poll_id = 0;
	}
	if (interval_ms > 0) {
		ctl_refresh(ctl);
		ctl->poll_id = g_timeout_add(interval_ms, poll_tick, ctl);
	}
}

const CtlState *
ctl_state(Ctl *ctl)
{
	return &ctl->state;
}

/* ---- root helper, asynchronously ---------------------------------------------- */

static void
run_done(GPid pid, gint status, gpointer data)
{
	Run *run = data;
	Ctl *ctl = run->ctl;

	(void)status;
	g_spawn_close_pid(pid);
	if (run->speaker)
		ctl->want_speaker = -1;
	ctl->runs = g_slist_remove(ctl->runs, run);
	g_free(run);
	ctl_refresh(ctl);
}

/*
 * Start "pkexec tposd-mute-led which 0|1"; FALSE if it could not start.
 * Never wait for it here: run synchronously from a GTK handler while the
 * popup held its grab, pkexec made the plugin crash (Xlib _XAllocID
 * assertion).
 */
static gboolean
run_helper(Ctl *ctl, const char *which, int on, int speaker)
{
	char *argv[] = { "pkexec", MUTE_LED_HELPER, (char *)which,
	    on ? "1" : "0", NULL };
	GError *error = NULL;
	Run *run;
	GPid pid;

	if (!g_file_test(MUTE_LED_HELPER, G_FILE_TEST_IS_EXECUTABLE))
		return FALSE;
	if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH |
	    G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL |
	    G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &pid, &error)) {
		g_warning("%s: %s", MUTE_LED_HELPER, error->message);
		g_error_free(error);
		return FALSE;
	}
	run = g_new0(Run, 1);
	run->ctl = ctl;
	run->speaker = speaker;
	run->watch_id = g_child_watch_add(pid, run_done, run);
	ctl->runs = g_slist_prepend(ctl->runs, run);
	return TRUE;
}

/* ---- requests ------------------------------------------------------------------- */

void
ctl_set_brightness(Ctl *ctl, int value)
{
	hw_set_brightness(value);
	ctl->hw.brightness = hw_get_brightness();
	update(ctl);
}

void
ctl_step_brightness(Ctl *ctl, int step)
{
	int b = hw_get_brightness();

	if (b >= 0)
		ctl_set_brightness(ctl, b + step);
}

void
ctl_set_pcm(Ctl *ctl, int value)
{
	hw_set_pcm(value);
	ctl_refresh(ctl);		/* also updates oss_pcm */
}

/* SSMS mutes and sets the LED together; one request at a time */
void
ctl_set_speaker_mute(Ctl *ctl, int mute)
{
	mute = mute != 0;
	if (ctl->want_speaker >= 0 || ctl->hw.speaker_mute < 0)
		return;
	if (mute != ctl->hw.speaker_mute &&
	    run_helper(ctl, "speaker", mute, 1))
		ctl->want_speaker = mute;
	update(ctl);
}

/*
 * The mixer is ours to change; the LED needs root.  The tposd watcher sets
 * the LED too when it sees the change; that is harmless (same value), and
 * keeps the LED right when tposd does not run.
 */
void
ctl_set_mic_mute(Ctl *ctl, int mute)
{
	mute = mute != 0;
	if (hw_set_micmute(mute) == 0)
		run_helper(ctl, "mic", mute, 0);
	ctl_refresh(ctl);
}

/* ---- life -------------------------------------------------------------------------- */

Ctl *
ctl_new(CtlChanged changed, gpointer data)
{
	Ctl *ctl = g_new0(Ctl, 1);

	ctl->want_speaker = -1;
	ctl->changed = changed;
	ctl->data = data;
	read_hw(ctl);
	update(ctl);
	return ctl;
}

void
ctl_free(Ctl *ctl)
{
	GSList *l;

	if (ctl->poll_id != 0)
		g_source_remove(ctl->poll_id);
	/* Helpers still running finish on their own; just stop watching */
	for (l = ctl->runs; l != NULL; l = l->next) {
		g_source_remove(((Run *)l->data)->watch_id);
		g_free(l->data);
	}
	g_slist_free(ctl->runs);
	g_free(ctl);
}
