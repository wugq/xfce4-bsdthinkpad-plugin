# How it fits together

What each part does, who runs it, and what happens on each key. For
installing and configuring, see the README.

## The parts

```
 desktop (the user) ─────────────────────────────────────────────────────────
   bsdthinkpad-plugin        bsdthinkpad (watcher)         PulseAudio and its
   panel: sliders, switches  notifications, mute LEDs,     panel plugin: volume,
                             PulseAudio sink mute          the speaker mute popup
        │ pkexec                   │ pkexec                       │ OSS ioctl
 root ──┼──────────────────────────┼──────────────────────────────┼──────────────
   bsdthinkpad-mute-led: sets a mute LED, nothing else    devd: kernel events,
   bsdthinkpad-key: run by the devd rule (mic-mute key)    rules, broadcast ▲
 kernel ───────────────────────────────────────────────────────────────────┼────
   acpi_ibm(4): dev.acpi_ibm.0.mute,      acpi_call    backlight(9)   OSS   │
   evdev keys, ACPI notify → devd                                  mixers   │
   tpmute (optional kmod, separate project): polls the EC mute ────────────┘
 firmware / hardware ─────────────────────────────────────────────────────────
   embedded controller (EC): handles the mute and volume keys itself
   BIOS: ACPI methods SSMS/GSMS → SMI → AUDC (audio mute), mute LEDs
   backlight, sound card
```

| Part | Runs as | Job |
|---|---|---|
| `bsdthinkpad` (watcher) | the user, from autostart | watches brightness, speaker mute, microphone mute; on a change: notification, LEDs, PulseAudio sink |
| `bsdthinkpad-plugin` | the user, in the panel | the controls; `ctl.c` holds the state and requests, `hw.c` touches the hardware (shared with the watcher) |
| `bsdthinkpad-mute-led` | root, via pkexec + polkit | the only root part; sets a mute LED and accepts nothing else |
| devd rule + `bsdthinkpad-key` | root | the mic-mute key arrives only as an ACPI event; mutes the microphones |
| tpmute (optional) | kernel | turns a change of the EC speaker mute into a devd event |
| PulseAudio + its plugin | the user | not part of this project: volume, and the speaker mute popup |

## Rules the design keeps

1. **Only the helper is root**, and it can only set LEDs.
2. **The watcher follows, it never changes a state.** The EC owns the
   speaker mute; the watcher makes the LED, the PulseAudio sink and the
   notifications follow it. (It used to write the mute back through `SSMS`,
   and quick presses of the key then ended in the wrong state.)
3. **System configuration is the user's** (`rc.conf`, `loader.conf`,
   `sysctl.conf`); the programs only check it.
4. **Where the hardware tells nobody, the watcher looks.** tpmute and the
   nudges below let it look less often; without them it polls, and all
   works the same.

## The keys

