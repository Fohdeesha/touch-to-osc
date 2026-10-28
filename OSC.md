# OSC interface (osc_api 6)

All UDP. Send commands and subscriptions to the daemon's `listen` port (9480). It sends from
other ports, so a firewall in front of a receiver has to let in UDP from its machine.
Addresses start with the prefix (`/touch`); each screen shows up by its config `id` (`screen`
below).

- Numbers you send can be `i`, `h`, `d` or a whole `f` (`T`/`F` = 1/0). Strings are ASCII.
- Later versions only add addresses and append arguments, so ignore what you don't know.
- Answers go to the IP and port a command came from (or the port it names), plus every port
  that IP holds a lease on.

## Guarantees

- Every contact ends with exactly one `up` or `cancel`, sent by the daemon: unplugs, silent
  screens, resyncs, USB resets, stops and restarts all cancel what's down.
- Every second the contacts still down go out, so a lost `up` clears within a second.
- A finger already down when reading starts is never announced. A `cancel` is never a tap.
- Nothing creates a touch, and reading touches never waits on the network or the disk.

## Touches

One bundle per screen frame that changed something, one message per changed contact:

    /touch/screen/down    i contact  f x  f y  i frame  h t_us
    /touch/screen/move    i contact  f x  f y  i frame  h t_us
    /touch/screen/up      i contact  f x  f y  i frame  h t_us
    /touch/screen/cancel  i contact  s reason  i frame  h t_us

- `contact` counts up from 1 per run, never reused.
- `x`, `y`: 0..1 of the display, top left `0 0`, after rotation, mirror and calibration.
  Where a picture sits on the screen is the receiver's job.
- `frame` numbers this screen's bundles from 1 per run; a gap means a lost bundle.
- `t_us`: Unix time in microseconds when the screen reported it.
- Cancel reasons: `device lost`, `device silent`, `device disabled`, `resync`, `palm`,
  `reconfigured`, `identifying`, `calibrating`, `stopping`, `restarting`.

## Leases and liveness

    /touch/hello [i port]                          -> /touch/here  s host  s version  i osc_api  i listen_port  i devices  i allowed
    /touch/subscribe  i port [i seconds] [s what]  -> /touch/subscribed  i port  i seconds  s what
    /touch/unsubscribe  i port                     -> /touch/unsubscribed  i port
    /touch/alive  i counter  i run  s version  s health  i devices_connected  i devices_configured  i contacts_active  i status_gen
    /touch/device/screen/active  i frame  i contact ...
    /touch/bye  i run

- `hello` answers any host, broadcast too; `allowed` says if that host may subscribe.
- Leases are per (IP, port), 1-60 s (10 default), 8 max. Renew every few seconds. They
  survive a `restart`, not a stop.
- `what`: `all` (default), `touches` or `status`, optionally plus `plain` ("touches plain").
  `plain` sends no `h` (touches without `t_us`, counts as `i`) for OSC libraries without
  64-bit ints.
- `alive` comes every second with one `active` per screen: the contacts down as of `frame`.
  `run` is random per start. `bye` comes on a clean stop or restart.

Receiver rules:

1. Track contacts by (run, device, contact).
2. Drop everything when `run` changes, alives stop for 3 s, on `bye`, or when the lease is
   lost.
3. On `active`, end held contacts on that screen that aren't listed and whose last frame is
   not above the list's `frame`.
4. Ignore `move`, `up` and `cancel` for a contact whose `down` you never saw.
5. Never treat a `cancel` as a tap.
6. If `status_gen` skips, ask for `status/send`.

## Status

Sent in full on a new lease, on `status/send` and every 10 s, and each part again when it
changes. Each status bundle starts with `status_gen`.

    /touch/status_gen                      i
    /touch/osc_api                         i 6
    /touch/version                         s
    /touch/run                             i
    /touch/health                          s ok|warning|fault
    /touch/reasons                         s ...      what's wrong and what to do
    /touch/host                            s
    /touch/config                          s
    /touch/settings_file                   s
    /touch/listen                          s          settable (restart)
    /touch/prefix                          s          settable (restart)
    /touch/log_level                       s          settable
    /touch/allowed_ips                     s ...
    /touch/saved_destinations              s ...      "ip:port touches status"
    /touch/overrides                       s ...      settings saved over OSC
    /touch/devices                         s id ...
    /touch/unused_screens                  s ...      "port usb name"
    /touch/identify_s                      i
    /touch/device/screen/health            s ok|warning|fault
    /touch/device/screen/reasons           s ...
    /touch/device/screen/label             s          settable
    /touch/device/screen/enabled           i 0|1      settable
    /touch/device/screen/connected         i 0|1
    /touch/device/screen/state             s          below
    /touch/device/screen/reason            s
    /touch/device/screen/calibrate_s       i
    /touch/device/screen/match_usb         s          settable
    /touch/device/screen/match_serial      s          settable
    /touch/device/screen/match_port        s          settable
    /touch/device/screen/calibration       f a  f b  f c  f d  f e  f f    settable
    /touch/device/screen/grab              i 0|1      settable
    /touch/device/screen/usb_recovery      i 0|1      settable
    /touch/device/screen/usb_watched_port  s
    /touch/device/screen/usb_port_state    s
    /touch/device/screen/usb_resets        i
    /touch/device/screen/name              s
    /touch/device/screen/usb               s          "vvvv:pppp serial"
    /touch/device/screen/port              s
    /touch/device/screen/slots             i
    /touch/device/screen/size_mm           f w  f h
    /touch/device/screen/rotation          i 0|90|180|270   settable
    /touch/device/screen/mirror            s ""|x|y|xy      settable
    /touch/device/screen/aspect            f          settable
    /touch/device/screen/silence_ms        i          settable
    /touch/device/screen/grabbed           i 0|1

