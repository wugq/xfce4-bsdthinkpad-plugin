# tposd

On-screen display for ThinkPad brightness, volume and mute keys on **FreeBSD**.

On Linux, desktops such as XFCE show a small popup when you press the
brightness, volume or mute keys (`xfce4-power-manager`, `xfce4-pulseaudio-plugin`).
On FreeBSD several of these do not work out of the box: the power manager cannot
see the backlight, and newer ThinkPads mute the speaker in hardware without
telling anyone. `tposd` fills the gap with one small C program that uses
FreeBSD's own interfaces and sends standard desktop notifications.

Written and tested on a ThinkPad A475 (AMD) with FreeBSD 15.1 and XFCE.

## What it does

| Key | How FreeBSD sees it | tposd |
|---|---|---|
| Brightness −/+ | `backlight(9)`: `/dev/backlight/backlight0` | `tposd brightness ±N` changes it; popup with a bar |
| Volume −/+ | `mixer(3)`: `vol` of the default mixer | `tposd volume ±N` changes it; popup with a bar (this is the sound card's master volume; PulseAudio's own volume is a separate layer) |
| Speaker mute | the embedded controller mutes in hardware; only `sysctl dev.acpi_ibm.0.mute` changes | popup, and turns the **mute LED** on/off via the ACPI method `SSMS` (`acpi_ibm(4)` does not) |
| Microphone mute | `mic` of the default mixer | popup |

The watcher polls these values (default every 200 ms) and shows a notification
whenever one changes, whoever changed it. Notifications are updated in place,
so pressing a key several times shows one popup, not a stack.

## Requirements

- FreeBSD 14 or later (`mixer(3)`, `backlight(9)`)
- A notification daemon (XFCE: `xfce4-notifyd`)
- Packages `libnotify`, `acpi_call`, `polkit`, `consolekit2`; kernel modules `acpi_ibm`, `acpi_call`

`make setup` (as root, once) installs and configures all of these:
- `pkg install libnotify acpi_call polkit consolekit2`
- `acpi_ibm_load="YES"` and `acpi_call_load="YES"` in `/boot/loader.conf`, and loads them now
- the mute LED helper `/usr/local/libexec/tposd-mute-led` and its polkit policy
  `/usr/local/share/polkit-1/actions/org.tposd.mute-led.policy` (see below)
- the mic-mute devd rule and script from `contrib/`

## Build and install

```
su -m root -c 'make setup'   # once, as root
make
make install                 # ~/.local/bin/tposd and ~/.config/autostart/tposd.desktop
```
`make install PREFIX=/usr/local` for a system-wide install (run as root).
The Makefile works with both FreeBSD make and GNU make.
Log out and in again (or run `~/.local/bin/tposd &`) to start it.

## Bind the keys (XFCE)

Settings → Keyboard → Application Shortcuts, or:
```
xfconf-query -c xfce4-keyboard-shortcuts -n -t string -p /commands/custom/XF86MonBrightnessUp   -s "$HOME/.local/bin/tposd brightness +10"
xfconf-query -c xfce4-keyboard-shortcuts -n -t string -p /commands/custom/XF86MonBrightnessDown -s "$HOME/.local/bin/tposd brightness -10"
xfconf-query -c xfce4-keyboard-shortcuts -n -t string -p /commands/custom/XF86AudioRaiseVolume  -s "$HOME/.local/bin/tposd volume +5"
xfconf-query -c xfce4-keyboard-shortcuts -n -t string -p /commands/custom/XF86AudioLowerVolume  -s "$HOME/.local/bin/tposd volume -5"
```
So that other programs do not handle the same keys:
```
xfconf-query -c xfce4-power-manager -n -t bool -p /xfce4-power-manager/handle-brightness-keys -s false
# pulseaudio panel plugin (find its number with: xfconf-query -c xfce4-panel -p /plugins -l -v)
xfconf-query -c xfce4-panel -p /plugins/plugin-N/enable-keyboard-shortcuts -s false
```

## Usage

```
tposd [-n] [-i interval-ms]                  watch and notify (started by autostart)
tposd brightness [+|-]N                      e.g. +10, -10, or 50 to set
tposd volume [+|-]N                          e.g. +5, -5, or 40 to set
  -n  do not drive the mute LED
  -i  poll interval in milliseconds (default 200)
```

## Microphone mute key

On the A475 the mic-mute key arrives only as an ACPI event (`notify=0x1b`),
not as an X key. A devd rule runs a small script that toggles the microphone
and its LED; tposd then shows the popup. Both are in `contrib/` and are
installed by `make setup`.

## How the mute LED gets root

Turning the LED on needs an ACPI method call, which only root may do
(`/dev/acpi`). tposd itself runs as you, inside your desktop session, so it
follows the same pattern as `xfce4-power-manager` and its
`xfpm-power-backlight-helper`:

- a tiny helper, `tposd-mute-led 0|1`, is the only part that runs as root.
  It accepts nothing but `0` or `1` and calls only the method `SSMS` of the
  ThinkPad hotkey device, so it cannot be used to call any other ACPI method;
- tposd runs it with `pkexec`;
- the polkit policy `org.tposd.mute-led` allows the user at the console
  (active local session, tracked by ConsoleKit2) to do that without a
  password; remote (ssh) and inactive sessions are refused:
  ```
  pkcheck --action-id org.tposd.mute-led --process $(pgrep -x tposd)   # allowed
  ```

If the helper is missing or refused, tposd simply stops driving the LED and
still shows the notifications.

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
/usr/local/libexec/tposd-mute-led 1     # LED on (also mutes the speaker)
/usr/local/libexec/tposd-mute-led 0     # LED off
```
Brightness, volume and microphone notifications work on any FreeBSD laptop.

## License

BSD 2-Clause, see `LICENSE`. tposd links against libnotify (LGPL-2.1-or-later) as a shared library.
