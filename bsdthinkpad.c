/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * bsdthinkpad -- on-screen display for ThinkPad keys on FreeBSD (XFCE)
 *
 * Shows desktop notifications for what the XFCE pulseaudio plugin and power
 * manager can not see on FreeBSD:
 *
 *   brightness       /dev/backlight/backlight0 (backlight(9)), with a bar
 *   speaker mute     the embedded controller mutes the speaker in hardware;
 *                    only the sysctl dev.acpi_ibm.0.mute changes, and the
 *                    key sends no event.  bsdthinkpad turns the mute LED
 *                    on/off (bsdthinkpad-mute-led via pkexec), which
 *                    acpi_ibm(4) does not
 *   microphone mute  the "rec" level of every mixer (mixer(3), see hw.h); the
 *                    LED follows it, whoever changed it (key, panel, command)
 *
 * polkit may refuse bsdthinkpad's pkexec, because ConsoleKit can not always
 * read a process's environment (kern.proc.env fails with ENOMEM for a few percent
 * of processes, depending on where ASLR puts the stack; see
 * docs/kern-proc-env-bug.md), and so does not find the session.  bsdthinkpad
 * checks this at start and logs it.
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
#include <sys/sysctl.h>

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <libnotify/notify.h>

#include "hw.h"

#define TIMEOUT_MS	1500
/*
 * After a brightness change, poll fast for a while, so that the notification's
 * bar follows a key held down or the panel icon scrolled step by step instead
 * of jumping several steps at a time.
 */
#define FAST_MS		30
#define FAST_FOR_US	(2 * G_USEC_PER_SEC)

static int interval_ms = 200;		/* how often to poll */
static int drive_leds = 1;		/* set the mute LEDs (via pkexec) */
static int led_warned;			/* reported a helper failure */

/* Last seen values; -1 = unknown / not available */
static int last_brightness = -1;
static int last_hwmute = -1;
static int last_micmute = -1;
static gint64 last_change;		/* g_get_monotonic_time() of the last
					   brightness change */

/* One notification per kind, updated in place so new ones replace old ones */
static NotifyNotification *n_brightness, *n_mute, *n_mic;

/*
 * Set a mute LED.  On failure warn once and keep trying at the next change:
 * the cause may be passing (e.g. polkit or PAM, see README).
 */
static void
led(const char *which, int on)
{
	if (!drive_leds)
		return;
	if (hw_set_led(which, on) == 0)
		led_warned = 0;
	else if (!led_warned) {
		syslog(LOG_WARNING, "pkexec %s %s %d failed; "
		    "the mute LED does not follow (logged once)",
		    MUTE_LED_HELPER, which, on);
		led_warned = 1;
	}
}

/*
 * Show (or replace) a notification.  value >= 0 adds the "value" hint,
 * which notification daemons such as xfce4-notifyd draw as a bar.
 */
static void
show(NotifyNotification **n, const char *summary, const char *body,
    const char *icon, int value)
{
	GError *error = NULL;

	if (*n == NULL) {
		*n = notify_notification_new(summary, body, icon);
		notify_notification_set_timeout(*n, TIMEOUT_MS);
		/* An on-screen display: keep it out of the notification log */
		notify_notification_set_hint(*n, "transient",
		    g_variant_new_boolean(TRUE));
	} else {
		notify_notification_update(*n, summary, body, icon);
	}
	if (value >= 0)
		notify_notification_set_hint_int32(*n, "value", value);
	if (!notify_notification_show(*n, &error)) {
		syslog(LOG_WARNING, "notification failed: %s", error->message);
		g_error_free(error);
	}
}

/* ---- the watch loop ----------------------------------------------------- */

static void
poll_once(int first)
{
	int b, hw, mic;
	char text[32];

	b = hw_get_brightness();
	if (b >= 0 && b != last_brightness) {
		if (!first) {
			snprintf(text, sizeof(text), "%d%%", b);
			/* No theme has per-level brightness icons */
			show(&n_brightness, "Brightness", text,
			    "display-brightness-symbolic", b);
			last_change = g_get_monotonic_time();
		}
		last_brightness = b;
	}

	hw = hw_get_hwmute();
	if (hw >= 0 && hw != last_hwmute) {
		led("speaker", hw);
		if (!first)
			show(&n_mute, hw ? "Speaker muted" : "Speaker on",
			    hw ? "Sound is off" : "Sound is on", hw ?
			    "audio-volume-muted" : "audio-volume-high", -1);
		last_hwmute = hw;
	}

	mic = hw_get_micmute();
	if (mic >= 0 && mic != last_micmute) {
		led("mic", mic);
		if (!first)
			show(&n_mic,
			    mic ? "Microphone muted" : "Microphone on",
			    mic ? "Microphone is off" : "Microphone is on",
			    mic ? "microphone-sensitivity-muted" :
			    "audio-input-microphone", -1);
		last_micmute = mic;
	}
}

/* Poll, then come back soon after a brightness change, else after -i ms */
static gboolean
poll_tick(gpointer data)
{
	guint ms = (guint)interval_ms;

	(void)data;
	poll_once(0);
	if (g_get_monotonic_time() - last_change < FAST_FOR_US && ms > FAST_MS)
		ms = FAST_MS;
	g_timeout_add(ms, poll_tick, NULL);
	return G_SOURCE_REMOVE;
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

	g_timeout_add((guint)interval_ms, poll_tick, NULL);
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
