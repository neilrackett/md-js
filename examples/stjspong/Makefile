#
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# STJSPONG — "Pong Battle: ST vs JS". A self-playing Pong demo where a native
# 68000 AI plays the MD/JS JavaScript worker. Built as STJSPONG.TOS.
#
# Build from the repo root:  STCMD_NO_TTY=1 make examples
# Or standalone:             ST_WORKING_FOLDER=<repo-root> stcmd make -C examples/stjspong
#

LIB_DIR = ../../target/atarist/src
BUILD_DIR = ./build
DIST_DIR = ./dist
BIN = STJSPONG.TOS

CC = m68k-atari-mint-gcc
CFLAGS = -m68000 -O2 -fomit-frame-pointer -I. -I$(LIB_DIR)

.PHONY: all clean

all: $(DIST_DIR)/$(BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(DIST_DIR):
	mkdir -p $(DIST_DIR)

$(BUILD_DIR)/sidecart_stubs.o: $(LIB_DIR)/sidecart_stubs.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/mdjs.o: $(LIB_DIR)/mdjs.c $(LIB_DIR)/mdjs.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/main.o: main.c $(LIB_DIR)/mdjs.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(DIST_DIR)/$(BIN): $(BUILD_DIR)/sidecart_stubs.o $(BUILD_DIR)/mdjs.o \
                    $(BUILD_DIR)/main.o | $(DIST_DIR)
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)
