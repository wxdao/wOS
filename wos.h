#ifndef __WOS_H
#define __WOS_H

#include <stddef.h>

// configs

#define HEAP_SIZE 0x2000

#define MAX_TASKS 10

#define MAX_QUEUES 5

// defs

#define NO_TIMEOUT 0xffffffff

// return codes

#define RET_OS_INIT_HEAP_FAILED -1

#define RET_OS_START_ALREADY_STARTED -1
#define RET_OS_START_NO_TASK -2
#define RET_OS_START_CANNOT_CREATE_TASKS -3

#define RET_TASK_CREATE_INSUFFICIENT_STACK_SPACE -1
#define RET_TASK_CREATE_TOO_MANY_TASKS -2
#define RET_TASK_CREATE_NOT_ALIGNED -3

#define RET_QUEUE_CREATE_INSUFFICIENT_DATA_SPACE -1
#define RET_QUEUE_CREATE_TOO_MANY_QUEUES -2

#define RET_QUEUE_TRANS_INVALID -1
#define RET_QUEUE_TRANS_FULL -2
#define RET_QUEUE_TRANS_EMPTY -3
#define RET_QUEUE_TRANS_TIMEOUT -4

// types

typedef int task_id_t;

typedef int queue_id_t;

typedef void (*task_func_t)(void *);

// methods

int os_init(void);

int os_start(void);

task_id_t task_create(task_func_t entry, void *arg, size_t stack_size,
                      int priority);
task_id_t task_create_isr(task_func_t entry, void *arg, size_t stack_size,
                          int priority);

void task_sleep(unsigned int ticks);

void task_exit(void);

void task_kill(task_id_t task_id);
void task_kill_isr(task_id_t task_id);

queue_id_t queue_create(size_t item_size, size_t length);
queue_id_t queue_create_isr(size_t item_size, size_t length);

int queue_push(queue_id_t queue_id, void *item, unsigned int timeout);
int queue_push_isr(queue_id_t queue_id, void *item);

int queue_pull(queue_id_t queue_id, void *item, unsigned int timeout);
int queue_pull_isr(queue_id_t queue_id, void *item);

void queue_close(queue_id_t queue_id);
void queue_close_isr(queue_id_t queue_id);

void enter_critical(void);
void leave_critical(void);



#endif
