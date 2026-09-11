/*
 * nginx_stub.c - the nginx surface the host build has to supply itself.
 *
 * What this file provides depends on which headers the build is using, and the
 * two branches below are that difference:
 *
 *   - Against a configured nginx tree (the Linux host build; see NGX_SRC in
 *     test/Makefile) nginx supplies its own types and macros, and this file
 *     only supplies the symbols nginx's .c files would: the slab, the shm
 *     mutex, the logger, the timer rbtree. nginx_stub.c is not linked into
 *     that build as a stub of nginx -- it stands in for the handful of units
 *     the tests deliberately do not link.
 *   - Without one (the Windows cross build, which cannot use a Linux-configured
 *     tree) it also supplies the objects themselves, from test/include/.
 *
 * The registry IS covered, by test_shm.c: source and session lifecycle,
 * ownership hand-off and the reaper's reclaim. The earlier note here said the
 * opposite -- that host tests must not touch ngx_rtc_source_get / *_first /
 * *_next / session add-remove -- because the rbtree helpers were link-only
 * no-ops at the time. They are real now, which is what made the registry
 * testable and what let the sanitizer catch the shm source leak.
 */

#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_stream.h>

#include "ngx_rtc_shm.h"
#include "ngx_rtc_srtp.h"
#include "ngx_rtc_core.h"

volatile ngx_msec_t ngx_current_msec = 0;

/*
 * Real red-black tree, replacing the link-only no-ops this file used to carry.
 * Their comment read "the host tests never call the source registry" -- but
 * test_shm.c does now, and with empty bodies the registry was silently broken:
 * lookup always missed, so get() allocated a fresh source on every call and a
 * miss never returned NULL. Both failures showed up as test_shm.c FAILs the
 * moment the unit was linked in, which is the point of covering it.
 */

static void
ngx_rbtree_left_rotate(ngx_rbtree_node_t **root, ngx_rbtree_node_t *sentinel,
    ngx_rbtree_node_t *node)
{
    ngx_rbtree_node_t  *temp;

    temp = node->right;
    node->right = temp->left;

    if (temp->left != sentinel) {
        temp->left->parent = node;
    }

    temp->parent = node->parent;

    if (node == *root) {
        *root = temp;

    } else if (node == node->parent->left) {
        node->parent->left = temp;

    } else {
        node->parent->right = temp;
    }

    temp->left = node;
    node->parent = temp;
}


static void
ngx_rbtree_right_rotate(ngx_rbtree_node_t **root, ngx_rbtree_node_t *sentinel,
    ngx_rbtree_node_t *node)
{
    ngx_rbtree_node_t  *temp;

    temp = node->left;
    node->left = temp->right;

    if (temp->right != sentinel) {
        temp->right->parent = node;
    }

    temp->parent = node->parent;

    if (node == *root) {
        *root = temp;

    } else if (node == node->parent->right) {
        node->parent->right = temp;

    } else {
        node->parent->left = temp;
    }

    temp->right = node;
    node->parent = temp;
}


void
ngx_rbtree_insert(ngx_rbtree_t *tree, ngx_rbtree_node_t *node)
{
    ngx_rbtree_node_t  **root, *temp, *sentinel;

    /* a binary tree insert */

    root = &tree->root;
    sentinel = tree->sentinel;

    if (*root == sentinel) {
        node->parent = NULL;
        node->left = sentinel;
        node->right = sentinel;
        ngx_rbt_black(node);
        *root = node;

        return;
    }

    tree->insert(*root, node, sentinel);

    /* re-balance tree */

    while (node != *root && ngx_rbt_is_red(node->parent)) {

        if (node->parent == node->parent->parent->left) {
            temp = node->parent->parent->right;

            if (ngx_rbt_is_red(temp)) {
                ngx_rbt_black(node->parent);
                ngx_rbt_black(temp);
                ngx_rbt_red(node->parent->parent);
                node = node->parent->parent;

            } else {
                if (node == node->parent->right) {
                    node = node->parent;
                    ngx_rbtree_left_rotate(root, sentinel, node);
                }

                ngx_rbt_black(node->parent);
                ngx_rbt_red(node->parent->parent);
                ngx_rbtree_right_rotate(root, sentinel, node->parent->parent);
            }

        } else {
            temp = node->parent->parent->left;

            if (ngx_rbt_is_red(temp)) {
                ngx_rbt_black(node->parent);
                ngx_rbt_black(temp);
                ngx_rbt_red(node->parent->parent);
                node = node->parent->parent;

            } else {
                if (node == node->parent->left) {
                    node = node->parent;
                    ngx_rbtree_right_rotate(root, sentinel, node);
                }

                ngx_rbt_black(node->parent);
                ngx_rbt_red(node->parent->parent);
                ngx_rbtree_left_rotate(root, sentinel, node->parent->parent);
            }
        }
    }

    ngx_rbt_black(*root);
}


