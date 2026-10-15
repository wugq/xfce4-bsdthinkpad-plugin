/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * bsdthinkpad -- on-screen display for ThinkPad keys on FreeBSD (XFCE)
 *
 * Shows desktop notifications for what the XFCE pulseaudio plugin and power
 * manager can not see on FreeBSD, and drives the ThinkPad mute LEDs:
 *
 *   brightness       /dev/backlight/backlight0 (backlight(9)), with a bar
 *   speaker mute     the embedded controller mutes the speaker in hardware;
 *                    only the sysctl dev.acpi_ibm.0.mute changes, and the
 *                    key sends no event.  bsdthinkpad turns the mute LED
 *                    on/off (bsdthinkpad-mute-led via pkexec), which
 *                    acpi_ibm(4) does not, and keeps the PulseAudio sink
 *                    mute in step with it, so the pulseaudio plugin shows
 *                    it (no popup of ours)
 *   microphone mute  the "rec" level of every mixer (mixer(3), see hw.h); the
 *                    LED follows it, whoever changed it (key, panel, command)
 *
 * polkit may refuse bsdthinkpad's pkexec, because ConsoleKit can not always
 * read a process's environment (kern.proc.env fails with ENOMEM for a few percent
 * of processes, depending on where ASLR puts the stack; see
 * docs/kern-proc-env-bug.md), and so does not find the session.  bsdthinkpad
 * checks this at start and logs it.
 *
 * None of these send an event of their own, so the watcher polls them, every
 * 200 ms.  With the tpmute kernel module loaded (a separate project: it
 * reports the speaker mute to devd as "system=ACPI subsystem=IBM type=MUTE"),
 * it waits instead: it looks at once on an acpi_ibm event from devd (the
 * speaker mute from tpmute, the mic-mute key) and when "bsdthinkpad
 * brightness" or the panel plugin changed something (SIGWINCH, see
 * hw_nudge_watcher()), and otherwise only every 2 s, for changes made by
 * other programs.
 *
 * The watcher logs to syslog(3) (ident "bsdthinkpad", /var/log/messages).
 *
 * Volume is left to the pulseaudio panel plugin (keys and popups).
 *
 * Usage:
 *   bsdthinkpad [-n] [-i ms]          watch and notify (run from autostart)
 *   bsdthinkpad brightness [+|-]N     change brightness, e.g. +10, -10, 50
 *   bsdthinkpad pcm [+|-]N            change the OSS "pcm" level of the default
 *                               mixer (pulseaudio restores only "vol" at start)
 *   bsdthinkpad micmute [on|off|toggle]
 *                               show or change the microphone mute (used
 *                               by the devd rule of the mic-mute key)
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/un.h>

#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <glib-unix.h>
#include <libnotify/notify.h>

#include "hw.h"

#ifndef PACTL
#define PACTL	"/usr/local/bin/pactl"
#endif

/* ---- tunables ----------------------------------------------------------- */

#define TIMEOUT_MS	2000		/* notifications, as the pulseaudio
					   plugin's */
#define FAST_MS		30		/* poll this often for FAST_FOR_US */
#define FAST_FOR_US	(2 * G_USEC_PER_SEC)	/* after a change or an event,
					   so a brightness bar follows a key
					   held down step by step */
#define IDLE_MS		2000		/* with tpmute and devd: the poll is
					   only a safety net for changes no
					   one tells us about */
#define SINK_RECHECK_MS	150		/* the sink once more after a speaker
					   mute change: the pulseaudio plugin
					   may toggle it a moment late */
#define LED_SETTLE_MS	500		/* within this of the last change, the
					   speaker LED waits until the mute
					   has stayed put this long */
#define LED_MISMATCH_MAX 3		/* LED syncs in a row that may disagree
					   with the mute, see speaker_led() */
#define DEVD_SOCKET	"/var/run/devd.seqpacket.pipe"
#define DEVD_RETRY_US	(10 * G_USEC_PER_SEC)

/* ---- state -------------------------------------------------------------- */

static int interval_ms = 200;		/* -i: how often to poll */
static int drive_leds = 1;		/* -n clears it: no mute LEDs */

/* Each value watched; last = last seen, -1 = unknown / not available */
static struct {
	int		 last;
	NotifyNotification *n;		/* updated in place, so a new one
					   replaces the old */
} brightness = { -1, NULL }, mic = { -1, NULL };

