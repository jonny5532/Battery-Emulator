#ifndef FREERTOS_TASK_H
#define FREERTOS_TASK_H

typedef void (*TaskFunction_t)(void*);

struct tskTaskControlBlock; /* The old naming convention is used to prevent breaking kernel aware debuggers. */
typedef struct tskTaskControlBlock* TaskHandle_t;

#endif
