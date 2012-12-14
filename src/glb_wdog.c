/*
 * Copyright (C) 2012 Codership Oy <info@codership.com>
 *
 * $Id$
 */

#undef NDEBUG // for now

#include "glb_wdog.h"
#include "glb_wdog_backend.h"
#include "glb_dst.h"
#include "glb_log.h"
#include "glb_socket.h"
#include "glb_misc.h"

#include <math.h>     // fabs()
#include <assert.h>
#include <errno.h>
#include <time.h>     // nanosleep()
#include <sys/time.h> // gettimeofday()

typedef struct wdog_dst
{
    bool               explicit; //! was added explicitly, never remove
//    bool               joined;
    glb_dst_t          dst;
    double             weight;
//    glb_wdog_check_t   current; // is it needed?
    glb_wdog_check_t   pending;
    bool               memb_changed;
    glb_backend_ctx_t* ctx;      //! backend thread context
} wdog_dst_t;

struct glb_wdog
{
    const glb_cnf_t*      cnf;
    glb_router_t*         router;
    glb_backend_t*        backend;
    glb_backend_thread_t  backend_thread;
    glb_backend_destroy_t backend_destroy;
    pthread_t             thd;
    pthread_mutex_t       lock;
    pthread_cond_t        cond;
    bool                  quit;
    bool                  join;
    long long             interval; // nsec
    struct timespec       next;
    int                   n_dst;
    wdog_dst_t*           dst;
};

static glb_backend_ctx_t*
wdog_backend_ctx_create (glb_backend_t* backend, const glb_dst_t* const dst)
{
    char* addr = strdup (glb_socket_addr_to_string (&dst->addr));

    if (addr)
    {
        char* colon = strchr (addr, ':');

        if (colon) {
            *colon = '\0';
            long port = strtol (colon + 1, NULL, 10);
            assert (port > 0 && port <= 65535);

            glb_backend_ctx_t* ret = calloc (1, sizeof(*ret));

            if (ret) {
                glb_log_info("Created context for %s:%ld", addr, port);
                ret->backend = backend;
                pthread_mutex_init (&ret->lock, NULL);
                pthread_cond_init  (&ret->cond, NULL);
                ret->addr = addr;
                ret->port = port;
                return ret;
            }
        }

        free (addr);
    }

    return NULL;
}

static void
wdog_backend_ctx_destroy (glb_backend_ctx_t* ctx)
{
    free (ctx->addr);
    free ((void*)ctx->result.others);
    pthread_mutex_destroy (&ctx->lock);
    pthread_cond_destroy  (&ctx->cond);
    free (ctx);
}

