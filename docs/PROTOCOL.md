# Voltra BLE protocol notes

What Voltra Remote knows about the Beyond Power Voltra I's Bluetooth protocol, and how it
was established. It builds on community reverse-engineering (see
[Credits](../README.md#credits)); everything marked *captured* was confirmed against a
real Voltra I with this project's diagnostic build.

## Transport

The Voltra exposes one GATT service, `E4DADA34-0867-8783-9F70-2CA29216C7E4`.

| Characteristic | Use |
|---|---|
| `A010891D-F50F-44F0-901F-9A2421A9E050` | commands are written here (it also notifies) |
| `55CA1E52-7354-25DE-6AFC-B7DF1E8816AC` | responses and telemetry (notify) |
| `CA94658C-0525-5046-E78B-5391B65F47AD` | notify |

Devices advertise as `VTR-…`. Every frame is:

```
55 | len | type | crc8 | AA 10 | seq(2) | 20 00 | cmd | payload | crc16(2)
```

- `crc8` covers the first three bytes: init `0xEE`, poly `0x31`, reflected.
- `crc16` covers everything before it: init `0x496C`, poly `0x1021`, reflected.
- `AA 10` is sender (app) and receiver (Voltra); `20 00` is the protocol id.
- Frames can arrive split across notifications, or several to one notification.

On connect the app must send its "app hello" (command `0x4F`) within a short window,
followed by the bootstrap sequence captured from the official app. Only then does the
Voltra answer reads and accept writes.

| Command | Meaning |
|---|---|
| `0x0F` | read parameters: `count(2)` then ids; the reply carries `(id, value)` pairs |
| `0x10` | asynchronous push from the Voltra, same `(id, value)` layout |
| `0x11` | write one parameter: `01 00 id(2) value` |
| `0x4F` | app hello |
| `0xAA` | vendor channel: app → Voltra sub-commands, Voltra → app workout telemetry |

Values are little-endian. A parameter's width comes from the vendor dictionary
(`paramInfo.csv` in the Android port); ids missing from it have to be inferred.

## Parameters

| Setting | Parameter | Value |
|---|---|---|
| Target weight | `0x3E86` | u16 lb, 5 to the ceiling below |
| Weight ceiling | `0x541E` | u16 lb (`OVERDRIVE_USER_CFG_FORCE_MAX`); not answered by all firmware, 230 lb assumed then |
| Load / unload | `0x3E89` | u16: write `5` to load, `4` to unload (see [Loading](#loading)) |
| Enter weight training | `0x4FB0` | u8 `1` |
| Chains amount | `0x54DA` / `0x3E87` | i32 percent of base × 100 / u16 lb (see [Units](#chains-and-eccentric-units)) |
| Eccentric amount | `0x53D0` / `0x3E88` | i32 percent of base × 100 / i16 lb |
| Chains / eccentric limits | `0x54D4` / `0x5319` | u8 percent (100 / 60 on current firmware) |
| Inverse chains | `0x53B0` | u8 `0` / `1` |
| Mountain | `0x556F` | u8 `1` = mountain, `0` = plain chains (undocumented) |
| Accessory unit | `0x5569` | u8 `1` = percent, `0` = pounds (undocumented) |
| Battery | `0x4E2D` | u8 percent |
| Auto load | command `0xAA`, payload `12` | then poll `0x53C7` and `0x53C8` (see [Auto load](#auto-load)) |

Chains, inverse chains and mountain are one accessory in three styles. They share one
amount (`0x54DA`), and the two flags pick the style, so only one can be on at a time.
Toggling Mountain on the Voltra itself moves `0x54DA`, which is what shows the amount is
shared rather than held per style. The knob writes the flags before the amount, so the
Voltra never briefly applies a new amount in the old style.

`0x556F`, `0x5569` and `0x53C7` are never pushed asynchronously, so they have to be
polled.

## Loading

`0x3E89` does not jump straight to the value written. *Captured* read-backs have three
values:

| Read | Meaning |
|---|---|
| `0` | unloaded |
| `1` | loaded, idle: just loaded, or resting between sets |
| `5` | loaded, set active |

`1` is easy to mistake for "unloaded", but the Voltra holds the weight there. Loading from
`0` steps through `1` first, so the knob re-issues the write every 600 ms (up to four
times) until it reads `5`. Unloading (writing `4`) lands on `0`.

Quirks handled alongside:

- After a settings write, and after reps, the Voltra echoes its whole settings block
  asynchronously, and the fitness mode in that echo is stale. *Captured* 100 ms apart
  while loaded: a direct read said `5`, the echo said `1`. A single-parameter push is a
  genuine change and is honoured; a multi-parameter one is an echo and its mode ignored.
- Changing the weight or an accessory during an active set drops the mode from `5` to
  `1`. The knob then re-asserts `5` so the change applies to the set in progress. It does
  not do this at `1` (resting), so a change between sets does not end the rest.

## Workout telemetry

The Voltra pushes workout telemetry on command `0xAA`. Two frames matter here:

- **Rep telemetry** `81 2B phase set rep_hi rep_lo …`: `phase` is `0` idle, `1` pull,
  `2` transition, `3` return; `set` is the Voltra's set counter; the rep count is
  big-endian.
- **Workout status** `80 25 01 00 SS …`, pushed the moment it changes (*captured*):

  | `SS` | Meaning |
  |---|---|
  | `0` | unloaded |
  | `1` | loading |
  | `2` | set in progress (also right after loading, before any rep) |
  | `3` | resting, about 3.5 s after the last rep |
  | `4` | unloading |

The fitness mode cannot separate a set from a rest: after the first set it stays at `1`
through every later set, and through all of them when the Voltra was loaded from its own
screen. The knob's set screen therefore keys off status `2` plus cable movement from the
rep telemetry, and ends on any other status.

## Auto load

The Voltra's own auto load ("direct load" in the Android port) is started with vendor
command `0xAA`, payload `12`, after making sure weight training is active (`0x4FB0`).
The Voltra then waits for the cable to be pulled out and held, counts down, and loads.

*Captured* on current firmware, the fitness mode goes to `0x23` (`35`) and **stays there
after loading**, so the mode alone cannot say whether the weight is on.
`DIRECT_LOAD_SAFETY_CHECK_ST` (`0x53C7`) does:

| `0x53C7` | Meaning |
|---|---|
| `11` | waiting for the cable to be pulled out |
| `12` | held: counting down (`0x53C8`, 3000 ms to 0, restarting if the cable moves) |
| `13` | countdown done, weight coming on |
| `14` | loaded |

The knob counts `0x23` with `13` or `14` as loaded. Writing `4` (unload) during the wait
cancels it. The Android port lists fitness modes `0x26` / `0x27` for this on its
firmware; those are treated the same way.

### Loading with the cable out

*Captured* (Watch 2.06 diag build):

- A plain load (`0x3E89` = `5`) with the cable pulled out (41 cm) is refused: the mode
  echoes `4` and falls back to `0` within ~200 ms, every retry. No safety-check register
  (`0x53C7` / `0x53C8` / `0x53C9`) changes, so the Voltra gives a remote nothing to
  answer. The same load with the cable near home (11 cm) goes through.
  A refused load already reads back as `4` at the first check (~0.6 s after the write);
  loads that go through read `0`, `1` or `5` there, so the watch calls it refused then.
- The Voltra's own "cable out" prompt, answered with **override** on its screen, shows
  up only as the mode going `4` -> `1` (loaded) with the cable at 31 cm. `0x53C7` and
  `0x53C9` stay `0`. **Cancel** changes nothing visible.
- Auto load with the cable out works: `0x53C7` `11` -> `12` (countdown `0x53C8` from
  ~2200 ms) -> `13` -> `14`.
- Writing mode `1` after a refusal, to copy the Voltra's own override, is refused too:
  the Voltra answers with mode `0` (cable at 53-58 cm, captured twice). A remote cannot
  override the cable-out check this way.
- `DIRECT_LOAD_SAFETY_CHECK_CTRL` (`0x53C9`, "cancel / bypass") is writable and reads
  back. Writing `1` or `2` before a plain load changes nothing: the load is still
  refused. Writing `1` during an auto-load countdown **cancels** the auto load at once
  (`0x53C7` `12` -> `0`, mode `0x23` -> `0`).
- **Override from a remote** (`Client::loadOverride()`): start auto load, then write `2`
  (**bypass**) while it waits (`0x53C7` = `11`). The Voltra skips the pull-and-hold
  countdown and loads at once: `0x53C7` `11` -> `14`, mode `0x23` -> `1`, about 0.2 s
  after the write, with the cable at 43 cm (captured).

The link has dropped (reason 520, supervision timeout) in every watch capture, with and
without the diagnostic register sweep (`VOLTRA_DIAG_SWEEP=0` in `watch206_diag`).

## Chains and eccentric units

The Voltra keeps chains and eccentric in both pounds and percent, but only one pair is
authoritative: whichever its own accessory lb/% setting (`0x5569`) selects. The other pair
is derived from it and the base weight, and a write to the derived pair is ignored or
reverted. *Captured* in percent mode, with a 40 lb base:

```
41.9s  tx write 3E87 = 0b00      write chains = 11 lb
42.2s  rx 10  0100873e0b00       Voltra acknowledges 11
42.4s  rx 0f  ... 873e0a00 ...   read back: 10 again (25% of 40 lb)
```

So the knob reads `0x5569`, dials and writes in that unit, and shows the other alongside.

Two traps from the vendor dictionary:

- It lists 16-bit percent variants (`0x5189` / `0x518A`). They stay `0`; the signed 32-bit
  pair (`0x54DA` / `0x53D0`) is live.
- `WEIGHT_TRAINING_EXTRA_MODE` (`0x53C6`) sounds like the unit selector but is not: it
  stayed `0` in both modes.

## Finding undocumented registers

Mountain, the accessory unit and parts of auto load are newer than the vendor
dictionary. The dictionary's highest id is `0x5522`, yet reading the *unlisted* ids in
`0x5300`–`0x57FF` turns up 38 live registers.

The diagnostic build finds them:

```bash
pio run -e remote_diag -t upload
```

It logs every parameter read and write and all workout telemetry, and periodically sweeps
the registers listed in `src/voltra/sweep_params.cpp`. `tools/gen_sweep_params.py`
generates that list from the vendor dictionary, in one of three modes:

- `documented`: every BP/MC id.
- `undocumented`: the unlisted ids in `0x5300`–`0x57FF`.
- `settings`: the BP ids followed by the undocumented range.

The method:

1. Capture the serial log while holding each device state long enough for a full sweep.
   A sweep takes about 50 s.
2. Diff the snapshots. Undocumented registers have unknown widths, so they are read in
   small batches and the widths inferred from where each next id falls in the response.
3. **Confirm what each value means by writing it** and watching the Voltra's own screen.
   The diff only identifies the register: Mountain's polarity was first read backwards.

Flash the release build (`pio run -e remote -t upload`) afterwards; it carries none of this.

## Twin mode

*Captured* with two units, VTR-002166 hosting the twin and VTR-066162 following:

- While hosting, the host **stops advertising** and does not accept connections, so a
  remote cannot reach it after the twin is set up.
- The follower keeps advertising (same name and service) and accepts a connection, but
  commands sent to it act on the follower only: a load through it lit the follower's
  twin icon and left the host unchanged. The follower does not relay to the host.
- Two registers on the follower change with twinning (the host was read only before
  it hosted, when both were zero):

  | Register | Not twinned | Follower, twinned |
  |---|---|---|
  | `0x516C` (u8) | `00` | `02` (twin role? 1 = host would fit) |
  | `0x521F` (4 bytes) | `00000000` | `01 39 00 02` |

A unit also refuses to twin from its own screen while a remote is connected to it.

### How Beyond+ twins (iPhone PacketLogger capture)

The app connects to **both** units (normal handshake on each), then:

1. To the future **follower** only: vendor-style command `0xA8`, payload `01` + host
   address (6 bytes, as printed: `80 b5 4e 07 02 a6`). Reply `00 01 00`; the follower then
   drops the app and joins the host itself.
2. The app **keeps its connection to the host**. A hosting unit stops advertising but keeps
   connections it already has, which is how the app can still drive it.
3. The host reports the twin unprompted. Command `0xA7` (twin status, empty request) answers
   `39 01 SS <own addr> <peer addr> ...`: `SS` = `00` alone, `11` then `12` once the
   follower is in, with the follower's address as peer. It also pushes `0x521F` =
   `01 39 00 02`.
4. `0xA9 01` / `0xA9 03` to the host return the follower's name (`VTR-066162`) and serial:
   queries, not part of the setup.

Driving the pair, everything to the host:

- Weight: the usual `0x3E86`.
- Load: fitness mode **`0x0101`**; unload: **`0x0100`** (single unit: `0x0005` / `0x0004`).
  The host echoes mode `1` / `0`.
- While loaded the app sends `0xAA 13 01` every 0.5 s (answered `00 13 00`); purpose unknown.

Un-twin: to the host, `0xA8` with `02` + follower address (reply `00 02 00`).

Twinned, the host's fitness mode changes shape (captured from the watch):

| | Single unit | Twinned host |
|---|---|---|
| Idle / unloaded | `0` or `4` | `0x0100` |
| Loaded | `1` / `5` | `0x0101` (echoed at once on an accepted load) |
| Refused load (cable out) | settles at `4` | `0x0100` -> `0`, no `0x0101` echo |
| Auto load | `0x23` | `0x2003` |
| Auto load finished (`0x53C7`) | `14` | `15` |

The cable-out bypass (`0x53C9` = `2` while auto load waits) is the same.