static struct {
	int	 last;			/* dev.acpi_ibm.0.mute */
	guint	 sink_recheck;		/* pending speaker_sink_recheck() */
	guint	 led_settle;		/* pending speaker_led() */
	gint64	 changed;		/* g_get_monotonic_time() of the last
					   change */
	int	 led_mismatch;		/* LED syncs in a row that disagreed */
} speaker = { -1, 0, 0, 0, 0 };

/* When to look next, see next_interval() */
static struct {
	guint	 poll;			/* pending poll_tick() */
	gint64	 fast_until;		/* g_get_monotonic_time() */
	int	 devd_fd;		/* connection to devd, or -1 */
	gint64	 devd_tried;		/* last connection attempt */
} sched = { 0, 0, -1, 0 };

/* Failures already logged (once each, until it works again) */
static int led_warned, sink_warned;

/* ---- what the watcher acts on: LEDs, PulseAudio, notifications ---------- */

/*
 * Set a mute LED through the root helper; with on = -1 ("sync", speaker only)
 * the helper sets it to the firmware's mute and returns that state.  On
 * failure warn once and keep trying at the next change: the cause may be
 * passing (e.g. polkit or PAM, see README).
 */
static int
led(const char *which, int on)
{
	int r;

	if (!drive_leds)
		return -1;
	if ((r = hw_set_led(which, on)) >= 0)
		led_warned = 0;
	else if (!led_warned) {
		syslog(LOG_WARNING, "pkexec %s %s %s failed; "
		    "the mute LED does not follow (logged once)",
		    MUTE_LED_HELPER, which,
		    on < 0 ? "sync" : on ? "1" : "0");
		led_warned = 1;
	}
	return r;
}

/*
 * Run "pactl <cmd> @DEFAULT_SINK@ [arg]" and wait for it; TRUE if it ran and
 * exited 0.  Its output goes to *out (free it) if out is not NULL.
 * LC_ALL=C: pactl translates its "yes" and "no".
 */
static gboolean
pactl(const char *cmd, const char *arg, char **out)
{
	char *argv[] = { PACTL, (char *)cmd, "@DEFAULT_SINK@", (char *)arg,
	    NULL };
	char **env = g_environ_setenv(g_get_environ(), "LC_ALL", "C", TRUE);
	int status;
	gboolean ok;

	ok = g_spawn_sync(NULL, argv, env, out == NULL ?
	    G_SPAWN_STDOUT_TO_DEV_NULL : 0, NULL, NULL, out, NULL, &status,
	    NULL) && g_spawn_check_wait_status(status, NULL);
	g_strfreev(env);
	return ok;
}

/* Is the PulseAudio sink muted?  1/0, -1 if pactl fails */
static int
sink_muted(void)
{
	char *out = NULL;
	int muted = -1;

	if (pactl("get-sink-mute", NULL, &out) && out != NULL) {
		if (strstr(out, "yes") != NULL)
			muted = 1;
		else if (strstr(out, "no") != NULL)
			muted = 0;
	}
	g_free(out);
	return muted;
}

static void
set_sink_mute(int on)
{
	if (pactl("set-sink-mute", on ? "1" : "0", NULL))
		sink_warned = 0;
	else if (!sink_warned) {
		syslog(LOG_WARNING, "%s set-sink-mute failed; the speaker "
		    "mute in PulseAudio may not follow the hardware "
		    "(logged once)", PACTL);
		sink_warned = 1;
	}
}

/*
 * Show (or replace) a notification: a summary only, as the pulseaudio
 * plugin and the power manager show theirs.  value >= 0 adds the "value"
 * hint, which notification daemons such as xfce4-notifyd draw as a bar.
 */
static void
show(NotifyNotification **n, const char *summary, const char *icon,
    int value)
{
	GError *error = NULL;

	if (*n == NULL) {
		*n = notify_notification_new(summary, NULL, icon);
		notify_notification_set_timeout(*n, TIMEOUT_MS);
		/* An on-screen display: keep it out of the notification log */
		notify_notification_set_hint(*n, "transient",
		    g_variant_new_boolean(TRUE));
	} else {
		notify_notification_update(*n, summary, NULL, icon);
	}
	if (value >= 0)
		notify_notification_set_hint_int32(*n, "value", value);
	if (!notify_notification_show(*n, &error)) {
		syslog(LOG_WARNING, "notification failed: %s", error->message);
		g_error_free(error);
	}
}

