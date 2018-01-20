#include "wos.h"

#include "stm32f10x.h"

#define SYSCALL_OS_START     0
#define SYSCALL_TASK_CREATE  1
#define SYSCALL_TASK_SLEEP   2
#define SYSCALL_TASK_EXIT    3
#define SYSCALL_TASK_KILL    4
#define SYSCALL_QUEUE_CREATE 5
#define SYSCALL_QUEUE_PUSH   6
#define SYSCALL_QUEUE_PULL   7
#define SYSCALL_QUEUE_CLOSE  8

#define TASK_STATE_FREE      0
#define TASK_STATE_RUNNING   1
#define TASK_STATE_READY     2
#define TASK_STATE_SLEEPING  3
#define TASK_WAITING_TO_PUSH 5
#define TASK_WAITING_TO_PULL 6

#define QUEUE_STATE_FREE     0
#define QUEUE_STATE_NORMAL   1

// contexts defs

typedef struct {
  uint32_t r0;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;
  uint32_t r12;
  uint32_t lr;
  uint32_t pc;
  uint32_t xpsr;
} interrupt_context_t;

typedef struct {
  uint32_t r4;
  uint32_t r5;
  uint32_t r6;
  uint32_t r7;
  uint32_t r8;
  uint32_t r9;
  uint32_t r10;
  uint32_t r11;
  interrupt_context_t int_ctx;
} task_context_t;

// control blocks defs

typedef struct {
  uint16_t id;
  uint8_t state;
  uint8_t priority;
  uint16_t trans_queue_id;
  void *trans_item;
  uint32_t wait_timeout;
  uint32_t stack_base;
  uint32_t stack_size;
  task_context_t *ctx;
} task_control_block_t;

typedef struct {
  uint16_t id;
  uint8_t state;
  uint8_t ctl;
  uint32_t count;
  uint32_t data_base;
  uint32_t item_size;
  uint32_t length;
} queue_control_block_t;

// params defs

typedef struct {
  task_func_t entry;
  void *arg;
  uint32_t stack_size;
  uint8_t priority;
} task_create_param_t;

typedef struct {
  uint32_t item_size;
  uint32_t length;
} queue_create_param_t;

typedef struct {
  uint16_t queue_id;
  void *item;
  uint32_t timeout;
} queue_trans_param_t;

// os vars

static int os_started = 0;

static uint32_t os_ticks = 0;

// task vars

static uint64_t tasks_stack_pool[TASKS_STACK_POOL_SIZE / 8] = {0};

static task_control_block_t tcbs[MAX_TASKS] = {0};

static task_control_block_t *current_tcb = 0;

// queue vars

static uint8_t queues_data_pool[QUEUES_DATA_POOL_SIZE] = {0};

static queue_control_block_t qcbs[MAX_QUEUES] = {0};

// static methods

// implement these here so it doesn't rely on C library
static void *memset(void *s, int c, uint32_t n) {
  const unsigned char uc = c;
  unsigned char *su;
  for(su = s; 0 < n; ++su, --n)
    *su = uc;
  return s;
}

static void *memmove(void *dst, const void *src, uint32_t count) {
    char *tmpdst = (char *)dst;
    char *tmpsrc = (char *)src;
    if (tmpdst <= tmpsrc || tmpdst >= tmpsrc + count){
      while(count--) {
          *tmpdst++ = *tmpsrc++; 
      }
    }
    else {
      tmpdst = tmpdst + count - 1;
      tmpsrc = tmpsrc + count - 1;
      while(count--) {
        *tmpdst-- = *tmpsrc--;
      }
    }
    return dst; 
}

// helper inline func to set PendSV exception pending
static inline void set_pendsv(void) {
  SCB->ICSR = SCB_ICSR_PENDSVSET;
}

// yield, which simply set PendSV pending so PendSV_Handler will be invoked upon return
static void os_yield(void) {
  set_pendsv();
}

