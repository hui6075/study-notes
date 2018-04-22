/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <k_api.h>

#if (RHINO_CONFIG_TIMER > 0)
static void timer_list_pri_insert(klist_t *head, ktimer_t *timer)
{
    sys_time_t val;
    klist_t   *q;
    klist_t   *start;
    klist_t   *end;
    ktimer_t  *task_iter_temp;

    start = end = head;
    val = timer->remain;

    for (q = start->next; q != end; q = q->next) {
        task_iter_temp = krhino_list_entry(q, ktimer_t, timer_list);
        if ((task_iter_temp->match - g_timer_count) > val) {
            break;
        }
    }

    klist_insert(q, &timer->timer_list);
}

static void timer_list_rm(ktimer_t *timer)
{
    klist_t *head;

    head = timer->to_head;
    if (head != NULL) {
        klist_rm(&timer->timer_list);
        timer->to_head = NULL;
    }
}

static kstat_t timer_create(ktimer_t *timer, const name_t *name, timer_cb_t cb,
                            sys_time_t first, sys_time_t round, void *arg, uint8_t auto_run,
                            uint8_t mm_alloc_flag)
{
    kstat_t err = RHINO_SUCCESS;

    NULL_PARA_CHK(timer);
    NULL_PARA_CHK(name);
    NULL_PARA_CHK(cb);

    if (first == 0u) {
        return RHINO_INV_PARAM;
    }

    if (first >= MAX_TIMER_TICKS) {
        return RHINO_INV_PARAM;
    }

    if (round >= MAX_TIMER_TICKS) {
        return RHINO_INV_PARAM;
    }

    timer->name          = name;
    timer->cb            = cb; /* 回调函数 */
    timer->init_count    = first; /* 第一次时间 */
    timer->round_ticks   = round; /* 任务执行周期, 0表示只执行一次 */
    timer->remain        = 0u; /* 定时器剩余时间 */
    timer->match         = 0u;
    timer->timer_state   = TIMER_DEACTIVE;
    timer->to_head       = NULL;
    timer->mm_alloc_flag = mm_alloc_flag;
    timer->timer_cb_arg  = arg;
    klist_init(&timer->timer_list);

    timer->obj_type = RHINO_TIMER_OBJ_TYPE;

    if (auto_run > 0u) { /* 自带开始的话 */
        err = krhino_timer_start(timer);
    }

    TRACE_TIMER_CREATE(krhino_cur_task_get(), timer);

    return err;
}

kstat_t krhino_timer_create(ktimer_t *timer, const name_t *name, timer_cb_t cb,
                            sys_time_t first, sys_time_t round, void *arg, uint8_t auto_run)
{
    return timer_create(timer, name, cb, first, round, arg, auto_run,
                        K_OBJ_STATIC_ALLOC);
}

kstat_t krhino_timer_del(ktimer_t *timer)
{
    k_timer_queue_cb cb;
    kstat_t err;

    NULL_PARA_CHK(timer);

    cb.timer  = timer;
    cb.cb_num = TIMER_CMD_DEL;
    err = krhino_buf_queue_send(&g_timer_queue, &cb, sizeof(k_timer_queue_cb));
    return err;
}

#if (RHINO_CONFIG_KOBJ_DYN_ALLOC > 0)
kstat_t krhino_timer_dyn_create(ktimer_t **timer, const name_t *name,
                                timer_cb_t cb,
                                sys_time_t first, sys_time_t round, void *arg, uint8_t auto_run)
{
    kstat_t   ret;
    ktimer_t *timer_obj;

    NULL_PARA_CHK(timer);

    timer_obj = krhino_mm_alloc(sizeof(ktimer_t));
    if (timer_obj == NULL) {
        return RHINO_NO_MEM;
    }

    ret = timer_create(timer_obj, name, cb, first, round, arg, auto_run,
                       K_OBJ_DYN_ALLOC);
    if (ret != RHINO_SUCCESS) {
        krhino_mm_free(timer_obj);
        return ret;
    }

   *timer = timer_obj;

    return ret;
}

kstat_t krhino_timer_dyn_del(ktimer_t *timer)
{
    k_timer_queue_cb cb;
    kstat_t err;

    NULL_PARA_CHK(timer);

    cb.timer  = timer;
    cb.cb_num = TIMER_CMD_DYN_DEL;
    err = krhino_buf_queue_send(&g_timer_queue, &cb, sizeof(k_timer_queue_cb));

    return err;
}
#endif

kstat_t krhino_timer_start(ktimer_t *timer)
{
    k_timer_queue_cb cb;
    kstat_t err;

    NULL_PARA_CHK(timer);

    cb.timer  = timer;
    cb.cb_num = TIMER_CMD_START; /* 消息会拷贝给接收任务，所以无需担心cb的作用域 */
    err = krhino_buf_queue_send(&g_timer_queue, &cb, sizeof(k_timer_queue_cb)); /* g_timer_queue由系统任务来消费 */
    return err;
}

