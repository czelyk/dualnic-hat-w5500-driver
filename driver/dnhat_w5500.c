// SPDX-License-Identifier: GPL-2.0
/*
 * dnhat_w5500.c -- DualNIC-HAT projesi
 *
 * Cift portlu DualNIC-HAT genisleme karti uzerindeki iki adet WIZnet W5500
 * SPI Ethernet denetleyicisini, Linux'a iki bagimsiz ag arayuzu (ethX)
 * olarak tanitan minimal NIC surucusu.
 *
 * Mimari kararlar (staj defteri 2. gun, Tablo 2.2):
 *   - Surucu sinifi   : spi_driver + net_device
 *   - Cihaz eslesmesi : Device Tree, of_match_table (SPI numaralandirilamaz)
 *   - Kesme modeli    : threaded IRQ (kesme kaynagini okumak SPI islemidir)
 *   - Veri kopyalama  : SPI blok aktarimi (DMA ve descriptor ring yok)
 *   - Cip calisma modu: MACRAW (cipin TCP/IP yigini kullanilmaz)
 *   - Ornekleme       : cihaz basina bagimsiz, global degisken yok (DR-11)
 *
 * Copyright (C) 2026 CTech Bilisim Teknolojileri -- Staj Projesi
 */

#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>

#include "dnhat_w5500.h"

/*
 * Cihaz ornegi basina tutulan baglam (8. gun, Kod 8.1).
 *
 * Surucu hicbir global degisken kullanmaz; iki W5500 ornegi arasinda
 * paylasilan tek kaynak, cekirdek SPI katmaninin kendi icinde
 * serilestirdigi fiziksel SPI0 veri yoludur.
 */
struct dnhat_priv {
	struct spi_device	*spi;		/* Bagli oldugu SPI cihazi     */
	struct net_device	*ndev;		/* Cekirdege kaydedilen arayuz */
	struct gpio_desc	*reset_gpio;	/* Donanimsal reset hatti      */
	int			irq;		/* Kesme numarasi              */

	struct mutex		lock;		/* SPI erisimlerini serilestirir */
	struct workqueue_struct	*wq;		/* Ornege ozel is kuyrugu      */
	struct work_struct	tx_work;	/* TX gonderimi (uyku gerekir) */
	struct work_struct	rxmode_work;	/* Promisc/multicast degisimi  */
	struct work_struct	restart_work;	/* TX zaman asimi sonrasi reset */
	struct delayed_work	link_work;	/* Baglanti durumu yoklamasi   */
	struct sk_buff		*tx_skb;	/* Gonderim sirasindaki cerceve */

	bool			link_up;	/* Aga bildirilen son durum    */
	bool			link_raw;	/* Bir onceki ham okuma        */
	u8			sock_mr;	/* Istenen Sn_MR degeri        */

	u8			*cmd;		/* SPI adres + kontrol tamponu */
	u8			*xfer_buf;	/* Kayit erisim tamponu        */
};

/* ================================================================== */
/* 4. gun: SPI register erisim katmani                                 */
/* ================================================================== */

static inline u8 w5500_ctrl(u8 bsb, u8 rwb)
{
	return (bsb << 3) | rwb | W5500_OM_VDM;
}

/*
 * Blok icindeki 'addr' adresinden 'len' bayt okur.
 *
 * DIKKAT: 'buf', SPI denetleyicisi tarafindan dogrudan kullanildigindan
 * DMA'ya uygun (yigin uzerinde olmayan) bir tampon olmalidir. Tek baytlik
 * ve 16 bitlik kayitlar icin asagidaki sarmalayicilar priv->xfer_buf
 * uzerinden calisir; cerceve verisi ise zaten kmalloc ile ayrilmis
 * sk_buff tamponuna okunur.
 */
static int w5500_read_buf(struct dnhat_priv *priv, u8 bsb, u16 addr,
			  void *buf, size_t len)
{
	struct spi_transfer xfer[2];
	struct spi_message msg;

	priv->cmd[0] = addr >> 8;			/* Adres yuksek bayt */
	priv->cmd[1] = addr & 0xFF;			/* Adres dusuk bayt  */
	priv->cmd[2] = w5500_ctrl(bsb, W5500_RWB_READ);

	memset(xfer, 0, sizeof(xfer));
	xfer[0].tx_buf = priv->cmd;			/* Adres + kontrol fazi */
	xfer[0].len    = DNHAT_CMD_LEN;
	xfer[1].rx_buf = buf;				/* Veri fazi            */
	xfer[1].len    = len;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer[0], &msg);
	spi_message_add_tail(&xfer[1], &msg);

	/* Tek mesaj: chip select islem ortasinda yukselmez. */
	return spi_sync(priv->spi, &msg);
}

static int w5500_write_buf(struct dnhat_priv *priv, u8 bsb, u16 addr,
			   const void *buf, size_t len)
{
	struct spi_transfer xfer[2];
	struct spi_message msg;

	priv->cmd[0] = addr >> 8;
	priv->cmd[1] = addr & 0xFF;
	priv->cmd[2] = w5500_ctrl(bsb, W5500_RWB_WRITE);

	memset(xfer, 0, sizeof(xfer));
	xfer[0].tx_buf = priv->cmd;
	xfer[0].len    = DNHAT_CMD_LEN;
	xfer[1].tx_buf = buf;
	xfer[1].len    = len;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer[0], &msg);
	spi_message_add_tail(&xfer[1], &msg);

	return spi_sync(priv->spi, &msg);
}

/* Tek baytlik ve 16 bitlik kayitlar icin kolaylik sarmalayicilari.
 * Hepsi DMA guvenli priv->xfer_buf uzerinden calisir.
 */
static int w5500_read8(struct dnhat_priv *priv, u8 bsb, u16 addr, u8 *val)
{
	int ret = w5500_read_buf(priv, bsb, addr, priv->xfer_buf, 1);

	if (!ret)
		*val = priv->xfer_buf[0];
	return ret;
}

