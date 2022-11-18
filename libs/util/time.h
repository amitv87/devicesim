#ifndef UTIL_TIME_H
#define UTIL_TIME_H

#include <time.h>
#include <stdint.h>
#include <sys/time.h>

uint64_t uptime();
uint64_t sys_now();
uint64_t clock_now();

#endif
