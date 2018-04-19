/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <k_api.h>

static kstat_t task_create(ktask_t *task, const name_t *name, void *arg,
                           uint8_t prio, tick_t ticks, cpu_stack_t *stack_buf,
                           size_t stack_size, task_entry_t entry, uint8_t autorun,
                           uint8_t mm_alloc_flag, uint8_t cpu_num, uint8_t cpu_binded)
{
    CPSR_ALLOC();

    cpu_stack_t *tmp;

    NULL_PARA_CHK(task);
    NULL_PARA_CHK(name);
    NULL_PARA_CHK(entry);
    NULL_PARA_CHK(stack_buf);

    if (stack_size == 0u) {
        return RHINO_TASK_INV_STACK_SIZE;
    }

    if (prio >= RHINO_CONFIG_PRI_MAX) {
        return RHINO_BEYOND_MAX_PRI;
    }

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    /* idle task is only allowed to create once */
    /*  如果要创建的任务优先级为RHINO_IDLE_PRI，并且idle task已经创建，则返回失败 */
    if (prio == RHINO_IDLE_PRI) {
        if (g_idle_task_spawned[cpu_num] > 0u) {
            RHINO_CRITICAL_EXIT();
            return RHINO_IDLE_TASK_EXIST;
        }

        g_idle_task_spawned[cpu_num] = 1u;
    }

    RHINO_CRITICAL_EXIT();

    memset(task, 0, sizeof(ktask_t));

#if (RHINO_CONFIG_SCHED_RR > 0)
    if (ticks > 0u) {
        task->time_total = ticks;
    } else {
        task->time_total = RHINO_CONFIG_TIME_SLICE_DEFAULT;
    }

    task->time_slice   = task->time_total;
    task->sched_policy = KSCHED_RR;
#endif

    if (autorun > 0u) {/*任务是否自动运行*/
        task->task_state    = K_RDY;
    } else {
        task->task_state    = K_SUSPENDED;
        task->suspend_count = 1u;
    }

    /* init all the stack element to 0 */
    task->task_stack_base = stack_buf; /*task stack起始地址，栈内存由krhino_mm_alloc*/
    tmp = stack_buf;

    memset(tmp, 0, stack_size * sizeof(cpu_stack_t));

    task->task_name     = name;
    task->prio          = prio;
    task->b_prio        = prio;
    task->stack_size    = stack_size;
    task->mm_alloc_flag = mm_alloc_flag;
    task->cpu_num       = cpu_num;
    cpu_binded          = cpu_binded;

#if (RHINO_CONFIG_CPU_NUM > 1)
    task->cpu_binded    = cpu_binded;
#endif

#if (RHINO_CONFIG_TASK_STACK_OVF_CHECK > 0)
#if (RHINO_CONFIG_CPU_STACK_DOWN > 0)
    tmp  = task->task_stack_base;
    *tmp = RHINO_TASK_STACK_OVF_MAGIC; /* 栈起始地址前4个字节是一个MAGIC NUMBER */
#else
    tmp  = (cpu_stack_t *)(task->task_stack_base) + task->stack_size - 1u;
    *tmp = RHINO_TASK_STACK_OVF_MAGIC;
#endif
#endif

    task->task_stack = cpu_task_stack_init(stack_buf, stack_size, arg, entry); /*入口函数、参数、链接寄存器入栈*/
    /*task结束的链接寄存器是一个函数，可能是sleep函数，也可能是task_death之类的函数*/
#if (RHINO_CONFIG_USER_HOOK > 0)
    krhino_task_create_hook(task);
#endif

    TRACE_TASK_CREATE(task);

    RHINO_CRITICAL_ENTER(); /*关中断*/

#if (RHINO_CONFIG_SYSTEM_STATS > 0)
    klist_insert(&(g_kobj_list.task_head), &task->task_stats_item);
#endif
    /* 如果当前任务创建完可以自动开始执行 */
    if (autorun > 0u) {/*ready list，每个优先级的任务一个klist*/
        ready_list_add_tail(&g_ready_queue, task);/*按照task->prio插入klist*/
        /* if system is not start,not call core_sched */
        if (g_sys_stat == RHINO_RUNNING) {
            RHINO_CRITICAL_EXIT_SCHED();/*core_sched()执行调度，调度完直接返回LR指定的函数(可能是这次创建任务的入口函数) */
            return RHINO_SUCCESS;
        }
    }

    RHINO_CRITICAL_EXIT(); /*开中断*/
    return RHINO_SUCCESS;
}

