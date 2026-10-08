/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * hw.c -- read and change brightness, mixer channels and mute LEDs (FreeBSD)
 */

#include <sys/types.h>
#include <sys/backlight.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <limits.h>
#include <mixer.h>
#include <spawn.h>
#include <unistd.h>

#include "hw.h"

extern char **environ;

static int
clamp(int v)
{
	return v < 0 ? 0 : (v > 100 ? 100 : v);
}

/* ---- brightness: backlight(9) ------------------------------------------- */

int
hw_get_brightness(void)
{
	struct backlight_props props;
	int fd, r = -1;

	if ((fd = open(HW_BACKLIGHT_DEV, O_RDONLY)) < 0)
		return -1;
	if (ioctl(fd, BACKLIGHTGETSTATUS, &props) == 0)
		r = (int)props.brightness;
	close(fd);
	return r;
}

int
hw_set_brightness(int value)
{
	struct backlight_props props;
	int fd, r;

	if ((fd = open(HW_BACKLIGHT_DEV, O_RDWR)) < 0)
		return -1;
	r = ioctl(fd, BACKLIGHTGETSTATUS, &props);
	if (r == 0) {
		props.brightness = (uint32_t)clamp(value);
		r = ioctl(fd, BACKLIGHTUPDATESTATUS, &props);
	}
	close(fd);
	return r;
}

/* ---- speaker mute by the embedded controller: acpi_ibm(4) --------------- */

int
hw_get_hwmute(void)
{
	int val;
	size_t len = sizeof(val);

	if (sysctlbyname("dev.acpi_ibm.0.mute", &val, &len, NULL, 0) != 0)
		return -1;
	return val != 0;
}

/* ---- mixer channels: mixer(3) ------------------------------------------- */
/*
 * mixer_open() reads the current state, so open the mixer for every call;
 * cheap enough for a few calls per second.
 */

/*
 * mixer_get_dev() compares the device number with the number of devices the
 * mixer has (m->ndev), so it fails for higher numbers, e.g. "rec" (11) on a
 * mixer with 8 devices.  Look the device up ourselves.  mixer_set_vol() and
 * mixer_set_mute() work on m->dev, which this sets.
 */
static struct mix_dev *
select_dev(struct mixer *m, int devno)
{
	struct mix_dev *d;

	TAILQ_FOREACH(d, &m->devs, devs)
		if (d->devno == devno)
			return m->dev = d;
	return m->dev = NULL;
}

static int
get_volume(int devno)
{
	struct mixer *m;
	struct mix_dev *d;
	int r = -1;

	if ((m = mixer_open(NULL)) == NULL)
		return -1;
	if ((d = select_dev(m, devno)) != NULL)
		r = MIX_VOLDENORM((d->vol.left + d->vol.right) / 2.0f);
	mixer_close(m);
	return r;
}

static int
set_volume(int devno, int value)
{
	struct mixer *m;
	mix_volume_t v;
	int r = -1;

	if ((m = mixer_open(NULL)) == NULL)
		return -1;
	if (select_dev(m, devno) != NULL) {
		v.left = v.right = MIX_VOLNORM(clamp(value));
		r = mixer_set_vol(m, v);
	}
	mixer_close(m);
	return r;
}

int
hw_get_pcm(void)
{
	return get_volume(SOUND_MIXER_PCM);
}

int
hw_set_pcm(int value)
{
	return set_volume(SOUND_MIXER_PCM, value);
}

int
hw_get_volume(void)
{
	return get_volume(SOUND_MIXER_VOLUME);
}

/*
 * Microphone mute: the "rec" channel of every mixer, as "mixer -a" walks
 * them.  set < 0 only reads: returns 1 if all are muted, 0 if not, -1 if no
 * mixer has a recording level.  set 0/1 changes all of them: returns 0, or
 * -1 if none could be changed.
 */
static int
rec_mute(int set)
{
	char path[NAME_MAX];
	struct mixer *m;
	int i, n, found = 0, muted = 0, failed = 0;

	if ((n = mixer_get_nmixers()) < 0)
		return -1;
	for (i = 0; i < n; i++) {
		(void)mixer_get_path(path, sizeof(path), i);
		if ((m = mixer_open(path)) == NULL)
			continue;
		if (MIX_ISDEV(m, SOUND_MIXER_RECLEV)) {
			found++;
			if (set < 0)
				muted += MIX_ISMUTE(m, SOUND_MIXER_RECLEV) != 0;
			else if (select_dev(m, SOUND_MIXER_RECLEV) == NULL ||
			    mixer_set_mute(m, set ? MIX_MUTE : MIX_UNMUTE) != 0)
				failed++;
		}
		mixer_close(m);
	}
	if (found == 0)
		return -1;
	if (set < 0)
		return muted == found;
	return failed == found ? -1 : 0;
}

int
hw_get_micmute(void)
{
	return rec_mute(-1);
}

int
hw_set_micmute(int on)
{
	return rec_mute(on != 0);
}

/* ---- mute LEDs: root helper via pkexec ---------------------------------- */

int
hw_set_led(const char *which, int on)
{
	char *argv[] = { "pkexec", MUTE_LED_HELPER, (char *)which,
	    on ? "1" : "0", NULL };
	posix_spawn_file_actions_t fa;
	pid_t pid;
	int status, r = -1;

	if (access(MUTE_LED_HELPER, X_OK) != 0)
		return -1;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null",
	    O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null",
	    O_WRONLY, 0);
	if (posix_spawnp(&pid, "pkexec", &fa, NULL, argv, environ) == 0 &&
	    waitpid(pid, &status, 0) == pid &&
	    WIFEXITED(status) && WEXITSTATUS(status) == 0)
		r = 0;
	posix_spawn_file_actions_destroy(&fa);
	return r;
}
