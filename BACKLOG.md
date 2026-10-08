# Backlog

Open ideas and known issues, newest first within each section.

## Open

Nothing at the moment.

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

- The popup is a GtkMenu (`xfce_panel_plugin_popup_menu()`), as in the
  pulseaudio and power manager plugins, instead of a hand-made window with
  its own grab; narrower, with a separator between display and sound.
- Debug messages: `PANEL_DEBUG=bsdthinkpad-plugin` (or `all`), as with the
  other panel plugins (README, "Debug messages").

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
