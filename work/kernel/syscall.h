/*
 * define the syscall numbers of PKE OS kernel.
 */
#ifndef _SYSCALL_H_
#define _SYSCALL_H_

// syscalls of PKE OS kernel. append below if adding new syscalls.
#define SYS_user_base 64
#define SYS_user_print (SYS_user_base + 0)
#define SYS_user_exit (SYS_user_base + 1)
// added @lab2_2
#define SYS_user_allocate_page (SYS_user_base + 2)
#define SYS_user_free_page (SYS_user_base + 3)
// added @lab3_1
#define SYS_user_fork (SYS_user_base + 4)
#define SYS_user_yield (SYS_user_base + 5)

long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7);

// 在已有的系统调用定义（如 SYS_user_exit）下面添加 
#define SYS_user_sem_new    (SYS_user_base + 10) // 数字只要不冲突就行
#define SYS_user_sem_P      (SYS_user_base + 11)
#define SYS_user_sem_V      (SYS_user_base + 12)

#define SYS_user_printpa    (SYS_user_base + 13) // 打印物理地址

#endif
