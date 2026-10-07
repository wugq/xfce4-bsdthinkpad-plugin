/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * tposd-mute-led -- turn the ThinkPad speaker mute LED on (1) or off (0)
 *
 * The only part of tposd that needs root.  It is run by tposd through
 * pkexec(1); the polkit policy org.tposd.mute-led allows the user at the
 * console (active local session) to run it without a password, in the same
 * way xfce4-power-manager runs xfpm-power-backlight-helper.
 *
 * It does exactly one thing: call the ACPI method SSMS of the ThinkPad
 * hotkey device with 0 or 1, using acpi_call(8).  The device path is taken
 * from acpi_ibm(4) (sysctl dev.acpi_ibm.0.%location, e.g.
 * "handle=\_SB_.PCI0.LPC0.EC0_.HKEY"), so it works on any ThinkPad whose
 * firmware has SSMS, and it can not be used to call any other method.
 * SSMS(1) also mutes the speaker in hardware, so tposd only passes the
 * state the controller already has.
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

int
main(int argc, char *argv[])
{
	char location[256], method[300];
	char *args[] = { "acpi_call", "-p", method, "-i", NULL, NULL };
	char *env[] = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };
	const char *handle;
	size_t len = sizeof(location) - 1;

	if (argc != 2 ||
	    (strcmp(argv[1], "0") != 0 && strcmp(argv[1], "1") != 0))
		errx(2, "usage: tposd-mute-led 0|1");

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
	args[4] = argv[1];
	execve(ACPI_CALL, args, env);
	err(1, "%s", ACPI_CALL);
}