static int w5500_write8(struct dnhat_priv *priv, u8 bsb, u16 addr, u8 val)
{
	priv->xfer_buf[0] = val;
	return w5500_write_buf(priv, bsb, addr, priv->xfer_buf, 1);
}

/*
 * 16 bitlik kayitlar tek islemde okunur (10. gun, Tablo 10.2: cerceve
 * basina SPI islemi sayisi 8'den 5'e dusurulmustur).
 *
 * Cip 16 bitlik kayitlari big-endian sirayla sunar; ana kart little-endian
 * calistigindan donusum zorunludur (4. gun, 4.4). Donusumun atlanmasi,
 * cokme degil, tampon isaretcilerinin kaymasiyla aciklanamayan paket
 * bozulmalari uretir.
 */
static int w5500_read16(struct dnhat_priv *priv, u8 bsb, u16 addr, u16 *val)
{
	int ret = w5500_read_buf(priv, bsb, addr, priv->xfer_buf,
				 sizeof(__be16));

	if (!ret)
		*val = be16_to_cpup((__be16 *)priv->xfer_buf);
	return ret;
}

static int w5500_write16(struct dnhat_priv *priv, u8 bsb, u16 addr, u16 val)
{
	*(__be16 *)priv->xfer_buf = cpu_to_be16(val);
	return w5500_write_buf(priv, bsb, addr, priv->xfer_buf,
			       sizeof(__be16));
}

/* Yigin uzerindeki kucuk dizileri (ornegin MAC adresi) DMA guvenli
 * tampona kopyalayarak yazar.
 */
static int w5500_write_bytes(struct dnhat_priv *priv, u8 bsb, u16 addr,
			     const void *src, size_t len)
{
	if (WARN_ON(len > DNHAT_XFER_SZ))
		return -EINVAL;

	memcpy(priv->xfer_buf, src, len);
	return w5500_write_buf(priv, bsb, addr, priv->xfer_buf, len);
}

/* Soket komutu. Komut, cip tarafindan islenince Sn_CR otomatik sifirlanir;
 * sonucun dogrulanmasi gereken yerlerde Sn_SR ayrica okunur.
 */
static int dnhat_send_cmd(struct dnhat_priv *priv, u8 cmd)
{
	return w5500_write8(priv, W5500_BSB_S0_REG, W5500_Sn_CR, cmd);
}

/* ================================================================== */
/* 9. gun: reset dizileri                                              */
/* ================================================================== */

/*
 * Donanimsal reset: RSTn hatti uzerinden.
 *
 * Device Tree'de GPIO_ACTIVE_LOW bildirildigi icin surucu mantiksal
 * seviye ile calisir; 1 = "reset aktif" anlamina gelir. GPIO islemleri
 * uyuyabildiginden gpiod_set_value_cansleep() kullanilmali, bekleme icin
 * mesgul bekleme yerine usleep_range() tercih edilmelidir.
 *
 * Reset sonrasi 2 ms'lik bekleme, kart uzerindeki RC devresinin olculen
 * 1.02 ms'lik yukselme suresi ve cipin PLL kilitlenme suresi icindir
 * (Tablo 9.2). 500 us'ye dusurulen denemede VERSIONR zaman zaman 0x00
 * okunmustur.
 */
static void dnhat_hw_reset(struct dnhat_priv *priv)
{
	gpiod_set_value_cansleep(priv->reset_gpio, 1);	/* RSTn = LOW  */
	usleep_range(DNHAT_RST_LOW_US_MIN, DNHAT_RST_LOW_US_MAX);
	gpiod_set_value_cansleep(priv->reset_gpio, 0);	/* RSTn = HIGH */
	usleep_range(DNHAT_PLL_LOCK_US_MIN, DNHAT_PLL_LOCK_US_MAX);
}

/*
 * Yazilimsal reset: MR kaydindaki RST biti.
 * Bit, reset tamamlandiginda cip tarafindan otomatik olarak temizlenir.
 */
static int dnhat_sw_reset(struct dnhat_priv *priv)
{
	int ret, tries = DNHAT_POLL_TRIES;
	u8 mr;

	ret = w5500_write8(priv, W5500_BSB_COMMON, W5500_REG_MR, W5500_MR_RST);
	if (ret)
		return ret;

	do {
		usleep_range(DNHAT_POLL_US_MIN, DNHAT_POLL_US_MAX);
		ret = w5500_read8(priv, W5500_BSB_COMMON, W5500_REG_MR, &mr);
		if (ret)
			return ret;
	} while ((mr & W5500_MR_RST) && --tries);

	if (mr & W5500_MR_RST) {
		dev_err(&priv->spi->dev, "yazilimsal reset tamamlanmadi\n");
		return -ETIMEDOUT;
	}
	return 0;
}

/* ================================================================== */
/* 11. ve 13. gun: kesme maskeleri ve MACRAW soketi                    */
/* ================================================================== */

/*
 * Cip tarafinda kesmelerin acilmasi iki kademelidir: once soket seviyesinde
 * hangi olaylarin kesme uretecegi (Sn_IMR), sonra genel seviyede hangi
 * soketin kesme uretebilecegi (SIMR) belirtilir.
 */
static int dnhat_enable_irqs(struct dnhat_priv *priv)
{
	int ret;

	/* Once bekleyen bayraklar temizlenir; aksi halde soket acilir
	 * acilmaz sahte bir kesme gorulebilir.
	 */
	ret = w5500_write8(priv, W5500_BSB_S0_REG, W5500_Sn_IR,
			   W5500_Sn_IR_ALL);
	if (ret)
		return ret;

	/* Soket 0 icin: veri alindi (RECV) ve gonderim tamamlandi (SEND_OK) */
	ret = w5500_write8(priv, W5500_BSB_S0_REG, W5500_Sn_IMR,
			   W5500_Sn_IR_RECV | W5500_Sn_IR_SEND_OK);
	if (ret)
		return ret;

	/* Genel maske: yalnizca soket 0 kesme uretebilir. */
	return w5500_write8(priv, W5500_BSB_COMMON, W5500_REG_SIMR,
			    W5500_SIMR_S0);
}