/* ---- brightness: a notification with a bar ------------------------------ */

static void
check_brightness(int first)
{
	char text[32];
	int b = hw_get_brightness();

	if (b < 0 || b == brightness.last)
		return;
	if (!first) {
		/* As the power manager words it; no theme has per-level
		   brightness icons */
		snprintf(text, sizeof(text), "Brightness: %d%%", b);
		show(&brightness.n, text, "display-brightness-symbolic", b);
		/* the key may still be held down: follow it closely */
		sched.fast_until = g_get_monotonic_time() + FAST_FOR_US;
	}
	brightness.last = b;
}

/* ---- speaker mute: the PulseAudio sink, then the LED -------------------- */

/*
 * The embedded controller mutes the speaker itself.  The watcher only makes
 * the rest follow, and never changes the mute:
 *
 * - the PulseAudio sink mute, at once (and once more SINK_RECHECK_MS later):
 *   the pulseaudio plugin shows the sink, so setting it is what shows the
 *   speaker mute, on every ThinkPad.  Where the mute key reaches the desktop
 *   (the X1 Carbon) the plugin toggles the sink itself, but the volume keys
 *   unmute only the controller, and a quick double press can leave the two
 *   apart; where it does not (the A475) the sink never follows on its own.
 *
 * - the LED: at once after a single press, but while the key is pressed
 *   quickly (a change within LED_SETTLE_MS of the one before) only once the
 *   mute has stayed put for LED_SETTLE_MS.  The helper calls into the
 *   firmware (GSMS, an SMI); doing that on every change of a quick series
 *   left the firmware's audio mute and the controller's apart (traced on the
 *   A475).  "sync" (GSMS), never SSMS: SSMS sets the mute too, and writing
 *   back a state just read could undo a press.
 */

static void
speaker_sink_sync(void)
{
	int hw = hw_get_hwmute(), sink = sink_muted();

	if (hw >= 0 && sink >= 0 && sink != hw)
		set_sink_mute(hw);
}

static gboolean
speaker_sink_recheck(gpointer data)
{
	(void)data;
	speaker.sink_recheck = 0;
	speaker_sink_sync();
	return G_SOURCE_REMOVE;
}

/*
 * The helper returns the state the LED got; if that is not the controller's,
 * look again later, but only a few times in a row (a ThinkPad whose GSMS
 * means something else would never agree).
 */
static gboolean
speaker_led(gpointer data)
{
	int hw, got;

	(void)data;
	speaker.led_settle = 0;
	hw = hw_get_hwmute();
	got = led("speaker", -1);
	if (got >= 0 && hw >= 0 && got != hw) {
		if (++speaker.led_mismatch < LED_MISMATCH_MAX)
			speaker.led_settle = g_timeout_add(LED_SETTLE_MS,
			    speaker_led, NULL);
		else if (speaker.led_mismatch == LED_MISMATCH_MAX)
			syslog(LOG_WARNING, "the speaker LED state from GSMS "
			    "keeps differing from dev.acpi_ibm.0.mute; "
			    "leaving it");
	} else
		speaker.led_mismatch = 0;
	return G_SOURCE_REMOVE;
}

static void
check_speaker(void)
{
	int hw = hw_get_hwmute();
	gint64 now = g_get_monotonic_time();
	gboolean quick;

	if (hw < 0 || hw == speaker.last)
		return;
	speaker_sink_sync();
	if (speaker.sink_recheck == 0)
		speaker.sink_recheck = g_timeout_add(SINK_RECHECK_MS,
		    speaker_sink_recheck, NULL);

	/* part of a quick series: wait (again) until the key is left alone */
	quick = speaker.changed != 0 &&
	    now - speaker.changed < LED_SETTLE_MS * 1000;
	if (speaker.led_settle != 0) {
		g_source_remove(speaker.led_settle);
		speaker.led_settle = 0;
	}
	if (quick)
		speaker.led_settle = g_timeout_add(LED_SETTLE_MS, speaker_led,
		    NULL);
	speaker.changed = now;
	speaker.last = hw;
	if (!quick)
		speaker_led(NULL);		/* a single press: at once */
}

/* ---- microphone mute: the LED and a notification ------------------------ */

