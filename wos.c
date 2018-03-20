#include "wos.h"

#include "heap.h"

#include <stdint.h>

#include "stm32f10x.h"

#define SYSCALL_OS_START 0
#define SYSCALL_TASK_CREATE 1
#define SYSCALL_TASK_SLEEP 2
#define SYSCALL_TASK_EXIT 3
#define SYSCALL_TASK_KILL 4
#define SYSCALL_QUEUE_CREATE 5
#define SYSCALL_QUEUE_PUSH 6
#define SYSCALL_QUEUE_PULL 7
#define SYSCALL_QUEUE_CLOSE 8

#define TASK_STATE_FREE 0
#define TASK_STATE_RUNNING 1
#define TASK_STATE_READY 2
#define TASK_STATE_SLEEPING 3
#define TASK_WAITING_TO_PUSH 5
#define TASK_WAITING_TO_PULL 6

#define QUEUE_STATE_FREE 0
#define QUEUE_STATE_NORMAL 1

// contexts defs

typedef struct {
  int32_t r0;
  int32_t r1;
  int32_t r2;
  int32_t r3;
  int32_t r12;
  uint32_t lr;
  uint32_t pc;
  uint32_t xpsr;
} interrupt_context_t;

typedef struct {
  int32_t r4;
  int32_t r5;
  int32_t r6;
  int32_t r7;
  int32_t r8;
  int32_t r9;
  int32_t r10;
  int32_t r11;
  interrupt_context_t int_ctx;
} task_context_t;

// control blocks defs

typedef struct {
  task_id_t id;
  int state;
  int priority;
  queue_id_t trans_queue_id;
  void *trans_item;
  unsigned int wait_timeout;
  void *stack_base;
  size_t stack_size;
  task_context_t *ctx;
} task_control_block_t;

typedef struct {
  queue_id_t id;
  int state;
  size_t count;
  size_t head_pos;
  void *data_base;
  size_t item_size;
  size_t length;
} queue_control_block_t;

// params defs

typedef struct {
  task_func_t entry;
  void *arg;
  size_t stack_size;
  int priority;
} task_create_param_t;

typedef struct {
  size_t item_size;
  size_t length;
} queue_create_param_t;

typedef struct {
  queue_id_t queue_id;
  void *item;
  unsigned int timeout;
} queue_trans_param_t;

// os vars

static int os_started = 0;

static unsigned int os_ticks = 0;

static uint8_t heap_pool[HEAP_SIZE] = {0};

heap_t *heap;

// task vars

static task_control_block_t tcbs[MAX_TASKS] = {0};

static task_control_block_t *current_tcb = 0;

// queue vars

static queue_control_block_t qcbs[MAX_QUEUES] = {0};

// static methods

static void *align_stack_ptr_low(void *ptr) {
  return (void *)(8 * (((uintptr_t)ptr) / 8));
}

// implement these here so it doesn't rely on C library
static void *memset(void *s, int c, size_t n) {
  const unsigned char uc = c;
  unsigned char *su;
  for (su = s; 0 < n; ++su, --n)
    *su = uc;
  return s;
}

static void *memmove(void *dst, const void *src, size_t count) {
  char *tmpdst = (char *)dst;
  char *tmpsrc = (char *)src;
  if (tmpdst <= tmpsrc || tmpdst >= tmpsrc + count) {
    while (count--) {
      *tmpdst++ = *tmpsrc++;
    }
  } else {
    tmpdst = tmpdst + count - 1;
    tmpsrc = tmpsrc + count - 1;
    while (count--) {
      *tmpdst-- = *tmpsrc--;
    }
  }
  return dst;
}

// helper inline func to set PendSV exception pending
static inline void set_pendsv(void) { SCB->ICSR = SCB_ICSR_PENDSVSET; }

// yield, which simply set PendSV pending so PendSV_Handler will be invoked upon
// return
static void os_yield(void) { set_pendsv(); }

