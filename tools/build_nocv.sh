#!/bin/sh
# Build the full PalletGuard app with NO OpenCV, using tools/minicv instead.
# Works both natively on the QNX Pi image (which ships gcc but no OpenCV)
# and on any POSIX host (WSL/Linux) for fallback-footage testing.
#
# Excludes: angle_detector (needs OpenCV contours; occupancy-only fallback
# per PRD), person_detector (needs OpenCV Haar; main.c uses zone presence).
#
# Usage: sh tools/build_nocv.sh   (from the repo root)
set -e

EXTRA_LIBS=""
if [ "$(uname -s)" = "QNX" ]; then
    EXTRA_LIBS="-lcamapi"
fi

mkdir -p out logs

gcc -Wall -Wextra -O2 \
    -Iinclude -Isrc -Isrc/frame_source -Isrc/alerts -Itools/minicv \
    -o palletguard \
    src/main.c \
    src/frame_source/frame_source.c \
    src/frame_source/live_camera.c \
    src/frame_source/fallback_source.c \
    src/instability/motion_detector.c \
    src/instability/reference_detector.c \
    src/zone/zone_check.c \
    src/zone/state_machine.c \
    src/alerts/gpio_alert.c \
    src/alerts/overlay.c \
    src/alerts/logger.c \
    tools/minicv/mini_cv_core.c \
    $EXTRA_LIBS -lm

echo "built: ./palletguard"