static int dnhat_disable_irqs(struct dnhat_priv *priv)
{
	int ret;

	ret = w5500_write8(priv, W5500_BSB_COMMON, W5500_REG_SIMR, 0);
	if (ret)
		return ret;

	return w5500_write8(priv, W5500_BSB_S0_REG, W5500_Sn_IMR, 0);
}

/*
 * Arayuzun bayraklarina gore istenen Sn_MR degeri.
 *
 * MACRAW modunda MF biti 1 iken cip yalnizca kendi MAC adresine ve
 * yayin adresine gelen cerceveleri alir. Promisc, allmulti veya bos
 * olmayan bir cok noktali yayin listesi varsa suzgec kapatilir ve
 * suzme isi ag yiginina birakilir.
 */
static u8 dnhat_sock_mr(struct net_device *ndev)
{
	u8 mr = W5500_Sn_MR_MACRAW;

	if (!(ndev->flags & (IFF_PROMISC | IFF_ALLMULTI)) &&
	    netdev_mc_empty(ndev))
		mr |= W5500_Sn_MR_MF;

	return mr;
}

/*
 * Soketin MACRAW moduna alinmasi (13. gun, Sekil 13.1).
 *
 * Sira kritiktir: MAC adresi soket acildiktan sonra yazilirsa cip eski
 * adresle calismaya devam eder; kesme, cip hazir olmadan acilirsa ilk
 * anda sahte kesmeler olusabilir.
 *
 * Cagiran priv->lock'u tutuyor olmalidir.
 */
static int dnhat_hw_open(struct dnhat_priv *priv)
{
	int ret, tries = DNHAT_POLL_TRIES;
	u8 status;

	/* 1) MAC adresi cipin SHAR kaydina yazilir. */
	ret = w5500_write_bytes(priv, W5500_BSB_COMMON, W5500_REG_SHAR,
				priv->ndev->dev_addr, ETH_ALEN);
	if (ret)
		return ret;

	/* 2) Soket 0 MACRAW moduna alinir. */
	ret = w5500_write8(priv, W5500_BSB_S0_REG, W5500_Sn_MR, priv->sock_mr);
	if (ret)
		return ret;

	/* 3) OPEN komutu verilir. */
	ret = dnhat_send_cmd(priv, W5500_Sn_CR_OPEN);
	if (ret)
		return ret;

	/* 4) Soket gercekten MACRAW durumuna gecti mi? Komut yazip durumu
	 *    kontrol etmemek, surucuyu cipin gercek durumundan habersiz
	 *    birakir.
	 */
	do {
		usleep_range(DNHAT_POLL_US_MIN, DNHAT_POLL_US_MAX);
		ret = w5500_read8(priv, W5500_BSB_S0_REG, W5500_Sn_SR, &status);
		if (ret)
			return ret;
	} while (status != W5500_SOCK_MACRAW && --tries);

	if (status != W5500_SOCK_MACRAW) {
		netdev_err(priv->ndev,
			   "soket MACRAW moduna gecmedi (Sn_SR = 0x%02x)\n",
			   status);
		return -EIO;
	}

	return dnhat_enable_irqs(priv);
}

/* Soketin kapatilmasi. Cagiran priv->lock'u tutuyor olmalidir. */
static int dnhat_hw_close(struct dnhat_priv *priv)
{
	dnhat_disable_irqs(priv);
	return dnhat_send_cmd(priv, W5500_Sn_CR_CLOSE);
}

/* ================================================================== */
/* 15. gun: alma (RX) yolu                                             */
/* ================================================================== */

/*
 * Kesme is parcaciginda cagrilir; priv->lock tutulmus durumdadir.
 *
 * Tek bir kesmede birden fazla cerceve bekliyor olabileceginden dongu
 * halinde calisir. Dongunun DNHAT_RX_BUDGET ile sinirlanmasi bilincli
 * bir karardir: sinir konulmazsa yogun trafik altinda is parcacigi
 * donguden hic cikamaz ve sistemin diger gorevlerine zaman kalmaz
 * (Tablo 15.1).
 */
