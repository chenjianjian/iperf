/*
 * iperf, Copyright (c) 2014-2020, The Regents of the University of
 * California, through Lawrence Berkeley National Laboratory (subject
 * to receipt of any required approvals from the U.S. Dept. of
 * Energy).  All rights reserved.
 *
 * If you have questions about your rights to use or distribute this
 * software, please contact Berkeley Lab's Technology Transfer
 * Department at TTD@lbl.gov.
 *
 * NOTICE.  This software is owned by the U.S. Department of Energy.
 * As such, the U.S. Government has been granted for itself and others
 * acting on its behalf a paid-up, nonexclusive, irrevocable,
 * worldwide license in the Software to reproduce, prepare derivative
 * works, and perform publicly and display publicly.  Beginning five
 * (5) years after the date permission to assert copyright is obtained
 * from the U.S. Department of Energy, and subject to any subsequent
 * five (5) year renewals, the U.S. Government is granted for itself
 * and others acting on its behalf a paid-up, nonexclusive,
 * irrevocable, worldwide license in the Software to reproduce,
 * prepare derivative works, distribute copies to the public, perform
 * publicly and display publicly, and to permit others to do so.
 *
 * This code is distributed under a BSD style license, see the LICENSE
 * file for complete information.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#include <sys/time.h>
#include <sys/select.h>

#include "iperf.h"
#include "iperf_api.h"
#include "iperf_util.h"
#include "iperf_rutp.h"
#include "timer.h"
#include "net.h"
#include "cjson.h"
#include "portable_endian.h"

#if defined(HAVE_INTTYPES_H)
# include <inttypes.h>
#else
# ifndef PRIu64
#  if sizeof(long) == 8
#   define PRIu64		"lu"
#  else
#   define PRIu64		"llu"
#  endif
# endif
#endif

#define IPERF_RUTP_MIN_LEN 1000

static char 
*iperf_rutp_make_packet(uint64_t connection_id, char *buf, int size, struct sockaddr *server_addr)
{
    char *ptr;
    uint8_t *flags, *type;
    uint64_t *cid;
    uint16_t *port;
    struct sockaddr_in *sin;
    struct sockaddr_in6 *sin6;

    if (size < IPERF_RUTP_MIN_LEN || !server_addr) {
        printf("The param is error.\n");
        return NULL;
    }

    ptr = buf;
    flags = (uint8_t *)ptr;
    *flags = PACKET_PUBLIC_FLAGS_VERSION | PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID 
                    | PACKET_PUBLIC_FLAGS_PROXY;
    ptr++;
    
    cid = (uint64_t *)ptr;
    *cid = htobe64(connection_id);
    ptr += sizeof(uint64_t);

    memcpy(ptr, "Q042", sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    
    type = (uint8_t *)ptr;
    ptr++;
    if (server_addr->sa_family == AF_INET) {
        *type = 1;
        sin = (struct sockaddr_in *)server_addr;
        memcpy(ptr, &sin->sin_addr, 4);
        ptr += 4;
        port = (uint16_t *)ptr;
        *port = sin->sin_port;
    } else {
        *type = 2;
        sin6 = (struct sockaddr_in6 *)server_addr;
        memcpy(ptr, &sin6->sin6_addr, sizeof(sin6->sin6_addr));
        ptr += sizeof(sin6->sin6_addr);
         port = (uint16_t *)ptr;
        *port = sin6->sin6_port;
    }
    ptr += sizeof(uint16_t);
    
    return ptr;
}

static char 
*iperf_rutp_parse_packet(struct iperf_stream *sp, char *buf, int size)
{
    char *ptr;
    uint8_t flags, type;
    uint64_t cid;
    uint16_t port;

    ptr = buf;
    flags = *(uint8_t *)ptr;
    ptr++;
    if (flags & PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID) {
        if (sp->test->debug) {
            cid = be64toh(*(uint64_t *)ptr); 
            printf("Connection id is %llu.\n", cid);
        }
        ptr += sizeof(uint64_t);
    }

    if (flags & PACKET_PUBLIC_FLAGS_VERSION) {
        ptr += sizeof(uint32_t);
    }

    if (flags & PACKET_PUBLIC_FLAGS_PROXY) {
        type = *(uint8_t *)ptr;
        ptr++;
        if (type == 1) {
            ptr += 6;
        } else if (type == 2) {
            ptr += sizeof(struct in6_addr) + 2;
        } else {
            printf("The proxy address type is error.\n");
            return NULL;
        }
    }
    
    if (ptr - buf > size) {
        printf("The received data is too short.\n");
        return NULL;
    }

    return ptr;
}


/* iperf_rutp_recv
 *
 * receives the data for UDP
 */
