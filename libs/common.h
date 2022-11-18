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

#define countof(x) (sizeof(x)/sizeof(x[0]))

#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#define LOG(fmt, ...) fprintf(stderr, "[%llu|%s:%d|%s] " fmt "\r\n", uptime(), __FILENAME__, __LINE__, __func__, ##__VA_ARGS__);

#define CHK_ERR(func, ...) if((ret = func(__VA_ARGS__))){fprintf(stderr, #func " ret: %d, err: %d, msg: %s\r\n", ret, errno, strerror(errno));}

typedef struct{
  uint8_t* bytes;
  size_t r_idx, w_idx, len;
} io_buff_t;

#endif