int
glb_wdog_change_dst (glb_wdog_t*      const wdog,
                     const glb_dst_t* const dst,
                     bool             const explicit)
{
    int         i;
    void*       tmp;
    wdog_dst_t* d = NULL;

    GLB_MUTEX_LOCK (&wdog->lock);

    // try to find destination in the list
    for (i = 0; i < wdog->n_dst; i++) {
        if (glb_dst_is_equal(&wdog->dst[i].dst, dst)) {
            d = &wdog->dst[i];
            break;
        }
    }

    // sanity check
    if (!d && dst->weight < 0) {
        GLB_MUTEX_UNLOCK (&wdog->lock);
#ifdef GLBD
        char tmp[256];
        glb_dst_print (tmp, sizeof(tmp), dst);
        glb_log_warn ("Command to remove inexisting destination: %s", tmp);
#endif
        return -EADDRNOTAVAIL;
    }

    if (!d) { // add destination

        assert (i == wdog->n_dst);

        glb_backend_ctx_t* ctx = wdog_backend_ctx_create (wdog->backend, dst);

        if (!ctx) {
            i = -ENOMEM;
        }
        else {
            tmp = realloc (wdog->dst, (wdog->n_dst + 1) * sizeof(wdog_dst_t));

            if (!tmp) {
                wdog_backend_ctx_destroy (ctx);
                i = -ENOMEM;
            }
            else {
                bool success = false;
                wdog->dst = tmp;

                GLB_MUTEX_LOCK (&ctx->lock);
                {
                    pthread_create (&ctx->id, NULL, wdog->backend_thread, ctx);
                    pthread_cond_wait (&ctx->cond, &ctx->lock);
                    success = !ctx->join;
                }
                GLB_MUTEX_UNLOCK (&ctx->lock);

                if (success)
                {
                    d = wdog->dst + wdog->n_dst;
                    wdog->n_dst++;
                    memset (d, 0, sizeof(*d));
                    d->explicit = explicit;
                    d->dst      = *dst;
                    d->ctx      = ctx;
                }
                else {
                    i = -ctx->errn;
                    pthread_join (ctx->id, NULL);
                    wdog_backend_ctx_destroy(ctx);
                }
            }
        }
    }
    else if (dst->weight < 0) // remove destination from the list
    {
        assert (d);
        assert (i >= 0 && i < wdog->n_dst);

        if (explicit || !d->explicit)
        {
            GLB_MUTEX_LOCK (&d->ctx->lock);
            d->ctx->quit = true;
            pthread_cond_signal (&d->ctx->cond);
            GLB_MUTEX_UNLOCK (&d->ctx->lock);
            /* thread will be joined context will be cleaned up later */
#if REMOVE
            if ((i + 1) < router->n_dst) {
            // it is not the last, copy the last to close the gap
            router_dst_t* next = d + 1;
            size_t len = (router->n_dst - i - 1)*sizeof(router_dst_t);
            memmove (d, next, len);
            }
            tmp = realloc (router->dst,
                           (router->n_dst - 1) * sizeof(router_dst_t));

            if (!tmp && (router->n_dst > 1)) {
                i = -ENOMEM;
            }
            else {
                router->dst = tmp;
                router->n_dst--;
                router->rrb_next = router->rrb_next % router->n_dst;
            }
#endif /* REMOVE */
        }
        else
        {
            // no right to remove, just mark it inaccessible
            d->dst.weight = -1.0;
        }
    }
    else if (d->dst.weight != dst->weight) {
        d->dst.weight = dst->weight;
    }

    GLB_MUTEX_UNLOCK (&wdog->lock);

    return i;
}

static void*
dummy_backend_thread (void* arg)
{
    glb_backend_ctx_t* ctx = arg;
    GLB_MUTEX_LOCK(&ctx->lock);
    pthread_cond_signal (&ctx->cond);
    while (!ctx->quit) {
        pthread_cond_wait (&ctx->cond, &ctx->lock);
    }
    ctx->join = true;
    GLB_MUTEX_UNLOCK(&ctx->lock);
    return NULL;
}

static void
wdog_backend_factory (const glb_cnf_t*       cnf,
                      glb_backend_t**        backend,
                      glb_backend_thread_t*  thread,
                      glb_backend_destroy_t* destroy)
{
    *backend = NULL;
    *thread  = NULL;
    *destroy = NULL;

    assert (cnf->watchdog);

    char* spec = strchr(cnf->watchdog, ':'); // seek for first colon

    if (spec)
    {
        *spec = '\0'; // separate watchdog id string
        spec++;
    }

    if (strlen (cnf->watchdog) == 0)
    {
        *thread = dummy_backend_thread;
    }
    else if (!strcmp (cnf->watchdog, "script"))
    {
        glb_log_error("%s watchdog not implemented.", cnf->watchdog);
//        *backend = glb_script_create (spec, thread, destroy);
    }
    else
    {
        glb_log_error("%s watchdog not implemented.", cnf->watchdog);
    }
}

