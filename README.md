# tposd

On-screen display and an XFCE panel plugin for ThinkPad brightness and mute
keys on **FreeBSD**.

On Linux, desktops such as XFCE show a small popup when you press the
brightness, volume or mute keys (`xfce4-power-manager`, `xfce4-pulseaudio-plugin`).
On FreeBSD several of these do not work out of the box: the power manager cannot
see the backlight, and newer ThinkPads mute the speaker in hardware without
telling anyone. `tposd` fills the gap with small C programs that use
FreeBSD's own interfaces:

- `tposd`, a watcher that sends standard desktop notifications and keeps the
  mute LEDs in step;
- a devd rule for the microphone mute key;
- `tposd-panel`, an XFCE panel plugin for the controls the PulseAudio plugin
  does not have.

## Working together with the PulseAudio plugin

Everyday **volume** stays with `xfce4-pulseaudio-plugin`: its volume keys,
popup, per-application volume and output switching. tposd does not touch
volume. It only adds what the PulseAudio plugin can not see on FreeBSD:

| | PulseAudio plugin | tposd |
|---|---|---|
| Volume keys, volume popup, per-app volume | yes | — |
| Brightness keys and popup | — | yes |
| Speaker mute key (muted in hardware by the EC) + its LED | — | yes |
| Microphone mute key + its LED | — | yes |
| OSS `pcm` level | — | yes: PCM slider in the panel plugin, `tposd pcm N` |

With `module-oss` the PulseAudio sink has hardware volume control
(`pactl list sinks` shows `HW_VOLUME_CTRL`): its volume is the OSS `vol` of the
sound card. The OSS `pcm` level is not reliably managed by PulseAudio (it
sometimes follows, mostly not), and what you hear is limited by both. The
panel plugin has a PCM slider for it, and its tooltip shows the OSS `vol` and
`pcm` of the default mixer, so a low `pcm` is easy to spot. Leave `pcm` at
1.00 (`tposd-setup` sets it and saves it to `/var/db/mixerN-state`) and use
the PulseAudio plugin for everyday volume.

Written and tested on a ThinkPad A475 (AMD) with FreeBSD 15.1 and XFCE.

## What it does

| Key | How FreeBSD sees it | tposd |
|---|---|---|
| Brightness −/+ | `backlight(9)`: `/dev/backlight/backlight0` | `tposd brightness ±N` changes it; popup with a bar |
| Speaker mute | the embedded controller mutes in hardware; only `sysctl dev.acpi_ibm.0.mute` changes; the key sends no event | popup, and turns the **mute LED** on/off via the ACPI method `SSMS` (`acpi_ibm(4)` does not) |
| Microphone mute | the recording level (`rec`) of every mixer; the key arrives as an ACPI event (`notify=0x1b`) | a devd rule toggles the microphone mute and its **LED**; popup, and tposd keeps the LED in step whoever changed the mute (panel, `tposd micmute`, `mixer`) |

The watcher polls these values (default every 200 ms) and shows a notification
whenever one changes, whoever changed it. Notifications are updated in place,
so pressing a key several times shows one popup, not a stack.

## Requirements

- FreeBSD 14 or later (`mixer(3)`, `backlight(9)`)
- A notification daemon (XFCE: `xfce4-notifyd`)
- Packages `libnotify`, `acpi_call`, `polkit`, `consolekit2`, `xfce4-panel`; kernel modules `acpi_ibm`, `acpi_call`

## Install

### As a package (port)

The port is in `port/sysutils/tposd`. It needs the ports framework
(`/usr/ports/Mk`), e.g. a shallow clone of the ports tree:
```
git clone --depth 1 https://git.FreeBSD.org/ports.git /usr/ports
```
Then, from a checkout of these sources:
```
make dist                                  # tposd-0.1.1.tar.gz (git archive of HEAD)
mkdir -p /usr/ports/distfiles
cp tposd-0.1.1.tar.gz /usr/ports/distfiles/
cd port/sysutils/tposd
make makesum                               # writes distinfo
make package                               # work/pkg/tposd-0.1.1.pkg
pkg install work/pkg/tposd-0.1.1.pkg       # as root; or "make install"
```
pkg installs the dependencies (libnotify, acpi_call, polkit, consolekit2,
xfce4-panel). Like any package, it does not change system configuration;
run `tposd-setup` once (below). `pkg delete tposd` removes it again.

