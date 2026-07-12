# PalletGuard build
#
# QNX cross-build (the real target):
#   source <qnx-sdp-dir>/qnxsdp-env.sh     # sets QNX_TARGET etc.
#   make                                   # ./palletguard (needs every module implemented)
#   make test_frame_source                 # Role A standalone smoke test / recorder
#   make gpio_test                         # Role D GPIO wiring bring-up (run first!)
#
# Host build (no QNX env):
#   make test_frame_source   # needs OpenCV dev headers (override OPENCV_INC/OPENCV_LIB)
#   make test                # Role D unit tests (ASan/UBSan); no OpenCV install
#                            # needed — tests/host_shim satisfies the header include
#   make gpio_test           # GPIO tool in console-simulation mode
#
# If OpenCV lives somewhere else, override: make OPENCV_INC=... OPENCV_LIB=...

ifdef QNX_TARGET
CC         := qcc -Vgcc_ntoaarch64le
PLATLIBS   := -lcamapi
OPENCV_INC ?= $(QNX_TARGET)/usr/include/opencv4
OPENCV_LIB ?= $(QNX_TARGET)/aarch64le/usr/lib
SAN        :=
else
CC         := cc
PLATLIBS   := -lpthread
OPENCV_INC ?= /usr/include/opencv4
OPENCV_LIB ?= /usr/lib
SAN        := -fsanitize=address,undefined -fno-omit-frame-pointer
endif

CFLAGS  += -Wall -O2 -Iinclude -Isrc/frame_source -Isrc/alerts -I$(OPENCV_INC)
LDFLAGS += -L$(OPENCV_LIB)

# Full app links whatever each role uses; trim if a module drops a dependency.
OPENCV_LIBS ?= -lopencv_core -lopencv_imgproc -lopencv_objdetect

FRAME_SRC := src/frame_source/frame_source.c \
             src/frame_source/live_camera.c \
             src/frame_source/fallback_source.c

ALERTS_SRC := src/alerts/gpio_alert.c \
              src/alerts/overlay.c \
              src/alerts/logger.c

APP_SRC := src/main.c \
           $(wildcard src/detection/*.c) \
           $(wildcard src/instability/*.c) \
           $(wildcard src/zone/*.c) \
           $(wildcard src/alerts/*.c) \
           $(FRAME_SRC)

all: palletguard

palletguard: $(APP_SRC) include/common_types.h
	$(CC) $(CFLAGS) -o $@ $(APP_SRC) $(LDFLAGS) $(OPENCV_LIBS) $(PLATLIBS) -lm

test_frame_source: $(FRAME_SRC) tests/test_frame_source.c include/common_types.h
	$(CC) $(CFLAGS) -o $@ $(FRAME_SRC) tests/test_frame_source.c $(LDFLAGS) -lopencv_core $(PLATLIBS) -lm

# ---- Role D (alerts) ----
# tests/host_shim satisfies common_types.h's OpenCV include on machines with
# no OpenCV headers; the real headers ($(OPENCV_INC)) win when present.
SHIM_INC := -Itests/host_shim

test_alerts: $(ALERTS_SRC) tests/test_alerts.c $(wildcard src/alerts/*.h) include/common_types.h
	$(CC) $(CFLAGS) -Wextra -Werror $(SHIM_INC) $(SAN) -g -o $@ $(ALERTS_SRC) tests/test_alerts.c

test: test_alerts
	./test_alerts

# GPIO bring-up tool; links libc only, so it builds even without OpenCV.
gpio_test: src/alerts/gpio_alert.c scripts/gpio_test.c include/common_types.h
	$(CC) $(CFLAGS) -Wextra -Werror $(SHIM_INC) -o $@ src/alerts/gpio_alert.c scripts/gpio_test.c

clean:
	rm -f palletguard test_frame_source test_alerts gpio_test *.o
	rm -rf out

.PHONY: all test clean