static void dnhat_rx_process(struct dnhat_priv *priv)
{
	struct net_device *ndev = priv->ndev;
	u16 rx_size, rd_ptr, frame_len;
	int ret, guard = DNHAT_RX_BUDGET;
	bool first = true;
	struct sk_buff *skb;
	__be16 hdr;

	while (guard--) {
		/* 1) Bekleyen veri var mi? */
		ret = w5500_read16(priv, W5500_BSB_S0_REG, W5500_Sn_RX_RSR,
				   &rx_size);
		if (ret || rx_size < DNHAT_MACRAW_HDR)
			break;

		/* Cipin tamponu dolmak uzereyse surucu yetisemiyor demektir;
		 * 19. gunde paket kaybinin kaynagini ayirt eden sayac budur.
		 * Bosaltma turu basina en fazla bir kez artirilir.
		 */
		if (first && rx_size >= DNHAT_RX_FULL_MARK)
			ndev->stats.rx_over_errors++;
		first = false;

		/* 2) Okuma isaretcisi */
		ret = w5500_read16(priv, W5500_BSB_S0_REG, W5500_Sn_RX_RD,
				   &rd_ptr);
		if (ret)
			break;

		/* 3) MACRAW basligi: uzunluk, kendisi dahil toplam bayt sayisi */
		ret = w5500_read_buf(priv, W5500_BSB_S0_RX, rd_ptr,
				     priv->xfer_buf, DNHAT_MACRAW_HDR);
		if (ret)
			break;

		hdr = *(__be16 *)priv->xfer_buf;
		frame_len = be16_to_cpu(hdr) - DNHAT_MACRAW_HDR;

		/* Bozuk uzunluk: isaretciler kaymis demektir, cip sifirlanir. */
		if (frame_len < ETH_ZLEN || frame_len > ETH_FRAME_LEN ||
		    rx_size < frame_len + DNHAT_MACRAW_HDR) {
			netdev_err_ratelimited(ndev,
					       "gecersiz cerceve uzunlugu: %u\n",
					       frame_len);
			ndev->stats.rx_length_errors++;
			dnhat_sw_reset(priv);
			dnhat_hw_open(priv);
			break;
		}

		/* 4) skb ayrilir. NET_IP_ALIGN, IP basliginin hizali
		 *    baslamasini saglar.
		 */
		skb = netdev_alloc_skb(ndev, frame_len + NET_IP_ALIGN);
		if (!skb) {
			ndev->stats.rx_dropped++;
			break;
		}
		skb_reserve(skb, NET_IP_ALIGN);

		/* 5) Cerceve dogrudan skb tamponuna okunur (ara kopya yok). */
		ret = w5500_read_buf(priv, W5500_BSB_S0_RX,
				     rd_ptr + DNHAT_MACRAW_HDR,
				     skb_put(skb, frame_len), frame_len);
		if (ret) {
			dev_kfree_skb(skb);
			ndev->stats.rx_errors++;
			break;
		}

		/* 6) Isaretci ilerletilir ve cipe RECV komutu verilir. */
		rd_ptr += DNHAT_MACRAW_HDR + frame_len;
		w5500_write16(priv, W5500_BSB_S0_REG, W5500_Sn_RX_RD, rd_ptr);
		dnhat_send_cmd(priv, W5500_Sn_CR_RECV);

		/* 7) Protokol tipi cozulur ve cerceve ag yiginina verilir. */
		skb->protocol = eth_type_trans(skb, ndev);
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += frame_len;
		netif_rx(skb);
	}
}

/* ================================================================== */
/* 11. gun: threaded IRQ                                               */
/* ================================================================== */

/*
 * Ust yari: atomik baglamda calisir, hicbir SPI islemi yapmaz.
 * Gorevi yalnizca is parcacigini uyandirmaktir.
 */
static irqreturn_t dnhat_irq_top(int irq, void *dev_id)
{
	return IRQ_WAKE_THREAD;
}

/*
 * Alt yari: uyuyabilen baglam. Cipin kesme kaynagi burada okunur.
 *
 * Bayraklar isleme baslamadan once temizlenir. Temizleme ile isleme
 * arasinda yeni bir cerceve gelirse cip bayragi tekrar kaldirir ve
 * dongunun bir sonraki turunda yakalanir; ters siralamada ise o olay
 * kaybedilir ve kenar tetiklemeli yapilandirmada arayuz "bir paket
 * sonra donuyor" gibi teshisi zor bir hataya duser.
 */
static irqreturn_t dnhat_irq_thread(int irq, void *dev_id)
{
	struct dnhat_priv *priv = dev_id;
	int rounds = DNHAT_IRQ_ROUNDS;
	u16 pending;
	u8 sock_ir;
	int ret;

	mutex_lock(&priv->lock);

	while (rounds--) {
		/* Hangi olay kesmeyi uretti? */
		ret = w5500_read8(priv, W5500_BSB_S0_REG, W5500_Sn_IR,
				  &sock_ir);
		if (ret) {
			dev_err_ratelimited(&priv->spi->dev,
					    "Sn_IR okunamadi\n");
			break;
		}
		if (!sock_ir)
			break;

		/* Kesme bayraklari, ilgili bite 1 yazilarak temizlenir. */
		ret = w5500_write8(priv, W5500_BSB_S0_REG, W5500_Sn_IR,
				   sock_ir);
		if (ret)
			break;

		if (sock_ir & W5500_Sn_IR_RECV)
			dnhat_rx_process(priv);	   /* RX tamponu bosaltilir */

		if (sock_ir & W5500_Sn_IR_SEND_OK)
			netif_wake_queue(priv->ndev); /* TX kuyrugu serbest   */
	}

	/* RX butcesi nedeniyle cipte veri kalmis olabilir. Kesme kenar
	 * tetiklemeli oldugundan tampon burada bosaltilmazsa yeni bir
	 * cerceve gelene kadar bekler.
	 */
	rounds = DNHAT_IRQ_ROUNDS;
	while (rounds-- &&
	       !w5500_read16(priv, W5500_BSB_S0_REG, W5500_Sn_RX_RSR,
			     &pending) &&
	       pending >= DNHAT_MACRAW_HDR)
		dnhat_rx_process(priv);

	mutex_unlock(&priv->lock);
	return IRQ_HANDLED;
}

/*
 * IRQF_ONESHOT: is parcacigi bitene kadar kesme maskeli kalir.
 * Kenar tipi Device Tree'de tanimli oldugu icin burada tekrar
 * belirtilmesine gerek yoktur.
 */
static int dnhat_request_irq(struct dnhat_priv *priv)
{
	return devm_request_threaded_irq(&priv->spi->dev, priv->irq,
					 dnhat_irq_top,
					 dnhat_irq_thread,
					 IRQF_ONESHOT,
					 priv->ndev->name, priv);
}

/* ================================================================== */
/* 14. gun: gonderim (TX) yolu                                         */
/* ================================================================== */

/*
 * ndo_start_xmit() uyumayan bir baglamda cagrilir; cerceveyi cipe yazmak
 * ise SPI islemi, yani uyuyabilen bir islemdir. Bu nedenle cerceve burada
 * yalnizca saklanir ve gercek gonderim ornege ozel is kuyruguna devredilir.
 */