### Without the port

```
make
su -m root -c 'make setup'   # install the dependencies, make install, tposd-setup
```
`make install` alone installs under `PREFIX` (default `/usr/local`), with
`DESTDIR` for staging. The Makefile works with both FreeBSD make and GNU make.

### What gets installed

| File (under `/usr/local`) | |
|---|---|
| `bin/tposd` | the watcher |
| `etc/xdg/autostart/tposd.desktop` | starts it with the desktop session |
| `lib/xfce4/panel/plugins/libtposd-panel.so`, `share/xfce4/panel/plugins/tposd-panel.desktop` | the panel plugin |
| `libexec/tposd-mute-led`, `share/polkit-1/actions/org.tposd.mute-led.policy` | the LED helper and its polkit policy (see below) |
| `libexec/tposd-key`, `etc/devd/tposd.conf` | the mic-mute key (devd) |
| `sbin/tposd-setup` | the system configuration, run once |
| `share/man/man1/tposd.1`, `share/man/man8/tposd-setup.8`, `share/man/man8/tposd-mute-led.8` | manual pages |

### tposd-setup

Run once as root after installing (`tposd-setup -n` shows the steps without
doing them):
- `acpi_ibm_load="YES"` and `acpi_call_load="YES"` in `/boot/loader.conf`,
  and loads them now
- the session part of `/usr/local/etc/pam.d/polkit-1` set to `pam_permit`, so
  pkexec does not abort (see "pkexec aborts" below; original kept as
  `polkit-1.orig`)
- restarts devd, so it reads the mic-mute rule. PulseAudio exits when devd
  restarts (its `module-devd-detect` loses the devd socket and aborts); it is
  started again, but programs that were playing sound lose it, hence the
  new login below
- OSS `pcm` at 1.00 on the default mixer, saved to `/var/db/mixerN-state`

Then log out and in again (tposd starts with the session) and add the panel
item (below).

## Configure XFCE

**Brightness keys → tposd.** Settings → Keyboard → Application Shortcuts, or:
```
xfconf-query -c xfce4-keyboard-shortcuts -n -t string -p /commands/custom/XF86MonBrightnessUp   -s "tposd brightness +10"
xfconf-query -c xfce4-keyboard-shortcuts -n -t string -p /commands/custom/XF86MonBrightnessDown -s "tposd brightness -10"
```
and stop the power manager from handling them too (it cannot see the
FreeBSD backlight anyway):
```
xfconf-query -c xfce4-power-manager -n -t bool -p /xfce4-power-manager/handle-brightness-keys -s false
```

**Volume keys → PulseAudio plugin.** Right-click the speaker icon in the
panel → Properties → "Enable keyboard shortcuts for volume control", or:
```
# find the plugin's number with: xfconf-query -c xfce4-panel -p /plugins -l -v | grep pulseaudio
xfconf-query -c xfce4-panel -p /plugins/plugin-N/enable-keyboard-shortcuts -s true
```
Do not bind `XF86AudioRaiseVolume` / `XF86AudioLowerVolume` in Application
Shortcuts; if you did earlier, remove them:
```
xfconf-query -c xfce4-keyboard-shortcuts -p /commands/custom/XF86AudioRaiseVolume -r
xfconf-query -c xfce4-keyboard-shortcuts -p /commands/custom/XF86AudioLowerVolume -r
```

**Mute keys** need no binding: the speaker mute is done by the embedded
controller, the mic mute by the devd rule (see below); tposd notices both.

**Panel plugin.** Right-click the panel → Panel → Add New Items →
"ThinkPad Controls". Put it next to the PulseAudio plugin.

## Panel plugin

