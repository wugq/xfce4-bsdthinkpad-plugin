# Backlog

Open ideas and known issues, newest first within each section.

## Open

### Fewer wake-ups: events where FreeBSD has them

The watcher polls every 200 ms (about 0.1% of a CPU on the A475, 2026-10-13,
after the microphone check was made cheap). It could react to events where
there are some and poll the rest slowly:
- the mic-mute key: devd notify `0x1b` (`/var/run/devd.seqpacket.pipe`);
- brightness: `bsdthinkpad brightness` could tell the watcher;
- microphone mute from the panel or `mixer`, brightness from other tools:
  no event, a slow poll (~2 s) as a fallback.

The A475's speaker mute sends no event at all (only the sysctl changes), so
a fast poll of `dev.acpi_ibm.0.mute` stays; that is why the gain is small
(an estimated 0.1% → 0.05%). Not worth the extra code for now.

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
- A PCM slider in the panel again: PulseAudio does not change the card's
  `pcm` after all (measured on FreeBSD 15.1), and a stray low `pcm` made
  everything quiet.
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
