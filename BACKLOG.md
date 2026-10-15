# Backlog

Open ideas and known issues, newest first within each section.

## Open

### Speaker LED on the X1 Carbon

The watcher now sets the speaker LED with `GSMS` only (never `SSMS`). On the
A475 `GSMS` sets the LED; on the X1 Carbon 3rd gen its DSDT shows `GSMS` only
reading (`Return (\AUDC (0x00, 0x00))`). Check whether the X1 Carbon's LED
follows the mute key by itself; if not, it needs another way (e.g. `SSMS`
once the key has been left alone, which is when it can no longer undo a
press).

## Decided not to do

### Volume keys unmute the speaker

With the hardware mute on, volume up/down unmute the speaker. This is the
embedded controller's own behaviour: in both modes that keep a hardware mute
(SAUM `LATCH` and `TOGGLE`, see Linux thinkpad_acpi) up/down unmute. Linux
avoids it with `NONE` (the EC never mutes, mute is done in software), which
would be a redesign here. Left as the firmware does it.

## Done (2026-10)

- The watcher's microphone check costs ~76 us instead of ~600 us (two
  ioctls per mixer, the mixer count kept for 10 s); its CPU time fell to a
  third.
- The watcher waits for events when the tpmute kernel module (a separate
  project) is loaded: devd for the speaker mute (tpmute's `type=MUTE`) and
  the mic-mute key, `SIGWINCH` from `bsdthinkpad brightness`/`micmute` and
  the panel plugin, a 2 s safety poll; without tpmute, 200 ms polling as
  before. 0.01 s of CPU per minute on the A475 instead of ~0.15 s. Neither
  test machine's firmware sends a devd event for the mute key itself
  (checked on the A475 and the X1 Carbon: acpi_ibm would pass notify
  0x15-0x17 on, but they are never raised), hence the kernel module.
- Speaker LED: the watcher no longer calls `SSMS` (it sets the mute too,
  and writing back a just-read state could undo a key press); `speaker
  sync` calls only `GSMS`, which on the A475 sets the LED to the firmware's
  mute, and the watcher calls it once the mute has stayed put for 0.5 s
  (firmware calls during quick presses left the firmware's mute and the
  controller's apart). If `GSMS` disagrees with the mute it looks again, at
  most three times in a row.
- Panel sliders are dragged by their menu item itself: the PCM slider would
  not move until the brightness slider had been dragged once.
- A PCM slider in the panel again: PulseAudio writes `pcm` with every volume
  change, but restores only `vol` when it starts, so a stray low `pcm` from
  before made everything quiet with no way to see it from the desktop.
- The speaker mute is shown by the PulseAudio plugin: the watcher keeps the
  sink mute in step with the embedded controller's.

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
