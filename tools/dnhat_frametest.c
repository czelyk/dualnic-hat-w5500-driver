// SPDX-License-Identifier: GPL-2.0
/*
 * dnhat_frametest.c -- NIC surucusunu ham cerceve duzeyinde dogrular.
 *
 * Amac uygulama gelistirmek degil, surucunun urettigi cerceveyi bayt bayt
 * kontrol edebilmektir. AF_PACKET soketi kullanildigindan IP yigini hic
 * devreye girmez; surucuye tam olarak istenen baytlar verilir ve kablodaki
 * sonuc karsi uctan gozlenebilir.
 *
 * Yuk olarak artan bayt dizisi kullanilmasi bilincli bir tercihtir: tek
 * baytlik bir kayma dahi dokumde hemen fark edilir.
 *
 * Derleme:  make
 * Gonderim: sudo ./dnhat_frametest -i eth1 -s 60 -c 1000
 * Dinleme:  sudo ./dnhat_frametest -i eth2 -l -c 1000
 */

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* Deneysel EtherType: uretim trafigiyle karismamasi icin secilmistir. */
#define DNHAT_TEST_ETHERTYPE	0x88B5

#define DNHAT_MIN_FRAME		60		/* Asgari Ethernet cercevesi */
#define DNHAT_MAX_FRAME		ETH_FRAME_LEN	/* 1514 bayt                 */
#define DNHAT_HDR_LEN		14

struct opts {
	const char	*ifname;
	unsigned int	size;
	unsigned int	count;
	int		listen;
	int		verbose;
};

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Kullanim: %s [-i arayuz] [-s boyut] [-c adet] [-l] [-v]\n"
		"  -i  arayuz adi           (varsayilan: eth1)\n"
		"  -s  cerceve boyutu bayt  (%d..%d, varsayilan: %d)\n"
		"  -c  cerceve adedi        (varsayilan: 1)\n"
		"  -l  dinleme modu: gelen test cercevelerini dogrular\n"
		"  -v  ilk cercevenin bayt dokumunu yazdir\n",
		argv0, DNHAT_MIN_FRAME, DNHAT_MAX_FRAME, DNHAT_MIN_FRAME);
}

/* Test cercevesini kurar: hedef yayin, kaynak arayuzun MAC'i olmadan
 * (cekirdek doldurur), ardindan artan bayt dizisi.
 */
static void build_frame(unsigned char *frame, unsigned int len,
			unsigned int seq)
{
	unsigned int i;

	memset(frame, 0xFF, ETH_ALEN);		/* Hedef: broadcast */
	frame[6]  = 0x02;			/* Kaynak MAC       */
	frame[7]  = 0x00;
	frame[8]  = 0x00;
	frame[9]  = 0x00;
	frame[10] = 0x00;
	frame[11] = 0x01;
	frame[12] = (DNHAT_TEST_ETHERTYPE >> 8) & 0xFF;
	frame[13] = DNHAT_TEST_ETHERTYPE & 0xFF;

	/* Ilk iki yuk bayti sira numarasi, gerisi artan bayt dizisi. */
	frame[14] = (seq >> 8) & 0xFF;
	frame[15] = seq & 0xFF;
	for (i = 16; i < len; i++)
		frame[i] = (unsigned char)(i - DNHAT_HDR_LEN);
}

/* Alinan cercevenin gonderilenle birebir ayni olup olmadigini denetler. */
static int verify_frame(const unsigned char *frame, unsigned int len)
{
	unsigned int i;

	for (i = 16; i < len; i++) {
		if (frame[i] != (unsigned char)(i - DNHAT_HDR_LEN)) {
			fprintf(stderr,
				"bozulma: offset %u, beklenen 0x%02x, "
				"okunan 0x%02x\n",
				i, (unsigned char)(i - DNHAT_HDR_LEN),
				frame[i]);
			return -1;
		}
	}
	return 0;
}

static void dump_frame(const unsigned char *frame, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; i++) {
		if (i % 16 == 0)
			printf("\n    0x%04x: ", i);
		printf("%02x ", frame[i]);
	}
	printf("\n");
}

