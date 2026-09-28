# Pallet Danger Zone Alert System

## Project Overview
Real-time pallet safety monitoring system built in C for Raspberry Pi 5 + Camera Module 3 AF running QNX SDP 8.0.

**Detects:**
- Pallet falling — motion detection via frame differencing (`absdiff` / `threshold` / `countNonZero`)
- Pallet leaning — reference-image comparison (occupancy %) + optional angle via `minAreaRect`
- Person entering a fixed danger zone while either condition is active

**Does NOT claim:** structural analysis, fall prediction, or safety certification.

## Platform
- **Hardware:** Raspberry Pi 5 + Camera Module 3 AF
- **OS:** QNX SDP 8.0
- **Language:** C (C99), compiled with `aarch64-unknown-nto-qnx8.0.0-gcc`
- **Vision:** OpenCV C API (QNX-ported, linking confirmed as prerequisite)

## File Structure
```
pallet-danger-zone/
├── config.yaml                    — tunable thresholds (edit before demo)
├── Makefile                       — QNX cross-compilation
├── include/common_types.h         — ALL shared structs/enums — LOCK BEFORE HOUR 1
├── src/
│   ├── main.c                     — entry point, main loop
│   ├── frame_source/              — Role A: camera pipeline + fallback
│   │   ├── frame_source.h         — vtable interface (FrameSource struct)
│   │   ├── live_camera.c/h        — cvCaptureFromCAM
│   │   └── fallback_source.c/h    — cvCaptureFromFile, loops on end
│   ├── detection/                 — Role B: HOG person detection
│   │   ├── person_detector.c/h    — HOG + default SVM via OpenCV C API
│   │   └── tflite_detector.c/h    — STRETCH: TFLite object detection
│   ├── instability/               — Role C: pallet state detectors
│   │   ├── motion_detector.c/h    — frame diff, threshold, countNonZero
│   │   ├── reference_detector.c/h — absdiff vs reference, occupancy %
│   │   └── angle_detector.c/h     — contours, minAreaRect, angle
│   ├── zone/                      — Role C: zone logic
│   │   ├── zone_check.c/h         — point-in-polygon (ray casting)
│   │   └── state_machine.c/h      — NORMAL/HAZARD_ACTIVE/ALARMING transitions
│   └── alerts/                    — Role D: output
│       ├── gpio_alert.c/h         — QNX GPIO pin toggle
│       ├── overlay.c/h            — OpenCV bounding box + text on frame
│       └── logger.c/h             — CSV event logger
├── assets/
│   ├── reference_image.jpg        — capture with scripts/capture_reference.py BEFORE clock starts
│   ├── fallback_footage/          — pre-recorded video files for testing
│   └── demo_notes.md              — demo sequence, talking points
├── tests/
│   ├── test_zone_check.c          — unit: point-in-polygon cases
│   ├── test_instability.c         — unit: motion + reference detector
│   └── test_integration.c         — end-to-end on fallback footage
└── scripts/
    ├── calibrate_zone.py           — click to define zone polygon, prints Point[] to paste into config
    ├── capture_reference.py        — grab a single frame from the camera, saves as reference_image.jpg
    └── gpio_test.c                 — standalone GPIO toggle test (run BEFORE integration)
```

## Team Roles
| Role | Owns | Build Hours |
|------|------|-------------|
| A — Platform/Camera | Camera pipeline, fallback footage, on-call | 0–0.5, then on-call |
| B — Person Detection | HOG detector (OpenCV C API), person points | 0.5–2 |
| C — Instability & Zone | Motion/reference/angle, zone check, state machine | 2–9 |
| D — Alerts/Demo | GPIO wiring, overlay, CSV logger, demo | 8–16 |

## Critical Interfaces (lock before hour 1, defined in common_types.h)
```c
Frame   *frame_source_get_next(FrameSource *src);
Point   *person_detector_get_points(Frame *frame, int *count_out);
bool     zone_check_is_breached(Polygon *zone, Point *points, int count);
```
- `Frame` — `IplImage* image`, `uint64_t timestamp_ms`, `int frame_id`
- `Point` — `int x, y`
- `Polygon` — `Point *vertices`, `int count`
- `SystemState` — `STATE_NORMAL / STATE_HAZARD_ACTIVE / STATE_ALARMING`

## Config Defaults (config.yaml — tune during hours 12–14)
| Parameter | Default |
|-----------|---------|
| `motion_threshold` | 25 |
| `motion_persistence` | 3 frames |
| `reference_occupancy_threshold` | 0.08 (8%) |
| `occupancy_persistence` | 4 frames |
| `angle_deviation_threshold` | 12° |
| `alert_latency_target` | 1.0 sec |

## Schedule Risk — The One Rule
**Angle detection (hours 6.5–9) is the highest-risk task.**
If `angle_detector` is not clean by hour 9 → **drop it immediately** and use occupancy-only mode.
This is a planned fallback, not a failure. Protect the hours after it.

## Build
```sh
# Cross-compile on QNX build host
make all

# Run live
./pallet-danger-zone

# Run on fallback footage
./pallet-danger-zone --fallback assets/fallback_footage/scene.mp4

# Standalone GPIO test (run before full integration)
./gpio_test

# Run tests
make test
```

## Test Plan (compressed — 5 walk-ins each, not 10)
| Test | Condition | Pass |
|------|-----------|------|
| Inactive crossing | 5 passes, no hazard | 0 alerts |
| Active crossing (falling) | Prop knocked over, 5 walk-ins | ≥4 alerts, ≤1s latency |
| Active crossing (leaning) | Prop tilted, 5 walk-ins | ≥4 alerts, ≤1s latency |
| False-trigger | 30s idle, no people | 0 triggers |
| Fallback parity | Recorded footage | Same behavior as live |
| Rehearsal | Full demo sequence | No crashes/restarts |

## Known Limitations (state plainly to judges)
- Motion/reference detectors respond to any meaningful pixel change, not pallet-specifically.
- Angle measurement may fall back to occupancy-only under real lighting conditions.
- 5 walk-ins per scenario vs 10 — noted honestly, not hidden.
- Visual-cue prototype only — not a structural safety system.
