# Beko Dishwasher Integration

An ESP32-C3-based sniffer that taps the SPI bus between a Beko dishwasher's control panel
and mainboard, decodes the traffic, and exposes it to Home Assistant via ESPHome — so you
can see the running program, time remaining, and status without a physical display.

## Hardware setup

The control panel talks to the mainboard over SPI with **no chip-select line** (there's only
one slave, so CS isn't needed). An ESP32-C3 is wired in as a passive listener on two of the
three signal lines:

| Signal | ESP32-C3 pin | Notes |
|---|---|---|
| CLK  | GPIO3 | Clock, sniffer-only |
| MOSI | GPIO4 | Data, sniffer-only |
| GND  | — | **Must share a common ground** with the mainboard, or every read is meaningless noise |
| MISO | *not wired* | The mainboard->panel direction is not currently tapped (see [Unimplemented: MISO frame](#unimplemented-miso-frame) below) |

Both pins are configured `INPUT_PULLDOWN` in software so a disconnected/idle bus reads a
clean low instead of drifting on ambient noise; the mainboard's actively-driven signal easily
overrides the weak internal pull when connected.

### Why polling doesn't work

An early version of this sniffer read the CLK pin by polling `digitalRead()` in ESPHome's
`loop()`. This does not work reliably: Arduino's `loop()` competes with the WiFi stack for
CPU time and is far too slow and jittery to catch a real clock edge — in testing it caught
roughly one stray bit per real ~100ms bus cycle, aliasing what looked like random low-rate
noise. **The sniffer must use a GPIO interrupt** (`attachInterruptArg` on `RISING`) to sample
MOSI exactly once per clock pulse, regardless of what else the CPU is doing. This is the
single most important lesson from building this: if you're seeing sparse, seemingly-random
single-byte "frames," you're probably polling instead of using an ISR.

## Bus timing

- Measured clock rate: **~1.75 kHz** (~3400-3500 rising edges per 2-second window)
- A full status update repeats roughly **every ~100 ms**
- There is no CS line, and during sustained bus activity (e.g. normal-paced button presses)
  the mainboard sends frames **back-to-back with no idle gap at all** between them. An
  earlier version of this sniffer relied on an idle-gap timeout to find frame boundaries,
  which worked fine at rest but caused multi-second stalls with zero valid frames under
  normal use, since the byte accumulator had nothing to anchor alignment to and could drift
  out of sync indefinitely. The fix: continuously check the incoming bitstream against the
  frame's own known-constant 3-byte header (`02 10 2B`), independent of any byte-boundary
  assumption -- finding it locks alignment onto real content rather than a guessed reset
  point. See `on_clk_edge_()` in the component source.
- Right after boot, or after a physical disturbance to the wiring (e.g. the door being
  opened/closed while the ESP32 is tucked inside the panel), the capture can briefly produce
  oversized/garbled reads before the header-sync locks onto real content again. These are
  discarded (see [Framing validity check](#framing-validity-check--the-unsolved-checksum)).
- Even with interrupt-driven capture, isolated single-bit (occasionally double-bit)
  corruption still occurs at a low rate (roughly 1 in 15-20 frames), most visible in the
  otherwise-constant framing bytes -- but it can land anywhere. Confirmed via Home Assistant
  history: `remaining_time` showed brief round-trip spikes (e.g. `142 -> 202 -> 142` within
  100ms, a single flipped bit in the hours byte and back) from frames that passed the framing
  check but still carried corruption in a field the check doesn't cover. The sniffer now
  requires a changed value to be seen twice in a row before publishing it, which filters this
  out without needing the (uncracked) checksum.

## Frame format (MOSI direction, mainboard -> panel)

This is the frame the hardware currently captures — a 22-byte status update sent from the
mainboard to the panel roughly 10 times per second. All byte offsets are 0-indexed.

| Offset | Meaning | Values |
|---|---|---|
| 0-2 | Constant framing | always `02 10 2B` |
| 3-5 | Mostly constant ("fixed") | `00 00 00` in almost every capture; rare single-frame blips to `01` seen during heavy button-mashing testing, meaning unconfirmed (possibly delayed-start button transients) |
| 6-7 | **Remaining time**, hours:minutes | byte 6 = hours, byte 7 = minutes, **plain binary, not BCD** (see [below](#time-format)) |
| 8 | Partially decoded | `0x05` while idle, `0x07` while running — a secondary running indicator, exact meaning of other values unconfirmed |
| 9 | Unknown | always `0xFF` in every capture so far |
| 10 | **Beeper / sound code** | `0x64` Stop, `0x65` Bip-Bop (start/option chirp), `0x6B` Bop-Bip, `0x69` Long beep |
| 11 | **Current program + options bitfield** | see [table below](#program--options-bitfield-byte-11) |
| 12 | **Start/Stop LED state** | `0x0E` Solid, `0x0C` Flashing, `0x00` Off |
| 13 | Timer / other | almost always `0x00`; unconfirmed bit meanings noted in [Open questions](#open-questions) |
| 14 | **Power state** | `0x00` OFF, `0xFF` ON |
| 15 | **Program running** | `0x00` not running, `0x01` running (a `0x02` was seen once, in a corrupted/desynced capture around a capsule-drop event — unconfirmed, see [Open questions](#open-questions)) |
| 16-17 | Unknown | always `0x00`; one unconfirmed single-frame blip to `01` at offset 17 |
| 18 | Constant framing | always `0xAC` |
| 19 | **Checksum** | real, but algorithm not cracked — see below |
| 20-21 | Constant framing | always `BD 99` |

### Program + options bitfield (byte 11)

Verified live against the physical panel -- this corrected the source notes this project
started from twice over: they initially had bits 5 and 6 swapped, and separately claimed
bit 5 was Half Load and bit 7 unused. Bit 5 never once appeared set in any real capture; a
live before/after toggle instead showed bit 7 flip in lockstep with Half Load on the panel:

| Bit | Mask | Meaning |
|---|---|---|
| 0 | `0x01` | 70°C Intensive |
| 1 | `0x02` | Eco |
| 2 | `0x04` | 65°C |
| 3 | `0x08` | 60°C Quick Shine |
| 4 | `0x10` | 35°C Mini |
| 5 | — | never observed set; unknown |
| 6 | `0x40` | Extra Rinse option |
| 7 | `0x80` | Half Load option |

All of bits 0-4 clear means no program selected ("Off").

### Machine state: Off / Idle / Draining / Running

`power_on` (byte 14) and `running` (byte 15) alone can't fully describe what the machine is
doing: both are `power_on=true, running=false` for two very different real situations --
freshly powered on with nothing started yet, and a cycle that just finished but hasn't fully
shut down. The **LED byte (12)** is what tells them apart. Confirmed by watching a complete
end-of-cycle sequence live in Home Assistant history:

```
remaining_time = 1 min                              (actively running)
remaining_time = 0 min, running -> false, LED -> Flashing     (cycle ends, draining begins)
                    ~80 seconds later
power_on -> false, program -> Off, LED -> Off, remaining_time -> idle preview value
```

So `running` drops to false **the instant** the displayed countdown hits `0:00`, but the
machine stays powered on with the Start/Stop LED **flashing** for about 80 more seconds
(very likely draining residual water before the machine considers the cycle truly complete)
before `power_on` finally drops. The derived `status` entity encodes this as four states:

| `status` | Condition |
|---|---|
| `Off` | `power_on = false` |
| `Running` | `power_on = true`, `running = true` |
| `Draining` | `power_on = true`, `running = false`, LED = Flashing (`0x0C`) |
| `Idle` | `power_on = true`, `running = false`, LED != Flashing (freshly on / nothing started) |

### Time format

Bytes 6-7 display as **H:MM**, confirmed by watching a live countdown during an actual wash
cycle. The proof it's plain binary and not BCD: the minutes byte was caught rolling over from
`0x11` to `0x0F` with no frame ever showing `0x10` in between over that transition window —
in BCD, "10 minutes" would be stored as `0x10` and the next tick would jump straight to
`0x09` (BCD has no valid representation for `0x0A`-`0x0F`). Seeing a stable `0x0F` rules that
out. While idle/browsing programs, this field shows the *estimated total duration* for the
currently selected program + options rather than a countdown; it only starts decrementing
once the cycle is actually running.

Checked against a full real wash cycle's Home Assistant history: the value went from 113 min
to 0 min over ~111 real minutes elapsed -- essentially exact 1:1 real-time tracking for the
entire cycle, not a rough estimate that drifts. One legitimate mid-cycle jump was observed
(`46 -> 42 -> 41 -> 46 -> 45 min` over ~45 seconds, distinguishable from corruption noise by
not being an instant single-sample round-trip) that doesn't fit the corruption pattern --
possibly the mainboard re-estimating total cycle time based on a soil/turbidity sensor
reading, but not confirmed or reproduced yet.

### Framing validity check & the unsolved checksum

Byte 19 **is** a genuine deterministic checksum — this was confirmed by grouping ~100 real
captured frames by their leading bytes and finding the checksum byte was consistent across
repeated identical content in the large majority of cases (the remaining inconsistent cases
line up with the known ~1-in-15-20 single-bit-corruption rate). However, an exhaustive
brute-force search against that dataset found no match for:

- Plain XOR, additive sum, two's complement, one's complement — over every possible
  contiguous byte range and both `0x00`/`0xFF` seeds
- 12 common CRC-8 polynomials, both normal and bit-reflected
- Fletcher-8
- Weighted (position-multiplied) sum
- Rotate-XOR

The best result across all of these was 6/94 frames matching — statistically indistinguishable
from chance. This strongly suggests byte 19 uses a proprietary lookup table baked into the
mainboard firmware, which isn't recoverable from blackbox SPI captures alone (would need the
firmware image itself, or a MISO-side capture in case the checksum incorporates data the panel
sends back).

Since the checksum can't be validated, the sniffer instead validates frames against the
**constant framing bytes** (0, 1, 2, 18, 20, 21 — always `02 10 2B ... AC ... BD 99`), which
in practice catches essentially all corruption seen so far. Failed frames are simply
discarded; there's no separate resync step needed since byte alignment is continuously
re-derived from the header content itself (see [Bus timing](#bus-timing)), not from any
stateful assumption that could drift and need correcting.

A frame can still occasionally pass this check while carrying an isolated bit flip in a
field the check doesn't cover (e.g. the time bytes) -- observed live as brief round-trip
spikes in Home Assistant history. Every published value therefore also has to be seen twice
in a row before the sniffer commits to it, which filters this out without needing the
checksum.

## Unimplemented: MISO frame

The hardware only taps CLK and MOSI, so it never sees the mainboard<-panel direction. Prior
notes on this project (not verified against live captures) describe a separate, sparser
22-byte MISO-direction frame carrying raw button-press state as a 16-bit bitmask:

| Value | Button |
|---|---|
| `0x0100` | Power |
| `0x0200` | 70°C |
| `0x0400` | Eco |
| `0x0800` | 65°C |
| `0x1000` | 60°C Quick Shine |
| `0x2000` | 35°C |
| `0x8000` | Extra water |
| `0x0001` | Half load |
| `0x0002` | Timer delay |
| `0x0004` | Start/Stop |

Those notes also claim a checksum formula of `(-sum_of_preceding_bytes) & 0xFF` for this
direction specifically — untested, since no MISO wiring/captures exist yet. Wiring up MISO
would be needed to confirm or refute this, and would also settle whether the MOSI checksum
incorporates any MISO-side data.

## Open questions

- **Checksum algorithm (byte 19)** — not cracked; see above.
- **Bytes 3, 5, 13, 17** — each showed one unconfirmed single-frame value change during
  testing (delayed-start button interaction is the leading guess for some of these), never
  reproduced cleanly enough to pin down.
- **Byte 15 value `0x02`** — seen once in a corrupted/desynced frame around a detergent
  capsule drop. Possibly a genuine "dispensing" sub-phase distinct from plain
  running/not-running, but the surrounding frame was desynced so this isn't confirmed.
- **Byte 8, byte 13** — partially understood at best (see table).
- **MISO direction** — entirely unverified against live data (see above).

## ESPHome component

The `beko_dishwasher` custom component (`components/beko_dishwasher/`) exposes the decoded
fields as standard ESPHome entities, auto-discovered into Home Assistant via the `api:`
integration once flashed.

```yaml
external_components:
  - source:
      type: local
      path: components
    components:
      - beko_dishwasher

beko_dishwasher:
  clk_pin: 3
  mosi_pin: 4
  program:
    name: Program
  remaining_time_minutes:
    name: Remaining Time          # numeric sensor (minutes), device_class: duration
  remaining_time:
    name: Remaining Time (H:MM)   # human-readable text
  status:
    name: Status                  # derived enum: Off / Idle / Draining / Running
  led_state:
    name: Start/Stop LED          # raw LED decode: Solid / Flashing / Off
  power_on:
    name: Power On                # device_class: power
  running:
    name: Running                 # device_class: running
  half_load:
    name: Half Load
  extra_rinse:
    name: Extra Rinse
  raw_mosi:
    name: Raw MOSI Frame          # diagnostic: raw hex dump of the last frame, for debugging
```

All fields are optional — omit any you don't want exposed. `raw_mosi` is marked as a
diagnostic entity in Home Assistant since it's mainly useful for debugging new firmware, not
day-to-day dashboard use.

Every entity only publishes when its value actually changes (frames arrive ~10/s, but real
state changes far less often) — without this, Home Assistant's recorder would be flooded
with thousands of duplicate states per minute.