static int open_socket(const char *ifname, struct sockaddr_ll *addr)
{
	int fd;

	fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0) {
		perror("socket");
		return -1;
	}

	memset(addr, 0, sizeof(*addr));
	addr->sll_family   = AF_PACKET;
	addr->sll_protocol = htons(ETH_P_ALL);
	addr->sll_ifindex  = if_nametoindex(ifname);
	addr->sll_halen    = ETH_ALEN;
	memset(addr->sll_addr, 0xFF, ETH_ALEN);		/* Yayin adresi */

	if (!addr->sll_ifindex) {
		fprintf(stderr, "arayuz bulunamadi: %s\n", ifname);
		close(fd);
		return -1;
	}
	return fd;
}

static int do_send(const struct opts *o)
{
	unsigned char frame[DNHAT_MAX_FRAME];
	struct sockaddr_ll addr;
	struct timeval t0, t1;
	unsigned int sent = 0, i;
	double secs, mbps;
	int fd;

	fd = open_socket(o->ifname, &addr);
	if (fd < 0)
		return 1;

	gettimeofday(&t0, NULL);
	for (i = 0; i < o->count; i++) {
		build_frame(frame, o->size, i);

		if (i == 0 && o->verbose)
			dump_frame(frame, o->size);

		if (sendto(fd, frame, o->size, 0,
			   (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			perror("sendto");
			break;
		}
		sent++;
	}
	gettimeofday(&t1, NULL);

	secs = (t1.tv_sec - t0.tv_sec) +
	       (t1.tv_usec - t0.tv_usec) / 1000000.0;
	mbps = secs > 0 ? (sent * (double)o->size * 8) / (secs * 1000000.0) : 0;

	printf("%s uzerinden %u bayt x %u cerceve gonderildi "
	       "(%.3f s, %.2f Mbps)\n", o->ifname, o->size, sent, secs, mbps);

	close(fd);
	return sent == o->count ? 0 : 1;
}

static int do_listen(const struct opts *o)
{
	unsigned char frame[DNHAT_MAX_FRAME + 64];
	struct sockaddr_ll addr;
	unsigned int good = 0, bad = 0;
	int fd;
	ssize_t n;

	fd = open_socket(o->ifname, &addr);
	if (fd < 0)
		return 1;

	printf("%s dinleniyor, %u cerceve bekleniyor...\n",
	       o->ifname, o->count);

	while (good + bad < o->count) {
		n = recvfrom(fd, frame, sizeof(frame), 0, NULL, NULL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("recvfrom");
			break;
		}
		if (n < DNHAT_HDR_LEN)
			continue;

		/* Yalnizca test cerceveleri sayilir. */
		if (((frame[12] << 8) | frame[13]) != DNHAT_TEST_ETHERTYPE)
			continue;

		if (good + bad == 0 && o->verbose)
			dump_frame(frame, (unsigned int)n);

		if (verify_frame(frame, (unsigned int)n) == 0)
			good++;
		else
			bad++;
	}

	printf("alinan: %u hatasiz, %u bozuk\n", good, bad);
	close(fd);
	return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
	struct opts o = {
		.ifname = "eth1",
		.size   = DNHAT_MIN_FRAME,
		.count  = 1,
		.listen = 0,
		.verbose = 0,
	};
	int c;

	while ((c = getopt(argc, argv, "i:s:c:lvh")) != -1) {
		switch (c) {
		case 'i':
			o.ifname = optarg;
			break;
		case 's':
			o.size = (unsigned int)atoi(optarg);
			break;
		case 'c':
			o.count = (unsigned int)atoi(optarg);
			break;
		case 'l':
			o.listen = 1;
			break;
		case 'v':
			o.verbose = 1;
			break;
		default:
			usage(argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}

	if (o.size < DNHAT_MIN_FRAME || o.size > DNHAT_MAX_FRAME) {
		fprintf(stderr, "gecersiz boyut: %u (%d..%d olmali)\n",
			o.size, DNHAT_MIN_FRAME, DNHAT_MAX_FRAME);
		return 1;
	}

	return o.listen ? do_listen(&o) : do_send(&o);
}
