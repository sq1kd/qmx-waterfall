# CAT link behaviour

The radio (QMX, firmware 1_04_010 on the reference unit) exposes one USB CDC
port. `main/cat/service.c` owns it, runs a single-command-at-a-time state
machine (150 ms between commands, 700 ms timeout) and publishes state through
`cat_service_get_status()`.

## Command budget

| Command | When | Notes |
|---|---|---|
| `Q9;` | connect, then every 2 s while `IQ_ACTIVE` | IQ mode get/health |
| `Q91;` | when IQ is missing or lost | IQ mode on; the set has no reply |
| `VN;` | once per connection, during `PROBE` | informational; silent/unsupported radio is not a fault |
| `IF;` | every `poll_period_ms` (1 s) | mode, RIT, TX/RX |
| `FW;` | 250 ms after a mode change, and after IQ recovery | filter bandwidth |

Only one response is ever outstanding. `VN;` and the `Q9;` health poll are
"optional" commands: a timeout reschedules rather than escalating to
`DEGRADED`/`FAULT`, so a radio that is busy in its configuration menu cannot
trip the link-fault path.

## Auto info (`AI1;`) is intentionally not used

The CAT manual documents `AI1;`: the radio pushes an `IF` frame on every change
plus every 1.5 s. Measurements on firmware 1_04_010 (2026-11-01):

| `AI1` | IQ mode | Menu entered | Result |
|---|---|---|---|
| on | off | yes | fine |
| on | on | no | fine (120 s, heartbeats exactly 1.500 s) |
| on | on | **yes** | **radio freezes in ~1 s**, needs power cycle (3/3 runs) |
| off | on | yes | fine |

Closing or reopening the CDC port with `AI1` enabled is harmless, and an
active USB audio host does not change the outcome. Because the app always
streams IQ and menu entry is user-initiated, `AI1;` cannot be enabled here.
Do not reintroduce it without re-testing this exact combination on the target
firmware. `AI2;` is accepted but emits nothing; `AI3;` emits the same frames
as `AI1`.

## IQ mode auto-recovery

IQ mode is session-only on the radio and is cleared when the operator leaves
the configuration menu. `IQ_ACTIVE` therefore polls `Q9;` every 2 s:

1. A reply of `0` increments `iq_drops`, sets `iq_recovering` and enters
   `IQ_ENABLING`.
2. `IQ_ENABLING` sends `Q91;` (no reply), then `IQ_VERIFY` reads `Q9;`.
3. A reply of `1` increments `iq_reenables`, schedules an `FW;` refresh,
   requests a waterfall/spectrum history reset and returns to `IQ_ACTIVE`.
4. A reply of `0` retries after 1 s (three fast attempts), then backs off
   2/4/8 s up to a 10 s cap, indefinitely. Health-poll timeouts do not count
   as link faults.

Operator-visible behaviour: the waterfall keeps rendering whatever the radio
streams, so it shows audio instead of I/Q for up to one poll period (plus
~0.3 s for the `Q91;`/`Q9;` exchange). History captured in that window is
discarded once IQ is restored, so no audio-spectrum rows linger.

## Startup checklist

`main/display/display.c` renders a 2x3 grid of state-coloured badges while the
device is connecting (border, background tint and a `+`/`>`/`!`/`.` marker per
state); the two-line status view is only used for "waiting for device" and
"disconnected". Badges are driven from the connection callbacks and from the
CAT status callback:

| Row | Set done by | Detail |
|---|---|---|
| USB device | UAC driver connect | `found` |
| Audio | after UAC alt-parameter scan | `48k 2ch 24b` |
| Control link | CDC port open, then first `Q9;` reply | `opening` → `synced` |
| Radio | `VN;` reply during `PROBE` | `1_04_010` (`?` if silent) |
| IQ mode | `IQ_VERIFY` → `Q9;`=1, or the drop is recovered | `on` |
| Spectrum | first waterfall frame pushed by the pipeline | `running` |

The checklist stays above the spectrum canvas and disappears when the first
frame is on screen, so the startup sequence never blinks through unreadable
text. `display_show_status()` resets it for the next connection.