static netdev_tx_t dnhat_start_xmit(struct sk_buff *skb,
				    struct net_device *ndev)
{
	struct dnhat_priv *priv = netdev_priv(ndev);

	netif_stop_queue(ndev);
	priv->tx_skb = skb;
	queue_work(priv->wq, &priv->tx_work);

	return NETDEV_TX_OK;
}

static void dnhat_tx_work(struct work_struct *work)
{
	struct dnhat_priv *priv = container_of(work, struct dnhat_priv,
					       tx_work);
	struct net_device *ndev = priv->ndev;
	struct sk_buff *skb = priv->tx_skb;
	u16 free_size, wr_ptr;
	bool sent = false;
	int ret;

	if (!skb)
		return;

	mutex_lock(&priv->lock);

	/* 1) TX tamponunda yeterli bos alan var mi? */
	ret = w5500_read16(priv, W5500_BSB_S0_REG, W5500_Sn_TX_FSR,
			   &free_size);
	if (ret)
		goto err;

	if (free_size < skb->len) {
		ndev->stats.tx_fifo_errors++;	/* Cerceve dusurulur */
		goto err;
	}

	/* 2) Yazma isaretcisi okunur. */
	ret = w5500_read16(priv, W5500_BSB_S0_REG, W5500_Sn_TX_WR, &wr_ptr);
	if (ret)
		goto err;

	/* 3) Cerceve TX tamponuna yazilir.
	 *    Cip, 16 bitlik isaretciyi kendi icinde dolandirir; surucu
	 *    tarafinda ayrica maskeleme gerekmez. skb->data kmalloc ile
	 *    ayrildigindan DMA acisindan guvenlidir.
	 */
	ret = w5500_write_buf(priv, W5500_BSB_S0_TX, wr_ptr,
			      skb->data, skb->len);
	if (ret)
		goto err;

	/* 4) Isaretci ilerletilir. */
	ret = w5500_write16(priv, W5500_BSB_S0_REG, W5500_Sn_TX_WR,
			    wr_ptr + skb->len);
	if (ret)
		goto err;

	/* 5) SEND komutu: cip cerceveyi hatta cikarir. */
	ret = dnhat_send_cmd(priv, W5500_Sn_CR_SEND);
	if (ret)
		goto err;

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	sent = true;
	goto out;

err:
	ndev->stats.tx_errors++;
out:
	mutex_unlock(&priv->lock);
	dev_kfree_skb(skb);
	priv->tx_skb = NULL;

	/*
	 * Basarili gonderimde kuyruk, SEND_OK kesmesi geldiginde uyandirilir;
	 * kesme hic gelmezse watchdog devreye girer (ndo_tx_timeout).
	 * Hata durumunda ise cipe hicbir komut verilmemis olabileceginden
	 * kuyruk burada dogrudan acilir, aksi halde arayuz watchdog suresi
	 * boyunca gereksiz yere bloke kalir.
	 */
	if (!sent)
		netif_wake_queue(ndev);
}

/* Cipin yeniden baslatilmasi: yazilimsal reset + soketin yeniden acilmasi. */
static void dnhat_restart_work(struct work_struct *work)
{
	struct dnhat_priv *priv = container_of(work, struct dnhat_priv,
					       restart_work);
	int ret;

	mutex_lock(&priv->lock);
	ret = dnhat_sw_reset(priv);
	if (!ret)
		ret = dnhat_hw_open(priv);
	mutex_unlock(&priv->lock);

	if (ret) {
		netdev_err(priv->ndev, "cip yeniden baslatilamadi: %d\n", ret);
		return;
	}
	netif_wake_queue(priv->ndev);
}

/*
 * ndo_tx_timeout(), cekirdegin watchdog zamanlayicisi tarafindan ATOMIK
 * baglamda cagrilir. Cipe yapilacak her erisim bir SPI islemi, yani
 * uyuyabilen bir cagri oldugundan yeniden baslatma is kuyruguna
 * devredilir. Kuyruk sirali (ordered) oldugu icin, varsa bekleyen TX isi
 * tamamlandiktan sonra calisir.
 */
static void dnhat_tx_timeout(struct net_device *ndev, unsigned int txqueue)
{
	struct dnhat_priv *priv = netdev_priv(ndev);

	netdev_warn(ndev, "TX zaman asimi, cip yeniden baslatiliyor\n");
	ndev->stats.tx_errors++;
	queue_work(priv->wq, &priv->restart_work);
}

/* ================================================================== */
/* 17. gun: baglanti durumu ve carrier yonetimi                        */
/* ================================================================== */

/*
 * Cip, baglanti durumu degistiginde kesme uretmediginden PHYCFGR kaydinin
 * periyodik olarak yoklanmasi gerekir. 200 ms'lik aralik, algilama
 * gecikmesi ile SPI veri yoluna binen yuk arasindaki dengeden secilmistir
 * (Tablo 17.2); ag ekibinin yedeklilik gereksinimi 250 ms'nin altidir.
 *
 * 23. gunde eklenen kararlilik filtresi: durum degisimi ag yiginina
 * yalnizca iki ardisik yoklamada ayni okunursa bildirilir. Bu, kisa kablo
 * temassizliklarinda rotanin gidip gelmesini onler.
 */
