# touch-to-osc

A small Linux daemon that reads USB touchscreens (kernel evdev, HID multitouch) and sends
every touch as OSC over UDP. It reports its own health over OSC as well: a heartbeat, a
status that receivers subscribe to, and an answer to every command.

It does not know what the touches are for. Positions go out as 0..1 fractions of the
screen's display area; the receiver decides what a touch at that place means, including
where a picture shown on the screen sits.

## What it guarantees

- Every contact it announces ends with exactly one `up` or `cancel`, which it sends itself.
  A contact never outlives its device, its run or the process: unplugging the screen, the
  screen going silent, a disable command, the kernel dropping events, a USB reset and
  stopping or restarting the daemon all cancel the contacts that were down.
- Every second it sends the list of contacts still down on each device, so a receiver that
  lost an `up` (UDP can lose datagrams) ends that contact within about a second.
- A finger that was already on the screen when reading started (at start, after a
  reconnect, after the kernel dropped events, after re-enabling) is never announced. It
  has to lift first; its next touch is a new contact.
- A `cancel` is never a tap. The kernel's own release of a device that is going away is
  read as the device being lost, never as a finger lifting.
- No command, file or signal creates a touch. A settings change that alters how a screen
  is read (its alignment, its exclusive hold, which device it matches) cancels that
  screen's contacts and reads it again from scratch.
- Reading a touch never waits on the network, the disk or the status work: each device has
  its own reader thread that sends without blocking (a full socket drops the datagram and
  counts it).

## Requirements

Linux with evdev, libevdev, a C++17 compiler, CMake 3.18 or newer and Ninja. Built and
tested on Debian 11 (kernel 5.10, g++ 10, libevdev 1.11):

    apt install build-essential cmake ninja-build pkg-config libevdev-dev

`evtest` and `evemu-tools` are useful for looking at a screen and recording it. The tests
also need `python3`.

## Build and install

    cmake -S . -B build -G Ninja
    ninja -C build
    build/tto_tests
    sudo scripts/install.sh
    sudo systemctl enable --now touch-to-osc

`install.sh` installs `/usr/local/bin/touch-to-osc`, writes
`/etc/touch-to-osc/touch-to-osc.toml` from `config.example.toml` if there is none yet,
checks it, and installs the systemd unit. Settings changed over OSC are saved in
`/var/lib/touch-to-osc/settings.toml`. `scripts/uninstall.sh` removes the program and the
unit again (`--purge` also removes the config and the saved settings).

The service runs as root, which it needs to reset a USB bus (see USB recovery below), but
systemd takes every capability away from it, lets it reach no devices but the input and
USB ones, and keeps the file system read-only apart from its settings. It restarts after
a failure (every second, without giving up), stays stopped when the config is refused (fix
it, then `systemctl restart touch-to-osc`), and reports its version, run and settings in
the journal when it starts:

    journalctl -u touch-to-osc

## Configuration

`config.example.toml` documents every key with its default. In short:

- `[osc]`: the UDP port that takes commands and subscriptions (`listen`, 9480 by default),
  the address `prefix` (`/touch`), and `allowed_ips`, the hosts that may subscribe and
  send commands. Add the receiver's machine there.
- `[[destination]]` (optional, none by default): receivers that get everything without
  subscribing.
- `[[device]]`, one per screen: an `id` that becomes part of the addresses, which device
  it is (`usb` vendor:product, `serial`, `port`), how its touch panel lines up with the
  screen's display (`rotation`, `mirror`, `calibration`), and the display's `aspect`.
  `silence_ms` cancels held contacts when the screen stops reporting; see the example
  config before changing it. `usb_recovery` resets a USB connection the kernel gave up on
  (below).

Unknown keys, wrong types and out-of-range values are refused with a message that names
the key. `touch-to-osc --check-config=FILE` checks a file without opening anything.

Settings can also be changed over OSC (see Commands below), for example from Bridge
Studio. Those changes are saved in the daemon's own settings file, `[paths] settings`
(`/var/lib/touch-to-osc/settings.toml` by default), and win over the config file at every
start; the config file itself is never written. The status lists which settings come from
there (`/touch/overrides`), and `/touch/settings/forget` puts one or all of them back to
the config file's values. Changes are written within about a second; several changes in
quick succession are written once, and a clean stop writes at once. A settings file the
daemon refuses or cannot read at start is renamed to `settings.toml.refused` and reported
in the health until the next restart, and the daemon runs on the config file, except that
a screen the file had turned off stays off. `--check-config` checks the settings file too.

