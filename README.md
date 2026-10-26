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
  screen going silent, a disable command, the kernel dropping events, a USB reset, another
  program taking the screen and stopping or restarting the daemon all cancel the contacts
  that were down.
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
  counts it). Files are written on a thread of their own, and USB checks and resets on
  another, so neither holds up the heartbeat.

## Quick start

1. Install the package (see Install, upgrade and remove):

       sudo apt install ./touch-to-osc_*_amd64.deb

2. Find the screen:

       sudo touch-to-osc --list

   It lists each touchscreen with its USB id, serial and port, which `[[device]]` block
   of the config reads it, and a block ready to paste for any screen no block reads.

3. In `/etc/touch-to-osc/touch-to-osc.toml`, set `match_usb` in the `[[device]]` block
   (and `match_port` too when two screens are the same model), and add the IP of each
   machine that runs a receiver to `osc.allowed_ips`. Then check the file:

       sudo touch-to-osc --check-config

4. Restart the service and look at it:

       sudo systemctl restart touch-to-osc
       touch-to-osc --status

5. Watch the touches and touch the four corners of the display: top left should read about
   `0 0`, top right `1 0`, bottom left `0 1`, bottom right `1 1`. If they do not, see
   Lining up the touch panel.

       touch-to-osc-watch --no-alive

## Install, upgrade and remove

Every version is released on GitHub (`Fohdeesha/touch-to-osc`, Releases) with its Debian
package attached, `touch-to-osc_<version>-1_amd64.deb`, built on Debian 11 so that the one
file installs on Debian 11 to 13 and Ubuntu 22.04 and 24.04. Download it (for example
`gh release download -R Fohdeesha/touch-to-osc -p '*.deb'` for the newest) and install it
on each machine:

    sudo apt install ./touch-to-osc_*_amd64.deb

The package installs `/usr/bin/touch-to-osc`, `/usr/bin/touch-to-osc-watch` and the
systemd unit, writes `/etc/touch-to-osc/touch-to-osc.toml` from the example in
`/usr/share/touch-to-osc` if there is none yet, checks it, and enables and starts the
service.

- **Upgrading:** installing a newer package keeps the config and the saved settings and
  restarts the service if it is running, which cancels the contacts that are down and
  starts a new run: install between shows. When the new version refuses the config, the
  service is not restarted and keeps running the old version until it stops; the install
  says so. Fix the file, then `sudo systemctl restart touch-to-osc`.
- **Removing:** `sudo apt remove touch-to-osc` stops and removes the program and keeps the
  config and the saved settings; installing it again later enables and starts it again if
  it was enabled before. `sudo apt purge touch-to-osc` removes the config and the saved
  settings too.
- **Going back to an older version:** 0.5.1 and older refuse `match_usb`, `match_serial`,
  `match_port` and `[paths] state` in the config (use `usb`, `serial` and `port` and drop
  `state`) and a settings file written by 0.6.0 or later; 0.6.0 refuses a destination's
  `plain` and a settings file that holds a listen address, a prefix or devices added or
  removed over OSC. A refused settings file is set aside and the daemon runs on the config
  file.

The service runs as root, which it needs to reset a USB bus (see USB recovery), but systemd
takes every capability away from it, lets it reach no devices but the input and USB ones,
and keeps the file system read-only apart from `/var/lib/touch-to-osc`. It restarts after
a failure (every second, without giving up). When the config file is refused at a start,
it runs on its copy of the last config that started, with a health warning that says why
the file is refused; with no such copy (a first install) it stays stopped until the file
is fixed and `sudo systemctl restart touch-to-osc`. It reports its version, run, allowed
hosts and settings in the journal when it starts:

    journalctl -u touch-to-osc

A copy installed before with `scripts/install.sh` is replaced: the package removes that
program and its unit. To build the package or install from the source tree, see Building.

## Configuration

`config.example.toml` documents every key with its default. In short:

- `[osc]`: the UDP port that takes commands and subscriptions (`listen`, 9480 by default),
  the address `prefix` (`/touch`), and `allowed_ips`, the hosts that may subscribe and
  send commands. Add the receiver's machine there.
- `[[destination]]` (optional, none by default): receivers that get everything without
  subscribing (`plain = true` for receivers that cannot take 64-bit arguments).
- `[[device]]`, one per screen: an `id` that becomes part of the addresses, which screen
  it reads (`match_usb` vendor:product, `match_serial`, `match_port`), how its touch panel
  lines up with the screen's display (`rotation`, `mirror`, `calibration`), and the
  display's `aspect`. `silence_ms` cancels held contacts when the screen stops reporting;
  see the example config before changing it. `usb_recovery` resets a USB connection the
  kernel gave up on (see USB recovery).
- `[paths]`: where settings changed over OSC are saved (`settings`) and what the daemon
  keeps between runs (`state`), both under `/var/lib/touch-to-osc`.