static void dnhat_link_work(struct work_struct *work)
{
	struct dnhat_priv *priv = container_of(to_delayed_work(work),
					       struct dnhat_priv, link_work);
	struct net_device *ndev = priv->ndev;
	bool link_up;
	int ret;
	u8 phy;

	mutex_lock(&priv->lock);
	ret = w5500_read8(priv, W5500_BSB_COMMON, W5500_REG_PHYCFGR, &phy);
	mutex_unlock(&priv->lock);

	if (ret)
		goto reschedule;

	link_up = !!(phy & W5500_PHYCFGR_LNK);

	if (link_up != priv->link_raw) {
		/* Ilk gozlem: henuz kararli sayilmaz. */
		priv->link_raw = link_up;
	} else if (link_up != priv->link_up) {
		/* Iki ardisik okuma ayni: ag yiginina bildirilir. */
		priv->link_up = link_up;

		if (link_up) {
			netif_carrier_on(ndev);
			netdev_info(ndev, "link up, %s Mbps, %s duplex\n",
				    (phy & W5500_PHYCFGR_SPD) ? "100" : "10",
				    (phy & W5500_PHYCFGR_DPX) ? "full" : "half");
		} else {
			netif_carrier_off(ndev);
			netdev_info(ndev, "link down\n");
		}
	}

reschedule:
	queue_delayed_work(priv->wq, &priv->link_work,
			   msecs_to_jiffies(DNHAT_LINK_POLL_MS));
}

/* ================================================================== */
/* Promisc / cok noktali yayin                                         */
/* ================================================================== */

/*
 * ndo_set_rx_mode() atomik baglamda cagrilir; Sn_MR degisikligi ise
 * soketin kapatilip yeniden acilmasini, yani SPI islemi gerektirir.
 * Bu nedenle istenen deger burada yalnizca kaydedilir, uygulama is
 * kuyruguna devredilir.
 */
static void dnhat_rxmode_work(struct work_struct *work)
{
	struct dnhat_priv *priv = container_of(work, struct dnhat_priv,
					       rxmode_work);

	mutex_lock(&priv->lock);
	dnhat_hw_close(priv);
	dnhat_hw_open(priv);
	mutex_unlock(&priv->lock);
}

static void dnhat_set_rx_mode(struct net_device *ndev)
{
	struct dnhat_priv *priv = netdev_priv(ndev);
	u8 mr = dnhat_sock_mr(ndev);

	if (READ_ONCE(priv->sock_mr) == mr)
		return;

	WRITE_ONCE(priv->sock_mr, mr);

	if (netif_running(ndev))
		queue_work(priv->wq, &priv->rxmode_work);
}

static int dnhat_set_mac_address(struct net_device *ndev, void *addr)
{
	struct dnhat_priv *priv = netdev_priv(ndev);
	int ret;

	ret = eth_mac_addr(ndev, addr);
	if (ret)
		return ret;

	if (!netif_running(ndev))
		return 0;

	mutex_lock(&priv->lock);
	ret = w5500_write_bytes(priv, W5500_BSB_COMMON, W5500_REG_SHAR,
				ndev->dev_addr, ETH_ALEN);
	mutex_unlock(&priv->lock);

	return ret;
}

/* ================================================================== */
/* 13. gun: arayuz acma / kapama                                       */
/* ================================================================== */

static int dnhat_open(struct net_device *ndev)
{
	struct dnhat_priv *priv = netdev_priv(ndev);
	int ret;

	mutex_lock(&priv->lock);
	ret = dnhat_hw_open(priv);
	mutex_unlock(&priv->lock);
	if (ret)
		return ret;

	ret = dnhat_request_irq(priv);
	if (ret) {
		netdev_err(ndev, "kesme kaydedilemedi: %d\n", ret);
		mutex_lock(&priv->lock);
		dnhat_hw_close(priv);
		mutex_unlock(&priv->lock);
		return ret;
	}

	/* Baglanti durumu yoklamasi baslatilir (17. gun). */
	priv->link_up  = false;
	priv->link_raw = false;
	netif_carrier_off(ndev);
	queue_delayed_work(priv->wq, &priv->link_work,
			   msecs_to_jiffies(DNHAT_LINK_POLL_MS));

	netif_start_queue(ndev);	/* Ag yigini artik cerceve verebilir */
	netdev_info(ndev, "arayuz acildi\n");
	return 0;
}

/*
 * Kapatma sirasi (22. gun, Kod 22.2).
 *
 * Ilk gerceklemede kesme, is kuyrugundaki isler beklenmeden serbest
 * birakiliyordu; serbest birakma aninda hala calisan bir TX isi,
 * serbest birakilmis kaynaklara erisebiliyor ve cekirdek uyarisi
 * uretiliyordu. Dogru sira asagidadir.
 */
static int dnhat_stop(struct net_device *ndev)
{
	struct dnhat_priv *priv = netdev_priv(ndev);

	netif_stop_queue(ndev);				/* 1) Yeni is gelmesin */
	cancel_delayed_work_sync(&priv->link_work);	/* 2) Yoklama dursun   */
	cancel_work_sync(&priv->rxmode_work);		/* 3) Mod isi bitsin   */
	cancel_work_sync(&priv->restart_work);		/* 4) Reset isi bitsin */
	cancel_work_sync(&priv->tx_work);		/* 5) TX isi bitsin    */
	devm_free_irq(&priv->spi->dev, priv->irq, priv);/* 6) Sonra IRQ        */

	mutex_lock(&priv->lock);
	dnhat_hw_close(priv);				/* 7) Soket kapatilir  */
	mutex_unlock(&priv->lock);

	/* Is kuyrugunda islenmeden kalmis bir cerceve varsa serbest birakilir. */
	if (priv->tx_skb) {
		dev_kfree_skb(priv->tx_skb);
		priv->tx_skb = NULL;
	}

	netif_carrier_off(ndev);
	priv->link_up = false;

	netdev_info(ndev, "arayuz kapatildi\n");
	return 0;
}

/* ================================================================== */
/* 12. gun: net_device islem tablosu                                   */
/* ================================================================== */

static const struct net_device_ops dnhat_netdev_ops = {
	.ndo_open		= dnhat_open,		/* ip link set up   */
	.ndo_stop		= dnhat_stop,		/* ip link set down */
	.ndo_start_xmit		= dnhat_start_xmit,	/* Cerceve gonderimi */
	.ndo_set_rx_mode	= dnhat_set_rx_mode,	/* Promisc/multicast */
	.ndo_set_mac_address	= dnhat_set_mac_address,
	.ndo_tx_timeout		= dnhat_tx_timeout,
	.ndo_validate_addr	= eth_validate_addr,	/* Cekirdek yardimcisi */
};

