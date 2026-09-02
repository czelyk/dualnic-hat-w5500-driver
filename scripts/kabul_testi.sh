#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# kabul_testi.sh -- DualNIC-HAT W5500 surucusu kabul testi
#
# Bu betik DR-01 .. DR-11 gereksinimlerini mumkun oldugu kadar
# otomatik ve tekrarlanabilir bicimde dogrular.
#
# Onemli tasarim kararlari:
#   * Iki NIC icin ayri peer adresleri desteklenir.
#   * Test oncesindeki arayuz UP/DOWN durumu kaydedilir ve cikista geri yuklenir.
#   * IRQ testi yalnizca "sayac > 0" demez; test trafiginden once/sonra fark arar.
#   * Cekirdek uyarilarinda yalnizca bu test sirasinda eklenen dmesg satirlari incelenir.
#   * Modul unload/reload testi varsayilan olarak YAPILMAZ. --lifecycle verilirse
#     guvenli yasam dongusu testi gercekten uygulanir.
#
# Kullanim:
#   sudo ./kabul_testi.sh \
#       --if1 eth1 --peer1 192.168.10.1 \
#       --if2 eth2 --peer2 192.168.20.1
#
# Yasam dongusu testi ile:
#   sudo ./kabul_testi.sh \
#       --if1 eth1 --peer1 192.168.10.1 \
#       --if2 eth2 --peer2 192.168.20.1 \
#       --lifecycle --ko ./driver/dnhat_w5500.ko
#
# Cikis kodu:
#   0 -> tum calistirilan testler basarili
#   1 -> en az bir test basarisiz
#   2 -> kullanim / on kosul hatasi

set -uo pipefail

MODULE="dnhat_w5500"
IF1="eth1"
IF2="eth2"
PEER1="192.168.10.1"
PEER2="192.168.20.1"
KO_PATH=""
RUN_LIFECYCLE=0
PING_COUNT=3
PING_TIMEOUT=1

PASS=0
FAIL=0
SKIP=0
DMESG_LINES_BEFORE=0
IF1_WAS_UP=0
IF2_WAS_UP=0
MODULE_WAS_LOADED=0

usage() {
	cat <<USAGE
Kullanim: sudo $0 [secenekler]

Secenekler:
  --if1 IFACE       Birinci NIC arayuzu (varsayilan: ${IF1})
  --if2 IFACE       Ikinci NIC arayuzu  (varsayilan: ${IF2})
  --peer1 IP        ${IF1} icin karsi uc (varsayilan: ${PEER1})
  --peer2 IP        ${IF2} icin karsi uc (varsayilan: ${PEER2})
  --lifecycle       DR-10 icin gercek rmmod/insmod testi yap
  --ko PATH         --lifecycle sonrasi yuklenecek .ko dosyasi
  --ping-count N    Trafik testindeki ICMP sayisi (varsayilan: ${PING_COUNT})
  -h, --help        Bu yardimi goster
USAGE
}

fatal() {
	echo "HATA: $*" >&2
	exit 2
}

need_root() {
	[ "$(id -u)" -eq 0 ] || fatal "bu betik root olarak calistirilmalidir"
}

need_cmd() {
	command -v "$1" >/dev/null 2>&1 || fatal "gerekli komut bulunamadi: $1"
}

module_loaded() {
	grep -q "^${MODULE} " /proc/modules 2>/dev/null
}

iface_exists() {
	[ -d "/sys/class/net/$1" ]
}

iface_is_up() {
	local flags
	flags=$(cat "/sys/class/net/$1/flags" 2>/dev/null) || return 1
	(( flags & 0x1 ))
}

set_iface_state() {
	local ifname=$1
	local wanted=$2

	iface_exists "$ifname" || return 0

	if [ "$wanted" -eq 1 ]; then
		ip link set dev "$ifname" up >/dev/null 2>&1 || true
	else
		ip link set dev "$ifname" down >/dev/null 2>&1 || true
	fi
}

restore_state() {
	# DR-10 sirasinda bir hata olursa modulu geri getirmeyi dene.
	if [ "$MODULE_WAS_LOADED" -eq 1 ] && ! module_loaded; then
		if [ -n "$KO_PATH" ] && [ -r "$KO_PATH" ]; then
			insmod "$KO_PATH" >/dev/null 2>&1 || true
		elif command -v modprobe >/dev/null 2>&1; then
			modprobe "$MODULE" >/dev/null 2>&1 || true
		fi
		sleep 1
	fi

	set_iface_state "$IF1" "$IF1_WAS_UP"
	set_iface_state "$IF2" "$IF2_WAS_UP"
}

