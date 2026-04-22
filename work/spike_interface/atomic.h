// See LICENSE for license details.
// borrowed from https://github.com/riscv/riscv-pk:
// machine/atomic.h

#ifndef _RISCV_ATOMIC_H_
#define _RISCV_ATOMIC_H_


typedef struct {
  int lock;
  // For debugging:
  char* name;       // Name of lock.
  struct cpu* cpu;  // The cpu holding the lock.
} spinlock_t;

#define SPINLOCK_INIT \
  { 0 }

#define mb() asm volatile("fence" ::: "memory")

// 64 bits atomic fetch and add
static inline long atomic_binop_d(long *ptr, long inc, long op) {
  long old;
  __asm__ __volatile__ (
    "amoadd.d %0, %2, (%1)"
    : "=r"(old) : "r" (ptr), "r"(op) : "memory"
  );
  return old;
}

// 32 bits atomic fetch and add
static inline int atomic_binop_w(int *ptr, int inc, int op) {
  int old;
  __asm__ __volatile__ (
    "amoadd.w %0, %2, (%1)"
    : "=r"(old) : "r" (ptr), "r"(op) : "memory"
  );
  return old;
}

#define atomic_add(ptr, inc) atomic_binop(ptr, inc, res + (inc))
#define atomic_or(ptr, inc) atomic_binop(ptr, inc, res | (inc))
#define atomic_swap(ptr, inc) atomic_binop(ptr, inc, (inc))
#define atomic_cas(ptr, cmp, swp)                           \
  ({                                                        \
    long flags = disable_irqsave();                         \
    typeof(*(ptr)) res = *(volatile typeof(*(ptr))*)(ptr);  \
    if (res == (cmp)) *(volatile typeof(ptr))(ptr) = (swp); \
    enable_irqrestore(flags);                               \
    res;                                                    \
  })

static inline int spinlock_trylock(spinlock_t* lock) {
  int res = atomic_swap(&lock->lock, -1);
  mb();
  return res;
}

static inline void spinlock_lock(spinlock_t* lock) {
  do {
    while (atomic_read(&lock->lock))
      ;
  } while (spinlock_trylock(lock));
}

static inline void spinlock_unlock(spinlock_t* lock) {
  mb();
  atomic_set(&lock->lock, 0);
}

static inline long spinlock_lock_irqsave(spinlock_t* lock) {
  long flags = disable_irqsave();
  spinlock_lock(lock);
  return flags;
}

static inline void spinlock_unlock_irqrestore(spinlock_t* lock, long flags) {
  spinlock_unlock(lock);
  enable_irqrestore(flags);
}

#endif