static void
check_mic(int first)
{
	char summary[32], icon[64];
	int m = hw_get_micmute();

	if (m < 0 || m == mic.last)
		return;
	led("mic", m);
	if (!first) {
		/* Its state only, no bar */
		snprintf(summary, sizeof(summary), "Microphone %s",
		    m ? "muted" : "on");
		snprintf(icon, sizeof(icon),
		    "microphone-sensitivity-%s-symbolic", m ? "muted" : "high");
		show(&mic.n, summary, icon, -1);
	}
	mic.last = m;
}

/* first: learn the state at start, notifying nothing */
static void
check_all(int first)
{
	check_brightness(first);
	check_speaker();
	check_mic(first);
}

/* ---- when to look: polling, devd, SIGWINCH ------------------------------ */

/*
 * None of the values sends an event of its own, so the watcher polls.  How
 * often:
 *   - FAST_MS for a while after a change or an event: a key held down;
 *   - IDLE_MS when the tpmute kernel module is loaded and devd is connected:
 *     the speaker mute and the mic-mute key then come as devd events, and
 *     "bsdthinkpad brightness", micmute and the panel plugin send SIGWINCH;
 *     the poll is left only for changes made by other programs;
 *   - else -i ms (200).
 */
static guint
next_interval(void)
{
	if (g_get_monotonic_time() < sched.fast_until)
		return FAST_MS;
	if (sched.devd_fd >= 0 && hw_have_tpmute())
		return IDLE_MS;
	return (guint)interval_ms;
}

static void devd_connect(void);

static gboolean
poll_tick(gpointer data)
{
	(void)data;
	sched.poll = 0;
	check_all(0);
	if (sched.devd_fd < 0)
		devd_connect();
	sched.poll = g_timeout_add(next_interval(), poll_tick, NULL);
	return G_SOURCE_REMOVE;
}

/*
 * Something changed, or is about to: look now, and then often for a while.
 * The mic-mute key's devd event comes at the same time as the devd rule that
 * mutes the microphone, so the change may land a moment after the event.
 */
static void
look_now(void)
{
	sched.fast_until = g_get_monotonic_time() + FAST_FOR_US;
	if (sched.poll != 0)
		g_source_remove(sched.poll);
	poll_tick(NULL);
}

/* "bsdthinkpad brightness", micmute or the panel plugin changed something */
static gboolean
on_sigwinch(gpointer data)
{
	(void)data;
	look_now();
	return G_SOURCE_CONTINUE;
}

/* One devd event per read on the seqpacket socket */
static gboolean
on_devd(gint fd, GIOCondition cond, gpointer data)
{
	char buf[1024];
	ssize_t n;

	(void)data;
	if ((cond & G_IO_IN) != 0 &&
	    (n = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
		buf[n] = '\0';
		/* tpmute's speaker mute, the mic-mute key, other hotkeys */
		if (strstr(buf, "system=ACPI") != NULL &&
		    strstr(buf, "subsystem=IBM") != NULL)
			look_now();
		return G_SOURCE_CONTINUE;
	}
	/* devd went away (restarted): poll as before, connect again later */
	close(fd);
	sched.devd_fd = -1;
	return G_SOURCE_REMOVE;
}

/* Connect to devd, at most every DEVD_RETRY_US */
static void
devd_connect(void)
{
	struct sockaddr_un sun;
	gint64 now = g_get_monotonic_time();
	int fd;

	if (sched.devd_tried != 0 && now - sched.devd_tried < DEVD_RETRY_US)
		return;
	sched.devd_tried = now;
	if ((fd = socket(PF_LOCAL, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)) < 0)
		return;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_LOCAL;
	strlcpy(sun.sun_path, DEVD_SOCKET, sizeof(sun.sun_path));
	if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
		close(fd);
		return;
	}
	sched.devd_fd = fd;
	g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_devd, NULL);
}

/* ---- start -------------------------------------------------------------- */

/*
 * ConsoleKit, and so polkit, needs to read our environment through
 * kern.proc.env.  For a few percent of processes the kernel fails to
 * (docs/kern-proc-env-bug.md); then every pkexec is refused.
 */
static void
check_env_readable(void)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ENV, (int)getpid() };
	size_t len;

	/*
	 * Only ask for the size: the kernel still reads the strings (and fails
	 * the same way), and a buffer of ours that is too small can not cause
	 * an ENOMEM of its own.
	 */
	if (sysctl(mib, 4, NULL, &len, NULL, 0) == 0 || errno != ENOMEM)
		return;
	syslog(LOG_WARNING, "the kernel can not read this process's "
	    "environment (kern.proc.env: ENOMEM, an ASLR-dependent FreeBSD bug), so polkit "
	    "may refuse pkexec and the mute LEDs may not follow; "
	    "logging out and in again usually helps");
}