trap restore_state EXIT INT TERM

print_header() {
	printf '\n=== DualNIC-HAT NIC surucusu kabul testi ===\n'
	printf 'Modul : %s\n' "$MODULE"
	printf 'NIC #1: %s -> %s\n' "$IF1" "$PEER1"
	printf 'NIC #2: %s -> %s\n\n' "$IF2" "$PEER2"
	printf '  %-7s %-43s %-24s %s\n' \
		"Kimlik" "Gereksinim" "Kanit" "Durum"
	printf '  %-7s %-43s %-24s %s\n' \
		"-------" "-------------------------------------------" \
		"------------------------" "---------"
}

pass_result() {
	local id=$1 desc=$2 evidence=$3
	printf '  %-7s %-43s %-24s BASARILI\n' "$id" "$desc" "$evidence"
	PASS=$((PASS + 1))
}

fail_result() {
	local id=$1 desc=$2 evidence=$3
	printf '  %-7s %-43s %-24s BASARISIZ\n' "$id" "$desc" "$evidence"
	FAIL=$((FAIL + 1))
}

skip_result() {
	local id=$1 desc=$2 evidence=$3
	printf '  %-7s %-43s %-24s ATLANDI\n' "$id" "$desc" "$evidence"
	SKIP=$((SKIP + 1))
}

check() {
	local id=$1 desc=$2 evidence=$3
	shift 3

	if "$@" >/dev/null 2>&1; then
		pass_result "$id" "$desc" "$evidence"
	else
		fail_result "$id" "$desc" "$evidence"
	fi
}

sysfs_counter() {
	local ifname=$1
	local name=$2
	cat "/sys/class/net/${ifname}/statistics/${name}"
}

irq_count() {
	local ifname=$1

	awk -v needle="$ifname" '
		index($0, needle) {
			for (i = 2; i <= NF; i++) {
				if ($i ~ /^[0-9]+$/)
					s += $i;
				else
					break;
			}
		}
		END { print s + 0 }
	' /proc/interrupts
}

ping_on_iface() {
	local ifname=$1
	local peer=$2

	ping -n -c "$PING_COUNT" -W "$PING_TIMEOUT" -I "$ifname" "$peer"
}

wait_for_iface() {
	local ifname=$1
	local i

	for i in $(seq 1 30); do
		iface_exists "$ifname" && return 0
		sleep 0.1
	done
	return 1
}

# ---------------------------------------------------------------------------
# DR testleri
# ---------------------------------------------------------------------------

dr01() {
	local driver_dir="/sys/bus/spi/drivers/${MODULE}"
	local devs=()

	[ -d "$driver_dir" ] || return 1

	shopt -s nullglob
	devs=("$driver_dir"/spi*)
	shopt -u nullglob

	[ "${#devs[@]}" -eq 2 ]
}

dr02() {
	iface_exists "$IF1" && iface_exists "$IF2" && [ "$IF1" != "$IF2" ]
}

dr03() {
	local count

	# Probe sirasinda reset + VERSIONR kimlik dogrulamasi basariliysa
	# surucu her W5500 icin bu mesaji uretir.
	count=$(dmesg | grep -c "W5500 dogrulandi (VERSIONR = 0x04)" || true)
	[ "$count" -ge 2 ]
}

dr04() {
	local before after

	before=$(irq_count "$IF1") || return 1
	ping_on_iface "$IF1" "$PEER1" >/dev/null 2>&1 || true
	after=$(irq_count "$IF1") || return 1

	[ "$after" -gt "$before" ]
}

dr05() {
	local before after

	before=$(sysfs_counter "$IF1" rx_packets) || return 1
	ping_on_iface "$IF1" "$PEER1" >/dev/null 2>&1 || return 1
	after=$(sysfs_counter "$IF1" rx_packets) || return 1

	[ "$after" -gt "$before" ]
}

dr06() {
	local before after

	before=$(sysfs_counter "$IF1" tx_packets) || return 1
	ping_on_iface "$IF1" "$PEER1" >/dev/null 2>&1 || true
	after=$(sysfs_counter "$IF1" tx_packets) || return 1

	[ "$after" -gt "$before" ]
}

