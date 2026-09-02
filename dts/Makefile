# SPDX-License-Identifier: GPL-2.0
# Makefile -- DualNIC-HAT Device Tree overlay

DTC    ?= dtc
CPP    ?= cpp
KDIR   ?= $(HOME)/kernel/linux
TARGET ?= pi@dualnic-target

OVERLAY := dualnic-hat
SRC     := $(OVERLAY)-overlay.dts

all: $(OVERLAY).dtbo

# Overlay, dt-bindings basliklarini kullandigi icin once on islemciden
# gecirilir, sonra dtc ile derlenir. -@ bayragi, target = <&spi0> gibi
# sembolik referanslarin cozulebilmesi icin __symbols__ dugumunu uretir.
$(OVERLAY).dtbo: $(SRC)
	$(CPP) -nostdinc -I $(KDIR)/include -undef -x assembler-with-cpp $< \
		| $(DTC) -@ -H epapr -I dts -O dtb -o $@ -

deploy: $(OVERLAY).dtbo
	scp $(OVERLAY).dtbo $(TARGET):/tmp/
	ssh $(TARGET) 'sudo cp /tmp/$(OVERLAY).dtbo /boot/firmware/overlays/'

clean:
	rm -f $(OVERLAY).dtbo

.PHONY: all deploy clean
