# Makefile for xfce4-bsdthinkpad-plugin -- works with FreeBSD make and GNU make
#
#   make                   build bsdthinkpad, bsdthinkpad-mute-led and the
#                          XFCE panel plugin libbsdthinkpad-plugin.so
#   make install           install under PREFIX (default /usr/local); with
#                          DESTDIR for staging (ports, packages)
#   make setup             as root, without the port: install the
#                          dependencies, "make install", then bsdthinkpad-setup
#                          (kernel modules, pkexec PAM fix, devd, OSS pcm)
#   make dist              source tarball of HEAD (needs git)
#   make CFLAGS="-O0 -g3"  build for debugging
#   make clean

PACKAGE    = xfce4-bsdthinkpad-plugin
VERSION    = 0.2.1

PROG       = bsdthinkpad
HELPER     = bsdthinkpad-mute-led
PLUGIN     = libbsdthinkpad-plugin.so

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
GENERATED  = bsdthinkpad.desktop org.bsdthinkpad.mute-led.policy \
	     contrib/bsdthinkpad.conf contrib/bsdthinkpad-key \
	     contrib/bsdthinkpad-setup man/bsdthinkpad.1 \
	     man/bsdthinkpad-mute-led.8 man/bsdthinkpad-setup.8
SUBST      = sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@BINDIR@|$(BINDIR)|g' \
		 -e 's|@LIBEXECDIR@|$(LIBEXECDIR)|g'

all: $(PROG) $(HELPER) $(PLUGIN) $(GENERATED)

$(PROG): bsdthinkpad.c hw.c hw.h
	$(CC) $(CFLAGS) $(HW_CFLAGS) $(NOTIFY_CFLAGS) -o $(PROG) \
	    bsdthinkpad.c hw.c $(NOTIFY_LIBS) -lmixer

$(PLUGIN): bsdthinkpad-plugin.c ctl.c ctl.h hw.c hw.h
	$(CC) $(CFLAGS) -fPIC -shared $(HW_CFLAGS) $(PANEL_CFLAGS) \
	    -DG_LOG_DOMAIN='"bsdthinkpad-plugin"' -DVERSION='"$(VERSION)"' \
	    -o $(PLUGIN) bsdthinkpad-plugin.c ctl.c hw.c $(PANEL_LIBS) -lmixer

$(HELPER): bsdthinkpad-mute-led.c
	$(CC) $(CFLAGS) -DACPI_CALL='"$(LOCALBASE)/sbin/acpi_call"' \
	    -o $(HELPER) bsdthinkpad-mute-led.c

bsdthinkpad.desktop: bsdthinkpad.desktop.in
	$(SUBST) bsdthinkpad.desktop.in > $@
org.bsdthinkpad.mute-led.policy: org.bsdthinkpad.mute-led.policy.in
	$(SUBST) org.bsdthinkpad.mute-led.policy.in > $@
contrib/bsdthinkpad.conf: contrib/bsdthinkpad.conf.in
	$(SUBST) contrib/bsdthinkpad.conf.in > $@
contrib/bsdthinkpad-key: contrib/bsdthinkpad-key.in
	$(SUBST) contrib/bsdthinkpad-key.in > $@
contrib/bsdthinkpad-setup: contrib/bsdthinkpad-setup.in
	$(SUBST) contrib/bsdthinkpad-setup.in > $@
man/bsdthinkpad.1: man/bsdthinkpad.1.in
	$(SUBST) man/bsdthinkpad.1.in > $@
man/bsdthinkpad-mute-led.8: man/bsdthinkpad-mute-led.8.in
	$(SUBST) man/bsdthinkpad-mute-led.8.in > $@
man/bsdthinkpad-setup.8: man/bsdthinkpad-setup.8.in
	$(SUBST) man/bsdthinkpad-setup.8.in > $@

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(SBINDIR) \
	    $(DESTDIR)$(LIBEXECDIR) $(DESTDIR)$(POLKITDIR) \
	    $(DESTDIR)$(PLUGINDIR) $(DESTDIR)$(PLUGINDATA) \
	    $(DESTDIR)$(AUTOSTART) $(DESTDIR)$(DEVDDIR) \
	    $(DESTDIR)$(MANDIR)/man1 $(DESTDIR)$(MANDIR)/man8
	install -m 755 $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)
	install -m 755 contrib/bsdthinkpad-setup \
	    $(DESTDIR)$(SBINDIR)/bsdthinkpad-setup
	install -m 755 $(HELPER) $(DESTDIR)$(LIBEXECDIR)/$(HELPER)
	install -m 755 contrib/bsdthinkpad-key \
	    $(DESTDIR)$(LIBEXECDIR)/bsdthinkpad-key
	install -m 644 org.bsdthinkpad.mute-led.policy \
	    $(DESTDIR)$(POLKITDIR)/org.bsdthinkpad.mute-led.policy
	install -m 755 $(PLUGIN) $(DESTDIR)$(PLUGINDIR)/$(PLUGIN)
	install -m 644 bsdthinkpad-plugin.desktop \
	    $(DESTDIR)$(PLUGINDATA)/bsdthinkpad.desktop
	install -m 644 bsdthinkpad.desktop \
	    $(DESTDIR)$(AUTOSTART)/bsdthinkpad.desktop
	install -m 644 contrib/bsdthinkpad.conf \
	    $(DESTDIR)$(DEVDDIR)/bsdthinkpad.conf
	install -m 644 man/bsdthinkpad.1 $(DESTDIR)$(MANDIR)/man1/bsdthinkpad.1
	install -m 644 man/bsdthinkpad-setup.8 man/bsdthinkpad-mute-led.8 \
	    $(DESTDIR)$(MANDIR)/man8

# Without the port; as root (e.g. "su -m root -c 'make setup'")
setup: install
	pkg install -y libnotify acpi_call polkit consolekit2 xfce4-panel
	$(SBINDIR)/bsdthinkpad-setup

dist:
	git archive --prefix=$(PACKAGE)-$(VERSION)/ \
	    -o $(PACKAGE)-$(VERSION).tar.gz HEAD

clean:
	rm -f $(PROG) $(HELPER) $(PLUGIN) $(GENERATED)

.PHONY: all install setup dist clean
