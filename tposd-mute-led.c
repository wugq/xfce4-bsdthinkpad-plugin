/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * tposd-mute-led -- turn a ThinkPad mute LED on (1) or off (0)
 *
 *   tposd-mute-led speaker 0|1   speaker mute LED: ACPI method SSMS
 *   tposd-mute-led mic 0|1       microphone mute LED: sysctl dev.acpi_ibm.0.mic_led
 *
 * The only part of tposd that needs root.  It is run by tposd and
 * tposd-panel through pkexec(1); the polkit policy org.tposd.mute-led allows
 * the user at the console (active local session) to run it without a
 * password, in the same way xfce4-power-manager runs
 * xfpm-power-backlight-helper.
 *
 * It accepts nothing but these two forms.  For the speaker LED it calls only
 * the method SSMS of the ThinkPad hotkey device, whose path is taken from
 * acpi_ibm(4) (sysctl dev.acpi_ibm.0.%location, e.g.
 * "handle=\_SB_.PCI0.LPC0.EC0_.HKEY"), using acpi_call(8).  SSMS(1) also
 * mutes the speaker in hardware, so callers only pass the state the
 * controller already has.
 */

#include <sys/types.h>
#include <sys/sysctl.h>

#include <err.h>
#include <stdio.h>
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

static void
speaker_led(int on)
{
	char location[256], method[300];
	char *args[] = { "acpi_call", "-p", method, "-i", NULL, NULL };
	char *env[] = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };
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

	snprintf(method, sizeof(method), "%s.SSMS", handle);
	args[4] = on ? "1" : "0";
	/* Running as root for the caller: a fixed path and environment only */
	execve(ACPI_CALL, args, env);
	err(1, "%s", ACPI_CALL);
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

	if (argc != 3 ||
	    (strcmp(argv[2], "0") != 0 && strcmp(argv[2], "1") != 0))
		errx(2, "usage: tposd-mute-led speaker|mic 0|1");
	on = (argv[2][0] == '1');

	if (strcmp(argv[1], "speaker") == 0)
		speaker_led(on);		/* does not return */
	else if (strcmp(argv[1], "mic") == 0)
		mic_led(on);
	else
		errx(2, "usage: tposd-mute-led speaker|mic 0|1");
	return 0;
}
