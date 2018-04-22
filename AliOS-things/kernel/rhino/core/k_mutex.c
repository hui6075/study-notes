/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <k_api.h>

kstat_t mutex_create(kmutex_t *mutex, const name_t *name, uint8_t mm_alloc_flag)
{
    CPSR_ALLOC();

    NULL_PARA_CHK(mutex);
    NULL_PARA_CHK(name);

    /* init the list */
    klist_init(&mutex->blk_obj.blk_list);
    mutex->blk_obj.blk_policy = BLK_POLICY_PRI;
    mutex->blk_obj.name       = name;
    mutex->mutex_task         = NULL;
    mutex->mutex_list         = NULL;
    mutex->mm_alloc_flag      = mm_alloc_flag;

#if (RHINO_CONFIG_SYSTEM_STATS > 0)
    RHINO_CRITICAL_ENTER();
    klist_insert(&(g_kobj_list.mutex_head), &mutex->mutex_item);
    RHINO_CRITICAL_EXIT();
#endif

    mutex->blk_obj.obj_type = RHINO_MUTEX_OBJ_TYPE;

    TRACE_MUTEX_CREATE(krhino_cur_task_get(), mutex, name);

    return RHINO_SUCCESS;
}

kstat_t krhino_mutex_create(kmutex_t *mutex, const name_t *name)
{ /* 创建互斥锁 */
    return mutex_create(mutex, name, K_OBJ_STATIC_ALLOC);
}

static void mutex_release(ktask_t *task, kmutex_t *mutex_rel)
{ /* 当前任务释放互斥锁(释放完还需要唤醒阻塞在此互斥锁上的其他任务) */
    uint8_t new_pri;

    /* find suitable task prio */
    new_pri = mutex_pri_look(task, mutex_rel);
    if (new_pri != task->prio) {
        /* change prio */ /* 若之前因为优先级翻转问题提升过任务的优先级，此时需要改回默认优先级 */
        task_pri_change(task, new_pri);

        TRACE_MUTEX_RELEASE(g_active_task[cpu_cur_get()], task, new_pri);

    }
}

kstat_t krhino_mutex_del(kmutex_t *mutex)
{ /**/
    CPSR_ALLOC();

    klist_t *blk_list_head;

    if (mutex == NULL) {
        return RHINO_NULL_PTR;
    }

    NULL_PARA_CHK(mutex);

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    if (mutex->blk_obj.obj_type != RHINO_MUTEX_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_TYPE_ERR;
    }

    if (mutex->mm_alloc_flag != K_OBJ_STATIC_ALLOC) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_DEL_ERR;
    }

    blk_list_head = &mutex->blk_obj.blk_list;

    mutex->blk_obj.obj_type = RHINO_OBJ_TYPE_NONE;

    if (mutex->mutex_task != NULL) { /* 释放互斥锁 */
        mutex_release(mutex->mutex_task, mutex);
    }

    /* all task blocked on this mutex is waken up */
    while (!is_klist_empty(blk_list_head)) { /* 删除所有阻塞在此互斥锁上的任务 */
        pend_task_rm(krhino_list_entry(blk_list_head->next, ktask_t, task_list));
    }

#if (RHINO_CONFIG_SYSTEM_STATS > 0)
    klist_rm(&mutex->mutex_item);
#endif

    TRACE_MUTEX_DEL(g_active_task[cpu_cur_get()], mutex);

    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}

#if (RHINO_CONFIG_KOBJ_DYN_ALLOC > 0)
kstat_t krhino_mutex_dyn_create(kmutex_t **mutex, const name_t *name)
{ /* 动态创建互斥锁 */
    kstat_t  stat;
    kmutex_t *mutex_obj;

    if (mutex == NULL) {
        return RHINO_NULL_PTR;
    }

    NULL_PARA_CHK(mutex);

    mutex_obj = krhino_mm_alloc(sizeof(kmutex_t));
    if (mutex_obj == NULL) {
        return RHINO_NO_MEM;
    }

    stat = mutex_create(mutex_obj, name, K_OBJ_DYN_ALLOC);
    if (stat != RHINO_SUCCESS) {
        krhino_mm_free(mutex_obj);
        return stat;
    }

    *mutex = mutex_obj;

    return stat;
}

kstat_t krhino_mutex_dyn_del(kmutex_t *mutex)
{ /* 动态删除互斥锁 */
    CPSR_ALLOC();

    klist_t *blk_list_head;

    if (mutex == NULL) {
        return RHINO_NULL_PTR;
    }

    NULL_PARA_CHK(mutex);

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    if (mutex->blk_obj.obj_type != RHINO_MUTEX_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_TYPE_ERR;
    }

    if (mutex->mm_alloc_flag != K_OBJ_DYN_ALLOC) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_DEL_ERR;
    }

    blk_list_head = &mutex->blk_obj.blk_list;

    mutex->blk_obj.obj_type = RHINO_OBJ_TYPE_NONE;

    if (mutex->mutex_task != NULL) {
        mutex_release(mutex->mutex_task, mutex);
    }

    /* all task blocked on this mutex is waken up */
    while (!is_klist_empty(blk_list_head)) {
        pend_task_rm(krhino_list_entry(blk_list_head->next, ktask_t, task_list));
    }

