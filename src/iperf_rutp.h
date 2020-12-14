/*
 * iperf, Copyright (c) 2014, The Regents of the University of
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
#ifndef __IPERF_RUTP_H
#define __IPERF_RUTP_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define RUTP_MIN_PACKET_LEN      11
#define RUTP_MIN_CHLO_LEN        1000

/* Used to indicate a QuicSequenceNumberLength using two flag bits. */
enum RUTP_PACKET_LEN_FLAGS {
    PACKET_FLAGS_1BYTE_PACKET = 0,           /* 00 */
    PACKET_FLAGS_2BYTE_PACKET = 1,           /* 01 */
    PACKET_FLAGS_4BYTE_PACKET = 1 << 1,      /* 10 */
    PACKET_FLAGS_8BYTE_PACKET = 1 << 1 | 1,  /* 11 */
};

/* The public flags are specified in one byte. */
enum RUTP_PACKET_PUBLIC_FLAGS {
    PACKET_PUBLIC_FLAGS_NONE = 0,

    /* Bit 0: Does the packet header contains version info? */
    PACKET_PUBLIC_FLAGS_VERSION = 1 << 0,

    /* Bit 1: Is this packet a public reset packet? */
    PACKET_PUBLIC_FLAGS_RST = 1 << 1,

    /* Bit 2: indicates the header includes a nonce. */
    PACKET_PUBLIC_FLAGS_NONCE = 1 << 2,

    /* Bit 3: indicates whether a ConnectionID is included. */
    PACKET_PUBLIC_FLAGS_0BYTE_CONNECTION_ID = 0,
    PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID = 1 << 3,

    /* QUIC_VERSION_32 and earlier use two bits for an 8 byte */
    /* connection id.*/
    PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID_OLD = 1 << 3 | 1 << 2,

    /* Bits 4 and 5 describe the packet number length as follows: */
    /* --00----: 1 byte */
    /* --01----: 2 bytes */
    /* --10----: 4 bytes */
    /* --11----: 6 bytes */
    PACKET_PUBLIC_FLAGS_1BYTE_PACKET = PACKET_FLAGS_1BYTE_PACKET << 4,
    PACKET_PUBLIC_FLAGS_2BYTE_PACKET = PACKET_FLAGS_2BYTE_PACKET << 4,
    PACKET_PUBLIC_FLAGS_4BYTE_PACKET = PACKET_FLAGS_4BYTE_PACKET << 4,
    PACKET_PUBLIC_FLAGS_6BYTE_PACKET = PACKET_FLAGS_8BYTE_PACKET << 4,

    /* Reserved, unimplemented flags: */
    PACKET_PUBLIC_FLAGS_PROXY = 1 << 6,

    /* Bit 7: indicates the presence of a second flags byte. */
    PACKET_PUBLIC_FLAGS_TWO_OR_MORE_BYTES = 1 << 7,

    /* All bits set (bit 7 is not currently used): 01111111 */
    PACKET_PUBLIC_FLAGS_MAX = (1 << 6) - 1,
};


/**
 * iperf_rutp_recv -- receives the client data for RUTP
 *
 *returns state of packet received
 *
 */
int iperf_rutp_recv(struct iperf_stream *);

/**
 * iperf_rutp_send -- sends the client data for RUTP
 *
 * returns: bytes sent
 *
 */
int iperf_rutp_send(struct iperf_stream *) /* __attribute__((hot)) */;


/**
 * iperf_rutp_accept -- accepts a new RUTP connection
 * on udp_listener_socket
 *returns 0 on success
 *
 */
int iperf_rutp_accept(struct iperf_test *);


int iperf_rutp_listen(struct iperf_test *);

int iperf_rutp_connect(struct iperf_test *);

int iperf_rutp_init(struct iperf_test *);


#endif
