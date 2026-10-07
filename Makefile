# Makefile for tposd -- works with FreeBSD make and GNU make
#
#   make                   build tposd, tposd-mute-led and the XFCE panel
#                          plugin libtposd-panel.so
#   make install           install under PREFIX (default /usr/local); with
#                          DESTDIR for staging (ports, packages)
#   make setup             as root, without the port: install the
#                          dependencies, "make install", then tposd-setup
#                          (kernel modules, pkexec PAM fix, devd, OSS pcm)
#   make dist              source tarball for the port (needs git)
#   make CFLAGS="-O0 -g3"  build for debugging
#   make clean

VERSION    = 0.1.1

PROG       = tposd
HELPER     = tposd-mute-led
PLUGIN     = libtposd-panel.so

PREFIX    ?= /usr/local
LOCALBASE ?= /usr/local
DESTDIR   ?=
BINDIR     = $(PREFIX)/bin
SBINDIR    = $(PREFIX)/sbin
LIBEXECDIR = $(PREFIX)/libexec
POLKITDIR  = $(PREFIX)/share/polkit-1/actions
PLUGINDIR  = $(PREFIX)/lib/xfce4/panel/plugins
PLUGINDATA = $(PREFIX)/share/xfce4/panel/plugins
AUTOSTART  = $(PREFIX)/etc/xdg/autostart
DEVDDIR    = $(PREFIX)/etc/devd
MANDIR     = $(PREFIX)/share/man

CC        ?= cc
CFLAGS    ?= -O2 -pipe
CFLAGS    += -Wall -Wextra

NOTIFY_CFLAGS != pkg-config --cflags libnotify
NOTIFY_LIBS   != pkg-config --libs libnotify
PANEL_CFLAGS  != pkg-config --cflags libxfce4panel-2.0 gtk+-3.0
PANEL_LIBS    != pkg-config --libs libxfce4panel-2.0 gtk+-3.0
HW_CFLAGS      = -DMUTE_LED_HELPER='"$(LIBEXECDIR)/$(HELPER)"'

# Files made from templates: @PREFIX@, @BINDIR@ and @LIBEXECDIR@ filled in
GENERATED  = tposd.desktop org.tposd.mute-led.policy \
	     contrib/tposd.conf contrib/tposd-key contrib/tposd-setup \
	     man/tposd.1 man/tposd-mute-led.8 man/tposd-setup.8
SUBST      = sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@BINDIR@|$(BINDIR)|g' \
		 -e 's|@LIBEXECDIR@|$(LIBEXECDIR)|g'

all: $(PROG) $(HELPER) $(PLUGIN) $(GENERATED)

$(PROG): tposd.c hw.c hw.h
	$(CC) $(CFLAGS) $(HW_CFLAGS) $(NOTIFY_CFLAGS) -o $(PROG) \
	    tposd.c hw.c $(NOTIFY_LIBS) -lmixer

$(PLUGIN): tposd-panel.c ctl.c ctl.h hw.c hw.h
	$(CC) $(CFLAGS) -fPIC -shared $(HW_CFLAGS) $(PANEL_CFLAGS) \
	    -o $(PLUGIN) tposd-panel.c ctl.c hw.c $(PANEL_LIBS) -lmixer

$(HELPER): tposd-mute-led.c
	$(CC) $(CFLAGS) -DACPI_CALL='"$(LOCALBASE)/sbin/acpi_call"' \
	    -o $(HELPER) tposd-mute-led.c

tposd.desktop: tposd.desktop.in
	$(SUBST) tposd.desktop.in > $@
org.tposd.mute-led.policy: org.tposd.mute-led.policy.in
	$(SUBST) org.tposd.mute-led.policy.in > $@
contrib/tposd.conf: contrib/tposd.conf.in
	$(SUBST) contrib/tposd.conf.in > $@
contrib/tposd-key: contrib/tposd-key.in
	$(SUBST) contrib/tposd-key.in > $@
contrib/tposd-setup: contrib/tposd-setup.in
	$(SUBST) contrib/tposd-setup.in > $@
man/tposd.1: man/tposd.1.in
	$(SUBST) man/tposd.1.in > $@
man/tposd-mute-led.8: man/tposd-mute-led.8.in
	$(SUBST) man/tposd-mute-led.8.in > $@
man/tposd-setup.8: man/tposd-setup.8.in
	$(SUBST) man/tposd-setup.8.in > $@

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(SBINDIR) \
	    $(DESTDIR)$(LIBEXECDIR) $(DESTDIR)$(POLKITDIR) \
	    $(DESTDIR)$(PLUGINDIR) $(DESTDIR)$(PLUGINDATA) \
	    $(DESTDIR)$(AUTOSTART) $(DESTDIR)$(DEVDDIR) \
	    $(DESTDIR)$(MANDIR)/man1 $(DESTDIR)$(MANDIR)/man8
	install -m 755 $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)
	install -m 755 contrib/tposd-setup $(DESTDIR)$(SBINDIR)/tposd-setup
	install -m 755 $(HELPER) $(DESTDIR)$(LIBEXECDIR)/$(HELPER)
	install -m 755 contrib/tposd-key $(DESTDIR)$(LIBEXECDIR)/tposd-key
	install -m 644 org.tposd.mute-led.policy \
	    $(DESTDIR)$(POLKITDIR)/org.tposd.mute-led.policy
	install -m 755 $(PLUGIN) $(DESTDIR)$(PLUGINDIR)/$(PLUGIN)
	install -m 644 tposd-panel.desktop \
	    $(DESTDIR)$(PLUGINDATA)/tposd-panel.desktop
	install -m 644 tposd.desktop $(DESTDIR)$(AUTOSTART)/tposd.desktop
	install -m 644 contrib/tposd.conf $(DESTDIR)$(DEVDDIR)/tposd.conf
	install -m 644 man/tposd.1 $(DESTDIR)$(MANDIR)/man1/tposd.1
	install -m 644 man/tposd-setup.8 man/tposd-mute-led.8 \
	    $(DESTDIR)$(MANDIR)/man8

# Without the port; as root (e.g. "su -m root -c 'make setup'")
setup: install
	pkg install -y libnotify acpi_call polkit consolekit2 xfce4-panel
	$(SBINDIR)/tposd-setup

dist:
	git archive --prefix=$(PROG)-$(VERSION)/ -o $(PROG)-$(VERSION).tar.gz HEAD

clean:
	rm -f $(PROG) $(HELPER) $(PLUGIN) $(GENERATED)

.PHONY: all install setup dist clean
