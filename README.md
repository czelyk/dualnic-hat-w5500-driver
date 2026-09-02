# DualNIC-HAT — W5500 Gömülü Linux NIC Sürücüsü

Çift portlu **DualNIC-HAT** genişleme kartı üzerindeki iki adet WIZnet
W5500 SPI Ethernet denetleyicisini, Linux'a iki bağımsız ağ arayüzü
(`eth1`, `eth2`) olarak tanıtan çekirdek modülü ve doğrulama araçları.

Bu depo, staj defterindeki **"Gömülü Linux NIC Sürücüsü ve Donanım
Entegrasyonu"** kaleminin kod tarafıdır.

---

## Dizin yapısı

```
DualnicHat/
├── Makefile                       Üst seviye derleme/dağıtım
├── driver/
│   ├── Makefile                   Ağaç dışı (out-of-tree) modül derlemesi
│   ├── dnhat_w5500.h              Register haritası ve sürücü sabitleri
│   └── dnhat_w5500.c              NIC sürücüsü (spi_driver + net_device)
├── dts/
│   ├── Makefile
│   └── dualnic-hat-overlay.dts    Device Tree overlay (2 × W5500)
├── tools/
│   ├── Makefile
│   └── dnhat_frametest.c          AF_PACKET ham çerçeve doğrulama aracı
└── scripts/
    ├── dnhat_debug.sh             Dinamik hata ayıklama + gözlem noktaları
    ├── leak_test.sh               Modül yaşam döngüsü sızıntı testi (DR-10)
    ├── trafik_uret.sh             Çift port eş zamanlı trafik testi
    └── kabul_testi.sh             DR-01…DR-11 kabul matrisi
```

---

## Mimari

W5500 bir **SPI NIC**'tir; PCIe veya dahili MAC+RMII PHY çözümlerinde
standart kabul edilen yapıların hiçbiri burada geçerli değildir:

| Varsayım | W5500'de durumu |
|---|---|
| MMIO ve BAR ile register erişimi | **Yok** — her erişim tam bir SPI işlemi |
| DMA ve descriptor ring | **Yok** — çerçeveler çipin 32 kB dahili tamponuna kopyalanır |
| NAPI ile yoklamaya geçiş | Gerekçesi geçersiz — darboğaz kesme sayısı değil SPI veri yolu |
| Kesme bağlamında register okuma | **İmkânsız** — SPI işlemi uyuyabilir |

Son madde belirleyicidir ve sürücünün **threaded IRQ** kullanmasını
zorunlu kılar.

### Seçilen kalıp

| Konu | Karar | Gerekçe |
|---|---|---|
| Sürücü sınıfı | `spi_driver` + `net_device` | Cihaz SPI veri yolunda, işlevi ağ arayüzü |
| Cihaz eşleşmesi | Device Tree, `of_match_table` | SPI numaralandırılamaz |
| Kesme modeli | Threaded IRQ | SPI işlemleri uyuyabilir |
| Veri kopyalama | SPI blok aktarımı | DMA yok; çip tamponu ana bellek değil |
| Çip çalışma modu | MACRAW | Ham Ethernet çerçevesi aktarımı |
| Örnekleme | Cihaz başına bağımsız | DR-11 gereksinimi |

### Katman görünümü

```
Linux Ağ Yığını (IP, ARP, soket katmanı)
        │
struct net_device (eth1 / eth2)
        │
struct net_device_ops  ← bu sürücünün doldurduğu tablo
        │
SPI register erişimi + MACRAW soket yönetimi
        │  SPI
W5500 donanımı
```

---

## Donanım arayüz sözleşmesi

Donanım ekibiyle birinci günde mutabık kalınan kaynak haritası
(`dts/dualnic-hat-overlay.dts` bunu birebir yansıtır):

| Kaynak | NIC #1 | NIC #2 |
|---|---|---|
| Chip select | CE0 (`reg = <0>`) | CE1 (`reg = <1>`) |
| SPI modu / hızı | Mod 0, 30 MHz | Mod 0, 30 MHz |
| Kesme hattı | GPIO25, düşen kenar | GPIO24, düşen kenar |
| Reset hattı | GPIO23, aktif-düşük | GPIO22, aktif-düşük |
| MAC adresi | `local-mac-address` | `local-mac-address` |
| Bring-up doğrulaması | `VERSIONR = 0x04` | `VERSIONR = 0x04` |

Reset hatlarının kaynak kodunda değil Device Tree'de tanımlanması bilinçli
bir tercihtir: kartın bir sonraki revizyonunda hat değişse dahi sürücü
yeniden derlenmeden çalışmaya devam eder.

---

## Derleme

Hedef sistem ARM mimarisinde olduğundan modül çapraz derlenir. Bir çekirdek
modülünün, derlendiği çekirdek başlıklarıyla **birebir** uyumlu olması
zorunludur.