static void
watch(void)
{
	GMainLoop *loop;

	openlog("bsdthinkpad", LOG_PID | LOG_PERROR, LOG_USER);
	if (!notify_init("bsdthinkpad")) {
		syslog(LOG_ERR, "cannot connect to the notification service");
		exit(1);
	}
	if (drive_leds)
		check_env_readable();

	/* Learn the current state (and set the LEDs) without notifying */
	check_all(1);

	g_unix_signal_add(SIGWINCH, on_sigwinch, NULL);
	devd_connect();
	sched.poll = g_timeout_add(next_interval(), poll_tick, NULL);
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);
}

/* ---- bsdthinkpad brightness|pcm [+|-]N ---------------------------------- */

/*
 * "+N" / "-N" change the value by N, "N" sets it.  Prints the value read
 * back, not the one asked for: the hardware rounds (brightness 50 -> 49).
 * The watcher (if running) shows the brightness notification.
 */
static int
set_cmd(const char *what, const char *arg, int (*get)(void), int (*set)(int))
{
	char *end;
	long n;
	int cur, val;

	errno = 0;
	n = strtol(arg, &end, 10);
	if (errno != 0 || *end != '\0' || end == arg)
		errx(2, "%s: expected +N, -N or N, got '%s'", what, arg);

	if ((cur = get()) < 0)
		errx(1, "%s: not available", what);
	val = (arg[0] == '+' || arg[0] == '-') ? cur + (int)n : (int)n;
	if (set(val) != 0)
		err(1, "%s", what);
	hw_nudge_watcher();		/* shows the brightness at once */
	printf("%d\n", get());
	return 0;
}

/* bsdthinkpad micmute [on|off|toggle]: prints the (new) state, "on" = muted */
static int
micmute_cmd(const char *arg)
{
	int cur;

	if ((cur = hw_get_micmute()) < 0)
		errx(1, "micmute: no mixer with a recording level");
	if (arg != NULL) {
		if (strcmp(arg, "on") == 0)
			cur = 1;
		else if (strcmp(arg, "off") == 0)
			cur = 0;
		else if (strcmp(arg, "toggle") == 0)
			cur = !cur;
		else
			errx(2, "micmute: expected on, off or toggle, got '%s'",
			    arg);
		if (hw_set_micmute(cur) != 0)
			errx(1, "micmute: cannot change the mixers");
		/* As root from the devd rule this finds no watcher of
		   root's; the watcher has the key's devd event anyway */
		hw_nudge_watcher();
		cur = hw_get_micmute();
	}
	printf("%s\n", cur ? "on" : "off");
	return 0;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: bsdthinkpad [-n] [-i interval-ms]\n"
	    "       bsdthinkpad brightness [+|-]N\n"
	    "       bsdthinkpad pcm [+|-]N\n"
	    "       bsdthinkpad micmute [on|off|toggle]\n"
	    "  -n  do not drive the mute LEDs\n"
	    "  -i  poll interval in milliseconds (default 200)\n");
	exit(2);
}

int
main(int argc, char *argv[])
{
	const char *errstr;
	int ch;

	if (argc == 3 && strcmp(argv[1], "brightness") == 0)
		return set_cmd("brightness", argv[2], hw_get_brightness,
		    hw_set_brightness);
	if (argc == 3 && strcmp(argv[1], "pcm") == 0)
		return set_cmd("pcm", argv[2], hw_get_pcm, hw_set_pcm);
	if ((argc == 2 || argc == 3) && strcmp(argv[1], "micmute") == 0)
		return micmute_cmd(argc == 3 ? argv[2] : NULL);

	while ((ch = getopt(argc, argv, "ni:")) != -1) {
		switch (ch) {
		case 'n':
			drive_leds = 0;
			break;
		case 'i':
			interval_ms = (int)strtonum(optarg, 50, 60000, &errstr);
			if (errstr != NULL)
				errx(2, "-i %s: %s (50..60000 ms)", optarg,
				    errstr);
			break;
		default:
			usage();
		}
	}
	if (optind != argc)
		usage();

	watch();
	return 0;
}