static inline int
wdog_copy_result (wdog_dst_t* d, double* max_lat)
{
    double old_lat    = d->pending.latency;
    char*  others     = d->pending.others;
    size_t others_len = d->pending.others_len;

    GLB_MUTEX_LOCK (&d->ctx->lock);
    {
        glb_wdog_check_t* res = &d->ctx->result;

        d->pending = *res;
        res->ready = false;

        // restore original buffer
        d->pending.others     = others;
        d->pending.others_len = others_len;

        if (d->pending.ready) {
            if (GLB_DST_NOTFOUND == d->pending.state) {
                if (!d->explicit) { // if selfdiscovered - schedule for cleanup
                    d->ctx->quit = true;
                    pthread_cond_signal (&d->ctx->cond);
                }
            }
            else { // remote destination is live, handle others string
                bool changed_length = false;
                if (others_len < res->others_len ||
                    others_len > (res->others_len * 2)) {
                    // buffer size is too different, reallocate
                    d->pending.others = realloc (others, res->others_len);
                    if (!d->pending.others) {
                        // this is pretty much fatal, but we'll try
                        free (others);
                        d->pending.others_len = 0;
                    }
                    else {
                        changed_length = true;
                        d->pending.others_len = res->others_len;
                    }
                }

                if (d->pending.others_len >= res->others_len &&
                    (changed_length || strcmp(d->pneding.others, res->others))){
                    d->memb_changed = true;
                    strcpy (d->pending.others, res->others);
                }
            }
        }
    }
    GLB_MUTEX_UNLOCK (&d->ctx->lock);

    if (d->pending.ready && GLB_DST_READY == d->pending.state) {
        // smooth latency measurement with the previous one
        d->pending.latency = (d->pending.latency + old_lat) / 2.0;
        if (*max_lat < d->pending.latency) *max_lat = d->pending.latency;
    }
    else {
        // preserve previously measured latency
        d->pending.latency = old_lat;
    }

    return 0;
}

// returns latency adjusted weight
static inline double
wdog_result_weight (wdog_dst_t* const d, double const max_lat)
{
    assert (d->pending.ready); // this must be called only for fresh data

//    d->current.state = d->pending.state;

    switch (d->pending.state)
    {
    case GLB_DST_NOTFOUND:
    case GLB_DST_NOTREADY:
        return -1.0;
    case GLB_DST_AVOID:
        return 0.0;
    case GLB_DST_READY:
        if (max_lat > 0) return d->dst.weight * max_lat / d->pending.latency;
        return d->dst.weight;
    }

    return 0.0;
}

static void
wdog_dst_free (wdog_dst_t* d)
{
    wdog_backend_ctx_destroy (d->ctx);
    free (d->pending.others);
}

// collects and processes results, returns the number of results collected
static int
wdog_collect_results (glb_wdog_t* const wdog)
{
    double max_lat = 0.0;
    int results = 0;

    int i;
    for (i = 0; i < wdog->n_dst; i++)
    {
        wdog_copy_result (&wdog->dst[i], &max_lat);
    }

    for (i = wdog->n_dst - 1; i >= 0; i--) // reverse order for ease of cleanup
    {
        wdog_dst_t* d = &wdog->dst[i];
        double new_weight;

        if (d->ctx->join) {
            pthread_join (d->ctx->id, NULL);
//            d->joined = true;
            if (i + 1 < wdog->n_dst) {
                // not the last in the list, copy the last one over this
            }
            continue;
        }

        if (d->pending.ready) {
            results++;
            new_weight = wdog_result_weight (d, max_lat);
        }
        else {
            // have heard nothing from the backend thread, put dest on hold
//            d->current.state = GLB_DST_AVOID;
            new_weight = 0.0;
        }

        static double const WEIGHT_TOLERANCE = 0.1; // 10%
        if (new_weight != d->weight &&
            (new_weight <= 0.0 ||
             fabs(d->weight/new_weight - 1.0) > WEIGHT_TOLERANCE)) {
            glb_dst_t dst = d->dst;
            dst.weight = new_weight;
            if (!glb_router_change_dst (wdog->router, &dst)) {
                d->weight = new_weight;
            }
//            d->current.state = d->pending.state;
        }
    }

    return results;
}

static inline void
timespec_add (struct timespec* t, long long d)
{
    d += t->tv_nsec;
    t->tv_sec += d / 1000000000;
    t->tv_nsec = d % 1000000000;
}

