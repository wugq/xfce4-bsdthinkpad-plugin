# Makefile for tposd -- works with FreeBSD make and GNU make
#
#   make                   build tposd, tposd-mute-led and the XFCE panel
#                          plugin libtposd-panel.so
#   make install           tposd to ~/.local/bin, plus XFCE autostart
#                          (PREFIX=... to change)
#   make setup             as root, once: packages, kernel modules, the
#                          mute LED helper and its polkit policy, the
#                          mic-mute devd rule, the panel plugin, OSS pcm
#                          at 1.00, the pkexec PAM fix (FreeBSD only)
#   make CFLAGS="-O0 -g3"  build for debugging
#   make clean

PROG       = tposd
HELPER     = tposd-mute-led
PREFIX    ?= $(HOME)/.local
LIBEXECDIR = /usr/local/libexec
POLKITDIR  = /usr/local/share/polkit-1/actions
POLKITPAM  = /usr/local/etc/pam.d/polkit-1
PLUGIN     = libtposd-panel.so
PLUGINDIR  = /usr/local/lib/xfce4/panel/plugins
PLUGINDATA = /usr/local/share/xfce4/panel/plugins
CC        ?= cc
CFLAGS    ?= -O2 -pipe
CFLAGS    += -Wall -Wextra

NOTIFY_CFLAGS != pkg-config --cflags libnotify
NOTIFY_LIBS   != pkg-config --libs libnotify
PANEL_CFLAGS  != pkg-config --cflags libxfce4panel-2.0 gtk+-3.0
PANEL_LIBS    != pkg-config --libs libxfce4panel-2.0 gtk+-3.0
HW_CFLAGS      = -DMUTE_LED_HELPER='"$(LIBEXECDIR)/$(HELPER)"'

all: $(PROG) $(HELPER) $(PLUGIN)

$(PROG): tposd.c hw.c hw.h
	$(CC) $(CFLAGS) $(HW_CFLAGS) $(NOTIFY_CFLAGS) -o $(PROG) \
	    tposd.c hw.c $(NOTIFY_LIBS) -lmixer

$(PLUGIN): tposd-panel.c ctl.c ctl.h hw.c hw.h
	$(CC) $(CFLAGS) -fPIC -shared $(HW_CFLAGS) $(PANEL_CFLAGS) \
	    -o $(PLUGIN) tposd-panel.c ctl.c hw.c $(PANEL_LIBS) -lmixer

$(HELPER): tposd-mute-led.c
	$(CC) $(CFLAGS) -o $(HELPER) tposd-mute-led.c

install: $(PROG)
	install -d $(PREFIX)/bin $(HOME)/.config/autostart
	install -m 755 $(PROG) $(PREFIX)/bin/$(PROG)
	sed "s#@BINDIR@#$(PREFIX)/bin#" tposd.desktop.in > $(HOME)/.config/autostart/tposd.desktop

clean:
	rm -f $(PROG) $(HELPER) $(PLUGIN)

# System-wide part; run as root (e.g. "su -m root -c 'make setup'")
setup: $(HELPER) $(PLUGIN)
	pkg install -y libnotify acpi_call polkit consolekit2 xfce4-panel
	sysrc -f /boot/loader.conf acpi_ibm_load=YES acpi_call_load=YES
	kldload -n acpi_ibm
	kldload -n acpi_call
	install -d $(LIBEXECDIR) $(POLKITDIR) /usr/local/etc/devd
	install -o root -g wheel -m 755 $(HELPER) $(LIBEXECDIR)/$(HELPER)
	sed "s#@LIBEXECDIR@#$(LIBEXECDIR)#" org.tposd.mute-led.policy.in > $(POLKITDIR)/org.tposd.mute-led.policy
	chmod 644 $(POLKITDIR)/org.tposd.mute-led.policy
	@# pkexec aborts when pam_lastlog (session part of "system") prints
	@# "Last login"; give polkit-1 a session stack without it.  Auth and
	@# account stay as they are.  Original kept as polkit-1.orig.
	if grep -q '^session[[:space:]]*include[[:space:]]*system$$' $(POLKITPAM); then \
	    sed -i.orig 's/^session[[:space:]]*include[[:space:]]*system$$/session    required     pam_permit.so/' $(POLKITPAM); \
	fi
	install -m 755 contrib/thinkpad-micmute $(LIBEXECDIR)/thinkpad-micmute
	install -m 644 contrib/thinkpad-micmute.conf /usr/local/etc/devd/thinkpad-micmute.conf
	install -d $(PLUGINDIR) $(PLUGINDATA)
	install -m 755 $(PLUGIN) $(PLUGINDIR)/$(PLUGIN)
	install -m 644 tposd-panel.desktop $(PLUGINDATA)/tposd-panel.desktop
	service devd restart
	mixer pcm=1.00
	for m in /dev/mixer[0-9]*; do \
	    n=$${m#/dev/mixer}; mixer -f $$m -o > /var/db/mixer$$n-state; \
	done
	rm -f /usr/local/etc/sudoers.d/tposd

.PHONY: all install clean setup
