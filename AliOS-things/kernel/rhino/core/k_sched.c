/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <k_api.h>

#if (RHINO_CONFIG_DISABLE_SCHED_STATS > 0)
static void sched_disable_measure_start(void)
{
    /* start measure system lock time */
    if (g_sched_lock[cpu_cur_get()] == 0u) {
        g_sched_disable_time_start = HR_COUNT_GET();
    }
}

static void sched_disable_measure_stop(void)
{
    hr_timer_t diff;

    /* stop measure system lock time, g_sched_lock is always zero here */
    diff = HR_COUNT_GET() - g_sched_disable_time_start;

    if (g_sched_disable_max_time < diff) {
        g_sched_disable_max_time = diff;
    }

    if (g_cur_sched_disable_max_time < diff) {
        g_cur_sched_disable_max_time = diff;
    }
}
#endif

kstat_t krhino_sched_disable(void)
{
    CPSR_ALLOC();

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    if (g_sched_lock[cpu_cur_get()] >= SCHED_MAX_LOCK_COUNT) {
        RHINO_CRITICAL_EXIT();
        return RHINO_SCHED_LOCK_COUNT_OVF;
    }

#if (RHINO_CONFIG_DISABLE_SCHED_STATS > 0)
    sched_disable_measure_start();
#endif

    g_sched_lock[cpu_cur_get()]++;

    RHINO_CRITICAL_EXIT();

    return RHINO_SUCCESS;
}

kstat_t krhino_sched_enable(void)
{
    CPSR_ALLOC();

    RHINO_CRITICAL_ENTER();

    INTRPT_NESTED_LEVEL_CHK();

    if (g_sched_lock[cpu_cur_get()] == 0u) {
        RHINO_CRITICAL_EXIT();
        return RHINO_SCHED_ALREADY_ENABLED;
    }

    g_sched_lock[cpu_cur_get()]--;

    if (g_sched_lock[cpu_cur_get()] > 0u) {
        RHINO_CRITICAL_EXIT();
        return RHINO_SCHED_DISABLE;
    }

#if (RHINO_CONFIG_DISABLE_SCHED_STATS > 0)
    sched_disable_measure_stop();
#endif

    RHINO_CRITICAL_EXIT_SCHED();

    return RHINO_SUCCESS;
}

#if (RHINO_CONFIG_CPU_NUM > 1)
void core_sched(void)
{
    uint8_t cur_cpu_num;

    cur_cpu_num = cpu_cur_get();

    if (g_intrpt_nested_level[cur_cpu_num] > 0u) {
        return;
    }

    if (g_sched_lock[cur_cpu_num] > 0u) {
        return;
    }

    preferred_cpu_ready_task_get(&g_ready_queue, cur_cpu_num);

    /* if preferred task is currently task, then no need to do switch and just return */
    if (g_preferred_ready_task[cur_cpu_num] == g_active_task[cur_cpu_num]) {
        return;
    }

    TRACE_TASK_SWITCH(g_active_task[cur_cpu_num], g_preferred_ready_task[cur_cpu_num]);

#if (RHINO_CONFIG_USER_HOOK > 0)
    krhino_task_switch_hook(g_active_task[cur_cpu_num], g_preferred_ready_task[cur_cpu_num]);
#endif

    g_active_task[cur_cpu_num]->cur_exc = 0;

    cpu_task_switch();

}
#else //(RHINO_CONFIG_CPU_NUM > 1)
void core_sched(void)
{
    CPSR_ALLOC();
    uint8_t cur_cpu_num;

    RHINO_CPU_INTRPT_DISABLE();

    cur_cpu_num = cpu_cur_get();

    if (g_intrpt_nested_level[cur_cpu_num] > 0u) {
        RHINO_CPU_INTRPT_ENABLE();
        return;
    }

    if (g_sched_lock[cur_cpu_num] > 0u) {
        RHINO_CPU_INTRPT_ENABLE();
        return;
    }

    /* 把根据调度算法调度出来的任务存到g_prefered_ready_task */
    preferred_cpu_ready_task_get(&g_ready_queue, cur_cpu_num); 

    /* if preferred task is currently task, then no need to do switch and just return */
    if (g_preferred_ready_task[cur_cpu_num] == g_active_task[cur_cpu_num]) {
        RHINO_CPU_INTRPT_ENABLE(); /* MSR开中断 */
        return;
    }

    TRACE_TASK_SWITCH(g_active_task[cur_cpu_num], g_preferred_ready_task[cur_cpu_num]);

#if (RHINO_CONFIG_USER_HOOK > 0)
    krhino_task_switch_hook(g_active_task[cur_cpu_num], g_preferred_ready_task[cur_cpu_num]);
#endif

    cpu_task_switch();
/*
cpu_task_switch:(armv7)
LDR  R0, =SCB_ICSR       //SCB_ICSR EQU 0xE000ED04 ; Interrupt Control and State Register.
LDR	 R1, =ICSR_PENDSVSET //ICSR_PENDSVSET EQU 0x10000000 ; Value to trigger PendSV exception.
STR	 R1, [R0]            //R1 -> *R0
BX 	 LR                  //无条件跳转

cpu_task_switch:(linux)
swap_context()...
*/
    RHINO_CPU_INTRPT_ENABLE();
}
#endif