kstat_t krhino_timer_stop(ktimer_t *timer)
{
    k_timer_queue_cb cb;
    kstat_t err;

    NULL_PARA_CHK(timer);

    cb.timer  = timer;
    cb.cb_num = TIMER_CMD_STOP;
    err = krhino_buf_queue_send(&g_timer_queue, &cb, sizeof(k_timer_queue_cb));
    return err;
}

kstat_t krhino_timer_change(ktimer_t *timer, sys_time_t first, sys_time_t round)
{
    k_timer_queue_cb cb;
    kstat_t err;

    NULL_PARA_CHK(timer);

    if (first == 0u) {
        return RHINO_INV_PARAM;
    }

    if (first >= MAX_TIMER_TICKS) {
        return RHINO_INV_PARAM;
    }

    if (round >= MAX_TIMER_TICKS) {
        return RHINO_INV_PARAM;
    }

    cb.timer   = timer;
    cb.first   = first;
    cb.u.round = round;
    cb.cb_num  = TIMER_CMD_CHG;
    err = krhino_buf_queue_send(&g_timer_queue, &cb, sizeof(k_timer_queue_cb));
    return err;
}

kstat_t krhino_timer_arg_change(ktimer_t *timer, void *arg)
{
    k_timer_queue_cb cb;
    kstat_t err;

    NULL_PARA_CHK(timer);

    cb.timer  = timer;
    cb.u.arg  = arg;
    cb.cb_num = TIMER_ARG_CHG;
    err = krhino_buf_queue_send(&g_timer_queue, &cb, sizeof(k_timer_queue_cb));
    return err;
}

kstat_t krhino_timer_arg_change_auto(ktimer_t *timer, void *arg)
{
    k_timer_queue_cb cb;
    kstat_t err;

    NULL_PARA_CHK(timer);

    cb.timer   = timer;
    cb.u.arg  = arg;
    cb.cb_num = TIMER_ARG_CHG_AUTO;

    err = krhino_buf_queue_send(&g_timer_queue, &cb, sizeof(k_timer_queue_cb));
    return err;
}

static void timer_cb_proc(void)
{
    klist_t     *q;
    klist_t     *start;
    klist_t     *end;
    ktimer_t    *timer;
    sys_time_i_t delta;

    start = end = &g_timer_head;

    for (q = start->next; q != end; q = q->next) {
        timer = krhino_list_entry(q, ktimer_t, timer_list);
        delta = (sys_time_i_t)timer->match - (sys_time_i_t)g_timer_count; /* delta有可能小于0 */
                                                                                                                     /* 比如某个时钟周期没有执行完当前定时器 */
        if (delta <= 0) {
            timer->cb(timer, timer->timer_cb_arg); /* 执行定时器中的回调函数 */
            timer_list_rm(timer);

            if (timer->round_ticks > 0u) { /* 如果是周期性任务, 还需要重新调整剩余时间并加入定时器链表 */
                timer->remain  =  timer->round_ticks;
                timer->match   =  g_timer_count + timer->remain;
                timer->to_head = &g_timer_head;
                timer_list_pri_insert(&g_timer_head, timer);
            } else {
                timer->timer_state = TIMER_DEACTIVE;
            }
        }
        else {
            break;
        }
    }
}

static void cmd_proc(k_timer_queue_cb *cb, uint8_t cmd)
{
    ktimer_t *timer;
    timer = cb->timer;

    switch (cmd) {
        case TIMER_CMD_START: /* 新定时器 */
            if (timer->obj_type != RHINO_TIMER_OBJ_TYPE) {  
                break;
            }

            if (timer->timer_state == TIMER_ACTIVE) {      
                break;
            }

            timer->match   =  g_timer_count + timer->init_count;
            /* sort by remain time */
            timer->remain  =  timer->init_count;
            /* used by timer delete */
            timer->to_head = &g_timer_head;
            timer_list_pri_insert(&g_timer_head, timer); /* 按照剩余时间插入定时器队列 */
            timer->timer_state = TIMER_ACTIVE; /* 激活 */
            break;
        case TIMER_CMD_STOP: /* 终止定时器 */
            if (timer->obj_type != RHINO_TIMER_OBJ_TYPE) {
                break;
            }

            if (timer->timer_state == TIMER_DEACTIVE) {
                break;
            }
            timer_list_rm(timer); /* 把定时器从定时器链表中删除 */
            timer->timer_state = TIMER_DEACTIVE;
            break;
        case TIMER_CMD_CHG: /* 更改定时器周期 */
            if (cb->first == 0u) {
                break;
            }

            if (timer->obj_type != RHINO_TIMER_OBJ_TYPE) {
                break;
            }

            if (timer->timer_state != TIMER_DEACTIVE) {
                break;
            }

            timer->init_count  = cb->first;
            timer->round_ticks = cb->u.round;
            break;
        case TIMER_ARG_CHG: /* 更改定时器回调参数 */
            if (timer->obj_type != RHINO_TIMER_OBJ_TYPE) {
                break;
            }

            if (timer->timer_state != TIMER_DEACTIVE) {
                break;
            }

            timer->timer_cb_arg = cb->u.arg;
            break;
        case TIMER_CMD_DEL: /* 删除定时器 */
            if (timer->obj_type != RHINO_TIMER_OBJ_TYPE) {
                break;               
            }

            if (timer->timer_state != TIMER_DEACTIVE) {
                break;                    
            } 

            if (timer->mm_alloc_flag != K_OBJ_STATIC_ALLOC) {
                break;
            }

            timer->obj_type = RHINO_OBJ_TYPE_NONE;
            TRACE_TIMER_DEL(krhino_cur_task_get(), timer);
            break;
#if (RHINO_CONFIG_KOBJ_DYN_ALLOC > 0)
        case TIMER_CMD_DYN_DEL:
            if (timer->obj_type != RHINO_TIMER_OBJ_TYPE) {
                break;
            }

            if (timer->timer_state != TIMER_DEACTIVE) {
                break;
            }

            if (timer->mm_alloc_flag != K_OBJ_DYN_ALLOC) {
                break;
            }

            timer->obj_type = RHINO_OBJ_TYPE_NONE;
            TRACE_TIMER_DEL(krhino_cur_task_get(), timer);
            krhino_mm_free(timer);
            break;
#endif
        default:
            k_err_proc(RHINO_SYS_FATAL_ERR);
            break;
    }   

}