**Speaker mute** (the hard one)
1. The EC mutes the speaker and flips its mute bit. Nothing is told: no
   ACPI notify on the A475 or the X1 Carbon. (The X1 Carbon's EC also sends
   an `XF86AudioMute` key to X, which the PulseAudio plugin handles; the
   A475's does not.)
2. The watcher sees it: by tpmute's devd event
   (`system=ACPI subsystem=IBM type=MUTE`), or else by its 200 ms poll.
3. At once it sets the PulseAudio sink mute to the EC's; the PulseAudio
   plugin sees the sink change and shows its popup. If the plugin already
   toggled the sink (X1 Carbon) nothing changes and no second popup comes.
4. Once the mute has stayed put for 0.5 s, the watcher runs
   `pkexec bsdthinkpad-mute-led speaker sync`, which calls `GSMS`: on the
   A475 the firmware sets the LED to its own mute, without changing it.
   (Calling the firmware while the key was pressed quickly left the
   firmware's mute and the EC's apart.)

**Microphone mute**
1. The firmware raises ACPI notify `0x1b`; `acpi_ibm` passes it to devd.
2. The devd rule runs `bsdthinkpad-key micmute` as root: the recording
   level of every mixer muted or unmuted, and the LED.
3. The watcher gets the same devd event (or sees it at its next poll) and
   shows "Microphone muted".

**Brightness**
1. The XFCE shortcut runs `bsdthinkpad brightness ±10` (backlight(9)).
2. The command sends the watcher `SIGWINCH`; it reads at once and shows
   the popup with a bar, then polls fast for 2 s to follow a key held down.

**Volume** is the PulseAudio plugin's (OSS `vol` and `pcm`). The EC also
unmutes the speaker on the volume keys; the watcher then unmutes the sink.

**Panel**: the sliders set the brightness or `pcm` and send `SIGWINCH`; the
speaker switch runs the helper with `SSMS` (meant to change the mute); the
microphone switch sets the recording levels and the LED.

## The speaker mute inside the firmware

What the A475's DSDT shows (`acpidump -dt`), and what follows from it. The
firmware's own code (the EC's, and the BIOS's in SMM) can not be read, so
part of this is inferred; it is marked so.

**Seen in the DSDT (A475):**

- None of the 62 EC query methods (`_Qxx`) handles the mute key (hotkey
  `0x1017` appears nowhere). Pressing it runs no ACPI code at all: the EC and
  the BIOS handle it out of the OS's sight.
- `SSMS(x)` sets the LED's GPIO (`G143`) and then calls `AUDC(1, x)`;
  `GSMS()` calls `AUDC(0, 0)`, sets `G143` from the result and returns it.
  `AUDC` is an SMI: `SMI()` takes a lock, writes `0xF5` to the APM port
  (the CPU leaves the OS for the BIOS's SMM code), and while the BIOS
  reports busy writes it again every 1 ms.
- The only other caller of `AUDC` is the wake method: after S3 the BIOS
  reads `AUDC` and sets `G143` from it. So nothing sets the LED on its own;
  even the firmware sets it from `AUDC`.

**Inferred (not visible):** the speaker mute exists twice. The EC flips
its bit (register `0x30`, bit 6, what `dev.acpi_ibm.0.mute` and tpmute
read) and cuts the amplifier at once; the BIOS's state, `AUDC`, follows a
moment later through some path of its own (most likely an SMI raised by
the EC). Until it has, `GSMS` returns the old state and an SMI of ours
competes with the firmware's.

**What that means for the watcher, as measured:**

- `SSMS` sets the mute as well as the LED, so the watcher never calls it:
  writing back a state just read undid presses of the key.
- `GSMS` called too soon after a press sets the LED to the old state:
  with tpmute polling every 50 ms the LED was often wrong; at 200 ms (plus
  pkexec) it is right. The LED can not be made faster than the firmware
  catches up.
- `GSMS` called on every change of a quick series of presses left the EC's
  mute and `AUDC` apart; so within a quick series the LED waits until the
  mute has stayed put for 0.5 s (a single press is synced at once).
- The microphone has none of this: its mute is the OS's own (the mixers'
  recording levels) and its LED has a method of its own, `MMTS`
  (`dev.acpi_ibm.0.mic_led`), that touches nothing else.

**Other models:** on the X1 Carbon 3rd gen `SSMS` and `GSMS` only call
`AUDC`; no LED GPIO is set in ACPI code, so there the LED is presumably
set inside the BIOS (SMM). Whether its LED follows the mute key by itself
is not checked yet (see BACKLOG). This is a difference of firmware between
models, not known to follow the CPU vendor: two machines say nothing about
AMD and Intel ThinkPads in general.

## When the watcher looks

All in `next_interval()` in `bsdthinkpad.c`:

- 30 ms for 2 s after a change or an event (a key held down);
- 2 s when tpmute is loaded and devd is connected: events and `SIGWINCH`
  cover the keys and the panel, the poll is only a safety net for other
  programs (`mixer`, another backlight tool), which can show up to 2 s late;
- otherwise every 200 ms (`-i`).

## Where to look in the code

| File | What |
|---|---|
| `bsdthinkpad.c` | the watcher: one `check_*()` per value, then "when to look"; the commands |
| `hw.c`, `hw.h` | every hardware access (backlight, mixers, `acpi_ibm`, the helper, `SIGWINCH`) |
| `ctl.c`, `ctl.h` | the panel plugin's state and requests, no GTK |
| `bsdthinkpad-plugin.c` | the panel plugin's UI |
| `bsdthinkpad-mute-led.c` | the root helper |
| `contrib/` | the devd rule, `bsdthinkpad-key`, `bsdthinkpad-setup` |