int
iperf_rutp_recv(struct iperf_stream *sp)
{
    char *ptr;
    uint32_t  sec, usec;
    uint64_t  pcount;
    int       r;
    int       size = sp->settings->blksize;
    int       first_packet = 0;
    double    transit = 0, d = 0;
    struct iperf_time sent_time, arrival_time, temp_time;

    r = Nread(sp->socket, sp->buffer, size, Pudp);

    /*
     * If we got an error in the read, or if we didn't read anything
     * because the underlying read(2) got a EAGAIN, then skip packet
     * processing.
     */
    if (r <= 0)
        return r;

    /* Only count bytes received while we're in the correct state. */
    if (sp->test->state == TEST_RUNNING) {

	/*
	 * For jitter computation below, it's important to know if this
	 * packet is the first packet received.
	 */
	if (sp->result->bytes_received == 0) {
	    first_packet = 1;
	}

	sp->result->bytes_received += r;
	sp->result->bytes_received_this_interval += r;

    ptr = iperf_rutp_parse_packet(sp, sp->buffer, r);
    if (!ptr) {
        return r;
    }
    
	/* Dig the various counters out of the incoming UDP packet */
	if (sp->test->udp_counters_64bit) {
	    memcpy(&sec, ptr, sizeof(sec));
	    memcpy(&usec, ptr+4, sizeof(usec));
	    memcpy(&pcount, ptr+8, sizeof(pcount));
	    sec = ntohl(sec);
	    usec = ntohl(usec);
	    pcount = be64toh(pcount);
	    sent_time.secs = sec;
	    sent_time.usecs = usec;
	}
	else {
	    uint32_t pc;
	    memcpy(&sec, ptr, sizeof(sec));
	    memcpy(&usec, ptr+4, sizeof(usec));
	    memcpy(&pc, ptr+8, sizeof(pc));
	    sec = ntohl(sec);
	    usec = ntohl(usec);
	    pcount = ntohl(pc);
	    sent_time.secs = sec;
	    sent_time.usecs = usec;
	}

	if (sp->test->debug)
	    fprintf(stderr, "pcount %" PRIu64 " packet_count %d\n", pcount, sp->packet_count);

	/*
	 * Try to handle out of order packets.  The way we do this
	 * uses a constant amount of storage but might not be
	 * correct in all cases.  In particular we seem to have the
	 * assumption that packets can't be duplicated in the network,
	 * because duplicate packets will possibly cause some problems here.
	 *
	 * First figure out if the sequence numbers are going forward.
	 * Note that pcount is the sequence number read from the packet,
	 * and sp->packet_count is the highest sequence number seen so
	 * far (so we're expecting to see the packet with sequence number
	 * sp->packet_count + 1 arrive next).
	 */
	if (pcount >= sp->packet_count + 1) {

	    /* Forward, but is there a gap in sequence numbers? */
	    if (pcount > sp->packet_count + 1) {
		/* There's a gap so count that as a loss. */
		sp->cnt_error += (pcount - 1) - sp->packet_count;
	    }
	    /* Update the highest sequence number seen so far. */
	    sp->packet_count = pcount;
	} else {

	    /* 
	     * Sequence number went backward (or was stationary?!?).
	     * This counts as an out-of-order packet.
	     */
	    sp->outoforder_packets++;

	    /*
	     * If we have lost packets, then the fact that we are now
	     * seeing an out-of-order packet offsets a prior sequence
	     * number gap that was counted as a loss.  So we can take
	     * away a loss.
	     */
	    if (sp->cnt_error > 0)
		sp->cnt_error--;
	
	    /* Log the out-of-order packet */
	    if (sp->test->debug) 
		fprintf(stderr, "OUT OF ORDER - incoming packet sequence %" PRIu64 " but expected sequence %d on stream %d", pcount, sp->packet_count + 1, sp->socket);
	}

	/*
	 * jitter measurement
	 *
	 * This computation is based on RFC 1889 (specifically
	 * sections 6.3.1 and A.8).
	 *
	 * Note that synchronized clocks are not required since
	 * the source packet delta times are known.  Also this
	 * computation does not require knowing the round-trip
	 * time.
	 */
	iperf_time_now(&arrival_time);

	iperf_time_diff(&arrival_time, &sent_time, &temp_time);
	transit = iperf_time_in_secs(&temp_time);

	/* Hack to handle the first packet by initializing prev_transit. */
	if (first_packet)
	    sp->prev_transit = transit;

	d = transit - sp->prev_transit;
	if (d < 0)
	    d = -d;
	sp->prev_transit = transit;
	sp->jitter += (d - sp->jitter) / 16.0;
    }
    else {
	if (sp->test->debug)
	    printf("Late receive, state = %d\n", sp->test->state);
    }

    return r;
}


