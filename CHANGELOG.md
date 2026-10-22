# Changelog

## 0.5.0 - 2026-10-22

OSC interface version 5: screens dropping out are easier to monitor.

- `/touch/event` carries a third string, its level: `notice`, `warning` or `error`.
- `/touch/device/<id>/counts` carries a sixth number, `losses`: how many times the screen
  was unplugged, dropped off USB or could not be read since the start.
- For a minute after a screen was unplugged or dropped off USB, the health is `warning`
  with "<label> was unplugged or dropped off USB N time(s) in the last minute", so even a
  drop of half a second is seen.
- The USB port check no longer runs on the thread that sends the heartbeat. While the
  kernel retried a screen it holds the hub, each check waited for it, and the heartbeat,
  status and commands stopped for 2-3 s at a time (seen on the testing box with a USB
  extender's joint pulled). A check that has to wait for the kernel now counts as the
  kernel still trying, so that port reads `being set up` and is not reset; a reset asked
  for meanwhile runs as soon as the check is done.
- A USB hub the kernel does not bind again is reported as "the kernel did not take it"
  instead of an empty pair of brackets.

## 0.4.0 - 2026-10-22

OSC interface version 4: USB recovery.

- While a screen is gone, the daemon watches the USB port it was last on; when something
  is plugged in there that the kernel has given up on for 10 s (after failed attempts to
  set it up), the daemon resets that port's USB hub so the kernel tries again, backing off
  to every 300 s while it keeps failing. Nothing happens on an empty port or while the
  kernel is still trying. On by default (`usb_recovery`, settable over OSC);
  `/touch/device/<id>/usb_reset` resets on command.
- New status per device: `usb_recovery`, `usb_watched_port`, `usb_port_state`,
  `usb_resets`; new event kind `usb`; while a screen is stuck its reason says so. A reset
  that fails is a health fault until the next one works; one that left the hub unbound
  names the command that binds it again.
- The service runs as root (a USB reset needs it), with no capabilities, only the input
  and USB devices, and a read-only file system apart from its settings.

## 0.3.0 - 2026-10-21

OSC interface version 3: a receiver can restart the daemon.

- `/touch/restart`: every contact is cancelled with the new reason `restarting`, pending
  settings are written, `bye` goes out, and the same process starts again from the config
  file and the saved settings (a newly installed version takes over; systemd keeps
  watching the same process). The run number changes. Refused, with the reason, when the
  config file would be refused at the restart, so a remote restart never leaves the daemon
  stopped.

## 0.2.1 - 2026-10-21

Hardening for long, unattended running.

- A subscriber whose lease ran out while another subscriber was renewing kept getting
  touches; it no longer does.
- Settings changed over OSC are written to disk at most once a second, a change that
  changes nothing is neither saved nor reported, and a clean stop writes at once.
- Rotation, mirror, calibration and aspect apply without reopening the screen; only
  grab and the match settings reopen it.
- Events of one kind are passed on 10 at once and then one a second, with a count of the
  rest; refusals to hosts outside `allowed_ips` are capped at 20 a second; the memory of
  refused hosts and of failing destinations no longer grows, and a removed failing
  destination stops the health warning.
- Logging never waits for the journal: lines it cannot take are dropped and counted.
- A refused or unreadable settings file keeps a screen that it had turned off turned off,
  and its warning stays until the next restart.
- Text settings must be printable ASCII (a non-ASCII value could make the settings file
  unreadable at the next start).
- The disabled reason says whether the config file or a saved setting turned the screen
  off, and how to turn it back on.
- Retaking a screen from another program reads it afresh; with `grab = false`, a screen
  that goes silent with fingers down is checked for another program holding it.
- The device's clock is set before anything is queued (no false "events dropped"
  warning when a screen is opened mid-touch), a permission refusal is reported only if it
  lasts a second (udev applies the group a moment after the node appears), a reader that
  hits an internal error cancels its contacts and starts again, and a failing wait no
  longer loops.
- Status goes out in bundles and only when something changed; the once-a-second work
  follows the clock, not timer wakeups.
- OSC packets up to 64 KB are read whole; frame numbers and contact ids wrap to 1.
- The service restarts without limit.
- `log.level = "debug"` logs every command and lease renewal.

## 0.2.0 - 2026-10-21

OSC interface version 2: the daemon's health in a receiver's app, and its settings
changed from there.

- New status: uptime (with each alive), host name, config and settings file paths, log
  level, allowed hosts, saved destinations, which settings are saved overrides, and per
  device the usb id, serial and port it matches, its calibration and whether it grabs.
- Settings commands at the same addresses as their status: label, on/off, rotation,
  mirror, calibration, aspect, silence, grab and the three match settings per device; the
  log level; adding and removing allowed hosts and saved destinations; `settings/forget`
  for one setting or all of them. Checked with the config's own rules and words; a change
  that would lock out the sending host, or every host, is refused.
- Changes are saved in the daemon's own settings file (`[paths] settings`, by default
  `/var/lib/touch-to-osc/settings.toml`) and win over the config file at every start; the
  config file is never written. A settings file that would be refused is set aside as
  `settings.toml.refused` and reported in the health. `--check-config` checks it too.
- A change to how a screen is read cancels its contacts with the new reason
  `reconfigured` and reopens it; its state reads `restarting` meanwhile. Label, on/off,
  silence and the lists apply without a restart.
- Turning a device off over OSC is now saved like every other setting.
- The service has a state directory for the settings file.

## 0.1.0 - 2026-10-21

First version.

- Reads multitouch (and single-touch) USB touchscreens through libevdev and sends each
  changed frame as one OSC bundle of `down`, `move`, `up` and `cancel` messages with
  positions on the screen's display area (0..1), the daemon's own contact ids, a frame number and the
  screen's time stamp.
- Every announced contact ends with exactly one `up` or `cancel`: on unplug, on a read
  error, when the kernel releases the device, when the screen goes silent with fingers
  down (`silence_ms`, 200 by default), when the kernel drops events, on a disable command
  and on stopping. Fingers already down when reading starts are not announced.
- `alive` every second with the list of contacts down per device, tagged with the frame it
  is valid for; `bye` on a clean stop.
- Subscriptions (leases), saved destinations, the status (sent in full, on change and
  every 10 s), counters with each alive, events, and commands in requests with kept
  replies: `ping`, `status/send`, `device/<id>/enabled`.
- Several screens at once, told apart by USB id, serial or port; hotplug; exclusive grab
  (a screen held by another program is reported as a fault).
- Rotation, mirror and an optional affine calibration per device.
- `--list`, `--dump`, `--check-config`, a strict TOML config, a hardened systemd unit and
  install scripts, `tools/watch.py`.
