#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# leak_test.sh -- modul yasam dongusu sizinti testi (22. gun, DR-10)
#
# Bir cekirdek modulunun kaldirilirken biraktigi kaynak, kullanici
# alanindaki bir sizintidan cok daha ciddidir: cekirdek bellegi hicbir
# zaman geri alinmaz ve sistem yeniden baslatilana kadar kalici olarak
# kaybedilir.
#
# Kullanim: sudo ./leak_test.sh [tekrar_sayisi] [peer_ip]

set -uo pipefail

ITER=${1:-500}
PEER=${2:-192.168.10.1}
MODULE=dnhat_w5500
KO=${KO:-/home/pi/${MODULE}.ko}

if [ "$(id -u)" -ne 0 ]; then
	echo "bu betik root olarak calistirilmalidir" >&2
	exit 1
fi

slab_kb() { awk '/^Slab:/ {print $2}' /proc/meminfo; }

before=$(slab_kb)
warn_before=$(dmesg | grep -ciE "lockdep|BUG|WARNING|scheduling while" || true)

echo "=== $ITER tekrarli yukle/kaldir testi basliyor ==="
echo "Slab oncesi : ${before} kB"

fail=0
for i in $(seq 1 "$ITER"); do
	if ! insmod "$KO"; then
		echo "insmod basarisiz (tekrar $i)" >&2
		fail=1
		break
	fi

	ip link set eth1 up 2>/dev/null
	ip link set eth2 up 2>/dev/null

	ping -c 1 -W 1 "$PEER" > /dev/null 2>&1

	ip link set eth1 down 2>/dev/null
	ip link set eth2 down 2>/dev/null

	if ! rmmod "$MODULE"; then
		echo "rmmod basarisiz (tekrar $i)" >&2
		fail=1
		break
	fi

	if [ $((i % 50)) -eq 0 ]; then
		printf '  %4d/%d  Slab: %s kB\n' "$i" "$ITER" "$(slab_kb)"
	fi
done

after=$(slab_kb)
warn_after=$(dmesg | grep -ciE "lockdep|BUG|WARNING|scheduling while" || true)

echo
echo "Slab sonrasi: ${after} kB"
echo "Fark        : $((after - before)) kB"
echo "Yeni cekirdek uyarisi: $((warn_after - warn_before))"

echo
echo "=== Artik kaynak kontrolu ==="
echo "-- /proc/interrupts (kesme kaydi kalmis mi?)"
grep -iE "dnhat|eth[12]" /proc/interrupts || echo "   temiz"
echo "-- calisan is parcaciklari"
ps -eo pid,comm | grep -E "irq/.*-eth|dnhat" || echo "   temiz"
echo "-- kayitli arayuzler"
ip -brief link | grep -E "eth[12]" || echo "   temiz"

# Sizinti esigi: olcum gurultusu icin 64 kB tolerans
if [ "$fail" -ne 0 ]; then
	echo "SONUC: BASARISIZ (yukleme/kaldirma hatasi)"
	exit 1
elif [ $((after - before)) -gt 64 ]; then
	echo "SONUC: BASARISIZ (olasi bellek sizintisi)"
	exit 1
elif [ $((warn_after - warn_before)) -ne 0 ]; then
	echo "SONUC: BASARISIZ (cekirdek uyarisi uretildi)"
	exit 1
else
	echo "SONUC: BASARILI"
fi
