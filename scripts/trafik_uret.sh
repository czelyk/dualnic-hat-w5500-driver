#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# trafik_uret.sh -- cift port es zamanli trafik testi (20. gun)
#
# Iki NIC ornegi ayni fiziksel SPI0 veri yolunu paylastigindan, es zamanli
# calismada toplam basarimin tek port degerinin iki katina cikmamasi
# beklenen sonuctur; darbogaz veri yolunun kendisidir (Tablo 20.2).
#
# Kullanim: sudo ./trafik_uret.sh eth1 eth2 300 [peer1] [peer2]

set -uo pipefail

IF1=${1:-eth1}
IF2=${2:-eth2}
DURATION=${3:-60}
PEER1=${4:-192.168.10.1}
PEER2=${5:-192.168.20.1}

if [ "$(id -u)" -ne 0 ]; then
	echo "bu betik root olarak calistirilmalidir" >&2
	exit 1
fi

rx_bytes() { cat "/sys/class/net/$1/statistics/rx_bytes"; }
tx_bytes() { cat "/sys/class/net/$1/statistics/tx_bytes"; }
err_sum() {
	local d=/sys/class/net/$1/statistics
	echo $(( $(cat "$d/rx_errors") + $(cat "$d/tx_errors") +
		 $(cat "$d/rx_dropped") + $(cat "$d/rx_over_errors") +
		 $(cat "$d/rx_length_errors") + $(cat "$d/tx_fifo_errors") ))
}
irq_count() {
	grep -E "[[:space:]]$1\$" /proc/interrupts \
		| awk '{s=0; for (i=2; i<=NF-3; i++) s+=$i; print s}'
}

for i in "$IF1" "$IF2"; do
	if [ ! -d "/sys/class/net/$i" ]; then
		echo "arayuz bulunamadi: $i" >&2
		exit 1
	fi
done

echo "=== $DURATION saniyelik cift port trafik testi ==="
echo "arayuzler: $IF1 -> $PEER1, $IF2 -> $PEER2"

r1_0=$(rx_bytes "$IF1"); t1_0=$(tx_bytes "$IF1"); e1_0=$(err_sum "$IF1")
r2_0=$(rx_bytes "$IF2"); t2_0=$(tx_bytes "$IF2"); e2_0=$(err_sum "$IF2")
i1_0=$(irq_count "$IF1"); i2_0=$(irq_count "$IF2")

# iperf3 varsa gercekci bir yuk uretilir, yoksa ping seli kullanilir.
if command -v iperf3 > /dev/null 2>&1; then
	iperf3 -c "$PEER1" -B "$(ip -4 -brief addr show "$IF1" \
		| awk '{print $3}' | cut -d/ -f1)" \
		-t "$DURATION" > "/tmp/iperf_$IF1.log" 2>&1 &
	P1=$!
	iperf3 -c "$PEER2" -B "$(ip -4 -brief addr show "$IF2" \
		| awk '{print $3}' | cut -d/ -f1)" \
		-t "$DURATION" > "/tmp/iperf_$IF2.log" 2>&1 &
	P2=$!
else
	echo "(iperf3 bulunamadi, ping seli kullaniliyor)"
	ping -f -w "$DURATION" -I "$IF1" "$PEER1" > /dev/null 2>&1 & P1=$!
	ping -f -w "$DURATION" -I "$IF2" "$PEER2" > /dev/null 2>&1 & P2=$!
fi

# Test suresince CPU kullanimi ornekle
( top -b -n "$((DURATION / 5))" -d 5 | awk '/^%Cpu/ {print $2+$4}' \
	> /tmp/dnhat_cpu.log ) 2>/dev/null &

wait $P1 $P2 2>/dev/null

r1_1=$(rx_bytes "$IF1"); t1_1=$(tx_bytes "$IF1"); e1_1=$(err_sum "$IF1")
r2_1=$(rx_bytes "$IF2"); t2_1=$(tx_bytes "$IF2"); e2_1=$(err_sum "$IF2")
i1_1=$(irq_count "$IF1"); i2_1=$(irq_count "$IF2")

mbps() { echo "scale=1; ($1 * 8) / ($2 * 1000000)" | bc; }

b1=$(( (r1_1 - r1_0) + (t1_1 - t1_0) ))
b2=$(( (r2_1 - r2_0) + (t2_1 - t2_0) ))

echo
printf '%-10s %12s %10s %10s\n' "Arayuz" "Basarim" "Hata" "Kesme"
printf '%-10s %10s M %10s %10s\n' "$IF1" "$(mbps $b1 $DURATION)" \
	"$((e1_1 - e1_0))" "$((i1_1 - i1_0))"
printf '%-10s %10s M %10s %10s\n' "$IF2" "$(mbps $b2 $DURATION)" \
	"$((e2_1 - e2_0))" "$((i2_1 - i2_0))"
printf '%-10s %10s M\n' "TOPLAM" "$(mbps $((b1 + b2)) $DURATION)"

if [ -s /tmp/dnhat_cpu.log ]; then
	echo
	awk '{s+=$1; n++} END {if (n) printf "Ortalama CPU: %.0f%%\n", s/n}' \
		/tmp/dnhat_cpu.log
fi

echo
echo "=== Kilitlenme / atomik baglamda uyku kontrolu ==="
if dmesg | grep -iE "lockdep|BUG|WARNING|scheduling while" | tail -5 \
	| grep -q .; then
	echo "UYARI: cekirdek uyarisi tespit edildi:"
	dmesg | grep -iE "lockdep|BUG|WARNING|scheduling while" | tail -5
else
	echo "cikti yok -- kilitlenme veya atomik baglamda uyku tespit edilmedi"
fi
