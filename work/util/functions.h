#ifndef _FUNCTIONS_H_
#define _FUNCTIONS_H_

// 除法实现，性能不足，且为 0 时有巨大漏洞
/*
#define ROUNDUP(a, b) ((((a)-1) / (b) + 1) * (b))
#define ROUNDDOWN(a, b) ((a) / (b) * (b))
*/

// 位运算通过增量实现，高速，无 0 溢出漏洞
#define ROUNDUP(a, b) (((a) + (b) - 1) & ~((b) - 1))
#define ROUNDDOWN(a, b) ((a) & ~((b) - 1))

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

char* safestrcpy(char*, const char*, int);

#endif