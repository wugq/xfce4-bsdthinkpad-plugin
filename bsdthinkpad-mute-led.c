/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * bsdthinkpad-mute-led -- turn a ThinkPad mute LED on (1) or off (0)
 *
 *   bsdthinkpad-mute-led speaker 0|1   speaker mute LED: ACPI method SSMS
 *   bsdthinkpad-mute-led speaker sync  speaker mute LED to the mute the
 *                                      controller has: GSMS, then SSMS;
 *                                      prints the state it set (0 or 1)
 *   bsdthinkpad-mute-led mic 0|1       microphone mute LED: sysctl
 *                                      dev.acpi_ibm.0.mic_led
 *
 * The only part of xfce4-bsdthinkpad-plugin that needs root.  It is run by
 * bsdthinkpad and the panel plugin through pkexec(1); the polkit policy
 * org.bsdthinkpad.mute-led allows the user at the console (active local
 * session) to run it without a password, in the same way
 * xfce4-power-manager runs xfpm-power-backlight-helper.
 *
 * It accepts nothing but these two forms.  For the speaker LED it calls only
 * the method SSMS of the ThinkPad hotkey device, whose path is taken from
 * acpi_ibm(4) (sysctl dev.acpi_ibm.0.%location, e.g.
 * "handle=\_SB_.PCI0.LPC0.EC0_.HKEY"), using acpi_call(8): SSMS to set the
 * mute and its LED (the panel's speaker switch), and GSMS to read the mute
 * right before it (the watcher, after the mute key).
 */

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef ACPI_CALL
#define ACPI_CALL	"/usr/local/sbin/acpi_call"
#endif

/* Characters allowed in an ACPI path such as \_SB_.PCI0.LPC0.EC0_.HKEY */
static int
valid_acpi_path(const char *p)
{
	if (p[0] != '\\')
		return 0;
	for (p++; *p != '\0'; p++)
		if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
		    *p == '_' || *p == '.'))
			return 0;
	return 1;
}

/* "<hotkey device>.<name>", e.g. \_SB_.PCI0.LPC0.EC0_.HKEY.SSMS */
static void
hkey_method(char *method, size_t size, const char *name)
{
	char location[256];
	const char *handle;
	size_t len = sizeof(location) - 1;

	if (sysctlbyname("dev.acpi_ibm.0.%location", location, &len,
	    NULL, 0) != 0)
		err(1, "dev.acpi_ibm.0.%%location (is acpi_ibm loaded?)");
	location[len] = '\0';
	if ((handle = strstr(location, "handle=")) == NULL)
		errx(1, "no ACPI handle in '%s'", location);
	handle += strlen("handle=");
	if (!valid_acpi_path(handle))
		errx(1, "unexpected ACPI handle '%s'", handle);
	snprintf(method, size, "%s.%s", handle, name);
}

/* SSMS: set the speaker mute, and its LED with it */
static void
speaker_led(int on)
{
	char method[300];
	char *args[] = { "acpi_call", "-p", method, "-i", NULL, NULL };
	char *env[] = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };

	hkey_method(method, sizeof(method), "SSMS");
	args[4] = on ? "1" : "0";
	/* Running as root for the caller: a fixed path and environment only */
	execve(ACPI_CALL, args, env);
	err(1, "%s", ACPI_CALL);
}

/*
 * The LED to the mute the controller has now.  SSMS sets the mute too, so
 * the state must be read right before it: one the caller read before pkexec
 * (which takes a while) could undo a mute key pressed meanwhile.  GSMS reads
 * it from the firmware (AUDC(0, 0), mute in bit 0); on the A475 it also sets
 * the LED, on the X1 Carbon only reads, so SSMS follows at once.  Prints the
 * state the LED was set to, for the caller to compare with the mute it sees
 * next.
 */
static void
speaker_sync(void)
{
	char method[300], out[64];
	char *args[] = { "acpi_call", "-p", method, "-i", "0", NULL };
	char *env[] = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };
	int fd[2], null, status, on;
	ssize_t n;
	pid_t pid;

	hkey_method(method, sizeof(method), "GSMS");
	if (pipe(fd) != 0)
		err(1, "pipe");
	if ((pid = fork()) < 0)
		err(1, "fork");
	if (pid == 0) {
		dup2(fd[1], STDOUT_FILENO);
		close(fd[0]);
		close(fd[1]);
		execve(ACPI_CALL, args, env);
		_exit(127);
	}
	close(fd[1]);
	n = read(fd[0], out, sizeof(out) - 1);
	close(fd[0]);
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0 || n <= 0)
		errx(1, "%s %s failed", ACPI_CALL, method);
	out[n] = '\0';
	on = (int)(strtol(out, NULL, 0) & 1);
	printf("%d\n", on);
	fflush(stdout);
	/* SSMS's own output would follow ours on the caller's pipe */
	if ((null = open("/dev/null", O_WRONLY)) >= 0)
		dup2(null, STDOUT_FILENO);
	speaker_led(on);			/* does not return */
}

static void
mic_led(int on)
{
	if (sysctlbyname("dev.acpi_ibm.0.mic_led", NULL, NULL, &on,
	    sizeof(on)) != 0)
		err(1, "dev.acpi_ibm.0.mic_led");
}

int
main(int argc, char *argv[])
{
	int on;

	if (argc == 3 && strcmp(argv[1], "speaker") == 0 &&
	    strcmp(argv[2], "sync") == 0) {
		speaker_sync();			/* does not return */
	}
	if (argc != 3 ||
	    (strcmp(argv[2], "0") != 0 && strcmp(argv[2], "1") != 0))
		errx(2, "usage: bsdthinkpad-mute-led speaker|mic 0|1, "
		    "bsdthinkpad-mute-led speaker sync");
	on = (argv[2][0] == '1');

	if (strcmp(argv[1], "speaker") == 0)
		speaker_led(on);		/* does not return */
	else if (strcmp(argv[1], "mic") == 0)
		mic_led(on);
	else
		errx(2, "usage: bsdthinkpad-mute-led speaker|mic 0|1, "
		    "bsdthinkpad-mute-led speaker sync");
	return 0;
}
