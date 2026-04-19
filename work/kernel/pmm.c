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

// 链表节点，双向列表实现 O（1） 增删
// 和原有链表重名了，但是仅是增加了结构，并不会报错
typedef struct List_Node {
  list_node* pre;
  list_node* next;
} list_node;

// 整体 Buddy 系统
typedef struct Buddy_System{
  spinlock_t buddy_lock;
  list_node free_area[MAX_ORDER];
} __attribute__((aligned(64))) buddy_system;

// 记录每个页的元数据，同时提供染色信息，便于实现 O(1) 查询修改操作
typedef struct page {
    union {
        uint64 raw_status;
        struct {
            uint32 is_free : 1;  // 空闲标记，1 表示空闲、能够使用。
            uint32 is_head : 1;  // 领头羊标记，仅有块首有标记，用于判断自己是否是“随从”
            uint32 order   : 5;  // 阶数，表明当前 page 代表的连续页块大小
            uint32 alloc   : 3;  // 归属标记，标识属于的内存系统，Buddy System/ Slab、Slub/Per-CPU Cache/内核静态预留（不可释放）
            uint32 cpu_id  : 6;  // 亲和性标记，属于哪个核（最多支持 64），便于实现跨核释放
            uint32 ref     : 16; // 引用计数，实现 COW
            uint32 color   : 8;  // 审计染色，存储父块标记，便于追踪内存来源，排查错误
        };
    } status;
    list_node node;
} __attribute__((aligned(8))) page_t; // 注意！保证了 status 的原子性，但 node 仍大概率截断。因修改 node 概率小（存疑）

// 物理页描述符数组指针
page_t *pages;
// total num of pa pages
uint64 nr_pages;

static uint64 free_mem_start_addr;  //beginning address of free memory
static uint64 free_mem_end_addr;    //end address of free memory (not included)

static short page_ref[MAX_PAGE] = {0};

static spinlock_t page_ref_lock = {0};
static spinlock_t pmm_lock = {0};

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
  // 在链接文件中写锚定了，内核从 0x80000000 开始，
  // 原始的固件信息可能被迁移到了后面
  uint64 g_kernel_start = KERN_BASE;
  uint64 g_kernel_end = (uint64)&_end;

  uint64 pke_kernel_size = g_kernel_end - g_kernel_start;
  sprint("PKE kernel start 0x%lx, PKE kernel end: 0x%lx, PKE kernel size: 0x%lx .\n",
    g_kernel_start, g_kernel_end, pke_kernel_size);

  // free memory starts from the end of PKE kernel and must be page-aligined
  free_mem_start_addr = ROUNDUP(g_kernel_end , PGSIZE);
  // recompute g_mem_size to limit the physical memory space that our riscv-pke kernel
  // needs to manage
  // PKE_MAX_ALLOWABLE_RAM 可以动态的分配，修改宏定义即可
  g_mem_size = MIN(PKE_MAX_ALLOWABLE_RAM, g_mem_size);
  if( g_mem_size < pke_kernel_size )
    panic( "Error when recomputing physical memory size (g_mem_size).\n" );

  
  free_mem_end_addr = g_mem_size + DRAM_BASE;
  sprint("free physical memory address: [0x%lx, 0x%lx] \n", free_mem_start_addr,
    free_mem_end_addr - 1);

  sprint("kernel memory manager is initializing ...\n");

  // pages 按 8 位对齐，与 status 对齐，保证 status 读写时的原子性
  pages = (page_t *)ROUNDUP((uint64)_end, 8);

  nr_pages = g_mem_size / PGSIZE;
  uint64 metaspace = nr_pages * sizeof(page_t);
  uint64 buddy_start = ROUNDUP((metaspace + (uint64)pages), PGSIZE);
  uint64 buddy_end = ROUNDDOWN(free_mem_end_addr, PGSIZE);

  memset(pages, 0, metaspace);
  
  // 初始 alloc 置为 3，保证内核部分代码不被修改
  for (int i = 0; i < nr_pages; ++i) {
    pages[i].status.alloc = 3;
  }

  buddy_system_init(buddy_start, buddy_end);
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

// 当前内存的主 Buddy System
buddy_system main_buddy;

void list_node_add(page_t *p, uint32 order) {
  p->node.next = main_buddy.free_area[order].next;
  p->node.pre = &main_buddy.free_area[order];
  main_buddy.free_area[order].next = p;
}

// 初始化物理空间，在 pmm_init() 中被调用
// 是否加上 inline？static 保证仅在该文件中被使用
static inline uint64 buddy_system_init(uint64 start_addr, uint64 end_addr) {
  // pages 数组的初始化在 pmm_init 中完成

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
    uint64 align_order = __builtin_ctzll(curr_addr / PGSIZE);

    // 计算剩余空间大小，取最高有效位作为 "limit"
    uint64 remaining_pages = (stop_addr - curr_addr) / PGSIZE;
    uint64 limit_order = 63 - __builtin_clzll(remaining_pages);

    uint64 order = align_order < limit_order ? align_order : limit_order;
    if (order > MAX_ORDER) order = MAX_ORDER - 1;

    uint64 PFN = (curr_addr - stop_addr) / PGSIZE;
    page_t *p = &pages[PFN];
    p->status.alloc = 0;
    p->status.is_head = 1;
    p->status.is_free = 1;
    p->status.order = order;
    list_node_add(p, order);

    curr_addr += (1ULL << order) * PGSIZE;
  }
}

void buddy_split(uint32 bigorder, uint32 order) {
  lock_acquire(main_buddy.buddy_lock.lock);
  uint32 curr_order = order + 1;
  while (curr_order < MAX_ORDER) {
    if (main_buddy.free_area[curr_order].next != NULL) {

    }
  }
}

// 不判断是否为空
void pop_list_head(uint32 order) {
  list_node *first_node = main_buddy.free_area[order].next;
}

void *buddy_alloc(uint32 order) {

  // 操作链表，只能上 Spinlock
  lock_acquire(main_buddy.buddy_lock.lock);

  for(int i = order; i < MAX_ORDER; ++i) {
    if (main_buddy.free_area[i].next == NULL)
      continue;
    if (i > order) 
      buddy_split(i, order);
    
    list_node *free_node = main_buddy.free_area[order].next;
    lock_release(main_buddy.buddy_lock.lock);
    
    
  }

  return NULL;
  list_node *free_node;
  if (main_buddy.free_area[order].next != NULL) {
    free_node = main_buddy.free_area[order].next;
    main_buddy.free_area[order].next = free_node->next;
    if (free_node->next != NULL)
      free_node->next->pre = &main_buddy.free_area[order];

    lock_release(main_buddy.buddy_lock.lock);
    return (void *)free_node;
  }
  lock_release(main_buddy.buddy_lock.lock);
}

#endif