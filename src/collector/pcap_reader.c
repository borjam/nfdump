/*
 *  Copyright (c) 2009-2022, Peter Haag
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *   * Neither the name of the author nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <config.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#define __FAVOR_BSD 1
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <pcap.h>
#include <stdint.h>
#include <string.h>

#include "pcap_reader.h"

#define NUMLISTS 5
#define HASHSIZE 8192
#define HASH_KEYLENGTH 4

enum { SRC_IP, DST_IP, UDP_PORT, TCP_PORT, ICMP_TYPE };

static pcap_t *pcap_handle;
static uint32_t udp_count, arp_count, unknow_count;

/*
 * Function prototypes
 */

static pcap_t *setup_pcap(char *fname, char *filter, char *errbuf);

static ssize_t decode_packet(struct pcap_pkthdr *hdr, u_char *pkt, void *buffer, struct sockaddr *sock);

/*
 * function definitions
 */

static pcap_t *setup_pcap(char *fname, char *filter, char *errbuf) {
    struct bpf_program filter_code;

    bpf_u_int32 netmask;

    /*
     *  Open the packet capturing file
     */
    pcap_handle = pcap_open_offline(fname, errbuf);
    if (!pcap_handle) return NULL;

    netmask = 0;
    /* apply filters if any are requested */
    if (filter) {
        if (pcap_compile(pcap_handle, &filter_code, filter, 1, netmask) == -1) {
            /* pcap does not fill in the error code on pcap_compile */
            snprintf(errbuf, PCAP_ERRBUF_SIZE, "pcap_compile() failed: %s\n", pcap_geterr(pcap_handle));
            pcap_close(pcap_handle);
            return NULL;
        }
        if (pcap_setfilter(pcap_handle, &filter_code) == -1) {
            /* pcap does not fill in the error code on pcap_compile */
            snprintf(errbuf, PCAP_ERRBUF_SIZE, "pcap_setfilter() failed: %s\n", pcap_geterr(pcap_handle));
            pcap_close(pcap_handle);
            return NULL;
        }
    }

    /*
     *  We need to make sure this is Ethernet.  The DLTEN10MB specifies
     *  standard 10MB and higher Ethernet.
     */
    if (pcap_datalink(pcap_handle) != DLT_EN10MB) {
        snprintf(errbuf, PCAP_ERRBUF_SIZE - 1, "Snooping not on an ethernet.\n");
        pcap_close(pcap_handle);
        return NULL;
    }
    return pcap_handle;

} /* setup_pcap */

static ssize_t decode_packet(struct pcap_pkthdr *hdr, u_char *pkt, void *buffer, struct sockaddr *sock) {
    struct ip *ip;
    struct udphdr *udp;
    u_char *payload;
    int len;
    u_int hlen, version;
    u_int packet_len;
    struct sockaddr_in *in_sock = (struct sockaddr_in *)sock;

    u_int length = hdr->len;
    if (hdr->len > hdr->caplen) {
        printf("Short packet - missing: %u bytes\n", hdr->len - hdr->caplen);
        length = hdr->caplen;
        return 0;
    }
    ip = NULL;

/*
 *  Figure out which layer 2 protocol the frame belongs to and call
 *  the corresponding decoding module.  The protocol field of an
 *  Ethernet II header is the 13th + 14th byte.  This is an endian
 *  independent way of extracting a big endian short from memory.  We
 *  extract the first byte and make it the big byte and then extract
 *  the next byte and make it the small byte.
 */
REDO_LINK:
    switch (pkt[12] << 0x08 | pkt[13]) {
        case 0x0800:
            /* IPv4 */
            length -= 14;
            ip = (struct ip *)&pkt[14];
            in_sock->sin_family = AF_INET;
            in_sock->sin_addr = ip->ip_src;
#ifdef HAVE_STRUCT_SOCKADDR_SA_LEN
            in_sock->sin_len = sizeof(struct sockaddr_in);
#endif
            break;
        case 0x0806:
            /* ARP */
            arp_count++;
            break;
        case 0x8100:   // VLAN
            pkt += 4;  // strip off vlan
            printf("Skip VLAN labels\n");
            goto REDO_LINK;
            break;
        default:
            /* We're not bothering with 802.3 or anything else */
            printf("PCAP unknown linktype %u%u\n", pkt[12], pkt[13]);
            unknow_count++;
            break;
    }

    /* for the moment we handle only IPv4 */
    if (!ip || ip->ip_v != 4) return 0;

    len = ntohs(ip->ip_len);
    hlen = ip->ip_hl;   /* header length */
    version = ip->ip_v; /* ip version */

    /* check version */
    if (version != 4) {
        fprintf(stdout, "Unknown version %d\n", version);
        return 0;
    }

    /* check header length */
    if (hlen < 5) {
        fprintf(stdout, "bad-hlen %d \n", hlen);
        return 0;
    }

    /* see if we have as much packet as we should */
    if (length < len) {
        printf("\ntruncated IP - %d bytes missing\n", len - length);
        return 0;
    }

    switch (ip->ip_p) {
        case IPPROTO_UDP:
            udp_count++;
            udp = (struct udphdr *)((void *)ip + (ip->ip_hl << 0x02));
            packet_len = ntohs(udp->uh_ulen) - 8;
            payload = (u_char *)((void *)udp + sizeof(struct udphdr));

            memcpy(buffer, payload, packet_len);
            in_sock->sin_port = udp->uh_sport;
            return packet_len;
            // unreached
            break;
        default:
            /* no default */
            break;
    }

    return 0;

} /* decode_packet */

void setup_packethandler(char *fname, char *filter) {
    char errbuf[PCAP_ERRBUF_SIZE];

    pcap_handle = setup_pcap(fname, filter, errbuf);
    if (!pcap_handle) {
        fprintf(stderr, "Can't init pcap: %s\n", errbuf);
        exit(255);
    }

} /* End of setup_packethandler */

ssize_t NextPacket(int fill1, void *buffer, size_t buffer_size, int fill2, struct sockaddr *sock, socklen_t *size) {
    // ssize_t NextPacket(void *buffer, size_t buffer_size) {
    struct pcap_pkthdr *header;
    u_char *pkt_data;
    int i;

    i = pcap_next_ex(pcap_handle, &header, (const u_char **)&pkt_data);
    if (i != 1) return -2;

    *size = sizeof(struct sockaddr_in);
    return decode_packet(header, pkt_data, buffer, sock);
}

/*
int main( int argc, char **argv ) {

        setup_packethandler("sflow.pack", NULL);
        return 0;
}
*/
