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
 *                               mixer (pulseaudio does not manage it)
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

#define TIMEOUT_MS	2000		/* as the pulseaudio plugin's */
/*
 * After a brightness change, poll fast for a while, so that the notification's
 * bar follows a key held down or the panel icon scrolled step by step instead
 * of jumping several steps at a time.
 */
#define FAST_MS		30
#define FAST_FOR_US	(2 * G_USEC_PER_SEC)
/*
 * With tpmute and devd: the poll is only a safety net for changes nobody
 * tells us about (another program's mixer or backlight change)
 */
#define IDLE_MS		2000
#define DEVD_SOCKET	"/var/run/devd.seqpacket.pipe"
#define DEVD_RETRY_US	(10 * G_USEC_PER_SEC)
/*
 * How long after the speaker mute changed the sink is checked once more:
 * where the mute key reaches the desktop the pulseaudio plugin toggles the
 * sink itself, usually within a few ms of the embedded controller (so before
 * we see the change), but not always
 */
#define MUTE_WAIT_MS	150

static int interval_ms = 200;		/* how often to poll */
static int drive_leds = 1;		/* set the mute LEDs (via pkexec) */
static int led_warned;			/* reported a helper failure */
static int sink_warned;			/* reported a pactl failure */

/* Last seen values; -1 = unknown / not available */
static int last_brightness = -1;
static int last_hwmute = -1;
static int last_micmute = -1;
static int sync_pending;		/* a sink_sync_later() is due */
static gint64 last_change;		/* g_get_monotonic_time() of the last
					   brightness change or event */
static guint poll_id;			/* the pending poll_tick() */
static int devd_fd = -1;		/* connection to devd, or -1 */
static gint64 devd_tried;		/* last connection attempt */

/* One notification per kind, updated in place so new ones replace old ones */
static NotifyNotification *n_brightness, *n_mic;

/*
 * Set a mute LED.  On failure warn once and keep trying at the next change:
 * the cause may be passing (e.g. polkit or PAM, see README).
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

/*
 * The microphone mute changed: its state only, no bar.
 */
static void
show_mute(NotifyNotification **n, const char *what, int muted,
    const char *icon_base)
{
	char summary[32], icon[64];

	snprintf(summary, sizeof(summary), "%s %s", what,
	    muted ? "muted" : "on");
	snprintf(icon, sizeof(icon), "%s-%s-symbolic", icon_base,
	    muted ? "muted" : "high");
	show(n, summary, icon, -1);
}

/*
 * Put the PulseAudio sink mute in step with the embedded controller's.  The
 * two are independent: the controller mutes the speaker in hardware, the
 * sink is what the pulseaudio plugin shows.  Where the mute key reaches the
 * desktop (the X1 Carbon) the plugin toggles the sink with the controller,
 * but the volume keys unmute only the controller, and a quick double press
 * can leave the two apart; where it does not (the A475) the sink never
 * follows on its own.  Setting it shows the speaker mute through the plugin
 * on every ThinkPad, so bsdthinkpad has no speaker popup of its own.
 */
static void
sink_sync(void)
{
	int hw = hw_get_hwmute(), sink = sink_muted();

	if (hw >= 0 && sink >= 0 && sink != hw)
		set_sink_mute(hw);
}

/* The second look, MUTE_WAIT_MS after the change */
static gboolean
sink_sync_later(gpointer data)
{
	(void)data;
	sync_pending = 0;
	sink_sync();
	return G_SOURCE_REMOVE;
}

/* ---- the watch loop ----------------------------------------------------- */

static void
poll_once(int first)
{
	int b, hw, mic, led_hw;
	char text[32];

	b = hw_get_brightness();
	if (b >= 0 && b != last_brightness) {
		if (!first) {
			/* As the power manager words it */
			snprintf(text, sizeof(text), "Brightness: %d%%", b);
			/* No theme has per-level brightness icons */
			show(&n_brightness, text, "display-brightness-symbolic",
			    b);
			last_change = g_get_monotonic_time();
		}
		last_brightness = b;
	}

	hw = hw_get_hwmute();
	if (hw >= 0 && hw != last_hwmute) {
		/*
		 * "sync" (GSMS, then SSMS), not hw: SSMS sets the mute too, and
		 * pkexec takes long enough for a quick second press of the
		 * key to land meanwhile; hw would then undo it.  Remember the
		 * state the LED got rather than hw: if the key was pressed
		 * twice meanwhile, they differ, and the next poll syncs again.
		 */
		led_hw = led("speaker", -1);
		/* the sink now, and again in case the plugin was late */
		sink_sync();
		if (!sync_pending) {
			sync_pending = 1;
			g_timeout_add(MUTE_WAIT_MS, sink_sync_later, NULL);
		}
		last_hwmute = led_hw >= 0 ? led_hw : hw;
	}

	mic = hw_get_micmute();
	if (mic >= 0 && mic != last_micmute) {
		led("mic", mic);
		if (!first)
			show_mute(&n_mic, "Microphone", mic,
			    "microphone-sensitivity");
		last_micmute = mic;
	}
}

static void devd_connect(void);

/*
 * Poll, then come back: soon after a brightness change or an event (a key
 * held down, the bar following step by step); else after -i ms, or after
 * IDLE_MS when tpmute and devd tell us about the speaker mute.
 */
static gboolean
poll_tick(gpointer data)
{
	guint ms = (guint)interval_ms;

	(void)data;
	poll_id = 0;
	poll_once(0);
	if (devd_fd < 0)
		devd_connect();
	if (devd_fd >= 0 && hw_have_tpmute())
		ms = IDLE_MS;
	if (g_get_monotonic_time() - last_change < FAST_FOR_US && ms > FAST_MS)
		ms = FAST_MS;
	poll_id = g_timeout_add(ms, poll_tick, NULL);
	return G_SOURCE_REMOVE;
}

/*
 * Something changed, or is about to: look now, and then often for a while.
 * The mic-mute key's devd event comes at the same time as the devd rule that
 * mutes the microphone, so the change may land a moment after the event.
 */
static void
kick(void)
{
	last_change = g_get_monotonic_time();
	if (poll_id != 0)
		g_source_remove(poll_id);
	poll_tick(NULL);
}

/* "bsdthinkpad brightness", micmute or the panel plugin changed something */
static gboolean
on_nudge(gpointer data)
{
	(void)data;
	kick();
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
			kick();
		return G_SOURCE_CONTINUE;
	}
	/* devd went away (restarted): poll as before, connect again later */
	close(fd);
	devd_fd = -1;
	return G_SOURCE_REMOVE;
}

/*
 * Connect to devd, at most every DEVD_RETRY_US; without it (or without
 * tpmute) the watcher just polls every -i ms as always.
 */
static void
devd_connect(void)
{
	struct sockaddr_un sun;
	gint64 now = g_get_monotonic_time();
	int fd;

	if (devd_tried != 0 && now - devd_tried < DEVD_RETRY_US)
		return;
	devd_tried = now;
	if ((fd = socket(PF_LOCAL, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)) < 0)
		return;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_LOCAL;
	strlcpy(sun.sun_path, DEVD_SOCKET, sizeof(sun.sun_path));
	if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
		close(fd);
		return;
	}
	devd_fd = fd;
	g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_devd, NULL);
}

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
	poll_once(1);

	g_unix_signal_add(SIGWINCH, on_nudge, NULL);
	devd_connect();
	poll_id = g_timeout_add((guint)interval_ms, poll_tick, NULL);
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