`state`: `starting`, `ok`, `not connected`, `disabled`, `no permission`, `grab refused`,
`read error`, `not a touchscreen`, `restarting`, `identifying`, `calibrating`.

With every alive, to status receivers:

    /touch/uptime_s                    i
    /touch/device/screen/contacts      i
    /touch/device/screen/oldest_s      f          -1 when none
    /touch/device/screen/last_event_s  f          -1 never
    /touch/device/screen/last_loss_s   f          -1 never
    /touch/device/screen/last_gone_s   f          -1 still gone or never
    /touch/device/screen/rate_hz       f
    /touch/device/screen/latency_us    i p50  i p99
    /touch/device/screen/counts        h frames  h touches  h resyncs  h send_drops  h cancels  h losses
    /touch/subscribers                 s "ip:port seconds_left" ...
    /touch/destinations                s "ip:port ok|failing" ...

`fault` means an enabled screen isn't working or a USB reset failed. `warning` means
something needs a look. `reasons` says which.

## Events

    /touch/event  s kind  s text  s level  s device

`level` is `notice`, `warning` or `error`; `device` is "" when it's about no screen. Kinds:
`device`, `resync`, `palm`, `grab`, `enabled`, `silence`, `usb`, `screens`, `identify`,
`calibrate`, `restart`, `settings`, `subscriber`, `send`.

## Identify and calibrate

    /touch/identify  i seconds                         1-60, 0 ends it
    -> /touch/identified  s device  s label  s usb_port  f x  f y

    /touch/device/screen/calibrate  i seconds            1-60, 0 ends it
    -> /touch/device/screen/calibration/touch  i contact  s down|up  f u  f v
    /touch/device/screen/calibration/fit  f x f y f u f v ...    3 to 16 points
    -> /touch/device/screen/calibration/fitted  f worst  f worst_mm

Both pause touches (cancelled `identifying` or `calibrating`) and end by themselves. For
calibration, show targets, collect where each was touched (`u v`, uncalibrated), and send
target and touch pairs to `fit`. The daemon fits, applies and saves it, and refuses a fit
that misses by more than 5% of the screen.

## Self-description

    /touch/describe [i port]   -> /touch/describe/item s json ...   then   /touch/describe/done i items

One JSON object per setting, action and device, with its address, type, allowed values,
current and config values, and what a change does (`effect`: `live`, `cancels`, `reopens`,
`restart`). An action with `confirm` disrupts something, so ask first. Ask again when
`devices` or `overrides` changes.

## Commands

    /touch/request  i seq  s address  ...args   -> /touch/reply  i seq  s ok|refused  s note

Wrap any command in `request` to get exactly one reply; a repeated seq gets the same reply
without running again. A bare command that's refused gets `/touch/error  s address  s note`.

    /touch/ping [i port]            -> /touch/pong i run
    /touch/status/send [i port]     -> the full status (one every 200 ms, across all hosts)
    /touch/restart                  restart the daemon (leases are kept)
    /touch/device/screen/usb_reset  reset the screen's USB hub now

Settings live at the address the status reports them on, and are saved:

    /touch/device/screen/label         s
    /touch/device/screen/enabled       i 0|1
    /touch/device/screen/rotation      i 0|90|180|270
    /touch/device/screen/mirror        s ""|x|y|xy
    /touch/device/screen/calibration   f a  f b  f c  f d  f e  f f   (none to remove it)
    /touch/device/screen/aspect        f          0 = from the device
    /touch/device/screen/silence_ms    i          0 or 50-10000
    /touch/device/screen/grab          i 0|1
    /touch/device/screen/match_usb     s
    /touch/device/screen/match_serial  s
    /touch/device/screen/match_port    s
    /touch/device/screen/usb_recovery  i 0|1
    /touch/log_level                   s
    /touch/allowed_ips/add             s ip
    /touch/allowed_ips/remove          s ip
    /touch/saved_destinations/add      s "ip:port" [i touches  i status]
    /touch/saved_destinations/remove   s "ip:port"
    /touch/devices/add                 s id  s match_usb  s match_port
    /touch/devices/remove              s id
    /touch/listen                      s "ip:port"
    /touch/prefix                      s
    /touch/settings/forget             [s name]   one, device/<id>, or all

`rotation`, `mirror` and `calibration` cancel the screen's contacts; `grab` and `match_*`
reopen it; `devices`, `listen` and `prefix` restart the daemon. Hosts outside `allowed_ips`
can only `hello`. Nothing creates a touch.
