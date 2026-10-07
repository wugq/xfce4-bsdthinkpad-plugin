# Makefile for tposd -- works with FreeBSD make and GNU make
#
#   make                 build
#   make install         install to ~/.local/bin (PREFIX=... to change)
#   make CFLAGS="-O0 -g3"  build for debugging
#   make clean
#   make setup           as root, once: packages, kernel modules, sudo rule,
#                        mic-mute devd rule (FreeBSD only)

PROG    = tposd
PREFIX ?= $(HOME)/.local
CC     ?= cc
CFLAGS ?= -O2 -pipe
CFLAGS += -Wall -Wextra

NOTIFY_CFLAGS != pkg-config --cflags libnotify
NOTIFY_LIBS   != pkg-config --libs libnotify

all: $(PROG)

$(PROG): tposd.c
	$(CC) $(CFLAGS) $(NOTIFY_CFLAGS) -o $(PROG) tposd.c $(NOTIFY_LIBS) -lmixer

install: $(PROG)
	install -d $(PREFIX)/bin $(HOME)/.config/autostart
	install -m 755 $(PROG) $(PREFIX)/bin/$(PROG)
	sed "s#@BINDIR@#$(PREFIX)/bin#" tposd.desktop.in > $(HOME)/.config/autostart/tposd.desktop

clean:
	rm -f $(PROG)

# System-wide dependencies; run as root (e.g. "su -m root -c 'make setup'")
setup:
	pkg install -y libnotify acpi_call sudo
	sysrc -f /boot/loader.conf acpi_ibm_load=YES acpi_call_load=YES
	kldload -n acpi_ibm
	kldload -n acpi_call
	install -d /usr/local/etc/sudoers.d /usr/local/libexec /usr/local/etc/devd
	install -m 440 contrib/tposd.sudoers /usr/local/etc/sudoers.d/tposd
	visudo -c -f /usr/local/etc/sudoers.d/tposd
	install -m 755 contrib/thinkpad-micmute /usr/local/libexec/thinkpad-micmute
	install -m 644 contrib/thinkpad-micmute.conf /usr/local/etc/devd/thinkpad-micmute.conf
	service devd restart

.PHONY: all install clean setup