static void*
wdog_main_loop (void* arg)
{
    glb_wdog_t* wdog = arg;

    GLB_MUTEX_LOCK(&wdog->lock);

    if (wdog->n_dst > 0) {
        // since we're just starting and we have non-empty destination list,
        // try to get at least one destination confirmed
        int n = wdog_collect_results (wdog);
        int i = 10;
        while (!n && i--) {
            struct timespec t = { 0, 100000000 }; // 0.1 sec
            nanosleep (&t, NULL);
            n = wdog_collect_results (wdog);
        }
    }

    struct timeval now;
    gettimeofday (&now, NULL);
    wdog->next.tv_sec  = now.tv_sec;
    wdog->next.tv_nsec = now.tv_usec * 1000;

    pthread_cond_signal (&wdog->cond);

    while (!wdog->quit) {
        timespec_add (&wdog->next, wdog->interval);
        int err;
        do {
            err = pthread_cond_timedwait (&wdog->cond, &wdog->lock,
                                          &wdog->next);
        } while (err != ETIMEDOUT && !wdog->quit);

        if (wdog->quit) break;

        wdog_collect_results (wdog);
    }
    wdog->join = true;
    GLB_MUTEX_UNLOCK(&wdog->lock);

    return NULL;
}

static void
wdog_dst_cleanup (glb_wdog_t* wdog)
{
    int i;

    // tell all backend threads to quit
    for (i = 0; i < wdog->n_dst; i++)
    {
        wdog_dst_t* d = &wdog->dst[i];

        GLB_MUTEX_LOCK (&d->ctx->lock);
        if (!d->ctx->quit)
        {
            d->ctx->quit = true;
            pthread_cond_signal (&d->ctx->cond);
        }
        GLB_MUTEX_UNLOCK (&d->ctx->lock);
    }

    // join the threads and free contexts
    for (i = 0; i < wdog->n_dst; i++)
    {
        wdog_dst_t* d = &wdog->dst[i];
        pthread_join (d->ctx->id, NULL);
        wdog_backend_ctx_destroy (d->ctx);
    }
}

glb_wdog_t*
glb_wdog_create (const glb_cnf_t* cnf, glb_router_t* router)
{
    assert (cnf->watchdog);

    glb_wdog_t* ret = calloc (1, sizeof(*ret));

    if (ret)
    {
        ret->cnf = cnf;
        wdog_backend_factory (cnf,
                              &ret->backend,
                              &ret->backend_thread,
                              &ret->backend_destroy);

        if (!ret->backend_thread)
        {
            free(ret);
            return NULL;
        }

        assert (ret->backend || !ret->backend_destroy);
        assert (ret->backend_destroy || !ret->backend);

        pthread_mutex_init (&ret->lock, NULL);
        pthread_cond_init  (&ret->cond, NULL);

        ret->interval = cnf->interval * 1.5;

        int i;
        for (i = 0; i < cnf->n_dst; i++) {
            if (glb_wdog_change_dst(ret, &cnf->dst[i], true) < 0) {
                wdog_dst_cleanup (ret);
                pthread_cond_destroy  (&ret->cond);
                pthread_mutex_destroy (&ret->lock);
                free (ret);
                return NULL;
            }
        }

        assert (ret->n_dst == cnf->n_dst);

        GLB_MUTEX_LOCK (&ret->lock);
        pthread_create (&ret->thd, NULL, wdog_main_loop, ret);
        pthread_cond_wait (&ret->cond, &ret->lock);
        GLB_MUTEX_UNLOCK (&ret->lock);
    }

    return ret;
}

void
glb_wdog_destroy(glb_wdog_t* wdog)
{
    GLB_MUTEX_LOCK (&wdog->lock);
    wdog->quit = true;
    pthread_cond_signal (&wdog->cond);
    pthread_cond_wait (&wdog->cond, &wdog->lock);
    wdog_dst_cleanup (wdog);
    pthread_cond_destroy  (&wdog->cond);
    GLB_MUTEX_UNLOCK (&wdog->lock);
    pthread_mutex_destroy (&wdog->lock);
    free (wdog->dst);
    free (wdog);
}


