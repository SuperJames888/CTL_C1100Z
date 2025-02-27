/*
 * Copyright (C) 2012, Broadcom Corporation. All Rights Reserved.
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Fundamental constants relating to IP Protocol
 *
 * $Id: bcmip.h 329791 2012-04-26 22:36:58Z $
 */

#ifndef _bcmip_h_
#define _bcmip_h_

#ifndef _TYPEDEFS_H_
#include <typedefs.h>
#endif

/* This marks the start of a packed structure section. */
#include <packed_section_start.h>


/* IPV4 and IPV6 common */
#define IP_VER_OFFSET		0x0	/* offset to version field */
#define IP_VER_MASK		0xf0	/* version mask */
#define IP_VER_SHIFT		4	/* version shift */
#define IP_VER_4		4	/* version number for IPV4 */
#define IP_VER_6		6	/* version number for IPV6 */

#define IP_VER(ip_body) \
	((((uint8 *)(ip_body))[IP_VER_OFFSET] & IP_VER_MASK) >> IP_VER_SHIFT)

#define IP_PROT_ICMP		0x1	/* ICMP protocol */
#define IP_PROT_IGMP		0x2	/* IGMP protocol */
#define IP_PROT_TCP		0x6	/* TCP protocol */
#define IP_PROT_UDP		0x11	/* UDP protocol type */
#define IP_PROT_ICMP6		0x3a	/* ICMPv6 protocol type */

/* IPV4 field offsets */
#define IPV4_VER_HL_OFFSET      0       /* version and ihl byte offset */
#define IPV4_TOS_OFFSET         1       /* type of service offset */
#define IPV4_PKTLEN_OFFSET      2       /* packet length offset */
#define IPV4_PKTFLAG_OFFSET     6       /* more-frag,dont-frag flag offset */
#define IPV4_PROT_OFFSET        9       /* protocol type offset */
#define IPV4_CHKSUM_OFFSET      10      /* IP header checksum offset */
#define IPV4_SRC_IP_OFFSET      12      /* src IP addr offset */
#define IPV4_DEST_IP_OFFSET     16      /* dest IP addr offset */
#define IPV4_OPTIONS_OFFSET     20      /* IP options offset */
#define IPV4_MIN_HEADER_LEN     20      /* Minimum size for an IP header (no options) */

/* IPV4 field decodes */
#define IPV4_VER_MASK		0xf0	/* IPV4 version mask */
#define IPV4_VER_SHIFT		4	/* IPV4 version shift */

#define IPV4_HLEN_MASK		0x0f	/* IPV4 header length mask */
#define IPV4_HLEN(ipv4_body)	(4 * (((uint8 *)(ipv4_body))[IPV4_VER_HL_OFFSET] & IPV4_HLEN_MASK))

#define IPV4_ADDR_LEN		4	/* IPV4 address length */

#define IPV4_ADDR_NULL(a)	((((uint8 *)(a))[0] | ((uint8 *)(a))[1] | \
				  ((uint8 *)(a))[2] | ((uint8 *)(a))[3]) == 0)

#define IPV4_ADDR_BCAST(a)	((((uint8 *)(a))[0] & ((uint8 *)(a))[1] & \
				  ((uint8 *)(a))[2] & ((uint8 *)(a))[3]) == 0xff)

#define	IPV4_TOS_DSCP_MASK	0xfc	/* DiffServ codepoint mask */
#define	IPV4_TOS_DSCP_SHIFT	2	/* DiffServ codepoint shift */

#define	IPV4_TOS(ipv4_body)	(((uint8 *)(ipv4_body))[IPV4_TOS_OFFSET])

#define	IPV4_TOS_PREC_MASK	0xe0	/* Historical precedence mask */
#define	IPV4_TOS_PREC_SHIFT	5	/* Historical precedence shift */

#define IPV4_TOS_LOWDELAY	0x10	/* Lowest delay requested */
#define IPV4_TOS_THROUGHPUT	0x8	/* Best throughput requested */
#define IPV4_TOS_RELIABILITY	0x4	/* Most reliable delivery requested */

#define IPV4_PROT(ipv4_body)	(((uint8 *)(ipv4_body))[IPV4_PROT_OFFSET])

#define IPV4_FRAG_RESV		0x8000	/* Reserved */
#define IPV4_FRAG_DONT		0x4000	/* Don't fragment */
#define IPV4_FRAG_MORE		0x2000	/* More fragments */
#define IPV4_FRAG_OFFSET_MASK	0x1fff	/* Fragment offset */

#define IPV4_ADDR_STR_LEN	16	/* Max IP address length in string format */

