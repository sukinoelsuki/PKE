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

#include "pmm.h"
#include "util/functions.h"
#include "riscv.h"
#include "config.h"
#include "util/string.h"
#include "memlayout.h"
#include "spike_interface/spike_utils.h"

// 按照写定的 page_t 结构，将节点的地址转换成 page 的首地址
#define NODE_TO_PAGE(ptr) ((page_t *)((char *)(ptr) - 8))

// 各类型计算转化
#define PFN_TO_PAGE(pfn)  (&pages[(pfn)])
#define PAGE_TO_PFN(p)    ((uint64)((p) - pages))
#define PFN_TO_ADDR(pfn)  ((void *)((pfn) * PGSIZE + DRAM_BASE))
#define ADDR_TO_PFN(addr) (((uint64)(addr) - DRAM_BASE) / PGSIZE)

// _end is defined in kernel/kernel.lds, it marks the ending (virtual) address of PKE kernel
extern char _end[];

// g_mem_size is defined in spike_interface/spike_memory.c, it indicates the size of our
// (emulated) spike machine. g_mem_size's value is obtained when initializing HTIF. 
extern uint64 g_mem_size;

// 链表节点，双向列表实现 O（1） 增删
// 和原有链表重名了，但是仅是增加了结构，并不会报错
// 注意命名规范
typedef struct List_Node {
  struct List_Node* prev;
  struct List_Node* next;
} list_node;

// 相关 list_node 双向链表操作
// 删去 prev 到 next 间的所有节点
static inline void __list_del(list_node *node) {
  node->prev->next = node->next;
  node->next->prev = node->prev;
}

// 将一段 new_list 插入到 prev 节点后
static inline void __list_add(list_node *prev, list_node* new_head, list_node *new_tail) {
  new_head->prev = prev;
  new_tail->next = prev->next;
  prev->next->prev = new_tail;
  prev->next = new_head;
}

// 不判断是否为空，保证安全
static inline list_node* list_pop(list_node *head) {
  list_node *node = head->next;
  __list_del(node);
  return node;
}

// 整体 Buddy 系统
typedef struct Buddy_System {
  spinlock_t buddy_lock;
  list_node free_area[MAX_ORDER];
} __attribute__((aligned(64))) buddy_system;

// 记录每个页的元数据，同时提供染色信息，便于实现 O(1) 查询修改操作
typedef struct page {
    union {
        uint64 raw_status;
        struct {
            uint32 is_free : 1;  // 空闲标记，1 仅表示在 buddy system 中空闲、能够使用。
            uint32 is_head : 1;  // 领头羊标记，仅有块首有标记，用于判断自己是否是“随从”
            uint32 order   : 5;  // 当前阶数，表明当前 page 代表的连续页块大小，从0阶开始
            uint32 alloc   : 3;  // 归属标记，标识属于的内存系统，Buddy System/ Slab、Slub/Per-CPU Cache/内核静态预留（不可释放）
            uint32 cpu_id  : 6;  // 亲和性标记，属于哪个核（最多支持 64），便于实现跨核释放
            uint32 ref     : 16; // 引用计数，实现 COW
            uint32 color   : 8;  // 审计染色，存储父块标记，便于追踪内存来源，排查错误
        };
    } status;
    list_node node;
} __attribute__((aligned(8))) page_t; // 注意！保证了 status 的原子性，但 node 仍大概率截断。因修改 node 概率小（存疑）

typedef struct percpu_cache {
  page_t *objs[PCPU_CACHE_SIZE];
  uint32 count;
} __attribute__((aligned(64))) percpu_cache_t;

// 物理页描述符数组指针
page_t *pages;
// total num of pa pages
uint64 nr_pages;

// Per-CPU Cache 实现“批发-零售”策略
percpu_cache_t pcpu_caches[NCPU];

static uint64 free_mem_start_addr;  //beginning address of free memory
static uint64 free_mem_end_addr;    //end address of free memory (not included)

// 当前内存的主 Buddy System
buddy_system main_buddy;

// 初始化物理空间，在 pmm_init() 中被调用
// 是否加上 inline？static 保证仅在该文件中被使用
static inline uint64 buddy_system_init(uint64 start_addr, uint64 end_addr) {
  // pages 数组的初始化在 pmm_init 中完成

  // 初始化每阶的链表头结点，构造双向循环链表
  for (int i = 0; i < MAX_ORDER; ++i) {
    main_buddy.free_area[i].next = &main_buddy.free_area[i];
    main_buddy.free_area[i].prev = &main_buddy.free_area[i];
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
    if (order >= MAX_ORDER) order = MAX_ORDER - 1;

    uint64 PFN = ADDR_TO_PFN(curr_addr);
    page_t *p = PFN_TO_PAGE(PFN);
    p->status.alloc = 0;
    p->status.is_head = 1;
    p->status.is_free = 1;
    p->status.order = order;
    __list_add(&main_buddy.free_area[order], &p->node, &p->node);

    curr_addr += (1ULL << order) * PGSIZE;
  }
}

void buddy_split(uint32 big_order, uint32 target_order) {
  for (int i = big_order; i > target_order; i--) {
    // 将当前阶第一个空闲的 page 取出来
    list_node *n = list_pop(&main_buddy.free_area[i]);
    page_t *p = node_to_page(n);
    
    // 计算当前页的 buddy，以便实现分割
    // 分割意味着降级了，所以在下一阶上加 1 就是它的 buddy
    uint64 buddy_pfn = (uint64)(p - pages) + (1ULL << (i - 1));
    page_t *bud = &pages[buddy_pfn];
    bud->status.alloc = 0;
    bud->status.is_free = 1;
    bud->status.is_head = 1;
    bud->status.order = i - 1;
    
    // 将分割开的两个页块从表头插入链表
    __list_add(&main_buddy.free_area[i - 1], &bud->node, &bud->node);
    p->status.order = i - 1;
    // 写作 n 少一些计算，应该会更快
    __list_add(&main_buddy.free_area[i - 1], n, n);
  }
}


