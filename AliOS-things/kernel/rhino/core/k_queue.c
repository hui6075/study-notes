/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <k_api.h>

#if (RHINO_CONFIG_QUEUE > 0)
RHINO_INLINE void task_msg_recv(ktask_t *task, void *msg)
{ /* 有任务向队列发送消息时，调用此函数，唤醒阻塞在此队列上的第一个任务 */
    task->msg = msg;
    pend_task_wakeup(task); /* 任务被唤醒之后就被从阻塞队列删除了 */
}

static kstat_t queue_create(kqueue_t *queue, const name_t *name, void **start,
                            size_t msg_num, uint8_t mm_alloc_flag)
{
    CPSR_ALLOC();

    NULL_PARA_CHK(queue);
    NULL_PARA_CHK(start);
    NULL_PARA_CHK(name);

    if (msg_num == 0u) {
        return RHINO_INV_PARAM;
    }

    /* init the queue blocked list */
    klist_init(&queue->blk_obj.blk_list);

    queue->blk_obj.name       = name; /* "test_queue" */
    queue->blk_obj.blk_policy = BLK_POLICY_PRI; /* 唤醒调度策略 */
    queue->msg_q.queue_start  = start; /* 存放消息的内存起始地址 */

    ringbuf_init(&queue->ringbuf, (void *)start, msg_num * sizeof(void *),
                 RINGBUF_TYPE_FIX, sizeof(void *)); /* 初始化RingBuffer */

    queue->msg_q.size         = msg_num; /* 消息队列容量 */
    queue->msg_q.cur_num      = 0u; /* 消息队列当前消息数量，刚创建时还没有消息入队，所以为0 */
    queue->msg_q.peak_num     = 0u; /* 消息数量峰值 */
    queue->mm_alloc_flag      = mm_alloc_flag;

#if (RHINO_CONFIG_SYSTEM_STATS > 0)
    RHINO_CRITICAL_ENTER();
    klist_insert(&(g_kobj_list.queue_head), &queue->queue_item);
    RHINO_CRITICAL_EXIT();
#endif

    queue->blk_obj.obj_type = RHINO_QUEUE_OBJ_TYPE; /* 事件类型 */

    return RHINO_SUCCESS;
}

kstat_t krhino_queue_create(kqueue_t *queue, const name_t *name, void **start,
                            size_t msg_num)
{
    return queue_create(queue, name, start, msg_num, K_OBJ_STATIC_ALLOC);
}

kstat_t krhino_queue_del(kqueue_t *queue)
{
    CPSR_ALLOC();

    klist_t *blk_list_head;

    NULL_PARA_CHK(queue);

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    if (queue->blk_obj.obj_type != RHINO_QUEUE_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_TYPE_ERR;
    }

    if (queue->mm_alloc_flag != K_OBJ_STATIC_ALLOC) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_DEL_ERR;
    }

    blk_list_head = &queue->blk_obj.blk_list;

    queue->blk_obj.obj_type = RHINO_OBJ_TYPE_NONE;

    /* all task blocked on this queue is waken up */
    while (!is_klist_empty(blk_list_head)) {
        pend_task_rm(krhino_list_entry(blk_list_head->next, ktask_t, task_list));
    }

#if (RHINO_CONFIG_SYSTEM_STATS > 0)
    klist_rm(&queue->queue_item);
#endif

    ringbuf_reset(&queue->ringbuf);

    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}

#if (RHINO_CONFIG_KOBJ_DYN_ALLOC > 0)
kstat_t krhino_queue_dyn_create(kqueue_t **queue, const name_t *name,
                                size_t msg_num)
{
    kstat_t   stat;
    kqueue_t *queue_obj;
    void     *msg_start;

    NULL_PARA_CHK(queue);

    queue_obj = krhino_mm_alloc(sizeof(kqueue_t));
    if (queue_obj == NULL) {
        return RHINO_NO_MEM;
    }

    msg_start = krhino_mm_alloc(msg_num * sizeof(void *));
    if (msg_start == NULL) {
        krhino_mm_free(queue_obj);
        return RHINO_NO_MEM;
    }

    stat = queue_create(queue_obj, name, (void **)msg_start, msg_num,
                        K_OBJ_DYN_ALLOC);
    if (stat != RHINO_SUCCESS) {
        krhino_mm_free(msg_start);
        krhino_mm_free(queue_obj);
        return stat;
    }

    *queue = queue_obj;

    return stat;
}

