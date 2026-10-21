# Changelog

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