void runqueue_init(runqueue_t *rq)
{
    uint8_t prio;

    rq->highest_pri = RHINO_CONFIG_PRI_MAX;

    for (prio = 0; prio < RHINO_CONFIG_PRI_MAX; prio++) {
        rq->cur_list_item[prio] = NULL;
    }
}

RHINO_INLINE void ready_list_init(runqueue_t *rq, ktask_t *task)
{
    rq->cur_list_item[task->prio] = &task->task_list;
    klist_init(rq->cur_list_item[task->prio]);
    krhino_bitmap_set(rq->task_bit_map, task->prio);

    if ((task->prio) < (rq->highest_pri)) {
        rq->highest_pri = task->prio;
    }
}

RHINO_INLINE uint8_t is_ready_list_empty(uint8_t prio)
{
    return (g_ready_queue.cur_list_item[prio] == NULL);
}

RHINO_INLINE void _ready_list_add_tail(runqueue_t *rq, ktask_t *task)
{
    if (is_ready_list_empty(task->prio)) {
        ready_list_init(rq, task);
        return;
    }

    klist_insert(rq->cur_list_item[task->prio], &task->task_list);
}

RHINO_INLINE void _ready_list_add_head(runqueue_t *rq, ktask_t *task)
{
    if (is_ready_list_empty(task->prio)) {
        ready_list_init(rq, task);
        return;
    }

    klist_insert(rq->cur_list_item[task->prio], &task->task_list);
    rq->cur_list_item[task->prio] = &task->task_list;
}

#if (RHINO_CONFIG_CPU_NUM > 1)
static void task_sched_to_cpu(runqueue_t *rq, ktask_t *task, uint8_t cur_cpu_num)
{
    uint8_t i;
    uint8_t low_pri;

    (void)rq;

    if (g_sys_stat == RHINO_RUNNING) {
        if (task->cpu_binded == 1) {
            if (task->cpu_num != cur_cpu_num) {
                if (task->prio <= g_active_task[task->cpu_num]->prio) {
                    cpu_signal(task->cpu_num);
                }
            }
        } else {
            /* find the lowest pri */
            low_pri = g_active_task[0]->prio;
            for (i = 0; i < RHINO_CONFIG_CPU_NUM - 1; i++) {
                if (low_pri < g_active_task[i + 1]->prio) {
                     low_pri = g_active_task[i + 1]->prio;
                }
            }

            /* which cpu run the lowest pri, just notify it */
            for (i = 0; i < RHINO_CONFIG_CPU_NUM; i++) {
                if (low_pri == g_active_task[i]->prio) {
                    if (i != cur_cpu_num) {
                        cpu_signal(i);
                    }
                    return;
                }
            }
        }
    }
}

void ready_list_add_head(runqueue_t *rq, ktask_t *task)
{
    _ready_list_add_head(rq, task);
    task_sched_to_cpu(rq, task, cpu_cur_get());
}

