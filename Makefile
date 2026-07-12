# Pallet Danger Zone — build
#
# Host (dev machine) — Role D unit tests + tools, no OpenCV install needed
# (include/common_types.h's OpenCV include is satisfied by tests/host_shim):
#   make test        build + run Role D unit tests (ASan/UBSan)
#   make gpio_test   GPIO bring-up tool (console sim on host)
#
# QNX target (run inside an SDP 8.0 environment):
#   make QNX=1 gpio_test                                # no OpenCV needed
#   make QNX=1 OPENCV_INC="-I/path/to/opencv/include" roled
#
# Integration (once all roles' sources are present):
#   make QNX=1 OPENCV_INC=... OPENCV_LIB="-L... -lopencv_core ..." app

CSTD := -std=gnu11
WARN := -Wall -Wextra -Werror
OPT  ?= -O2
INC  := -Iinclude -Isrc/alerts

ifeq ($(QNX),1)
  ifeq ($(origin CC),default)
    CC := qcc -Vgcc_ntoaarch64le
  endif
  SAN :=
else
  SAN := -fsanitize=address,undefined -fno-omit-frame-pointer
endif

# Real OpenCV headers (required for roled/app) win over the host shim;
# the shim only satisfies the include for host tests and gpio_test.
OPENCV_INC ?=
OPENCV_LIB ?=
SHIM_INC   := -Itests/host_shim

CFLAGS := $(CSTD) $(WARN) $(OPT) $(INC)

ALERTS_SRC := src/alerts/gpio_alert.c src/alerts/overlay.c src/alerts/logger.c
ALERTS_HDR := $(wildcard src/alerts/*.h) include/common_types.h

.PHONY: all test run-tests gpio_test roled app clean

all: test

out:
	mkdir -p out

out/test_alerts: $(ALERTS_SRC) tests/test_alerts.c $(ALERTS_HDR) | out
	$(CC) $(CFLAGS) $(SAN) -g $(OPENCV_INC) $(SHIM_INC) $(ALERTS_SRC) tests/test_alerts.c -o $@

test: out/test_alerts
	./out/test_alerts

run-tests: test

out/gpio_test: src/alerts/gpio_alert.c scripts/gpio_test.c $(ALERTS_HDR) | out
	$(CC) $(CFLAGS) $(OPENCV_INC) $(SHIM_INC) src/alerts/gpio_alert.c scripts/gpio_test.c -o $@

gpio_test: out/gpio_test

# Compile Role D objects against the real OpenCV headers (integration check).
roled: | out
	@test -n "$(OPENCV_INC)" || { echo "error: set OPENCV_INC=-I<opencv-include-dir>"; exit 1; }
	mkdir -p out/obj
	$(CC) $(CFLAGS) $(OPENCV_INC) -c src/alerts/gpio_alert.c -o out/obj/gpio_alert.o
	$(CC) $(CFLAGS) $(OPENCV_INC) -c src/alerts/overlay.c -o out/obj/overlay.o
	$(CC) $(CFLAGS) $(OPENCV_INC) -c src/alerts/logger.c -o out/obj/logger.o
	@echo "Role D objects in out/obj/"

# Best-effort integration build; expects every role's sources to compile.
APP_SRC := $(wildcard src/*.c src/*/*.c)
app: | out
	@test -n "$(OPENCV_INC)" || { echo "error: set OPENCV_INC and OPENCV_LIB"; exit 1; }
	$(CC) $(CFLAGS) $(OPENCV_INC) $(APP_SRC) $(OPENCV_LIB) -o out/pallet_danger_zone

clean:
	rm -rf out
