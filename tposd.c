/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * tposd -- on-screen display for ThinkPad keys on FreeBSD (XFCE and others)
 *
 * Shows desktop notifications, like xfce4-power-manager and the
 * xfce4-pulseaudio-plugin do on Linux, for things FreeBSD does not report:
 *
 *   brightness       /dev/backlight/backlight0 (backlight(9)), with a bar
 *   speaker mute     the embedded controller mutes the speaker in hardware;
 *                    only the sysctl dev.acpi_ibm.0.mute changes.  tposd also
 *                    turns the mute LED on/off through the ACPI method SSMS
 *                    (acpi_call), which acpi_ibm(4) does not drive
 *   microphone mute  the "mic" channel of the default mixer (mixer(3))
 *   volume           the "vol" channel of the default mixer, with a bar
 *
 * Usage:
 *   tposd [-n] [-i ms] [-a acpi-path]   watch and notify (run from autostart)
 *   tposd brightness [+|-]N             change brightness, e.g. +10, -10, 50
 *   tposd volume [+|-]N                 change volume (percent), e.g. +5, -5
 *
 * Bind the keys (XF86MonBrightnessUp/Down, XF86AudioRaiseVolume/LowerVolume)
 * to the subcommands; the watcher shows the result whoever changed it.
 */

#include <sys/types.h>
#include <sys/backlight.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <mixer.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libnotify/notify.h>

#define BACKLIGHT_DEV   "/dev/backlight/backlight0"
#define MUTE_SYSCTL     "dev.acpi_ibm.0.mute"
#define DEFAULT_SSMS    "\\_SB.PCI0.LPC0.EC0.HKEY.SSMS"
#define TIMEOUT_MS      1500

extern char **environ;

static int         interval_ms = 200;    /* how often to poll */
static int         drive_led = 1;        /* set the mute LED with acpi_call */
static const char *ssms_path = DEFAULT_SSMS;

/* Last seen values; -1 = unknown / not available */
static int last_brightness = -1;
static int last_hwmute = -1;
static int last_micmute = -1;
static int last_volume = -1;

/* One notification per kind, updated in place so new ones replace old ones */
static NotifyNotification *n_brightness, *n_mute, *n_mic, *n_volume;

/* ---- reading and writing the hardware --------------------------------- */

static int
read_brightness(void)
{
	struct backlight_props props;
	int fd, r = -1;

	if ((fd = open(BACKLIGHT_DEV, O_RDONLY)) < 0)
		return -1;
	if (ioctl(fd, BACKLIGHTGETSTATUS, &props) == 0)
		r = (int)props.brightness;
	close(fd);
	return r;
}

static int
write_brightness(int value)
{
	struct backlight_props props;
	int fd, r;

	if (value < 0)
		value = 0;
	if (value > 100)
		value = 100;
	if ((fd = open(BACKLIGHT_DEV, O_RDWR)) < 0)
		return -1;
	r = ioctl(fd, BACKLIGHTGETSTATUS, &props);
	if (r == 0) {
		props.brightness = (uint32_t)value;
		r = ioctl(fd, BACKLIGHTUPDATESTATUS, &props);
	}
	close(fd);
	return r;
}

static int
read_hwmute(void)
{
	int val;
	size_t len = sizeof(val);

	if (sysctlbyname(MUTE_SYSCTL, &val, &len, NULL, 0) != 0)
		return -1;
	return val != 0;
}

static int
read_micmute(void)
{
	struct mixer *m;
	int r;

	/* mixer_open() reads the current state, so open it each time */
	if ((m = mixer_open(NULL)) == NULL)
		return -1;
	r = MIX_ISDEV(m, SOUND_MIXER_MIC) ? MIX_ISMUTE(m, SOUND_MIXER_MIC) : -1;
	mixer_close(m);
	return r;
}

/* Master volume of the default mixer in percent (average of left and right) */
static int
read_volume(void)
{
	struct mixer *m;
	struct mix_dev *d;
	int r = -1;

	if ((m = mixer_open(NULL)) == NULL)
		return -1;
	if ((d = mixer_get_dev(m, SOUND_MIXER_VOLUME)) != NULL)
		r = MIX_VOLDENORM((d->vol.left + d->vol.right) / 2.0f);
	mixer_close(m);
	return r;
}

static int
write_volume(int value)
{
	struct mixer *m;
	mix_volume_t v;
	int r = -1;

	if (value < 0)
		value = 0;
	if (value > 100)
		value = 100;
	if ((m = mixer_open(NULL)) == NULL)
		return -1;
	if (mixer_get_dev(m, SOUND_MIXER_VOLUME) != NULL) {
		v.left = v.right = MIX_VOLNORM(value);
		r = mixer_set_vol(m, v);
	}
	mixer_close(m);
	return r;
}

/*
 * Turn the speaker mute LED on or off.  SSMS(1) also mutes in hardware, so
 * we only ever pass the state the controller already has.  Needs root:
 * run through "sudo -n" (fails quietly when sudo is not set up).
 */
