# Copyright (c) 2026 akira-toriyama
# SPDX-License-Identifier: MIT
#
# Host-side unit tests for the pure HID report parser/decoder.
# No Zephyr toolchain required -- a plain C compiler is enough, because the
# code under test (include/zmk_ble_hid_host/hid_report_parser.h and, from M2,
# drivers/input/hid_report_*.c) has no Zephyr dependencies.

REPO_INCLUDE := $(abspath ../../include)

CC     ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -I$(REPO_INCLUDE)

# M2 appends the implementations once they exist:
#   SRCS += ../../drivers/input/hid_report_parser.c
#   SRCS += ../../drivers/input/hid_report_decode.c
SRCS := test_runner.c

.PHONY: test clean
test: test_runner
	./test_runner

test_runner: $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $@

clean:
	rm -f test_runner
