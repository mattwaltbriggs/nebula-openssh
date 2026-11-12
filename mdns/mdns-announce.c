/*
 * mdns-announce: announce a host name and SSH/SFTP services over mDNS (Bonjour).
 *
 * Publishes, for each service type T in {_ssh._tcp, _sftp-ssh._tcp, _sftp._tcp}:
 *   T.local                          PTR   <label>.T.local
 *   <label>.T.local                  SRV   <port> host.local
 *   <label>.T.local                  TXT   (empty)
 *   _services._dns-sd._udp.local     PTR   T.local
 * and for the host:
 *   host.local                       A     <ipv4>
 *
 * Usage: mdns-announce [-n host.local] [-s label] [-p port] [-a ipv4] [-i iface_ipv4]
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
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"
#define HOST_TTL 120
#define SVC_TTL 4500
#define PKT_MAX 1024

#define T_A 1
#define T_PTR 12
#define T_TXT 16
#define T_SRV 33
#define T_ANY 255

#define NSVC 3
static const char *service_types[NSVC] = {
	"_ssh._tcp.local",
	"_sftp-ssh._tcp.local",
	"_sftp._tcp.local",
};
static const char DNS_SD_BROWSE[] = "_services._dns-sd._udp.local";

struct cfg {
	char host[256];                 /* host.local */
	char label[64];                 /* instance label, e.g. "Nebula" */
	char inst[NSVC][320];           /* label.<type> for each service type */
	uint32_t ip;                    /* network order */
	uint16_t port;
};

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

/* Reads a (possibly compressed) question name into out as dotted text.
 * Returns the offset just past the name, or -1 on malformed input. */
static int parse_qname(const uint8_t *pkt, int n, int off, char *out, int outlen)
{
	int len = 0, hops = 0, end = -1;
	out[0] = 0;
	for (;;) {
		int l;
		if (off >= n)
			return -1;
		l = pkt[off];
		if (l == 0) {
			if (end < 0)
				end = off + 1;
			break;
		}
		if ((l & 0xC0) == 0xC0) {
			if (off + 1 >= n || ++hops > 16)
				return -1;
			if (end < 0)
				end = off + 2;
			off = ((l & 0x3F) << 8) | pkt[off + 1];
			continue;
		}
		if (l > 63 || off + 1 + l > n)
			return -1;
		if (len) {
			if (len + 1 >= outlen)
				return -1;
			out[len++] = '.';
		}
		if (len + l >= outlen)
			return -1;
		memcpy(out + len, pkt + off + 1, l);
		len += l;
		out[len] = 0;
		off += l + 1;
	}
	return end;
}

static int add_rr(uint8_t *buf, int n, const char *name, uint16_t type,
		  uint16_t cls, uint32_t ttl, const uint8_t *rd, int rdlen)
{
	n += encode_name(buf + n, name);
	buf[n++] = type >> 8;   buf[n++] = type & 0xff;
	buf[n++] = cls >> 8;    buf[n++] = cls & 0xff;
	buf[n++] = (ttl >> 24) & 0xff; buf[n++] = (ttl >> 16) & 0xff;
	buf[n++] = (ttl >> 8) & 0xff;  buf[n++] = ttl & 0xff;
	buf[n++] = rdlen >> 8;  buf[n++] = rdlen & 0xff;
	memcpy(buf + n, rd, rdlen);
	return n + rdlen;
}

/* Builds one response with every record. goodbye=1 sends TTL 0 for all. */
static int build_announce(uint8_t *buf, const struct cfg *c, int goodbye)
{
	uint8_t rd[300];
	int n = 12, rdn, count = 0, s;
	uint32_t httl = goodbye ? 0 : HOST_TTL;
	uint32_t sttl = goodbye ? 0 : SVC_TTL;

	memset(buf, 0, 12);
	buf[2] = 0x84;                       /* response, authoritative */

	memcpy(rd, &c->ip, 4);
	n = add_rr(buf, n, c->host, T_A, 0x8001, httl, rd, 4);
	count++;

	for (s = 0; s < NSVC; s++) {
		const char *svc = service_types[s];
		const char *inst = c->inst[s];

		rdn = encode_name(rd, inst);
		n = add_rr(buf, n, svc, T_PTR, 0x0001, sttl, rd, rdn);
		count++;

		memset(rd, 0, 4);
		rd[4] = c->port >> 8;
		rd[5] = c->port & 0xff;
		rdn = 6 + encode_name(rd + 6, c->host);
		n = add_rr(buf, n, inst, T_SRV, 0x8001, httl, rd, rdn);
		count++;

		rd[0] = 0;
		n = add_rr(buf, n, inst, T_TXT, 0x8001, sttl, rd, 1);
		count++;

		rdn = encode_name(rd, svc);
		n = add_rr(buf, n, DNS_SD_BROWSE, T_PTR, 0x0001, sttl, rd, rdn);
		count++;
	}

	buf[6] = count >> 8;
	buf[7] = count & 0xff;
	return n;
}

