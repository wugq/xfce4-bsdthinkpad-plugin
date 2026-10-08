/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * ctl.c -- the logic behind bsdthinkpad-plugin, without any UI (GLib only)
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

/* A running "pkexec bsdthinkpad-mute-led" */
typedef struct {
	Ctl	*ctl;
	guint	 watch_id;
	int	 speaker;	/* the speaker request: clears want_speaker */
} Run;

/* ---- state -------------------------------------------------------------- */

static void
read_hw(Ctl *ctl)
{
	CtlState *s = &ctl->hw;

	s->brightness = hw_get_brightness();
	s->speaker_mute = hw_get_hwmute();
	s->mic_mute = hw_get_micmute();
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
	g_debug("state: brightness %d, speaker mute %d, mic mute %d",
	    s.brightness, s.speaker_mute, s.mic_mute);
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

/* ---- root helper, asynchronously ---------------------------------------- */

static void
run_done(GPid pid, gint status, gpointer data)
{
	Run *run = data;
	Ctl *ctl = run->ctl;

	g_debug("%s helper done, %s", run->speaker ? "speaker" : "mic",
	    g_spawn_check_wait_status(status, NULL) ? "ok" : "failed");
	g_spawn_close_pid(pid);
	if (run->speaker)
		ctl->want_speaker = -1;
	ctl->runs = g_slist_remove(ctl->runs, run);
	g_free(run);
	ctl_refresh(ctl);
}

/*
 * Start "pkexec bsdthinkpad-mute-led which 0|1"; FALSE if it could not start.
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

	if (!g_file_test(MUTE_LED_HELPER, G_FILE_TEST_IS_EXECUTABLE)) {
		g_debug("%s: not installed", MUTE_LED_HELPER);
		return FALSE;
	}
	g_debug("pkexec %s %s %s", MUTE_LED_HELPER, which, argv[3]);
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

/* ---- requests ----------------------------------------------------------- */

void
ctl_set_brightness(Ctl *ctl, int value)
{
	hw_set_brightness(value);
	ctl->hw.brightness = hw_get_brightness();
	g_debug("brightness: set %d, got %d", value, ctl->hw.brightness);
	update(ctl);
}

/*
 * The hardware rounds down (50 -> 49), so a small step up may not move the
 * level at all: go further until it does, or until the end of the range.
 */
void
ctl_step_brightness(Ctl *ctl, int step)
{
	int b = hw_get_brightness(), dir = step > 0 ? 1 : -1, v;

	if (b < 0 || step == 0)
		return;
	for (v = CLAMP(b + step, 0, 100);; v += dir) {
		hw_set_brightness(v);
		if (hw_get_brightness() != b || v == 0 || v == 100)
			break;
	}
	ctl->hw.brightness = hw_get_brightness();
	g_debug("brightness: step %+d from %d, set %d, got %d", step, b, v,
	    ctl->hw.brightness);
	update(ctl);
}

/* SSMS mutes and sets the LED together; one request at a time */
void
ctl_set_speaker_mute(Ctl *ctl, int mute)
{
	mute = mute != 0;
	if (ctl->want_speaker >= 0 || ctl->hw.speaker_mute < 0) {
		g_debug("speaker mute %d: refused (%s)", mute,
		    ctl->want_speaker >= 0 ? "a request is pending" :
		    "no acpi_ibm");
		return;
	}
	if (mute != ctl->hw.speaker_mute &&
	    run_helper(ctl, "speaker", mute, 1))
		ctl->want_speaker = mute;
	update(ctl);
}

/*
 * The mixer is ours to change; the LED needs root.  The bsdthinkpad watcher
 * sets the LED too when it sees the change; that is harmless (same value),
 * and keeps the LED right when bsdthinkpad does not run.
 */
void
ctl_set_mic_mute(Ctl *ctl, int mute)
{
	mute = mute != 0;
	if (hw_set_micmute(mute) == 0)
		run_helper(ctl, "mic", mute, 0);
	else
		g_debug("mic mute %d: the mixers can not be changed", mute);
	ctl_refresh(ctl);
}

/* ---- life --------------------------------------------------------------- */

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
