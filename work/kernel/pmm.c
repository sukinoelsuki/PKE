#include "pmm.h"
#include "util/functions.h"
#include "riscv.h"
#include "config.h"
#include "util/string.h"
#include "memlayout.h"
#include "spike_interface/spike_utils.h"

// _end is defined in kernel/kernel.lds, it marks the ending (virtual) address of PKE kernel
extern char _end[];
// g_mem_size is defined in spike_interface/spike_memory.c, it indicates the size of our
// (emulated) spike machine. g_mem_size's value is obtained when initializing HTIF. 
extern uint64 g_mem_size;

static uint64 free_mem_start_addr;  //beginning address of free memory
static uint64 free_mem_end_addr;    //end address of free memory (not included)

static short page_ref[MAX_PAGE] = {0};

static spinlock_t page_ref_lock = {0};
static spinlock_t pmm_lock = {0};

typedef struct node {
  struct node *next;
} list_node;

// g_free_mem_list is the head of the list of free physical memory pages
static list_node g_free_mem_list;

// 如果 *addr == expected, 则将 *addr 设为 new，并返回 1 (成功)
// 否则返回 0 (失败，说明被别人抢先了)
static inline int atomic_cas_pte(pte_t *addr, pte_t expected, pte_t new_val) {
    int success;
    __asm__ __volatile__ (
        "1: lr.d %0, (%1)       \n" // 读取 addr 指向的值到 %0，并挂上“保留标记”
        "   bne %0, %2, 2f      \n" // 如果读取的值 != expected，跳转到 2（失败）
        "   sc.d %0, %3, (%1)   \n" // 尝试将 new_val 写入 addr，结果存入 %0
        "   bnez %0, 1b         \n" // 如果 %0 不为 0，说明写入期间有干扰，回到 1 重试
        "   li %0, 1            \n" // 走到这说明写入成功，设置返回值为 1
        "   j 3f                \n" // 跳到结束
        "2: li %0, 0            \n" // 失败分支：设置返回值为 0
        "3:                     \n"
        : "=&r" (success)           // %0: 输出变量
        : "r" (addr), "r" (expected), "r" (new_val) // %1, %2, %3: 输入变量
        : "memory"                  // 告诉编译器内存发生了变化
    );
    return success;
}

//
// actually creates the freepage list. each page occupies 4KB (PGSIZE), i.e., small page.
// PGSIZE is defined in kernel/riscv.h, ROUNDUP is defined in util/functions.h.
//
static void create_freepage_list(uint64 start, uint64 end) {
  g_free_mem_list.next = 0;
  for (uint64 p = ROUNDUP(start, PGSIZE); p + PGSIZE < end; p += PGSIZE) {
    //提前置1，便于在free_page时将节点仍进链表，完成逻辑闭环。
    spin_lock(&page_ref_lock.lock);
    page_ref[PA_TO_IDX(p)] = 1;
    spin_unlock(&page_ref_lock.lock);

    free_page( (void *)p );
  }
}

//
// place a physical page at *pa to the free list of g_free_mem_list (to reclaim the page)
//
void free_page(void *pa) {
  if (((uint64)pa % PGSIZE) != 0 || (uint64)pa < free_mem_start_addr || (uint64)pa >= free_mem_end_addr)
    panic("free_page 0x%lx \n", pa);

  //维护index数组，实时管理page生命周期。
  
  int page_index = PA_TO_IDX(pa);

  spin_lock(&page_ref_lock.lock);
  if (page_ref[page_index] <= 0) {
    //unlock before exit.
    spin_unlock(&page_ref_lock.lock);
    panic("free_page: double free or ref count error!");
  }
  
  page_ref[page_index]--;

  spin_lock(&pmm_lock.lock);
  if (page_ref[page_index] == 0) {
    list_node *n = (list_node *)pa;
    n->next = g_free_mem_list.next;
    g_free_mem_list.next = n;
  }
  spin_unlock(&pmm_lock.lock);
  spin_unlock(&page_ref_lock.lock);

}

//
// takes the first free page from g_free_mem_list, and returns (allocates) it.
// Allocates only ONE page!
//
void *alloc_page(void) {

  // lock
  spin_lock(&pmm_lock.lock);

  list_node *n = g_free_mem_list.next;
  if (n) {
    g_free_mem_list.next = n->next;
    page_ref[PA_TO_IDX(n)] = 1;
  }

  // unlock
  spin_unlock(&pmm_lock.lock);

  return (void *)n;
}

// another process share the page, increase the index
void page_ref_share(void *pa) {
  spin_lock(&page_ref_lock.lock);
  page_ref[PA_TO_IDX(pa)]++;
  spin_unlock(&page_ref_lock.lock);
}

//不带锁，简单事务接口。
//return value in page_ref
short __get_page_ref(void* pa) {
  return page_ref[PA_TO_IDX(pa)];
}

//COW fork add
void __page_ref_inc(void* pa) {
  page_ref[PA_TO_IDX(pa)]++;
}

void __page_ref_dec(void *pa) {
  page_ref[PA_TO_IDX(pa)]--;
  return (page_ref[PA_TO_IDX(pa)] == 0);
}

//check shared or not
int __is_shared_page(void* pa) {
  return page_ref[PA_TO_IDX(pa)] > 1;
}

//
// pmm_init() establishes the list of free physical pages according to available
// physical memory space.
//
void pmm_init() {
  
  // start of kernel program segment
  uint64 g_kernel_start = KERN_BASE;
  uint64 g_kernel_end = (uint64)&_end;

  pmm_lock.lock = 0;
  page_ref_lock.lock = 0;

  uint64 pke_kernel_size = g_kernel_end - g_kernel_start;
  sprint("PKE kernel start 0x%lx, PKE kernel end: 0x%lx, PKE kernel size: 0x%lx .\n",
    g_kernel_start, g_kernel_end, pke_kernel_size);

  // free memory starts from the end of PKE kernel and must be page-aligined
  free_mem_start_addr = ROUNDUP(g_kernel_end , PGSIZE);

  // recompute g_mem_size to limit the physical memory space that our riscv-pke kernel
  // needs to manage
  g_mem_size = MIN(PKE_MAX_ALLOWABLE_RAM, g_mem_size);
  if( g_mem_size < pke_kernel_size )
    panic( "Error when recomputing physical memory size (g_mem_size).\n" );

  free_mem_end_addr = g_mem_size + DRAM_BASE;
  sprint("free physical memory address: [0x%lx, 0x%lx] \n", free_mem_start_addr,
    free_mem_end_addr - 1);

  sprint("kernel memory manager is initializing ...\n");

  // create the list of free pages
  create_freepage_list(free_mem_start_addr, free_mem_end_addr);
}