kstat_t krhino_queue_dyn_del(kqueue_t *queue)
{
    CPSR_ALLOC();

    klist_t *blk_list_head;

    NULL_PARA_CHK(queue);

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    if (queue->blk_obj.obj_type != RHINO_QUEUE_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_TYPE_ERR;
    }

    if (queue->mm_alloc_flag != K_OBJ_DYN_ALLOC) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_DEL_ERR;
    }

    blk_list_head = &queue->blk_obj.blk_list;

    queue->blk_obj.obj_type = RHINO_OBJ_TYPE_NONE;

    /* all task blocked on this queue is waken up */
    while (!is_klist_empty(blk_list_head)) {
        pend_task_rm(krhino_list_entry(blk_list_head->next, ktask_t, task_list));
    }

#if (RHINO_CONFIG_SYSTEM_STATS > 0)
    klist_rm(&queue->queue_item);
#endif

    ringbuf_reset(&queue->ringbuf);

    RHINO_CRITICAL_EXIT_SCHED();

    krhino_mm_free(queue->msg_q.queue_start);
    krhino_mm_free(queue);

    return RHINO_SUCCESS;
}
#endif

static kstat_t msg_send(kqueue_t *p_q, void *p_void, uint8_t opt_wake_all)
{
    CPSR_ALLOC();

    klist_t *blk_list_head;

    NULL_PARA_CHK(p_q);

    /* this is only needed when system zero interrupt feature is enabled */
#if (RHINO_CONFIG_INTRPT_GUARD > 0)
    soc_intrpt_guard();
#endif

    RHINO_CRITICAL_ENTER();

    if (p_q->blk_obj.obj_type != RHINO_QUEUE_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_TYPE_ERR;
    }

    if (p_q->msg_q.cur_num >= p_q->msg_q.size) {
        RHINO_CRITICAL_EXIT(); /* 消息队列满了，返回消息发送失败 */
        return RHINO_QUEUE_FULL;
    }

    blk_list_head = &p_q->blk_obj.blk_list;

    /* queue is not full here, if there is no blocked receive task */
    if (is_klist_empty(blk_list_head)) { /* 消息队列没满，也没有任务阻塞在队列上 */
        p_q->msg_q.cur_num++;

        /* update peak_num for debug */
        if (p_q->msg_q.cur_num > p_q->msg_q.peak_num) {
            p_q->msg_q.peak_num = p_q->msg_q.cur_num; /* 更新消息峰值 */
        }

        ringbuf_push(&p_q->ringbuf, &p_void, sizeof(void *));

        RHINO_CRITICAL_EXIT();
        return RHINO_SUCCESS; /* 直接把消息塞进Ring Buffer并返回 */
    }

    /* wake all the task blocked on this queue */
    if (opt_wake_all) { /* 把此消息发送给所有任务，并唤醒 */
        while (!is_klist_empty(blk_list_head)) {
            task_msg_recv(krhino_list_entry(blk_list_head->next, ktask_t, task_list),
                          p_void);
        }
    } else { /* 只唤醒一个任务 */
        task_msg_recv(krhino_list_entry(blk_list_head->next, ktask_t, task_list),
                      p_void);
    }

    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}

kstat_t krhino_queue_back_send(kqueue_t *queue, void *msg)
{ /* 生产vs消费 1 vs 1 */
    return msg_send(queue, msg, WAKE_ONE_TASK);
}

kstat_t krhino_queue_all_send(kqueue_t *queue, void *msg)
{ /* 生产vs消费 1 vs all */
    return msg_send(queue, msg, WAKE_ALL_TASK);
}

