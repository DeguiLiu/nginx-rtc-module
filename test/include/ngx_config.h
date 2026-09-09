/*
 * ngx_config.h - host-test stub for the nginx auto-config header.
 *
 * ngx_rtc_core.c includes <ngx_config.h> / <ngx_core.h> / <ngx_event.h>
 * unconditionally even though its GOP-ring helpers (the part exercised by the
 * host tests) only need calloc + memcpy. These stubs provide just enough nginx
 * surface to compile that one translation unit; the source/session registry
 * half of ngx_rtc_core.c is never called by the host tests and its rbtree /
 * queue / crc32 symbols are link-only stubs in test/nginx_stub.c.
 */

#ifndef NGX_STUB_CONFIG_H
#define NGX_STUB_CONFIG_H

#endif /* NGX_STUB_CONFIG_H */
