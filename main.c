#include <stdlib.h>
#include <stdio.h>

#include "stm32f10x.h"

#include "wos.h"

void init() {
  NVIC_SetPriorityGrouping(0);

  SysTick_Config(SystemCoreClock/1000);
  NVIC_SetPriority(SysTick_IRQn, NVIC_EncodePriority(0, 0, 0));

  NVIC_SetPriority(SVCall_IRQn, NVIC_EncodePriority(0, 1, 0));
  NVIC_SetPriority(PendSV_IRQn, NVIC_EncodePriority(0, 1, 0));
}

static uint16_t q1;
static uint16_t q2;

void task_ping(void *arg) {
  int id = (int)arg;
  printf("task_ping(%d) entry!\n", id);
  for (;;) {
    printf("%d: queue_push(q1) = %d\n", id, queue_push(q1, &id, NO_TIMEOUT));
    task_sleep(id);
  }
}

void task_echo(void *arg) {
  int i;
  for (;;) {
    printf("echo: queue_pull(q1) = %d\n", queue_pull(q1, &i, NO_TIMEOUT));  
    printf("echo: %d\n", i);
  }
}

int main() {
  init();

  printf("create task_ping(50): id = %d\n", task_create(task_ping, (void *)50, 0x400, 0));
  printf("create task_ping(100): id = %d\n", task_create(task_ping, (void *)100, 0x400, 0));
  printf("create task_echo(): id = %d\n", task_create(task_echo, (void *)0, 0x400, 0));

  printf("create queue: id = %d\n", q1 = queue_create(0x4, 1));
  printf("create queue: id = %d\n", q2 = queue_create(0x4, 2));

  os_start();

  for (;;) {
    printf("should not happen!\n");
  }
}