/* iperf_rutp_send
 *
 * sends the data for RUTP
 */
int
iperf_rutp_send(struct iperf_stream *sp)
{
    int r;
    char *ret;
    int       size = sp->settings->blksize;
    struct iperf_time before;

    iperf_time_now(&before);

    ++sp->packet_count;

    ret = iperf_rutp_make_packet((uint64_t)sp->socket, sp->buffer, sp->settings->blksize, 
                                    &sp->test->rutp_server);
    if (!ret) {
        return -1;
    }
    if (sp->test->udp_counters_64bit) {

    	uint32_t  sec, usec;
    	uint64_t  pcount;

    	sec = htonl(before.secs);
    	usec = htonl(before.usecs);
    	pcount = htobe64(sp->packet_count);
    	
    	memcpy(ret, &sec, sizeof(sec));
    	memcpy(ret+4, &usec, sizeof(usec));
    	memcpy(ret+8, &pcount, sizeof(pcount));
	
    }
    else {

    	uint32_t  sec, usec, pcount;

    	sec = htonl(before.secs);
    	usec = htonl(before.usecs);
    	pcount = htonl(sp->packet_count);
    	
    	memcpy(ret, &sec, sizeof(sec));
    	memcpy(ret+4, &usec, sizeof(usec));
    	memcpy(ret+8, &pcount, sizeof(pcount));
	
    }

    r = Nwrite(sp->socket, sp->buffer, size, Pudp);

    if (r < 0)
	return r;

    sp->result->bytes_sent += r;
    sp->result->bytes_sent_this_interval += r;

    if (sp->test->debug)
	printf("sent %d bytes of %d, total %" PRIu64 "\n", r, sp->settings->blksize, sp->result->bytes_sent);

    return r;
}


/**************************************************************************/

/*
 * The following functions all have to do with managing UDP data sockets.
 * UDP of course is connectionless, so there isn't really a concept of
 * setting up a connection, although connect(2) can (and is) used to
 * bind the remote end of sockets.  We need to simulate some of the
 * connection management that is built-in to TCP so that each side of the
 * connection knows about each other before the real data transfers begin.
 */

/*
 * Set and verify socket buffer sizes.
 * Return 0 if no error, -1 if an error, +1 if socket buffers are
 * potentially too small to hold a message.
 */
