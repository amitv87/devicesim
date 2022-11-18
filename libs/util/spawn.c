#include "spawn.h"
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#define CB(x, y, ...) if(x->cb && x->cb->y) x->cb->y(spawn, ##__VA_ARGS__)

#define SWAP_BLOCK(...) { \
  if(iop->file_no == STDIN_FILENO) swap(iop->pipe.fds); \
  __VA_ARGS__ \
  if(iop->file_no == STDIN_FILENO) swap(iop->pipe.fds); \
}

static void swap(int nums[2]){
  int num = nums[0];
  nums[0] = nums[1];
  nums[1] = num;
}

static void on_stdio(io_handle_t* handle, uint8_t fd_mode_mask){
  io_std_pipe* iop = handle->usr_data;
  io_spawn_t* spawn = iop->usr_data;
  if(!spawn) return;
  if(fd_mode_mask & FD_READ){
    int rc = read(handle->fd, io_rx_buff, sizeof(io_rx_buff) - 2);
    if(!rc) goto except;
    else CB(spawn, on_activity, iop->file_no, io_rx_buff, rc);
  }
  if(fd_mode_mask & FD_EXCEPT) except: io_spawn_wait(spawn, false);
}

bool io_spawn_start(io_spawn_t* spawn){
  int ret;
  bool rc = false;

  if(spawn->pid > 0) return rc;

  posix_spawn_file_actions_t child_fd_actions;
  CHK_ERR(posix_spawn_file_actions_init, &child_fd_actions);

  for(int i = 0; i < countof(spawn->pipes); i++){
    io_std_pipe* iop = &spawn->pipes[i];
    iop->file_no = STDIN_FILENO + i;
    // if(iop->file_no == STDIN_FILENO) continue;
    CHK_ERR(pipe, iop->pipe.fds);
    SWAP_BLOCK({
      CHK_ERR(posix_spawn_file_actions_addclose, &child_fd_actions, iop->pipe.fds[0]);
      CHK_ERR(posix_spawn_file_actions_adddup2, &child_fd_actions, iop->pipe.fds[1], iop->file_no);
      CHK_ERR(posix_spawn_file_actions_addclose, &child_fd_actions, iop->pipe.fds[1]);

      if(iop->file_no != STDIN_FILENO){
        iop->usr_data = spawn;
        iop->handle = (io_handle_t){
          .cb = on_stdio,
          .usr_data = iop,
          .fd = iop->pipe.fds[0],
          .fd_mode_mask = (iop->file_no == STDIN_FILENO ? FD_WRITE : FD_READ) | FD_EXCEPT,
        };
      }
      else iop->usr_data = NULL;
    });
    LOG("pipe file_no: %d -> ret: %d, rd_fd: %d, wr_fd: %d", iop->file_no, ret, iop->pipe.rd_fd, iop->pipe.wr_fd);
  }

  CHK_ERR(posix_spawnp, &spawn->pid, spawn->argv[0], &child_fd_actions, NULL, spawn->argv, spawn->env);
  CHK_ERR(posix_spawn_file_actions_destroy, &child_fd_actions);

  if((rc = ret == 0)){
    for(int i = 0; i < countof(spawn->pipes); i++){
      io_std_pipe* iop = &spawn->pipes[i];
      SWAP_BLOCK(close(iop->pipe.fds[1]););
      if(iop->usr_data) io_reg_handle(&iop->handle);
    }
    CB(spawn, on_state, SPAWN_STARTED, &spawn->pid);
  }
  return rc;
}

void io_spawn_wait(io_spawn_t* spawn, bool stop){
  if(spawn->pid <= 0) return;
  if(stop) kill(spawn->pid, SIGINT);
  int status;
  waitpid(spawn->pid, &status, 0);
  spawn->pid = 0;
  io_step_loop(0);
  for(int i = 0; i < countof(spawn->pipes); i++){
    io_std_pipe* iop = &spawn->pipes[i];
    SWAP_BLOCK(close(iop->pipe.fds[0]););
    if(iop->usr_data) io_dereg_handle(&iop->handle);
    iop->usr_data = NULL;
  }
  CB(spawn, on_state, SPAWN_STOPPED, &status);
}

int io_spawn_input(io_spawn_t* spawn, uint8_t* data, size_t length){
  return write(spawn->pipes[IO_STDIN].pipe.wr_fd, data, length);
}
