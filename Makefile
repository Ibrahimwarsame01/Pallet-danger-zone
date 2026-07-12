# PalletGuard build
#
# QNX cross-build (the real target):
#   source <qnx-sdp-dir>/qnxsdp-env.sh     # sets QNX_TARGET etc.
#   make                                   # ./palletguard (needs every module implemented)
#   make test_frame_source                 # Role A standalone smoke test / recorder
#
# Host build (no QNX env): builds test_frame_source with the live camera stubbed
# out, so recorded fallback footage can be tested on a dev machine.
# Needs OpenCV dev headers (only opencv_core is linked for the frame source).
#
# If OpenCV lives somewhere else, override: make OPENCV_INC=... OPENCV_LIB=...

ifdef QNX_TARGET
CC         := qcc -Vgcc_ntoaarch64le
PLATLIBS   := -lcamapi
OPENCV_INC ?= $(QNX_TARGET)/usr/include/opencv4
OPENCV_LIB ?= $(QNX_TARGET)/aarch64le/usr/lib
else
CC         := cc
PLATLIBS   := -lpthread
OPENCV_INC ?= /usr/include/opencv4
OPENCV_LIB ?= /usr/lib
endif

CFLAGS  += -Wall -O2 -Iinclude -Isrc/frame_source -I$(OPENCV_INC)
LDFLAGS += -L$(OPENCV_LIB)

# Full app links whatever each role uses; trim if a module drops a dependency.
OPENCV_LIBS ?= -lopencv_core -lopencv_imgproc -lopencv_objdetect

FRAME_SRC := src/frame_source/frame_source.c \
             src/frame_source/live_camera.c \
             src/frame_source/fallback_source.c

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

clean:
	rm -f palletguard test_frame_source *.o

.PHONY: all clean