kstat_t krhino_task_create(ktask_t *task, const name_t *name, void *arg,
                           uint8_t prio, tick_t ticks, cpu_stack_t *stack_buf,
                           size_t stack_size, task_entry_t entry, uint8_t autorun)
{
    return task_create(task, name, arg, prio, ticks, stack_buf, stack_size, entry,
                       autorun, K_OBJ_STATIC_ALLOC, 0, 0);
}

#if (RHINO_CONFIG_CPU_NUM > 1)
kstat_t krhino_task_cpu_create(ktask_t *task, const name_t *name, void *arg,
                               uint8_t prio, tick_t ticks, cpu_stack_t *stack_buf,
                               size_t stack_size, task_entry_t entry, uint8_t cpu_num,
                               uint8_t autorun)
{
    return task_create(task, name, arg, prio, ticks, stack_buf, stack_size, entry,
                       autorun, K_OBJ_STATIC_ALLOC, cpu_num, 1);
}

kstat_t krhino_task_cpu_bind(ktask_t *task, uint8_t cpu_num)
{
    CPSR_ALLOC();

    ktask_t *task_cur;

    RHINO_CRITICAL_ENTER();
    task_cur = g_active_task[cpu_cur_get()];
    if (task != task_cur) {
        RHINO_CRITICAL_EXIT();
        return RHINO_INV_PARAM;
    }
    task->cpu_num    = cpu_num;
    task->cpu_binded = 1u;
    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}

kstat_t krhino_task_cpu_unbind(ktask_t *task)
{
    CPSR_ALLOC();

    ktask_t *task_cur;

    RHINO_CRITICAL_ENTER();
    task_cur = g_active_task[cpu_cur_get()];
    if (task != task_cur) {
        RHINO_CRITICAL_EXIT();
        return RHINO_INV_PARAM;
    }
    task->cpu_binded = 0u;
    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}
#endif

#if (RHINO_CONFIG_KOBJ_DYN_ALLOC > 0)
kstat_t task_dyn_create(ktask_t **task, const name_t *name, void *arg,
                        uint8_t pri, tick_t ticks, size_t stack, task_entry_t entry,
                        uint8_t cpu_num, uint8_t cpu_binded, uint8_t autorun)
{
    kstat_t      ret;
    cpu_stack_t *task_stack;
    ktask_t     *task_obj;

    NULL_PARA_CHK(task);

    if (stack == 0) {
        return RHINO_INV_PARAM;
    }

    task_stack = krhino_mm_alloc(stack * sizeof(cpu_stack_t));
    if (task_stack == NULL) {
        return RHINO_NO_MEM;
    }

    task_obj = krhino_mm_alloc(sizeof(ktask_t));
    if (task_obj == NULL) {
        krhino_mm_free(task_stack);
        return RHINO_NO_MEM;
    }

    *task = task_obj;

    ret = task_create(task_obj, name, arg, pri, ticks, task_stack, stack, entry,
                      autorun, K_OBJ_DYN_ALLOC, cpu_num, cpu_binded);
    if ((ret != RHINO_SUCCESS) && (ret != RHINO_STOPPED)) {
        krhino_mm_free(task_stack);
        krhino_mm_free(task_obj);
        *task = NULL;
        return ret;
    }

    return ret;
}

kstat_t krhino_task_dyn_create(ktask_t **task, const name_t *name, void *arg,
                               uint8_t pri, tick_t ticks, size_t stack,
                               task_entry_t entry, uint8_t autorun)
{
    return task_dyn_create(task, name, arg, pri, ticks, stack, entry, 0, 0, autorun);
}