#if (RHINO_CONFIG_SYSTEM_STATS > 0)
    klist_rm(&mutex->mutex_item);
#endif

    TRACE_MUTEX_DEL(g_active_task[cpu_cur_get()], mutex);

    RHINO_CRITICAL_EXIT_SCHED();

    krhino_mm_free(mutex);

    return RHINO_SUCCESS;
}
#endif

uint8_t mutex_pri_limit(ktask_t *task, uint8_t pri)
{ /* 用户动态修改任务的优先级时调此API获取互斥锁等待队列最大优先级 */
    kmutex_t *mutex_tmp;
    uint8_t  high_pri;
    ktask_t  *first_blk_task;
    klist_t *blk_list_head;

    high_pri = pri;

    for (mutex_tmp = task->mutex_list; mutex_tmp != NULL;
         mutex_tmp = mutex_tmp->mutex_list) {
        blk_list_head = &mutex_tmp->blk_obj.blk_list;

        if (!is_klist_empty(blk_list_head)) {
            first_blk_task = krhino_list_entry(blk_list_head->next, ktask_t, task_list);
            pri = first_blk_task->prio;
        }

        /* can not set lower prio than the highest prio in all mutexes which hold lock */
        if (pri < high_pri) {
            high_pri = pri;
        }
    }

    return high_pri;
}

uint8_t mutex_pri_look(ktask_t *task, kmutex_t *mutex_rel)
{
    kmutex_t  *mutex_tmp;
    kmutex_t **prev;
    uint8_t   new_pri;
    uint8_t   pri;
    ktask_t  *first_blk_task;
    klist_t  *blk_list_head;

    /* the base prio of task */
    new_pri = task->b_prio;

    /* the highest prio in mutex which is locked */
    pri  = new_pri;
    prev = &task->mutex_list;

    while ((mutex_tmp = *prev) != NULL) {
        if (mutex_tmp == mutex_rel) {
            /* delete itself from list and make task->mutex_list point to next */
            *prev = mutex_tmp->mutex_list;
            continue;
        }

        blk_list_head = &mutex_tmp->blk_obj.blk_list;
        if (!is_klist_empty(blk_list_head)) {
            first_blk_task = krhino_list_entry(blk_list_head->next, ktask_t, task_list);
            pri = first_blk_task->prio; /* 找到mutex_tmp阻塞队列的所有任务最高优先级 */
        }

        if (new_pri > pri) {
            new_pri = pri;
        }

        prev = &mutex_tmp->mutex_list;
    }

    return new_pri;
}

void mutex_task_pri_reset(ktask_t *task)
{
    kmutex_t *mutex_tmp;
    ktask_t *mutex_task;

    if (task->blk_obj->obj_type == RHINO_MUTEX_OBJ_TYPE) {
        mutex_tmp = (kmutex_t *)(task->blk_obj);
        mutex_task = mutex_tmp->mutex_task;

        /* the new highest prio task blocked on this mutex may decrease prio than before so reset the mutex task prio */
        if (mutex_task->prio == task->prio) {
            mutex_release(mutex_task, NULL);
        }
    }
}

