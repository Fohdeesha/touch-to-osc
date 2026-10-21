# touch-to-osc

A small Linux daemon that reads USB touchscreens (kernel evdev, HID multitouch) and sends
every touch as OSC over UDP. It reports its own health over OSC as well: a heartbeat, a
status that receivers subscribe to, and an answer to every command.

It does not know what the touches are for. Positions go out as 0..1 fractions of the
picture on the screen; the receiver decides what a touch at that place means.

## What it guarantees

- Every contact it announces ends with exactly one `up` or `cancel`, which it sends itself.
  A contact never outlives its device, its run or the process: unplugging the screen, the
  screen going silent, a disable command, the kernel dropping events and stopping the
  daemon all cancel the contacts that were down.
- Every second it sends the list of contacts still down on each device, so a receiver that
  lost an `up` (UDP can lose datagrams) ends that contact within about a second.
- A finger that was already on the screen when reading started (at start, after a
  reconnect, after the kernel dropped events, after re-enabling) is never announced. It
  has to lift first; its next touch is a new contact.
- A `cancel` is never a tap. The kernel's own release of a device that is going away is
  read as the device being lost, never as a finger lifting.
- No command, file or signal creates a touch.
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
checks it, and installs the systemd unit. `scripts/uninstall.sh` removes them again
(`--purge` also removes the config).

The service runs as an unprivileged user in the `input` group, restarts after a failure,
stays stopped when the config is refused (fix it, then `systemctl restart touch-to-osc`),
and reports its version, run and settings in the journal when it starts:

    journalctl -u touch-to-osc

## Configuration

`config.example.toml` documents every key with its default. In short:

- `[osc]`: the UDP port that takes commands and subscriptions (`listen`, 9480 by default),
  the address `prefix` (`/touch`), and `allowed_ips`, the hosts that may subscribe and
  send commands. Add the receiver's machine there.
- `[[destination]]` (optional, none by default): receivers that get everything without
  subscribing.
- `[[device]]`, one per screen: an `id` that becomes part of the addresses, which device
  it is (`usb` vendor:product, `serial`, `port`), and how its touches map onto the
  picture (`rotation`, `mirror`, `calibration`, `aspect`). `silence_ms` cancels held
  contacts when the screen stops reporting; see the example config before changing it.

Unknown keys, wrong types and out-of-range values are refused with a message that names
the key. `touch-to-osc --check-config=FILE` checks a file without opening anything.

Several screens work side by side. Two identical screens are told apart by `port` (the
USB socket they are plugged into, as `--list` shows it) or by `serial` when theirs
differ.

## Command line

    touch-to-osc [--config=FILE]            run (default /etc/touch-to-osc/touch-to-osc.toml)
    touch-to-osc --check-config[=FILE]      check a config, exit 0 (good) or 2 (refused)
    touch-to-osc --list [--config=FILE]     every input device and what the config makes of it
    touch-to-osc --dump[=ID] [--config=FILE]  one line per touch frame of a configured device
    touch-to-osc --version

`--list` is the first thing to run with a new screen: it shows each input device's USB id,
serial and port, whether it is a touchscreen (contacts, axis ranges, size), and which
`[[device]]` would read it.

`--dump` prints each frame with the kernel time, every contact's raw position and where it
lands in the picture. Touch the four corners to check `rotation` and `mirror`. The service
holds the screen exclusively, so stop it first (`systemctl stop touch-to-osc`); `--dump`
says so if it finds the screen held.

To record a screen for the tests, stop the service and use evemu:

    evemu-record /dev/input/by-id/usb-QDtech_MPI1168-event-if00 > trace.evemu

Exit codes: 0 clean stop, 1 wrong command line, 2 config refused, 3 the listen port cannot
be bound, 4 internal error.

## OSC interface (osc_api 1)

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
- `x`, `y`: 0..1 in the picture as shown on that screen: origin top left, x to the right,
  y down, after rotation, mirror and calibration, clamped to 0..1. `up` carries where the
  finger lifted.
- `frame`: the number of this bundle on this device, counting from 1 within a run. Every
  message in one bundle has the same number, and the next bundle has the next number, so
  a gap means a bundle was lost. Screen frames in which nothing changed send nothing and
  use no number.
- `t_us`: when the screen reported the frame, as Unix time in microseconds (the moment of
  sending for cancels the daemon makes itself). With synced clocks a receiver can measure
  the whole path.
- `move` is sent whenever the position changed by at least one device unit.
- `cancel` reasons: `device lost` (unplugged, a read error, the kernel released the
  device), `device silent` (the screen sent nothing for `silence_ms` with the contact
  down), `device disabled` (a command), `resync` (the kernel dropped events because they
  were not read in time; the state was read again), `palm` (the screen marked the contact
  as a palm), `stopping` (the daemon is stopping).

### Liveness

    /touch/subscribe    i port [i seconds]   -> /touch/subscribed   i port  i seconds
    /touch/unsubscribe  i port               -> /touch/unsubscribed i port
    /touch/alive        i counter  i run  s version  s health  i devices_connected  i devices_configured  i contacts_active
    /touch/device/face/active  i frame  i contact ...
    /touch/bye          i run

