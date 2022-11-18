#include "time.h"
#include <time.h>
#include <sys/time.h>

static uint64_t start_time = 0;

uint64_t sys_now(){
  struct timespec uptime;
  clock_gettime(CLOCK_MONOTONIC_RAW, &uptime);
  return (uptime.tv_sec * 1000) + (uptime.tv_nsec / (1000 * 1000));
}

uint64_t uptime(){
  if(!start_time) start_time = sys_now();
  return (sys_now() - start_time);
}

uint64_t clock_now(){
  struct timeval tv = {};
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000ULL) + (tv.tv_usec / 1000ULL);
}
