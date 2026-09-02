// SPDX-License-Identifier: GPL-2.0
/*
 * dnhat_frametest.c -- DualNIC-HAT NIC surucusunu ham Ethernet
 * cercevesi seviyesinde dogrulayan test araci.
 *
 * Bu arac uygulama gelistirmek icin degil, surucunun TX/RX yolunu
 * Ethernet basligi ve yuk baytlari seviyesinde dogrulamak icin tasarlanmistir.
 * AF_PACKET/SOCK_RAW kullanildigi icin IP/TCP/UDP katmanlari devreye girmez.
 *
 * Gonderilen test cercevesi:
 *
 *   dst MAC   : ff:ff:ff:ff:ff:ff
 *   src MAC   : secilen arayuzun gercek MAC adresi
 *   EtherType : 0x88B5 (deneysel/yerel test amacli)
 *   payload   : 16 bit sira numarasi + deterministik bayt deseni
 *
 * Derleme:
 *   cc -O2 -Wall -Wextra -Wpedantic -o dnhat_frametest dnhat_frametest.c
 *
 * Gonderim:
 *   sudo ./dnhat_frametest -i eth1 -s 1514 -c 1000
 *
 * Dinleme:
 *   sudo ./dnhat_frametest -i eth2 -l -s 1514 -c 1000 -t 5000
 */

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DNHAT_TEST_ETHERTYPE    0x88B5

#define DNHAT_MIN_FRAME         60U
#define DNHAT_MAX_FRAME         ETH_FRAME_LEN
#define DNHAT_HDR_LEN           ETH_HLEN
#define DNHAT_SEQ_LEN           2U
#define DNHAT_MIN_PAYLOAD       (DNHAT_SEQ_LEN)
#define DNHAT_DEFAULT_TIMEOUT   5000U

struct opts {
	const char *ifname;
	unsigned int size;
	unsigned int count;
	unsigned int timeout_ms;
	int listen;
	int verbose;
};

struct listen_stats {
	unsigned int good;
	unsigned int bad;
	unsigned int gaps;
	unsigned int duplicates;
	unsigned int reordered;
};

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Kullanim: %s [-i arayuz] [-s boyut] [-c adet] [-t ms] [-l] [-v]\n"
		"  -i  arayuz adi           (varsayilan: eth1)\n"
		"  -s  cerceve boyutu       (%u..%u, varsayilan: %u)\n"
		"  -c  cerceve adedi        (varsayilan: 1)\n"
		"  -t  dinleme timeout'u ms (varsayilan: %u)\n"
		"  -l  dinleme modu\n"
		"  -v  ilk gecerli cercevenin bayt dokumunu yazdir\n"
		"  -h  yardim\n",
		argv0, DNHAT_MIN_FRAME, DNHAT_MAX_FRAME,
		DNHAT_MIN_FRAME, DNHAT_DEFAULT_TIMEOUT);
}

static int parse_u32(const char *text, unsigned int min,
		     unsigned int max, unsigned int *out)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || !end || *end != '\0' || value < min || value > max)
		return -1;

	*out = (unsigned int)value;
	return 0;
}

static double elapsed_sec(const struct timespec *start,
			  const struct timespec *end)
{
	return (double)(end->tv_sec - start->tv_sec) +
	       (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static void format_mac(const unsigned char mac[ETH_ALEN],
		       char *buf, size_t len)
{
	snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int get_interface_info(int fd, const char *ifname,
			      int *ifindex, unsigned char mac[ETH_ALEN])
{
	struct ifreq ifr;

	memset(&ifr, 0, sizeof(ifr));
	if (strlen(ifname) >= sizeof(ifr.ifr_name)) {
		fprintf(stderr, "arayuz adi cok uzun: %s\n", ifname);
		return -1;
	}

	strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);

	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		perror("SIOCGIFINDEX");
		return -1;
	}
	*ifindex = ifr.ifr_ifindex;

	if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
		perror("SIOCGIFHWADDR");
		return -1;
	}
	memcpy(mac, ifr.ifr_hwaddr.sa_data, ETH_ALEN);

	return 0;
}

