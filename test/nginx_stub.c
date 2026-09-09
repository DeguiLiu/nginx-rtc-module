/*
 * nginx_stub.c - link-only stubs for the nginx surface used by ngx_rtc_core.c.
 *
 * The host tests exercise only the GOP-ring helpers in ngx_rtc_core.c
 * (ngx_rtc_rtp_ring_push / _get / _replay). The source/session registry half of
 * that file references nginx rbtree / crc32 / queue helpers and the SRTP
 * protector; these symbols must exist for the final link but are never called,
 * so each is a trivial no-op. Do NOT write host tests against the registry
 * functions (ngx_rtc_source_get / *_first / *_next / session add/remove):
 * they depend on the real nginx rbtree semantics that these stubs do not model.
 */

#include <ngx_core.h>

#include "ngx_rtc_srtp.h"

void
ngx_rbtree_insert(ngx_rbtree_t *tree, ngx_rbtree_node_t *node)
{
    (void)tree;
    (void)node;
}

void
ngx_rbtree_delete(ngx_rbtree_t *tree, ngx_rbtree_node_t *node)
{
    (void)tree;
    (void)node;
}

ngx_rbtree_node_t *
ngx_rbtree_next(ngx_rbtree_t *tree, ngx_rbtree_node_t *node)
{
    (void)tree;
    (void)node;
    return NULL;
}

void
ngx_str_rbtree_insert_value(ngx_rbtree_node_t *root, ngx_rbtree_node_t *node,
                            ngx_rbtree_node_t *sentinel)
{
    (void)root;
    (void)node;
    (void)sentinel;
}

ngx_str_node_t *
ngx_str_rbtree_lookup(ngx_rbtree_t *rbtree, ngx_str_t *name, uint32_t hash)
{
    (void)rbtree;
    (void)name;
    (void)hash;
    return NULL;
}

uint32_t
ngx_crc32_long(u_char *p, size_t len)
{
    (void)p;
    (void)len;
    return 0u;
}

int
ngx_rtc_srtp_protect_rtp(ngx_rtc_srtp_t *srtp, uint8_t *packet, int *nb)
{
    /* Host tests do not build libsrtp2: replay delivery is asserted on the
     * plaintext copy, so "protect" is a pass-through that keeps *nb unchanged. */
    (void)srtp;
    (void)packet;
    (void)nb;
    return 0;
}
