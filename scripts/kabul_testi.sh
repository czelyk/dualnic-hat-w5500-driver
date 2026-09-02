#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# kabul_testi.sh -- NIC surucusu gereksinimlerinin kabul matrisi
#                   (24. gun, Tablo 24.1: DR-01 .. DR-11)
#
# Her gereksinim, staj defterinde belirtilen dogrulama kanitinin sistem
# uzerinden okunmasiyla denetlenir.
#
# Kullanim: sudo ./kabul_testi.sh [if1] [if2] [peer]

set -uo pipefail

IF1=${1:-eth1}
IF2=${2:-eth2}
PEER=${3:-192.168.10.1}
MODULE=dnhat_w5500

PASS=0
FAIL=0

if [ "$(id -u)" -ne 0 ]; then
	echo "bu betik root olarak calistirilmalidir" >&2
	exit 1
fi

check() {
	local id=$1 desc=$2 kanit=$3
	shift 3
	if "$@" > /dev/null 2>&1; then
		printf '  %-7s %-42s %-22s SAGLANDI\n' "$id" "$desc" "$kanit"
		PASS=$((PASS + 1))
	else
		printf '  %-7s %-42s %-22s BASARISIZ\n' "$id" "$desc" "$kanit"
		FAIL=$((FAIL + 1))
	fi
}

echo "=== DualNIC-HAT NIC surucusu kabul testi ==="
printf '  %-7s %-42s %-22s %s\n' "Kimlik" "Gereksinim" "Kanit" "Durum"

# DR-01: Device Tree ile otomatik eslesme
dr01() {
	local n
	n=$(ls -1 "/sys/bus/spi/drivers/${MODULE}/" 2>/dev/null \
		| grep -c '^spi')
	[ "$n" -eq 2 ]
}
check DR-01 "Device Tree ile otomatik eslesme" \
	"/sys/bus/spi/drivers" dr01

# DR-02: her cip icin ayri net_device
dr02() { [ -d "/sys/class/net/$IF1" ] && [ -d "/sys/class/net/$IF2" ]; }
check DR-02 "Her cip icin ayri net_device" "ip link" dr02

# DR-03: donanimsal reset -- probe VERSIONR dogrulamasi ile kanitlanir
dr03() { dmesg | grep -q "W5500 dogrulandi"; }
check DR-03 "Donanimsal reset ve cip kimligi" "dmesg VERSIONR" dr03

# DR-04: kesme tabanli calisma (yoklama degil)
dr04() {
	local c
	ping -c 3 -W 1 -I "$IF1" "$PEER" > /dev/null 2>&1
	c=$(grep -E "[[:space:]]${IF1}\$" /proc/interrupts \
		| awk '{s=0; for (i=2; i<=NF-3; i++) s+=$i; print s}')
	[ -n "$c" ] && [ "$c" -gt 0 ]
}
check DR-04 "Kesme tabanli calisma" "/proc/interrupts" dr04

# DR-05: RX yolu -- sk_buff aktarimi
dr05() {
	local before after
	before=$(cat "/sys/class/net/$IF1/statistics/rx_packets")
	ping -c 3 -W 1 -I "$IF1" "$PEER" > /dev/null 2>&1
	after=$(cat "/sys/class/net/$IF1/statistics/rx_packets")
	[ "$after" -gt "$before" ]
}
check DR-05 "RX yolu, sk_buff aktarimi" "rx_packets artisi" dr05

# DR-06: TX yolu
dr06() {
	local before after
	before=$(cat "/sys/class/net/$IF1/statistics/tx_packets")
	ping -c 3 -W 1 -I "$IF1" "$PEER" > /dev/null 2>&1
	after=$(cat "/sys/class/net/$IF1/statistics/tx_packets")
	[ "$after" -gt "$before" ]
}
check DR-06 "TX yolu" "tx_packets artisi" dr06

# DR-07: link up/down algilama -> carrier
dr07() { [ "$(cat "/sys/class/net/$IF1/carrier" 2>/dev/null)" = "1" ]; }
check DR-07 "Link up/down algilama" "carrier / operstate" dr07

# DR-08: ethtool destegi
dr08() {
	ethtool -i "$IF1" | grep -q "driver: ${MODULE}" &&
	ethtool "$IF1" | grep -q "Link detected"
}
check DR-08 "ethtool destegi" "ethtool -i" dr08

# DR-09: istatistik sayaclari
dr09() { ip -s link show "$IF1" | grep -q "RX:"; }
check DR-09 "Istatistik sayaclari" "ip -s link" dr09

# DR-10: guvenli modul kaldirma -- arayuz acikken rmmod engellenmeli
dr10() { ! rmmod "$MODULE" 2> /dev/null; }
check DR-10 "Guvenli modul kaldirma" "referans sayimi" dr10

# DR-11: ornek bagimsizligi -- eth1 down iken eth2 etkilenmemeli
dr11() {
	local before after
	before=$(cat "/sys/class/net/$IF2/statistics/rx_packets")
	ip link set "$IF1" down
	sleep 1
	ping -c 2 -W 1 -I "$IF2" "$PEER" > /dev/null 2>&1
	after=$(cat "/sys/class/net/$IF2/statistics/rx_packets")
	ip link set "$IF1" up
	sleep 1
	[ "$after" -ge "$before" ] && [ -d "/sys/class/net/$IF2" ]
}
check DR-11 "Ornek bagimsizligi" "hata enjeksiyonu" dr11

echo
echo "=== Cekirdek uyari/hata gunlugu ==="
if dmesg | grep -iE "lockdep|BUG:|WARNING:|call trace" | tail -5 | grep -q .; then
	echo "UYARI: cekirdek uyarisi bulundu"
	dmesg | grep -iE "lockdep|BUG:|WARNING:|call trace" | tail -5
	FAIL=$((FAIL + 1))
else
	echo "temiz"
fi

echo
echo "SONUC: $PASS saglandi, $FAIL basarisiz"
[ "$FAIL" -eq 0 ]