void ready_list_add_tail(runqueue_t *rq, ktask_t *task)
{
    _ready_list_add_tail(rq, task);
    task_sched_to_cpu(rq, task, cpu_cur_get());
}

#else
void ready_list_add_head(runqueue_t *rq, ktask_t *task)
{
    _ready_list_add_head(rq, task);
}

void ready_list_add_tail(runqueue_t *rq, ktask_t *task)
{
    _ready_list_add_tail(rq, task);
}
#endif

void ready_list_add(runqueue_t *rq, ktask_t *task)
{
    /* if task prio is equal current task prio then add to the end */
    if (task->prio == g_active_task[cpu_cur_get()]->prio) {
        ready_list_add_tail(rq, task);
    } else {
        ready_list_add_head(rq, task);
    }
}

void ready_list_rm(runqueue_t *rq, ktask_t *task)
{
    int32_t  i;
    uint8_t  pri = task->prio;

    /* if the ready list is not only one, we do not need to update the highest prio */
    if ((rq->cur_list_item[pri]) != (rq->cur_list_item[pri]->next)) { /* 待删除任务所在队列不止一个任务 */
        if (rq->cur_list_item[pri] == &task->task_list) {
            rq->cur_list_item[pri] = rq->cur_list_item[pri]->next;
        }

        klist_rm(&task->task_list); /* 将此任务从队列删除 */
        return;
    }

    /* only one item,just set cur item ptr to NULL */
    rq->cur_list_item[pri] = NULL; /* 队列只有一个任务，删除任务之后清空任务 */

    krhino_bitmap_clear(rq->task_bit_map, pri); /* 清除优先级位图 */

    /* if task prio not equal to the highest prio, then we do not need to update the highest prio */
    /* this condition happens when a current high prio task to suspend a low priotity task */
    if (pri != rq->highest_pri) { /* 不用调整运行队列最高优先级 */
        return;
    }

    /* find the highest ready task */
    i = krhino_find_first_bit(rq->task_bit_map); /* 删除的任务原本是最高优先级，删除之后找次最高优先级 */

    /* update the next highest prio task */
    if (i >= 0) { /* 更新运行队列最高优先级为次最高优先级 */
        rq->highest_pri = i;
    } else {
        k_err_proc(RHINO_SYS_FATAL_ERR);
    }
}

void ready_list_head_to_tail(runqueue_t *rq, ktask_t *task)
{
    rq->cur_list_item[task->prio] = rq->cur_list_item[task->prio]->next;
}

#if (RHINO_CONFIG_CPU_NUM > 1)
void preferred_cpu_ready_task_get(runqueue_t *rq, uint8_t cpu_num)
{
    klist_t *iter;
    ktask_t *task;
    uint32_t task_bit_map[NUM_WORDS];
    klist_t *node;
    uint8_t flag;
    uint8_t  highest_pri = rq->highest_pri;

    node = rq->cur_list_item[highest_pri];
    iter = node;
    memcpy(task_bit_map, rq->task_bit_map, NUM_WORDS * sizeof(uint32_t));

    while (1) {

        task = krhino_list_entry(iter, ktask_t, task_list); /* 找到当前优先级队列中的第一个任务 */

        if (g_active_task[cpu_num] == task) {/*优先级队列没有变化的情况下，快速返回*/
            break;
        }

        flag = ((task->cur_exc == 0) && (task->cpu_binded == 0))
               || ((task->cur_exc == 0) && (task->cpu_binded == 1) && (task->cpu_num == cpu_num));

        if (flag > 0) { /* 当前task没有绑定到某个核，或者绑定到了当前核 */
            task->cpu_num = cpu_num;
            task->cur_exc = 1;
            g_preferred_ready_task[cpu_num] = task; /*把当前优先级最高的任务放到g_preferred_ready_task中*/
            break;
        }

        if (iter->next == rq->cur_list_item[highest_pri]) {/* 如果当前优先级队列遍历完了 */
            task_bit_map[highest_pri >> 5] &= ~(1u << (31u - (highest_pri & 31u))); /* 把当前优先级在bitmap中清了 */
            highest_pri = krhino_find_first_bit(task_bit_map); /* 然后找下一个存在的优先级 */
            iter = rq->cur_list_item[highest_pri]; /* 找到此优先级队列iter. */
        } else {/* 找当前优先级队列中的下一个任务 */
            iter = iter->next;
        }
    }
}
#else //(RHINO_CONFIG_CPU_NUM > 1)
void preferred_cpu_ready_task_get(runqueue_t *rq, uint8_t cpu_num)
{
    klist_t *node = rq->cur_list_item[rq->highest_pri]; /* 单核情况下，不用考虑任务绑定核的问题，直接返回最高优先级的任务 */
    /* get the highest prio task object */
    g_preferred_ready_task[cpu_num] = krhino_list_entry(node, ktask_t, task_list);
}
#endif