/* ================================================================== */
/* 18. gun: ethtool destegi                                            */
/* ================================================================== */

static void dnhat_get_drvinfo(struct net_device *ndev,
			      struct ethtool_drvinfo *info)
{
	struct dnhat_priv *priv = netdev_priv(ndev);

	strscpy(info->driver, DNHAT_DRV_NAME, sizeof(info->driver));
	strscpy(info->version, DNHAT_VERSION, sizeof(info->version));
	strscpy(info->bus_info, dev_name(&priv->spi->dev),
		sizeof(info->bus_info));
}

static int dnhat_get_link_ksettings(struct net_device *ndev,
				    struct ethtool_link_ksettings *cmd)
{
	struct dnhat_priv *priv = netdev_priv(ndev);
	int ret;
	u8 phy;

	mutex_lock(&priv->lock);
	ret = w5500_read8(priv, W5500_BSB_COMMON, W5500_REG_PHYCFGR, &phy);
	mutex_unlock(&priv->lock);
	if (ret)
		return ret;

	/* Cipin destekledigi modlar: 10/100, yarim/tam, otomatik anlasma */
	ethtool_link_ksettings_zero_link_mode(cmd, supported);
	ethtool_link_ksettings_add_link_mode(cmd, supported, 10baseT_Half);
	ethtool_link_ksettings_add_link_mode(cmd, supported, 10baseT_Full);
	ethtool_link_ksettings_add_link_mode(cmd, supported, 100baseT_Half);
	ethtool_link_ksettings_add_link_mode(cmd, supported, 100baseT_Full);
	ethtool_link_ksettings_add_link_mode(cmd, supported, Autoneg);
	ethtool_link_ksettings_add_link_mode(cmd, supported, TP);

	cmd->base.speed   = (phy & W5500_PHYCFGR_SPD) ? SPEED_100 : SPEED_10;
	cmd->base.duplex  = (phy & W5500_PHYCFGR_DPX) ? DUPLEX_FULL
						      : DUPLEX_HALF;
	cmd->base.autoneg = AUTONEG_ENABLE;
	cmd->base.port    = PORT_TP;
	return 0;
}

static const struct ethtool_ops dnhat_ethtool_ops = {
	.get_drvinfo		= dnhat_get_drvinfo,
	.get_link		= ethtool_op_get_link,	/* carrier durumu */
	.get_link_ksettings	= dnhat_get_link_ksettings,
	.get_ts_info		= ethtool_op_get_ts_info,
};

/* ================================================================== */
/* 7. gun: Device Tree kaynaklarinin alinmasi                          */
/* ================================================================== */

/*
 * devm_ onekli fonksiyonlarin kullanilmasi bilincli bir karardir: bu
 * fonksiyonlarla alinan kaynaklar cihaz kaldirildiginda cekirdek
 * tarafindan otomatik olarak serbest birakilir ve DR-10 gereksiniminde
 * belirtilen guvenli kaldirma buyuk olcude garanti altina alinir.
 */
static int dnhat_get_resources(struct dnhat_priv *priv)
{
	struct spi_device *spi = priv->spi;
	struct device *dev = &spi->dev;
	int ret;

	/* 1) Reset hatti: DT'deki "reset-gpios" ozelliginden alinir.
	 *    GPIO_ACTIVE_LOW bilgisi DT'de oldugu icin surucu mantiksal
	 *    seviye ile calisir; 1 = "reset aktif" anlamina gelir.
	 */
	priv->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->reset_gpio),
				     "reset GPIO alinamadi\n");

	/* 2) Kesme numarasi: DT'deki "interrupts" ozelliginden cekirdek
	 *    tarafindan cozulup spi->irq alanina yerlestirilmistir.
	 */
	if (spi->irq <= 0)
		return dev_err_probe(dev, -EINVAL,
				     "kesme hatti tanimlanmamis\n");
	priv->irq = spi->irq;

	/* 3) MAC adresi: DT'de yoksa rastgele bir adres uretilir.
	 *    Kartta EEPROM bulunmadigindan local-mac-address kullanilir.
	 */
	ret = of_get_ethdev_address(dev->of_node, priv->ndev);
	if (ret) {
		eth_hw_addr_random(priv->ndev);
		dev_warn(dev, "DT'de MAC adresi yok, rastgele adres uretildi\n");
	}

	dev_info(dev, "kaynaklar: CS=%d, IRQ=%d, hiz=%u Hz\n",
		 spi_get_chipselect(spi, 0), priv->irq, spi->max_speed_hz);
	return 0;
}

/* ================================================================== */
/* 12. gun: net_device kurulumu                                        */
/* ================================================================== */

static void dnhat_destroy_wq(void *data)
{
	destroy_workqueue(data);
}

