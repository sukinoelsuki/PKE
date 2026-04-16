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

//
// actually creates the freepage list. each page occupies 4KB (PGSIZE), i.e., small page.
// PGSIZE is defined in kernel/riscv.h, ROUNDUP is defined in util/functions.h.
//
static void create_freepage_list(uint64 start, uint64 end) {
  g_free_mem_list.next = 0;
  for (uint64 p = ROUNDUP(start, PGSIZE); p + PGSIZE < end; p += PGSIZE) {
    // 提前置0，与 free_page 逻辑分开，函数仅在主核初始化时调用，无需另外加锁
    page_ref[PA_TO_IDX(p)] = 0;
    list_node *n = (list_node *)p;
    n->next = g_free_mem_list.next;
    g_free_mem_list.next = n;
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
/*
  lock_acquire(&page_ref_lock.lock);
  if (page_ref[page_index] <= 0) {
    //unlock before exit.
    lock_release(&page_ref_lock.lock);
    panic("free_page: double free or ref count error!");
  }

  page_ref[page_index]--;
*/
  if (page_ref_dec_and_test(&page_ref[page_index])) {
    lock_acquire(&pmm_lock.lock);
    list_node *n = (list_node *)pa;
    n->next = g_free_mem_list.next;
    g_free_mem_list.next = n;
    lock_release(&pmm_lock.lock);
  }
}

//
// takes the first free page from g_free_mem_list, and returns (allocates) it.
// Allocates only ONE page!
//
void *alloc_page(void) {

  // lock 锁住内存块链表，只能Spinlock，其它原子操作难以
  // 操作链表结构，但是存在无锁链表（容错？复杂度？）
  lock_acquire(&pmm_lock.lock);

  list_node *n = g_free_mem_list.next;
  if (likely(n != NULL)) {
    g_free_mem_list.next = n->next;

    // 未从链表中取出来，n 所代表的地址不会被访问到？
    // 所以直接进行 page_ref[] 的操作，不会影响到其
    // 它核。其他核被 pmm_lock 锁住，不会访问到同一
    // 地址
    page_ref[PA_TO_IDX(n)] = 1;

  } else {
    panic("alloc_page : Out of memory!\n");
  }

  // unlock
  lock_release(&pmm_lock.lock);

  return (void *)n;
}

// another process share the page, increase the index
void page_ref_share(void *pa) {
  spin_lock(&page_ref_lock.lock);
  page_ref[PA_TO_IDX(pa)]++;
  spin_unlock(&page_ref_lock.lock);
}

// 原子操作事务接口。
//return value in page_ref
short get_page_ref(void* pa) {
  return page_ref[PA_TO_IDX(pa)];
}

//COW fork add
void page_ref_inc(void* pa) {
  page_ref[PA_TO_IDX(pa)]++;
}

int page_ref_dec_and_test(void *pa) {
  return (atomic_fetch_add(&page_ref[PA_TO_IDX(pa)], -1) == 1);
}

//check shared or not
int is_shared_page(void* pa) {
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
  
  // 采用 Buddy_System 来管理物理内存块
  /*
  init_Buddy_System(free_mem_start_addr, free_mem_end_addr);
  */
}


// 下面篇幅用于实现 Buddy System，在实现前，需要保留原有的链表结构逻辑
// Buddy System 的各项构建必须封装在 pmm.c 中，仅向外界提供调用接口，
// 防止泄露。

#ifndef __Buddy_System__
#define __Buddy_System__

//#define __DEBUG_BUDDY__
#ifdef __DEBUG_BUDDY__
  #define MARK_COLOR(p, val) ((p)->debug_tag = val)
#else 
  #define MARK_COLOR(p, val)
#endif

// 链表节点，双向列表实现 O（1） 增删
// 和原有链表重名了，但是仅是增加了结构，并不会报错
typedef struct List_Node {
  list_node* pre;
  list_node* next;
} list_node;

// 整体系统
typedef struct Buddy_System{
  list_node free_area[MAX_ORDER];
  spinlock_t buddy_lock;
} buddy_system;

typedef struct page {
  uint32 is_head;
  uint32 order;
  list_node page_node;
}page_t;


// pages 数组，由内核启动时直接分配
page_t *pages;

// 当前内存的主 Buddy System
buddy_system main_buddy;

// 初始化物理空间，在 pmm_init() 中被调用
// 是否加上 inline？static 保证仅在该文件中被使用
static inline uint64 init_Buddy_System(uint64 start_addr, uint64 end_addr) {

  // 首先初始化 pages，需要保证 is_head 初值为 0，对于初始的每个页 Order 应该也是 0，
  // 节点的指向也应该是空，这些节点还不需要插入任何一个 Order 的队列，目前是一个整体，
  // 这个整体现在应该由一个或多个代表来表示，这两个代表是对阶后产生的。
  // 要根据大小确定代表的 PFN 编号，将页信息更改，采用表头插入插入链表。
  // 可见 Buddy System 有个不可忽视的问题，申请低阶页时向高阶页申请划分空间，这个操作
  // 必须是常数级的，而且尽可能要快。尤其是初始化三级页表的过程，如果上来就全部分配，必
  // 然造成巨大的开销，开销在划分最小页表上，所以虚拟地址可能要按需使用，而不是上来就
  // map 整个虚拟空间，这样也会造成太多碎块，导致出现普通 Buddy System 无法提供连续空
  // 间的情况。
  memset(pages, 0, MAX_PAGE * sizeof(page_t));

  // 初始化每阶的链表头结点，将其置为节点自己
  for (int i = 0; i < MAX_ORDER; ++i) {
    main_buddy.free_area[i].next = &main_buddy.free_area[i];
    main_buddy.free_area[i].pre = &main_buddy.free_area[i];
  }

  // 极其简单的初始化锁结构，因为现在实现的锁结构很简单，待扩充
  main_buddy.buddy_lock.lock = 0;

  // 初始化给定的物理空间，进行分块
  // 分块的前提是地址对齐
  uint64 curr_addr = ROUNDUP(start_addr, PGSIZE);
  uint64 stop_addr = ROUNDDOWN(end_addr, PGSIZE);

  while (curr_addr < stop_addr) {
    // 当前的起始物理地址需要对齐，取最低有效位作为 "limit"
    
    // 计算剩余空间大小，取最高有效位作为 "limit"

  }
  int PFN_index = ROUNDDOWN((end_addr - start_addr), 4096);
  // 在人的角度能看到当前的内存是固定的，所以其实能直接填入信息，但是为了严谨，普适的系
  // 统应该有计算最低有效位，然后将填入 pages 信息以及插入对应阶数的链表。
  // 如何快速确定最低有效位？内存大小的逻辑是不是支持我们仅去寻找一位？因为按大小来说更
  // 可能是 2 的 n 次方，而不是其他的大小。

}

#endif