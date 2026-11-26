# Changelog

## 0.8.0 - 2026-11-25

Fixes from a full review. Nothing on the wire changed (still osc_api 6).

Settings:

- `listen`, `prefix`, `devices/add` and `devices/remove` are saved before the restart that
  applies them. If the save fails they're refused and nothing restarts; it used to
  restart anyway, cancel every contact and come back unchanged.
- `--status` and `--send` look where the saved settings say the service listens, so they
  still reach it after `listen` or `prefix` was changed over OSC.
- A `listen` or `prefix` changed in the config file after one was saved over OSC wins at
  the next start, as `log_level` and the per-screen settings already did. Settings saved
  by older versions get this from now on.
- When the listen port can't be used at a start, the message says whether it came from the
  config file or was set over OSC, and how to put it back.
- `settings/forget` with no name can no longer lock out the host that sends it.
- Turning a screen off is saved at once instead of up to a second later (but, like any
  setting, at most once a second). A settings file too damaged to read still keeps a
  screen that was turned off turned off.
- One saved setting the config file has since made impossible (the last device removed,
  the last host taken off, a device left matching nothing) is dropped by itself with a
  note; it used to set the whole settings file aside.
- A config with `'''` in it no longer breaks the file the daemon keeps between runs, which
  lost the last USB port and the config copy it falls back on. A failed write of that file
  is tried again every 10 s.
- Refused now: a calibration that puts every touch on one line, a settings or state path
  that is the config file, anything but printable ASCII in a list, a saved device without
  an id, and over OSC a destination that is the daemon's own listen address (in the config
  file that one is a warning; a saved one is dropped with a note).
- Files a save left unfinished (a kill or a power cut in the middle) are cleared at the
  start.

Screens:

- With `grab = false`, another program taking the screen for itself is noticed within a
  few seconds as a fault, and a finger held on a screen with `silence_ms = 0` is
  cancelled. It used to go unnoticed until a finger was down with `silence_ms` on.
- If the thread reading a screen with `silence_ms` on ever stops dead with contacts down,
  the daemon says so, cancels what the other screens hold, sends `bye` and exits so its
  service starts it again. Before, it would have gone on listing those contacts as down.
- A new subscriber gets an alive at once, before its first touch, so it knows the run
  the touch belongs to.
- SIGHUP and SIGQUIT stop it cleanly like SIGTERM (contacts cancelled, then `bye`),
  unless whoever started it had them ignored (nohup); SIGUSR1 and SIGUSR2 are ignored.
  They used to kill it on the spot.
- A screen event the screen's own description rules out is counted and noted in the
  journal once a minute, instead of going to the journal raw.
- Touch messages that couldn't be sent, kernel drops and silences in the first second of
  a run now show in the health like later ones; they used to be missed.
- At a stop or a restart the contacts are cancelled and `bye` is sent before anything else
  is waited for. They used to wait for the thread that looks for unused touchscreens,
  which can be held up for seconds by a USB device the kernel is busy with.

USB recovery:

- A failed reset no longer leaves a fault until the next restart: it clears when the
  screen reads again or its hub is bound or gone. A hub that a failed reset left
  unbound is bound again while running, every 10 s, instead of only at the next start.
- A hub that isn't there isn't "reset", and isn't taken for one left unbound.
- An automatic reset is skipped when the note that lets a cut-short reset be finished
  can't be written, or when what else is on the hub can't be read. That isn't counted as a
  reset and isn't tried over and over; `usb_reset` still does it.
- A screen that's turned off is left alone.

Tools:

- `touch-to-osc-watch` keeps the contacts that are down by the receiver rules (the
  `Contacts` class in it is there to copy) and says when one ends without its own `up` or
  `cancel`. It no longer ends on a packet it can't read or stops renewing after a failed
  send, and it leaves out packets that don't come from the daemon's address
  (`--any-sender` takes them).
- `--touch` and `--all` without `--list` are refused instead of starting a second daemon.
  `--list --touch` ends when its screen is unplugged instead of spinning.
- `--status` asks again when another host's request was just answered.

The journal and the disk:

- A service that can't start, or dies within a minute of starting, is no longer started
  again every second for ever (about 300 journal lines a minute, and a disk sync for
  every failed start). The first restart still comes at once; after that it waits 2, 4,
  8, 16, 32 and then 60 s before each start, and says so. `systemctl restart touch-to-osc`
  starts it at once. A run that lasts a minute, or a stop that was asked for, clears the
  count, which is kept in `/run/touch-to-osc` (memory, not disk).
- `restart`, and the settings that restart the daemon, are refused after 10 in a row,
  whether they restarted it or were refused; one more is taken every 30 s. A host
  sending `restart` in a loop used to restart it several times a second, 15 journal
  lines each time.
- `usb_reset` is refused for 5 s after the last one.
- A screen that came back for a moment after every USB reset and dropped off again was
  reset every 12 s or so for ever: reading again started the count of resets over. Now
  the count is only forgotten once the screen has stayed read for a minute, so such a
  screen gets its 5 resets and then the usual give-up.
- A screen turned off and on in a loop no longer syncs the settings file to disk for
  every turn-off (hundreds of syncs a second); settings are saved at most once a second.
- A settings file that can't be written is said once, in the journal and in an event, and
  once more when it works again, instead of every 10 s. The health goes on saying it.
- Events of one kind that keep coming still reach receivers at one a second, but the
  journal takes 60 in a row and then one a minute, with a count of what it left out.
  Refused commands, hosts that aren't allowed and malformed packets are thinned the same
  way. A destination that can't be sent to is logged 6 times, then every 5 minutes (its
  `send` events too).
- Log level `debug` logs 200 commands in a row, then 10 a second.
- Since 0.7.0, once any input device had been plugged in or out, the daemon re-read every
  input device's description 4 times a second for as long as it ran (about 1,000 system
  calls a second with 7 input devices, 0.4% of a core). It looks every 5 s again.

The rest:

- The binary is built with stack protection, fortified libc calls and full RELRO.
- The service starts without waiting for the network to be fully up, and its unit is
  locked down a little more. With `listen` on one address rather than `0.0.0.0`, it
  restarts at boot until that address is there, up to a minute apart (see above).
- The package recommends python3, which `touch-to-osc-watch` needs. `scripts/install.sh`
  no longer stops at a refused config, and installing the package over a script install
  clears everything the script put under `/usr/local`.
- OSC.md: the receiver rules say that a contact ended by the active list or a lost daemon
  isn't a tap, and to take packets only from the daemon's address. The event kind `palm`
  is gone from the list; nothing ever sent it.

## 0.7.3 - 2026-10-28

- USB recovery gives up. It resets a stuck screen's USB at most 5 times (10, 20, 40, 80
  and 160 s apart), then stops and says so in the health and an event, where it used to
  go on every 5 minutes for as long as the screen stayed stuck. Unplugging the screen
  starts it over, up to 10 resets until the screen reads touches again; turning
  `usb_recovery` off and on starts it over too.
