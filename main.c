#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <at/engine.h>
#include <nmea/generator.h>

#include "at_cmd.h"

typedef struct{
  io_handle_t input_handle;
  bool use_stdin;
  int output_fd;
  uint32_t baud;
  char* name;
  char* tty_path;
  char* slave_path;
  char* symlink_path;
  bool is_symlinked;
} transport_t;

static bool make_symlink(char* dest, char* src){
  unlink(dest);
  int rc = symlink(src, dest);
  if(rc != 0){
    LOG("symlink %s -> %s, failed with rc: %d, msg: %s (%d)", dest, src, rc, strerror(errno), errno);
    return false;
  }
  return true;
}

static bool setup_transport(transport_t* transport){
  transport->slave_path = 0;
  if(transport->tty_path) transport->input_handle.fd = transport->output_fd = io_tty_open(transport->tty_path, transport->baud, &transport->slave_path);
  else {
    transport->output_fd = STDOUT_FILENO;
    if(transport->use_stdin) transport->input_handle.fd = STDIN_FILENO;
  }
  if(transport->output_fd < 0){
    LOG("open %s -> err: %d, msg: %s", transport->tty_path, errno, strerror(errno));
    return false;
  }
  if(transport->input_handle.fd >= 0) io_reg_handle(&transport->input_handle);

  if(transport->tty_path){
    transport->is_symlinked = make_symlink(transport->symlink_path, transport->slave_path);
    LOG("%s path: %s", transport->name, transport->is_symlinked ? transport->symlink_path : transport->slave_path);
  }

  return transport->output_fd >= 0;
}

static void close_transport(transport_t* transport){
  if(transport->input_handle.fd >= 0) io_dereg_handle(&transport->input_handle);
  transport->input_handle.fd = -1;
  if(transport->tty_path && transport->output_fd >= 0) close(transport->output_fd);
  if(transport->is_symlinked) unlink(transport->symlink_path);
}

static void handle_read_error(transport_t* transport, int rc){
  if(rc < 0){LOG("read rc: %d, err: %d, msg: %s\r\n", rc, errno, strerror(errno));}
  else{LOG("%s -> EOF", transport->tty_path ? transport->tty_path : "stdin");}
  close(transport->input_handle.fd);
  io_dereg_handle(&transport->input_handle);
  transport->input_handle.fd = -1;
  if(transport->slave_path && setup_transport(transport));
  else io_stop_loop();
}

static int ate_output(at_engine_t *engine, uint8_t* data, size_t len){
  transport_t* transport = engine->usr_data;
  return io_write(transport->output_fd, data, len);
}

static void on_ate_input(io_handle_t* handle, uint8_t fd_mode_mask){
  int rc = read(handle->fd, io_rx_buff, sizeof(io_rx_buff));
  if(rc > 0) at_engine_input(handle->usr_data, 0, io_rx_buff, rc);
  else handle_read_error((transport_t*)handle, rc);
}

static void fetch_loc_info(nmea_gen_t *gen, float lat_lng[2]){
  if(lat_lng[0] == 0) lat_lng[0] = 19.0051252, lat_lng[1] = 73.0332695;
  else lat_lng[0] += 0.000001, lat_lng[1] += 0.000004;
}

static int on_nmea_output(nmea_gen_t *gen, char* sentence, size_t length){
  transport_t* transport = gen->usr_data;
  return io_write(transport->output_fd, (uint8_t*)sentence, length);
}

static void on_nmea_input(io_handle_t* handle, uint8_t fd_mode_mask){
  int rc = read(handle->fd, io_rx_buff, sizeof(io_rx_buff));
  if(rc > 0); // todo nmea_input
  else handle_read_error((transport_t*)handle, rc);
}

static at_engine_t engine = {0};