kstat_t krhino_queue_recv(kqueue_t *queue, tick_t ticks, void **msg)
{ /* 消费者任务从队列接收消息，带超时选项 */
    CPSR_ALLOC();

    kstat_t ret;
    uint8_t cur_cpu_num;

    NULL_PARA_CHK(queue);
    NULL_PARA_CHK(msg);

    RHINO_CRITICAL_ENTER(); /* 关中断 */

    cur_cpu_num = cpu_cur_get();

    if ((g_intrpt_nested_level[cur_cpu_num] > 0u) && (ticks != RHINO_NO_WAIT)) {
        RHINO_CRITICAL_EXIT(); /* 存在中断嵌套的情况下不能进入临界区，直接返回 */
        return RHINO_NOT_CALLED_BY_INTRPT;
    }

    if (queue->blk_obj.obj_type != RHINO_QUEUE_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_TYPE_ERR;
    }

    /* if queue has msgs, just receive it */
    if (queue->msg_q.cur_num > 0u) { /* 队列有消息 */
        /* 从Ring Buffer中弹出一条消息 */
        ringbuf_pop(&queue->ringbuf, msg, NULL);

        queue->msg_q.cur_num--; /* 队列消息数量减一 */

        RHINO_CRITICAL_EXIT();

        return RHINO_SUCCESS;
    }

    if (ticks == RHINO_NO_WAIT) { /* 队列没有消息，且不进行读阻塞 */
        *msg = NULL;
        RHINO_CRITICAL_EXIT();

        return RHINO_NO_PEND_WAIT;
    }

    /* if system is locked, block operation is not allowed */
    if (g_sched_lock[cur_cpu_num] > 0u) {
        *msg = NULL;
        RHINO_CRITICAL_EXIT();
        return RHINO_SCHED_DISABLE;
    }
    /* 阻塞当前任务 */
    pend_to_blk_obj((blk_obj_t *)queue, g_active_task[cur_cpu_num], ticks);
    /* 切换到别的进程 */
    RHINO_CRITICAL_EXIT_SCHED();
    /* 关中断。执行到此说明再次调度到本任务 */
    RHINO_CPU_INTRPT_DISABLE();

    cur_cpu_num = cpu_cur_get();
    /* 获取本任务被唤醒，调度执行的原因 */
    ret = pend_state_end_proc(g_active_task[cur_cpu_num]);

    switch (ret) {
        case RHINO_SUCCESS: /* 因为别的任务投递了消息造成本任务被唤醒 */
            *msg = g_active_task[cur_cpu_num]->msg; /* msg已经在别的任务中被赋了值 */
            break;
        default:
            *msg = NULL; /*可能因为阻塞超时/消息队列被删除造成本任务被唤醒，消息返回为空*/
            break;
    }

    RHINO_CPU_INTRPT_ENABLE();

    return ret;
}

kstat_t krhino_queue_is_full(kqueue_t *queue)
{ /* 判断队列是否满，非阻塞 */
    CPSR_ALLOC();

    kstat_t ret;

    NULL_PARA_CHK(queue);

    RHINO_CRITICAL_ENTER();

    if (queue->blk_obj.obj_type != RHINO_QUEUE_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_TYPE_ERR;
    }

    if (queue->msg_q.cur_num >= queue->msg_q.size) {
        ret = RHINO_QUEUE_FULL;
    } else {
        ret = RHINO_QUEUE_NOT_FULL;
    }

    RHINO_CRITICAL_EXIT();

    return ret;
}

kstat_t krhino_queue_flush(kqueue_t *queue)
{ /* 重置消息队列 */
    CPSR_ALLOC();

    NULL_PARA_CHK(queue);

    RHINO_CRITICAL_ENTER(); /* 关中断 */

    INTRPT_NESTED_LEVEL_CHK();

    if (queue->blk_obj.obj_type != RHINO_QUEUE_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_TYPE_ERR;
    }

    queue->msg_q.cur_num = 0u; /* 清空当前消息 */
    ringbuf_reset(&queue->ringbuf); /* 重置RingBuffer */

    RHINO_CRITICAL_EXIT();

    return RHINO_SUCCESS;
}

kstat_t krhino_queue_info_get(kqueue_t *queue, msg_info_t *info)
{ /* 获取队列信息 */
    CPSR_ALLOC();

    klist_t *blk_list_head;

    if (queue == NULL) {
        return RHINO_NULL_PTR;
    }

    if (info == NULL) {
        return RHINO_NULL_PTR;
    }

    NULL_PARA_CHK(queue);
    NULL_PARA_CHK(info);

    RHINO_CPU_INTRPT_DISABLE(); /* 进入临界区，关中断 */

    if (queue->blk_obj.obj_type != RHINO_QUEUE_OBJ_TYPE) {
        RHINO_CPU_INTRPT_ENABLE();
        return RHINO_KOBJ_TYPE_ERR;
    }

    blk_list_head           = &queue->blk_obj.blk_list;

    info->msg_q.peak_num    = queue->msg_q.peak_num;
    info->msg_q.cur_num     = queue->msg_q.cur_num;
    info->msg_q.queue_start = queue->msg_q.queue_start;
    info->msg_q.size        = queue->msg_q.size;
    info->pend_entry        = blk_list_head->next;

    RHINO_CPU_INTRPT_ENABLE(); /* 开中断 */

    return RHINO_SUCCESS;
}
#endif

