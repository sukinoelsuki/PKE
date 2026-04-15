#ifndef __sync_utils__
#define __sync_utils__

#include "util/types.h"

// 原子加法 （64位）
static inline void atomic_add(void *addr, long delta);

// 原子加法并返回旧值 （64位）
static inline long atomic_fetch_add(void *addr, long delta);

// 原子交换.aq （32位）
static inline uint32 atomic_swap(void *addr, uint32 new_val);

// 原子释放.rl （32位）
static inline void atomic_set_release(void *addr, uint32 val);

// 原子比较后交换 （64位）
static inline uint32 atomic_cas(void *addr, uint64 expected, uint64 new_val);

// Spin_lock (32位)
static inline void lock_acquire(void *lock);

// Spin_unlock (32位)
static inline void lock_release(void *lock);

#endif