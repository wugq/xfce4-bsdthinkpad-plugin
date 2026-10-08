# xfce4-bsdthinkpad-plugin

On-screen display and an XFCE panel plugin for ThinkPad brightness and mute
keys on **FreeBSD**, next to `xfce4-pulseaudio-plugin`. Linux does not need
it: there the power manager and the PulseAudio plugin already handle these
keys.

On Linux, desktops such as XFCE show a small popup when you press the
brightness, volume or mute keys (`xfce4-power-manager`, `xfce4-pulseaudio-plugin`).
On FreeBSD several of these do not work out of the box: the power manager cannot
see the backlight, and newer ThinkPads mute the speaker in hardware without
telling anyone. This package fills the gap with small C programs that use
FreeBSD's own interfaces:

- `bsdthinkpad`, a watcher that sends standard desktop notifications and keeps the
  mute LEDs in step;
- a devd rule for the microphone mute key;
- `bsdthinkpad-plugin`, an XFCE panel plugin for the controls the PulseAudio plugin
  does not have.

## Working together with the PulseAudio plugin

Everyday **volume** stays with `xfce4-pulseaudio-plugin`: its volume keys,
popup, per-application volume and output switching. bsdthinkpad does not touch
volume. It only adds what the PulseAudio plugin can not see on FreeBSD:

| | PulseAudio plugin | bsdthinkpad |
|---|---|---|
| Volume keys, volume popup, per-app volume | yes | — |
| Brightness keys and popup | — | yes |
| Speaker mute key (muted in hardware by the EC) + its LED | — | yes |
| Microphone mute key + its LED | — | yes |
| OSS `pcm` level | — | `bsdthinkpad pcm N` (command line only) |

With `module-oss` the PulseAudio sink has hardware volume control
(`pactl list sinks` shows `HW_VOLUME_CTRL`): its volume is the OSS `vol` of the
sound card. Whenever PulseAudio changes its volume it writes the OSS `pcm`
level too, the same value as `vol`, but it reads only `vol`; so a `pcm` set by
anything else is overwritten at the next volume change. The panel plugin leaves
both alone; use the PulseAudio plugin for everyday volume.
`bsdthinkpad-setup` sets `pcm` to 1.00 and saves it to `/var/db/mixerN-state`;
`bsdthinkpad pcm N` changes it by hand if needed.

**Tested on one machine only:** a ThinkPad A475 (AMD) with FreeBSD 15.1 and
XFCE 4.20. Other ThinkPads and other FreeBSD versions have not been tried;
see "Other ThinkPads" below for what is model-specific. Reports from other
models are welcome.

## What it does

| Key | How FreeBSD sees it | bsdthinkpad |
|---|---|---|
| Brightness −/+ | `backlight(9)`: `/dev/backlight/backlight0` | `bsdthinkpad brightness ±N` changes it; popup with a bar |
| Speaker mute | the embedded controller mutes in hardware; only `sysctl dev.acpi_ibm.0.mute` changes; the key sends no event | popup, and turns the **mute LED** on/off via the ACPI method `SSMS` (`acpi_ibm(4)` does not) |
| Microphone mute | the recording level (`rec`) of every mixer; the key arrives as an ACPI event (`notify=0x1b`) | a devd rule toggles the microphone mute and its **LED**; popup, and bsdthinkpad keeps the LED in step whoever changed the mute (panel, `bsdthinkpad micmute`, `mixer`) |

The watcher polls these values (default every 200 ms) and shows a notification
whenever one changes, whoever changed it. Notifications are updated in place,
so pressing a key several times shows one popup, not a stack.

## Requirements

- FreeBSD 14 or later (`mixer(3)`, `backlight(9)`)
- A notification daemon (XFCE: `xfce4-notifyd`)
- Packages `libnotify`, `acpi_call`, `polkit`, `consolekit2`, `xfce4-panel`; kernel modules `acpi_ibm`, `acpi_call`

## Install

### Binary package from a release