// 'syscall' wrapper
static int32_t syscall(int32_t _r0, int32_t _r1, int32_t _r2, int32_t _r3) {
  register int32_t r0 asm("r0") = _r0;
  register int32_t r1 asm("r1") = _r1;
  register int32_t r2 asm("r2") = _r2;
  register int32_t r3 asm("r3") = _r3;
  __asm__ volatile("svc #0x80  \n\t"
                   "mov %0, r0 \n\t"
                   : "=r"(r0)
                   : "r"(r1), "r"(r2), "r"(r3)
                   :);
  return r0;
}

// critical region methods

static int critical_depth = 0;

void enter_critical(void) {
  if (critical_depth++ == 0) {
    __set_PRIMASK(1);
  }
}

void leave_critical(void) {
  critical_depth--;
  if (critical_depth < 0) {
    critical_depth = 0;
  }
  if (critical_depth == 0) {
    __set_PRIMASK(0);
  }
}

// resident tasks

// this task is run when no other tasks are ready
static void _task_suspending(void *arg) {
  for (;;) {
    __asm__("wfe");
  }
}

// do methods, which are invoked by handlers only

static task_id_t do_task_create(task_create_param_t *param) {
  if (param->stack_size % 8 != 0) {
    return RET_TASK_CREATE_NOT_ALIGNED;
  }
  enter_critical();
  void *stack_base = heap_alloc(heap, param->stack_size);
  if (stack_base == NULL) {
    leave_critical();
    return RET_TASK_CREATE_INSUFFICIENT_STACK_SPACE;
  }

  // build context
  task_context_t *task_ctx = (task_context_t *)align_stack_ptr_low(
      ((char *)stack_base) + param->stack_size - sizeof(task_context_t));
  memset(task_ctx, 0, sizeof(task_context_t));
  task_ctx->int_ctx.xpsr = 0x01000000;
  task_ctx->int_ctx.pc = (uint32_t)(uint32_t *)param->entry | 1;
  task_ctx->int_ctx.lr = (uint32_t)(uint32_t *)task_exit | 1;
  task_ctx->int_ctx.r0 = (int32_t)(int32_t *)param->arg;

  // build tcb
  task_control_block_t *tcb = 0;
  task_id_t task_id = 0;
  for (; task_id < MAX_TASKS; task_id++) {
    if (tcbs[task_id].state == TASK_STATE_FREE) {
      tcb = &tcbs[task_id];
      break;
    }
  }
  if (tcb == 0) {
    leave_critical();
    return RET_TASK_CREATE_TOO_MANY_TASKS;
  }
  tcb->id = task_id;
  tcb->priority = param->priority;
  tcb->ctx = task_ctx;
  tcb->state = TASK_STATE_READY;
  tcb->stack_base = stack_base;
  tcb->stack_size = param->stack_size;

  leave_critical();
  return tcb->id;
}

// these are simply setting current task's state
static int do_task_sleep(unsigned int timeout) {
  enter_critical();
  current_tcb->wait_timeout = timeout;
  current_tcb->state = TASK_STATE_SLEEPING;
  leave_critical();
  os_yield();
  return 0;
}

static int do_task_exit(void) {
  enter_critical();
  current_tcb->state = TASK_STATE_FREE;
  heap_free(heap, current_tcb->stack_base);
  leave_critical();
  os_yield();
  return 0;
}

static int do_task_kill(task_id_t task_id) {
  enter_critical();
  tcbs[task_id].state = TASK_STATE_FREE;
  heap_free(heap, tcbs[task_id].stack_base);
  leave_critical();
  os_yield();
  return 0;
}

static queue_id_t do_queue_create(queue_create_param_t *param) {
  enter_critical();

  void *data_base = heap_alloc(heap, param->item_size * param->length);
  if (data_base == NULL) {
    leave_critical();
    return RET_QUEUE_CREATE_INSUFFICIENT_DATA_SPACE;
  }

  // build qcb
  queue_control_block_t *qcb = 0;
  queue_id_t queue_id = 0;
  for (; queue_id < MAX_QUEUES; queue_id++) {
    if (qcbs[queue_id].state == QUEUE_STATE_FREE) {
      qcb = &qcbs[queue_id];
      break;
    }
  }
  if (qcb == 0) {
    leave_critical();
    return RET_QUEUE_CREATE_TOO_MANY_QUEUES;
  }

  qcb->id = queue_id;
  qcb->state = QUEUE_STATE_NORMAL;
  qcb->data_base = data_base;
  qcb->item_size = param->item_size;
  qcb->length = param->length;
  qcb->count = 0;

  leave_critical();
  return qcb->id;
}

