/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <sys/time.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include <errno.h>
#include <aos/aos.h>
#include <aos/network.h>

#include "yloop.h"

#define TAG "yloop"

typedef struct {
    int              sock;
    void            *private_data;
    aos_poll_call_t  cb;
} yloop_sock_t;

typedef struct yloop_timeout_s {
    dlist_t          next;
    long long        timeout_ms;
    void            *private_data;
    aos_call_t       cb;
    int              ms;
} yloop_timeout_t;

typedef struct {
    dlist_t          timeouts; /* 定时事件升序双链表 */
    struct pollfd   *pollfds;
    yloop_sock_t    *readers;
    int              eventfd; /* /dev/event的fd */
    uint8_t          max_sock; /* 最大的fd */
    uint8_t          reader_count; /* 使用了yloop的task总数 */
    bool             pending_terminate;
    bool             terminate; /* 释放终止yloop */
} yloop_ctx_t;

static yloop_ctx_t    *g_main_ctx;
static aos_task_key_t  g_loop_key;

static inline void _set_context(yloop_ctx_t *ctx)
{
    aos_task_setspecific(g_loop_key, ctx);
}

static inline yloop_ctx_t *_get_context(void)
{ /* 以g_loop_key为索引，获取ktask_t->user_info[g_loop_key] */
    return aos_task_getspecific(g_loop_key);
}

static inline yloop_ctx_t *get_context(void)
{
    yloop_ctx_t *ctx = _get_context();
    if (!ctx) {
        _set_context(g_main_ctx);
        return g_main_ctx;
    }
    return ctx;
}

void aos_loop_set_eventfd(int fd)
{ /* 设置yloop的eventfd */
    yloop_ctx_t *ctx = get_context();
    ctx->eventfd = fd;
}

int aos_loop_get_eventfd(void *loop)
{/* 获取yloop的eventfd */
    yloop_ctx_t *ctx = loop ? loop : get_context();
    return ctx->eventfd;
}

aos_loop_t aos_current_loop(void)
{
    return get_context();
}
AOS_EXPORT(aos_loop_t, aos_current_loop, void);

aos_loop_t aos_loop_init(void)
{ /* 获取yloop_ctx_t，如果之前没有，则创建(malloc)一个 */
    yloop_ctx_t *ctx = _get_context(); /* 存放在k_task->user_info中 */

    if (!g_main_ctx) { /* 保证系统中只有一个g_loop_key */
        aos_task_key_create(&g_loop_key);
    } else if (ctx) {
        LOGE(TAG, "yloop already inited");
        return ctx;
    }

    ctx = aos_zalloc(sizeof(*g_main_ctx));
    if (!g_main_ctx) {
        g_main_ctx = ctx;
    }

    dlist_init(&ctx->timeouts);
    ctx->eventfd = -1;
    _set_context(ctx); /* 把yloop_ctx_t放到ktask_t->user_info[g_loop_key] */

    aos_event_service_init(); /* 把eventfd(/dev/event)加入yloop */

    return ctx;
}
AOS_EXPORT(aos_loop_t, aos_loop_init, void);

int aos_poll_read_fd(int sock, aos_poll_call_t cb, void *private_data)
{ /* 把sock放到yloop->readers[]中。重复sock怎么办? */
    yloop_ctx_t *ctx = get_context();
    if (sock  < 0) {
        return -EINVAL;
    }

    yloop_sock_t *new_sock;
    struct pollfd *new_loop_pollfds;
    int cnt = ctx->reader_count + 1;

    new_sock = aos_malloc(cnt * sizeof(yloop_sock_t));
    new_loop_pollfds = aos_malloc(cnt * sizeof(struct pollfd));

    if (new_sock == NULL || new_loop_pollfds == NULL) {
        LOGE(TAG, "out of memory");
        aos_free(new_sock);
        aos_free(new_loop_pollfds);
        return -ENOMEM;
    }

    int status = aos_fcntl(sock, F_GETFL, 0);
    aos_fcntl(sock, F_SETFL, status | O_NONBLOCK); /* 把sockfd设为非阻塞 */

    ctx->reader_count++;

    memcpy(new_sock, ctx->readers, (cnt - 1) * sizeof(yloop_sock_t)); /* 把sockfd加入yloop_t->readers[] */
    aos_free(ctx->readers);
    ctx->readers = new_sock;

    memcpy(new_loop_pollfds, ctx->pollfds, (cnt - 1) * sizeof(struct pollfd));
    aos_free(ctx->pollfds);
    ctx->pollfds = new_loop_pollfds;

    new_sock += cnt - 1;
    new_sock->sock = sock; /* 设置yloop_t->readers[yloop_t-> reader_count-1]->sock*/
    new_sock->private_data = private_data; /* 设置yloop_t->readers[yloop_t-> reader_count-1]参数 */
    new_sock->cb = cb; /* 设置yloop_t->readers[yloop_t-> reader_count-1]回调函数 */

    if (sock > ctx->max_sock) { /* 更新整个yloop_t最大fd */
        ctx->max_sock = sock;
    }

    return 0;
}
AOS_EXPORT(int, aos_poll_read_fd, int, aos_poll_call_t, void *);