static void
set_mute_led(int on)
{
	char *argv[] = { "sudo", "-n", "/usr/local/sbin/acpi_call",
	    "-p", (char *)ssms_path, "-i", on ? "1" : "0", NULL };
	posix_spawn_file_actions_t fa;
	pid_t pid;
	int status;

	if (!drive_led)
		return;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null",
	    O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null",
	    O_WRONLY, 0);
	if (posix_spawnp(&pid, "sudo", &fa, NULL, argv, environ) == 0)
		waitpid(pid, &status, 0);
	posix_spawn_file_actions_destroy(&fa);
}

/* ---- notifications ------------------------------------------------------ */

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
	} else {
		notify_notification_update(*n, summary, body, icon);
	}
	if (value >= 0)
		notify_notification_set_hint_int32(*n, "value", value);
	if (!notify_notification_show(*n, &error)) {
		warnx("notification failed: %s", error->message);
		g_error_free(error);
	}
}

static const char *
brightness_icon(int v)
{
	if (v < 34)
		return "display-brightness-low";
	if (v < 67)
		return "display-brightness-medium";
	return "display-brightness-high";
}

static const char *
volume_icon(int v)
{
	if (v == 0)
		return "audio-volume-muted";
	if (v < 34)
		return "audio-volume-low";
	if (v < 67)
		return "audio-volume-medium";
	return "audio-volume-high";
}

/* ---- the watch loop ------------------------------------------------------ */

static gboolean
poll_once(gpointer data)
{
	int first = GPOINTER_TO_INT(data);
	int b, hw, mic, vol;
	char text[32];

	b = read_brightness();
	if (b >= 0 && b != last_brightness) {
		if (!first) {
			snprintf(text, sizeof(text), "%d%%", b);
			show(&n_brightness, "Brightness", text,
			    brightness_icon(b), b);
		}
		last_brightness = b;
	}

	hw = read_hwmute();
	if (hw >= 0 && hw != last_hwmute) {
		set_mute_led(hw);
		if (!first)
			show(&n_mute, hw ? "Speaker muted" : "Speaker on",
			    hw ? "Sound is off" : "Sound is on",
			    hw ? "audio-volume-muted" : "audio-volume-high", -1);
		last_hwmute = hw;
	}

	mic = read_micmute();
	if (mic >= 0 && mic != last_micmute) {
		if (!first)
			show(&n_mic,
			    mic ? "Microphone muted" : "Microphone on",
			    mic ? "Microphone is off" : "Microphone is on",
			    mic ? "microphone-sensitivity-muted" :
			    "audio-input-microphone", -1);
		last_micmute = mic;
	}

	vol = read_volume();
	if (vol >= 0 && vol != last_volume) {
		if (!first) {
			snprintf(text, sizeof(text), "%d%%", vol);
			show(&n_volume, "Volume", text, volume_icon(vol), vol);
		}
		last_volume = vol;
	}

	return G_SOURCE_CONTINUE;
}

static void
watch(void)
{
	GMainLoop *loop;

	if (!notify_init("tposd"))
		errx(1, "cannot connect to the notification service");

	/* Learn the current state (and set the LED) without notifying */
	poll_once(GINT_TO_POINTER(1));

	g_timeout_add((guint)interval_ms, poll_once, GINT_TO_POINTER(0));
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);
}

/* ---- tposd brightness|volume [+|-]N ------------------------------------ */

/*
 * "+N" / "-N" change the value by N, "N" sets it.  Prints the new value.
 * The watcher (if running) shows the notification.
 */
static int
set_cmd(const char *what, const char *arg, int (*get)(void),
    int (*set)(int), const char *device)
{
	char *end;
	long n;
	int cur, val;

	errno = 0;
	n = strtol(arg, &end, 10);
	if (errno != 0 || *end != '\0' || end == arg)
		errx(2, "%s: expected +N, -N or N, got '%s'", what, arg);

	if ((cur = get()) < 0)
		err(1, "%s", device);
	val = (arg[0] == '+' || arg[0] == '-') ? cur + (int)n : (int)n;
	if (set(val) != 0)
		err(1, "%s", device);
	printf("%d\n", get());
	return 0;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: tposd [-n] [-i interval-ms] [-a acpi-path]\n"
	    "       tposd brightness [+|-]N\n"
	    "       tposd volume [+|-]N\n"
	    "  -n  do not drive the mute LED (acpi_call)\n"
	    "  -i  poll interval in milliseconds (default 200)\n"
	    "  -a  ACPI method for the mute LED (default %s)\n",
	    DEFAULT_SSMS);
	exit(2);
}

int
main(int argc, char *argv[])
{
	int ch;

	if (argc == 3 && strcmp(argv[1], "brightness") == 0)
		return set_cmd("brightness", argv[2], read_brightness,
		    write_brightness, BACKLIGHT_DEV);
	if (argc == 3 && strcmp(argv[1], "volume") == 0)
		return set_cmd("volume", argv[2], read_volume, write_volume,
		    "mixer");

	while ((ch = getopt(argc, argv, "na:i:")) != -1) {
		switch (ch) {
		case 'n':
			drive_led = 0;
			break;
		case 'a':
			ssms_path = optarg;
			break;
		case 'i':
			interval_ms = atoi(optarg);
			if (interval_ms < 50)
				interval_ms = 50;
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