static void do_queue_tasks_pull(void) {
  enter_critical();
  for (int i = 0; i < MAX_QUEUES; i++) {
    if (qcbs[i].state == QUEUE_STATE_FREE) {
      continue;
    }
    if (qcbs[i].count == 0) {
      continue;
    }
    task_control_block_t *pull_tcbs[MAX_TASKS] = {0};
    int pull_task_count = 0;
    for (int j = 0; j < MAX_TASKS; j++) {
      if (tcbs[j].state != TASK_WAITING_TO_PULL) {
        continue;
      }
      if (tcbs[j].trans_queue_id != qcbs[i].id) {
        continue;
      }
      pull_task_count++;
      pull_tcbs[j] = &tcbs[j];
    }
    while (qcbs[i].count > 0) {
      if (pull_task_count == 0) {
        break;
      }
      int highest_priority_task_i = MAX_TASKS;
      for (int j = 0; j < MAX_TASKS; j++) {
        if (pull_tcbs[j] == 0) {
          continue;
        }
        if (pull_tcbs[j]->priority < highest_priority_task_i) {
          highest_priority_task_i = j;
        }
      }
      // pull
      memmove(pull_tcbs[highest_priority_task_i]->trans_item,
              (char *)qcbs[i].data_base + qcbs[i].head_pos * qcbs[i].item_size,
              qcbs[i].item_size);
      // adjust
      qcbs[i].head_pos = (qcbs[i].head_pos + 1) % qcbs[i].length;
      pull_tcbs[highest_priority_task_i]->ctx->int_ctx.r0 = 0;
      pull_tcbs[highest_priority_task_i]->state = TASK_STATE_READY;

      qcbs[i].count--;
      pull_task_count--;
      pull_tcbs[highest_priority_task_i] = 0;
    }
  }
  leave_critical();
}

static void do_queue_tasks_push(void) {
  enter_critical();
  for (int i = 0; i < MAX_TASKS; i++) {
    if (tcbs[i].state != TASK_WAITING_TO_PUSH) {
      continue;
    }
    queue_control_block_t *qcb = &qcbs[tcbs[i].trans_queue_id];
    if (qcb->state == QUEUE_STATE_FREE) {
      continue;
    }
    if (qcb->count >= qcb->length) {
      continue;
    }
    // push
    memmove(
        (void *)(qcb->data_base +
                 qcb->item_size * ((qcb->count + qcb->head_pos) % qcb->length)),
        tcbs[i].trans_item, qcb->item_size);

    qcb->count++;
    tcbs[i].ctx->int_ctx.r0 = 0;
    tcbs[i].state = TASK_STATE_READY;
  }
  leave_critical();
}

// queue push and pull are similar, and the process to really push/pull is in
// do_os_sche
static int do_queue_push(queue_trans_param_t *param) {
  enter_critical();
  if (qcbs[param->queue_id].state == QUEUE_STATE_FREE) {
    leave_critical();
    return RET_QUEUE_TRANS_INVALID;
  }
  current_tcb->state = TASK_WAITING_TO_PUSH;
  current_tcb->trans_queue_id = param->queue_id;
  current_tcb->trans_item = param->item;
  current_tcb->wait_timeout = param->timeout;
  do_queue_tasks_push();
  do_queue_tasks_pull();
  leave_critical();
  os_yield();
  return 0;
}

