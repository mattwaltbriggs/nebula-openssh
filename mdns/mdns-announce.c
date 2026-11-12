/*
 * mdns-announce: announce a single A record over mDNS (Bonjour), e.g.
 * nebula.local -> the pad's IPv4 address. Answers queries for that name
 * and sends unsolicited announcements at startup.
 *
 * Usage: mdns-announce [-n name.local] [-a ipv4] [-i iface_ipv4]
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"
#define TTL 120

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

static int encode_name(uint8_t *buf, const char *name)
{
	int len = 0;
	const char *p = name;
	while (*p) {
		const char *dot = strchr(p, '.');
		int l = dot ? (int)(dot - p) : (int)strlen(p);
		if (l == 0)
			break;
		buf[len++] = (uint8_t)l;
		memcpy(buf + len, p, l);
		len += l;
		p = dot ? dot + 1 : p + l;
	}
	buf[len++] = 0;
	return len;
}

static int name_matches(const uint8_t *pkt, int off, int end, const char *name)
{
	const char *p = name;
	while (off < end) {
		int l = pkt[off++];
		if (l == 0)
			return *p == 0;
		if ((l & 0xC0) || off + l > end)
			return 0;
		const char *dot = strchr(p, '.');
		int nl = dot ? (int)(dot - p) : (int)strlen(p);
		if (nl != l || strncasecmp((const char *)pkt + off, p, l) != 0)
			return 0;
		off += l;
		p = dot ? dot + 1 : p + nl;
	}
	return 0;
}

static int build_response(uint8_t *buf, const char *name, uint32_t ip, int ttl)
{
	int n = 0;
	buf[n++] = 0; buf[n++] = 0;          /* id */
	buf[n++] = 0x84; buf[n++] = 0x00;    /* flags: response, authoritative */
	buf[n++] = 0; buf[n++] = 0;          /* qdcount */
	buf[n++] = 0; buf[n++] = 1;          /* ancount */
	buf[n++] = 0; buf[n++] = 0;          /* nscount */
	buf[n++] = 0; buf[n++] = 0;          /* arcount */
	n += encode_name(buf + n, name);
	buf[n++] = 0; buf[n++] = 1;          /* type A */
	buf[n++] = 0x80; buf[n++] = 1;       /* class IN, cache-flush */
	buf[n++] = (ttl >> 24) & 0xff; buf[n++] = (ttl >> 16) & 0xff;
	buf[n++] = (ttl >> 8) & 0xff;  buf[n++] = ttl & 0xff;
	buf[n++] = 0; buf[n++] = 4;          /* rdlength */
	memcpy(buf + n, &ip, 4);             /* ip is already network order */
	n += 4;
	return n;
}

static void send_response(int fd, const char *name, uint32_t ip, int ttl)
{
	uint8_t buf[512];
	struct sockaddr_in dst = { 0 };
	int n = build_response(buf, name, ip, ttl);
	dst.sin_family = AF_INET;
	dst.sin_port = htons(MDNS_PORT);
	inet_pton(AF_INET, MDNS_GROUP, &dst.sin_addr);
	sendto(fd, buf, n, 0, (struct sockaddr *)&dst, sizeof(dst));
}

static uint32_t detect_ipv4(void)
{
	struct ifaddrs *ifa, *p;
	uint32_t found = 0;
	if (getifaddrs(&ifa) != 0)
		return 0;
	for (p = ifa; p; p = p->ifa_next) {
		struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
		if (!sin || sin->sin_family != AF_INET)
			continue;
		if (p->ifa_flags & IFF_LOOPBACK)
			continue;
		if (!(p->ifa_flags & IFF_UP))
			continue;
		found = sin->sin_addr.s_addr;
		break;
	}
	freeifaddrs(ifa);
	return found;
}

int main(int argc, char **argv)
{
	const char *name = "nebula.local";
	uint32_t ip = 0, iface = 0;
	int opt, fd, one = 1;
	struct sockaddr_in sa = { 0 };
	struct ip_mreq mreq;
	uint8_t buf[512];

	while ((opt = getopt(argc, argv, "n:a:i:")) != -1) {
		switch (opt) {
		case 'n': name = optarg; break;
		case 'a': if (inet_pton(AF_INET, optarg, &ip) != 1) return 1; break;
		case 'i': if (inet_pton(AF_INET, optarg, &iface) != 1) return 1; break;
		default:
			fprintf(stderr, "usage: %s [-n name.local] [-a ipv4] [-i iface_ipv4]\n", argv[0]);
			return 1;
		}
	}
	if (!iface)
		iface = detect_ipv4();
	if (!ip)
		ip = iface;
	if (!ip) {
		fprintf(stderr, "mdns-announce: no IPv4 address found\n");
		return 1;
	}

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

	sa.sin_family = AF_INET;
	sa.sin_port = htons(MDNS_PORT);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("bind");
		return 1;
	}

	inet_pton(AF_INET, MDNS_GROUP, &mreq.imr_multiaddr);
	mreq.imr_interface.s_addr = iface;
	if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
		perror("IP_ADD_MEMBERSHIP");
		return 1;
	}
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));
	int ttl255 = 255;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl255, sizeof(ttl255));

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	printf("mdns-announce: %s -> %s\n", name, inet_ntoa(*(struct in_addr *)&ip));
	fflush(stdout);

	/* Startup announcements: 1s, 2s, 4s apart. */
	{
		int delays[] = { 0, 1, 2, 4 };
		for (int i = 0; i < 4 && running; i++) {
			if (delays[i])
				sleep(delays[i]);
			send_response(fd, name, ip, TTL);
		}
	}

	while (running) {
		struct sockaddr_in from;
		socklen_t flen = sizeof(from);
		ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
		if (n < 12)
			continue;
		if (buf[2] & 0x80)          /* response: ignore */
			continue;
		int qd = (buf[4] << 8) | buf[5];
		int off = 12;
		for (int q = 0; q < qd && off < n; q++) {
			int start = off;
			while (off < n && buf[off] != 0) {
				if (buf[off] & 0xC0) { off += 2; goto next; }
				off += buf[off] + 1;
			}
			off++;               /* root label */
		next:
			if (off + 4 > n)
				break;
			int qtype = (buf[off] << 8) | buf[off + 1];
			if ((qtype == 1 || qtype == 255) && name_matches(buf, start, n, name))
				send_response(fd, name, ip, TTL);
			off += 4;
		}
	}

	/* Goodbye: TTL 0 so clients drop the record on shutdown. */
	send_response(fd, name, ip, 0);
	close(fd);
	return 0;
}