Each [release](https://github.com/wugq/xfce4-bsdthinkpad-plugin/releases)
has packages for FreeBSD 14 and 15 (amd64), built by GitHub Actions from the
tag (`.github/workflows/release.yml`). Download the one for your FreeBSD
version and, as root:
```
pkg install ./xfce4-bsdthinkpad-plugin-0.2.1-FreeBSD-15-amd64.pkg
```
pkg installs the dependencies from the FreeBSD package repositories. Then
run `bsdthinkpad-setup` (below). Newer releases are not picked up by
`pkg upgrade`; install the new package the same way.

### As a package (port)

The port is in `port/sysutils/xfce4-bsdthinkpad-plugin`; it fetches the
release (tag `v0.2.1`) from GitHub. It is not in the FreeBSD ports tree, but
it does not need to be: a port directory works from anywhere, as long as the
ports framework (`/usr/ports/Mk`) is installed. As root:
```
# 1. the ports framework, if there is no /usr/ports yet (a shallow clone, ~1 GB)
git clone --depth 1 https://git.FreeBSD.org/ports.git /usr/ports

# 2. these sources
git clone https://github.com/wugq/xfce4-bsdthinkpad-plugin
cd xfce4-bsdthinkpad-plugin/port/sysutils/xfce4-bsdthinkpad-plugin

# 3. build the package and install it
make install-missing-packages   # dependencies as binary packages (pkg)
make package                    # work/pkg/xfce4-bsdthinkpad-plugin-0.2.1.pkg
pkg install work/pkg/xfce4-bsdthinkpad-plugin-0.2.1.pkg
make clean
```
Without `make install-missing-packages`, the ports framework would build the
missing dependencies from source.

(`git` itself comes from `pkg install git`.) The dependencies are libnotify,
acpi_call, polkit, consolekit2 and xfce4-panel. Like any package, it does
not change system configuration; run `bsdthinkpad-setup` once (below).
`pkg delete xfce4-bsdthinkpad-plugin` removes it again (`pkg autoremove` then
also removes dependencies nothing else needs, such as acpi_call).

### Without the port

```
make
su -m root -c 'make setup'   # install the dependencies, make install, bsdthinkpad-setup
```
`make install` alone installs under `PREFIX` (default `/usr/local`), with
`DESTDIR` for staging. The Makefile works with both FreeBSD make and GNU make.

### What gets installed

| File (under `/usr/local`) | |
|---|---|
| `bin/bsdthinkpad` | the watcher |
| `etc/xdg/autostart/bsdthinkpad.desktop` | starts it with the desktop session |
| `lib/xfce4/panel/plugins/libbsdthinkpad-plugin.so`, `share/xfce4/panel/plugins/bsdthinkpad.desktop` | the panel plugin |
| `libexec/bsdthinkpad-mute-led`, `share/polkit-1/actions/org.bsdthinkpad.mute-led.policy` | the LED helper and its polkit policy (see below) |
| `libexec/bsdthinkpad-key`, `etc/devd/bsdthinkpad.conf` | the mic-mute key (devd) |
| `sbin/bsdthinkpad-setup` | the system configuration, run once |
| `share/man/man1/bsdthinkpad.1`, `share/man/man8/bsdthinkpad-setup.8`, `share/man/man8/bsdthinkpad-mute-led.8` | manual pages |

### bsdthinkpad-setup

Run once as root after installing (`bsdthinkpad-setup -n` shows the steps without
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

Then log out and in again (bsdthinkpad starts with the session) and add the panel
item (below).

## Configure XFCE

**Brightness keys → bsdthinkpad.** Settings → Keyboard → Application Shortcuts, or:
```
xfconf-query -c xfce4-keyboard-shortcuts -n -t string -p /commands/custom/XF86MonBrightnessUp   -s "bsdthinkpad brightness +10"
xfconf-query -c xfce4-keyboard-shortcuts -n -t string -p /commands/custom/XF86MonBrightnessDown -s "bsdthinkpad brightness -10"
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
controller, the mic mute by the devd rule (see below); bsdthinkpad notices both.

**Panel plugin.** Right-click the panel → Panel → Add New Items →
"ThinkPad Controls". Put it next to the PulseAudio plugin.

## Panel plugin

One icon in the panel (it shows the brightness level). Scroll on it to change
the brightness. Hover for a summary, which follows the keys while the pointer
stays there (as the PulseAudio plugin's tooltip does). Click for:

```
 ☀  ━━━━━━━━●━━━━   78%       backlight(9)
 ──────────────────────
 🔈 Speaker        [ on ]     EC hardware mute (ACPI SSMS), LED included
 🎤 Microphone     [ on ]     recording level of all mixers, LED included
```

The popup is a menu, like the PulseAudio plugin's: it grabs keyboard and
pointer while open, so the brightness and volume keys work again once it is
closed (a click outside, Escape, or the panel icon closes it). A click on a
switch's row flips it and leaves the menu open. Items the machine does not
have (no backlight, no `acpi_ibm`, no mixer with a recording level) are
hidden. The menu re-reads the hardware while open, so keys pressed meanwhile
show up at once.

### Debug messages

Off unless asked for, as with the other panel plugins. The plugin runs in a
process of the panel's (`wrapper-2.0`) and writes to the panel's standard
error, so restart the panel with `PANEL_DEBUG` set (a comma separated list;
`all` turns on the panel's own messages too):
```
xfce4-panel -q
PANEL_DEBUG=bsdthinkpad-plugin xfce4-panel > /tmp/panel.log 2>&1 &
tail -f /tmp/panel.log | grep bsdthinkpad-plugin
```
They tell what the menu, the slider, the switches and the scroll wheel did,
what brightness was asked for and what the hardware took, and the root
helper's requests and results. `G_MESSAGES_DEBUG=bsdthinkpad-plugin` works
too. Log out and in (or run `xfce4-panel -q; xfce4-panel &` without it) to
turn them off again.

## Usage

```
bsdthinkpad [-n] [-i interval-ms]                  watch and notify (started by autostart)
bsdthinkpad brightness [+|-]N                      e.g. +10, -10, or 50 to set
bsdthinkpad pcm [+|-]N                             OSS pcm level, e.g. 100
bsdthinkpad micmute [on|off|toggle]                show or change the microphone mute
  -n  do not drive the mute LEDs
  -i  poll interval in milliseconds (default 200)
```
The watcher logs to syslog (`grep bsdthinkpad /var/log/messages`): a failing LED
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
pcm4 open. So bsdthinkpad's microphone mute is the recording level `rec` of
**every** mixer that has one (as `mixer -a` lists them): muting sets all of
them, and the microphone counts as muted only when all are. Whatever device a
program records from, it then gets silence. (On Linux desktops the
equivalent switch is the mute of the PulseAudio/PipeWire source; the
mic-mute key here reaches only devd, which runs as root outside the desktop
session.)

## Mute keys

On the A475 the **speaker mute** key is handled by the embedded controller
alone: it mutes in hardware and sends no event (no ACPI notify, no X key);
only `dev.acpi_ibm.0.mute` changes. bsdthinkpad notices the change and sets the LED
(pkexec, see below).

The **mic-mute** key arrives only as an ACPI event (`notify=0x1b`), not as
an X key. As in the examples of `acpi_ibm(4)`, a devd rule
(`etc/devd/bsdthinkpad.conf`) runs `bsdthinkpad-key micmute` as root, which runs `bsdthinkpad micmute toggle`
and sets the LED; bsdthinkpad then shows the popup.
The microphone switch in the panel plugin does the same. `bsdthinkpad-setup`
restarts devd so it reads the rule (then log out and in; see
"bsdthinkpad-setup" above for why).

## How the mute LED gets root

Turning the speaker LED on needs an ACPI method call, which only root may do
(`/dev/acpi`); the mic LED is a sysctl that only root may write. bsdthinkpad itself runs as you, inside your desktop session, so it
follows the same pattern as `xfce4-power-manager` and its
`xfpm-power-backlight-helper`:

- a tiny helper, `bsdthinkpad-mute-led speaker|mic 0|1`, is the only part that
  runs as root. It accepts nothing else: for `speaker` it calls only the
  method `SSMS` of the ThinkPad hotkey device, so it cannot be used to call
  any other ACPI method; for `mic` it sets only `dev.acpi_ibm.0.mic_led`;
- bsdthinkpad (at start, and when the microphone mute changes) and the panel
  plugin run it with `pkexec`; devd runs it directly for the mic-mute key;
- the polkit policy `org.bsdthinkpad.mute-led` allows the user at the console
  (active local session, tracked by ConsoleKit2) to do that without a
  password; remote (ssh) and inactive sessions are refused:
  ```
  pkcheck --action-id org.bsdthinkpad.mute-led --process $(pgrep -x bsdthinkpad)   # allowed
pkexec /usr/local/libexec/bsdthinkpad-mute-led mic 1   # from ssh: "Not authorized"
  ```

If the helper fails, bsdthinkpad logs it once, still shows the notifications and
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
$ procstat -e $(pgrep -x bsdthinkpad)
procstat: sysctl(kern.proc.env): Cannot allocate memory
$ pkcheck --action-id org.bsdthinkpad.mute-led --process $(pgrep -x bsdthinkpad)
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
unchanged). `bsdthinkpad-setup` does this (only if the line is still
`session include system`; the original is kept as `polkit-1.orig`). By hand,
as root:
```
sed -i.orig 's/^session[[:space:]]*include[[:space:]]*system$/session    required     pam_permit.so/' /usr/local/etc/pam.d/polkit-1
```
To undo: `mv /usr/local/etc/pam.d/polkit-1.orig /usr/local/etc/pam.d/polkit-1`.

## Other ThinkPads

Only the A475 has been tested. What depends on the model:

- the speaker mute LED: the ACPI method `SSMS` (below);
- the mic-mute key arriving as ACPI event `notify=0x1b`, and the speaker mute
  key sending no event at all: other models may differ (watch
  `nc -U /var/run/devd.pipe` while pressing the keys);
- the internal microphone being a sound device of its own (`pcm4` on the
  A475), which is why the microphone mute covers every mixer.

The helper does not hard-code the ACPI path: it reads the hotkey device from
`acpi_ibm(4)` and appends `.SSMS`:
```
$ sysctl dev.acpi_ibm.0.%location
dev.acpi_ibm.0.%location: handle=\_SB_.PCI0.LPC0.EC0_.HKEY
```
It should work on ThinkPads whose firmware has the method `SSMS`. To check
yours, as root:
```
acpidump -dt | grep -n "Method (SSMS"
/usr/local/libexec/bsdthinkpad-mute-led speaker 1   # LED on (also mutes the speaker)
/usr/local/libexec/bsdthinkpad-mute-led speaker 0   # LED off
```
Brightness (`backlight(9)`) and the microphone mute use no ThinkPad
interface and should work on other FreeBSD laptops too (untested).

## Source layout

| File | Layer | |
|---|---|---|
| `hw.c`, `hw.h` | hardware | backlight(9), mixer(3), acpi_ibm(4) sysctls; no GLib, no UI |
| `ctl.c`, `ctl.h` | logic | state of the controls, polling, requests; root requests (pkexec) run asynchronously and the state shows the requested value meanwhile; reports changes through a callback. GLib only, no GTK |
| `bsdthinkpad-plugin.c` | UI | GTK widgets: renders the `ctl` state, passes user actions to `ctl_set_*()` |
| `bsdthinkpad.c` | watcher | notifications and LED sync, uses `hw.c` |
| `bsdthinkpad-mute-led.c` | root helper | run through pkexec |
| `contrib/` | system | devd rule and script for the mic-mute key; `bsdthinkpad-setup` |
| `man/` | manual | `bsdthinkpad(1)`, `bsdthinkpad-setup(8)`, `bsdthinkpad-mute-led(8)` |
| `port/sysutils/xfce4-bsdthinkpad-plugin` | packaging | FreeBSD port |

The UI never calls the hardware or pkexec itself, and never waits: an early
version ran pkexec synchronously inside a GTK handler while the popup held a
grab, and the plugin crashed (`_XAllocID` assertion in Xlib).

The menu follows the official panel plugins: a `GtkMenu` popped up with
`xfce_panel_plugin_popup_menu()`, as in the PulseAudio and power manager
plugins. Inside a menu the pointer's events go to the menu item, not to the
widgets in it, so the brightness item passes a press on its slider on to the
slider (which then grabs the pointer), and keeps the release from the menu.

## License

BSD 2-Clause, see `LICENSE`. bsdthinkpad links against libnotify (LGPL-2.1-or-later), the panel plugin against
GTK 3 and libxfce4panel (LGPL), all as shared libraries.