#if (RHINO_CONFIG_CPU_NUM > 1)
kstat_t krhino_task_cpu_dyn_create(ktask_t **task, const name_t *name, void *arg,
                                   uint8_t pri, tick_t ticks, size_t stack,
                                   task_entry_t entry, uint8_t cpu_num, uint8_t autorun)
{
    return task_dyn_create(task, name, arg, pri, ticks, stack, entry, cpu_num, 1, autorun);
}
#endif
#endif

kstat_t krhino_task_sleep(tick_t ticks)
{
    CPSR_ALLOC();

    uint8_t cur_cpu_num;

    kstat_t ret;

    if (ticks == 0u) {
        return RHINO_INV_PARAM;
    }

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    cur_cpu_num = cpu_cur_get();

    /* system is locked so task can not be blocked just return immediately */
    if (g_sched_lock[cur_cpu_num] > 0u) {
        RHINO_CRITICAL_EXIT();
        return RHINO_SCHED_DISABLE;
    }

    g_active_task[cur_cpu_num]->task_state = K_SLEEP;
    tick_list_insert(g_active_task[cur_cpu_num], ticks);
    ready_list_rm(&g_ready_queue, g_active_task[cur_cpu_num]);

    TRACE_TASK_SLEEP(g_active_task[cur_cpu_num], ticks);

    RHINO_CRITICAL_EXIT_SCHED();

    RHINO_CPU_INTRPT_DISABLE();

    /* is task timeout normally after sleep */
    ret = pend_state_end_proc(g_active_task[cpu_cur_get()]);

    RHINO_CPU_INTRPT_ENABLE();

    return ret;
}

kstat_t krhino_task_yield(void)
{
    CPSR_ALLOC();

    /* make current task to the end of ready list */
    RHINO_CRITICAL_ENTER();

    ready_list_head_to_tail(&g_ready_queue, g_active_task[cpu_cur_get()]);

    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}

ktask_t *krhino_cur_task_get(void)
{
    CPSR_ALLOC();
    ktask_t *task;

    RHINO_CRITICAL_ENTER();
    task = g_active_task[cpu_cur_get()];
    RHINO_CRITICAL_EXIT();

    return task;
}

kstat_t task_suspend(ktask_t *task)
{
    CPSR_ALLOC();

    uint8_t cur_cpu_num;

    RHINO_CRITICAL_ENTER();

    cur_cpu_num = cpu_cur_get();

#if (RHINO_CONFIG_CPU_NUM > 1)
    if (task->cpu_num != cur_cpu_num) {
        if (task->cur_exc == 1) {
            RHINO_CRITICAL_EXIT();
            return RHINO_TRY_AGAIN;
        }
    }
#endif

    if (task == g_active_task[cur_cpu_num]) {
        if (g_sched_lock[cur_cpu_num] > 0u) {
            RHINO_CRITICAL_EXIT();
            return RHINO_SCHED_DISABLE;
        }
    }

    switch (task->task_state) {
        case K_RDY:
            task->suspend_count = 1u;
            task->task_state    = K_SUSPENDED;
            ready_list_rm(&g_ready_queue, task);
            break;
        case K_SLEEP:
            task->suspend_count = 1u;
            task->task_state    = K_SLEEP_SUSPENDED;
            break;
        case K_PEND:
            task->suspend_count = 1u;
            task->task_state    = K_PEND_SUSPENDED;
            break;
        case K_SUSPENDED:
        case K_SLEEP_SUSPENDED:
        case K_PEND_SUSPENDED:
            if (task->suspend_count == (suspend_nested_t)-1) {
                RHINO_CRITICAL_EXIT();
                return RHINO_SUSPENDED_COUNT_OVF;
            }

            task->suspend_count++;
            break;
        case K_SEED:
        default:
            RHINO_CRITICAL_EXIT();
            return RHINO_INV_TASK_STATE;
    }

    TRACE_TASK_SUSPEND(g_active_task[cur_cpu_num], task);

    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}