```bash
export KDIR=$HOME/kernel/linux           # hedef karta ait çekirdek ağacı
export CROSS=arm-linux-gnueabihf-
export TARGET=pi@dualnic-target

make            # driver + dts + tools
make deploy     # .ko, .dtbo, araçlar ve betikler hedef karta kopyalanır
make check      # sparse (__CHECK_ENDIAN__) + checkpatch
```

Gerekli çekirdek seçenekleri: `CONFIG_SPI`, `CONFIG_SPI_BCM2835`,
`CONFIG_GPIOLIB`, `CONFIG_OF`.
Hata ayıklama için: `CONFIG_DYNAMIC_DEBUG`, `CONFIG_DEBUG_INFO`,
`CONFIG_PROVE_LOCKING`, `CONFIG_DEBUG_ATOMIC_SLEEP`.

## Kurulum

```bash
# Overlay
sudo cp dualnic-hat.dtbo /boot/firmware/overlays/
sudo dtoverlay dualnic-hat                  # geçici
# veya /boot/firmware/config.txt: dtoverlay=dualnic-hat

# Modül
sudo insmod dnhat_w5500.ko
dmesg | tail -6
```

Beklenen çıktı:

```
dnhat_w5500 spi0.0: kaynaklar: CS=0, IRQ=185, hiz=30000000 Hz
dnhat_w5500 spi0.0: W5500 dogrulandi (VERSIONR = 0x04)
dnhat_w5500 spi0.0: eth1: DualNIC-HAT arayuzu hazir, MAC 02:00:00:00:00:01
dnhat_w5500 spi0.1: kaynaklar: CS=1, IRQ=186, hiz=30000000 Hz
dnhat_w5500 spi0.1: W5500 dogrulandi (VERSIONR = 0x04)
dnhat_w5500 spi0.1: eth2: DualNIC-HAT arayuzu hazir, MAC 02:00:00:00:00:02
```

```bash
sudo ip addr add 192.168.10.10/24 dev eth1
sudo ip link set eth1 up
ping -c 4 192.168.10.1
```

---

## Test

```bash
sudo ./kabul_testi.sh eth1 eth2 192.168.10.1   # DR-01 … DR-11
sudo ./leak_test.sh 500                        # yükle/kaldır sızıntı testi
sudo ./trafik_uret.sh eth1 eth2 300            # çift port eş zamanlı trafik
sudo ./dnhat_debug.sh status                   # gözlem noktalarının dökümü

# Ham çerçeve düzeyinde doğrulama (karşı uçta dinleme modu çalıştırılır)
sudo ./dnhat_frametest -i eth1 -s 1514 -c 1000
sudo ./dnhat_frametest -i eth2 -l -c 1000
```

### Gözlem noktaları

| Nokta | Ne anlaşılır |
|---|---|
| `dmesg` | Sürücü günlükleri, `probe()` sonucu, hata kodları |
| `/sys/bus/spi/devices/` | SPI cihazının oluşup oluşmadığı |
| `/sys/bus/spi/drivers/` | Sürücünün kayıtlı olup olmadığı, eşleşme durumu |
| `/proc/interrupts` | Kesme hattının gerçekten tetiklenip tetiklenmediği |
| `/proc/net/dev`, `ip -s link` | RX/TX paket ve hata sayaçları |
| Mantık analizörü (TP6–TP9) | SPI hattındaki gerçek fiziksel işlem |
| Osiloskop (TP11, TP12) | INTn ve RSTn hatlarının gerçek darbe biçimi |

---

## Gereksinim izlenebilirliği

| Kimlik | Gereksinim | Gerçeklendiği yer |
|---|---|---|
| DR-01 | Device Tree ile otomatik eşleşme | `dnhat_of_match[]`, `dnhat_spi_driver` |
| DR-02 | Her denetleyici için ayrı `net_device` | `dnhat_probe()` → `devm_alloc_etherdev()` |
| DR-03 | Donanımsal reset | `dnhat_hw_reset()`, `dnhat_sw_reset()` |
| DR-04 | Kesme tabanlı çalışma | `dnhat_irq_top()` / `dnhat_irq_thread()` |
| DR-05 | RX yolu, `sk_buff` aktarımı | `dnhat_rx_process()` |
| DR-06 | TX yolu | `dnhat_start_xmit()` / `dnhat_tx_work()` |
| DR-07 | Link up/down → `carrier` | `dnhat_link_work()` |
| DR-08 | `ethtool` desteği | `dnhat_ethtool_ops` |
| DR-09 | İstatistik sayaçları | `ndev->stats` güncellemeleri |
| DR-10 | Güvenli modül kaldırma | `devm_*`, `dnhat_stop()` kapatma sırası |
| DR-11 | Örnek bağımsızlığı | Global değişken yok; örnek başına `priv`, `mutex`, `wq` |