int
iperf_rutp_buffercheck(struct iperf_test *test, int s)
{
    int rc = 0;
    int sndbuf_actual, rcvbuf_actual;

    /*
     * Set socket buffer size if requested.  Do this for both sending and
     * receiving so that we can cover both normal and --reverse operation.
     */
    int opt;
    socklen_t optlen;
    
    if ((opt = test->settings->socket_bufsize)) {
        if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &opt, sizeof(opt)) < 0) {
            i_errno = IESETBUF;
            return -1;
        }
        if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, &opt, sizeof(opt)) < 0) {
            i_errno = IESETBUF;
            return -1;
        }
    }

    /* Read back and verify the sender socket buffer size */
    optlen = sizeof(sndbuf_actual);
    if (getsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf_actual, &optlen) < 0) {
	i_errno = IESETBUF;
	return -1;
    }
    if (test->debug) {
	printf("SNDBUF is %u, expecting %u\n", sndbuf_actual, test->settings->socket_bufsize);
    }
    if (test->settings->socket_bufsize && test->settings->socket_bufsize > sndbuf_actual) {
	i_errno = IESETBUF2;
	return -1;
    }
    if (test->settings->blksize > sndbuf_actual) {
	char str[80];
	snprintf(str, sizeof(str),
		 "Block size %d > sending socket buffer size %d",
		 test->settings->blksize, sndbuf_actual);
	warning(str);
	rc = 1;
    }

    /* Read back and verify the receiver socket buffer size */
    optlen = sizeof(rcvbuf_actual);
    if (getsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf_actual, &optlen) < 0) {
	i_errno = IESETBUF;
	return -1;
    }
    if (test->debug) {
	printf("RCVBUF is %u, expecting %u\n", rcvbuf_actual, test->settings->socket_bufsize);
    }
    if (test->settings->socket_bufsize && test->settings->socket_bufsize > rcvbuf_actual) {
	i_errno = IESETBUF2;
	return -1;
    }
    if (test->settings->blksize > rcvbuf_actual) {
	char str[80];
	snprintf(str, sizeof(str),
		 "Block size %d > receiving socket buffer size %d",
		 test->settings->blksize, rcvbuf_actual);
	warning(str);
	rc = 1;
    }

    if (test->json_output) {
	cJSON_AddNumberToObject(test->json_start, "sock_bufsize", test->settings->socket_bufsize);
	cJSON_AddNumberToObject(test->json_start, "sndbuf_actual", sndbuf_actual);
	cJSON_AddNumberToObject(test->json_start, "rcvbuf_actual", rcvbuf_actual);
    }

    return rc;
}

/*
 * iperf_rutp_accept
 *
 * Accepts a new UDP "connection"
 */
int
iperf_rutp_accept(struct iperf_test *test)
{
    struct sockaddr_storage sa_peer;
    //int       buf;
    socklen_t len;
    int       sz, s;
    int	      rc;
    char buf[IPERF_RUTP_MIN_LEN];

    /*
     * Get the current outstanding socket.  This socket will be used to handle
     * data transfers and a new "listening" socket will be created.
     */
    s = test->prot_listener;

    /*
     * Grab the UDP packet sent by the client.  From that we can extract the
     * client's address, and then use that information to bind the remote side
     * of the socket to the client.
     */
    len = sizeof(sa_peer);
    if ((sz = recvfrom(test->prot_listener, &buf, sizeof(buf), 0, (struct sockaddr *) &sa_peer, &len)) < 0) {
        i_errno = IESTREAMACCEPT;
        return -1;
    }

    if (connect(s, (struct sockaddr *) &sa_peer, len) < 0) {
        i_errno = IESTREAMACCEPT;
        return -1;
    }

    /* Check and set socket buffer sizes */
    rc = iperf_rutp_buffercheck(test, s);
    if (rc < 0)
	/* error */
	return rc;
    /*
     * If the socket buffer was too small, but it was the default
     * size, then try explicitly setting it to something larger.
     */
    if (rc > 0) {
	if (test->settings->socket_bufsize == 0) {
	    int bufsize = test->settings->blksize + UDP_BUFFER_EXTRA;
	    printf("Increasing socket buffer size to %d\n",
		bufsize);
	    test->settings->socket_bufsize = bufsize;
	    rc = iperf_rutp_buffercheck(test, s);
	    if (rc < 0)
		return rc;
	}
    }
	
#if defined(HAVE_SO_MAX_PACING_RATE)
    /* If socket pacing is specified, try it. */
    if (test->settings->fqrate) {
	/* Convert bits per second to bytes per second */
	unsigned int fqrate = test->settings->fqrate / 8;
	if (fqrate > 0) {
	    if (test->debug) {
		printf("Setting fair-queue socket pacing to %u\n", fqrate);
	    }
	    if (setsockopt(s, SOL_SOCKET, SO_MAX_PACING_RATE, &fqrate, sizeof(fqrate)) < 0) {
		warning("Unable to set socket pacing");
	    }
	}
    }
#endif /* HAVE_SO_MAX_PACING_RATE */
    {
	unsigned int rate = test->settings->rate / 8;
	if (rate > 0) {
	    if (test->debug) {
		printf("Setting application pacing to %u\n", rate);
	    }
	}
    }

    /*
     * Create a new "listening" socket to replace the one we were using before.
     */
    test->prot_listener = netannounce(test->settings->domain, Pudp, test->bind_address, test->server_port);
    if (test->prot_listener < 0) {
        i_errno = IESTREAMLISTEN;
        return -1;
    }

    FD_SET(test->prot_listener, &test->read_set);
    test->max_fd = (test->max_fd < test->prot_listener) ? test->prot_listener : test->max_fd;

    /* Let the client know we're ready "accept" another UDP "stream" */
    //buf = 987654321;		/* any content will work here */
    if (write(s, &buf, sizeof(buf)) < 0) {
        i_errno = IESTREAMWRITE;
        return -1;
    }

    return s;
}


