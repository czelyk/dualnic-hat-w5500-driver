# SPDX-License-Identifier: GPL-2.0
# DualNIC-HAT -- ust seviye Makefile
#
# Ortam degiskenleri:
#   KDIR    hedef karta ait cekirdek kaynak agaci (varsayilan ~/kernel/linux)
#   CROSS   capraz derleyici oneki (varsayilan arm-linux-gnueabihf-)
#   TARGET  hedef kart ssh adresi  (varsayilan pi@dualnic-target)

export KDIR   ?= $(HOME)/kernel/linux
export ARCH   ?= arm
export CROSS  ?= arm-linux-gnueabihf-
export TARGET ?= pi@dualnic-target

SUBDIRS := driver dts tools

all: $(SUBDIRS)

driver:
	$(MAKE) -C driver

dts:
	$(MAKE) -C dts

tools:
	$(MAKE) -C tools

deploy: all
	$(MAKE) -C driver deploy
	$(MAKE) -C dts    deploy
	$(MAKE) -C tools  deploy
	scp scripts/*.sh $(TARGET):/home/pi/
	ssh $(TARGET) 'chmod +x /home/pi/*.sh'

check:
	$(MAKE) -C driver check

clean:
	$(MAKE) -C driver clean
	$(MAKE) -C dts    clean
	$(MAKE) -C tools  clean

.PHONY: all $(SUBDIRS) deploy check clean
