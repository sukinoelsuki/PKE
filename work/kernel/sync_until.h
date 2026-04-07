#include "util/types.h"


// 1. 原子加法：用于引用计数、统计信息
static inline void atomic_add(void *addr, long delta) {
    __asm__ __volatile__ (
        "amoadd.d x0, %1, (%0)" 
        : : "r" (addr), "r" (delta) : "memory"
    );
}

// 2. 原子加法并返回旧值：用于 "fetch-and-add" 逻辑
static inline long atomic_fetch_add(void *addr, long delta) {
    long old;
    __asm__ __volatile__ (
        "amoadd.d %0, %2, (%1)"
        : "=r" (old) : "r" (addr), "r" (delta) : "memory"
    );
    return old;
}

// 3. 原子交换：实现 Spinlock 的核心
static inline uint32 atomic_swap(void *addr, uint32 new_val) {
    uint32 old;
    __asm__ __volatile__ (
        "amoswap.w.aq %0, %2, (%1)" // .aq 确保后续操作不重排到此指令前
        : "=r" (old) : "r" (addr), "r" (new_val) : "memory"
    );
    return old;
}

// 4. 原子比较并交换 (CAS)：无锁挂载页表、更新 PTE 的利器
static inline int atomic_cas(void *addr, uint64 expected, uint64 new_val) {
    int success;
    __asm__ __volatile__ (
        "1: lr.d %0, (%1)       \n"
        "   bne %0, %2, 2f      \n"
        "   sc.d.rl %0, %3, (%1)\n" // .rl 确保之前的写入已对全局可见
        "   bnez %0, 1b         \n"
        "   li %0, 1            \n"
        "   j 3f                \n"
        "2: li %0, 0            \n"
        "3:                     \n"
        : "=&r" (success) : "r" (addr), "r" (expected), "r" (new_val) : "memory"
    );
    return success;
}