void aos_cancel_poll_read_fd(int sock, aos_poll_call_t action, void *param)
{
    yloop_ctx_t *ctx = get_context();
    if (ctx->readers == NULL || ctx->reader_count == 0) {
        return;
    }

    int i;
    for (i = 0; i < ctx->reader_count; i++) {
        if (ctx->readers[i].sock == sock) {
            break; /*后面还有相同的sock怎么办? */
        }
    }

    if (i == ctx->reader_count) {
        return;
    }

    if (i != ctx->reader_count - 1) {
        memmove(&ctx->readers[i], &ctx->readers[i + 1],
                (ctx->reader_count - i - 1) *
                sizeof(yloop_sock_t));
    }

    ctx->reader_count--;
}
AOS_EXPORT(void, aos_cancel_poll_read_fd, int, aos_poll_call_t, void *);

int aos_post_delayed_action(int ms, aos_call_t action, void *param)
{ /* 添加一个延时定时器 */
    if (action == NULL) {
        return -EINVAL;
    }

    yloop_ctx_t *ctx = get_context();
    yloop_timeout_t *timeout = aos_malloc(sizeof(*timeout));
    if (timeout == NULL) {
        return -ENOMEM;
    }

    timeout->timeout_ms = aos_now_ms() + ms; /* 定时器超时时间 */
    timeout->private_data = param; /* 定时器回调参数 */
    timeout->cb = action; /* 定时器回调函数 */
    timeout->ms = ms; /* 延时时间 */

    yloop_timeout_t *tmp;

    dlist_for_each_entry(&ctx->timeouts, tmp, yloop_timeout_t, next) {
        if (timeout->timeout_ms < tmp->timeout_ms) {
            break;
        }
    } /* 按超时时间升序排列的双链表 */

    dlist_add_tail(&timeout->next, &tmp->next); /* 把定时器加入升序链表 */

    return 0;
}
AOS_EXPORT(int, aos_post_delayed_action, int, aos_call_t, void *);

void aos_cancel_delayed_action(int ms, aos_call_t cb, void *private_data)
{ /* 删除延时定时器 */
    yloop_ctx_t *ctx = get_context();
    yloop_timeout_t *tmp;
    /* O(n)能否优化成O(1)? */
    dlist_for_each_entry(&ctx->timeouts, tmp, yloop_timeout_t, next) {
        if (ms != -1 && tmp->ms != ms) {
            continue;
        }

        if (tmp->cb != cb) {
            continue;
        }

        if (tmp->private_data != private_data) {
            continue;
        }

        dlist_del(&tmp->next);
        aos_free(tmp);
        return;
    }
}
AOS_EXPORT(void, aos_cancel_delayed_action, int, aos_call_t, void *);

void aos_loop_run(void)
{
    yloop_ctx_t *ctx = get_context();

    while (!ctx->terminate &&
           (!dlist_empty(&ctx->timeouts) || ctx->reader_count > 0)) { /* 存在定时器事件或者IO事件 */
        int delayed_ms = -1;
        int readers = ctx->reader_count;
        int i;

        if (!dlist_empty(&ctx->timeouts)) {
            yloop_timeout_t *tmo = dlist_first_entry(&ctx->timeouts, yloop_timeout_t, next);
            long long now = aos_now_ms();

            if (now < tmo->timeout_ms) {
                delayed_ms = tmo->timeout_ms - now;
            } else {
                delayed_ms = 0;
            } /* 找到最近一个定时器超时相对时间 */
        }

        for (i = 0; i < readers; i++) {
            ctx->pollfds[i].fd = ctx->readers[i].sock;
            ctx->pollfds[i].events = POLLIN;
        }

        int res = aos_poll(ctx->pollfds, readers, delayed_ms); /* IO多路复用 */

        if (res < 0 && errno != EINTR) {
            LOGE(TAG, "aos_poll");
            return;
        }

        /* check if some registered timeouts have occurred */ /* 再次执行到此，说明超时或事件发生 */
        if (!dlist_empty(&ctx->timeouts)) {
            yloop_timeout_t *tmo = dlist_first_entry(&ctx->timeouts, yloop_timeout_t, next);
            long long now = aos_now_ms();

            if (now >= tmo->timeout_ms) {
                dlist_del(&tmo->next); /* 删除准备执行的定时器 */
                tmo->cb(tmo->private_data); /* 执行每一个定时器事件的回调函数 */
                aos_free(tmo);
            }
        }

        if (res <= 0) {
            continue;
        }

        for (i = 0; i < readers; i++) { /* 执行每一个IO事件的回调函数 */
            if (ctx->pollfds[i].revents & POLLIN) {
                ctx->readers[i].cb(
                    ctx->readers[i].sock,
                    ctx->readers[i].private_data);
            }
        }
    }

    ctx->terminate = 0;
}
AOS_EXPORT(void, aos_loop_run, void);

void aos_loop_exit(void)
{
    yloop_ctx_t *ctx = get_context();
    ctx->terminate = 1;
}
AOS_EXPORT(void, aos_loop_exit, void);

void aos_loop_destroy(void)
{
    yloop_ctx_t *ctx = _get_context();

    if (ctx == NULL) {
        return;
    }

    aos_event_service_deinit(ctx->eventfd);

    while (!dlist_empty(&ctx->timeouts)) {
        yloop_timeout_t *timeout = dlist_first_entry(&ctx->timeouts, yloop_timeout_t,
                                                     next);
        dlist_del(&timeout->next);
        aos_free(timeout);
    }

    aos_free(ctx->readers);
    aos_free(ctx->pollfds);

    _set_context(NULL);
    if (ctx == g_main_ctx) {
        g_main_ctx = NULL;
    }
    aos_free(ctx);
}
AOS_EXPORT(void, aos_loop_destroy, void);