static int open_packet_socket(const char *ifname, int *ifindex,
			      unsigned char mac[ETH_ALEN])
{
	struct sockaddr_ll bind_addr;
	int fd;

#ifdef PACKET_IGNORE_OUTGOING
	int one = 1;
#endif

	/*
	 * Yalnizca DualNIC-HAT test EtherType'ini alacak packet socket.
	 *
	 * ETH_P_ALL yerine test protokolunun secilmesi, aracın ARP, IPv4,
	 * IPv6 gibi ilgisiz Ethernet trafigini kullanici alanina tasimamasini
	 * saglar.
	 */
	fd = socket(AF_PACKET, SOCK_RAW, htons(DNHAT_TEST_ETHERTYPE));
	if (fd < 0) {
		perror("socket(AF_PACKET)");
		return -1;
	}

	if (get_interface_info(fd, ifname, ifindex, mac) < 0) {
		close(fd);
		return -1;
	}

	/*
	 * Packet socket'i secilen fiziksel NIC'e sabitle.
	 *
	 * Bu bind yapilmazsa socket ayni host uzerindeki diger Ethernet
	 * arayuzlerinden gelen uygun protokollu frame'leri de gorebilir.
	 */
	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.sll_family = AF_PACKET;
	bind_addr.sll_protocol = htons(DNHAT_TEST_ETHERTYPE);
	bind_addr.sll_ifindex = *ifindex;

	if (bind(fd,
		 (struct sockaddr *)&bind_addr,
		 sizeof(bind_addr)) < 0) {
		perror("bind(AF_PACKET)");
		close(fd);
		return -1;
	}

#ifdef PACKET_IGNORE_OUTGOING
	/*
	 * Test araci ayni hostta hem gonderim hem dinleme icin kullanilirsa
	 * hostun kendi TX frame'lerini RX sonucu olarak sayma.
	 */
	if (setsockopt(fd,
		       SOL_PACKET,
		       PACKET_IGNORE_OUTGOING,
		       &one,
		       sizeof(one)) < 0 &&
	    errno != ENOPROTOOPT)
		perror("setsockopt(PACKET_IGNORE_OUTGOING)");
#endif

	return fd;
}

static void build_frame(unsigned char *frame, unsigned int len,
			const unsigned char src_mac[ETH_ALEN],
			uint16_t seq)
{
	unsigned int i;

	/* Destination MAC: broadcast */
	memset(frame, 0xFF, ETH_ALEN);

	/* Source MAC: test edilen NIC'in gercek MAC adresi */
	memcpy(frame + ETH_ALEN, src_mac, ETH_ALEN);

	/* EtherType */
	frame[12] = (unsigned char)(DNHAT_TEST_ETHERTYPE >> 8);
	frame[13] = (unsigned char)(DNHAT_TEST_ETHERTYPE & 0xFF);

	/* Payload'un ilk iki bayti sequence number */
	frame[14] = (unsigned char)(seq >> 8);
	frame[15] = (unsigned char)(seq & 0xFF);

	/*
	 * Geri kalan payload deterministik bir desen.
	 *
	 * Tek baytlik kayma, pointer/wrap veya MACRAW uzunluk hatalari
	 * verify_frame() tarafinda kolayca gorulebilir.
	 */
	for (i = DNHAT_HDR_LEN + DNHAT_SEQ_LEN; i < len; i++)
		frame[i] = (unsigned char)(i - DNHAT_HDR_LEN);
}