/* IPV4 packet formats */
BWL_PRE_PACKED_STRUCT struct ipv4_addr {
	uint8	addr[IPV4_ADDR_LEN];
} BWL_POST_PACKED_STRUCT;

BWL_PRE_PACKED_STRUCT struct ipv4_hdr {
	uint8	version_ihl;		/* Version and Internet Header Length */
	uint8	tos;			/* Type Of Service */
	uint16	tot_len;		/* Number of bytes in packet (max 65535) */
	uint16	id;
	uint16	frag;			/* 3 flag bits and fragment offset */
	uint8	ttl;			/* Time To Live */
	uint8	prot;			/* Protocol */
	uint16	hdr_chksum;		/* IP header checksum */
	uint8	src_ip[IPV4_ADDR_LEN];	/* Source IP Address */
	uint8	dst_ip[IPV4_ADDR_LEN];	/* Destination IP Address */
} BWL_POST_PACKED_STRUCT;

/* IPV6 field offsets */
#define IPV6_PAYLOAD_LEN_OFFSET	4	/* payload length offset */
#define IPV6_NEXT_HDR_OFFSET	6	/* next header/protocol offset */
#define IPV6_HOP_LIMIT_OFFSET	7	/* hop limit offset */
#define IPV6_SRC_IP_OFFSET	8	/* src IP addr offset */
#define IPV6_DEST_IP_OFFSET	24	/* dst IP addr offset */

/* IPV6 field decodes */
#define IPV6_TRAFFIC_CLASS(ipv6_body) \
	(((((uint8 *)(ipv6_body))[0] & 0x0f) << 4) | \
	 ((((uint8 *)(ipv6_body))[1] & 0xf0) >> 4))

#define IPV6_FLOW_LABEL(ipv6_body) \
	(((((uint8 *)(ipv6_body))[1] & 0x0f) << 16) | \
	 (((uint8 *)(ipv6_body))[2] << 8) | \
	 (((uint8 *)(ipv6_body))[3]))

#define IPV6_PAYLOAD_LEN(ipv6_body) \
	((((uint8 *)(ipv6_body))[IPV6_PAYLOAD_LEN_OFFSET + 0] << 8) | \
	 ((uint8 *)(ipv6_body))[IPV6_PAYLOAD_LEN_OFFSET + 1])

#define IPV6_NEXT_HDR(ipv6_body) \
	(((uint8 *)(ipv6_body))[IPV6_NEXT_HDR_OFFSET])

#define IPV6_PROT(ipv6_body)	IPV6_NEXT_HDR(ipv6_body)

#define IPV6_ADDR_LEN		16	/* IPV6 address length */

/* IPV4 TOS or IPV6 Traffic Classifier or 0 */
#define IP_TOS46(ip_body) \
	(IP_VER(ip_body) == IP_VER_4 ? IPV4_TOS(ip_body) : \
	 IP_VER(ip_body) == IP_VER_6 ? IPV6_TRAFFIC_CLASS(ip_body) : 0)

/* IPV6 extension headers (options) */
#define IPV6_EXTHDR_HOP		0
#define IPV6_EXTHDR_ROUTING	43
#define IPV6_EXTHDR_FRAGMENT	44
#define IPV6_EXTHDR_AUTH	51
#define IPV6_EXTHDR_NONE	59
#define IPV6_EXTHDR_DEST	60

#define IPV6_EXTHDR(prot)	(((prot) == IPV6_EXTHDR_HOP) || \
	                         ((prot) == IPV6_EXTHDR_ROUTING) || \
	                         ((prot) == IPV6_EXTHDR_FRAGMENT) || \
	                         ((prot) == IPV6_EXTHDR_AUTH) || \
	                         ((prot) == IPV6_EXTHDR_NONE) || \
	                         ((prot) == IPV6_EXTHDR_DEST))

#define IPV6_MIN_HLEN 		40

#define IPV6_EXTHDR_LEN(eh)	((((struct ipv6_exthdr *)(eh))->hdrlen + 1) << 3)

BWL_PRE_PACKED_STRUCT struct ipv6_exthdr {
	uint8	nexthdr;
	uint8	hdrlen;
} BWL_POST_PACKED_STRUCT;

BWL_PRE_PACKED_STRUCT struct ipv6_exthdr_frag {
	uint8	nexthdr;
	uint8	rsvd;
	uint16	frag_off;
	uint32	ident;
} BWL_POST_PACKED_STRUCT;

