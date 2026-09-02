# SPDX-License-Identifier: GPL-2.0
# Makefile -- dnhat_w5500 cekirdek modulu (agac disi / out-of-tree)
#
# Hedef sistem ARM mimarisinde bir gomulu Linux karti oldugundan modul,
# gelistirme bilgisayarinda capraz derlenir. Bir cekirdek modulunun,
# derlendigi cekirdek basliklariyla BIREBIR uyumlu olmasi zorunludur.

obj-m += dnhat_w5500.o

KDIR   ?= $(HOME)/kernel/linux
ARCH   ?= arm
CROSS  ?= arm-linux-gnueabihf-
TARGET ?= pi@dualnic-target

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS) clean

# Statik analiz: sparse ve cekirdek kodlama stili denetimi
check:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS) \
		C=2 CF="-D__CHECK_ENDIAN__" modules
	$(KDIR)/scripts/checkpatch.pl --no-tree -f dnhat_w5500.c dnhat_w5500.h

# Modulu hedef karta kopyala
deploy: all
	scp dnhat_w5500.ko $(TARGET):/home/pi/

.PHONY: all clean check deploy