static void timer_cmd_proc(k_timer_queue_cb *cb)
{
    if (cb->cb_num == TIMER_ARG_CHG_AUTO) {
        cmd_proc(cb, TIMER_CMD_STOP);
        cmd_proc(cb, TIMER_ARG_CHG);
        cmd_proc(cb, TIMER_CMD_START);
    }
    else { /* 新建的定时器, cb_num == TIMER_CMD_START */
        cmd_proc(cb, cb->cb_num);
    }
}

static void timer_task(void *pa)
{ /* 定时器任务(类似Linux内核线程) */
    ktimer_t         *timer;
    k_timer_queue_cb  cb_msg; /* 来自用户任务的定时器(回调函数、参数) */
    kstat_t           err;
    sys_time_t        tick_start;
    sys_time_t        tick_end;
    sys_time_i_t      delta;
    size_t            msg_size;
    (void)pa;

    while (RHINO_TRUE) { /* 如果g_timer_queue中没有消息, timer_task会被阻塞 */
        err = krhino_buf_queue_recv(&g_timer_queue, RHINO_WAIT_FOREVER, &cb_msg, &msg_size);
        tick_end   = krhino_sys_tick_get();

        if (err == RHINO_SUCCESS) {
            g_timer_count = tick_end;
        }
        else {
            k_err_proc(RHINO_SYS_FATAL_ERR);
        }

        timer_cmd_proc(&cb_msg); /* 消费定时器消息,把新的ktimer_t加入g_timer_head链表 */
                                                        /* 把更改的定时器做相应的处理 */
        while (!is_klist_empty(&g_timer_head)) { /* 如果定时器都被停止了，链表就空了 */
            timer = krhino_list_entry(g_timer_head.next, ktimer_t, timer_list);
            tick_start = krhino_sys_tick_get();
            delta = (sys_time_i_t)timer->match - (sys_time_i_t)tick_start;
            if (delta > 0) { /* 以最近一个定时事件剩余时间为超时时间去读取定时器队列 */
                err = krhino_buf_queue_recv(&g_timer_queue, (tick_t)delta, &cb_msg, &msg_size); 
                tick_end = krhino_sys_tick_get(); /* 运行到此 */
                if (err == RHINO_BLK_TIMEOUT) {
                    g_timer_count = tick_end;
                }
                else if (err == RHINO_SUCCESS) {
                    g_timer_count = tick_end;
                    timer_cmd_proc(&cb_msg); /* 如果有新的定时器，处理一下 */
                }
                else {
                    k_err_proc(RHINO_SYS_FATAL_ERR);
                }
            }
            else {
                g_timer_count = tick_start;
            }
                timer_cb_proc(); /* 遍历g_timer_head，挨个处理超时的定时器 */
        }
    }
}

void ktimer_init(void)
{
    klist_init(&g_timer_head);

    krhino_fix_buf_queue_create(&g_timer_queue, "timer_queue",
                                 timer_queue_cb, sizeof(k_timer_queue_cb), RHINO_CONFIG_TIMER_MSG_NUM);
    krhino_task_create(&g_timer_task, "timer_task", NULL,
                       RHINO_CONFIG_TIMER_TASK_PRI, 0u, g_timer_task_stack,
                       RHINO_CONFIG_TIMER_TASK_STACK_SIZE, timer_task, 1u);
}
#endif /* RHINO_CONFIG_TIMER */

