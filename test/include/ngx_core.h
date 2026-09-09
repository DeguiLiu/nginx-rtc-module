/*
 * ngx_core.h - host-test stub for the nginx core header.
 *
 * Provides the nginx types and queue/rbtree/memory macros used by
 * ngx_rtc_core.c. Queue macros are real (they are trivially correct); the
 * rbtree insert/lookup helpers and ngx_crc32_long are declared here and defined
 * as link-only no-ops in test/nginx_stub.c, because the host tests never call
 * the source registry (ngx_rtc_source_get / *_first / *_next).
 */

#ifndef NGX_STUB_CORE_H
#define NGX_STUB_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef unsigned char u_char;
typedef unsigned long ngx_msec_t;
typedef uintptr_t ngx_uint_t;
typedef intptr_t  ngx_int_t;

/* Cached monotonic clock (host build: link-only global, see nginx_stub.c). */
extern volatile ngx_msec_t ngx_current_msec;

/* Nginx-style helpers reduced to their C-library equivalents for the host
 * build. The media modules are nginx-free; these macros keep the few nginx
 * idioms used by ngx_rtc_core.c compilable without the real nginx headers. */
#define ngx_strlen(s) strlen((const char *)(s))
#define NGX_ERROR ((ssize_t)-1)
#define NGX_AGAIN ((ssize_t)-2)
#define NGX_LOG_DEBUG_EVENT 8
#define ngx_socket_errno 0
#define ngx_log_debug0(level, log, err, fmt) ((void)0)

/* --- ngx_queue (RFC-style intrusive doubly-linked list) --- */
typedef struct ngx_queue_s ngx_queue_t;
struct ngx_queue_s
{
    ngx_queue_t *prev;
    ngx_queue_t *next;
};

#define ngx_queue_init(q) \
    do { (q)->prev = (q); (q)->next = (q); } while (0)
#define ngx_queue_head(h) ((h)->next)
#define ngx_queue_sentinel(h) (h)
#define ngx_queue_next(q) ((q)->next)
#define ngx_queue_empty(h) ((h) == (h)->next)
#define ngx_queue_insert_head(h, x) \
    do { \
        (x)->next = (h)->next; \
        (x)->next->prev = (x); \
        (x)->prev = (h); \
        (h)->next = (x); \
    } while (0)
#define ngx_queue_remove(x) \
    do { \
        (x)->next->prev = (x)->prev; \
        (x)->prev->next = (x)->next; \
        (x)->prev = NULL; \
        (x)->next = NULL; \
    } while (0)
#define ngx_queue_data(q, type, link) \
    ((type *) ((u_char *) (q) - offsetof(type, link)))

/* --- ngx_rbtree (types only; helpers are link-only stubs) --- */
typedef unsigned long ngx_rbtree_key_t;

typedef struct ngx_rbtree_node_s ngx_rbtree_node_t;
struct ngx_rbtree_node_s
{
    ngx_rbtree_key_t  key;
    ngx_rbtree_node_t *left;
    ngx_rbtree_node_t *right;
    ngx_rbtree_node_t *parent;
    u_char            color;
    u_char            data;
};

typedef struct ngx_str_s
{
    u_char *data;
    size_t  len;
} ngx_str_t;

typedef struct ngx_str_node_s
{
    ngx_rbtree_node_t node;
    ngx_str_t         str;
} ngx_str_node_t;

typedef void (*ngx_rbtree_insert_pt)(ngx_rbtree_node_t *root,
                                     ngx_rbtree_node_t *node,
                                     ngx_rbtree_node_t *sentinel);

typedef struct ngx_rbtree_s
{
    ngx_rbtree_node_t     *root;
    ngx_rbtree_node_t     *sentinel;
    ngx_rbtree_insert_pt   insert;
} ngx_rbtree_t;

#define ngx_rbtree_min(node, sentinel) (node)

void ngx_rbtree_insert(ngx_rbtree_t *tree, ngx_rbtree_node_t *node);
ngx_rbtree_node_t *ngx_rbtree_next(ngx_rbtree_t *tree, ngx_rbtree_node_t *node);
void ngx_str_rbtree_insert_value(ngx_rbtree_node_t *root,
                                 ngx_rbtree_node_t *node,
                                 ngx_rbtree_node_t *sentinel);
ngx_str_node_t *ngx_str_rbtree_lookup(ngx_rbtree_t *rbtree, ngx_str_t *name,
                                      uint32_t hash);
void ngx_rbtree_delete(ngx_rbtree_t *tree, ngx_rbtree_node_t *node);

/* --- misc nginx helpers --- */
uint32_t ngx_crc32_long(u_char *p, size_t len);
#define ngx_memcpy(dst, src, n) (void)memcpy((dst), (src), (n))

/* --- connection (only the send method ngx_rtc_session_send_rtp uses) --- */
typedef struct ngx_connection_s ngx_connection_t;
struct ngx_connection_s
{
    int (*send)(ngx_connection_t *c, u_char *buf, size_t size);
    void *log;
};

#endif /* NGX_STUB_CORE_H */