kstat_t krhino_task_suspend(ktask_t *task)
{
    if (task == NULL) {
        return RHINO_NULL_PTR;
    }

    if (task->prio == RHINO_IDLE_PRI) {
        return RHINO_TASK_SUSPEND_NOT_ALLOWED;
    }

    return task_suspend(task);
}

kstat_t task_resume(ktask_t *task)
{
    CPSR_ALLOC();

    RHINO_CRITICAL_ENTER();

    switch (task->task_state) {
        case K_RDY:
        case K_SLEEP:
        case K_PEND:
            RHINO_CRITICAL_EXIT();
            return RHINO_TASK_NOT_SUSPENDED;
        case K_SUSPENDED:
            task->suspend_count--;

            if (task->suspend_count == 0u) {
                /* Make task ready */
                task->task_state = K_RDY;
                ready_list_add(&g_ready_queue, task);
            }

            break;
        case K_SLEEP_SUSPENDED:
            task->suspend_count--;

            if (task->suspend_count == 0u) {
                task->task_state = K_SLEEP;
            }

            break;
        case K_PEND_SUSPENDED:
            task->suspend_count--;

            if (task->suspend_count == 0u) {
                task->task_state = K_PEND;
            }

            break;
        case K_SEED:
        default:
            RHINO_CRITICAL_EXIT();
            return RHINO_INV_TASK_STATE;
    }

    TRACE_TASK_RESUME(g_active_task[cpu_cur_get()], task);

    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}

kstat_t krhino_task_resume(ktask_t *task)
{
    NULL_PARA_CHK(task);

    return task_resume(task);
}

kstat_t krhino_task_stack_min_free(ktask_t *task, size_t *free)
{
    cpu_stack_t *task_stack;
    size_t free_stk = 0;

    NULL_PARA_CHK(task);
    NULL_PARA_CHK(free);

    if (task->task_state == K_DELETED) {
        return RHINO_INV_TASK_STATE;
    }

#if (RHINO_CONFIG_CPU_STACK_DOWN > 0)
    task_stack = task->task_stack_base + 1u;
    while (*task_stack++ == 0u) {
        free_stk++;
    }
#else
    task_stack = (cpu_stack_t *)(task->task_stack_base) + task->stack_size - 2u;
    while (*task_stack-- == 0u) {
        free_stk++;
    }
#endif

    *free = free_stk;

    return RHINO_SUCCESS;
}

#if (RHINO_CONFIG_TASK_STACK_CUR_CHECK > 0)
kstat_t krhino_task_stack_cur_free(ktask_t *task, size_t *free)
{
    CPSR_ALLOC();
    size_t sp = 0;

    RHINO_CRITICAL_ENTER();

    if (task == NULL || task == g_active_task[cpu_cur_get()]) {
        task = g_active_task[cpu_cur_get()];
        sp = soc_get_cur_sp();
    } else {
        sp = (size_t)task->task_stack;
    }

    if (sp == 0) {
        RHINO_CRITICAL_EXIT();
        k_err_proc(RHINO_SYS_SP_ERR);
        return RHINO_SYS_SP_ERR;
    }

    if ((size_t)(task->task_stack_base + task->stack_size) < sp) {
        RHINO_CRITICAL_EXIT();
        k_err_proc(RHINO_TASK_STACK_OVF);
        return RHINO_TASK_STACK_OVF;
    }

    *free = ((size_t)(task->task_stack_base + task->stack_size) - sp) / sizeof(
                cpu_stack_t);

    RHINO_CRITICAL_EXIT();
    return RHINO_SUCCESS;
}
#endif

