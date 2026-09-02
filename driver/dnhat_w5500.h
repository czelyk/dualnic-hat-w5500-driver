/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dnhat_w5500.h -- DualNIC-HAT projesi
 *
 * WIZnet W5500 SPI Ethernet denetleyicisinin, NIC surucusu tarafindan
 * kullanilan kayit haritasi ve surucu ayar sabitleri.
 *
 * Buradaki kayitlar, staj defterinin 3. gununde (Tablo 3.1) cikarilan
 * "surucunun gercekten kullandigi kayitlar" listesidir; veri sayfasindaki
 * tum kayitlar bilincli olarak tanimlanmamistir.
 */

#ifndef _DNHAT_W5500_H
#define _DNHAT_W5500_H

#include <linux/bitops.h>
#include <linux/if_ether.h>

#define DNHAT_DRV_NAME		"dnhat_w5500"
#define DNHAT_VERSION		"1.1"

/* ------------------------------------------------------------------ */
/* SPI cerceve kontrol bayti (4. gun, Sekil 4.1)                       */
/*                                                                     */
/*   [15:0] adres | [7:3] BSB | [2] RWB | [1:0] OM                     */
/* ------------------------------------------------------------------ */

/* Blok secim alani (BSB): kayitlar duz bir adres uzayinda degildir. */
#define W5500_BSB_COMMON	0x00	/* Ortak kayit blogu     */
#define W5500_BSB_S0_REG	0x01	/* Soket 0 kayit blogu   */
#define W5500_BSB_S0_TX		0x02	/* Soket 0 TX tamponu    */
#define W5500_BSB_S0_RX		0x03	/* Soket 0 RX tamponu    */

/* Soket n icin blok secim degerleri 4'er artar. */
#define W5500_BSB_Sn_REG(n)	(0x01 + ((n) << 2))
#define W5500_BSB_Sn_TX(n)	(0x02 + ((n) << 2))
#define W5500_BSB_Sn_RX(n)	(0x03 + ((n) << 2))

/* Okuma/yazma biti */
#define W5500_RWB_READ		0x00
#define W5500_RWB_WRITE		0x04

/* Uzunluk modu: degisken uzunluk (VDM). Chip select dusuk kaldigi
 * surece istenen kadar bayt aktarilabilir (4. gun, karar 1).
 */
#define W5500_OM_VDM		0x00

/* ------------------------------------------------------------------ */
/* Ortak kayit blogu                                                   */
/* ------------------------------------------------------------------ */
#define W5500_REG_MR		0x0000	/* Mode Register            */
#define W5500_REG_SHAR		0x0009	/* Source Hardware Address  */
#define W5500_REG_SIR		0x0017	/* Socket Interrupt         */
#define W5500_REG_SIMR		0x0018	/* Socket Interrupt Mask    */
#define W5500_REG_PHYCFGR	0x002E	/* PHY Configuration        */
#define W5500_REG_VERSIONR	0x0039	/* Chip Version             */

#define W5500_MR_RST		BIT(7)	/* Yazilimsal reset         */

#define W5500_SIMR_S0		BIT(0)	/* Soket 0 kesme maskesi    */

#define W5500_PHYCFGR_LNK	BIT(0)	/* 1 = link up              */
#define W5500_PHYCFGR_SPD	BIT(1)	/* 1 = 100 Mbps             */
#define W5500_PHYCFGR_DPX	BIT(2)	/* 1 = full duplex          */

/* Bring-up dogrulama kriteri (1. gun, arayuz sozlesmesi). */
#define W5500_VERSION_ID	0x04

/* ------------------------------------------------------------------ */
/* Soket 0 kayit blogu                                                 */
/* ------------------------------------------------------------------ */
#define W5500_Sn_MR		0x0000	/* Soket modu               */
#define W5500_Sn_CR		0x0001	/* Soket komutu             */
#define W5500_Sn_IR		0x0002	/* Soket kesme bayraklari   */
#define W5500_Sn_SR		0x0003	/* Soket durumu             */
#define W5500_Sn_RXBUF_SIZE	0x001E	/* RX tampon boyutu (KB)    */
#define W5500_Sn_TXBUF_SIZE	0x001F	/* TX tampon boyutu (KB)    */
#define W5500_Sn_TX_FSR		0x0020	/* TX bos alan              */
#define W5500_Sn_TX_WR		0x0024	/* TX yazma isaretcisi      */
#define W5500_Sn_RX_RSR		0x0026	/* RX bekleyen bayt sayisi  */
#define W5500_Sn_RX_RD		0x0028	/* RX okuma isaretcisi      */
#define W5500_Sn_IMR		0x002C	/* Soket kesme maskesi      */