/*
 * iperf_rutp_listen
 *
 * Start up a listener for UDP stream connections.  Unlike for TCP,
 * there is no listen(2) for UDP.  This socket will however accept
 * a UDP datagram from a client (indicating the client's presence).
 */
int
iperf_rutp_listen(struct iperf_test *test)
{
    int s;

    if ((s = netannounce(test->settings->domain, Pudp, test->bind_address, test->server_port)) < 0) {
        i_errno = IESTREAMLISTEN;
        return -1;
    }

    /*
     * The caller will put this value into test->prot_listener.
     */
    return s;
}


/*
 * iperf_udp_connect
 *
 * "Connect" to a UDP stream listener.
 */
int
iperf_rutp_connect(struct iperf_test *test)
{
    int s, sz;
#ifdef SO_RCVTIMEO
    struct timeval tv;
#endif
    int rc;
    char *ret;
    char buf[IPERF_RUTP_MIN_LEN];

    /* Create and bind our local socket. */
    if ((s = netdial(test->settings->domain, Pudp, test->bind_address, test->bind_port, test->server_hostname, test->server_port, -1)) < 0) {
        i_errno = IESTREAMCONNECT;
        return -1;
    }

    /* Check and set socket buffer sizes */
    rc = iperf_rutp_buffercheck(test, s);
    if (rc < 0)
	/* error */
	return rc;
    /*
     * If the socket buffer was too small, but it was the default
     * size, then try explicitly setting it to something larger.
     */
    if (rc > 0) {
	if (test->settings->socket_bufsize == 0) {
	    int bufsize = test->settings->blksize + UDP_BUFFER_EXTRA;
	    printf("Increasing socket buffer size to %d\n",
		bufsize);
	    test->settings->socket_bufsize = bufsize;
	    rc = iperf_rutp_buffercheck(test, s);
	    if (rc < 0)
		return rc;
	}
    }
	
#if defined(HAVE_SO_MAX_PACING_RATE)
    /* If socket pacing is available and not disabled, try it. */
    if (test->settings->fqrate) {
	/* Convert bits per second to bytes per second */
	unsigned int fqrate = test->settings->fqrate / 8;
	if (fqrate > 0) {
	    if (test->debug) {
		printf("Setting fair-queue socket pacing to %u\n", fqrate);
	    }
	    if (setsockopt(s, SOL_SOCKET, SO_MAX_PACING_RATE, &fqrate, sizeof(fqrate)) < 0) {
		warning("Unable to set socket pacing");
	    }
	}
    }
#endif /* HAVE_SO_MAX_PACING_RATE */
    {
	unsigned int rate = test->settings->rate / 8;
	if (rate > 0) {
	    if (test->debug) {
		printf("Setting application pacing to %u\n", rate);
	    }
	}
    }

#ifdef SO_RCVTIMEO
    /* 30 sec timeout for a case when there is a network problem. */
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (struct timeval *)&tv, sizeof(struct timeval));
#endif

    /*
     * Write a datagram to the UDP stream to let the server know we're here.
     * The server learns our address by obtaining its peer's address.
     */
    //buf = 123456789;		/* this can be pretty much anything */
    memset(buf, 0, sizeof(buf));
    ret = iperf_rutp_make_packet((uint64_t)s, buf, sizeof(buf), &test->rutp_server);
    if (write(s, &buf, sizeof(buf)) < 0) {
        // XXX: Should this be changed to IESTREAMCONNECT? 
        i_errno = IESTREAMWRITE;
        return -1;
    }

    /*
     * Wait until the server replies back to us.
     */
    if ((sz = recv(s, &buf, sizeof(buf), 0)) < 0) {
        i_errno = IESTREAMREAD;
        return -1;
    }

    return s;
}


/* iperf_rutp_init
 *
 * initializer for UDP streams in TEST_START
 */
int
iperf_rutp_init(struct iperf_test *test)
{
    return 0;
}