static int verify_frame(const unsigned char *frame,
			unsigned int len,
			unsigned int expected_len,
			uint16_t *seq)
{
	static const unsigned char broadcast[ETH_ALEN] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	unsigned int i;
	uint16_t ethertype;

	if (len != expected_len) {
		fprintf(stderr,
			"boyut uyusmazligi: beklenen %u, alinan %u\n",
			expected_len, len);
		return -1;
	}

	if (len < DNHAT_HDR_LEN + DNHAT_MIN_PAYLOAD) {
		fprintf(stderr, "cerceve cok kisa: %u\n", len);
		return -1;
	}

	if (memcmp(frame, broadcast, ETH_ALEN) != 0) {
		fprintf(stderr, "hedef MAC broadcast degil\n");
		return -1;
	}

	ethertype = ((uint16_t)frame[12] << 8) | frame[13];

	if (ethertype != DNHAT_TEST_ETHERTYPE) {
		fprintf(stderr,
			"beklenmeyen EtherType: 0x%04x\n",
			ethertype);
		return -1;
	}

	*seq = ((uint16_t)frame[14] << 8) | frame[15];

	for (i = DNHAT_HDR_LEN + DNHAT_SEQ_LEN; i < len; i++) {
		unsigned char expected;

		expected = (unsigned char)(i - DNHAT_HDR_LEN);

		if (frame[i] != expected) {
			fprintf(stderr,
				"bozulma: seq=%u offset=%u "
				"beklenen=0x%02x okunan=0x%02x\n",
				(unsigned int)*seq,
				i,
				expected,
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

static int do_send(const struct opts *o)
{
	unsigned char frame[DNHAT_MAX_FRAME];
	unsigned char mac[ETH_ALEN];
	struct sockaddr_ll dst;
	struct timespec t0;
	struct timespec t1;
	char mac_text[32];
	unsigned int sent = 0;
	unsigned int i;
	double secs;
	double mbps;
	int ifindex;
	int fd;

	fd = open_packet_socket(o->ifname, &ifindex, mac);
	if (fd < 0)
		return 1;

	memset(&dst, 0, sizeof(dst));

	dst.sll_family = AF_PACKET;
	dst.sll_protocol = htons(DNHAT_TEST_ETHERTYPE);
	dst.sll_ifindex = ifindex;
	dst.sll_halen = ETH_ALEN;

	memset(dst.sll_addr, 0xFF, ETH_ALEN);

	format_mac(mac, mac_text, sizeof(mac_text));

	printf("arayuz=%s ifindex=%d kaynak-mac=%s\n",
	       o->ifname,
	       ifindex,
	       mac_text);

	if (clock_gettime(CLOCK_MONOTONIC, &t0) < 0) {
		perror("clock_gettime");
		close(fd);
		return 1;
	}

	for (i = 0; i < o->count; i++) {
		ssize_t n;

		build_frame(frame,
			    o->size,
			    mac,
			    (uint16_t)i);

		if (i == 0 && o->verbose)
			dump_frame(frame, o->size);

		do {
			n = sendto(fd,
				   frame,
				   o->size,
				   0,
				   (struct sockaddr *)&dst,
				   sizeof(dst));
		} while (n < 0 && errno == EINTR);

		if (n < 0) {
			perror("sendto");
			break;
		}

		if ((unsigned int)n != o->size) {
			fprintf(stderr,
				"kisa gonderim: %zd/%u bayt\n",
				n,
				o->size);
			break;
		}

		sent++;
	}

	if (clock_gettime(CLOCK_MONOTONIC, &t1) < 0) {
		perror("clock_gettime");
		close(fd);
		return 1;
	}

	secs = elapsed_sec(&t0, &t1);

	mbps = secs > 0.0
		? ((double)sent *
		   (double)o->size *
		   8.0) /
		  (secs * 1000000.0)
		: 0.0;

	printf("%s uzerinden %u bayt x %u/%u cerceve gonderildi "
	       "(%.6f s, %.2f Mbps uygulama-seviyesi)\n",
	       o->ifname,
	       o->size,
	       sent,
	       o->count,
	       secs,
	       mbps);

	close(fd);

	return sent == o->count ? 0 : 1;
}

static void update_sequence_stats(struct listen_stats *stats,
				  uint16_t seq,
				  int *have_last,
				  uint16_t *last_seq)
{
	if (!*have_last) {
		*have_last = 1;
		*last_seq = seq;
		return;
	}

	if (seq == *last_seq) {
		stats->duplicates++;
		return;
	}

	/*
	 * Test sequence number'i 16 bitliktir ve 65535 sonrasinda
	 * sifira doner.
	 */
	if ((uint16_t)(*last_seq + 1U) == seq) {
		*last_seq = seq;
		return;
	}

	/*
	 * Küçük pozitif fark:
	 *
	 * last = 100
	 * seq  = 104
	 *
	 * 101, 102, 103 kayip kabul edilir.
	 */
	if ((uint16_t)(seq - *last_seq) < 0x8000U) {
		unsigned int gap;

		gap = (uint16_t)(seq - *last_seq) - 1U;
		stats->gaps += gap;
		*last_seq = seq;
		return;
	}

	/* Daha eski bir sequence geldiyse reordering olarak raporla. */
	stats->reordered++;
}

static int do_listen(const struct opts *o)
{
	unsigned char frame[DNHAT_MAX_FRAME + 64];
	unsigned char mac[ETH_ALEN];
	struct listen_stats stats = { 0 };
	struct pollfd pfd;
	char mac_text[32];
	uint16_t last_seq = 0;
	int have_last = 0;
	int dumped = 0;
	int ifindex;
	int fd;

	fd = open_packet_socket(o->ifname, &ifindex, mac);
	if (fd < 0)
		return 1;

	format_mac(mac, mac_text, sizeof(mac_text));

	printf("%s dinleniyor (ifindex=%d, MAC=%s), "
	       "%u test cercevesi bekleniyor...\n",
	       o->ifname,
	       ifindex,
	       mac_text,
	       o->count);

	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	while (stats.good + stats.bad < o->count) {
		uint16_t seq;
		ssize_t n;
		int pr;

		do {
			pr = poll(&pfd,
				  1,
				  (int)o->timeout_ms);
		} while (pr < 0 && errno == EINTR);

		if (pr < 0) {
			perror("poll");
			break;
		}

		if (pr == 0) {
			fprintf(stderr,
				"timeout: %u ms boyunca "
				"yeni test cercevesi gelmedi\n",
				o->timeout_ms);
			break;
		}

		if (!(pfd.revents & POLLIN)) {
			fprintf(stderr,
				"beklenmeyen poll olayi: 0x%x\n",
				pfd.revents);
			break;
		}

		do {
			n = recv(fd,
				 frame,
				 sizeof(frame),
				 0);
		} while (n < 0 && errno == EINTR);

		if (n < 0) {
			perror("recv");
			break;
		}

		if (verify_frame(frame,
				 (unsigned int)n,
				 o->size,
				 &seq) == 0) {

			if (o->verbose && !dumped) {
				printf("ilk gecerli cerceve: seq=%u",
				       (unsigned int)seq);

				dump_frame(frame,
					   (unsigned int)n);

				dumped = 1;
			}

			update_sequence_stats(&stats,
					      seq,
					      &have_last,
					      &last_seq);

			stats.good++;
		} else {
			stats.bad++;
		}
	}

	printf("alinan: %u hatasiz, %u bozuk, "
	       "%u tahmini kayip, %u tekrar, %u sirasi-bozuk\n",
	       stats.good,
	       stats.bad,
	       stats.gaps,
	       stats.duplicates,
	       stats.reordered);

	close(fd);

	if (stats.bad || stats.good != o->count)
		return 1;

	return 0;
}

int main(int argc, char **argv)
{
	struct opts o = {
		.ifname = "eth1",
		.size = DNHAT_MIN_FRAME,
		.count = 1,
		.timeout_ms = DNHAT_DEFAULT_TIMEOUT,
		.listen = 0,
		.verbose = 0,
	};

	int c;

	while ((c = getopt(argc,
			   argv,
			   "i:s:c:t:lvh")) != -1) {

		switch (c) {
		case 'i':
			o.ifname = optarg;
			break;

		case 's':
			if (parse_u32(optarg,
				      DNHAT_MIN_FRAME,
				      DNHAT_MAX_FRAME,
				      &o.size) < 0) {

				fprintf(stderr,
					"gecersiz cerceve boyutu: %s\n",
					optarg);

				return 1;
			}
			break;

		case 'c':
			if (parse_u32(optarg,
				      1U,
				      65535U,
				      &o.count) < 0) {

				fprintf(stderr,
					"gecersiz cerceve adedi: %s\n",
					optarg);

				return 1;
			}
			break;

		case 't':
			if (parse_u32(optarg,
				      1U,
				      3600000U,
				      &o.timeout_ms) < 0) {

				fprintf(stderr,
					"gecersiz timeout: %s\n",
					optarg);

				return 1;
			}
			break;

		case 'l':
			o.listen = 1;
			break;

		case 'v':
			o.verbose = 1;
			break;

		case 'h':
			usage(argv[0]);
			return 0;

		default:
			usage(argv[0]);
			return 1;
		}
	}

	return o.listen
		? do_listen(&o)
		: do_send(&o);
}
