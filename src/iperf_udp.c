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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
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
#include "iperf_udp.h"
#include "timer.h"
#include "net.h"
#include "portable_endian.h"

/* iperf_udp_recv
 *
 * receives the data for UDP
 */
long long counter  = 0;
int
iperf_udp_recv(struct iperf_stream *sp)
{
    uint32_t  sec, usec;
    uint64_t  pcount;
    int       r;
    int       size = sp->settings->blksize;
    double    transit = 0, d = 0;
    struct timeval sent_time, arrival_time;

    r = Nread(sp->socket, sp->buffer, size, Pudp);

    /*
     * If we got an error in the read, or if we didn't read anything
     * because the underlying read(2) got a EAGAIN, then skip packet
     * processing.
     */
    if (r <= 0)
        return r;

    sp->result->bytes_received += r;
    sp->result->bytes_received_this_interval += r;

    if (sp->test->udp_counters_64bit) {
	memcpy(&sec, sp->buffer, sizeof(sec));
	memcpy(&usec, sp->buffer+4, sizeof(usec));
	memcpy(&pcount, sp->buffer+8, sizeof(pcount));
	sec = ntohl(sec);
	usec = ntohl(usec);
	pcount = be64toh(pcount);
	sent_time.tv_sec = sec;
	sent_time.tv_usec = usec;
    }
    else {
	uint32_t pc;
	memcpy(&sec, sp->buffer, sizeof(sec));
	memcpy(&usec, sp->buffer+4, sizeof(usec));
	memcpy(&pc, sp->buffer+8, sizeof(pc));
	sec = ntohl(sec);
	usec = ntohl(usec);
	pcount = ntohl(pc);
	sent_time.tv_sec = sec;
	sent_time.tv_usec = usec;
    }

    /* Out of order packets */
    if (pcount >= sp->packet_count + 1) {
        if (pcount > sp->packet_count + 1) {
            sp->cnt_error += (pcount - 1) - sp->packet_count;
        }
        sp->packet_count = pcount;
    } else {
        sp->outoforder_packets++;
	iperf_err(sp->test, "OUT OF ORDER - incoming packet = %zu and received packet = %d AND SP = %d", pcount, sp->packet_count, sp->socket);
    }

    /* jitter measurement */
    gettimeofday(&arrival_time, NULL);

    transit = timeval_diff(&sent_time, &arrival_time);
    d = transit - sp->prev_transit;
    if (d < 0)
        d = -d;
    sp->prev_transit = transit;
    // XXX: This is NOT the way to calculate jitter
    //      J = |(R1 - S1) - (R0 - S0)| [/ number of packets, for average]
    sp->jitter += (d - sp->jitter) / 16.0;

    if (sp->test->debug) {
	fprintf(stderr, "packet_count %d\n", sp->packet_count);
    }

    return r;
}


/* iperf_udp_send
 *
 * sends the data for UDP
 */
int
iperf_udp_send(struct iperf_stream *sp)
{
    int r;
    int       size = sp->settings->blksize;
    struct timeval before;

    gettimeofday(&before, 0);

    ++sp->packet_count;

    if (sp->test->udp_counters_64bit) {

	uint32_t  sec, usec;
	uint64_t  pcount;

	sec = htonl(before.tv_sec);
	usec = htonl(before.tv_usec);
	pcount = htobe64(sp->packet_count);
	
	memcpy(sp->buffer, &sec, sizeof(sec));
	memcpy(sp->buffer+4, &usec, sizeof(usec));
	memcpy(sp->buffer+8, &pcount, sizeof(pcount));
	
    }
    else {

	uint32_t  sec, usec, pcount;

	sec = htonl(before.tv_sec);
	usec = htonl(before.tv_usec);
	pcount = htonl(sp->packet_count);
	
	memcpy(sp->buffer, &sec, sizeof(sec));
	memcpy(sp->buffer+4, &usec, sizeof(usec));
	memcpy(sp->buffer+8, &pcount, sizeof(pcount));
	
    }

//    r = Nwrite(sp->socket, sp->buffer, size, Pudp);
//
    char buf[2048];
    sprintf(buf, "%lld", ++counter);
        
      r = Nwrite(sp->socket, buf, sizeof(buf), Pudp);

    if (r < 0)
	return r;

    sp->result->bytes_sent += r;
    sp->result->bytes_sent_this_interval += r;

    return r;
}


/**************************************************************************/

/* iperf_udp_accept
 *
 * accepts a new UDP connection
 */
int
iperf_udp_accept(struct iperf_test *test)
{
    struct sockaddr_storage sa_peer;
    int       buf;
    socklen_t len;
    int       sz, s;

    s = test->prot_listener;

    len = sizeof(sa_peer);
    if ((sz = recvfrom(test->prot_listener, &buf, sizeof(buf), 0, (struct sockaddr *) &sa_peer, &len)) < 0) {
        i_errno = IESTREAMACCEPT;
        return -1;
    }

    if (connect(s, (struct sockaddr *) &sa_peer, len) < 0) {
        i_errno = IESTREAMACCEPT;
        return -1;
    }

    test->prot_listener = netannounce(test->settings->domain, Pudp, test->bind_address, test->server_port);
    if (test->prot_listener < 0) {
        i_errno = IESTREAMLISTEN;
        return -1;
    }

    FD_SET(test->prot_listener, &test->read_set);
    test->max_fd = (test->max_fd < test->prot_listener) ? test->prot_listener : test->max_fd;

    /* Let the client know we're ready "accept" another UDP "stream" */
    buf = 987654321;
    if (write(s, &buf, sizeof(buf)) < 0) {
        i_errno = IESTREAMWRITE;
        return -1;
    }

    return s;
}


/* iperf_udp_listen
 *
 * start up a listener for UDP stream connections
 */
int
iperf_udp_listen(struct iperf_test *test)
{
    int s;

    if ((s = netannounce(test->settings->domain, Pudp, test->bind_address, test->server_port)) < 0) {
        i_errno = IESTREAMLISTEN;
        return -1;
    }

    return s;
}


/* iperf_udp_connect
 *
 * connect to a TCP stream listener
 */
int
iperf_udp_connect(struct iperf_test *test)
{
    int s, buf, sz;

    if ((s = netdial(test->settings->domain, Pudp, test->bind_address, test->bind_port, test->server_hostname, test->server_port)) < 0) {
        i_errno = IESTREAMCONNECT;
        return -1;
    }

    /* Write to the UDP stream to let the server know we're here. */
    buf = 123456789;
    if (write(s, &buf, sizeof(buf)) < 0) {
        // XXX: Should this be changed to IESTREAMCONNECT? 
        i_errno = IESTREAMWRITE;
        return -1;
    }

    /* Wait until the server confirms the client UDP write */
    // XXX: Should this read be TCP instead?
    if ((sz = recv(s, &buf, sizeof(buf), 0)) < 0) {
        i_errno = IESTREAMREAD;
        return -1;
    }

    return s;
}


/* iperf_udp_init
 *
 * initializer for UDP streams in TEST_START
 */
int
iperf_udp_init(struct iperf_test *test)
{
    return 0;
}
