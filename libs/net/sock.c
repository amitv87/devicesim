#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <fcntl.h>
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif

#include "sock.h"

#define CB(x, func) if(x->cb && x->cb->func) x->cb->func

static bool return_err(int fd, int rc, const char* msg){
  close(fd);
  perror(msg);
  return false;
}

static void on_client_activity(io_handle_t* handle, uint8_t fd_mode_mask){
  sock_client_t* client = handle->usr_data;
  int rc = 0;
  ip_addr_v4_t addr;
  uint16_t port = 0;;
  do_again:
  if(client->type == SOCKET_TCP){
    if(fd_mode_mask & FD_READ) rc = recv(client->io_handle.fd, io_rx_buff, sizeof(io_rx_buff), MSG_DONTWAIT);
    else{
      int opt_val = -1;
      socklen_t opt_len = sizeof(opt_val);
      int rc2 = getsockopt(client->io_handle.fd, SOL_SOCKET, SO_ERROR, &opt_val, &opt_len);
      LOG("getsockopt: %d, opt_val: %d, %s", rc2, opt_val, strerror(opt_val));
      if(rc2 || opt_val){
        goto clean_up;
      }
      else{
        client->io_handle.fd_mode_mask = FD_READ;
        client->state = SOCKET_OPEN;
        CB(client, on_open)(client);
      }
      return;
    }
  }
  else{
    struct sockaddr_in src;
    uint i = sizeof(src);
    rc = recvfrom(client->io_handle.fd, io_rx_buff, sizeof(io_rx_buff), MSG_DONTWAIT, (struct sockaddr*)&src , &i);
    addr.val = src.sin_addr.s_addr;
    port = htons(src.sin_port);
  }

  if(rc > 0){
    if(client->state == SOCKET_CONNECTING){
      client->io_handle.fd_mode_mask = FD_READ;
      client->state = SOCKET_OPEN;
      CB(client, on_open)(client);
    }

    CB(client, on_data)(client, io_rx_buff, rc);
    if(rc >= sizeof(io_rx_buff)) goto do_again;
  }
  else if(rc == -1){
    LOG("errno: %d -> %s", errno, strerror(errno));
    if(errno != EAGAIN && errno != EWOULDBLOCK) goto clean_up;
  }
  else{
    clean_up:;
    sock_client_close(client);
    CB(client, on_close)(client);
  }
}

static void on_server_activity(io_handle_t* handle, uint8_t fd_mode_mask){
  sock_server_t* server = handle->usr_data;
  int rc = 0;;
  struct sockaddr_in src;
  socklen_t len = sizeof(src);
  do_again:
  if(server->type == SOCKET_TCP){
    if(fd_mode_mask & FD_READ){
      struct sockaddr_in cli = {0};
      int connfd = accept(server->io_handle.fd, (struct sockaddr*)&src, &len);
      LOG("connfd: %d", connfd)
      if(connfd < 0) return_err(-1, -1, "connfd < 0");
      else{
        sock_client_t client = {
          .type = server->type,
          .io_handle = {
            .fd = connfd,
            .usr_data = &client,
            .fd_mode_mask = FD_READ,
            .cb = on_client_activity,
          },
          .remote_port = htons(src.sin_port),
          .remote_addr = {.val = src.sin_addr.s_addr},
        };
        CB(server, on_conn)(server, &client);
        if(client.cb) io_reg_handle(&client.io_handle);
      }
    }
  }
  else{
    rc = recvfrom(server->io_handle.fd, io_rx_buff, sizeof(io_rx_buff), MSG_DONTWAIT, (struct sockaddr*)&src , &len);
    ip_addr_v4_t addr = {.val = src.sin_addr.s_addr};
    uint16_t port = htons(src.sin_port);
    if(rc > 0){
      // CB(server, on_data)(server, io_rx_buff, rc);
      if(rc >= sizeof(io_rx_buff)) goto do_again;
    }
  }

  if(rc > 0){
    
  }
  else if(rc == -1){
    LOG("errno: %d -> %s", errno, strerror(errno));
    if(errno != EAGAIN && errno != EWOULDBLOCK) goto clean_up;
  }
  else{
    clean_up:;
    // sock_client_close(client);
    // CB(client, on_close)(client);
  }
}