static int do_queue_pull(queue_trans_param_t *param) {
  enter_critical();
  if (qcbs[param->queue_id].state == QUEUE_STATE_FREE) {
    leave_critical();
    return RET_QUEUE_TRANS_INVALID;
  }
  current_tcb->state = TASK_WAITING_TO_PULL;
  current_tcb->trans_queue_id = param->queue_id;
  current_tcb->trans_item = param->item;
  current_tcb->wait_timeout = param->timeout;
  do_queue_tasks_pull();
  do_queue_tasks_push();
  leave_critical();
  os_yield();
  return 0;
}

static int do_queue_close(queue_id_t queue_id) {
  enter_critical();
  qcbs[queue_id].state = QUEUE_STATE_FREE;
  heap_free(heap, qcbs[queue_id].data_base);
  for (int i = 0; i < MAX_TASKS; i++) {
    if (tcbs[i].state != TASK_WAITING_TO_PUSH &&
        tcbs[i].state != TASK_WAITING_TO_PULL) {
      continue;
    }
    if (tcbs[i].trans_queue_id != queue_id) {
      continue;
    }
    tcbs[i].ctx->int_ctx.r0 = RET_QUEUE_TRANS_INVALID;
    tcbs[i].state = TASK_STATE_READY;
  }
  leave_critical();
  os_yield();
  return 0;
}

static int do_os_start(void) {
  if (os_started) {
    return RET_OS_START_ALREADY_STARTED;
  }

  // add resident tasks
  task_create_param_t param;
  param.entry = _task_suspending;
  param.arg = (void *)0;
  param.stack_size = 0x100;
  param.priority = 0xff;
  if (do_task_create(&param) < 0) {
    return RET_OS_START_CANNOT_CREATE_TASKS;
  }

  for (int i = 0; i < MAX_TASKS; i++) {
    if (tcbs[i].state == TASK_STATE_READY) {
      current_tcb = &tcbs[i];
      current_tcb->state = TASK_STATE_RUNNING;
      register int r0 asm("r0") = (int)current_tcb->ctx;
      register int r1 asm("r1") = (int)&os_started;
      register int r2 asm("r2") = 1;
      __asm__ volatile("ldmia %0!, {r4-r11} \n\t"
                       "msr psp, %0         \n\t"

                       "str %2, [%1]        \n\t"
                       "ldr %0, =0xFFFFFFFD \n\t"
                       "bx %0               \n\t"
                       :
                       : "r"(r0), "r"(r1), "r"(r2));
      // should not be here
      return 0;
    }
  }
  return RET_OS_START_NO_TASK;
}

// invoked by SysTick_Handler
static void do_os_tick(void) {
  if (!os_started) {
    return;
  }
  os_ticks++;
  for (int i = 0; i < MAX_TASKS; i++) {
    if (tcbs[i].wait_timeout > 0 && tcbs[i].wait_timeout != NO_TIMEOUT) {
      tcbs[i].wait_timeout--;
    }
  }
  os_yield();
}

