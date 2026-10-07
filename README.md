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
- Packages `libnotify`, `acpi_call`, `sudo`; kernel modules `acpi_ibm`, `acpi_call`

`make setup` (as root, once) installs and configures all of these:
- `pkg install libnotify acpi_call sudo`
- `acpi_ibm_load="YES"` and `acpi_call_load="YES"` in `/boot/loader.conf`, and loads them now
- `/usr/local/etc/sudoers.d/tposd`: members of `wheel` may run `/usr/local/sbin/acpi_call`
  without a password (tposd runs `sudo -n acpi_call ...` for the mute LED; `tposd -n` skips it)
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
tposd [-n] [-i interval-ms] [-a acpi-path]   watch and notify (started by autostart)
tposd brightness [+|-]N                      e.g. +10, -10, or 50 to set
tposd volume [+|-]N                          e.g. +5, -5, or 40 to set
  -n  do not drive the mute LED
  -i  poll interval in milliseconds (default 200)
  -a  ACPI method for the mute LED (default \_SB.PCI0.LPC0.EC0.HKEY.SSMS)
```

## Microphone mute key

On the A475 the mic-mute key arrives only as an ACPI event (`notify=0x1b`),
not as an X key. A devd rule runs a small script that toggles the microphone
and its LED; tposd then shows the popup. Both are in `contrib/` and are
installed by `make setup`.

## Finding the ACPI path of the mute LED on another model

```
sudo acpidump -dt > dsdt.asl
grep -n "Method (SSMS" dsdt.asl        # then find the enclosing Device (HKEY) path
sudo acpi_call -p '\_SB.PCI0.LPC0.EC0.HKEY.SSMS' -i 1   # LED on (also mutes)
sudo acpi_call -p '\_SB.PCI0.LPC0.EC0.HKEY.SSMS' -i 0   # LED off
```

## License

BSD 2-Clause, see `LICENSE`. tposd links against libnotify (LGPL-2.1-or-later) as a shared library.
