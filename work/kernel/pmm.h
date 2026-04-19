#ifndef _PMM_H_
#define _PMM_H_

#include "riscv.h"

// 减去起始位，算出index
#define PA_TO_IDX(pa) (((uint64)(pa) - DRAM_BASE) >> 12) 

// 最大的页表数
#define MAX_PAGE 32768

// 最高物理块的阶，需要在比赛环境下够用
// 最高阶的物理块大小为 16Mb
// 支持 Megapage (9 阶 2MB)
#define MAX_ORDER 12


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

// 记录每个页的元数据，同时提供染色信息，便于实现 O(1) 查询修改操作
typedef struct page {
    union {
        uint64 raw_status;
        struct {
            uint32 is_free : 1;
            uint32 is_head : 1;
            uint32 order   : 5;
            uint32 alloc   : 3;  // 归属标记，便于后面实现多核静态内存
            uint32 cpu_id  : 6;
            uint32 ref     : 16;
            uint32 color   : 8;
        };
    } status;
    list_node node;
} page_t;

// Initialize phisical memeory manager
void pmm_init();
// Allocate a free phisical page
void* alloc_page();
// Free an allocated page
void free_page(void* pa);

//page issue with atomic issue

void page_ref_share(void *pa);

short get_page_ref(void* pa);

void page_ref_inc(void *pa);

int page_ref_dec_and_test(void *pa);

int is_shared_page(void *pa);

#endif