#ifndef _PMM_H_
#define _PMM_H_

#include "riscv.h"

// 减去起始位，算出index
#define PA_TO_IDX(pa) (((uint64)(pa) - DRAM_BASE) >> 12) 

// 最大的页表数
#define MAX_PAGE 32768

// 最高阶数，需要在比赛环境下够用
// 最高阶的物理块大小为 4Mb，阶数为 10
// 支持 Megapage (9 阶 2MB)，增加 TLB 命中效率
#define MAX_ORDER 11

#define PCPU_CACHE_SIZE 32

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

void *buddy_alloc(uint32 order);

#endif