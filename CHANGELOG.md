# Changelog

## 0.1.0 - 2026-10-21

First version.

- Reads multitouch (and single-touch) USB touchscreens through libevdev and sends each
  changed frame as one OSC bundle of `down`, `move`, `up` and `cancel` messages with
  positions in the picture (0..1), the daemon's own contact ids, a frame number and the
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