---

## Uygulamadaki kritik ayrıntılar

**Bayt sıralaması.** W5500, 16 bitlik kayıtlarını big-endian sırayla
sunar; ana kart little-endian çalışır. `be16_to_cpu()` dönüşümünün
atlanması, sürücünün çökmesi biçiminde değil, tampon işaretçilerinin
rastgele yerlere kaymasıyla açıklanamayan paket bozulmaları biçiminde
görülür.

**Değişken uzunluk modu.** Sabit uzunluk modunda 1500 baytlık bir çerçeve
için ek yük oranı %75'tir; değişken uzunluk modunda aynı çerçeve tek
işlemde aktarıldığı için oran %0,2'ye düşer. Adres, kontrol baytı ve veri
tek bir `spi_message` içinde zincirlenir; böylece chip select işlem
ortasında yükselmez.

**DMA güvenli tamponlar.** SPI denetleyicisi altyapısı yığın üzerindeki
tamponlarla güvenli çalışmayabileceğinden, register erişimleri
`priv->xfer_buf` (devm_kzalloc) üzerinden yapılır. Çerçeve verisi ise
zaten `kmalloc` ile ayrılmış `sk_buff` tamponuna okunur/yazılır.

**MACRAW başlığı.** RX tamponundan okunan verinin ilk iki baytı çerçeve
değil, kendisi dâhil toplam bayt sayısıdır. Gözden kaçması hâlinde her
çerçevenin başında iki baytlık bozulma oluşur.

**Kapatma sırası.** `ndo_stop()` içinde kesme, iş kuyruğundaki işler
beklenmeden serbest bırakılırsa hâlâ çalışan bir TX işi serbest bırakılmış
kaynaklara erişir. Doğru sıra: kuyruk durdur → gecikmeli işler → normal
işler → IRQ → soket kapat.

**RX bütçesi.** RX döngüsüne üst sınır konulmazsa yoğun trafik altında
kesme iş parçacığı döngüden hiç çıkamaz. Ölçümlerde 16 değeri seçilmiştir
(11.9 Mbps, %22 CPU). Bütçe dolduğunda çipte veri kalabileceği için,
kesme kenar tetiklemeli olduğundan tampon iş parçacığı çıkmadan önce
ayrıca boşaltılır.

**Bağlantı yoklaması.** Çip bağlantı durumu değiştiğinde kesme
üretmediğinden `PHYCFGR` 200 ms aralıklarla yoklanır. Durum değişimi ağ
yığınına yalnızca iki ardışık yoklamada aynı okunursa bildirilir; bu
kararlılık filtresi kısa kablo temassızlıklarında rotanın gidip gelmesini
önler.

---

## Staj defterinden bilinçli olarak sapılan iki nokta

Aşağıdaki iki nokta, defterdeki kod parçacıklarında çalışır görünse de
çekirdek bağlam kurallarını ihlal ettiği için burada düzeltilmiştir:

1. **`ndo_tx_timeout()` atomik bağlamda çağrılır.** Defterdeki
   `dnhat_tx_timeout()` doğrudan `mutex_lock()` alıp SPI işlemi
   yapmaktadır; her ikisi de uyuyabilen çağrılardır ve
   `CONFIG_DEBUG_ATOMIC_SLEEP` etkin bir çekirdekte uyarı üretir. Burada
   yeniden başlatma `restart_work` iş öğesine devredilmiştir. Kuyruk
   sıralı (ordered) olduğu için bekleyen TX işinden sonra çalışır.

2. **Kesme bayrakları işlemeden önce temizlenir.** Defterde `Sn_IR`,
   `dnhat_rx_process()` tamamlandıktan sonra temizlenmektedir; bu sırada
   gelen bir çerçevenin kaldırdığı bayrak da silinir ve kenar tetiklemeli
   yapılandırmada o olay kaybolur. Burada bayraklar okunur okunmaz
   temizlenir.

Ayrıca defterdeki `dnhat_tx_work()` hata yolunda kuyruğu uyandırmadığı
için arayüz watchdog süresi (5 s) boyunca bloke kalmaktadır; burada hata
durumunda kuyruk doğrudan açılır.

---

## Kaynaklar

- WIZnet, *W5500 Datasheet*, Version 1.0
- WIZnet, *W5500 Application Note: MACRAW Mode Operation*
- Linux Kernel, `Documentation/devicetree/bindings/net/wiznet,w5x00.txt`
- Linux Kernel, `drivers/net/ethernet/wiznet/w5100-spi.c`
- Corbet, Rubini, Kroah-Hartman, *Linux Device Drivers*, 3rd Ed.
- Linux Kernel Docs: SPI Subsystem, GPIO Descriptor Consumer Interface,
  `request_threaded_irq`, Devres, Dynamic Debug Howto