One icon in the panel (it shows the brightness level). Scroll on it to change
the brightness. Hover for a summary, which follows the keys while the pointer
stays there (as the PulseAudio plugin's tooltip does). Click for:

```
 ☀  Brightness  ━━━━━━━━●━━━━   78%   backlight(9)
 🔊 PCM (OSS)   ━━━━━━━━━━━━●  100%   OSS pcm of the default mixer
 🔈 Speaker                  [ on ]   EC hardware mute (ACPI SSMS), LED included
 🎤 Microphone               [ on ]   recording level of all mixers, LED included
```

The popup takes no keyboard grab, so the brightness and volume keys keep
working while it is open (it closes when it loses focus or on Escape). Items the machine does not have (no
backlight, no `acpi_ibm`, no mixer with a recording level) are hidden. The popup re-reads the
hardware while open, so keys pressed meanwhile show up at once.

## Usage

```
tposd [-n] [-i interval-ms]                  watch and notify (started by autostart)
tposd brightness [+|-]N                      e.g. +10, -10, or 50 to set
tposd pcm [+|-]N                             OSS pcm level, e.g. 100
tposd micmute [on|off|toggle]                show or change the microphone mute
  -n  do not drive the mute LEDs
  -i  poll interval in milliseconds (default 200)
```
The watcher logs to syslog (`grep tposd /var/log/messages`): a failing LED
helper, a failing notification, and, at start, whether the kernel can read
its environment (see "polkit refuses a program of the console session").

## One microphone switch

FreeBSD has no system-wide "microphone off": every sound device has its own
OSS mixer, and a laptop's internal microphone is often a device of its own.
On the A475:
```
pcm3: <Realtek ALC298 (Analog 2.0+HP/2.0)> (play/rec)   mic jack: "mic", "rec"
pcm4: <Realtek ALC298 (Internal Analog Mic)> (rec)       internal mic: "rec"
```
Muting `mic` on the default mixer (pcm3) leaves the internal microphone on
pcm4 open. So tposd's microphone mute is the recording level `rec` of
**every** mixer that has one (as `mixer -a` lists them): muting sets all of
them, and the microphone counts as muted only when all are. Whatever device a
program records from, it then gets silence. (On Linux desktops the
equivalent switch is the mute of the PulseAudio/PipeWire source; the
mic-mute key here reaches only devd, which runs as root outside the desktop
session.)

## Mute keys

On the A475 the **speaker mute** key is handled by the embedded controller
alone: it mutes in hardware and sends no event (no ACPI notify, no X key);
only `dev.acpi_ibm.0.mute` changes. tposd notices the change and sets the LED
(pkexec, see below).

The **mic-mute** key arrives only as an ACPI event (`notify=0x1b`), not as
an X key. As in the examples of `acpi_ibm(4)`, a devd rule
(`etc/devd/tposd.conf`) runs `tposd-key micmute` as root, which runs `tposd micmute toggle`
and sets the LED; tposd then shows the popup.
The microphone switch in the panel plugin does the same. `tposd-setup`
restarts devd so it reads the rule (after upgrading from 0.1.0:
`service devd restart`, then log out and in; see "tposd-setup" above for why).

## How the mute LED gets root

Turning the speaker LED on needs an ACPI method call, which only root may do
(`/dev/acpi`); the mic LED is a sysctl that only root may write. tposd itself runs as you, inside your desktop session, so it
follows the same pattern as `xfce4-power-manager` and its
`xfpm-power-backlight-helper`:

- a tiny helper, `tposd-mute-led speaker|mic 0|1`, is the only part that
  runs as root. It accepts nothing else: for `speaker` it calls only the
  method `SSMS` of the ThinkPad hotkey device, so it cannot be used to call
  any other ACPI method; for `mic` it sets only `dev.acpi_ibm.0.mic_led`;
- tposd (at start, and when the microphone mute changes) and the panel
  plugin run it with `pkexec`; devd runs it directly for the mic-mute key;
- the polkit policy `org.tposd.mute-led` allows the user at the console
  (active local session, tracked by ConsoleKit2) to do that without a
  password; remote (ssh) and inactive sessions are refused:
  ```
  pkcheck --action-id org.tposd.mute-led --process $(pgrep -x tposd)   # allowed
pkexec /usr/local/libexec/tposd-mute-led mic 1   # from ssh: "Not authorized"
  ```

If the helper fails, tposd logs it once, still shows the notifications and
tries again at the next change. Running `pkexec` from an ssh shell is refused
even for the same user, because that shell is not the active console session.

### polkit refuses a program of the console session

ConsoleKit finds the session of a process by reading `XDG_SESSION_COOKIE`
from its environment (`kern.proc.env`, as `procstat -e` does). On FreeBSD
15.1 that read fails with ENOMEM for a few percent of processes: the kernel
reads each string in 256-byte chunks and fails the whole read when a chunk
runs past the top of the stack, which happens when ASLR places the
environment strings within 256 bytes of it. Such a process gets "Not
authorized" although it belongs to the active session:
```
$ procstat -e $(pgrep -x tposd)
procstat: sysctl(kern.proc.env): Cannot allocate memory
$ pkcheck --action-id org.tposd.mute-led --process $(pgrep -x tposd)
Not authorized.
```
It is random per program start (with ASLR disabled through `proccontrol -m
aslr -s disable` it does not happen). Then the speaker LED does not follow
the mute key in that session; logging out and in again helps. Details,
reproduction and analysis: [docs/kern-proc-env-bug.md](docs/kern-proc-env-bug.md).

### pkexec aborts: "pam_conversation_function: code should not be reached"

On FreeBSD `/usr/local/etc/pam.d/polkit-1` includes `system`, whose session
part has `pam_lastlog`. Once root has a previous login record, `pam_lastlog`
wants to print "Last login: ...", and pkexec, which allows no PAM messages,
aborts (signal 6; `/var/log/messages` shows `pkexec ... exited on signal 6`).
Authorization itself has already succeeded at that point. This affects every
pkexec user, e.g. xfce4-power-manager's backlight helper too. Fix it by giving
polkit-1 a session stack without `pam_lastlog` (auth and account are
unchanged). `tposd-setup` does this (only if the line is still
`session include system`; the original is kept as `polkit-1.orig`). By hand,
as root:
```
sed -i.orig 's/^session[[:space:]]*include[[:space:]]*system$/session    required     pam_permit.so/' /usr/local/etc/pam.d/polkit-1
```
To undo: `mv /usr/local/etc/pam.d/polkit-1.orig /usr/local/etc/pam.d/polkit-1`.

## Other ThinkPads

The helper does not hard-code the ACPI path: it reads the hotkey device from
`acpi_ibm(4)` and appends `.SSMS`:
```
$ sysctl dev.acpi_ibm.0.%location
dev.acpi_ibm.0.%location: handle=\_SB_.PCI0.LPC0.EC0_.HKEY
```
It works on ThinkPads whose firmware has the method `SSMS` (most models of the
last decade). To check yours, as root:
```
acpidump -dt | grep -n "Method (SSMS"
/usr/local/libexec/tposd-mute-led speaker 1   # LED on (also mutes the speaker)
/usr/local/libexec/tposd-mute-led speaker 0   # LED off
```
Brightness, PCM and microphone work on any FreeBSD laptop.

## Source layout

| File | Layer | |
|---|---|---|
| `hw.c`, `hw.h` | hardware | backlight(9), mixer(3), acpi_ibm(4) sysctls; no GLib, no UI |
| `ctl.c`, `ctl.h` | logic | state of the controls, polling, requests; root requests (pkexec) run asynchronously and the state shows the requested value meanwhile; reports changes through a callback. GLib only, no GTK |
| `tposd-panel.c` | UI | GTK widgets: renders the `ctl` state, passes user actions to `ctl_set_*()` |
| `tposd.c` | watcher | notifications and LED sync, uses `hw.c` |
| `tposd-mute-led.c` | root helper | run through pkexec |
| `contrib/` | system | devd rule and script for the mic-mute key; `tposd-setup` |
| `man/` | manual | `tposd(1)`, `tposd-setup(8)`, `tposd-mute-led(8)` |
| `port/sysutils/tposd` | packaging | FreeBSD port |

The UI never calls the hardware or pkexec itself, and never waits: an early
version ran pkexec synchronously inside a GTK handler while the popup held a
grab, and the plugin crashed (`_XAllocID` assertion in Xlib).

## License

BSD 2-Clause, see `LICENSE`. tposd links against libnotify (LGPL-2.1-or-later), the panel plugin against
GTK 3 and libxfce4panel (LGPL), all as shared libraries.
