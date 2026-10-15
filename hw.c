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
#include <sys/user.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <limits.h>
#include <mixer.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

	/*
	 * tpmute already polls the EC: take its copy, so that the EC is not
	 * read twice as often (each read is an EC transaction, and a busy EC
	 * drops its own events: "EcCommand: no response").
	 */
	if (sysctlbyname("hw.tpmute.state", &val, &len, NULL, 0) == 0 &&
	    val >= 0)
		return val != 0;
	len = sizeof(val);
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

/*
 * The number of mixers, for hw_get_micmute().  mixer_get_nmixers() reads the
 * sound devices' list (~30 us, and mixer_get_path() calls it again for every
 * mixer), so it is kept, and asked again every MIXERS_TTL seconds (a USB
 * sound device plugged in) or when a mixer fails to open (one taken out).
 * libmixer's units are 0..n-1, its paths "/dev/mixerN".
 */
#define MIXERS_TTL	10

static int	nmixers = -1;
static time_t	nmixers_time;

static void
count_mixers(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	if (nmixers >= 0 && now.tv_sec - nmixers_time < MIXERS_TTL)
		return;
	nmixers_time = now.tv_sec;
	nmixers = mixer_get_nmixers();
}

/*
 * Polled several times a second, so not through rec_mute(): mixer_open()
 * reads every channel of the mixer.  Two ioctls per mixer are enough to
 * read the mute; with the count kept, ~30 us for six mixers on an A475,
 * against ~600 us before.
 */
int
hw_get_micmute(void)
{
	char path[32];
	int i, n, fd, devmask, mutemask, found = 0, muted = 0;

	count_mixers();
	for (i = 0, n = nmixers; i < n; i++) {
		snprintf(path, sizeof(path), "/dev/mixer%d", i);
		if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
			nmixers = -1;	/* gone: count again next time */
			continue;
		}
		if (ioctl(fd, SOUND_MIXER_READ_DEVMASK, &devmask) == 0 &&
		    (devmask & SOUND_MASK_RECLEV) != 0) {
			found++;
			if (ioctl(fd, SOUND_MIXER_READ_MUTE, &mutemask) == 0 &&
			    (mutemask & SOUND_MASK_RECLEV) != 0)
				muted++;
		}
		close(fd);
	}
	if (found == 0)
		return -1;
	return muted == found;
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
	    on < 0 ? "sync" : on ? "1" : "0", NULL };
	posix_spawn_file_actions_t fa;
	char out[16];
	ssize_t n = 0;
	pid_t pid;
	int fd[2] = { -1, -1 }, status, r = -1;

	if (access(MUTE_LED_HELPER, X_OK) != 0)
		return -1;
	/* "sync" prints the state it set the LED to */
	if (on < 0 && pipe2(fd, O_CLOEXEC) != 0)
		return -1;
	posix_spawn_file_actions_init(&fa);
	if (on < 0)
		posix_spawn_file_actions_adddup2(&fa, fd[1], STDOUT_FILENO);
	else
		posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO,
		    "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null",
	    O_WRONLY, 0);
	if (posix_spawnp(&pid, "pkexec", &fa, NULL, argv, environ) == 0) {
		if (on < 0) {
			close(fd[1]);
			fd[1] = -1;
			n = read(fd[0], out, sizeof(out) - 1);
		}
		if (waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
		    WEXITSTATUS(status) == 0)
			r = 0;
	}
	posix_spawn_file_actions_destroy(&fa);
	if (on < 0) {
		if (fd[1] >= 0)
			close(fd[1]);
		close(fd[0]);
		if (r == 0)
			r = (n > 0 && out[0] == '1') ? 1 :
			    (n > 0 && out[0] == '0') ? 0 : -1;
	}
	return r;
}

/* ---- the watcher -------------------------------------------------------- */

/*
 * SIGWINCH, not SIGUSR1: its default action is to ignore it, so a watcher
 * from before this (still running after an upgrade) is not killed by it.
 */
void
hw_nudge_watcher(void)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_UID, (int)getuid() };
	struct kinfo_proc *kp;
	size_t len, i;

	if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0 || len == 0)
		return;
	len += len / 4;			/* processes may start meanwhile */
	if ((kp = malloc(len)) == NULL)
		return;
	if (sysctl(mib, 4, kp, &len, NULL, 0) == 0)
		for (i = 0; i < len / sizeof(*kp); i++)
			if (kp[i].ki_pid != getpid() &&
			    strcmp(kp[i].ki_comm, "bsdthinkpad") == 0)
				(void)kill(kp[i].ki_pid, SIGWINCH);
	free(kp);
}

int
hw_have_tpmute(void)
{
	size_t len = 0;

	return sysctlbyname("hw.tpmute.state", NULL, &len, NULL, 0) == 0;
}

int
hw_tpmute_led(void)
{
	int v;
	size_t len = sizeof(v);

	return sysctlbyname("hw.tpmute.led", &v, &len, NULL, 0) == 0 &&
	    v == 1;
}