Unknown keys, wrong types and out-of-range values are refused with a message that names the
key and the device (`device "face" (the 1st [[device]]): rotation: must be 0, 90, 180 or
270`), and an unknown key that looks like a known one is named (`rotaton: unknown key (did
you mean rotation?)`). `sudo touch-to-osc --check-config` checks the file and its saved
settings without opening anything.

The same setting has the same name everywhere, except the config file's older names,
which still work:

| What | Config file | OSC and the settings file | Status |
|---|---|---|---|
| which screen to read | `match_usb`, `match_serial`, `match_port` (or `usb`, `serial`, `port`) | `device/<id>/match_usb` and so on | the same addresses |
| the screen read now | - | - | `device/<id>/usb`, `port`, `name` (cannot be set) |
| log level | `[log] level` | `log_level` | `log_level` |
| hosts allowed | `[osc] allowed_ips` | `allowed_ips/add`, `allowed_ips/remove` | `allowed_ips` |
| receivers without a lease | `[[destination]]` | `saved_destinations/add`, `/remove` | `saved_destinations` |
| where commands arrive | `[osc] listen`, `prefix` | `listen`, `prefix` (applied by a restart) | `listen`, `prefix` |
| the devices | `[[device]]` blocks | `devices/add`, `devices/remove` (applied by a restart) | `devices` |

### Settings changed over OSC

Settings can also be changed over OSC (see Commands), for example from Bridge Studio. Those
changes are saved in the daemon's own settings file, `[paths] settings`
(`/var/lib/touch-to-osc/settings.toml` by default), and win over the config file; the config
file itself is never written. The settings file also records what the config file said
when each setting was changed, so an edit made to the config file afterwards is not lost:
when the config file's value for a saved setting has changed since, the config file's new
value applies from the next start and the saved one is dropped, with a line in the
journal saying which. Allowed hosts and saved destinations added or removed over OSC are
kept as changes on top of the config file's lists, so a host added to the config file
later is allowed even though the list was changed over OSC.

To see and drop them from a shell:

    sudo touch-to-osc --check-config     which settings are saved, and whether the file is good
    touch-to-osc --status                the same from the running service
    touch-to-osc --send settings/forget device/face/rotation    one setting
    touch-to-osc --send settings/forget device/face             every saved setting of one device
    touch-to-osc --send settings/forget                         all of them

The listen address, the prefix and the device list can be changed over OSC too
(`listen`, `prefix`, `devices/add`, `devices/remove`); they are saved like the rest and
applied by a restart the daemon does itself right after answering, as is forgetting
them.

Changes are written within about a second; several changes in quick succession are written
once, and a clean stop writes at once. A settings file the daemon refuses or cannot read
at start is renamed to `settings.toml.refused` and reported in the health until the next
restart, and the daemon runs on the config file, except that a screen the file had turned
off stays off. Saved settings for a device the config no longer has (its id renamed, say)
are kept but not used, and reported at start: put the id back, or forget them.

Text settings (labels, serials, ports) may only use printable ASCII.

With `grab = false` and `silence_ms = 0`, the daemon cannot notice another program taking
the screen exclusively while a finger is down; with `silence_ms` on it cancels that
contact and reports the screen as held by another program.

## Several screens

Give each screen its own `[[device]]` block and id. Two screens of the same model have
the same `match_usb`, so tell them apart by `match_port`, the USB socket each is plugged
into as `--list` shows it (or by `match_serial`, if theirs differ). Ports follow the
socket, not the screen: label the sockets and the cables.

