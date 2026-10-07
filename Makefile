# Makefile for tposd -- works with FreeBSD make and GNU make
#
#   make                   build tposd and tposd-mute-led
#   make install           tposd to ~/.local/bin, plus XFCE autostart
#                          (PREFIX=... to change)
#   make setup             as root, once: packages, kernel modules, the
#                          mute LED helper and its polkit policy, the
#                          mic-mute devd rule (FreeBSD only)
#   make CFLAGS="-O0 -g3"  build for debugging
#   make clean

PROG       = tposd
HELPER     = tposd-mute-led
PREFIX    ?= $(HOME)/.local
LIBEXECDIR = /usr/local/libexec
POLKITDIR  = /usr/local/share/polkit-1/actions
CC        ?= cc
CFLAGS    ?= -O2 -pipe
CFLAGS    += -Wall -Wextra

NOTIFY_CFLAGS != pkg-config --cflags libnotify
NOTIFY_LIBS   != pkg-config --libs libnotify

all: $(PROG) $(HELPER)

$(PROG): tposd.c
	$(CC) $(CFLAGS) -DMUTE_LED_HELPER='"$(LIBEXECDIR)/$(HELPER)"' \
	    $(NOTIFY_CFLAGS) -o $(PROG) tposd.c $(NOTIFY_LIBS) -lmixer

$(HELPER): tposd-mute-led.c
	$(CC) $(CFLAGS) -o $(HELPER) tposd-mute-led.c

install: $(PROG)
	install -d $(PREFIX)/bin $(HOME)/.config/autostart
	install -m 755 $(PROG) $(PREFIX)/bin/$(PROG)
	sed "s#@BINDIR@#$(PREFIX)/bin#" tposd.desktop.in > $(HOME)/.config/autostart/tposd.desktop

clean:
	rm -f $(PROG) $(HELPER)

# System-wide part; run as root (e.g. "su -m root -c 'make setup'")
setup: $(HELPER)
	pkg install -y libnotify acpi_call polkit consolekit2
	sysrc -f /boot/loader.conf acpi_ibm_load=YES acpi_call_load=YES
	kldload -n acpi_ibm
	kldload -n acpi_call
	install -d $(LIBEXECDIR) $(POLKITDIR) /usr/local/etc/devd
	install -o root -g wheel -m 755 $(HELPER) $(LIBEXECDIR)/$(HELPER)
	sed "s#@LIBEXECDIR@#$(LIBEXECDIR)#" org.tposd.mute-led.policy.in > $(POLKITDIR)/org.tposd.mute-led.policy
	chmod 644 $(POLKITDIR)/org.tposd.mute-led.policy
	install -m 755 contrib/thinkpad-micmute $(LIBEXECDIR)/thinkpad-micmute
	install -m 644 contrib/thinkpad-micmute.conf /usr/local/etc/devd/thinkpad-micmute.conf
	service devd restart
	rm -f /usr/local/etc/sudoers.d/tposd

.PHONY: all install clean setup
