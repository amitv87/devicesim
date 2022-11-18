#ifndef UTIL_SPAWN_H
#define UTIL_SPAWN_H

#include "io.h"

typedef enum{
  IO_STDIN,
  IO_STDOUT,
  IO_STDERR,
  IO_STDMAX,
} io_std_type;

typedef enum{
  SPAWN_STARTED,
  SPAWN_STOPPED,
} io_spawn_state;

typedef union{
  struct{
    int rd_fd;
    int wr_fd;
  };
  int fds[2];
} io_pipe;

typedef struct{
  int file_no;
  io_pipe pipe;
  io_handle_t handle;
  void* usr_data;
} io_std_pipe;

typedef struct io_spawn_s io_spawn_t;
typedef void (*spawn_on_state)(io_spawn_t* spawn, io_spawn_state state, void* info);
typedef void (*spawn_on_activity)(io_spawn_t* spawn, io_std_type type, uint8_t* data, size_t size);

typedef struct io_spawn_cb_s{
  spawn_on_state on_state;
  spawn_on_activity on_activity;
} io_spawn_cb_t;

typedef struct io_spawn_s{
  int pid;
  size_t argc;
  char** argv, ** env;
  io_spawn_cb_t* cb;
  void* usr_data;
  io_std_pipe pipes[IO_STDMAX];
} io_spawn_t;

bool io_spawn_start(io_spawn_t* spawn);
void io_spawn_wait(io_spawn_t* spawn, bool stop);
int io_spawn_input(io_spawn_t* spawn, uint8_t* data, size_t length);

#endif
