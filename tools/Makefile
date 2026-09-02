# SPDX-License-Identifier: GPL-2.0
# Makefile -- kullanici alani dogrulama araclari

CC      ?= arm-linux-gnueabihf-gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
TARGET  ?= pi@dualnic-target

BIN := dnhat_frametest

all: $(BIN)

$(BIN): dnhat_frametest.c
	$(CC) $(CFLAGS) -o $@ $<

# Gelistirme bilgisayarinda yerel derleme (hizli sozdizimi kontrolu icin)
native:
	gcc $(CFLAGS) -o $(BIN) dnhat_frametest.c

deploy: $(BIN)
	scp $(BIN) $(TARGET):/home/pi/

clean:
	rm -f $(BIN)

.PHONY: all native deploy clean
