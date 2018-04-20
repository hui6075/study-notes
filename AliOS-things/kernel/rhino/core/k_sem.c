/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <k_api.h>

#if (RHINO_CONFIG_SEM > 0)
static kstat_t sem_create(ksem_t *sem, const name_t *name, sem_count_t count,
                          uint8_t mm_alloc_flag)
{ /* 创建信号量 */
    CPSR_ALLOC();

    NULL_PARA_CHK(sem);
    NULL_PARA_CHK(name);

    /* init the list */ /* 初始化，把blk_list->next / blk_list->prev指向blk_list自己，等于list为空 */
    klist_init(&sem->blk_obj.blk_list);

    /* init resource */
    sem->count              = count; /* 信号量的值 */
    sem->peak_count         = count; /* 信号量峰值(后面可以修改信号量的值) */
    sem->blk_obj.name       = name;
    sem->blk_obj.blk_policy = BLK_POLICY_PRI;
    sem->mm_alloc_flag      = mm_alloc_flag;

#if (RHINO_CONFIG_SYSTEM_STATS > 0)
    RHINO_CRITICAL_ENTER();
    klist_insert(&(g_kobj_list.sem_head), &sem->sem_item);
    RHINO_CRITICAL_EXIT();
#endif

    sem->blk_obj.obj_type = RHINO_SEM_OBJ_TYPE;

    TRACE_SEM_CREATE(krhino_cur_task_get(), sem);

    return RHINO_SUCCESS;
}

kstat_t krhino_sem_create(ksem_t *sem, const name_t *name, sem_count_t count)
{ /* 创建信号量API */
    return sem_create(sem, name, count, K_OBJ_STATIC_ALLOC);
}

kstat_t krhino_sem_del(ksem_t *sem)
{ /* 删除信号量 */
    CPSR_ALLOC();

    klist_t *blk_list_head;

    NULL_PARA_CHK(sem);

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    if (sem->blk_obj.obj_type != RHINO_SEM_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT(); /* IPC类型不对，出错返回 */
        return RHINO_KOBJ_TYPE_ERR;
    }

    if (sem->mm_alloc_flag != K_OBJ_STATIC_ALLOC) {
        RHINO_CRITICAL_EXIT(); /* 内存分配类型不为静态，出错返回 */
        return RHINO_KOBJ_DEL_ERR;
    }

    blk_list_head = &sem->blk_obj.blk_list;
    sem->blk_obj.obj_type = RHINO_OBJ_TYPE_NONE; /* 清空IPC类型 */

    /* all task blocked on this queue is waken up */
    while (!is_klist_empty(blk_list_head)) { /* 唤醒所有阻塞在此信号量上的任务(转为RDY或SUSPEND态) */
        pend_task_rm(krhino_list_entry(blk_list_head->next, ktask_t, task_list));
    }

#if (RHINO_CONFIG_SYSTEM_STATS > 0)
    klist_rm(&sem->sem_item);
#endif

    TRACE_SEM_DEL(g_active_task[cpu_cur_get()], sem);
    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}

#if (RHINO_CONFIG_KOBJ_DYN_ALLOC > 0)
kstat_t krhino_sem_dyn_create(ksem_t **sem, const name_t *name,
                              sem_count_t count)
{ /* 动态分配内存创建信号量，API返回创建的sem */
    kstat_t stat;
    ksem_t  *sem_obj;

    NULL_PARA_CHK(sem);

    sem_obj = krhino_mm_alloc(sizeof(ksem_t));

    if (sem_obj == NULL) {
        return RHINO_NO_MEM;
    }

    stat = sem_create(sem_obj, name, count, K_OBJ_DYN_ALLOC);

    if (stat != RHINO_SUCCESS) {
        krhino_mm_free(sem_obj);
        return stat;
    }

    *sem = sem_obj;

    return stat;
}

kstat_t krhino_sem_dyn_del(ksem_t *sem)
{ /* 删除动态分配内存创建的信号量 */
    CPSR_ALLOC();

    klist_t *blk_list_head;

    NULL_PARA_CHK(sem);

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    if (sem->blk_obj.obj_type != RHINO_SEM_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT(); /* IPC类型不对，出错返回 */
        return RHINO_KOBJ_TYPE_ERR;
    }

    if (sem->mm_alloc_flag != K_OBJ_DYN_ALLOC) {
        RHINO_CRITICAL_EXIT(); /* 内存分配类型不为动态，出错返回 */
        return RHINO_KOBJ_DEL_ERR;
    }

    blk_list_head = &sem->blk_obj.blk_list;
    sem->blk_obj.obj_type = RHINO_OBJ_TYPE_NONE; /* 清除IPC类型 */

    /* all task blocked on this queue is waken up */
    while (!is_klist_empty(blk_list_head)) { /* 唤醒所有阻塞在此信号量上的任务(转为RDY或SUSPEND态) */
        pend_task_rm(krhino_list_entry(blk_list_head->next, ktask_t, task_list));
    }

#if (RHINO_CONFIG_SYSTEM_STATS > 0)
    klist_rm(&sem->sem_item);
#endif

    TRACE_SEM_DEL(g_active_task[cpu_cur_get()], sem);
    RHINO_CRITICAL_EXIT_SCHED();

    krhino_mm_free(sem);

    return RHINO_SUCCESS;
}

#endif