void sock_server_init(sock_server_t* server, socket_type type){
  sock_server_close(server);
  server->type = type;
  server->io_handle = (io_handle_t){
    .usr_data = server,
    .fd_mode_mask = FD_READ,
    .cb = on_server_activity,
  };
}

bool sock_server_listen(sock_server_t* server, uint16_t port){
  int rc;
  int fd = socket(AF_INET, (server->type == SOCKET_TCP ? SOCK_STREAM : SOCK_DGRAM) | SOCK_NONBLOCK, (server->type == SOCKET_TCP ? IPPROTO_TCP : IPPROTO_UDP));
  LOG("fd: %d, type: %d", fd, server->type);
  if(fd < 0) return false;

  if(SOCK_NONBLOCK == 0) fcntl(server->io_handle.fd, F_SETFL, O_NONBLOCK | fcntl(server->io_handle.fd, F_GETFL, 0));

  #ifdef SO_REUSEADDR
  int reuseaddr = 1;
  if((rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr)) < 0)) return return_err(fd, rc, "sock setsockopt SO_REUSEADDR");
  #endif

  #ifdef SO_REUSEPORT
  int reuseport = 1;
  if((rc = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuseport, sizeof(reuseport)) < 0)) return return_err(fd, rc, "sock setsockopt SO_REUSEPORT");
  #endif

  struct sockaddr_in sock_addr = {0};
  sock_addr.sin_family = AF_INET;
  sock_addr.sin_port = htons(port);
  sock_addr.sin_addr.s_addr = 0;
  #ifndef __linux__
  sock_addr.sin_len = sizeof(struct sockaddr_in);
  #endif

  rc = bind(fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr));
  if(rc < 0) return return_err(fd, rc, "sock bind err");

  if(server->type == SOCKET_TCP){
    rc = listen(fd, 5);
    if(rc < 0) return return_err(fd, rc, "sock bind err");
  }

  server->io_handle.fd = fd;
  io_reg_handle(&server->io_handle);
  return true;
}

bool sock_server_send_to(sock_server_t* server, uint8_t* data, size_t length, sock_client_t* client);

bool sock_server_send_to_ex(sock_server_t* server, uint8_t* data, size_t length, ip_addr_v4_t remote_addr, uint16_t remote_port);

bool sock_server_close(sock_server_t* server){
  if(server->io_handle.fd <= 0) return false;
  io_dereg_handle(&server->io_handle);
  close(server->io_handle.fd);
  server->io_handle.fd = -1;
  return true;
}

void sock_client_int(sock_client_t* client, socket_type type);

bool sock_client_connect(sock_client_t* client, ip_addr_v4_t remote_addr, uint16_t remote_port);

bool sock_client_send(sock_client_t* client, uint8_t* data, size_t length){
  if(client->io_handle.fd < 0) return false;

  if(client->type == SOCKET_TCP) return send(client->io_handle.fd, data, length, 0) >= 0;
  // else if(dst_addr){
  //   struct sockaddr_in dest;
  //   dest.sin_family = AF_INET;
  //   dest.sin_port = pico_htons(dst_port);
  //   dest.sin_addr.s_addr = dst_addr->val;
  //   return sendto(sock->priv.fd, data, length, 0, (struct sockaddr*)&dest, sizeof(dest));
  // }
  return false;
}

bool sock_client_send_to(sock_client_t* client, uint8_t* data, size_t length, ip_addr_v4_t remote_addr, uint16_t remote_port);

bool sock_client_close(sock_client_t* client){
  if(client->io_handle.fd < 0) return false;
  client->state = SOCKET_CLOSING;
  io_dereg_handle(&client->io_handle);
  bool rc = close(client->io_handle.fd) == 0;
  client->io_handle.fd = -1;
  return rc;
}
