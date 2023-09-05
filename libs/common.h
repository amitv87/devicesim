#ifndef AT_COMMON_H
#define AT_COMMON_H

#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "util/time.h"

#define __PACKED__ __attribute__((__packed__))
#define ASSERT_STRUCT_SIZE(x, y) _Static_assert(sizeof(x) == y, "invalid struct " #x)

#define min(a,b)           \
({                         \
  __typeof__ (a) _a = (a); \
  __typeof__ (b) _b = (b); \
  _a < _b ? _a : _b;       \
})

#define countof(x) (sizeof(x)/sizeof(x[0]))

#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#define LOG(fmt, ...) fprintf(stderr, "[%llu|%s:%d|%s] " fmt "\r\n", uptime(), __FILENAME__, __LINE__, __func__, ##__VA_ARGS__);

#define HDUMP(data, length, fmt, ...) hexdump(data, length, fmt "\r\n", ##__VA_ARGS__)

#define CHK_ERR(func, ...) if((ret = func(__VA_ARGS__))){LOG(#func " ret: %d, err: %d, msg: %s", ret, errno, strerror(errno));}

typedef struct{
  uint8_t* bytes;
  size_t r_idx, w_idx, len;
} io_buff_t;

void hexdump(uint8_t* data, size_t length, const char* header, ...);

#endif