Text settings (labels, serials, ports) may only use printable ASCII.

With `grab = false` and `silence_ms = 0`, the daemon cannot notice another program taking
the screen exclusively while a finger is down; with `silence_ms` on it cancels that
contact and reports the screen as held by another program.

### USB recovery

A USB connection that falters (a loose joint, a marginal extender) can leave a screen
plugged in that the kernel tried to set up, failed, and then gave up on: it does not try
again until the plug is pulled and pushed back. With `usb_recovery = true` (the default),
the daemon watches the USB port it last read a screen on, or the configured `port`, while
that screen is gone. When the port reports something plugged in that the kernel has given
up on (the port is connected but switched off, with no device set up on it) for 10 s, it
resets the USB hub that port belongs to (unbinds and binds the kernel's hub driver), which
makes the kernel try again. While the kernel is still trying (the port is switched on but
the device is not set up yet), the daemon waits. For a port on the computer itself that hub
is the whole USB bus, so every device on that bus drops for a moment; contacts on other
screens there are cancelled like any unplug. If the screen does not come back, the next
reset waits 20 s, then 40, 80, 160 and from then on 300 s; the count starts again when the
screen is back or the port has been empty for 5 s. Nothing is reset while the port is
empty (the screen unplugged on purpose). Each reset is an event and a journal line, and
`/touch/device/<id>/usb_reset` resets on command. A port whose state cannot be read (no
`/dev/bus/usb`, or the daemon run by hand as a user other than root) is reported once and
left alone.

Several screens work side by side. Two identical screens are told apart by `port` (the
USB socket they are plugged into, as `--list` shows it) or by `serial` when theirs
differ.

## Command line

    touch-to-osc [--config=FILE]            run (default /etc/touch-to-osc/touch-to-osc.toml)
    touch-to-osc --check-config[=FILE]      check a config and its saved settings, exit 0 (good) or 2 (refused)
    touch-to-osc --list [--config=FILE]     every input device and what the config makes of it
    touch-to-osc --dump[=ID] [--config=FILE]  one line per touch frame of a configured device
    touch-to-osc --version

`--list` is the first thing to run with a new screen: it shows each input device's USB id,
serial and port, whether it is a touchscreen (contacts, axis ranges, size), and which
`[[device]]` would read it.

`--dump` prints each frame with the kernel time, every contact's raw position and where it
lands on the display. Touch the four corners to check `rotation` and `mirror`. The service
holds the screen exclusively, so stop it first (`systemctl stop touch-to-osc`); `--dump`
says so if it finds the screen held.

To record a screen for the tests, stop the service and use evemu:

    evemu-record /dev/input/by-id/usb-QDtech_MPI1168-event-if00 > trace.evemu

Exit codes: 0 clean stop, 1 wrong command line, 2 config refused, 3 the listen port cannot
be bound, 4 internal error.

## OSC interface (osc_api 4)

All addresses start with the prefix (`/touch` here) and devices appear by their config
`id` (`face` here). Number arguments in commands may be `i`, `h` or a whole `f`; the
daemon always sends the types written below. Strings are ASCII.

### Touches

One bundle per screen frame that changed something, immediate time tag, never split; it
holds one message per contact that changed:

    /touch/face/down    i contact  f x  f y  i frame  h t_us
    /touch/face/move    i contact  f x  f y  i frame  h t_us
    /touch/face/up      i contact  f x  f y  i frame  h t_us
    /touch/face/cancel  i contact  s reason  i frame  h t_us

- `contact`: the daemon's own id, counting up from 1 within a run and never reused, so a
  quick second tap is always a new contact.
- `x`, `y`: 0..1 of the screen's display area: origin at its top left, x to the right,
  y down, after rotation, mirror and calibration, clamped to 0..1. `up` carries where the
  finger lifted. Where a picture sits inside that area (a scaled or letterboxed video
  feed, say) is the receiver's to work out: calibration lines the touch panel up with the
  display, never with a picture on it.
- `frame`: the number of this bundle on this device, counting from 1 within a run. Every
  message in one bundle has the same number, and the next bundle has the next number, so
  a gap means a bundle was lost. Screen frames in which nothing changed send nothing and
  use no number. After 2147483647 it starts again at 1 (years of touching); contact ids
  do the same.
- `t_us`: when the screen reported the frame, as Unix time in microseconds (the moment of
  sending for cancels the daemon makes itself). With synced clocks a receiver can measure
  the whole path.
- `move` is sent whenever the position changed by at least one device unit.
- `cancel` reasons: `device lost` (unplugged, a read error, the kernel released the
  device), `device silent` (the screen sent nothing for `silence_ms` with the contact
  down), `device disabled` (a command), `resync` (the kernel dropped events because they
  were not read in time; the state was read again), `palm` (the screen marked the contact
  as a palm), `reconfigured` (a settings change restarts reading the screen), `stopping`
  (the daemon is stopping), `restarting` (the daemon is restarting on a command).

### Liveness

    /touch/subscribe    i port [i seconds]   -> /touch/subscribed   i port  i seconds
    /touch/unsubscribe  i port               -> /touch/unsubscribed i port
    /touch/alive        i counter  i run  s version  s health  i devices_connected  i devices_configured  i contacts_active
    /touch/device/face/active  i frame  i contact ...
    /touch/bye          i run

- A lease is kept per (sender IP, port), 1-60 s, 10 by default, at most 8 at once, in
  memory only. Renew it every few seconds. A renewal answers only `subscribed`; a new
  lease gets `subscribed`, the full status, then the event "IP:PORT added (lease N s)".
- `alive` goes out every second to every subscriber and saved destination, in one bundle
  with one `active` message per configured device. `active` lists the contacts down on
  that device as of bundle `frame` (no contacts: just the frame). `run` is a random
  non-zero number chosen at each start. `health` is `ok`, `warning` or `fault`.
- `bye` goes out once every contact has been cancelled on a clean stop.

What a receiver does with these:

1. Keep contacts per (run, device, contact).
2. End everything held for the daemon when `run` changes, when no `alive` arrives for 3 s,
   on `bye`, or when the lease is lost.
3. On each `active` list, end every contact held on that device that is missing from the
   list and whose last message had a frame number not above the list's `frame`. Ignore
   listed contacts it does not hold.
4. Ignore a `move` or `up` for a contact whose `down` it never saw (it subscribed in the
   middle of a touch).
5. Never treat a `cancel` as a tap.

### Status

Sent in full on a new lease, on `status/send` and every 10 s, and each part again as soon
as it changes (the health and reasons that look back over the last minute within a
second). Status messages may arrive one by one or several to a bundle:

    /touch/osc_api                  i 4
    /touch/version                  s
    /touch/run                      i
    /touch/health                   s ok|warning|fault
    /touch/reasons                  s ...    one string per reason, in plain words
    /touch/host                     s        the machine's host name
    /touch/config                   s        path of the config file
    /touch/settings_file            s        path of the saved settings, "" when nothing is saved
    /touch/log_level                s        settable
    /touch/allowed_ips              s ...    hosts allowed to send commands ("any" first with open_to_any)
    /touch/saved_destinations       s ...    "ip:port touches status", "ip:port touches" or "ip:port status"
    /touch/overrides                s ...    the settings that come from the settings file, by name
    /touch/devices                  s id ...
    /touch/device/face/label        s        settable
    /touch/device/face/enabled      i 0|1    settable; the applied state
    /touch/device/face/connected    i 0|1
    /touch/device/face/state        s ok|not connected|disabled|no permission|grab refused|read error|not a touchscreen|restarting
    /touch/device/face/reason       s        what to do about the state
    /touch/device/face/match_usb    s        settable: the "vvvv:pppp" it matches, or ""
    /touch/device/face/match_serial s        settable
    /touch/device/face/match_port   s        settable
    /touch/device/face/calibration  f a f b f c f d f e f f   settable; no arguments when none
    /touch/device/face/grab         i 0|1    settable: whether to hold it exclusively
    /touch/device/face/usb_recovery i 0|1    settable: reset the USB connection when it fails
    /touch/device/face/usb_watched_port s    the USB port recovery watches ("" when none is known)
    /touch/device/face/usb_port_state s      while the screen is gone: nothing plugged in|being set
                                             up|plugged in but not set up|set up|unknown; ""
                                             otherwise
    /touch/device/face/usb_resets   i        USB resets done for it since the start
    /touch/device/face/name         s        the kernel's name for the device
    /touch/device/face/usb          s        "vvvv:pppp serial"
    /touch/device/face/port         s        the USB port path
    /touch/device/face/slots        i        contacts it can track at once
    /touch/device/face/size_mm      f w  f h  0 0 when the device does not say
    /touch/device/face/rotation     i 0|90|180|270   settable
    /touch/device/face/mirror       s ""|x|y|xy      settable
    /touch/device/face/aspect       f        settable: width / height of its display area
    /touch/device/face/silence_ms   i        settable; 0 when off
    /touch/device/face/grabbed      i 0|1    held exclusively by the daemon now

`restarting` lasts from a settings change that reopens the screen (`grab` or a `match_`
setting) until it has been opened again; then the state is whatever the reopen found.

With every alive, to the receivers that get status:

    /touch/uptime_s                 i        seconds since this run started
    /touch/device/face/contacts     i        down now
    /touch/device/face/oldest_s     f        age of the oldest contact down, -1 when none
    /touch/device/face/last_event_s f        seconds since the screen's last frame, -1 never
    /touch/device/face/rate_hz      f        frames per second while touched, last measured
    /touch/device/face/latency_us   i p50  i p99   screen frame to sent, last 10 s
    /touch/device/face/counts       h frames  h touches  h resyncs  h send_drops  h cancels
    /touch/subscribers              s "ip:port seconds_left" ...
    /touch/destinations             s "ip:port ok|failing" ...

`health` is `fault` when an enabled device is not working (not connected, no permission,
held by another program, read error, not a touchscreen) or the last USB reset failed, and
`warning` when a device is
turned off or restarting after a settings change, when the kernel dropped events, a
screen went silent or touch messages could not be sent in the last minute, when sending to
a destination fails, or when the settings file was refused or could not be saved.
`reasons` says which, in words that say what to do. The heartbeat is the alive's
`counter`.

### Events

    /touch/event  s kind  s text

Kinds: `device` (connected, lost, released by the kernel, two screens match, restarting
after a settings change), `resync`, `silence`, `grab` (another program let go), `enabled`
(turned off or on), `usb` (a USB reset begun, done or failed, a screen back after resets,
a port whose state cannot be read), `restart` (asked for, and by whom), `settings` (a
setting changed, with the old and new value and who sent it, or the settings file could
not be saved), `subscriber` (added, ended), `send` (a destination failing, at most once
per 10 s each, with a count). Each event is also a line
in the journal. Events of one kind from one source are passed on up to 10 at once and then
one a second; the rest are counted and reported in one event ("N more ... came too fast to
report one by one").

Logging never holds the daemon up: when the journal cannot take a line at once, the line is
dropped and counted, and the count is logged as soon as it can be. `log.level = "debug"`
adds a line for every command and every lease renewal; touches are never logged.

### Commands

    /touch/request  i seq  s address  ...args   -> /touch/reply  i seq  s ok|refused  s note

    /touch/ping [i port]                  -> /touch/pong i run
    /touch/status/send [i port]           -> the full status and the counters (at most 5 a second)
    /touch/subscribe, /touch/unsubscribe  as above
    /touch/restart                        start the daemon again (below)
    /touch/device/face/usb_reset          reset the USB hub the screen's port belongs to now
                                             (see USB recovery); refused when no port is known
                                             or while a reset runs

`restart` answers first, then cancels every contact (`restarting`), writes any pending
settings, sends `bye` and starts the same process again, which reads the config file and
the saved settings afresh and runs the program as it is installed now (so it also brings
in a new version). Leases are not kept: receivers renew as usual and get the full status
again; the `run` changes. It is refused, and the daemon keeps running, when the config
file would be refused at the restart.

Settings: each one is changed at the same address the status reports it on, and saved.

    /touch/device/face/label         s
    /touch/device/face/enabled       i 0|1      off cancels the device's contacts
    /touch/device/face/rotation      i 0|90|180|270
    /touch/device/face/mirror        s ""|x|y|xy
    /touch/device/face/calibration   f a f b f c f d f e f f, or no arguments to remove it
    /touch/device/face/aspect        f          0 takes it from the device
    /touch/device/face/silence_ms    i          0 (off) or 50-10000
    /touch/device/face/grab          i 0|1
    /touch/device/face/match_usb     s          "vvvv:pppp", or "" to match by port alone
    /touch/device/face/match_serial  s
    /touch/device/face/match_port    s
    /touch/device/face/usb_recovery  i 0|1
    /touch/log_level                 s          crit|error|warning|notice|info|debug
    /touch/allowed_ips/add           s ip
    /touch/allowed_ips/remove        s ip
    /touch/saved_destinations/add    s "ip:port" [i touches  i status]   (both 1 by default)
    /touch/saved_destinations/remove s "ip:port"
    /touch/settings/forget           [s name]   one setting, by its name in /touch/overrides,
                                                or all of them: back to the config file

- A value is checked with the config file's own rules and refused in the same words
  ("rotation must be 0, 90, 180 or 270"). Also refused: a change that would leave a device
  matching nothing (no usb and no port), and one that would lock out the host sending it
  or every host (`allowed_ips`).
- The status shows a new value once it is in effect, and every change is a `settings`
  event and a journal line.
- `label`, `enabled`, `silence_ms`, `aspect`, the log level and the lists apply at once.
  `rotation`, `mirror` and `calibration` apply at once too, but cancel the device's
  contacts (`reconfigured`), since positions jump. `grab` and the three `match_` settings
  reopen the device: its contacts are cancelled `reconfigured` and its state reads
  `restarting` until it is open again. Either way, fingers still on it are not announced
  until they lift.
- A value set back to what the config file says stops being an override, and a command
  that changes nothing is answered `ok` without being saved or reported.
- `osc.listen`, `osc.prefix`, `open_to_any` and adding or removing devices stay in the
  config file; after editing it, `/touch/restart` applies it.

- Every command can go inside a request and then gets exactly one reply. Replies are kept
  per (IP, port, seq) for 30 s or 64 newer requests: an exact repeat gets the kept reply
  and is not run again; the same seq with another command is refused ("seq used for
  another command").
- A bare command works too; a refusal then answers `/touch/error s address s note`.
- Commands and subscriptions from a host outside `osc.allowed_ips` are answered with
  `/touch/error` saying so (at most once per 5 s per host, and at most 20 such answers a
  second in all) and otherwise ignored.
- There is deliberately no command that creates a touch or writes the config file.

## Watching it

`tools/watch.py` subscribes exactly as a receiver should and prints every touch with its
delay, every status change and event, and the alive. It says when the daemon is lost,
restarted or stopped:

    python3 tools/watch.py --target 192.168.1.246:9480 --no-alive

The machine running it must be in `osc.allowed_ips`.

## Tests

    build/tto_tests                          unit tests
    sudo python3 tests/e2e/run.py [--quick]  end-to-end tests
    sudo python3 tests/hw/usb_recovery.py    USB recovery on a real screen (below)

The end-to-end tests start the daemon against virtual touchscreens made through
`/dev/uinput` and a receiver that subscribes like a real one. They replay touches recorded
on a real screen (`tests/traces`), unplug screens in the middle of a touch, stop and
freeze the daemon, make the kernel drop events, and check every command. They need root,
`/dev/uinput` and `python3`, use UDP ports 19480-19499, and give their virtual screens USB
vendor id 7470 so a running service never takes them. `--quick` skips the three tests that
take longer than 10 s.

`tests/hw/usb_recovery.py` needs a real USB touchscreen (`--usb vendor:product`). It stops
the touch-to-osc service while it runs and starts its own daemon, or with `--service <id>`
tests that device of the running service (127.0.0.1 must be in its `allowed_ips`). It makes
the kernel drop the screen without unplugging it (`/sys/bus/usb/devices/<port>/remove`,
which leaves the port as a failed enumeration does), and checks a reset on command, the
automatic reset after 10 s, and that nothing is reset with `usb_recovery` off. Every device
on the screen's USB bus drops several times; at the end the hub is bound, the screen set
up and `usb_recovery` on again either way.

Sanitizer builds: `cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug
-DTTO_SANITIZE=address,undefined` (or `thread` in `build-tsan`), then run the tests with
`TTO_BUILD=build-asan`.

## License

GPL-3.0, see LICENSE.
