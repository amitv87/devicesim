#ifndef SOCK_H
#define SOCK_H

#include "../util/io.h"

#define MAX_CLIENTS (10)

typedef enum{
  SOCKET_UDP,
  SOCKET_TCP,
  SOCKET_SCTP,
} socket_type;

typedef enum{
  SOCKET_CLOSED,
  SOCKET_CONNECTING,
  SOCKET_OPEN,
  SOCKET_CLOSING,
} socket_state;

typedef struct sock_client_s sock_client_t;
typedef struct sock_server_s sock_server_t;

typedef void (*client_on_open)(sock_client_t* client);
typedef void (*client_on_data)(sock_client_t* client, uint8_t* data, size_t length);
typedef void (*client_on_error)(sock_client_t* client);
typedef void (*client_on_close)(sock_client_t* client);

typedef void (*server_on_conn)(sock_server_t* server, sock_client_t* client);
typedef void (*server_on_data)(sock_server_t* server, sock_client_t* client, uint8_t* data, size_t length);
typedef void (*server_on_error)(sock_server_t* server, sock_client_t* client);
typedef void (*server_on_close)(sock_server_t* server, sock_client_t* client);

typedef union{
  struct{
    uint8_t nib2 : 4;
    uint8_t nib1 : 4;
  } __PACKED__;
  uint8_t b[4];
  uint32_t val;
} __PACKED__ ip_addr_v4_t;

typedef struct{
  client_on_open on_open;
  client_on_data on_data;
  client_on_error on_error;
  client_on_close on_close;
} sock_client_cb_t;

typedef struct sock_client_s{
  void* usr_data;
  socket_type type;
  io_handle_t io_handle;
  socket_state state;
  uint16_t remote_port;
  ip_addr_v4_t remote_addr;
  sock_client_cb_t* cb;
} sock_client_t;

typedef struct{
  server_on_conn on_conn;
  server_on_data on_data;
  server_on_error on_error;
  server_on_close on_close;
} sock_server_cb_t;

typedef struct sock_server_s{
  void* usr_data;
  socket_type type;
  io_handle_t io_handle;
  sock_server_cb_t* cb;
} sock_server_t;

void sock_server_init(sock_server_t* server, socket_type type);
bool sock_server_listen(sock_server_t* server, uint16_t port);
bool sock_server_send_to(sock_server_t* server, uint8_t* data, size_t length, sock_client_t* client);
bool sock_server_send_to_ex(sock_server_t* server, uint8_t* data, size_t length, ip_addr_v4_t remote_addr, uint16_t remote_port);
bool sock_server_close(sock_server_t* server);

void sock_client_int(sock_client_t* client, socket_type type);
bool sock_client_connect(sock_client_t* client, ip_addr_v4_t remote_addr, uint16_t remote_port);
bool sock_client_send(sock_client_t* client, uint8_t* data, size_t length);
bool sock_client_send_to(sock_client_t* client, uint8_t* data, size_t length, ip_addr_v4_t remote_addr, uint16_t remote_port);
bool sock_client_close(sock_client_t* client);

#endif
