#
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# FETCHTST.TOS, for emu/test.sh. Build from the repo root:
#   STCMD_NO_TTY=1 ST_WORKING_FOLDER=$PWD stcmd make -C emu/test
#

LIB_DIR = ../../target/atarist/src
BUILD_DIR = ./build
DIST_DIR = ./dist
BIN = FETCHTST.TOS

CC = m68k-atari-mint-gcc
CFLAGS = -m68000 -O2 -std=gnu99 -fomit-frame-pointer -I. -I$(LIB_DIR)

.PHONY: all clean

all: $(DIST_DIR)/$(BIN)

$(BUILD_DIR) $(DIST_DIR):
	mkdir -p $@

$(BUILD_DIR)/sidecart_stubs.o: $(LIB_DIR)/sidecart_stubs.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/mdjs.o: $(LIB_DIR)/mdjs.c $(LIB_DIR)/mdjs.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fetchtst.o: fetchtst.c $(LIB_DIR)/mdjs.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(DIST_DIR)/$(BIN): $(BUILD_DIR)/sidecart_stubs.o $(BUILD_DIR)/mdjs.o \
                    $(BUILD_DIR)/fetchtst.o | $(DIST_DIR)
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)