// invoked by PendSV_Handler
static void do_os_sche(void) {
  // save current task
  task_control_block_t *tcb = current_tcb;
  __asm__ volatile("mrs %0, psp" : "=r"(tcb->ctx));

  int task_i =
      ((uint32_t)current_tcb - (uint32_t)tcbs) / sizeof(task_control_block_t);

  enter_critical();
  // handling
  task_control_block_t *next_tcb = 0;
  // find highest priority ready; handle queue push; handle wait_timeout
  uint8_t highest_priority = 0xff;
  for (int i = 0; i < MAX_TASKS; i++) {
    // handle wait_timeout
    if (tcbs[i].wait_timeout == 0) {
      if (tcbs[i].state == TASK_STATE_SLEEPING) {
        tcbs[i].state = TASK_STATE_READY;
      }
      if (tcbs[i].state == TASK_WAITING_TO_PUSH ||
          tcbs[i].state == TASK_WAITING_TO_PULL) {
        // timeout
        tcbs[i].ctx->int_ctx.r0 = RET_QUEUE_TRANS_TIMEOUT;
        tcbs[i].state = TASK_STATE_READY;
      }
    }
    if (tcbs[i].state != TASK_STATE_READY &&
        tcbs[i].state != TASK_STATE_RUNNING) {
      continue;
    }
    if (tcbs[i].priority < highest_priority) {
      highest_priority = tcbs[i].priority;
    }
  }

  // find next ready task
  int next_task_i = task_i + 1;
  for (; next_task_i < MAX_TASKS; next_task_i++) {
    if (tcbs[next_task_i].priority != highest_priority) {
      continue;
    }
    if (tcbs[next_task_i].state == TASK_STATE_READY) {
      next_tcb = &tcbs[next_task_i];
      break;
    }
  }

  if (next_tcb == 0) {
    next_task_i = 0;
    for (; next_task_i < MAX_TASKS; next_task_i++) {
      if (tcbs[next_task_i].priority != highest_priority) {
        continue;
      }
      if (tcbs[next_task_i].state == TASK_STATE_READY) {
        next_tcb = &tcbs[next_task_i];
        break;
      }
    }
    if (next_tcb == 0) {
      leave_critical();
      return;
    }
  }

  // switch running task
  if (tcb->state == TASK_STATE_RUNNING) {
    tcb->state = TASK_STATE_READY;
  }
  __asm__ volatile("msr psp, %0" : : "r"(next_tcb->ctx));
  next_tcb->state = TASK_STATE_RUNNING;
  current_tcb = next_tcb;
  leave_critical();
}

// queue methods (public methods)

int queue_create(size_t item_size, size_t length) {
  volatile queue_create_param_t param;
  param.item_size = item_size;
  param.length = length;
  return syscall(SYSCALL_QUEUE_CREATE, (int)&param, 0, 0);
}

int queue_push(queue_id_t queue_id, void *item, unsigned int timeout) {
  volatile queue_trans_param_t param;
  param.queue_id = queue_id;
  param.item = item;
  param.timeout = timeout;
  return syscall(SYSCALL_QUEUE_PUSH, (int)&param, 0, 0);
}

int queue_push_isr(queue_id_t queue_id, void *item) {
  enter_critical();
  queue_control_block_t *qcb = &qcbs[queue_id];
  if (qcb->state == QUEUE_STATE_FREE) {
    leave_critical();
    return RET_QUEUE_TRANS_INVALID;
  }
  if (qcb->count >= qcb->length) {
    leave_critical();
    return RET_QUEUE_TRANS_FULL;
  }
  memmove(
      (void *)(qcb->data_base +
               qcb->item_size * ((qcb->count + qcb->head_pos) % qcb->length)),
      item, qcb->item_size);
  qcb->count++;
  do_queue_tasks_pull();
  leave_critical();
  os_yield();
  return 0;
}

int queue_pull(queue_id_t queue_id, void *item, unsigned int timeout) {
  volatile queue_trans_param_t param;
  param.queue_id = queue_id;
  param.item = item;
  param.timeout = timeout;
  return syscall(SYSCALL_QUEUE_PULL, (int)&param, 0, 0);
}

int queue_pull_isr(queue_id_t queue_id, void *item) {
  enter_critical();
  queue_control_block_t *qcb = &qcbs[queue_id];
  if (qcb->state == QUEUE_STATE_FREE) {
    leave_critical();
    return RET_QUEUE_TRANS_INVALID;
  }
  if (qcb->count == 0) {
    leave_critical();
    return RET_QUEUE_TRANS_EMPTY;
  }

  // pull
  memmove(item, (char *)qcb->data_base + qcb->head_pos * qcb->item_size,
          qcb->item_size);
  // adjust
  qcb->head_pos = (qcb->head_pos + 1) % qcb->length;
  qcb->count--;
  do_queue_tasks_push();
  leave_critical();
  os_yield();
  return 0;
}

void queue_close(queue_id_t queue_id) {
  syscall(SYSCALL_QUEUE_CLOSE, queue_id, 0, 0);
}

void queue_close_isr(queue_id_t queue_id) { do_queue_close(queue_id); }

// task methods (public methods)