#if (RHINO_CONFIG_TASK_PRI_CHG > 0)
kstat_t task_pri_change(ktask_t *task, uint8_t new_pri)
{ /* 待调整的任务可能处于就绪态/睡眠态/PEND态 */
    uint8_t  old_pri;
    kmutex_t *mutex_tmp;
    ktask_t  *mutex_task;

    do {
        if (task->prio != new_pri) {
            switch (task->task_state) {
                case K_RDY: /* 待调整任务处于就绪态,只调整此任务的优先级即可 */
                    ready_list_rm(&g_ready_queue, task); /* 把待调整优先级的任务从ready queue中删除 */
                    task->prio = new_pri;

                    if (task == g_active_task[cpu_cur_get()]) {
                        ready_list_add_head(&g_ready_queue, task); /* 如果待调整任务当前正在执行，则把任务放到队首 */
                    } else {
                        ready_list_add_tail(&g_ready_queue, task); /* 如果待调整任务当前没有在执行，则把任务放到队尾 */
                    }

                    task = NULL; /* 只调整此任务 */
                    break;
                case K_SLEEP: /* 待调整任务处于睡眠态 */
                case K_SUSPENDED: /* 挂起态 */
                case K_SLEEP_SUSPENDED: /* 睡眠挂起态 */
                    /* set new task prio */
                    task->prio = new_pri; /* 直接更改优先级 */
                    task = NULL; /* 只调整此任务 */
                    break;
                case K_PEND:
                case K_PEND_SUSPENDED: /* 如果持有互斥锁的任务因为其他一些原因被阻塞(等待其他的互斥锁/信号量) */
                    old_pri    = task->prio;
                    task->prio = new_pri; /* 首先无条件更新此任务的优先级 */
                    pend_list_reorder(task); /* 然后调整任务在其所在阻塞队列的位置 */

                    if (task->blk_obj->obj_type == RHINO_MUTEX_OBJ_TYPE) { /* 如果此任务因为持有其他互斥锁而被阻塞 */
                        mutex_tmp  = (kmutex_t *)(task->blk_obj); /* 还要再调整其持有的互斥锁阻塞队列上的任务优先级 */
                        mutex_task = mutex_tmp->mutex_task;

                        if (mutex_task->prio > task->prio) { /* 如果其他互斥锁持有者的优先级更高 */
                            /* since the highest prio of the lock wait task
                            became higher, raise the lock get task prio
                            higher */
                            task = mutex_task; /* 在下一个循环中调整其优先级 */
                        } else if (mutex_task->prio == old_pri) {
                            /* find suitable tcb prio */ /* 找到相同的优先级的任务 */
                            new_pri = mutex_pri_look(mutex_task, 0);

                            if (new_pri != mutex_task->prio) {
                                /* Change prio of lock get task */
                                task = mutex_task;
                            } else {
                                task = NULL;
                            }
                        } else { /* 停止遍历 */
                            task = NULL;
                        }
                    } else {
                        task = NULL;
                    }

                    break;
                default:
                    k_err_proc(RHINO_INV_TASK_STATE);
                    return RHINO_INV_TASK_STATE;
            }
        } else {
            task = NULL;
        }
    } while (task != NULL);

    return RHINO_SUCCESS;
}

kstat_t krhino_task_pri_change(ktask_t *task, uint8_t pri, uint8_t *old_pri)
{
    CPSR_ALLOC();

    uint8_t pri_limit;
    kstat_t error;

    NULL_PARA_CHK(task);
    NULL_PARA_CHK(old_pri);

    /* idle task is not allowed to change prio */
    if (task->prio >= RHINO_IDLE_PRI) {
        return RHINO_PRI_CHG_NOT_ALLOWED;
    }

    /* not allowed change to idle prio */
    if (pri >= RHINO_IDLE_PRI) {
        return RHINO_PRI_CHG_NOT_ALLOWED;
    }

    /* deleted task is not allowed to change prio */
    if (task->task_state == K_DELETED) {
        return RHINO_INV_TASK_STATE;
    }

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    /* limit the prio change by mutex at task prio change */
    pri_limit = mutex_pri_limit(task, pri);

    task->b_prio = pri;
    /* new pripority may change here */
    pri      = pri_limit;
    *old_pri = task->prio;

    error = task_pri_change(task, pri);

    if (error != RHINO_SUCCESS) {
        RHINO_CRITICAL_EXIT();
        return error;
    }

    TRACE_TASK_PRI_CHANGE(g_active_task[cpu_cur_get()], task, pri);

    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}