When blocks can still read the same screens, the daemon shares them out the same way at
every start: blocks that say more (a port, then a serial, then a usb id) choose first, the
rest in config order, and each takes the first free matching screen in USB port order.
Moving a cable to another socket can still swap them, so `--check-config` and the journal
warn about blocks that cannot be told apart, and the health stays at `warning` while one
of them matches several screens. A block whose port has no screen but whose screen is
plugged in elsewhere says where it is ("a 0712:0009 touchscreen is plugged into port 3-4,
not 3-3").

To see which physical screen is which:

- For the screens the config reads: `touch-to-osc --send identify 30` pauses every
  screen's touches for 30 s and reports which screen each touch lands on, and
  `touch-to-osc-watch` prints them (`identified face front 3-3 0.42 0.61`). It ends by
  itself; `--send identify 0` ends it at once.
- For screens no block reads: `sudo touch-to-osc --list --touch` waits for a touch on any
  of them and prints the one touched with a block to paste.
- `--list` and the status's `unused_screens` list the touchscreens no block reads, and an
  event says when one is plugged in. `touch-to-osc --send devices/add screen2 0712:0009 3-4`
  adds a device for one over OSC (the daemon restarts to read it).

## Lining up the touch panel

Touch positions come out as 0..1 of the display, top left `0 0`. To check them, run
`touch-to-osc-watch --no-alive` (on the screen's machine, or on another with `--target
IP:9480` if that machine is in `allowed_ips`) and touch each corner of the display.

1. **Rotation and mirror** first. `rotation` turns positions clockwise by 0, 90, 180 or 270
   degrees, then `mirror` flips them (`x` left-right, `y` top-bottom, `xy` both). Both apply
   at once over OSC, so try them while watching:

       touch-to-osc --send device/face/rotation 90
       touch-to-osc --send device/face/mirror x

   Touching the top right corner should read about `1 0`: reading `0 0` means set
   `mirror = "x"`, `1 1` means `mirror = "y"`, `0 1` means `rotation = 180`. When moving
   the finger right moves the reading up or down, x and y are swapped: try `rotation`
   90 or 270, then the mirrors.

2. **Calibration** then corrects a panel that is offset or a little too large or small.
   An app can do it for you with guided calibration (see Guided calibration): it pauses the
   screen's touches, shows targets and has the daemon fit and save the numbers. By hand:
   readings are clamped to 0..1, which hides errors at the edges, so measure on targets
   about 10% in from them. Remove any calibration first, so the readings are the
   uncorrected ones:

       touch-to-osc --send device/face/calibration

   Show (or mark) two targets at x = 0.1 and x = 0.9 and write down the x each one reads,
   `u1` and `u2`; do the same for y with targets at y = 0.1 and y = 0.9 (`v1`, `v2`). Then

       a = (0.9 - 0.1) / (u2 - u1)    c = 0.1 - a * u1
       e = (0.9 - 0.1) / (v2 - v1)    f = 0.1 - e * v1

   and the calibration is `[a, 0, c, 0, e, f]`. For example, x targets reading 0.15 and
   0.93 give `a = 1.0256`, `c = -0.0538`:

       touch-to-osc --send device/face/calibration 1.0256 0 -0.0538 0 1 0

   The six numbers are `[a, b, c, d, e, f]` with `x' = a*x + b*y + c` and
   `y' = d*x + e*y + f`; `b` and `d` correct a panel that is twisted, which is rare.
   Calibration lines the panel up with the display, never with a picture scaled or
   letterboxed on it: that is the receiver's job.

3. **Aspect**: width / height of the display, sent in the status so receivers can keep
   circles round. It is worked out from the screen's size; a screen that does not report
   its size gets a guess from its units, which the journal says. Set `aspect` then (1.778
   for 16:9).

Settings sent over OSC are saved (see Settings changed over OSC); copy the final values
into the config file if you want them there too. `--dump` shows the raw device units of
each touch as well (stop the service first).

## USB recovery

A USB connection that falters (a loose joint, a marginal extender) can leave a screen
plugged in that the kernel tried to set up, failed, and then gave up on: it does not try
again until the plug is pulled and pushed back. With `usb_recovery = true` (the default),
the daemon watches the USB port it last read a screen on (remembered across restarts in
the state file), or the configured `match_port`, while that screen is gone. When the port
reports something plugged in that the kernel has given up on (the port is connected but
switched off, with no device set up on it) for 10 s, it resets the USB hub that port
belongs to (unbinds and binds the kernel's hub driver), which makes the kernel try again.
While the kernel is still trying (the port is switched on but the device is not set up
yet), the daemon waits. The kernel holds the hub while it tries, so a check of the port
that has to wait for the kernel also means it is still trying; the checks and resets run
beside the rest of the daemon and never hold up the heartbeat, the status or a command.

For a port on the computer itself that hub is the whole USB bus, so every device on that
bus drops for a moment; contacts on other screens there are cancelled like any unplug.
`sudo touch-to-osc --list` says what a reset for each screen resets and what else is on
it, and the event of a finished reset names what it dropped. If the screen does not come
back, the next reset waits 20 s, then 40, 80, 160 and from then on 300 s, for every screen
on that hub; the count starts again when the screen is back or the port has been empty for
5 s. Nothing is reset while the port is empty (the screen unplugged on purpose). Each reset
is an event and a journal line, and `/touch/device/<id>/usb_reset` resets on command. A port
whose state cannot be read (no `/dev/bus/usb`, or the daemon run by hand as a user other
than root) is reported once and left alone.

A stop or restart waits for a reset in flight (up to 30 s, while telling systemd it is
still alive), and a reset cut short anyway (a crash, a kill) leaves a note in the state
file: at the next start the daemon binds that hub again first.

## Troubleshooting

`touch-to-osc --status` shows the health and its reasons, which say what to do; the
journal (`journalctl -u touch-to-osc`) has the same and more.

| What you see | What to do |
|---|---|
| No touches arrive | `touch-to-osc --status`: the device's state and reason; then `sudo touch-to-osc --list` |
| `may not send commands here` | add the receiver's IP to `osc.allowed_ips`, or `--send allowed_ips/add IP` from an allowed host |
| An edit to the config does nothing | `touch-to-osc --status` lists the settings saved over OSC that win over it; restart after editing, or forget them |
| A finger held still ends after 200 ms | the screen reports only while a finger moves: `silence_ms = 0` for it (the journal suggests it when it sees this) |
| Touches rotated, mirrored or offset | see Lining up the touch panel |
| Two screens swapped | set `match_port` for each (see Several screens) |
| `--dump` shows nothing | the service holds the screen: `sudo systemctl stop touch-to-osc` first, and start it after |
| Other USB devices drop for a moment | a USB recovery reset: the journal names what it dropped; set `usb_recovery = false` or move the screen to its own bus |
| The health stays at `warning` after a start | the config file is refused and it runs on the last copy that worked, or the saved settings were refused: the reason says which |
| Nothing answers a tool | answers go to the IP and port a command came from: the tool must send and listen on the same port |

## Command line

    touch-to-osc [--config=FILE]               run (default /etc/touch-to-osc/touch-to-osc.toml)
    touch-to-osc --check-config[=FILE]         check a config and its saved settings, and exit
    touch-to-osc --list [--all] [--config=FILE]  the touchscreens and what the config makes of them
    touch-to-osc --list --touch[=SECONDS]      wait for a touch on a screen no device reads (30 s)
    touch-to-osc --dump[=ID] [--config=FILE]   one line per touch frame of a configured device
    touch-to-osc --status [--config=FILE]      the running service's health, screens and settings
    touch-to-osc [--config=FILE] --send ADDRESS [VALUE...]   one command to the running service
    touch-to-osc --version | --help

`--config FILE` (with a space) works too.

`--list` is the first thing to run with a new screen. For each touchscreen it shows its USB
id, serial and port, its contacts, axis ranges and size, which `[[device]]` reads it (the
same way the daemon shares screens out) and what a USB reset for it would reset; for a
screen no block reads, a block to paste. `--all` lists every other input device too
(keyboards, buttons).

`--status` and `--send` talk to the running service over OSC from this machine, so
127.0.0.1 must be in `osc.allowed_ips` (it is by default). `--send` takes an address with
or without the prefix and sends it as a request, so it gets exactly one answer, `ok` or
the reason it was refused:

    touch-to-osc --send device/face/rotation 90
    touch-to-osc --send device/face/label "front screen"
    touch-to-osc --send device/face/label s:2        (s: sends a number as text)
    touch-to-osc --send restart

`--dump` prints each frame with the kernel time, every contact's raw position and where it
lands on the display. The service holds the screen exclusively, so stop it first
(`sudo systemctl stop touch-to-osc`); `--dump` says so if it finds the screen held.

To record a screen for the tests, stop the service and use evemu:

    evemu-record /dev/input/by-id/usb-QDtech_MPI1168-event-if00 > trace.evemu

Exit codes: 0 good or a clean stop, 1 wrong command line (for `--dump`: the screen is not
there or cannot be read), 2 config refused, 3 the listen port cannot be used (most likely
the service already runs: stop it first), 4 internal error, 5 (`--check-config`) the config
is good but its saved settings are refused, 6 (`--status`, `--send`) no answer, or the
command was refused.

## OSC interface (osc_api 6)

All addresses start with the prefix (`/touch` here) and devices appear by their config
`id` (`face` here). Number arguments in commands may be `i`, `h`, `d` or a whole `f`; `T`
and `F` count as 1 and 0. A command with an argument of another type (a blob, say) is
answered with `/touch/error` naming it. The daemon always sends the types written below
(see Plain types for receivers that cannot take `h`). Strings are ASCII. Everything in
osc_api 5 is still sent as it was; osc_api 6 only adds addresses and appends arguments,
so a receiver that ignores what it does not know keeps working.

Commands and subscriptions go to `osc.listen` (UDP 9480 by default). Everything the daemon
sends comes from other ports of its machine, so a receiver behind a firewall must let in
UDP from that machine to the receiver's own port. Replies, errors, `pong` and `here` go to
the IP and port the command came from (or the port given to `ping`, `hello`, `describe`
and `status/send`), and also to every port that IP holds a lease on, each port once.

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
  device, another program had held it), `device silent` (the screen sent nothing for
  `silence_ms` with the contact down), `device disabled` (a command), `resync` (the kernel
  dropped events because they were not read in time; the state was read again), `palm`
  (the screen marked the contact as a palm), `reconfigured` (a settings change restarts
  reading the screen), `identifying` and `calibrating` (touches pause, see Identify screens
  and Guided calibration), `stopping` (the daemon is stopping), `restarting` (the daemon is
  restarting on a command or to apply a setting).

From the kernel's frame to the send, the daemon took about 0.1 ms at the median and 0.2 to
0.35 ms at the 99th percentile on the testing box (an HP t730, replaying a recorded touch);
the status reports each screen's own figure as `latency_us`. The screen's scan and the USB
polling come before the kernel's frame and are not measured here.

### Liveness

    /touch/hello [i port]                -> /touch/here  s host  s version  i osc_api  i listen_port  i devices  i allowed
    /touch/subscribe    i port [i seconds] [s what]   -> /touch/subscribed   i port  i seconds  s what
    /touch/unsubscribe  i port               -> /touch/unsubscribed i port
    /touch/alive        i counter  i run  s version  s health  i devices_connected  i devices_configured  i contacts_active  i status_gen
    /touch/device/face/active  i frame  i contact ...
    /touch/bye          i run

- `hello` is answered for any host, also when sent as a broadcast, so an app can find
  touch-to-osc on the network. `allowed` is 1 when the asking host may subscribe and send
  commands, 0 when it has to be added to `osc.allowed_ips` first. Answers to hosts that are
  not allowed are capped like refusals (below).
- A lease is kept per (sender IP, port), 1-60 s, 10 by default, at most 8 at once. Renew
  it every few seconds. A renewal answers only `subscribed`; a new lease gets
  `subscribed`, the full status, then the event "IP:PORT added (lease N s)". Leases are
  kept over a `restart` (below) but not over a stop. A subscription that is refused is
  answered on the port it names as well.
- `what` says what the lease gets: `all` (the default), `touches` (the touches, the alive,
  the active lists and `bye`) or `status` (the status, the events, the counters, the alive
  and `bye`), optionally followed by `plain` (see Plain types), as words separated by
  spaces: "touches plain". A renewal can change it. `/touch/subscribers` lists the leases
  as before.
- `alive` goes out every second to every subscriber and saved destination, in one bundle
  with one `active` message per configured device. `active` lists the contacts down on
  that device as of bundle `frame` (no contacts: just the frame). `run` is a random
  non-zero number chosen at each start. `health` is `ok`, `warning` or `fault`.
  `status_gen` is the number of the last status bundle sent (see Status).
- `bye` goes out once every contact has been cancelled on a clean stop or a restart.

What a receiver does with these:

1. Keep contacts per (run, device, contact).
2. End everything held for the daemon when `run` changes, when no `alive` arrives for 3 s,
   on `bye`, or when the lease is lost.
3. On each `active` list, end every contact held on that device that is missing from the
   list and whose last message had a frame number not above the list's `frame`. Ignore
   listed contacts it does not hold.
4. Ignore a `move`, `up` or `cancel` for a contact whose `down` it never saw (it subscribed
   in the middle of a touch).
5. Never treat a `cancel` as a tap.
6. Keep the last `status_gen` it got. When a status bundle's `status_gen` is not one more
   than that, or an alive's is higher, it missed a status bundle: ask for the full status
   (`status/send`).

### Plain types

A lease subscribed with `plain` (or a `[[destination]]` with `plain = true`) gets no `h`
arguments, for receivers whose OSC library takes only `i`, `f` and `s`: touches leave out
`t_us` (`i contact f x f y i frame`, a cancel `i contact s reason i frame`) and `counts`
carries `i` values (2147483647 at most). Everything else is the same.

### Status

Sent in full on a new lease, on `status/send` and every 10 s, and each part again as soon
as it changes. Every status bundle starts with `/touch/status_gen i n`: the bundles sent to
the status receivers are numbered 1, 2, 3... within a run, and a full status sent to one
receiver (a new lease, `status/send`) carries the current number without taking a new one.
All of `health`, `reasons` and every device's `health` and `reasons` come together, in one
bundle, whenever any of them changes:

    /touch/status_gen               i
    /touch/osc_api                  i 6
    /touch/version                  s
    /touch/run                      i
    /touch/health                   s ok|warning|fault
    /touch/reasons                  s ...    one string per reason, in plain words
    /touch/host                     s        the machine's host name
    /touch/config                   s        path of the config file
    /touch/settings_file            s        path of the saved settings, "" when nothing is saved
    /touch/listen                   s        "ip:port" it takes commands on; settable (restart)
    /touch/prefix                   s        settable (restart)
    /touch/log_level                s        settable
    /touch/allowed_ips              s ...    hosts allowed to send commands ("any" first with open_to_any)
    /touch/saved_destinations       s ...    "ip:port touches status", "ip:port touches" or "ip:port status"
    /touch/overrides                s ...    the settings that come from the settings file, by name
    /touch/devices                  s id ...
    /touch/unused_screens           s ...    touchscreens plugged in that no device reads: "port usb name"
    /touch/identify_s               i        seconds left identifying screens, 0 when off
    /touch/device/face/health       s ok|warning|fault   this device's part of the health
    /touch/device/face/reasons      s ...    this device's reasons
    /touch/device/face/label        s        settable
    /touch/device/face/enabled      i 0|1    settable; the applied state
    /touch/device/face/connected    i 0|1
    /touch/device/face/state        s starting|ok|not connected|disabled|no permission|grab refused|read error|not a touchscreen|restarting|identifying|calibrating
    /touch/device/face/reason       s        what to do about the state
    /touch/device/face/calibrate_s  i        seconds left in guided calibration, 0 when off
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

`starting` lasts from the start until a device's reader has looked for its screen.
`restarting` lasts from a settings change that reopens the screen (`grab` or a `match_`
setting) until it has been opened again; then the state is whatever the reopen found. A
screen that is not connected goes on reading `not connected`. `identifying` and
`calibrating` are read instead of `ok` while those modes run.

With every alive, to the receivers that get status:

    /touch/uptime_s                 i        seconds since this run started
    /touch/device/face/contacts     i        down now
    /touch/device/face/oldest_s     f        age of the oldest contact down, -1 when none
    /touch/device/face/last_event_s f        seconds since the screen's last frame, -1 never
    /touch/device/face/last_loss_s  f        seconds since the screen last dropped out, -1 never
    /touch/device/face/last_gone_s  f        how long that drop lasted, -1 while it is still gone or never
    /touch/device/face/rate_hz      f        frames per second while touched, last measured
    /touch/device/face/latency_us   i p50  i p99   screen frame to sent, last 10 s
    /touch/device/face/counts       h frames  h touches  h resyncs  h send_drops  h cancels  h losses
    /touch/subscribers              s "ip:port seconds_left" ...
    /touch/destinations             s "ip:port ok|failing" ...

The counts run from the start of the run; `losses` is how many times the screen was
unplugged, dropped off USB or could not be read. `losses` rising or `last_loss_s` getting
smaller says a screen dropped out even when its event was lost, and `last_gone_s` says for
how long; the `connected` event after a drop says so too ("back after 0.6 s").

`health` says what is true now; each device's `health` and `reasons` are its own part of
it, and the top-level ones add what is about no device. It is `fault` when an enabled
device is not working (not connected, no permission, held by another program, read error,
not a touchscreen) or the last USB reset failed, and `warning` when a device is turned off
by a saved setting or restarting after a settings change, while screens are identified or
a screen is calibrated (touches are paused), when a device matches several screens with no
`match_port` to choose, when in the last minute the kernel dropped events or touch messages
could not be sent, when sending to a destination fails, when the config file is refused and
the daemon runs on the copy that last worked, or when the settings file was refused or the
settings or the state could not be saved. A device turned off in the config file counts as
nothing. A screen that drops out is a `fault` while it is gone and `ok` again as soon as it
reads; the drop stays in its event, in `losses` and in `last_loss_s`. `reasons` says which,
in words that say what to do. The heartbeat is the alive's `counter`.

### Events

    /touch/event  s kind  s text  s level  s device

`level` is `notice`, `warning` or `error`: a screen lost is a warning, a screen connected a
notice, a USB reset that failed an error. `device` is the id of the device it is about, or
"" when it is about no device. Kinds:

- `device`: connected, lost, released by the kernel, several screens match, restarting
  after a settings change, a screen that reports no size (its aspect is a guess), reading
  stopped on an internal error.
- `resync`, `palm`, `grab` (another program let go), `enabled` (turned off or on).
- `silence`: a screen went silent with fingers down, and once a run a screen that went
  quiet and then reported the same finger again (it may report only while a finger
  moves).
- `usb`: a USB reset begun, done (with what else it dropped) or failed, a screen back
  after resets, the USB port a screen is on, a port whose state cannot be read.
- `screens`: a touchscreen that no device reads was plugged in or went away.
- `identify` (on, off, and each screen touched while on) and `calibrate` (on, off, a fit
  applied).
- `restart` (asked for, and by whom), `settings` (a setting changed, with the old and new
  value and who sent it; the settings or the state could not be saved), `subscriber`
  (added, ended), `send` (a destination failing, at most once per 10 s each, with a
  count).

Each event is also a line in the journal. Events of one kind from one source are passed on
up to 10 at once and then one a second; the rest are counted and reported in one event
("N more ... came too fast to report one by one").

Logging never holds the daemon up: when the journal cannot take a line at once, the line is
dropped and counted, and the count is logged as soon as it can be. `log.level = "debug"`
adds a line for every command and every lease renewal; touches are never logged.

### Identify screens

    /touch/identify  i seconds          1-60 starts it, or extends it; 0 ends it

While it runs no touch goes out from any screen: contacts that are down are cancelled
`identifying`, each device's state reads `identifying`, the health is `warning` ("touches
are paused while screens are identified (/touch/identify 0 ends it)") and `identify_s` counts down. Each new
touch on a screen sends, to the receivers of status,

    /touch/identified  s device  s label  s usb_port  f x  f y

and an `identify` event. It ends by itself when the seconds run out, so an app that is gone
never leaves the screens dead; an app keeps it on by sending it again. When it ends,
fingers still down are not announced until they lift. Refused while a screen is
calibrated.

### Guided calibration

    /touch/device/face/calibrate  i seconds                   1-60 starts it or extends it; 0 ends it
    /touch/device/face/calibration/fit  f x f y f u f v ...    3 to 16 points
        -> /touch/device/face/calibration/fitted  f worst  f worst_mm

While a screen is calibrated it sends no touches to anyone (contacts are cancelled
`calibrating`, its state reads `calibrating`, `calibrate_s` counts down, the health is
`warning`), and each touch on it sends, to the receivers of status,

    /touch/device/face/calibration/touch  i contact  s phase  f u  f v      phase: down or up

with `u`, `v` where the touch landed after rotation and mirror, before any calibration and
not clamped. An app shows targets on the display, collects where each one was touched, and
sends the pairs to `calibration/fit`: `x`, `y` where the target was shown (0..1 of the
display), `u`, `v` where it was touched. The daemon fits the six calibration numbers by
least squares, applies them and saves them as the `calibration` setting, and answers with
the worst miss of the fit (0..1 of the screen, and in mm when the screen reports its size,
-1 when it does not). Refused: fewer than 3 or more than 16 points, points that all lie on
one line, and a fit that misses a point by more than 5% of the screen ("the touches do not
line up with the targets: worst miss 12% of the screen; touch each target as it is shown
and fit again"). It ends by itself like identify; an app can also send the six numbers to
`calibration` itself, as before.

### Self-description

    /touch/describe [i port]   -> /touch/describe/item s json ...   then   /touch/describe/done i items

Everything an app can show and change, one JSON object per item, in bundles of about 1200
bytes, the last one followed by `done` with the number of items. Item keys are stable;
new ones may be added, so ignore keys you do not know.

    {"kind":"setting","address":"device/face/rotation","device":"face","label":"Rotation","type":"int",
     "values":[0,90,180,270],"unit":"degrees","value":0,"config":0,"source":"config","effect":"cancels",
     "help":"Turns touch positions clockwise to match the display."}
    {"kind":"action","address":"device/face/usb_reset","device":"face","label":"Reset USB","args":[],
     "confirm":"Every device on USB bus 3 drops for a moment.","help":"..."}
    {"kind":"device","id":"face","label":"face screen","source":"config"}

- `address` is without the prefix. `type`: `flag` (0 or 1), `int`, `float`, `text`,
  `floats` (with `count`, or `counts` for the allowed numbers of them), `list` (changed with
  its `add` and `remove` actions). Allowed values: `values` (exact ones) and `range`
  [min, max]; with both, either. `value` is the value in effect, `config` the config
  file's, `source` `config` or `saved`.
- `effect`: `live` (applies at once), `cancels` (applies at once and cancels that screen's
  contacts), `reopens` (the screen is opened again: contacts cancelled, state
  `restarting`), `restart` (touch-to-osc restarts itself to apply it).
- An action's `confirm` is set when it disrupts something: ask the person first. `args`
  lists each argument as `{"name":..,"type":..,"range":[..]}`.
- Ask again after `/touch/devices` or `/touch/overrides` changes.

### Commands

    /touch/request  i seq  s address  ...args   -> /touch/reply  i seq  s ok|refused  s note

    /touch/ping [i port]                  -> /touch/pong i run
    /touch/hello [i port]                 -> /touch/here (Liveness)
    /touch/status/send [i port]           -> the full status and the counters (at most 5 a second)
    /touch/describe [i port]              -> the self-description
    /touch/subscribe, /touch/unsubscribe  as above
    /touch/identify i seconds             identify screens (above)
    /touch/restart                        start the daemon again (below)
    /touch/device/face/usb_reset          reset the USB hub the screen's port belongs to now
                                             (see USB recovery); refused when no port is known
                                             or while a reset runs
    /touch/device/face/calibrate i seconds, /touch/device/face/calibration/fit   (above)

`restart` answers first, then cancels every contact (`restarting`), writes any pending
settings, sends `bye` and starts the same process again, which reads the config file and
the saved settings afresh and runs the program as it is installed now (so it also brings
in a new version). Each lease whose host the new config still allows is kept: right after
the restart it gets the full status and an alive with the new `run`. It is refused, and the
daemon keeps running, when the config file would be refused at the restart, when the
installed program would not start with it, when a changed `osc.listen` cannot be used,
and while a USB reset runs. A repeat of the same request (the same seq, its reply lost) is
answered from memory, also by the new run, and never restarts it again.

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
    /touch/devices/add               s id  s match_usb  s match_port      (one of the two may be "")
    /touch/devices/remove            s id
    /touch/listen                    s "ip:port"
    /touch/prefix                    s
    /touch/settings/forget           [s name]   one setting, by its name in /touch/overrides,
                                                device/<id> for all of one device's, or
                                                nothing for all of them: back to the config file

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
- `devices/add`, `devices/remove`, `listen` and `prefix` are saved and applied by a restart
  the daemon does itself right after answering, as `restart` above; the reply's note says
  so, and for `listen` and `prefix` names the new address ("restarting to apply it; then
  touch-to-osc listens on 0.0.0.0:9481"): an app has to change where it sends. Refused: an
  id in use, not valid or taken by the daemon's own addresses, a device that would match
  nothing, removing the last device, a listen address that cannot be used, a prefix that
  is not valid, and any of them while a USB reset runs. A device added here and later
  added to the config file under the same id: the config file's wins, with a journal line.
- A value set back to what the config file says stops being an override, and a command
  that changes nothing is answered `ok` without being saved or reported. `settings/forget`
  that forgets the listen address, the prefix or added or removed devices restarts the
  daemon too, as above.
- `open_to_any` stays in the config file. While a restart is on its way, every further
  command is refused ("touch-to-osc is restarting").

- Every command can go inside a request and then gets exactly one reply. Replies are kept
  per (IP, port, seq) for 30 s or 64 newer requests: an exact repeat gets the kept reply
  and is not run again; the same seq with another command is refused ("seq 7 was used for
  another command in the last 30 s: use a new seq for each command").
- A bare command works too; a refusal then answers `/touch/error s address s note`.
- An address that does not exist is refused with the closest one that does ("unknown
  address (did you mean /touch/device/face/rotation?)"), and a value only reported by the
  status says it cannot be set.
- Commands and subscriptions from a host outside `osc.allowed_ips` are answered with
  `/touch/error` saying so and where the allowed hosts come from (at most once per 5 s per
  host, and at most 20 such answers a second in all) and otherwise ignored; `hello` is the
  only command they may send.
- There is deliberately no command that creates a touch or writes the config file.

## Watching it

`touch-to-osc-watch` (`tools/watch.py` in the source tree) subscribes exactly as a receiver
should and prints every touch with its delay, every status change and event, and the alive.
It says when the daemon is lost, restarted or stopped, and when nothing answers at all:

    touch-to-osc-watch --no-alive                           the service on this machine
    touch-to-osc-watch --target 192.168.1.246:9480 --device face

`--target` defaults to 127.0.0.1:9480, `--device` shows one device only, `--no-alive`
leaves out the alive and the counters, `--no-moves` the moves. The machine running it
must be in `osc.allowed_ips`.

## Building

Linux with evdev, libevdev, a C++17 compiler, CMake 3.18 or newer and Ninja. Built and
tested on Debian 11 (kernel 5.10, g++ 10, libevdev 1.11):

    apt install build-essential cmake ninja-build pkg-config libevdev-dev

`evtest` and `evemu-tools` are useful for looking at a screen and recording it. The tests
also need `python3`.

To build the package, do it on the oldest system it is meant for:

    scripts/build-deb.sh
    sudo apt install ./build-deb/touch-to-osc_*_amd64.deb

`build-deb.sh` builds with warnings as errors, runs the unit tests and writes the package
to `build-deb/`.

Without the package, on any Linux with systemd, from the source tree:

    sudo scripts/install.sh
    sudo systemctl enable --now touch-to-osc

`install.sh` builds, runs the unit tests and installs under `/usr/local` (the unit in
`/usr/local/lib/systemd/system`), with the same config handling. `scripts/uninstall.sh`
removes it again (`--purge` also removes the config and the saved settings). Both refuse
while the package is installed.

## Tests

    build/tto_tests                          unit tests
    sudo python3 tests/e2e/run.py [--quick]  end-to-end tests
    sudo python3 tests/hw/usb_recovery.py    USB recovery on a real screen (below)

The end-to-end tests start the daemon against virtual touchscreens made through
`/dev/uinput` and a receiver that subscribes like a real one. They replay touches recorded
on a real screen (`tests/traces`), unplug screens in the middle of a touch, stop and
freeze the daemon, make the kernel drop events, and check every command. They need root,
`/dev/uinput` and `python3`, use UDP ports 19480-19499, and give their virtual screens USB
vendor id 7470 so a running service never takes them. `--quick` skips the tests that take
longer than 10 s. For USB recovery they point the daemon at a made-up `/sys` and `/dev`
with the environment variable `TTO_USB_ROOT`; the daemon warns at start when it is set.

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