#define SAT_INF(p,e,a,s) {.prn = p, .ele = e, .azi = a, .snr = s}
#define SAT_GRP(x, mc, ...) [NMEA_TALKER(x)] = {.max_count = mc, .sats = __VA_ARGS__}
static nmea_gen_t nmea_gen = {
  .output = on_nmea_output,
  .fetch_loc = fetch_loc_info,
  .sat_groups = {
    SAT_GRP(GP, 12, {
      SAT_INF(4,67,39,36),
      SAT_INF(9,45,336,43),
      SAT_INF(8,39,132,17),
      SAT_INF(3,38,191,19),
      SAT_INF(27,33,88,23),
      SAT_INF(16,31,35,38),
      SAT_INF(7,30,302,40),
      SAT_INF(42,12,97,0),
      SAT_INF(30,6,277,35),
      SAT_INF(14,5,222,0),
      SAT_INF(26,3,41,0),
      SAT_INF(1,1,183,0),
    }),
    SAT_GRP(GL, 12, {
      SAT_INF(79,0,0,22),
    }),
    SAT_GRP(GA, 12, {
      SAT_INF(30,75,342,34),
      SAT_INF(8,69,86,0),
      SAT_INF(2,27,34,34),
      SAT_INF(3,23,126,36),
      SAT_INF(15,1,171,19),
    }),
    SAT_GRP(QZ, 12, {
      SAT_INF(2,23,52,32),
      SAT_INF(3,6,96,28),
    }),
  },
};

static transport_t ate_transport = {
  .input_handle = {
    .fd_mode_mask = FD_READ | FD_EXCEPT,
    .usr_data = &engine, .cb = on_ate_input,
  },
  .use_stdin = true,
  .output_fd = -1, .baud = 0,
  .name = "gsm_device",
  .symlink_path = "/tmp/tty.gsm",
};

static transport_t nmea_transport = {
  .input_handle = {
    .fd_mode_mask = FD_READ | FD_EXCEPT,
    .usr_data = &nmea_gen, .cb = on_nmea_input,
  },
  .name = "gnss_device",
  .output_fd = -1, .baud = 0,
  .symlink_path = "/tmp/tty.gnss",
};

static transport_t* transports[] = {&ate_transport, &nmea_transport};

int main(int argc, char *argv[]){
  bool is_path = false, is_baud = false;
  for(char c; (c = getopt(argc, argv, "pa:ba:pn:bn:")) != -1;){
    switch (c){
      case 'p': is_path = true; break;
      case 'b': is_baud = true; break;
      case 'a':
        if(is_path) ate_transport.tty_path = optarg;
        else if(is_baud) ate_transport.baud = atoi(optarg);
        is_path = is_baud = false;
        break;
      case 'n':
        if(is_path) nmea_transport.tty_path = optarg;
        else if(is_baud) nmea_transport.baud = atoi(optarg);
        is_path = is_baud = false;
        break;
      case '?': LOG("Unknown option: %c, %s", optopt, optarg); return 1;
    }
  }

  engine.usr_data = &ate_transport;
  nmea_gen.usr_data = &nmea_transport;

  io_init_loop();

  for(int i = 0; i < countof(transports); i++) if(!setup_transport(transports[i])) return -1;

  at_engine_init(&engine, ate_output);
  for(int i = 0; i < countof(cmds); i++) at_engine_add_cmd(&engine, &cmds[i]);
  at_engine_start(&engine);

  nmea_set_talker(&nmea_gen, NMEA_SENTENCE(RMC), NMEA_TALKER(GN), 1);
  nmea_set_talker(&nmea_gen, NMEA_SENTENCE(GGA), NMEA_TALKER(GN), 1);
  nmea_set_talker(&nmea_gen, NMEA_SENTENCE(GSA), NMEA_TALKER(GP), 1);
  nmea_set_talker(&nmea_gen, NMEA_SENTENCE(GSA), NMEA_TALKER(GL), 1);
  nmea_set_talker(&nmea_gen, NMEA_SENTENCE(GSA), NMEA_TALKER(GA), 1);
  nmea_set_talker(&nmea_gen, NMEA_SENTENCE(QSA), NMEA_TALKER(QZ), 1);
  nmea_set_talker(&nmea_gen, NMEA_SENTENCE(GSV), NMEA_TALKER(GP), 1);
  nmea_set_talker(&nmea_gen, NMEA_SENTENCE(GSV), NMEA_TALKER(GL), 1);
  nmea_set_talker(&nmea_gen, NMEA_SENTENCE(GSV), NMEA_TALKER(GA), 1);
  nmea_set_talker(&nmea_gen, NMEA_SENTENCE(GSV), NMEA_TALKER(QZ), 1);
  nmea_set_talker(&nmea_gen, NMEA_SENTENCE(GST), NMEA_TALKER(GP), 1);
  nmea_gen_start(&nmea_gen, 1000);

  io_run_loop();
  LOG("exitting...");
  at_engine_stop(&engine);
  for(int i = 0; i < countof(transports); i++) close_transport(transports[i]);
  return 0;
}
