/*
 * header file to be used by applications.
 */

#include "util/types.h"
#include "util/snprintf.h"
#include "kernel/syscall.h"

int printu(const char *s, ...);
int exit(int code);
void* naive_malloc();
void naive_free(void* va);
int fork();
void yield();

int sem_new(int value);  //to do
void sem_P(int sem_id);  //
void sem_V(int sem_id);  //

// 打印虚拟地址对应的物理地址
uint64 printpa(void* va);