dr07() {
	local carrier operstate

	carrier=$(cat "/sys/class/net/${IF1}/carrier" 2>/dev/null) || return 1
	operstate=$(cat "/sys/class/net/${IF1}/operstate" 2>/dev/null) || return 1

	[ "$carrier" = "0" ] || [ "$carrier" = "1" ] || return 1
	[ -n "$operstate" ]
}

dr08() {
	local driver

	driver=$(ethtool -i "$IF1" 2>/dev/null | awk -F': ' '$1 == "driver" {print $2}')
	[ "$driver" = "$MODULE" ] || return 1

	ethtool "$IF1" 2>/dev/null | grep -q "Link detected:"
}

dr09() {
	for counter in rx_packets tx_packets rx_bytes tx_bytes rx_errors tx_errors; do
		[ -r "/sys/class/net/${IF1}/statistics/${counter}" ] || return 1
	done

	ip -s link show dev "$IF1" | grep -q "RX:"
}

dr10_lifecycle() {
	local ko="$KO_PATH"

	# Modulu geri yuklemek icin .ko yoluna ihtiyacimiz var.
	if [ -z "$ko" ] && command -v modinfo >/dev/null 2>&1; then
		ko=$(modinfo -n "$MODULE" 2>/dev/null || true)
	fi

	[ -n "$ko" ] && [ -r "$ko" ] || return 2

	ip link set dev "$IF1" down >/dev/null 2>&1 || return 1
	ip link set dev "$IF2" down >/dev/null 2>&1 || return 1

	rmmod "$MODULE" >/dev/null 2>&1 || return 1

	# Surucu kaldirildiginda net_device ve driver binding kalmamali.
	if iface_exists "$IF1" || iface_exists "$IF2"; then
		return 1
	fi

	insmod "$ko" >/dev/null 2>&1 || return 1

	wait_for_iface "$IF1" || return 1
	wait_for_iface "$IF2" || return 1

	# Sonraki test / cikis icin arayuzleri yeniden kullanilabilir duruma getir.
	ip link set dev "$IF1" up >/dev/null 2>&1 || return 1
	ip link set dev "$IF2" up >/dev/null 2>&1 || return 1

	return 0
}

dr11() {
	local if1_up_before=0
	local before_tx after_tx
	local rc=1

	iface_is_up "$IF1" && if1_up_before=1

	before_tx=$(sysfs_counter "$IF2" tx_packets) || return 1

	ip link set dev "$IF1" down >/dev/null 2>&1 || return 1
	sleep 0.2

	if iface_exists "$IF2" && ping_on_iface "$IF2" "$PEER2" >/dev/null 2>&1; then
		after_tx=$(sysfs_counter "$IF2" tx_packets) || after_tx=$before_tx
		if [ "$after_tx" -gt "$before_tx" ]; then
			rc=0
		fi
	fi

	if [ "$if1_up_before" -eq 1 ]; then
		ip link set dev "$IF1" up >/dev/null 2>&1 || true
	fi

	return "$rc"
}

# ---------------------------------------------------------------------------
# Argumanlar
# ---------------------------------------------------------------------------

while [ "$#" -gt 0 ]; do
	case "$1" in
	--if1)
		[ "$#" -ge 2 ] || fatal "--if1 bir deger ister"
		IF1=$2
		shift 2
		;;
	--if2)
		[ "$#" -ge 2 ] || fatal "--if2 bir deger ister"
		IF2=$2
		shift 2
		;;
	--peer1)
		[ "$#" -ge 2 ] || fatal "--peer1 bir deger ister"
		PEER1=$2
		shift 2
		;;
	--peer2)
		[ "$#" -ge 2 ] || fatal "--peer2 bir deger ister"
		PEER2=$2
		shift 2
		;;
	--lifecycle)
		RUN_LIFECYCLE=1
		shift
		;;
	--ko)
		[ "$#" -ge 2 ] || fatal "--ko bir deger ister"
		KO_PATH=$2
		shift 2
		;;
	--ping-count)
		[ "$#" -ge 2 ] || fatal "--ping-count bir deger ister"
		PING_COUNT=$2
		case "$PING_COUNT" in
		''|*[!0-9]*) fatal "--ping-count pozitif tam sayi olmali" ;;
		esac
		[ "$PING_COUNT" -gt 0 ] || fatal "--ping-count sifirdan buyuk olmali"
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		fatal "bilinmeyen secenek: $1"
		;;
	esac
