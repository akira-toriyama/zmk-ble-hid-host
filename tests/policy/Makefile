# Copyright (c) 2026 akira-toriyama
# SPDX-License-Identifier: MIT
#
# Host-side unit tests for the pure zombie-recovery escalation policy (#8 v3).
# No Zephyr toolchain required -- zr_policy.c has no Zephyr deps.
REPO_INCLUDE := $(abspath ../../include)

CC     ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -I$(REPO_INCLUDE)

SRCS := test_runner.c ../../drivers/input/zr_policy.c

.PHONY: test clean
test: test_runner
	./test_runner

test_runner: $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $@

clean:
	rm -f test_runner