static int dnhat_setup_netdev(struct dnhat_priv *priv)
{
	struct net_device *ndev = priv->ndev;

	/* Ethernet arayuzu icin varsayilan alanlar (tip, MTU, yayin adresi,
	 * baslik uzunlugu vb.) alloc_etherdev() tarafindan zaten
	 * doldurulmustur.
	 */
	ndev->netdev_ops   = &dnhat_netdev_ops;
	ndev->ethtool_ops  = &dnhat_ethtool_ops;
	ndev->watchdog_timeo = DNHAT_TX_TIMEOUT;

	/* Cip donanimsal cerceve bolme veya yuk bosaltma desteklemez. */
	ndev->features    = 0;
	ndev->hw_features = 0;

	/* MACRAW modunda standart Ethernet cerceve sinirlari gecerlidir. */
	ndev->min_mtu = ETH_MIN_MTU;	/*   68 bayt */
	ndev->max_mtu = ETH_DATA_LEN;	/* 1500 bayt */

	priv->sock_mr = W5500_Sn_MR_MACRAW | W5500_Sn_MR_MF;

	/* Ornek basina is kuyrugu: iki NIC birbirini bloklamaz (DR-11).
	 * WQ_MEM_RECLAIM, ag yolunun bellek baskisi altinda da ilerlemesini
	 * saglar.
	 */
	priv->wq = alloc_ordered_workqueue("%s-wq", WQ_MEM_RECLAIM,
					   dev_name(&priv->spi->dev));
	if (!priv->wq)
		return -ENOMEM;

	/* Is kuyrugu, netdev kaydindan once kaydedilir; devm teardown
	 * ters sirada calistigi icin once netdev kaldirilir, sonra kuyruk
	 * yok edilir.
	 */
	if (devm_add_action_or_reset(&priv->spi->dev, dnhat_destroy_wq,
				     priv->wq))
		return -ENOMEM;

	INIT_WORK(&priv->tx_work, dnhat_tx_work);
	INIT_WORK(&priv->rxmode_work, dnhat_rxmode_work);
	INIT_WORK(&priv->restart_work, dnhat_restart_work);
	INIT_DELAYED_WORK(&priv->link_work, dnhat_link_work);
	return 0;
}

/* ================================================================== */
/* 8. gun: probe() / remove()                                          */
/* ================================================================== */

/*
 * probe(), eslesme gerceklestikten sonra HER CIHAZ ORNEGI ICIN ayri ayri
 * cagrilir. Kartta iki adet W5500 bulundugundan iki kez calisir ve
 * birbirinden tamamen bagimsiz iki cihaz baglami olusturur (DR-11).
 */
static int dnhat_probe(struct spi_device *spi)
{
	struct net_device *ndev;
	struct dnhat_priv *priv;
	u8 version;
	int ret;

	/* net_device ve ozel baglam tek seferde ayrilir. */
	ndev = devm_alloc_etherdev(&spi->dev, sizeof(struct dnhat_priv));
	if (!ndev)
		return -ENOMEM;

	priv = netdev_priv(ndev);
	priv->spi  = spi;
	priv->ndev = ndev;
	mutex_init(&priv->lock);
	SET_NETDEV_DEV(ndev, &spi->dev);
	spi_set_drvdata(spi, priv);

	ret = dnhat_get_resources(priv);
	if (ret)
		return ret;

	/* SPI parametreleri: Mod 0, 8 bit. */
	spi->mode         = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "spi_setup basarisiz\n");

	/* Aktarim tamponlari DMA'ya uygun bicimde ayrilir (4. gun, karar 3). */
	priv->cmd      = devm_kzalloc(&spi->dev, DNHAT_CMD_LEN, GFP_KERNEL);
	priv->xfer_buf = devm_kzalloc(&spi->dev, DNHAT_XFER_SZ, GFP_KERNEL);
	if (!priv->cmd || !priv->xfer_buf)
		return -ENOMEM;

	/* Donanimsal reset ve cip kimlik dogrulamasi. */
	dnhat_hw_reset(priv);

	ret = w5500_read8(priv, W5500_BSB_COMMON, W5500_REG_VERSIONR,
			  &version);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "SPI okumasi basarisiz\n");

	if (version != W5500_VERSION_ID) {
		dev_err(&spi->dev,
			"beklenmeyen cip surumu: 0x%02x (beklenen 0x%02x)\n",
			version, W5500_VERSION_ID);
		return -ENODEV;
	}
	dev_info(&spi->dev, "W5500 dogrulandi (VERSIONR = 0x%02x)\n", version);

	ret = dnhat_setup_netdev(priv);		/* net_device_ops baglanir */
	if (ret)
		return ret;

	ret = devm_register_netdev(&spi->dev, ndev);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "netdev kaydi basarisiz\n");

	dev_info(&spi->dev, "%s: DualNIC-HAT arayuzu hazir, MAC %pM\n",
		 ndev->name, ndev->dev_addr);
	return 0;
}

static void dnhat_remove(struct spi_device *spi)
{
	struct dnhat_priv *priv = spi_get_drvdata(spi);

	/* devm_ ile alinan kaynaklar otomatik serbest birakilir;
	 * burada yalnizca cip guvenli duruma alinir.
	 */
	dnhat_hw_reset(priv);
	dev_info(&spi->dev, "cihaz kaldirildi\n");
}

/* ================================================================== */
/* 6. gun: surucu-cihaz eslesmesi                                      */
/* ================================================================== */

/*
 * SPI numaralandirilamaz bir veri yoludur: hattaki bir cipin kimligini
 * soracak standart bir mekanizma yoktur. Eslesme bu nedenle Device Tree
 * uzerinden yapilir. Buradaki compatible dizisi, Device Tree'deki dize
 * ile karakteri karakterine ayni olmalidir.
 */
static const struct of_device_id dnhat_of_match[] = {
	{ .compatible = "ctech,dnhat-w5500" },
	{ }						/* Sonlandirici kayit */
};
MODULE_DEVICE_TABLE(of, dnhat_of_match);

/* Device Tree kullanilmayan durumlar icin yedek eslesme tablosu. */
static const struct spi_device_id dnhat_spi_ids[] = {
	{ "dnhat-w5500", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, dnhat_spi_ids);

static struct spi_driver dnhat_spi_driver = {
	.driver = {
		.name		= DNHAT_DRV_NAME,
		.of_match_table	= dnhat_of_match,
	},
	.id_table	= dnhat_spi_ids,
	.probe		= dnhat_probe,
	.remove		= dnhat_remove,
};

/* module_init / module_exit yerine tek satirlik kolaylik makrosu. */
module_spi_driver(dnhat_spi_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DualNIC-HAT W5500 SPI Ethernet NIC surucusu");
MODULE_AUTHOR("Staj Projesi -- NIC Surucu Ekibi");
MODULE_VERSION(DNHAT_VERSION);