kstat_t krhino_mutex_lock(kmutex_t *mutex, tick_t ticks)
{
    CPSR_ALLOC();

    kstat_t  ret;
    ktask_t *mutex_task;
    uint8_t  cur_cpu_num;

    NULL_PARA_CHK(mutex);

    if (g_sys_stat == RHINO_STOPPED) {
        return RHINO_SUCCESS;
    }

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    if (mutex->blk_obj.obj_type != RHINO_MUTEX_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_TYPE_ERR;
    }

    cur_cpu_num = cpu_cur_get();

    /* if the same task get the same mutex again, it causes mutex owner nested */
    if (g_active_task[cur_cpu_num] == mutex->mutex_task) { /* 重复上锁 */
        if (mutex->owner_nested == (mutex_nested_t)-1) {
            /* fatal error here, system must be stoped here */
            k_err_proc(RHINO_MUTEX_NESTED_OVF);
            RHINO_CRITICAL_EXIT();
            return RHINO_MUTEX_NESTED_OVF;
        } else {
            mutex->owner_nested++;
        }

        RHINO_CRITICAL_EXIT();

        return RHINO_MUTEX_OWNER_NESTED;
    }

    mutex_task = mutex->mutex_task;
    if (mutex_task == NULL) {/* 没有任务持有此互斥锁，把本任务的mutex_list指向此互斥锁，并返回上锁成功 */
        /* get lock */
        mutex->mutex_task         = g_active_task[cur_cpu_num];
        mutex->mutex_list         = g_active_task[cur_cpu_num]->mutex_list;
        g_active_task[cur_cpu_num]->mutex_list = mutex;
        mutex->owner_nested       = 1u;

        TRACE_MUTEX_GET(g_active_task[cur_cpu_num], mutex, ticks);

        RHINO_CRITICAL_EXIT();

        return RHINO_SUCCESS; /* 只有0表示成功 */
    }

    /* can't get mutex, and return immediately if wait_option is RHINO_NO_WAIT */
    if (ticks == RHINO_NO_WAIT) { /* 上锁的超时时间为零 */
        RHINO_CRITICAL_EXIT();
        return RHINO_NO_PEND_WAIT; /* 返回失败 */
    }

    /* system is locked so task can not be blocked just return immediately */
    if (g_sched_lock[cur_cpu_num] > 0u) { /* 调度被禁止 */
        RHINO_CRITICAL_EXIT();
        return RHINO_SCHED_DISABLE;
    }

    /* if current task is a higher prio task and block on the mutex
       prio inverse condition happened, prio inherit method is used here */
    if (g_active_task[cur_cpu_num]->prio < mutex_task->prio) { /* 发生优先级翻转 */
        task_pri_change(mutex_task, g_active_task[cur_cpu_num]->prio); /* 优先级继承--把持有此互斥锁的任务优先级调高到跟本任务一致 */
        /* 如果持有此互斥锁的任务又因为其他的互斥锁而处于阻塞状态，还需要进一步调整其他互斥锁阻塞队列的任务优先级.. */
        TRACE_TASK_PRI_INV(g_active_task[cur_cpu_num], mutex_task);

    }

    /* any way block the current task */  /* 此互斥锁已被其他任务持有，因此调整状态为PEND并挂到此互斥锁的阻塞队列 */
    pend_to_blk_obj((blk_obj_t *)mutex, g_active_task[cur_cpu_num], ticks); /* 排队策略，FIFO/PRI */

    TRACE_MUTEX_GET_BLK(g_active_task[cur_cpu_num], mutex, ticks);

    RHINO_CRITICAL_EXIT_SCHED(); /* 调用core_sched进行一次调度及cpu_task_switch */

    RHINO_CPU_INTRPT_DISABLE();

    /* so the task is waked up, need know which reason cause wake up */
    ret = pend_state_end_proc(g_active_task[cpu_cur_get()]); /* 执行到此说明任务被调度了，获取被调度原因并返回给用户 */

    RHINO_CPU_INTRPT_ENABLE();

    return ret;
}

kstat_t krhino_mutex_unlock(kmutex_t *mutex)
{
    CPSR_ALLOC();

    klist_t *blk_list_head;
    ktask_t *task;
    uint8_t  cur_cpu_num;

    NULL_PARA_CHK(mutex);

    if (g_sys_stat == RHINO_STOPPED) {
        return RHINO_SUCCESS;
    }

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    if (mutex->blk_obj.obj_type != RHINO_MUTEX_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_TYPE_ERR;
    }

    cur_cpu_num = cpu_cur_get();

    /* mutex must be released by itself */ /* 只有互斥锁的持有者才能释放互斥锁 */
    if (g_active_task[cur_cpu_num] != mutex->mutex_task) {
        RHINO_CRITICAL_EXIT();
        return RHINO_MUTEX_NOT_RELEASED_BY_OWNER;
    }

    mutex->owner_nested--;

    if (mutex->owner_nested > 0u) {
        RHINO_CRITICAL_EXIT();
        return RHINO_MUTEX_OWNER_NESTED;
    }

    mutex_release(g_active_task[cur_cpu_num], mutex);

    blk_list_head = &mutex->blk_obj.blk_list;

    /* if no block task on this list just return */
    if (is_klist_empty(blk_list_head)) { /* 如果释放锁时没有任务阻塞在此锁上，直接返回 */
        /* No wait task */
        mutex->mutex_task = NULL;

        TRACE_MUTEX_RELEASE_SUCCESS(g_active_task[cur_cpu_num], mutex);
        RHINO_CRITICAL_EXIT();

        return RHINO_SUCCESS;
    }

    /* there must have task blocked on this mutex object */ /* 获取此自旋锁等待队列上的第一个(优先级最高的)任务 */
    task = krhino_list_entry(blk_list_head->next, ktask_t, task_list);

    /* wake up the occupy task, which is the highst prio task on the list */
    pend_task_wakeup(task); /* 唤醒此(优先级最高)的任务 */

    TRACE_MUTEX_TASK_WAKE(g_active_task[cur_cpu_num], task, mutex);

    /* change mutex get task */ /* 把自旋锁持有者改为该(优先级最高)的任务 */
    mutex->mutex_task   = task;
    mutex->mutex_list   = task->mutex_list;
    task->mutex_list    = mutex;
    mutex->owner_nested = 1u;

    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}

