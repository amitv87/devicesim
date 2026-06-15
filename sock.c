// #include <ctype.h>
// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <unistd.h>

#include <net/sock.h>

void cli_on_open(sock_client_t* client){
  LOG();
}
void cli_on_data(sock_client_t* client, uint8_t* data, size_t length){
  LOG("%.*s", (int)length, (char*)data);
}
void cli_on_error(sock_client_t* client){
  LOG();
}
void cli_on_close(sock_client_t* client){
  LOG();
}

static sock_client_cb_t cli_cb = {
  .on_open = cli_on_open,
  .on_data = cli_on_data,
  .on_error = cli_on_error,
  .on_close = cli_on_close,
};

void srv_on_conn(sock_server_t* server, sock_client_t* client){
  client->cb = &cli_cb;
  LOG();
}
void srv_on_data(sock_server_t* server, sock_client_t* client, uint8_t* data, size_t length){
  LOG("%.*s", (int)length, (char*)data);
}
void srv_on_error(sock_server_t* server, sock_client_t* client){
  LOG();
}
void srv_on_close(sock_server_t* server, sock_client_t* client){
  LOG();
}

static sock_server_cb_t srv_cb = {
  .on_conn = srv_on_conn,
  .on_data = srv_on_data,
  .on_error = srv_on_error,
  .on_close = srv_on_close,
};

static sock_server_t server = {
  .cb = &srv_cb,
};

int main(int argc, char *argv[]){
  LOG("starting...");
  sock_server_init(&server, SOCKET_TCP);
  sock_server_listen(&server, 3030);
  io_run_loop();
  LOG("exitting...");
  return 0;
}
