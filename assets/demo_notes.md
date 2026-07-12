# Demo Runbook — Pallet Danger Zone (Role D)

Owner: Role D. Rehearse the full sequence at least once (PRD hours 14–15.5);
freeze everything after 15.5. Nothing new gets wired, tuned, or rebuilt after
the freeze.

---

## 1. Wiring (BCM numbering, physical header pin in parens)

| Output | GPIO | Header pin | Circuit |
|---|---|---|---|
| LED    | GPIO17 | pin 11 | GPIO17 → 330 Ω resistor → LED anode; LED cathode → GND (pin 9) |
| Buzzer | GPIO27 | pin 13 | GPIO27 → active buzzer (+); buzzer (−) → GND (pin 14) |

Alert semantics (from `gpio_alert`):
- **NORMAL** — everything off
- **HAZARD_ACTIVE** (pallet falling/leaning, nobody in zone) — LED slow blink (1 s)
- **ALARMING** (hazard + person in zone) — LED fast blink (250 ms) + buzzer on
- Alarm outputs latch ~1 s after ALARMING ends so short events stay visible.

## 2. Pre-demo checklist (T−30 min)

1. Boot the Pi; confirm the GPIO server is up: `waitfor /dev/gpio` succeeds.
2. **Run `./gpio_test`** (PRD: before the integration slot, never during).
   PASS = LED fast-blinks + buzzer sounds each ALARMING phase, latches ~1 s
   into NORMAL, then quiet. Backend line printed at start ("qnx-msg" expected;
   "devfs-text" fine; "console" means wiring/server problem — fix now).
3. Camera up, frames flowing (Role A pipeline).
4. Danger zone calibrated for the actual floor position (`scripts/calibrate_zone.py`).
5. Reference image captured with the prop staged upright (`scripts/capture_reference.py`).
6. Writable working dir — logger creates `logs/run_*.csv` and prints the path
   at startup; confirm the line appears.
7. Fallback footage present on the device (Role A `fallback_source`).
8. 30 s idle run, nobody near the zone → **zero** alerts (false-trigger check).
9. Reset the prop; state back to NORMAL before judges arrive.

## 3. Demo sequence (~3 min)

| Step | Action | Expected |
|---|---|---|
| Intro (30 s) | State the claim plainly: detects pallet falling/leaning + person entering a fixed danger zone during either. Visual-cue prototype — not structural analysis, not fall prediction, not safety-certified. | — |
| Baseline | Point at overlay: green zone, `STATE: NORMAL`. Walk *outside* the zone. | No alerts |
| Inactive crossing | Walk *through* the zone while the pallet is fine. | **No alert** — this is the differentiator: zone entry alone is not a hazard |
| Falling | Knock the prop over (nobody in zone yet). | HAZARD_ACTIVE: amber overlay, LED slow blink, no buzzer |
| + walk-in | Walk into the zone. | ALARMING within ≤1 s: buzzer + fast LED + red blinking border + `ALERT!` |
| Reset | Restore prop, step out, wait for NORMAL. | Outputs quiet after ~1 s latch |
| Leaning | Tilt the prop against the wall; repeat the walk-in. | Same HAZARD → ALARMING flow |
| Wrap | Open `logs/run_*.csv`: show the `STATE ...->ALARMING ALERT-ON` rows and `uptime_ms` deltas as latency evidence. | — |

## 4. Fallback ladder (decide fast, don't debug live)

1. **Camera dies** → switch to recorded footage (`fallback_source`); behavior
   is identical per the fallback-parity test. Say so; it's planned.
2. **GPIO/wiring dies** → `gpio_alert` auto-falls back to console simulation
   and the overlay still shows the full alarm state. Demo continues on screen.
3. **Angle detector not clean** → occupancy-only mode; overlay shows `ANG:--`.
   State it plainly: "angle fell back to occupancy-percentage, as planned."
4. **App crash** → relaunch (< 10 s); logger starts a new CSV automatically.

## 5. Judge talking points — known limitations (say them, don't hide them)

- Motion/reference detectors respond to any meaningful pixel change in the
  ROI, not specifically pallet danger.
- Angle measurement depends on clean contour segmentation; may run in
  occupancy-only fallback under real lighting.
- Reduced test coverage (5 walk-ins per scenario instead of 10) due to the
  compressed timeline.
- Visual-cue prototype, not a structural safety system.

## 6. Latency evidence from the CSV

Transition rows are always logged and flushed immediately. Latency for a
walk-in = `uptime_ms` of the `ALERT-ON` row minus `uptime_ms` of the frame
where the person entered (person_in_zone flips to 1). Target ≤ 1000 ms.

## 7. Rehearsal (minimum 1 full run, 2 if time allows)

- Time the full sequence; cut intro words, never cut the inactive-crossing step.
- Assign: one person narrates, one operates the prop/walks, one watches the
  overlay + logs (Role D), one on hardware.
- After each run: prop reset, state NORMAL, fresh log noted.
- No crashes/restarts during a run = pass (PRD rehearsal criterion).