done

# ---------------------------------------------------------------------------
# On kosullar ve baslangic durumu
# ---------------------------------------------------------------------------

need_root
for cmd in ip ping awk grep dmesg ethtool; do
	need_cmd "$cmd"
done

module_loaded || fatal "${MODULE} modulu yuklu degil"
iface_exists "$IF1" || fatal "arayuz bulunamadi: $IF1"
iface_exists "$IF2" || fatal "arayuz bulunamadi: $IF2"

MODULE_WAS_LOADED=1
iface_is_up "$IF1" && IF1_WAS_UP=1
iface_is_up "$IF2" && IF2_WAS_UP=1
DMESG_LINES_BEFORE=$(dmesg | wc -l)

# Test trafigi icin arayuzleri ac.
ip link set dev "$IF1" up
ip link set dev "$IF2" up
sleep 0.5

print_header

check DR-01 "Device Tree ile iki SPI eslesmesi" "/sys/bus/spi/drivers" dr01
check DR-02 "Her cip icin ayri net_device" "sysfs / ip link" dr02
check DR-03 "Reset sonrasi iki cip kimlik dogrulamasi" "VERSIONR = 0x04" dr03
check DR-04 "IRQ sayaci trafikle artiyor" "/proc/interrupts" dr04
check DR-05 "RX yolu ag yigini tarafina veri tasiyor" "rx_packets" dr05
check DR-06 "TX yolu fiziksel NIC'e veri tasiyor" "tx_packets" dr06
check DR-07 "Carrier / operstate raporlamasi mevcut" "sysfs carrier" dr07
check DR-08 "ethtool surucu ve link bilgisi" "ethtool" dr08
check DR-09 "NIC istatistik sayaclari okunabilir" "ip -s / sysfs" dr09
check DR-11 "Birinci NIC down iken ikinci NIC calisiyor" "IF2 trafik testi" dr11

if [ "$RUN_LIFECYCLE" -eq 1 ]; then
	dr10_lifecycle
	case $? in
	0)
		pass_result DR-10 "Guvenli remove / reload yasam dongusu" "rmmod + insmod"
		;;
	2)
		skip_result DR-10 "Guvenli remove / reload yasam dongusu" ".ko yolu yok"
		;;
	*)
		fail_result DR-10 "Guvenli remove / reload yasam dongusu" "rmmod + insmod"
		;;
	esac
else
	skip_result DR-10 "Guvenli remove / reload yasam dongusu" "--lifecycle gerekli"
fi

# ---------------------------------------------------------------------------
# Yalnizca bu test boyunca olusan yeni cekirdek uyari / hata satirlari
# ---------------------------------------------------------------------------

echo
 echo "=== Bu test sirasinda eklenen cekirdek uyarilari ==="

DMESG_LINES_AFTER=$(dmesg | wc -l)
if [ "$DMESG_LINES_AFTER" -ge "$DMESG_LINES_BEFORE" ]; then
	NEW_DMESG=$(dmesg | tail -n +$((DMESG_LINES_BEFORE + 1)))
else
	# Ring buffer donduyse guvenli tarafta kalip mevcut logu tara.
	NEW_DMESG=$(dmesg)
fi

if printf '%s\n' "$NEW_DMESG" \
	| grep -iE "BUG:|WARNING:|kernel panic|Oops:|lockdep|scheduling while atomic" \
	| grep -q .; then
	echo "BASARISIZ: test sirasinda cekirdek uyarisi/hatasi bulundu"
	printf '%s\n' "$NEW_DMESG" \
		| grep -iE "BUG:|WARNING:|kernel panic|Oops:|lockdep|scheduling while atomic" \
		| tail -20
	FAIL=$((FAIL + 1))
else
	echo "temiz"
fi

# ---------------------------------------------------------------------------
# Ozet
# ---------------------------------------------------------------------------

echo
printf 'SONUC: %d basarili, %d basarisiz, %d atlandi\n' "$PASS" "$FAIL" "$SKIP"

if [ "$SKIP" -gt 0 ]; then
	echo "Not: ATLANDI sonucu basarisizlik sayilmaz; ilgili test acikca etkinlestirilmelidir."
fi

[ "$FAIL" -eq 0 ]