static INLINE int32
ipv6_exthdr_len(uint8 *h, uint8 *proto)
{
	uint16 len = 0, hlen;
	struct ipv6_exthdr *eh = (struct ipv6_exthdr *)h;

	while (IPV6_EXTHDR(eh->nexthdr)) {
		if (eh->nexthdr == IPV6_EXTHDR_NONE)
			return -1;
		else if (eh->nexthdr == IPV6_EXTHDR_FRAGMENT)
			hlen = 8;
		else if (eh->nexthdr == IPV6_EXTHDR_AUTH)
			hlen = (eh->hdrlen + 2) << 2;
		else
			hlen = IPV6_EXTHDR_LEN(eh);

		len += hlen;
		eh = (struct ipv6_exthdr *)(h + len);
	}

	*proto = eh->nexthdr;
	return len;
}

#define IPV4_ISMULTI(a) (((a) & 0xf0000000) == 0xe0000000)

/* This marks the end of a packed structure section. */
#include <packed_section_end.h>



#define BCM_IN6_ARE_ADDR_EQUAL(a,b)                                       \
       ((((__const uint32_t *) (a))[0] == ((__const uint32_t *) (b))[0])  \
         && (((__const uint32_t *) (a))[1] == ((__const uint32_t *) (b))[1])  \
         && (((__const uint32_t *) (a))[2] == ((__const uint32_t *) (b))[2])  \
         && (((__const uint32_t *) (a))[3] == ((__const uint32_t *) (b))[3])) 

#define BCM_IN6_ASSIGN_ADDR(a,b)                                  \
    do {                                                          \
        ((uint32_t *) (a))[0] = ((__const uint32_t *) (b))[0];    \
        ((uint32_t *) (a))[1] = ((__const uint32_t *) (b))[1];    \
        ((uint32_t *) (a))[2] = ((__const uint32_t *) (b))[2];    \
        ((uint32_t *) (a))[3] = ((__const uint32_t *) (b))[3];    \
    } while(0)

#define IPV6_ADDR_MULTICAST    	0x0002U

#define BCM_IN6_IS_ADDR_MULTICAST(a) (((__const uint8_t *) (a))[0] == 0xff)
#define BCM_IN6_MULTICAST(x)   (BCM_IN6_IS_ADDR_MULTICAST(x))
#define BCM_IN6_IS_ADDR_MC_NODELOCAL(a) \
        (BCM_IN6_IS_ADDR_MULTICAST(a)                                         \
         && ((((__const uint8_t *) (a))[1] & 0xf) == 0x1))

#define BCM_IN6_IS_ADDR_MC_LINKLOCAL(a) \
        (BCM_IN6_IS_ADDR_MULTICAST(a)                                         \
         && ((((__const uint8_t *) (a))[1] & 0xf) == 0x2))

#define BCM_IN6_IS_ADDR_MC_SITELOCAL(a) \
        (BCM_IN6_IS_ADDR_MULTICAST(a)                                         \
         && ((((__const uint8_t *) (a))[1] & 0xf) == 0x5))

#define BCM_IN6_IS_ADDR_MC_ORGLOCAL(a) \
        (BCM_IN6_IS_ADDR_MULTICAST(a)                                         \
         && ((((__const uint8_t *) (a))[1] & 0xf) == 0x8))

#define BCM_IN6_IS_ADDR_MC_GLOBAL(a) \
        (BCM_IN6_IS_ADDR_MULTICAST(a) \
         && ((((__const uint8_t *) (a))[1] & 0xf) == 0xe))

#define BCM_IN6_IS_ADDR_MC_SCOPE0(a) \
        (BCM_IN6_IS_ADDR_MULTICAST(a)                                         \
         && ((((__const uint8_t *) (a))[1] & 0xf) == 0x0))


struct inv6_addr {
         union {
                 uint8           u6_addr8[16];
                 uint16          u6_addr16[8];
                 uint32          u6_addr32[4];
         } in6_u;
 #define s6_addr                 in6_u.u6_addr8
 #define s6_addr16               in6_u.u6_addr16
 #define s6_addr32               in6_u.u6_addr32
};

#define ipv6_is_same(a,b) (!(\
			(((a).s6_addr32[0])^((b).s6_addr32[0])) ||\
			(((a).s6_addr32[1])^((b).s6_addr32[1])) ||\
			(((a).s6_addr32[2])^((b).s6_addr32[2])) ||\
			(((a).s6_addr32[3])^((b).s6_addr32[3]))))

struct ipv6_hdr {
         uint8                    version:4,
                                 priority:4;
         uint8                    flow_lbl[3];
 
         uint16                  payload_len;
         uint8                    nexthdr;
         uint8                    hop_limit;
 
         struct  inv6_addr        saddr;
         struct  inv6_addr        daddr;
 }; 
#endif	/* _bcmip_h_ */