static void send_announce(int fd, const struct cfg *c, int goodbye)
{
	uint8_t buf[PKT_MAX];
	struct sockaddr_in dst = { 0 };
	int n = build_announce(buf, c, goodbye);
	dst.sin_family = AF_INET;
	dst.sin_port = htons(MDNS_PORT);
	inet_pton(AF_INET, MDNS_GROUP, &dst.sin_addr);
	sendto(fd, buf, n, 0, (struct sockaddr *)&dst, sizeof(dst));
}

static int question_matches(const char *qname, const struct cfg *c)
{
	int s;
	if (strcasecmp(qname, c->host) == 0)
		return 1;
	if (strcasecmp(qname, DNS_SD_BROWSE) == 0)
		return 1;
	for (s = 0; s < NSVC; s++) {
		if (strcasecmp(qname, service_types[s]) == 0 ||
		    strcasecmp(qname, c->inst[s]) == 0)
			return 1;
	}
	return 0;
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
	struct cfg c;
	const char *label = "Nebula";
	const char *host = "nebula.local";
	uint32_t iface = 0;
	int opt, fd, one = 1, port = 22, s;
	struct sockaddr_in sa = { 0 };
	struct ip_mreq mreq;
	uint8_t buf[PKT_MAX];

	memset(&c, 0, sizeof(c));
	while ((opt = getopt(argc, argv, "n:s:p:a:i:")) != -1) {
		switch (opt) {
		case 'n': host = optarg; break;
		case 's': label = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 'a': if (inet_pton(AF_INET, optarg, &c.ip) != 1) return 1; break;
		case 'i': if (inet_pton(AF_INET, optarg, &iface) != 1) return 1; break;
		default:
			fprintf(stderr, "usage: %s [-n host.local] [-s label] [-p port] [-a ipv4] [-i iface_ipv4]\n", argv[0]);
			return 1;
		}
	}
	if (port <= 0 || port > 65535) {
		fprintf(stderr, "mdns-announce: bad port\n");
		return 1;
	}
	if (!iface)
		iface = detect_ipv4();
	if (!c.ip)
		c.ip = iface;
	if (!c.ip) {
		fprintf(stderr, "mdns-announce: no IPv4 address found\n");
		return 1;
	}

	snprintf(c.host, sizeof(c.host), "%s", host);
	snprintf(c.label, sizeof(c.label), "%s", label);
	for (s = 0; s < NSVC; s++)
		snprintf(c.inst[s], sizeof(c.inst[s]), "%s.%s", label, service_types[s]);
	c.port = (uint16_t)port;

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
	{
		int ttl255 = 255;
		setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl255, sizeof(ttl255));
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	printf("mdns-announce: %s, %s on port %d, address %s\n",
	       c.host, c.label, port, inet_ntoa(*(struct in_addr *)&c.ip));
	fflush(stdout);

	/* Startup announcements: 0s, 1s, 2s, 4s. */
	{
		int delays[] = { 0, 1, 2, 4 };
		for (int i = 0; i < 4 && running; i++) {
			if (delays[i])
				sleep(delays[i]);
			send_announce(fd, &c, 0);
		}
	}

	while (running) {
		struct sockaddr_in from;
		socklen_t flen = sizeof(from);
		ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
		int qd, off, q;
		if (n < 12 || (buf[2] & 0x80))  /* short, or a response: ignore */
			continue;
		qd = (buf[4] << 8) | buf[5];
		off = 12;
		for (q = 0; q < qd; q++) {
			char qname[256];
			int qtype;
			off = parse_qname(buf, (int)n, off, qname, sizeof(qname));
			if (off < 0 || off + 4 > n)
				break;
			qtype = (buf[off] << 8) | buf[off + 1];
			off += 4;
			if ((qtype == T_A || qtype == T_PTR || qtype == T_SRV ||
			     qtype == T_TXT || qtype == T_ANY) &&
			    question_matches(qname, &c)) {
				send_announce(fd, &c, 0);
				break;
			}
		}
	}

	send_announce(fd, &c, 1);
	close(fd);
	return 0;
}