#if (RHINO_CONFIG_SCHED_RR > 0)

#if (RHINO_CONFIG_CPU_NUM > 1)

static void _time_slice_update(ktask_t *task, uint8_t i)
{
    klist_t *head;

    head = g_ready_queue.cur_list_item[task->prio];

    /* if ready list is empty then just return because nothing is to be caculated */
    if (is_ready_list_empty(task->prio)) {
        return;
    }

    if (task->sched_policy == KSCHED_FIFO) { /* KSCHED_FIFO / KSCHED_RR */
        return;
    }

    /* there is only one task on this ready list, so do not need to caculate time slice */
    /* idle task must satisfy this condition */
    if (head->next == head) { /* 如果该优先级队列中只有一个任务，则无需调整时间片 */
        return;
    }

    if (task->time_slice > 0u) {
        task->time_slice--;
    }

    /* if current active task has time_slice, just return */
    if (task->time_slice > 0u) {
        return;
    }

    /* move current active task to the end of ready list for the same prio */
    ready_list_head_to_tail(&g_ready_queue, task); /* 如果时间片用完了，则把当前任务移到队尾 */

    /* restore the task time slice */
    task->time_slice = task->time_total; /* 然后恢复其时间片 */

    if (i != cpu_cur_get()) {
        cpu_signal(i);
    }

}

void time_slice_update(void)
{
    CPSR_ALLOC();
    uint8_t i;

    RHINO_CRITICAL_ENTER();

    for (i = 0; i < RHINO_CONFIG_CPU_NUM; i++) {
        _time_slice_update(g_active_task[i], i); /* 调整每个cpu当前正在运行的任务 */
    }

    RHINO_CRITICAL_EXIT();
}


#else
void time_slice_update(void)
{
    CPSR_ALLOC();

    ktask_t *task;
    klist_t *head;
    uint8_t  task_pri;

    RHINO_CRITICAL_ENTER();
    task_pri = g_active_task[cpu_cur_get()]->prio;

    head = g_ready_queue.cur_list_item[task_pri];

    /* if ready list is empty then just return because nothing is to be caculated */
    if (is_ready_list_empty(task_pri)) {
        RHINO_CRITICAL_EXIT();
        return;
    }

    /* Always look at the first task on the ready list */
    task = krhino_list_entry(head, ktask_t, task_list);

    if (task->sched_policy == KSCHED_FIFO) { /* FIFO策略下，不做任何事情 */
        RHINO_CRITICAL_EXIT();
        return;
    }

    /* there is only one task on this ready list, so do not need to caculate time slice */
    /* idle task must satisfy this condition */
    if (head->next == head) { /* 当前优先级队列只有一个任务 */
        RHINO_CRITICAL_EXIT();
        return;
    }

    if (task->time_slice > 0u) {
        task->time_slice--;
    }

    /* if current active task has time_slice, just return */
    if (task->time_slice > 0u) { /* 调整完，仍然有时间片可用 */
        RHINO_CRITICAL_EXIT();
        return;
    }

    /* move current active task to the end of ready list for the same prio */
    ready_list_head_to_tail(&g_ready_queue, task); /* 时间片用完，把任务挪到队尾 */

    /* restore the task time slice */
    task->time_slice = task->time_total; /* 并调整时间片为初始量 */

    RHINO_CRITICAL_EXIT();
}
#endif

#endif

