# Backlog

Open ideas and known issues, newest first within each section.

## Open

### Popup as a GtkMenu, like the other panel plugins

The popup is a hand-made `GtkWindow` (type hint UTILITY): placement, the
keyboard and pointer grab, closing on a click outside and on the panel button
are all our own code (the source of the "click on the icon does not close it"
bug fixed in 2026-10). Since 2026-10 it only borrows the theme's menu look
(`GTK_STYLE_CLASS_MENU`).

xfce4-pulseaudio-plugin does it differently: `PulseaudioMenu` is a `GtkMenu`,
its sliders are `XfpaScaleMenuItem` (a `GtkScale` in a menu item,
`scalemenuitem.c`), and it is shown with `xfce_panel_plugin_popup_menu()`;
GTK and the panel handle grab, closing and keyboard navigation. The power
manager plugin uses a menu too (with a switch item).

Survey (2026-10-09): nearly all official plugins use a GtkMenu with
`xfce_panel_plugin_popup_menu()` (pulseaudio, power manager, clipman, xkb,
notifications, window menu, tasklist, systray); the clock's calendar uses
`xfce_panel_plugin_popup_window()` (4.19.0+); only whiskermenu makes its own
window. So: rebuild the popup as a GtkMenu, the brightness slider as a scale
menu item (as pulseaudio's / the power manager's) and the switches as the
power manager's "Presentation mode" item. The same applies to
xfce4-bsdbluetooth-plugin.

## Decided not to do

### Volume keys unmute the speaker

With the hardware mute on, volume up/down unmute the speaker. This is the
embedded controller's own behaviour: in both modes that keep a hardware mute
(SAUM `LATCH` and `TOGGLE`, see Linux thinkpad_acpi) up/down unmute. Linux
avoids it with `NONE` (the EC never mutes, mute is done in software), which
would be a redesign here. Left as the firmware does it.

### A PCM slider in the panel

PulseAudio's module-oss writes the OSS `pcm` level together with `vol` on every
volume change, but reads only `vol`, so a `pcm` set by us is overwritten at the
next change. The PCM slider and the OSS levels in the tooltip were removed in
2026-10; `bsdthinkpad pcm N` remains for setting it by hand.

## Done (2026-10)

- Clicking the panel icon again closes the popup (compare screen
  coordinates, as xfce4-bsdbluetooth-plugin does).
- Scroll on the panel icon changes the brightness, 4% per notch (as the
  pulseaudio plugin's volume step); small steps no longer get stuck on the
  hardware's rounding.
- The popup has the theme's menu background and border, and less empty space.
- The brightness notification's bar follows scrolling and the keys smoothly.
  It looked choppier than pulseaudio's volume notification with the same 4%
  step; neither smooths the bar (xfce4-notifyd draws the value as it is), the
  difference was timing: pulseaudio notifies on every change, bsdthinkpad
  polled every 200 ms. Now it polls every 30 ms for 2 s after a change. The
  notifications also have the "transient" hint, like pulseaudio's.
