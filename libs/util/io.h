#ifndef UTIL_IO_H
#define UTIL_IO_H

#include "list.h"

#define FD_READ   (1 << 0)
#define FD_WRITE  (1 << 1)
#define FD_EXCEPT (1 << 2)

typedef struct io_timer_s io_timer_t;
typedef struct io_handle_s io_handle_t;

typedef void (*io_timer_cb)(io_timer_t* timer);
typedef void (*on_fd_activity)(io_handle_t* handle, uint8_t fd_mode_mask);

typedef struct io_timer_s{
  list_item_t item;
  bool repeat;
  bool run_now;
  bool skip_next;
  uint32_t last_ms;
  uint32_t interval_ms;
  void* usr_data;
  io_timer_cb cb;
} io_timer_t;

typedef struct io_handle_s{
  int fd;
  on_fd_activity cb;
  uint8_t fd_mode_mask;
  void* usr_data;
} io_handle_t;

extern uint8_t io_rx_buff[1024];

void io_init_loop();
void io_run_loop();
void io_stop_loop();
uint64_t io_step_loop(uint32_t timeout_ms);

void io_add_timer(io_timer_t* timer);
void io_rem_timer(io_timer_t* timer);

void io_reg_handle(io_handle_t* handle);
void io_dereg_handle(io_handle_t* handle);

int io_write(int fd, uint8_t* data, size_t length);

int io_tty_open(char* path, uint32_t baud_rate, char** slave_path);

#endif
