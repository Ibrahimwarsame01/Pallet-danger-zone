# PalletGuard — Pallet Danger Zone Alert System

A camera-based prototype that watches a staged pallet location, automatically detects visible pallet instability (falling or leaning), activates a pre-configured danger zone, and alerts staff and nearby personnel when a person enters that zone while a hazard is active.

Built for QNX SDP 8.0 on a Raspberry Pi 5 with a Raspberry Pi Camera Module 3 AF, in C.

**Product claim:** detects visible pallet-instability cues (falling and leaning) and person entry into a configured danger zone.
**Not claimed:** structural stability analysis, load-bearing assessment, exact fall prediction, or safety certification. This is a visual-cue prototype, not a certified safety system.

---

## How it works

1. Camera captures frames (live) or reads a recorded sequence (fallback).
2. A person detector finds people in each frame.
3. Two independent instability signals run on a fixed region around the pallet:
   - **Motion detection** — frame-differencing, catches active falling.
   - **Reference-image comparison + tilt-angle measurement** — catches a pallet that's already leaning/displaced, even if it's not currently moving.
4. If either instability signal fires, the pre-drawn danger zone activates.
5. If a detected person overlaps the active zone, the system enters an alarming state and fires a terminal alert — a bold, hard-to-miss warning banner printed to stdout — in addition to the on-screen overlay.
6. Every state transition is logged to CSV with timestamps.

State machine: `NORMAL → HAZARD_ACTIVE → ALARMING → cleared`

---

## Hardware

- Raspberry Pi 5 (8GB)
- Raspberry Pi Camera Module 3 AF
- QNX SDP 8.0 (flashed via QSTI)
- Camera mount/stand
- Heatsink/fan for the Pi 5
- A "pallet" prop that can be knocked over and manually tilted

No LEDs, buzzers, or other external alert hardware are required — alerts are printed
directly to the terminal running the app (see [Alerting](#alerting) below).

---

## Repository structure

```
pallet-danger-zone/
├── README.md
├── config.yaml                  # all tunable thresholds + zone coordinates
├── Makefile
├── src/
│   ├── main.c                   # wires all modules together, runs the main loop
│   ├── frame_source/            # camera capture + recorded-footage fallback
│   ├── detection/                # person detection
│   ├── instability/              # motion, reference-image, and angle detectors
│   ├── zone/                     # zone breach check + state machine
│   └── alerts/                   # terminal alert, on-screen overlay, CSV logger
├── include/
│   └── common_types.h           # shared structs (Frame, Point, Polygon, SystemState)
├── assets/
│   ├── reference_image.jpg      # known-good pallet reference photo
│   ├── fallback_footage/        # recorded demo sequence
│   └── demo_notes.md
├── tests/
└── scripts/
    ├── calibrate_zone.py        # click-to-define the danger zone polygon
    └── capture_reference.py     # one-shot script to save reference_image.jpg
```

---

## Setup

### 1. Flash and boot QNX
Flash the QSTI (Quick Start Target Image) for Raspberry Pi 5 to your SD card using Raspberry Pi Imager. Boot the Pi and confirm SSH access.

### 2. Confirm the camera works
```
camera_example3_viewfinder
```
Confirm you get a live image before writing any detection code.

### 3. Build OpenCV for QNX (do this before the event, not during)
Follow `qnx-ports/build-files` instructions for OpenCV + NumPy dependencies. This build is heavy — budget real time and a machine with sufficient RAM. Deploy the resulting libraries to the Pi target ahead of time.

### 4. Capture the reference image and calibrate the zone
```
python3 scripts/capture_reference.py
python3 scripts/calibrate_zone.py
```
Run these once your camera position and pallet prop are in their final staged positions. Re-run if either moves.

### 5. Build the project
```
source <qnx-sdp-dir>/qnxsdp-env.sh   # QNX cross-compile environment
make                                 # builds ./palletguard
make test_frame_source               # standalone frame-source smoke test (Role A)
```
If your OpenCV-for-QNX install lives outside `$QNX_TARGET`, point the Makefile at it:
`make OPENCV_INC=/path/to/include/opencv4 OPENCV_LIB=/path/to/lib`.
Without the QNX environment, `make test_frame_source` builds a host version (fallback
source only) for testing recorded footage on a dev machine.

### 6. Run
```
./palletguard --source=live      # use the real camera
./palletguard --source=fallback  # use recorded footage from assets/fallback_footage/
```
Keep the terminal visible during the demo — that's where alerts appear (see below).

### 7. Fallback footage format
`--source=fallback` plays every `.bmp` / `.ppm` in `assets/fallback_footage/`, sorted by
filename (zero-pad the frame numbers), paced at ~15 fps. Two ways to produce footage:
```
./test_frame_source --live --record assets/fallback_footage --frames 300   # on the Pi
ffmpeg -i demo.mp4 -pix_fmt bgr24 assets/fallback_footage/frame_%05d.bmp   # from any video
```
JPEG is intentionally not supported: OpenCV 4 removed the C-API image codecs, so the
fallback reader decodes BMP/PPM itself with zero extra dependencies.

---

## Alerting

There is no external alert hardware (no LEDs, no buzzer, no alarm). Instead, `terminal_alert`
prints a loud, unmissable banner straight to the terminal running `palletguard` whenever the
system enters the `ALARMING` state, and prints a clear "cleared" message when it drops back to
`NORMAL`. For example:

```
================================================================
   !!! DANGER: PERSON IN ZONE WHILE PALLET UNSTABLE !!!
================================================================
```

This is in addition to (not instead of) the on-screen overlay and the CSV log — all three
alerting channels fire from the same state-machine transition, so during a demo, watch the
terminal as much as the video feed.

---

## Configuration

All tunable values live in `config.yaml`, including:

| Parameter | Default |
|---|---|
| Motion threshold | 25 |
| Motion persistence | 3 frames |
| Reference occupancy threshold | 8% |
| Occupancy persistence | 3–5 frames |
| Angle deviation threshold | 10–15° |
| Alert latency target | ≤ 1.0 sec |
| Zone polygon | set via `calibrate_zone.py` |

Retune all thresholds against your actual demo lighting and prop before relying on them — defaults are starting points, not final values.

---

## Known limitations

- Motion and reference-image detectors respond to any meaningful pixel change in the monitored region — lighting shifts, shadows, or camera vibration can in principle trigger false positives. Mitigated, not eliminated, by requiring the signal to persist over several frames.
- Tilt-angle measurement depends on clean contour segmentation of the pallet shape. If lighting at demo time differs significantly from tuning conditions, the system falls back to occupancy-percentage-only detection for leaning (no angle).
- Person detection accuracy has a real ceiling versus a trained deep-learning model — an intentional tradeoff for lower setup risk and faster development.
- The danger zone is fixed and pre-configured; it is not calculated dynamically from where an instability event occurs.
- Alerting is terminal-only — there is no LED, buzzer, or other physical/remote alarm. Someone needs to be watching the terminal (or the on-screen overlay) for the alert to be noticed.
- This is a prototype demonstrating a perception-to-alert loop, not a certified or production-ready safety system.

---

## Team

| Role | Owns |
|---|---|
| Platform/Camera | Camera pipeline, fallback footage, QNX environment |
| Person Detection | Person detector, bounding boxes/points |
| Instability & Zone | Motion/reference/angle detectors, zone check, state machine |
| Alerts & Demo | Terminal alert, overlay, logging, demo script |

See the project PRD for detailed per-role task breakdowns, build order, and test plan.
