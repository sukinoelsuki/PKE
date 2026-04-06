#ifndef _PMM_H_
#define _PMM_H_

#include "riscv.h"

//减去起始位，算出index
#define PA_TO_IDX(pa) (((uint64)(pa) - DRAM_BASE) >> 12) 
//最大的页表数
#define MAX_PAGE 32768

// Initialize phisical memeory manager
void pmm_init();
// Allocate a free phisical page
void* alloc_page();
// Free an allocated page
void free_page(void* pa);

//page sharing issue

void page_ref_share(void *pa);

short __get_page_ref(void* pa);

void __page_ref_inc(void *pa);

void __page_ref_dec(void *pa);

int __is_shared_page(void *pa);

#endif