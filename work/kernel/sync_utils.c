#include "sync_utils.h"


// 1. 原子加法：用于引用计数、统计信息 （64位）
static inline void atomic_add(void *addr, long delta) {
    __asm__ __volatile__ (
        "amoadd.d x0, %1, (%0)" 
        : : "r" (addr), "r" (delta) : "memory"
    );
}

// 2. 原子加法并返回旧值：用于 "fetch-and-add" 逻辑 （64位）
// 该加法一定会改变值，所以不适合用于原有值不能修改的逻辑
static inline long atomic_fetch_add(void *addr, long delta) {
    long old;
    __asm__ __volatile__ (
        "amoadd.d %0, %2, (%1)"
        : "=r" (old) : "r" (addr), "r" (delta) : "memory"
    );
    return old;
}

// 3. 原子交换：实现 Spinlock 的核心 （32位锁）
static inline uint32 atomic_swap(void *addr, uint32 new_val) {
    uint32 old;
    __asm__ __volatile__ (
        "amoswap.w.aq %0, %2, (%1)" // .aq 确保后续操作不重排到此指令前
        : "=r" (old) : "r" (addr), "r" (new_val) : "memory"
    );
    return old;
}

// 4. 原子释放：实现 Spinlock 的核心（32位锁）
static inline void atomic_set_release(void *addr, uint32 val) {
    __asm__ __volatile__ (
        "amoswap.w.rl x0, %1, (%0)" // .rl 确保之前操作已全部完成
        : : "r" (addr), "r" (val): "memory"
    );
}

// 5. 原子比较并交换 (CAS)：无锁挂载页表、更新 PTE 的利器（64位）
static inline uint32 atomic_cas_d(void *addr, uint64 expected, uint64 new_val) {
    uint32 success;
    __asm__ __volatile__ (
        "1: lr.d %0, (%1)       \n" // 1. 加载并保留 (Load-Reserved)
        "   bne %0, %2, 2f      \n" // 2. 比较：如果不相等，跳转到标签 2 (失败)
        "   sc.d.rl %0, %3, (%1)\n" // 3. 条件存储 (Store-Conditional)
        "   bnez %0, 1b         \n" // 4. 如果存储失败（被干扰），跳回标签 1 重试
        "   li %0, 1            \n" // 5. 存储成功，设置 success = 1
        "   j 3f                \n" // 6. 跳转到结束
        "2: li %0, 0            \n" // 7. 比较失败，设置 success = 0
        "3:                     \n" 
        : "=&r" (success) : "r" (addr), "r" (expected), "r" (new_val) : "memory"
    );
    return success;
}

// 6. 原子比较并交换（CAS）（32位）
static inline void atomic_cas_w(void *addr, uint32 expected, uint32 new_val) {
    uint32 success;
    __asm__ __volatile__ (
        "1: lr.w %0, (%1)       \n"
        "   bne %0, %2, 2f      \n"
        "   sc.w.rl %0, %3, (%1)\n"
        "   bnez %0, 1b         \n"
        "   li %0, 1            \n"
        "   j 3f                \n"
        "2: li %0, 0            \n"
        "3:                     \n"
        : "=&r" (success) : "r" (addr), "r" (expected), "r" (new_val) : "memory"
    );
    return success;
}

// 7. Spinlock 实现
// Spinlock 上锁
static inline void lock_acquire(void* lock) {
    //持续请求锁
    while (1) {
        if (*((uint32 *)lock) == 0) {
            if(atomic_swap(lock, 1) == 0) {
                break;
            }
        }
        __asm__ __volatile__("pause");
    }
}

// Spinlock 解锁
static inline void lock_release(void* lock) {
    atomic_set_release(lock, 0);
}