- A lease is kept per (sender IP, port), 1-60 s, 10 by default, at most 8 at once, in
  memory only. Renew it every few seconds. A renewal answers only `subscribed`; a new
  lease gets `subscribed`, the full status, then the event "subscriber IP:PORT added".
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

Sent in full on a new lease, on `status/send` and every 10 s; any part that changes goes
out within 0.1 s:

    /touch/osc_api                  i 1
    /touch/version                  s
    /touch/run                      i
    /touch/health                   s ok|warning|fault
    /touch/reasons                  s ...    one string per reason, in plain words
    /touch/devices                  s id ...
    /touch/device/face/label        s
    /touch/device/face/enabled      i 0|1    the applied state
    /touch/device/face/connected    i 0|1
    /touch/device/face/state        s ok|not connected|disabled|no permission|grab refused|read error|not a touchscreen
    /touch/device/face/reason       s        what to do about the state
    /touch/device/face/name         s        the kernel's name for the device
    /touch/device/face/usb          s        "vvvv:pppp serial"
    /touch/device/face/port         s        the USB port path
    /touch/device/face/slots        i        contacts it can track at once
    /touch/device/face/size_mm      f w  f h  0 0 when the device does not say
    /touch/device/face/rotation     i 0|90|180|270
    /touch/device/face/mirror       s ""|x|y|xy
    /touch/device/face/aspect       f        width / height of the picture on it
    /touch/device/face/silence_ms   i        0 when off
    /touch/device/face/grabbed      i 0|1    held exclusively by the daemon

With every alive, to the receivers that get status:

    /touch/device/face/contacts     i        down now
    /touch/device/face/oldest_s     f        age of the oldest contact down, -1 when none
    /touch/device/face/last_event_s f        seconds since the screen's last frame, -1 never
    /touch/device/face/rate_hz      f        frames per second while touched, last measured
    /touch/device/face/latency_us   i p50  i p99   screen frame to sent, last 10 s
    /touch/device/face/counts       h frames  h touches  h resyncs  h send_drops  h cancels
    /touch/subscribers              s "ip:port seconds_left" ...
    /touch/destinations             s "ip:port ok|failing" ...

`health` is `fault` when an enabled device is not working (not connected, no permission,
held by another program, read error, not a touchscreen) and `warning` when a device is
turned off by a command, when the kernel dropped events, a screen went silent or touch
messages could not be sent in the last minute, or when sending to a destination fails.
`reasons` says which, in words that say what to do.

### Events

    /touch/event  s kind  s text

Kinds: `device` (connected, lost, released by the kernel, two screens match), `resync`,
`silence`, `grab` (another program let go), `enabled` (turned off or on), `subscriber`
(added, ended), `send` (a destination failing, at most once per 10 s each, with a count).
Each event is also a line in the journal.

### Commands

    /touch/request  i seq  s address  ...args   -> /touch/reply  i seq  s ok|refused  s note

    /touch/ping [i port]                  -> /touch/pong i run
    /touch/status/send [i port]           -> the full status and the counters (at most 5 a second)
    /touch/device/face/enabled  i 0|1     turn a device off (its contacts are cancelled) or on;
                                             kept until the daemon restarts
    /touch/subscribe, /touch/unsubscribe  as above

- Every command can go inside a request and then gets exactly one reply. Replies are kept
  per (IP, port, seq) for 30 s or 64 newer requests: an exact repeat gets the kept reply
  and is not run again; the same seq with another command is refused ("seq used for
  another command").
- A bare command works too; a refusal then answers `/touch/error s address s note`.
- Commands and subscriptions from a host outside `osc.allowed_ips` are answered with
  `/touch/error` saying so (at most once per 5 s per host) and otherwise ignored.
- There is deliberately no command that creates a touch or writes the config.

## Watching it

`tools/watch.py` subscribes exactly as a receiver should and prints every touch with its
delay, every status change and event, and the alive. It says when the daemon is lost,
restarted or stopped:

    python3 tools/watch.py --target 192.168.1.246:9480 --no-alive

The machine running it must be in `osc.allowed_ips`.

## Tests

    build/tto_tests                          unit tests
    sudo python3 tests/e2e/run.py [--quick]  end-to-end tests

The end-to-end tests start the daemon against virtual touchscreens made through
`/dev/uinput` and a receiver that subscribes like a real one. They replay touches recorded
on a real screen (`tests/traces`), unplug screens in the middle of a touch, stop and
freeze the daemon, make the kernel drop events, and check every command. They need root,
`/dev/uinput` and `python3`, use UDP ports 19480-19499, and give their virtual screens USB
vendor id 7470 so a running service never takes them. `--quick` skips the three tests that
take longer than 10 s.

Sanitizer builds: `cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug
-DTTO_SANITIZE=address,undefined` (or `thread` in `build-tsan`), then run the tests with
`TTO_BUILD=build-asan`.

## License

GPL-3.0, see LICENSE.