// 'syscall' wrapper
static inline uint32_t syscall(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
  uint32_t ret;
  __asm__ volatile(
    R"(
    mov r0, %1
    mov r1, %2
    mov r2, %3
    mov r3, %4
    svc #0x80
    mov %0, r0
    )"
    :"=r"(ret)
    :"r"(r0), "r"(r1), "r"(r2), "r"(r3)
    :"r0", "r1", "r2", "r3"
  );
  return ret;
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
    __asm__ ("wfe");
  }
}

// do methods, which are invoked by handlers only

static int32_t do_task_create(task_create_param_t *param) {
  if (param->stack_size % 8 != 0) {
    return RET_TASK_CREATE_NOT_ALIGNED;
  }
  enter_critical();
  // find memory for task stack
  // this algorithm is ugly...
  uint32_t next_task_stack_base = (uint32_t)(tasks_stack_pool + sizeof(tasks_stack_pool)/8) - 1;
  int found = 1;
  for (int i = -1; i < MAX_TASKS; i++) {
    if (i != -1 && tcbs[i].state == TASK_STATE_FREE) {
      continue;
    }
    uint32_t i_stack_base, i_stack_size;
    if (i == -1) {
      i_stack_base = (uint32_t)(tasks_stack_pool + sizeof(tasks_stack_pool)/8) - 1;
      i_stack_size = 0;
    } else {
      i_stack_base = tcbs[i].stack_base;
      i_stack_size = tcbs[i].stack_size;
    }
    uint32_t highest_stack_base = 0;
    for (int j = 0; j <= MAX_TASKS; j++) {
      if (j != MAX_TASKS && tcbs[j].state == TASK_STATE_FREE) {
        continue;
      }
      if (j == i) {
        continue;
      }
      uint32_t j_stack_base;
      if (j == MAX_TASKS) {
        j_stack_base = (uint32_t)tasks_stack_pool;
      } else {
        j_stack_base = tcbs[j].stack_base;
      }
      if (j_stack_base > i_stack_base) {
        continue;
      }
      if (j_stack_base > highest_stack_base) {
        highest_stack_base = j_stack_base;
      }
    }
    if (highest_stack_base != 0 && highest_stack_base <= i_stack_base - i_stack_size - param->stack_size) {
      next_task_stack_base = i_stack_base - i_stack_size;
      found = 1;
      break;
    }
    found = 0;
  }
  if (!found) {
    leave_critical();
    return RET_TASK_CREATE_INSUFFICIENT_STACK_SPACE;
  }

  // build context
  task_context_t *task_ctx = (task_context_t *)(next_task_stack_base - sizeof(task_context_t) + 1);
  memset(task_ctx, 0, sizeof(task_context_t));
  task_ctx->int_ctx.xpsr = 0x01000000;
  task_ctx->int_ctx.pc = (uint32_t)(uint32_t*)param->entry | 1;
  task_ctx->int_ctx.lr = (uint32_t)(uint32_t*)task_exit | 1;
  task_ctx->int_ctx.r0 = (uint32_t)(uint32_t*)param->arg;

  // build tcb
  task_control_block_t *tcb = 0;
  uint16_t task_id = 0;
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
  tcb->stack_base = next_task_stack_base;
  tcb->stack_size = param->stack_size;

  leave_critical();
  return tcb->id;
}

// these are simply setting current task's state
static int32_t do_task_sleep(uint32_t timeout) {
  enter_critical();
  current_tcb->wait_timeout = timeout;
  current_tcb->state = TASK_STATE_SLEEPING;
  leave_critical();
  os_yield();
  return 0;
}

static int32_t do_task_exit(void) {
  enter_critical();
  current_tcb->state = TASK_STATE_FREE;
  leave_critical();
  os_yield();
  return 0;
}

static int32_t do_task_kill(uint16_t task_id) {
  enter_critical();
  tcbs[task_id].state = TASK_STATE_FREE;
  leave_critical();
  os_yield();
  return 0;
}