static kstat_t sem_give(ksem_t *sem, uint8_t opt_wake_all)
{ /* 给信号(发信号) */
    CPSR_ALLOC();

    uint8_t  cur_cpu_num;
    klist_t *blk_list_head;

    /* this is only needed when system zero interrupt feature is enabled */
#if (RHINO_CONFIG_INTRPT_GUARD > 0)
    soc_intrpt_guard();
#endif

    RHINO_CRITICAL_ENTER();

    if (sem->blk_obj.obj_type != RHINO_SEM_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT(); /* IPC类型不对，出错返回 */
        return RHINO_KOBJ_TYPE_ERR;
    }

    cur_cpu_num = cpu_cur_get();
    (void)cur_cpu_num;

    blk_list_head = &sem->blk_obj.blk_list;

    if (is_klist_empty(blk_list_head)) { /* 如果没有任务阻塞在此信号量上 */
        if (sem->count == (sem_count_t)-1) {

            TRACE_SEM_OVERFLOW(g_active_task[cur_cpu_num], sem);
            RHINO_CRITICAL_EXIT();

            return RHINO_SEM_OVF;
        }

        /* increase resource */
        sem->count++;

        if (sem->count > sem->peak_count) {
            sem->peak_count = sem->count; /* 更新信号量峰值 */
        }

        TRACE_SEM_CNT_INCREASE(g_active_task[cur_cpu_num], sem);
        RHINO_CRITICAL_EXIT();
        return RHINO_SUCCESS;
    }

    /* wake all the task blocked on this semaphore */ /* 有任务阻塞在此信号量上 */
    if (opt_wake_all) { /* 唤醒所有任务 */
        while (!is_klist_empty(blk_list_head)) {
            TRACE_SEM_TASK_WAKE(g_active_task[cur_cpu_num], krhino_list_entry(blk_list_head->next,
                                                                              ktask_t, task_list),
                                sem, opt_wake_all);

            pend_task_wakeup(krhino_list_entry(blk_list_head->next, ktask_t, task_list));
        }

    } else { /* 只唤醒第一个任务 */
        TRACE_SEM_TASK_WAKE(g_active_task[cur_cpu_num], krhino_list_entry(blk_list_head->next,
                                                                          ktask_t, task_list),
                            sem, opt_wake_all);

        /* wake up the highest prio task block on the semaphore */
        pend_task_wakeup(krhino_list_entry(blk_list_head->next, ktask_t, task_list));
    }

    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}

kstat_t krhino_sem_give(ksem_t *sem)
{
    NULL_PARA_CHK(sem);

    return sem_give(sem, WAKE_ONE_SEM);
}

kstat_t krhino_sem_give_all(ksem_t *sem)
{
    NULL_PARA_CHK(sem);

    return sem_give(sem, WAKE_ALL_SEM);
}

kstat_t krhino_sem_take(ksem_t *sem, tick_t ticks)
{ /* 等信号 */
    CPSR_ALLOC();

    uint8_t  cur_cpu_num;
    kstat_t  stat;

    NULL_PARA_CHK(sem);

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    if (sem->blk_obj.obj_type != RHINO_SEM_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_TYPE_ERR;
    }

    cur_cpu_num = cpu_cur_get();

    if (sem->count > 0u) {
        sem->count--; /* 等信号时如果信号量大于0，直接返回成功 */

        TRACE_SEM_GET_SUCCESS(g_active_task[cur_cpu_num], sem);
        RHINO_CRITICAL_EXIT();

        return RHINO_SUCCESS;
    }

    /* can't get semphore, and return immediately if wait_option is  RHINO_NO_WAIT */
    if (ticks == RHINO_NO_WAIT) { /* 如果信号量<=0，但超时时间为0，则直接返回失败 */
        RHINO_CRITICAL_EXIT();
        return RHINO_NO_PEND_WAIT;
    }

    if (g_sched_lock[cur_cpu_num] > 0u) {
        RHINO_CRITICAL_EXIT();
        return RHINO_SCHED_DISABLE;
    }
    /*  把任务挂到tick_list，并从ready_queue中删除 */
    pend_to_blk_obj((blk_obj_t *)sem, g_active_task[cur_cpu_num], ticks);

    TRACE_SEM_GET_BLK(g_active_task[cur_cpu_num], sem, ticks);

    RHINO_CRITICAL_EXIT_SCHED();

    RHINO_CPU_INTRPT_DISABLE();
    /* 本任务再次得到调度，获取调度原因 */
    stat = pend_state_end_proc(g_active_task[cpu_cur_get()]);

    RHINO_CPU_INTRPT_ENABLE();
    /* 把调度原因返回给用户(成功获取信号量/超时/任务被删除) */
    return stat;
}

kstat_t krhino_sem_count_set(ksem_t *sem, sem_count_t sem_count)
{ /* 修改信号量的值 */
    CPSR_ALLOC();

    klist_t *blk_list_head;

    NULL_PARA_CHK(sem);

    blk_list_head = &sem->blk_obj.blk_list;

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    if (sem->blk_obj.obj_type != RHINO_SEM_OBJ_TYPE) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_TYPE_ERR;
    }

    /* set new count */
    if (sem->count > 0u) {
        sem->count = sem_count;
    } else {
        if (is_klist_empty(blk_list_head)) {
            sem->count = sem_count;
        } else { /* 如果此时正有任务阻塞在信号量上，信号量值是不能修改的 */
            RHINO_CRITICAL_EXIT();
            return RHINO_SEM_TASK_WAITING;
        }
    }

    /* update sem peak count if need */
    if (sem->count > sem->peak_count) {
        sem->peak_count = sem->count;
    }

    RHINO_CRITICAL_EXIT();

    return RHINO_SUCCESS;
}

kstat_t krhino_sem_count_get(ksem_t *sem, sem_count_t *count)
{ /* 获取信号量当前值 */
    CPSR_ALLOC();

    NULL_PARA_CHK(sem);
    NULL_PARA_CHK(count);

    RHINO_CRITICAL_ENTER();
   *count = sem->count;
    RHINO_CRITICAL_EXIT();

    return RHINO_SUCCESS;
}

#endif /* RHINO_CONFIG_SEM */