void *buddy_alloc(uint32 order) {

  // 操作链表，只能上 Spinlock
  lock_acquire(&main_buddy.buddy_lock.lock);

  for(int i = order; i < MAX_ORDER; ++i) {
    if (main_buddy.free_area[i].next == &main_buddy.free_area[i])
      continue;

    if (i > order) 
      buddy_split(i, order);
    
    list_node *free_node = list_pop(&main_buddy.free_area[order]);
    page_t *p = NODE_TO_PAGE(free_node);
    
    lock_release(&main_buddy.buddy_lock.lock);
    // 出厂设置，状态预置为 percpu_cache
    p->status.is_free = 0;
    p->status.alloc = 1;

    // 返回物理地址
    return PFN_TO_ADDR(PAGE_TO_PFN(p));
  }

  // 释放全局锁
  lock_release(&main_buddy.buddy_lock.lock);
  panic("buddy_alloc : Out of required size of memory!\n");
  return NULL;
}

static inline void __buddy_merge_inner(page_t *p) {
  uint64 pfn = PAGE_TO_PFN(p);
  uint64 order = p->status.order;
  
  p->status.is_free = 0;
  p->status.is_head = 0;
  p->status.alloc = 0;
  
  while (order < (MAX_ORDER - 1)) {
    page_t *my_buddy = PFN_TO_PAGE(pfn ^ (1ULL << order));

    if (my_buddy->status.order != order || my_buddy->status.is_free != 1 || my_buddy->status.is_head != 1) 
      break;

    __list_del(&my_buddy->node);
    my_buddy->status.is_free = 0;
    my_buddy->status.is_head = 0;

    pfn &= ~(1ULL << order);
    p = PFN_TO_PAGE(pfn);
    order++;
    p->status.order = order;
  }
  p->status.is_free = 1;
  p->status.is_head = 1;
  p->status.alloc = 0;
  __list_add(&main_buddy.free_area[order], &p->node, &p->node);
}

// 带锁的 __buddy_merge_inner，以供非连续性释放空间时使用
void buddy_free(void *addr) {
  // 经过分支预测，几乎没有性能损耗，同时监测了错误
  if (unlikely(!addr)){
    panic("buddy_free : NULL address!\n");
    return;
  }
  uint64 pfn = ADDR_TO_PFN(addr);
  lock_acquire(&main_buddy.buddy_lock.lock);
  __buddy_merge_inner(PFN_TO_PAGE(pfn));
  lock_release(&main_buddy.buddy_lock.lock);
}

void pfree(void* addr) {
  page_t *p = PFN_TO_PAGE(ADDR_TO_PFN(addr));

  if (unlikely(p->status.order != 0)) {
    push_off();
    lock_acquire(&main_buddy.buddy_lock.lock);
    __buddy_merge_inner(p);
    lock_release(&main_buddy.buddy_lock.lock);
    pop_off();
    return;
  }

  push_off();
  uint64 id = read_tp();

  if (pcpu_caches[id].count == PCPU_CACHE_SIZE) {
    lock_acquire(&main_buddy.buddy_lock.lock);
    for (int i = 0; i < 16; i++) {
      __buddy_merge_inner(pcpu_caches[id].objs[--pcpu_caches[id].count]);
    }
    lock_release(&main_buddy.buddy_lock.lock);
  }

  pcpu_caches[id].objs[++pcpu_caches[id].count] = p;
  pop_off();
}

// 对私有 Cache 集中释放 PERCPU_CACHE_SIZE / 2 个页块（拿一次锁）
void pfree_batch(uint64 curr_cpu_id) {
  lock_acquire(&main_buddy.buddy_lock.lock);

  // 每次只还 16 个，避免释放后马上取用
  for (int i = 0; i < 16; i++) 
    __buddy_merge_inner(pcpu_caches[curr_cpu_id].objs[--pcpu_caches[curr_cpu_id].count]);

  lock_release(&main_buddy.buddy_lock.lock);
}

// 只分配了大小为 4KB 的块
void *alloc_page(void) {
  uint64 cpu_id = read_tp();

  if (pcpu_caches[cpu_id].count <= 2) {
    // 从 buddy system 中批发
    lock_acquire(&main_buddy.buddy_lock.lock);
    for (int i = 0; i < 16; i++) {
      pcpu_caches[cpu_id].objs[pcpu_caches[cpu_id].count++] = PFN_TO_PAGE(ADDR_TO_PFN(buddy_alloc(0)));
    }
    lock_release(&main_buddy.buddy_lock.lock);
  }

  // 在 percpu cache 中零售
  return PFN_TO_ADDR(PAGE_TO_PFN(pcpu_caches[cpu_id].objs[--pcpu_caches[cpu_id].count]));
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

  // 预先将每个核的 cache 填满
  for (int i = 0; i < NCPU; i++) {
    for (int j = 0; j < PCPU_CACHE_SIZE; j++) {
      pcpu_caches[i].objs[j] = PFN_TO_PAGE(ADDR_TO_PFN(buddy_alloc(0)));
    }
    pcpu_caches[i].count = PCPU_CACHE_SIZE;
  }
}

#endif