#endif

#if (RHINO_CONFIG_TASK_WAIT_ABORT > 0)
kstat_t krhino_task_wait_abort(ktask_t *task)
{
    CPSR_ALLOC();

    NULL_PARA_CHK(task);

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    switch (task->task_state) {
        case K_RDY:
            break;
        case K_SUSPENDED:
            /* change to ready state */
            task->task_state = K_RDY;
            ready_list_add(&g_ready_queue, task);
            break;
        case K_SLEEP:
        case K_SLEEP_SUSPENDED:
            /* change to ready state */
            tick_list_rm(task);
            ready_list_add(&g_ready_queue, task);
            task->task_state = K_RDY;
            task->blk_state  = BLK_ABORT;
            break;
        case K_PEND_SUSPENDED:
        case K_PEND:
            /* remove task on the tick list because task is woken up */
            tick_list_rm(task);
            /* remove task on the block list because task is woken up */
            klist_rm(&task->task_list);
            /* add to the ready list again */
            ready_list_add(&g_ready_queue, task);
            task->task_state = K_RDY;
            task->blk_state  = BLK_ABORT;

            mutex_task_pri_reset(task);
            task->blk_obj = NULL;

            break;
        default:
            RHINO_CRITICAL_EXIT();
            return  RHINO_INV_TASK_STATE;
    }

#if (RHINO_CONFIG_USER_HOOK > 0)
    krhino_task_abort_hook(task);
#endif

    TRACE_TASK_WAIT_ABORT(g_active_task[cpu_cur_get()], task);

    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}
#endif

#if (RHINO_CONFIG_TASK_DEL > 0)
static void task_mutex_free(ktask_t *task)
{
    kmutex_t *mutex;
    kmutex_t *next_mutex;
    ktask_t  *next_task;
    klist_t *blk_list_head;

    next_mutex = task->mutex_list;

    while ((mutex = next_mutex) != NULL) {
        next_mutex = mutex->mutex_list;

        blk_list_head = &mutex->blk_obj.blk_list;

        if (!is_klist_empty(blk_list_head)) {
            next_task = krhino_list_entry(blk_list_head->next, ktask_t, task_list);

            /* wakeup wait task */
            pend_task_wakeup(next_task);

            /* change mutex get task */
            mutex->mutex_task     = next_task;
            mutex->mutex_list     = next_task->mutex_list;
            next_task->mutex_list = mutex;
        } else {
            /* no wait task */
            mutex->mutex_task = NULL;
        }
    }
}