static int32_t do_queue_create(queue_create_param_t *param) {
  enter_critical();
  // find memory for queue data
  // ugly algorithm. could have been better
  uint32_t next_queue_data_base = (uint32_t)(queues_data_pool + sizeof(queues_data_pool)) - 1;
  int found = 1;
  for (int i = -1; i < MAX_QUEUES; i++) {
    if (i != -1 && qcbs[i].state == QUEUE_STATE_FREE) {
      continue;
    }
    uint32_t i_data_base, i_data_size;
    if (i == -1) {
      i_data_base = (uint32_t)(queues_data_pool + sizeof(queues_data_pool)) - 1;
      i_data_size = 0;
    } else {
      i_data_base = qcbs[i].data_base;
      i_data_size = qcbs[i].length * qcbs[i].item_size;
    }
    uint32_t highest_data_base = 0;
    for (int j = 0; j <= MAX_QUEUES; j++) {
      if (j != MAX_QUEUES && qcbs[j].state == QUEUE_STATE_FREE) {
        continue;
      }
      if (j == i) {
        continue;
      }
      uint32_t j_data_base;
      if (j == MAX_QUEUES) {
        j_data_base = (uint32_t)queues_data_pool;
      } else {
        j_data_base = qcbs[j].data_base;
      }
      if (j_data_base > i_data_base) {
        continue;
      }
      if (j_data_base > highest_data_base) {
        highest_data_base = j_data_base;
      }
    }
    if (highest_data_base != 0 && highest_data_base <= i_data_base - i_data_size - param->item_size*param->length) {
      next_queue_data_base = i_data_base - i_data_size;
      found = 1;
      break;
    }
    found = 0;
  }
  if (!found) {
    leave_critical();
    return RET_QUEUE_CREATE_INSUFFICIENT_DATA_SPACE;
  }

  // build qcb
  queue_control_block_t *qcb = 0;
  uint16_t queue_id = 0;
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
  qcb->ctl = 0;
  qcb->state = QUEUE_STATE_NORMAL;
  qcb->data_base = next_queue_data_base;
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
      memmove(pull_tcbs[highest_priority_task_i]->trans_item, (void *)(qcbs[i].data_base - qcbs[i].item_size + 1), qcbs[i].item_size);
      // adjust
      memmove((void *)(qcbs[i].data_base - qcbs[i].item_size*(qcbs[i].count - 1) + 1), (void *)(qcbs[i].data_base - qcbs[i].item_size*qcbs[i].count + 1), qcbs[i].item_size*(qcbs[i].count - 1));
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
    memmove((void *)(qcb->data_base - qcb->item_size*(qcb->count+1) + 1), tcbs[i].trans_item, qcb->item_size);
    qcb->count++;
    tcbs[i].ctx->int_ctx.r0 = 0;
    tcbs[i].state = TASK_STATE_READY;
  }
  leave_critical();
}