ngx_rbtree_node_t *
ngx_rbtree_next(ngx_rbtree_t *tree, ngx_rbtree_node_t *node)
{
    ngx_rbtree_node_t  *root, *sentinel, *parent;

    sentinel = tree->sentinel;

    if (node != sentinel) {

        if (node->right != sentinel) {
            return ngx_rbtree_min(node->right, sentinel);
        }

        root = tree->root;

        for ( ;; ) {
            parent = node->parent;

            if (node == root) {
                return NULL;
            }

            if (node == parent->left) {
                return parent;
            }

            node = parent;
        }
    }

    return NULL;
}


void
ngx_str_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_str_node_t      *n, *t;
    ngx_rbtree_node_t  **p;

    for ( ;; ) {

        n = (ngx_str_node_t *) node;
        t = (ngx_str_node_t *) temp;

        if (node->key != temp->key) {
            p = (node->key < temp->key) ? &temp->left : &temp->right;

        } else if (n->str.len != t->str.len) {
            p = (n->str.len < t->str.len) ? &temp->left : &temp->right;

        } else {
            p = (memcmp(n->str.data, t->str.data, n->str.len) < 0)
                 ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


ngx_str_node_t *
ngx_str_rbtree_lookup(ngx_rbtree_t *rbtree, ngx_str_t *val, uint32_t hash)
{
    ngx_str_node_t     *n;
    ngx_rbtree_node_t  *node, *sentinel;
    int                 rc;

    node = rbtree->root;
    sentinel = rbtree->sentinel;

    while (node != sentinel) {

        n = (ngx_str_node_t *) node;

        if (hash != node->key) {
            node = (hash < node->key) ? node->left : node->right;
            continue;
        }

        if (val->len != n->str.len) {
            node = (val->len < n->str.len) ? node->left : node->right;
            continue;
        }

        rc = memcmp(val->data, n->str.data, val->len);

        if (rc < 0) {
            node = node->left;
            continue;
        }

        if (rc > 0) {
            node = node->right;
            continue;
        }

        return n;
    }

    return NULL;
}


void
ngx_rbtree_delete(ngx_rbtree_t *tree, ngx_rbtree_node_t *node)
{
    ngx_uint_t           red;
    ngx_rbtree_node_t  **root, *sentinel, *subst, *temp, *w;

    /* The registry removes a source by deleting its rbtree node and then
     * freeing it. Leaving the node in the tree (as the old empty stub did)
     * keeps a pointer to freed memory, and the next lookup dereferences it --
     * that was the SIGSEGV test_shm.c hit the first time it ran. */

    root = &tree->root;
    sentinel = tree->sentinel;

    if (node->left == sentinel) {
        temp = node->right;
        subst = node;

    } else if (node->right == sentinel) {
        temp = node->left;
        subst = node;

    } else {
        subst = ngx_rbtree_min(node->right, sentinel);
        temp = subst->right;
    }

    if (subst == *root) {
        *root = temp;
        ngx_rbt_black(temp);

        node->left = NULL;
        node->right = NULL;
        node->parent = NULL;
        node->key = 0;

        return;
    }

    red = ngx_rbt_is_red(subst);

    if (subst == subst->parent->left) {
        subst->parent->left = temp;

    } else {
        subst->parent->right = temp;
    }

    if (subst == node) {

        temp->parent = subst->parent;

    } else {

        if (subst->parent == node) {
            temp->parent = subst;

        } else {
            temp->parent = subst->parent;
        }

        subst->left = node->left;
        subst->right = node->right;
        subst->parent = node->parent;
        ngx_rbt_copy_color(subst, node);

        if (node == *root) {
            *root = subst;

        } else {
            if (node == node->parent->left) {
                node->parent->left = subst;
            } else {
                node->parent->right = subst;
            }
        }

        if (subst->left != sentinel) {
            subst->left->parent = subst;
        }

        if (subst->right != sentinel) {
            subst->right->parent = subst;
        }
    }

    node->left = NULL;
    node->right = NULL;
    node->parent = NULL;
    node->key = 0;

    if (red) {
        return;
    }

    /* a delete fixup */

    while (temp != *root && ngx_rbt_is_black(temp)) {

        if (temp == temp->parent->left) {
            w = temp->parent->right;

            if (ngx_rbt_is_red(w)) {
                ngx_rbt_black(w);
                ngx_rbt_red(temp->parent);
                ngx_rbtree_left_rotate(root, sentinel, temp->parent);
                w = temp->parent->right;
            }

            if (ngx_rbt_is_black(w->left) && ngx_rbt_is_black(w->right)) {
                ngx_rbt_red(w);
                temp = temp->parent;

            } else {
                if (ngx_rbt_is_black(w->right)) {
                    ngx_rbt_black(w->left);
                    ngx_rbt_red(w);
                    ngx_rbtree_right_rotate(root, sentinel, w);
                    w = temp->parent->right;
                }

                ngx_rbt_copy_color(w, temp->parent);
                ngx_rbt_black(temp->parent);
                ngx_rbt_black(w->right);
                ngx_rbtree_left_rotate(root, sentinel, temp->parent);
                temp = *root;
            }

        } else {
            w = temp->parent->left;

            if (ngx_rbt_is_red(w)) {
                ngx_rbt_black(w);
                ngx_rbt_red(temp->parent);
                ngx_rbtree_right_rotate(root, sentinel, temp->parent);
                w = temp->parent->left;
            }

            if (ngx_rbt_is_black(w->left) && ngx_rbt_is_black(w->right)) {
                ngx_rbt_red(w);
                temp = temp->parent;

            } else {
                if (ngx_rbt_is_black(w->left)) {
                    ngx_rbt_black(w->right);
                    ngx_rbt_red(w);
                    ngx_rbtree_left_rotate(root, sentinel, w);
                    w = temp->parent->left;
                }

                ngx_rbt_copy_color(w, temp->parent);
                ngx_rbt_black(temp->parent);
                ngx_rbt_black(w->left);
                ngx_rbtree_right_rotate(root, sentinel, temp->parent);
                temp = *root;
            }
        }
    }

    ngx_rbt_black(temp);
}

#ifndef NGX_RTC_REAL_NGINX_HEADERS
/* nginx's ngx_crc32_long() is an inline that reads this table, which
 * ngx_crc32.c fills in -- but only under the real headers. The stub header has
 * no inline, so it needs the function, and the two other nginx helpers that
 * nginx supplies as macros (ngx_random, ngx_gettimeofday) need real symbols
 * because the stub header has no macro either. */
uint32_t
ngx_crc32_long(u_char *p, size_t len)
{
    (void)p;
    (void)len;
    return 0u;
}


uint32_t
ngx_random(void)
{
    return (uint32_t) rand();
}


void
ngx_gettimeofday(struct timeval *tp)
{
    (void) gettimeofday(tp, NULL);
}


/* Handlers run inline under the stub ngx_post_event, so nothing is ever left
 * queued. The symbol exists so a test can be written without knowing which
 * header world it is compiled in. */
void
ngx_rtc_host_drain_posted(void)
{
}


/* nginx's ngx_log_stderr() is what the module's config-time refusals call. The
 * stub header defines it as a variadic macro that discards its arguments, so
 * the macro has to go before the real function of that name can be declared
 * here; under the real headers there is no macro and the undef is a no-op. */
#undef ngx_log_stderr

void
ngx_log_stderr(ngx_err_t err, const char *fmt, ...)
{
    (void) err;
    (void) fmt;
}

#endif /* !NGX_RTC_REAL_NGINX_HEADERS */

/*
 * The default host build does not compile src/ngx_rtc_srtp.c (it would drag in
 * libsrtp2), so replay-delivery tests get a pass-through stand-in. The
 * sanitizer build does link the real unit -- see scripts/sanitize-tests.sh,
 * which defines NGX_RTC_TEST_REAL_SRTP so this stub steps aside instead of
 * colliding at link time.
 */
/*
 * Controllable result for the stub below, in the same spirit as g_send_rc in
 * test_rtc_core.c. The SRTP failure branch in ngx_rtc_session_send_rtp -- the
 * one abandon point in that function that is not a socket error, and the one
 * that has to give the transport-wide sequence back -- was unreachable while
 * protect always returned 0, so nothing asserted it.
 *
 * Declared OUTSIDE the guard, and left at 0 by default, for two reasons: every
 * other test keeps its pass-through, and the symbol exists in both builds so
 * the sanitizer build (which links the real unit and compiles this stub out)
 * still links. That build cannot honour the injection, so the single case that
 * sets it is compiled out there.
 */
int ngx_rtc_test_srtp_protect_rc = 0;

#ifndef NGX_RTC_TEST_REAL_SRTP
int
ngx_rtc_srtp_protect_rtp(ngx_rtc_srtp_t *srtp, uint8_t *packet, int *nb)
{
    /* "protect" is a pass-through that keeps *nb unchanged: the tests that use
     * it assert on the plaintext copy. */
    (void)srtp;
    (void)packet;
    (void)nb;
    return ngx_rtc_test_srtp_protect_rc;
}


/* The rest of the libsrtp2 surface ngx_rtc_stream_module.c reaches. libsrtp2 is
 * not installed here, so ngx_rtc_srtp.c cannot compile and SRTP is NOT covered
 * by host tests -- the end-to-end run is what exercises it. create() leaves a
 * zeroed handle and destroy() clears it, so a session that never negotiates
 * SRTP still tears down cleanly and the sanitizer keeps its signal for the
 * parts that ARE covered (session lifetime, close-path UAF, double free). */
int
ngx_rtc_srtp_global_init(void)
{
    return 0;
}


int
ngx_rtc_srtp_create(ngx_rtc_srtp_t *srtp, const uint8_t *recv_key,
                    const uint8_t *send_key)
{
    (void) recv_key;
    (void) send_key;

    if (NULL == srtp) {
        return -1;
    }

    /* No key schedule is derived: see the note above. */
    srtp->recv_ctx = NULL;
    srtp->send_ctx = NULL;

    return 0;
}


int
ngx_rtc_srtp_unprotect_rtp(ngx_rtc_srtp_t *srtp, uint8_t *packet, int *nb)
{
    (void) srtp;
    (void) packet;
    (void) nb;

    return -1;
}


int
ngx_rtc_srtp_protect_rtcp(ngx_rtc_srtp_t *srtp, uint8_t *packet, int *nb)
{
    (void) srtp;
    (void) packet;
    (void) nb;

    return -1;
}


int
ngx_rtc_srtp_unprotect_rtcp(ngx_rtc_srtp_t *srtp, uint8_t *packet, int *nb)
{
    (void) srtp;
    (void) packet;
    (void) nb;

    return -1;
}


void
ngx_rtc_srtp_destroy(ngx_rtc_srtp_t *srtp)
{
    if (NULL == srtp) {
        return;
    }

    srtp->recv_ctx = NULL;
    srtp->send_ctx = NULL;
}
#endif

/* --- runtime symbols for ngx_rtc_shm.c -------------------------------------
 *
 * The registry locks a shm mutex and allocates from a slab pool. Real nginx
 * gets both from ngx_shmtx.c / ngx_slab.c, which this build does not link, so
 * they are supplied here.
 *
 * What differs between the two header worlds is only the STRUCT LAYOUT the
 * symbols operate on:
 *
 *   - real headers: ngx_shmtx_t / ngx_slab_pool_t are nginx's, so the
 *     implementations below use nginx's actual fields (mtx->lock, mtx->spin).
 *   - stub headers (the Windows cross build): nginx_stub's own structs, so the
 *     mutex is a pthread one.
 *
 * The slab stays malloc-backed in BOTH. That is the point of not linking
 * ngx_slab.c: a real slab hands out sub-allocations of one large zone, so the
 * sanitizer sees a single allocation and a leak or overflow inside it is
 * invisible. With malloc per object, LeakSanitizer and ASan keep per-object
 * granularity -- which is how the shm source leak and the stream close-path
 * use-after-free were caught.
 *
 * ngx_cycle stays NULL. Only ngx_rtc_publish_claim/release read it, and those
 * need a parsed config, so they are out of the host tests' scope -- this symbol
 * exists so the unit links. Real nginx declares it volatile; matching that is
 * required, not cosmetic.
 */

#ifdef NGX_RTC_REAL_NGINX_HEADERS
/* Production code reaches for ngx_cycle->log on allocation paths the host tests
 * do exercise (ngx_rtc_rtp_ring_reserve). NULL is not a usable default there:
 * under nginx's ngx_alloc() the argument is a real parameter, so building the
 * call dereferences ngx_cycle. The stub header's ngx_alloc() was a macro that
 * discarded the argument, which is why the same code survived before -- GCC
 * drops a discarded load, and the real header does not give it the chance.
 *
 * So the host build models one running worker: a cycle with a log, and a
 * conf_ctx that stays NULL because ngx_rtc_publish_claim/release are its only
 * readers and they want a parsed config a test run does not have. */
static ngx_cycle_t  ngx_rtc_host_cycle;
static ngx_log_t    ngx_rtc_host_log;

volatile ngx_cycle_t *ngx_cycle = &ngx_rtc_host_cycle;

static void __attribute__((constructor))
ngx_rtc_host_cycle_init(void)
{
    ngx_rtc_host_cycle.log = &ngx_rtc_host_log;
}

#else
ngx_cycle_t *ngx_cycle = NULL;
#endif


#ifdef NGX_RTC_REAL_NGINX_HEADERS

/* nginx's pgid/pid are globals from ngx_pid.c; ngx_shmtx_lock stamps the lock
 * with the owner. */
ngx_pid_t ngx_pid = 0;

ngx_int_t
ngx_shmtx_create(ngx_shmtx_t *mtx, ngx_shmtx_sh_t *addr, u_char *name)
{
    (void) name;

    if (NULL == mtx || NULL == addr) {
        return NGX_ERROR;
    }

    mtx->lock = &addr->lock;
#if (NGX_HAVE_POSIX_SEM)
    mtx->wait = &addr->wait;
#endif
    mtx->spin = 2048;

    return NGX_OK;
}


/* nginx's real lock first spins, then sleeps on a semaphore. There is no
 * competing process in a host test, and creating the semaphore would need
 * shared memory, so this spins with a yield. The critical sections the tests
 * exercise are a few instructions long. */
void
ngx_shmtx_lock(ngx_shmtx_t *mtx)
{
    for ( ;; ) {
        if (ngx_atomic_cmp_set(mtx->lock, 0, ngx_pid)) {
            return;
        }

        sched_yield();
    }
}


void
ngx_shmtx_unlock(ngx_shmtx_t *mtx)
{
    (void) ngx_atomic_cmp_set(mtx->lock, ngx_pid, 0);
}


ngx_uint_t
ngx_shmtx_trylock(ngx_shmtx_t *mtx)
{
    return (0 != ngx_atomic_cmp_set(mtx->lock, 0, ngx_pid));
}


/* malloc per object, not a sub-allocation of a zone -- see the header note. */
void *
ngx_slab_alloc_locked(ngx_slab_pool_t *pool, size_t size)
{
    (void) pool;

    return malloc(size);
}


void
ngx_slab_free_locked(ngx_slab_pool_t *pool, void *p)
{
    (void) pool;

    free(p);
}


/* nginx's ngx_log_error() macro tests log->log_level before calling here, so
 * the log handle must be a real (zeroed) ngx_log_t: tests set log_level to 0,
 * which is below every level, and this is then never reached. It exists for the
 * calls that pass an error anyway. */
void
ngx_log_error_core(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
                   const char *fmt, ...)
{
    (void) level;
    (void) log;
    (void) err;
    (void) fmt;
}


/* ngx_log_stderr() is what the module's config-time refusals call; there is no
 * stderr sink wired up in a test run, so it is dropped. */
void
ngx_log_stderr(ngx_err_t err, const char *fmt, ...)
{
    (void) err;
    (void) fmt;
}


/* nginx declares ngx_alloc/ngx_calloc/ngx_pcalloc as functions rather than
 * macros when the platform has a usable allocator, and defines them in
 * ngx_alloc.c / ngx_palloc.c, which this build does not link. */
void *
ngx_alloc(size_t size, ngx_log_t *log)
{
    (void) log;

    return malloc(size);
}


void *
ngx_calloc(size_t size, ngx_log_t *log)
{
    (void) log;

    return calloc(1, size);
}


void *
ngx_pcalloc(ngx_pool_t *pool, size_t size)
{
    (void) pool;

    return calloc(1, size);
}


/* ngx_event_add_timer() and ngx_rbtree_insert_value() are nginx's, from
 * ngx_event_timer.h and ngx_rbtree.c. The registry's own tree code lives in
 * this file, so only the generic insert callback is needed here. */
void
ngx_rbtree_insert_value(ngx_rbtree_node_t *temp, ngx_rbtree_node_t *node,
                        ngx_rbtree_node_t *sentinel)
{
    for ( ;; ) {
        if (node->key < temp->key) {
            if (temp->left == sentinel) {
                temp->left = node;
                break;
            }
            temp = temp->left;
            continue;
        }

        if (temp->right == sentinel) {
            temp->right = node;
            break;
        }
        temp = temp->right;
    }

    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


/* nginx's ngx_crc32_long() is an inline that reads this table, which
 * ngx_crc32.c fills in. Built at startup instead of pasted, from the standard
 * reflected CRC-32 polynomial nginx uses. */
uint32_t  ngx_crc32_table256[256];

static void __attribute__((constructor))
ngx_rtc_host_crc32_init(void)
{
    uint32_t  c;
    ngx_uint_t  n, k;

    for (n = 0; n < 256; n++) {
        c = (uint32_t) n;
        for (k = 0; k < 8; k++) {
            c = (0 != (c & 1)) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        }
        ngx_crc32_table256[n] = c;
    }
}


/* ngx_add_event / ngx_del_event are macros over this table. The host build has
 * no event loop, so registration is a no-op that succeeds -- which is what lets
 * the module's per-worker notify-fd setup run to completion. */
static ngx_int_t
ngx_rtc_host_event_add(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    (void) ev;
    (void) event;
    (void) flags;

    return NGX_OK;
}


static ngx_int_t
ngx_rtc_host_event_del(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    (void) ev;
    (void) event;
    (void) flags;

    return NGX_OK;
}


ngx_event_actions_t  ngx_event_actions = {
    ngx_rtc_host_event_add,
    ngx_rtc_host_event_del,
    NULL,                       /* enable */
    NULL,                       /* disable */
    NULL,                       /* add_conn */
    NULL,                       /* del_conn */
    NULL,                       /* notify */
    NULL,                       /* process_events */
    NULL,                       /* init */
    NULL                        /* done */
};

#else  /* stub headers (Windows cross build) */

ngx_int_t
ngx_shmtx_create(ngx_shmtx_t *mtx, ngx_shmtx_sh_t *sh, u_char *name)
{
    (void) sh;
    (void) name;

    return (0 == pthread_mutex_init(&mtx->host_lock, NULL))
           ? NGX_OK : NGX_ERROR;
}

#endif


/* The registry's publish claim/release read the per-cycle conf, whose definition
 * moved to ngx_rtc_core_module.c along with the rest of the config layer. The
 * host tests never install a cycle (that needs a parsed config), so this returns
 * NULL -- the same thing a worker sees when rtc_zone is not configured. Only the
 * two entry points that read it are out of scope here; every other registry
 * function takes the ctx directly. */
ngx_rtc_core_conf_t *
ngx_rtc_core_get_conf(ngx_cycle_t *cycle)
{
    (void) cycle;
    return NULL;
}


/* ============================================================================
 * Stream-framework dependencies of ngx_rtc_stream_module.c.
 *
 * Two symbols the media plane calls live outside this build, stubbed at the
 * narrowest boundary. Neither is a lifetime path, so nothing the stream-module
 * tests assert depends on them:
 *
 *   - ngx_rtc_broadcast_rtp is defined in ngx_rtmp_rtc_bridge_module.c, an
 *     nginx module the host build does not compile. It is the RTP egress path.
 *   - ngx_conf_set_msec_slot is an nginx config parser. The host tests never
 *     parse a config; the stream module's command table only needs the symbol
 *     to exist so the object links.
 *
 * SRTP, the third dependency, is stubbed above inside the ngx_rtc_srtp guard.
 * ============================================================================ */

void
ngx_rtc_broadcast_rtp(ngx_rtc_source_t *src, const uint8_t *rtp, uint32_t len,
                      uint8_t is_video, uint8_t is_gop_start)
{
    (void) src;
    (void) rtp;
    (void) len;
    (void) is_video;
    (void) is_gop_start;
}


char *
ngx_conf_set_msec_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    (void) cf;
    (void) cmd;
    (void) conf;

    return NGX_CONF_OK;
}


/* ============================================================================
 * Stream framework + event stubs.
 *
 * ngx_rtc_stream_module.c is the UDP media plane; the parts worth covering on
 * the host are the datagram state machine and the session lifetime rules, so
 * it is compiled here and needs the handful of nginx entry points it calls.
 * ============================================================================ */

#ifndef NGX_RTC_REAL_NGINX_HEADERS

/* ngx_rtc_stream_rtc() resolves the stream core module to install its handler.
 * nginx defines this object in ngx_stream_module.c, which this build does not
 * link. The real ngx_stream_core_module brings its whole conf layout with it;
 * only ctx_index is ever read (through ngx_stream_conf_get_module_srv_conf), so
 * a placeholder is enough. */
ngx_module_t ngx_stream_core_module = {
    NGX_MODULE_V1,
    NULL,                       /* ctx */
    NULL,                       /* commands */
    NGX_STREAM_MODULE,          /* module type */
    NULL,                       /* init master */
    NULL,                       /* init module */
    NULL,                       /* init process */
    NULL,                       /* init thread */
    NULL,                       /* exit thread */
    NULL,                       /* exit process */
    NULL,                       /* exit master */
    NGX_MODULE_V1_PADDING
};


/* The posted-event queue the module passes to ngx_post_event. */
ngx_event_t ngx_posted_events;


/* Ending the stream session is what returns the UDP connection slot to nginx --
 * the module's close path exists partly to cause it -- so the host tests assert
 * on it. Real nginx does it by destroying the pool; here it drops the session's
 * connection back-pointer, which is the observable both header worlds share
 * (the real-header branch sets exactly the same field). */
void
ngx_stream_finalize_session(ngx_stream_session_t *s, ngx_uint_t rc)
{
    (void) rc;

    if (NULL == s) {
        return;
    }

    if (NULL != s->connection) {
        s->connection->data = NULL;
    }
    s->connection = NULL;
}


/* Single process playing the worker role; see the note in ngx_core.h. */
ngx_int_t  ngx_process = NGX_PROCESS_WORKER;
ngx_uint_t ngx_worker = 0;


void
ngx_del_timer(ngx_event_t *ev)
{
    if (NULL == ev) {
        return;
    }

    ev->timer_set = 0;
}


/* No event loop: registration is a no-op that succeeds, so the module's
 * per-worker notify-fd setup runs to completion instead of bailing out. */
ngx_int_t
ngx_add_event(ngx_event_t *ev, ngx_uint_t event, ngx_uint_t flags)
{
    (void) ev;
    (void) event;
    (void) flags;

    return NGX_OK;
}


/* No event loop on the host: timers are recorded, never fired. A test that
 * wants to see the reaper cadence reads ev->timer_ms back. */
void
ngx_add_timer(ngx_event_t *ev, uint64_t ms)
{
    if (NULL == ev) {
        return;
    }

    ev->timer_set = 1;
    ev->timer_ms = ms;
}


/* Handlers run inline rather than "later". Every host test inspects state
 * immediately after the call that posted the event, so running it here is what
 * makes the effect observable. The production reason for the deferral -- not
 * freeing a connection under ngx_event_recvmsg's stack frame -- does not apply
 * to a test that calls the close path directly. */
void
ngx_post_event(ngx_event_t *ev, ngx_event_t *queue)
{
    (void) queue;

    if (NULL == ev) {
        return;
    }

    ev->posted = 1;
    if (NULL != ev->handler) {
        ev->handler(ev);
    }
}

#else  /* NGX_RTC_REAL_NGINX_HEADERS */

/* Under the real headers every symbol above is nginx's, and two of them are the
 * reason this half is not shared:
 *
 *   - ngx_post_event(ev, q) is a MACRO over ngx_queue_insert_tail, so there is
 *     no function to define. The handler runs later, when something drains
 *     ngx_posted_events; the tests call ngx_rtc_host_drain_posted() below.
 *   - ngx_add_timer(ev, ms) is a macro over the inline ngx_event_add_timer(),
 *     which inserts into ngx_event_timer_rbtree. That tree is a global nginx
 *     defines in ngx_event_timer.c, so it is defined and initialised here.
 *
 * Both are used as nginx uses them, not reimplemented, so a host test sees the
 * same event bookkeeping production does.
 */
ngx_queue_t ngx_posted_events;

ngx_rbtree_t ngx_event_timer_rbtree;
static ngx_rbtree_node_t ngx_event_timer_sentinel;

ngx_uint_t ngx_process = NGX_PROCESS_WORKER;
ngx_uint_t ngx_worker = 0;


/* nginx initialises both in ngx_event_timer_init() / ngx_event_posted.h, from
 * ngx_event.c's ngx_event_module_init(), which this build does not link. A
 * constructor runs before any test. */
static void __attribute__((constructor))
ngx_rtc_host_event_init(void)
{
    ngx_rbtree_init(&ngx_event_timer_rbtree, &ngx_event_timer_sentinel,
                    ngx_rbtree_insert_value);
    ngx_queue_init(&ngx_posted_events);
}


/* Drain the posted-event queue the way ngx_event_process_posted() does. The
 * production code posts the stream session's finalize so it does not run on the
 * receive path's stack; a test that wants to observe that finalize calls this
 * instead of relying on an event loop. */
void
ngx_rtc_host_drain_posted(void)
{
    ngx_queue_t *q;
    ngx_event_t *ev;

    while (!ngx_queue_empty(&ngx_posted_events)) {
        q = ngx_queue_head(&ngx_posted_events);
        ngx_queue_remove(q);

        ev = ngx_queue_data(q, ngx_event_t, queue);
        ev->posted = 0;

        if (NULL != ev->handler) {
            ev->handler(ev);
        }
    }
}


/* nginx's ngx_stream_finalize_session() destroys the connection pool, which is
 * what returns the ngx_connection_t slot. There is no pool here, so this does
 * the part the module's close path exists to cause: it drops the session's
 * connection back-pointer, which is what the tests assert on. */
void
ngx_stream_finalize_session(ngx_stream_session_t *s, ngx_uint_t rc)
{
    (void) rc;

    if (NULL == s) {
        return;
    }

    if (NULL != s->connection) {
        s->connection->data = NULL;
    }
    s->connection = NULL;
}


/* ngx_rtc_stream_rtc() resolves the stream core module to install its handler.
 * nginx defines the real object in ngx_stream_module.c; only ctx_index is read. */
ngx_module_t ngx_stream_core_module = {
    NGX_MODULE_V1,
    NULL,                       /* ctx */
    NULL,                       /* commands */
    NGX_STREAM_MODULE,          /* module type */
    NULL,                       /* init master */
    NULL,                       /* init module */
    NULL,                       /* init process */
    NULL,                       /* init thread */
    NULL,                       /* exit thread */
    NULL,                       /* exit process */
    NULL,                       /* exit master */
    NGX_MODULE_V1_PADDING
};

#endif /* NGX_RTC_REAL_NGINX_HEADERS */
