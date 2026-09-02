#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# dnhat_debug.sh -- dinamik hata ayiklamanin calisma aninda acilmasi
#
# Surucu gelistirmede hata ayiklama imkanlari kullanici alanina gore cok
# kisitlidir; hata ayiklayici ile adim adim ilerlemek pratik degildir.
# Cekirdegin dinamik hata ayiklama mekanizmasi sayesinde ayrintili gunluk
# satirlari modul yeniden derlenmeden calisma aninda acilip kapatilabilir.
#
# Kullanim:
#   sudo ./dnhat_debug.sh on          -> modulun tum dev_dbg() satirlari
#   sudo ./dnhat_debug.sh rx          -> yalnizca RX yolundaki satirlar
#   sudo ./dnhat_debug.sh off         -> hepsini kapat
#   sudo ./dnhat_debug.sh status      -> gozlem noktalarini dok (Tablo 5.2)

set -euo pipefail

MODULE=dnhat_w5500
CONTROL=/sys/kernel/debug/dynamic_debug/control

need_root() {
	if [ "$(id -u)" -ne 0 ]; then
		echo "bu betik root olarak calistirilmalidir" >&2
		exit 1
	fi
}

case "${1:-status}" in
on)
	need_root
	echo "module $MODULE +p" > "$CONTROL"
	echo "$MODULE: tum hata ayiklama satirlari acildi"
	;;
rx)
	need_root
	echo "func dnhat_rx_process +p" > "$CONTROL"
	echo "func dnhat_irq_thread +p" > "$CONTROL"
	echo "$MODULE: RX yolundaki satirlar acildi"
	;;
tx)
	need_root
	echo "func dnhat_tx_work +p" > "$CONTROL"
	echo "func dnhat_start_xmit +p" > "$CONTROL"
	echo "$MODULE: TX yolundaki satirlar acildi"
	;;
off)
	need_root
	echo "module $MODULE -p" > "$CONTROL"
	echo "$MODULE: hata ayiklama kapatildi"
	;;
watch)
	need_root
	dmesg -wH
	;;
status)
	# Tablo 5.2: surucu gelistirme boyunca kullanilan gozlem noktalari
	echo "=== Modul yuklu mu? ==="
	lsmod | grep -E "^${MODULE}|^Module" || echo "  (modul yuklu degil)"

	echo
	echo "=== Surucu kayitli mi? (/sys/bus/spi/drivers) ==="
	ls /sys/bus/spi/drivers/ 2>/dev/null || true

	echo
	echo "=== Hangi cihazlarla eslesti? ==="
	ls -l "/sys/bus/spi/drivers/${MODULE}/" 2>/dev/null \
		| grep spi || echo "  (eslesme yok)"

	echo
	echo "=== SPI cihazlari ve Device Tree dugumleri ==="
	for d in /sys/bus/spi/devices/*; do
		[ -e "$d" ] || continue
		printf '  %-12s modalias=%s\n' \
			"$(basename "$d")" "$(cat "$d/modalias" 2>/dev/null)"
		if [ -r "$d/of_node/compatible" ]; then
			printf '  %-12s compatible=%s\n' "" \
				"$(tr -d '\0' < "$d/of_node/compatible")"
		fi
	done

	echo
	echo "=== Kesme hatti gercekten tetikleniyor mu? ==="
	grep -iE "dnhat|eth[12]" /proc/interrupts || echo "  (kesme kaydi yok)"

	echo
	echo "=== Arayuz sayaclari ==="
	grep -E "eth1|eth2" /proc/net/dev || true

	echo
	echo "=== Son surucu gunlukleri ==="
	dmesg | grep -i dnhat | tail -20 || true
	;;
*)
	echo "kullanim: $0 {on|rx|tx|off|watch|status}" >&2
	exit 1
	;;
esac