// queue push and pull are similar, and the process to really push/pull is in do_os_sche
static int32_t do_queue_push(queue_trans_param_t *param) {
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

static int32_t do_queue_pull(queue_trans_param_t *param) {
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

static int32_t do_queue_close(uint16_t queue_id) {
  enter_critical();
  qcbs[queue_id].state = QUEUE_STATE_FREE;
  for (int i = 0; i < MAX_TASKS; i++) {
    if (tcbs[i].state != TASK_WAITING_TO_PUSH && tcbs[i].state != TASK_WAITING_TO_PULL) {
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

static int32_t do_os_start(void) {
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
      __asm__ volatile(
        R"(
        ldmia %0!, {r4-r11}
        msr psp, %0

        str %2, [%1]
        ldr %0, =0xFFFFFFFD
        bx %0
        )"
        :
        :"r"(r0), "r"(r1), "r"(r2)
      );
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
  __asm__ volatile(
    R"(
    mrs %0, psp
    )"
    :"=r"(tcb->ctx)
  );

  int task_i = ((uint32_t)current_tcb - (uint32_t)tcbs)/sizeof(task_control_block_t);

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
      if (tcbs[i].state == TASK_WAITING_TO_PUSH || tcbs[i].state == TASK_WAITING_TO_PULL) {
        // timeout
        tcbs[i].ctx->int_ctx.r0 = RET_QUEUE_TRANS_TIMEOUT;
        tcbs[i].state = TASK_STATE_READY;
      }
    }
    if (tcbs[i].state != TASK_STATE_READY && tcbs[i].state != TASK_STATE_RUNNING) {
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
  __asm__ volatile(
    R"(
    msr psp, %0
    )"
    :
    :"r"(next_tcb->ctx)
  );
  next_tcb->state = TASK_STATE_RUNNING;
  current_tcb = next_tcb;
  leave_critical();
}

// queue methods (public methods)

int32_t queue_create(uint32_t item_size, uint32_t length) {
  volatile queue_create_param_t param;
  param.item_size = item_size;
  param.length = length;
  return syscall(SYSCALL_QUEUE_CREATE, (int)&param, 0, 0);
}

int32_t queue_push(uint16_t queue_id, void *item, uint32_t timeout) {
  volatile queue_trans_param_t param;
  param.queue_id = queue_id;
  param.item = item;
  param.timeout = timeout;
  return syscall(SYSCALL_QUEUE_PUSH, (int)&param, 0, 0);
}

int32_t queue_push_isr(uint16_t queue_id, void *item) {
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
  memmove((void *)(qcb->data_base - qcb->item_size*(qcb->count+1) + 1), item, qcb->item_size);
  qcb->count++;
  do_queue_tasks_pull();
  leave_critical();
  os_yield();
  return 0;
}

int32_t queue_pull(uint16_t queue_id, void *item, uint32_t timeout) {
  volatile queue_trans_param_t param;
  param.queue_id = queue_id;
  param.item = item;
  param.timeout = timeout;
  return syscall(SYSCALL_QUEUE_PULL, (int)&param, 0, 0);
}

int32_t queue_pull_isr(uint16_t queue_id, void *item) {
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
  memmove(item, (void *)(qcb->data_base - qcb->item_size + 1), qcb->item_size);
  // adjust
  memmove((void *)(qcb->data_base - qcb->item_size*(qcb->count - 1) + 1), (void *)(qcb->data_base - qcb->item_size*qcb->count + 1), qcb->item_size*(qcb->count - 1));
  qcb->count--;
  do_queue_tasks_push();
  leave_critical();
  os_yield();
  return 0;
}

void queue_close(uint16_t queue_id) {
  syscall(SYSCALL_QUEUE_CLOSE, queue_id, 0, 0);
}

void queue_close_isr(uint16_t queue_id) {
  do_queue_close(queue_id);
}

// task methods (public methods)

void task_exit(void) {
  syscall(SYSCALL_TASK_EXIT, 0, 0, 0);
}

void task_kill(uint16_t task_id) {
  syscall(SYSCALL_TASK_KILL, task_id, 0, 0);
}

void task_sleep(uint32_t ms) {
  syscall(SYSCALL_TASK_SLEEP, ms, 0, 0);
}

int32_t task_create(task_func_t entry, void *arg, uint32_t stack_size, uint8_t priority) {
  volatile task_create_param_t param;
  param.entry = entry;
  param.arg = arg;
  param.stack_size = stack_size;
  param.priority = priority;
  return syscall(SYSCALL_TASK_CREATE, (int)&param, 0, 0);
}

int32_t task_create_isr(task_func_t entry, void *arg, uint32_t stack_size, uint8_t priority) {
  task_create_param_t param;
  param.entry = entry;
  param.arg = arg;
  param.stack_size = stack_size;
  param.priority = priority;
  return do_task_create(&param);
}

// os methods (public methods)

int32_t os_start(void) {
  return syscall(SYSCALL_OS_START, 0, 0, 0);
}

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
  __asm__(
    R"(
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp

    push {lr}
    bl Post_SVC_Handler
    pop {pc}
    )"
  );
}

void SysTick_Handler(void) {
  do_os_tick();
}

void Post_PendSV_Handler(void) {
  do_os_sche();
}

// pendsv handler wrapping, written in asm to save task context on entry and to restore it(may be a new one) on exit
__attribute__((naked)) void PendSV_Handler(void) {
  __asm__(
    R"(
    push {lr}

    mrs lr, psp
    stmdb lr!, {r4-r11}
    msr psp, lr

    bl Post_PendSV_Handler

    mrs lr, psp
    ldmia lr!, {r4-r11}
    msr psp, lr

    pop {pc}
    )"
  );
}
