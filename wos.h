#ifndef __WOS_H
#define __WOS_H

#include <stdint.h>

// configs

#define TASKS_STACK_POOL_SIZE 0x2000
#define MAX_TASKS             10

#define QUEUES_DATA_POOL_SIZE 0x400
#define MAX_QUEUES            5

// defs

#define NO_TIMEOUT 0xffffffff

// return codes

#define RET_OS_START_ALREADY_STARTED             -1
#define RET_OS_START_NO_TASK                     -2
#define RET_OS_START_CANNOT_CREATE_TASKS         -3

#define RET_TASK_CREATE_INSUFFICIENT_STACK_SPACE -1
#define RET_TASK_CREATE_TOO_MANY_TASKS           -2
#define RET_TASK_CREATE_NOT_ALIGNED              -3

#define RET_QUEUE_CREATE_INSUFFICIENT_DATA_SPACE -1
#define RET_QUEUE_CREATE_TOO_MANY_QUEUES         -2

#define RET_QUEUE_TRANS_INVALID                  -1
#define RET_QUEUE_TRANS_FULL                     -2
#define RET_QUEUE_TRANS_EMPTY                    -3
#define RET_QUEUE_TRANS_TIMEOUT                  -4

// types

typedef void (*task_func_t)(void *);

// methods

int32_t os_start(void);

int32_t task_create(task_func_t entry, void *arg, uint32_t stack_size, uint8_t priority);
int32_t task_create_isr(task_func_t entry, void *arg, uint32_t stack_size, uint8_t priority);

void    task_sleep(uint32_t ms);

void    task_exit(void);

void    task_kill(uint16_t task_id);
void    task_kill_isr(uint16_t);

int32_t queue_create(uint32_t item_size, uint32_t length);
int32_t queue_create_isr(uint32_t item_size, uint32_t length);

int32_t queue_push(uint16_t queue_id, void *item, uint32_t timeout);
int32_t queue_push_isr(uint16_t queue_id, void *item);

int32_t queue_pull(uint16_t queue_id, void *item, uint32_t timeout);
int32_t queue_pull_isr(uint16_t queue_id, void *item);

void    queue_close(uint16_t queue_id);
void    queue_close_isr(uint16_t queue_id);

void enter_critical(void);
void leave_critical(void);

#endif