- USB recovery never resets a hub that holds anything besides the screen (on a port of
  the computer itself, that's the whole USB bus). It says what a reset would drop and
  leaves it to you: `usb_reset` still resets it when you send it. `--list` says so too.
- Nothing on the wire changed (still osc_api 6); the `usb` events and reasons read
  differently.

## 0.7.2 - 2026-10-28

- The example config's screen is now `id = "screen"` with no label (it was `face`), so a
  fresh install sends `/touch/screen/down` and so on. Existing configs are left as they
  are. The docs, `--help` and messages use `screen` as the example too, and no longer name
  any particular receiver.

## 0.7.1 - 2026-10-28

- The README is cut down to the basics, and the OSC interface has its own short reference,
  OSC.md, installed next to the README in `/usr/share/doc/touch-to-osc`. Nothing on the
  wire changed (still osc_api 6).

## 0.7.0 - 2026-10-26

OSC interface version 6: an app can find touch-to-osc, read what
it can change, identify and calibrate screens and add them, and every problem can be shown
for the screen it is about. Everything in version 5 is still sent as it was; version 6
only adds addresses and appends arguments.

- Every event names its device as a fourth string ("" when it is about no device), and
  each device has its own `health` and `reasons` in the status. All health parts come in
  one bundle whenever any of them changes.
- `device/<id>/last_loss_s` and `last_gone_s` with the counters: seconds since the screen
  last dropped out and how long that lasted, so a drop is seen even when its event was
  lost; the `connected` event after a drop says how long it was gone.
- Every status bundle starts with `/touch/status_gen`, numbering the bundles sent to the
  status receivers, and the alive carries the latest number as an eighth argument: a
  receiver that sees a gap asks for the full status.
- `/touch/hello` is answered for any host, also as a broadcast, with the host, version,
  osc_api, port, number of devices and whether the asking host is allowed.
- `/touch/describe` sends every setting, action and device as a JSON item, with the value
  in effect, the config file's value, where it comes from and what a change does.
- `/touch/identify` pauses every screen's touches for up to 60 s and reports which screen
  each touch lands on (`/touch/identified`); guided calibration
  (`device/<id>/calibrate`) pauses one screen's touches and reports each touch before
  calibration, and `device/<id>/calibration/fit` fits, applies and saves the calibration
  from target and touch pairs, refusing points on one line or a fit that misses by more
  than 5%. Both end by themselves; contacts down are cancelled `identifying` or
  `calibrating`, and fingers still down afterwards are not announced until they lift.
- `devices/add`, `devices/remove`, `listen` and `prefix` change the device list, the listen
  address and the prefix over OSC: saved, and applied by a restart the daemon does itself
  right after answering, with the new address in the reply's note. `unused_screens` in the
  status and a `screens` event list the touchscreens no device reads.
- A subscription can ask for `touches` or `status` only, and for `plain` types (no 64-bit
  arguments: touches without `t_us`, counts as `i`), for receivers that cannot take `h`;
  `plain = true` does the same for a `[[destination]]`. `/touch/subscribed` carries what was
  asked for as a third argument.
- Replies, errors, `pong` and `here` also go to every port the asking host holds a lease
  on, and a refused subscription is answered on the port it names as well.
- Commands take OSC doubles (`d`); an argument of a type commands never take (a blob, say)
  is answered with `/touch/error` naming it instead of the packet being dropped.
- New states `identifying` and `calibrating`, cancel reasons `identifying` and
  `calibrating`, event kinds `screens`, `identify` and `calibrate`.
- `touch-to-osc --list --touch` waits for a touch on a screen no device reads and prints a
  block to paste for it; `--status` lists such screens.

## 0.6.0 - 2026-10-25

From a full review of the daemon. The OSC interface stays at version 5: no address or
argument changed; some texts and when health warns did.

Fixes:

- A screen held by another program was read again, as soon as that program let go, from
  the touch state the daemon had missed meanwhile (a lift could land on the wrong finger),
  and a contact announced before could end without an `up` or `cancel`. The screen is now
  opened afresh when the other program lets go, and its contacts are cancelled first.
- A second copy started by hand could share the service's UDP port and take its commands.
  It now exits 3, saying the service most likely runs already.
- A stop or restart while a USB reset waited in the kernel could leave the USB hub unbound,
  with every device on it off until a reboot. A stop or restart now waits for the reset
  (telling systemd it is still alive), a restart is refused while one runs, and a reset cut
  short anyway is finished at the next start.
- `restart` is refused when the installed program would not start with the config (it
  checked with the running version before), when a changed `osc.listen` cannot be used,
  and while a USB reset runs. A repeated restart request whose reply was lost no longer
  restarts twice.
- USB recovery remembers the port each screen was last read on across restarts, so a
  screen that is stuck at boot is still watched, and a reset advances the wait of every
  screen on that hub, not only the one that asked.
- Saving the settings no longer runs on the thread that sends the heartbeat; a slow disk
  cannot hold up the alive or the watchdog.
- An axis range near the 32-bit limits (a broken device) is mapped without overflowing.
- Text from outside (an address in a request, a name to forget, a host) is never echoed
  unprintable into replies, the status or the journal.
- Run by hand from a terminal that has `JOURNAL_STREAM` set, the daemon no longer makes
  that terminal non-blocking; a line cut short in the journal is ended before the next.
- `apt remove` followed by a new install left the service disabled and stopped; it is
  enabled and started again if it was before. An upgrade whose new version refuses the
  config no longer restarts the service into a stop: the old version keeps running, and
  the install says so.

Changed behaviour:

- `health` says what is true now. A screen that drops out is a `fault` while it is gone and
  `ok` again as soon as it reads (before, a `warning` stayed for a minute); the drop stays
  in its event and in `losses`. Silences no longer leave a warning either. A device turned
  off in the config file is no longer a warning. `health` and `reasons` always come
  together in one bundle.
- A config edit made after a setting was changed over OSC wins at the next start: the
  settings file now records what the config file said when each setting was saved, and a
  config value changed since then applies, with a journal line. Allowed hosts and saved
  destinations changed over OSC are kept as changes on top of the config file's lists, so a
  host added to the config file later is not hidden. Older settings files are read as
  before and rewritten in the new form; 0.5.1 and older refuse the new form.
- Saved settings for a device the config no longer has are kept, unused, instead of being
  dropped at the next save; `settings/forget device/<id>` forgets all of one device's.
- When the config file is refused at a start, the daemon runs on its copy of the last
  config that started, with a health warning saying why, instead of staying stopped.
- Leases are kept over `restart`: each subscriber still allowed gets the full status and an
  alive with the new run right after it, so nothing touched in those first seconds is
  lost.
- Identical screens matched by usb id alone are shared out in USB port order, and a block
  that names a port, a serial or a usb id chooses before one that says less, so the same
  cabling gives each device the same screen at every start. `--check-config` and the
  journal warn about blocks that cannot be told apart, and the health warns while one
  matches several screens.
- `match_usb`, `match_serial` and `match_port` are the config file's names too, as over
  OSC; `usb`, `serial` and `port` still work (giving both is refused). 0.5.1 and older
  refuse the new names.
- New `[paths] state` (`/var/lib/touch-to-osc/state.toml`): what the daemon keeps between
  runs. 0.5.1 and older refuse the key.
- Device ids `device`, `devices`, `status`, `settings`, `describe`, `allowed_ips` and
  `saved_destinations` are refused: touches to them would mix with the daemon's own
  addresses.

New:

- `touch-to-osc --status` shows the running service's health, screens and settings, and
  `touch-to-osc --send ADDRESS [VALUE...]` sends it one command and prints the answer.
- `--list` shows the touchscreens first, which device reads each (the way the daemon
  shares them out), what a USB reset for each would reset, and a `[[device]]` block to
  paste for a screen no block reads; `--all` lists the other input devices too.
- `touch-to-osc-watch` (watch.py) in `/usr/bin`; it defaults to this machine, has
  `--device`, says when nothing answers, and `--no-alive` really leaves out the counters.
- Messages that say more: an unknown key or address names the closest known one, a value
  only reported by the status says it cannot be set, config errors name the device
  (`device "screen" (the 1st [[device]])`), the refusal to an outside host says where the
  allowed hosts come from, the start lines list the allowed hosts, a screen on another
  port than `match_port` says where it is, a finished USB reset names what else it
  dropped, a screen that reports no size says its aspect is a guess, and a screen that
  goes quiet and then reports the same finger again gets a hint about `silence_ms`.
- `--config FILE` works with a space, `-h` and `--help` print the usage with the exit
  codes, `--check-config` without root says to use sudo instead of calling the settings
  refused, and exits 5 when only the saved settings are refused.
- README: a quick start, several screens, lining up the touch panel step by step,
  troubleshooting, where answers go and what a firewall must let in.

## 0.5.1 - 2026-10-22

- A Debian package, attached to each version's GitHub release from now on:
  `scripts/build-deb.sh` builds `touch-to-osc_<version>-1_amd64.deb` (built on Debian 11,
  it installs on Debian 11 to 13 and Ubuntu 22.04 and 24.04). It
  installs the program in `/usr/bin` and the systemd unit, writes the config from the
  example if there is none, and enables and starts the service; a newer package restarts
  it if it is running; `apt purge` also removes the config and the saved settings. It
  replaces a copy installed with `scripts/install.sh`.
- `scripts/install.sh` installs everything under `/usr/local`, the unit in
  `/usr/local/lib/systemd/system` instead of a copy in `/etc/systemd/system`, and with
  `scripts/uninstall.sh` refuses while the package is installed.
- The scripts are executable in a fresh clone.

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