kstat_t krhino_task_del(ktask_t *task)
{
    CPSR_ALLOC();

    uint8_t    cur_cpu_num;

#if (RHINO_CONFIG_USER_HOOK > 0)
    res_free_t *res_free;
#endif

    RHINO_CRITICAL_ENTER();
    cur_cpu_num = cpu_cur_get();

    INTRPT_NESTED_LEVEL_CHK();

    if (task == NULL) {
        task = g_active_task[cur_cpu_num];
    }

    if (task->prio == RHINO_IDLE_PRI) {
        return RHINO_TASK_DEL_NOT_ALLOWED;
    }

    if (task->mm_alloc_flag != K_OBJ_STATIC_ALLOC) {
        RHINO_CRITICAL_EXIT();
        return RHINO_KOBJ_DEL_ERR;
    }

#if (RHINO_CONFIG_CPU_NUM > 1)
    if (task->cpu_num != cur_cpu_num) {
        if (task->cur_exc == 1) {
            RHINO_CRITICAL_EXIT();
            return RHINO_TRY_AGAIN;
        }
    }
#endif

    if (task == g_active_task[cpu_cur_get()]) {
        if (g_sched_lock[cpu_cur_get()] > 0u) {
            RHINO_CRITICAL_EXIT();
            return RHINO_SCHED_DISABLE;
        }
    }

    /* free all the mutex which task hold */
    task_mutex_free(task);

    switch (task->task_state) {
        case K_RDY:
            ready_list_rm(&g_ready_queue, task);
            task->task_state = K_DELETED;
            break;
        case K_SUSPENDED:
            task->task_state = K_DELETED;
            break;
        case K_SLEEP:
        case K_SLEEP_SUSPENDED:
            tick_list_rm(task);
            task->task_state = K_DELETED;
            break;
        case K_PEND:
        case K_PEND_SUSPENDED:
            tick_list_rm(task);
            klist_rm(&task->task_list);
            task->task_state = K_DELETED;

            mutex_task_pri_reset(task);
            break;
        default:
            RHINO_CRITICAL_EXIT();
            return RHINO_INV_TASK_STATE;
    }

#if (RHINO_CONFIG_SYSTEM_STATS > 0)
    klist_rm(&task->task_stats_item);
#endif

    TRACE_TASK_DEL(g_active_task[cur_cpu_num], task);

#if (RHINO_CONFIG_USER_HOOK > 0)
#if (RHINO_CONFIG_CPU_STACK_DOWN > 0)
    res_free = (res_free_t *)(task->task_stack_base + 1u);
#else
    res_free = (res_free_t *)(task->task_stack_base + task->stack_size - (sizeof(res_free_t) / sizeof(cpu_stack_t)) - 1u);
#endif
    res_free->cnt = 0;
    krhino_task_del_hook(task, res_free);
#endif

    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}

#if (RHINO_CONFIG_KOBJ_DYN_ALLOC > 0)
kstat_t krhino_task_dyn_del(ktask_t *task)
{
    CPSR_ALLOC();

    kstat_t    ret;
    uint8_t    cur_cpu_num;
    res_free_t *res_free;

    RHINO_CRITICAL_ENTER();

    cur_cpu_num = cpu_cur_get();

    INTRPT_NESTED_LEVEL_CHK();

    if (task == NULL) {
        task = g_active_task[cur_cpu_num];
    }

    if (task->prio == RHINO_IDLE_PRI) {
        RHINO_CRITICAL_EXIT();

        return RHINO_TASK_DEL_NOT_ALLOWED;
    }

    if (task->mm_alloc_flag != K_OBJ_DYN_ALLOC) {
        RHINO_CRITICAL_EXIT();

        return RHINO_KOBJ_DEL_ERR;
    }

#if (RHINO_CONFIG_CPU_NUM > 1)
    if (task->cpu_num != cur_cpu_num) {
        if (task->cur_exc == 1) {
            RHINO_CRITICAL_EXIT();
            return RHINO_TRY_AGAIN;
        }
    }
#endif

    if (task == g_active_task[cpu_cur_get()]) {
        if (g_sched_lock[cpu_cur_get()] > 0u) {
            RHINO_CRITICAL_EXIT();
            return RHINO_SCHED_DISABLE;
        }
    }

    if (task->task_state == K_DELETED) {
        RHINO_CRITICAL_EXIT();
        return RHINO_INV_TASK_STATE;
    }

#if (RHINO_CONFIG_CPU_STACK_DOWN > 0)
    res_free = (res_free_t *)(task->task_stack_base + 1u);
#else
    res_free = (res_free_t *)(task->task_stack_base + task->stack_size - (sizeof(res_free_t) / sizeof(cpu_stack_t)) - 1u);
#endif
    res_free->cnt = 0;
    g_sched_lock[cpu_cur_get()]++;
    klist_insert(&g_res_list, &res_free->res_list);
    res_free->res[0] = task->task_stack_base;
    res_free->res[1] = task;
    res_free->cnt += 2;
    ret = krhino_sem_give(&g_res_sem);
    g_sched_lock[cpu_cur_get()]--;

    if (ret != RHINO_SUCCESS) {
        RHINO_CRITICAL_EXIT();
        k_err_proc(RHINO_SYS_SP_ERR);
        return ret;
    }

    /* free all the mutex which task hold */
    task_mutex_free(task);

    switch (task->task_state) {
        case K_RDY:
            ready_list_rm(&g_ready_queue, task);
            task->task_state = K_DELETED;
            break;
        case K_SUSPENDED:
            task->task_state = K_DELETED;
            break;
        case K_SLEEP:
        case K_SLEEP_SUSPENDED:
            tick_list_rm(task);
            task->task_state = K_DELETED;
            break;
        case K_PEND:
        case K_PEND_SUSPENDED:
            tick_list_rm(task);
            klist_rm(&task->task_list);
            task->task_state = K_DELETED;
            mutex_task_pri_reset(task);
            break;
        case K_SEED:
        default:
            break;
    }

#if (RHINO_CONFIG_SYSTEM_STATS > 0)
    klist_rm(&task->task_stats_item);
#endif

    TRACE_TASK_DEL(g_active_task[cpu_cur_get()], task);

#if (RHINO_CONFIG_USER_HOOK > 0)
    krhino_task_del_hook(task, res_free);
#endif

    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}
