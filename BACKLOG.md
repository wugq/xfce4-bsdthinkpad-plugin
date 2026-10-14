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

Idea 2026-10-14 (to discuss, nothing built): a small kernel module that
does the polling in the kernel and sends the events FreeBSD lacks.
- A callout every 200 ms reads the EC speaker mute (what `acpi_ibm`'s
  `dev.acpi_ibm.0.mute` handler reads) and, on a change, sends a devd
  event, e.g. `system=ACPI subsystem=IBM type=MUTE`. A kernel timer costs
  far less than waking a process (no context switch, no syscalls).
- bsdthinkpad then waits on `/var/run/devd.pipe` instead of polling: the
  speaker mute from the module, the mic-mute key from the existing ACPI
  notify, brightness from `bsdthinkpad brightness` telling the watcher; a
  slow (~2 s) poll stays only as a fallback for changes by other tools.
- Expected: the watcher's CPU from ~0.15% to ~0.01%, gone from `top`.
- Costs: a kmod built per FreeBSD version (a separate kmod port, root
  install, loader configuration); a bug panics the machine instead of
  ending one process; ThinkPad- and FreeBSD-only (NetBSD would need its own).
- Plan if done: a separate experiment first (~100+ lines), measured on the
  A475, merged into bsdthinkpad only if worth it. Measured before: the
  watcher at ~0.15-0.18% of one CPU on both machines (0.3.1).

Checked 2026-10-13: the X1 Carbon 3rd gen sends no devd event for the
speaker mute or volume keys either (`acpi_ibm` events on, eventmask with
the volume/mute bit; listening on `/var/run/devd.pipe`, which did show
other events, while the mute key toggled `dev.acpi_ibm.0.mute` six times:
nothing). `acpi_ibm` would pass notify 0x15-0x17 on, but neither firmware
raises them. So "assume no events, switch to events once one is seen" has
nothing to learn from on either machine. The only extra signal on the X1
Carbon is the X key `XF86AudioMute` (keyboard path, grabbed by the
pulseaudio plugin); an XInput2 raw-event listener could see it, but would
tie the watcher to X11 for ~0.1% CPU on some models only.

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