/* Sn_MR: MACRAW modu, cipin TCP/IP yigini devre disi (3. gun, Tablo 3.2). */
#define W5500_Sn_MR_MACRAW	0x04
#define W5500_Sn_MR_MF		BIT(7)	/* MAC filtresi etkin       */

/* Sn_CR komutlari */
#define W5500_Sn_CR_OPEN	0x01
#define W5500_Sn_CR_CLOSE	0x10
#define W5500_Sn_CR_SEND	0x20
#define W5500_Sn_CR_RECV	0x40

/* Sn_SR durumlari (13. gun, Sekil 13.1) */
#define W5500_SOCK_CLOSED	0x00
#define W5500_SOCK_MACRAW	0x42

/* Sn_IR / Sn_IMR bit alanlari */
#define W5500_Sn_IR_RECV	BIT(2)	/* Cerceve alindi           */
#define W5500_Sn_IR_SEND_OK	BIT(4)	/* Gonderim tamamlandi      */
#define W5500_Sn_IR_ALL		0xFF

/* W5500'de RX ve TX icin toplam 16 KB ayri tampon alani vardir.
 * MACRAW modunda yalnizca Socket 0 kullanildigindan tum alan Socket 0'a
 * ayrilir, Socket 1..7 kapatilir.
 */
#define W5500_SOCKET_COUNT	8
#define W5500_BUF_KB_0		0x00
#define W5500_BUF_KB_16		0x10

/* ------------------------------------------------------------------ */
/* Surucu ayar sabitleri                                               */
/* ------------------------------------------------------------------ */

/* MACRAW basligi: RX tamponundan okunan ilk iki bayt, cerceve degil
 * kendisi dahil toplam bayt sayisidir (3. gun, Sekil 3.2).
 */
#define DNHAT_MACRAW_HDR	2

/* Tek kesmede islenecek azami cerceve sayisi (15. gun, Tablo 15.1). */
#define DNHAT_RX_BUDGET		16

/* MACRAW modunda soket 0, cipin 16 kB'lik tamponunun tamamini kullanir.
 * Bekleyen bayt sayisinin bu degere yaklasmasi, surucunun tamponu
 * yeterince hizli bosaltamadigi anlamina gelir (19. gun, Tablo 19.1).
 */
#define DNHAT_RX_BUF_SZ		(16 * 1024)
#define DNHAT_RX_FULL_MARK	(DNHAT_RX_BUF_SZ - ETH_FRAME_LEN)

/* Kesme is parcaciginda yapilacak azami Sn_IR turu. Butce dolarsa
 * cipte veri kalabilecegi icin tampon burada bosaltilir.
 */
#define DNHAT_IRQ_ROUNDS	4

/* W5500 veri sayfasi, asenkron degisebilen Sn_TX_FSR ve Sn_RX_RSR
 * kayitlarinin iki ardisik ayni deger elde edilene kadar okunmasini
 * onerir.
 */
#define DNHAT_STABLE_READ_TRIES	8

/* Baglanti durumu yoklama araligi (17. gun, Tablo 17.2). */
#define DNHAT_LINK_POLL_MS	200

/* TX zaman asimi: SEND_OK kesmesi hic gelmezse watchdog devreye girer. */
#define DNHAT_TX_TIMEOUT	(5 * HZ)

/* SPI islemi basligi: 16 bit adres + 8 bit kontrol. */
#define DNHAT_CMD_LEN		3

/* Kayit erisimlerinde kullanilan DMA guvenli tampon (4. gun, karar 3). */
#define DNHAT_XFER_SZ		(ETH_FRAME_LEN + DNHAT_MACRAW_HDR)

/* Ethernet'in minimum kablo-ustu cerceve boyutu (FCS haric). */
#define DNHAT_TX_MIN_FRAME	ETH_ZLEN

/* Reset darbesi zamanlamalari (9. gun, Tablo 9.2). */
#define DNHAT_RST_LOW_US_MIN	600
#define DNHAT_RST_LOW_US_MAX	800
#define DNHAT_PLL_LOCK_US_MIN	2000
#define DNHAT_PLL_LOCK_US_MAX	2500

/* Yazilimsal reset ve soket acma icin yoklama sinirlari. */
#define DNHAT_POLL_TRIES	20
#define DNHAT_POLL_US_MIN	100
#define DNHAT_POLL_US_MAX	300

#endif /* _DNHAT_W5500_H 