void task_exit(void) { syscall(SYSCALL_TASK_EXIT, 0, 0, 0); }

void task_kill(task_id_t task_id) { syscall(SYSCALL_TASK_KILL, task_id, 0, 0); }

void task_sleep(unsigned int ms) { syscall(SYSCALL_TASK_SLEEP, ms, 0, 0); }

task_id_t task_create(task_func_t entry, void *arg, size_t stack_size,
                      int priority) {
  volatile task_create_param_t param;
  param.entry = entry;
  param.arg = arg;
  param.stack_size = stack_size;
  param.priority = priority;
  return syscall(SYSCALL_TASK_CREATE, (int)&param, 0, 0);
}

task_id_t task_create_isr(task_func_t entry, void *arg, size_t stack_size,
                          int priority) {
  task_create_param_t param;
  param.entry = entry;
  param.arg = arg;
  param.stack_size = stack_size;
  param.priority = priority;
  return do_task_create(&param);
}

// os methods (public methods)

int os_init() {
  if ((heap = heap_init(heap_pool, sizeof(heap_pool))) == NULL) {
    return RET_OS_INIT_HEAP_FAILED;
  }
  return 0;
}

int os_start(void) { return syscall(SYSCALL_OS_START, 0, 0, 0); }

// handlers

void Post_SVC_Handler(interrupt_context_t *ctx) {
  uint8_t service_no = *(uint8_t *)(ctx->pc - 2);
  if (service_no != 0x80) {
    return;
  }

  switch (ctx->r0) {
  case SYSCALL_OS_START: {
    ctx->r0 = do_os_start();
    break;
  }
  case SYSCALL_TASK_CREATE: {
    task_create_param_t *param = (task_create_param_t *)ctx->r1;
    ctx->r0 = do_task_create(param);
    break;
  }
  case SYSCALL_TASK_SLEEP: {
    ctx->r0 = do_task_sleep(ctx->r1);
    break;
  }
  case SYSCALL_TASK_EXIT: {
    ctx->r0 = do_task_exit();
    break;
  }
  case SYSCALL_TASK_KILL: {
    ctx->r0 = do_task_kill(ctx->r1);
    break;
  }
  case SYSCALL_QUEUE_CREATE: {
    queue_create_param_t *param = (queue_create_param_t *)ctx->r1;
    ctx->r0 = do_queue_create(param);
    break;
  }
  case SYSCALL_QUEUE_PUSH: {
    queue_trans_param_t *param = (queue_trans_param_t *)ctx->r1;
    ctx->r0 = do_queue_push(param);
    break;
  }
  case SYSCALL_QUEUE_PULL: {
    queue_trans_param_t *param = (queue_trans_param_t *)ctx->r1;
    ctx->r0 = do_queue_pull(param);
    break;
  }
  case SYSCALL_QUEUE_CLOSE: {
    ctx->r0 = do_queue_close(ctx->r1);
    break;
  }
  }
}

// svc handler wrapping, written in asm to put context on argument
__attribute__((naked)) void SVC_Handler(void) {
  __asm__("tst lr, #4          \n\t"
          "ite eq              \n\t"
          "mrseq r0, msp       \n\t"
          "mrsne r0, psp       \n\t"

          "push {lr}           \n\t"
          "bl Post_SVC_Handler \n\t"
          "pop {pc}            \n\t");
}

void SysTick_Handler(void) { do_os_tick(); }

void Post_PendSV_Handler(void) { do_os_sche(); }

// pendsv handler wrapping, written in asm to save task context on entry and to
// restore it(may be a new one) on exit
__attribute__((naked)) void PendSV_Handler(void) {
  __asm__("push {lr}              \n\t"
          "mrs lr, psp            \n\t"
          "stmdb lr!, {r4-r11}    \n\t"
          "msr psp, lr            \n\t"

          "bl Post_PendSV_Handler \n\t"

          "mrs lr, psp            \n\t"
          "ldmia lr!, {r4-r11}    \n\t"
          "msr psp, lr            \n\t"

          "pop {pc}               \n\t");
}
