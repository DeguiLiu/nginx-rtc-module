/*
 * srtp2/srtp.h - host-test stub for libsrtp2.
 *
 * The unit tests do not build ngx_rtc_srtp.c (which would need the real
 * libsrtp2). ngx_rtc_core.h only needs the srtp_t type to size the
 * ngx_rtc_srtp_t struct, and srtp_t is a pointer type in libsrtp2.
 */
#ifndef SRTP2_SRTP_H
#define SRTP2_SRTP_H

typedef void *srtp_t;

#endif /* SRTP2_SRTP_H */