#endif

#endif

#if (RHINO_CONFIG_SCHED_RR > 0)
kstat_t krhino_task_time_slice_set(ktask_t *task, size_t slice)
{
    CPSR_ALLOC();

    NULL_PARA_CHK(task);

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    if (slice > 0u) {
        /* assign the new time slice */
        task->time_total = slice;
    } else {
        /* assign the default time slice */
        task->time_total = RHINO_CONFIG_TIME_SLICE_DEFAULT;
    }

    task->time_slice = task->time_total;

    RHINO_CRITICAL_EXIT();

    return RHINO_SUCCESS;
}

kstat_t krhino_sched_policy_set(ktask_t *task, uint8_t policy)
{
    CPSR_ALLOC();

    NULL_PARA_CHK(task);

    if ((policy != KSCHED_FIFO) && (policy != KSCHED_RR)) {
        return RHINO_INV_SCHED_WAY;
    }

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    task->sched_policy = policy;
    RHINO_CRITICAL_EXIT();

    return RHINO_SUCCESS;
}

kstat_t krhino_sched_policy_get(ktask_t *task, uint8_t *policy)
{
    CPSR_ALLOC();

    NULL_PARA_CHK(task);
    NULL_PARA_CHK(policy);

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    *policy = task->sched_policy;
    RHINO_CRITICAL_EXIT();

    return RHINO_SUCCESS;
}
#endif

#if (RHINO_CONFIG_TASK_INFO > 0)
kstat_t krhino_task_info_set(ktask_t *task, size_t idx, void *info)
{
    CPSR_ALLOC();

    NULL_PARA_CHK(task);

    if (idx >= RHINO_CONFIG_TASK_INFO_NUM) {
        return RHINO_INV_PARAM;
    }

    RHINO_CPU_INTRPT_DISABLE();
    task->user_info[idx] = info;
    RHINO_CPU_INTRPT_ENABLE();

    return RHINO_SUCCESS;
}

kstat_t krhino_task_info_get(ktask_t *task, size_t idx, void **info)
{
    NULL_PARA_CHK(task);
    NULL_PARA_CHK(info);

    if (idx >= RHINO_CONFIG_TASK_INFO_NUM) {
        return RHINO_INV_PARAM;
    }

    *info = task->user_info[idx];

    return RHINO_SUCCESS;
}
#endif

void krhino_task_deathbed(void)
{
#if (RHINO_CONFIG_TASK_DEL > 0)
    ktask_t *task;

    task = krhino_cur_task_get();

    if (task->mm_alloc_flag == K_OBJ_DYN_ALLOC) {
        /* del my self*/
#if (RHINO_CONFIG_KOBJ_DYN_ALLOC > 0)
        krhino_task_dyn_del(NULL);
#endif
    } else {
        krhino_task_del(NULL);
    }
#else

    while (1) {
        krhino_task_sleep(RHINO_CONFIG_TICKS_PER_SECOND * 10);
    }
#endif
}

