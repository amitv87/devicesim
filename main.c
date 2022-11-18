#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <at/engine.h>
#include <nmea/generator.h>

#include "at_cmd.h"

static bool setup_ate_transport();
static bool setup_nmea_transport();

static uint32_t ate_baud = 0, nmea_baud = 0;
static char *ate_paths[2] = {0}, *nmea_paths[2] = {0};
static int ate_output_fd = -1, nmea_output_fd = -1;

static int ate_output(at_engine_t *engine, uint8_t* data, size_t len){
  return io_write(ate_output_fd, data, len);
}

static void on_ate_input(io_handle_t* handle, uint8_t fd_mode_mask){
  int rc = read(handle->fd, io_rx_buff, sizeof(io_rx_buff));
  if(rc > 0) at_engine_input(handle->usr_data, 0, io_rx_buff, rc);
  else{
    if(rc < 0){LOG("read rc: %d, err: %d, msg: %s\r\n", rc, errno, strerror(errno));}
    else{LOG("%s -> EOF", ate_paths[0] ? ate_paths[0] : "stdin");}
    close(handle->fd);
    io_dereg_handle(handle);
    handle->fd = -1;
    if(ate_paths[1] && setup_ate_transport());
    else io_stop_loop();
  }
}

static void fetch_loc_info(nmea_gen_t *gen, float lat_lng[2]){
  if(lat_lng[0] == 0) lat_lng[0] = 19.0051252, lat_lng[1] = 73.0332695;
  else lat_lng[0] += 0.000001, lat_lng[1] += 0.000004;
}

static int on_nmea_output(nmea_gen_t *gen, char* sentence, size_t length){
  return io_write(nmea_output_fd, (uint8_t*)sentence, length);
}

static void on_nmea_input(io_handle_t* handle, uint8_t fd_mode_mask){
  int rc = read(handle->fd, io_rx_buff, sizeof(io_rx_buff));
  if(rc > 0); // todo nmea_input
  else{
    if(rc < 0){LOG("read rc: %d, err: %d, msg: %s\r\n", rc, errno, strerror(errno));}
    else{LOG("%s -> EOF", nmea_paths[0] ? nmea_paths[0] : "stdin");}
    close(handle->fd);
    io_dereg_handle(handle);
    handle->fd = -1;
    if(ate_paths[1] && setup_nmea_transport());
    else io_stop_loop();
  }
}

static at_engine_t engine = {0};

static io_handle_t ate_input_handle = {
  .usr_data = &engine,
  .cb = on_ate_input,
  .fd_mode_mask = FD_READ | FD_EXCEPT,
};

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

static io_handle_t nmea_input_handle = {
  .usr_data = &nmea_gen,
  .cb = on_nmea_input,
  .fd_mode_mask = FD_READ | FD_EXCEPT,
};

static char* make_symlink(char* dest, char* src){
  unlink(dest);
  int rc = symlink(src, dest);
  if(rc != 0){
    LOG("symlink %s -> %s, failed with rc: %d, msg: %s (%d)", dest, src, rc, strerror(errno), errno);
    return NULL;
  }
  return dest;
}

static bool setup_ate_transport(){
  ate_paths[1] = 0;
  if(ate_paths[0]) ate_input_handle.fd = ate_output_fd = io_tty_open(ate_paths[0], ate_baud, &ate_paths[1]);
  else ate_output_fd = STDOUT_FILENO, ate_input_handle.fd = STDIN_FILENO;

  if(ate_input_handle.fd >= 0) io_reg_handle(&ate_input_handle);
  else LOG("open %s -> err: %d, msg: %s", ate_paths[0], errno, strerror(errno));

  if(ate_paths[1]){
    char* symlink_path = make_symlink("/tmp/tty.gsm", ate_paths[1]);
    LOG("gsm_device_path: %s", symlink_path ? symlink_path : ate_paths[1]);
  }

  return ate_input_handle.fd >= 0;
}

static bool setup_nmea_transport(){
  nmea_paths[1] = 0;
  nmea_input_handle.fd = -1;
  if(nmea_paths[0]) nmea_input_handle.fd = nmea_output_fd = io_tty_open(nmea_paths[0], nmea_baud, &nmea_paths[1]);
  else nmea_output_fd = STDOUT_FILENO;

  if(nmea_input_handle.fd > 0) io_reg_handle(&nmea_input_handle);
  else if(nmea_paths[0]) LOG("open %s -> err: %d, msg: %s", nmea_paths[0], errno, strerror(errno));

  if(nmea_paths[1]){
    char* symlink_path = make_symlink("/tmp/tty.gnss", nmea_paths[1]);
    LOG("gnss_device_path: %s", symlink_path ? symlink_path : nmea_paths[1]);
  }
  return nmea_output_fd >= 0;
}

int main(int argc, char *argv[]){
  bool is_path = false, is_baud = false;
  for(char c; (c = getopt(argc, argv, "pa:ba:pn:bn:")) != -1;){
    switch (c){
      case 'p': is_path = true; break;
      case 'b': is_baud = true; break;
      case 'a':
        if(is_path) ate_paths[0] = optarg;
        else if(is_baud) ate_baud = atoi(optarg);
        is_path = is_baud = false;
        break;
      case 'n':
        if(is_path) nmea_paths[0] = optarg;
        else if(is_baud) nmea_baud = atoi(optarg);
        is_path = is_baud = false;
        break;
      case '?': LOG("Unknown option: %c, %s", optopt, optarg); return 1;
    }
  }

  io_init_loop();
  if(!setup_ate_transport()) return -1;
  if(!setup_nmea_transport()) return -1;

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
  io_dereg_handle(&ate_input_handle);
  io_dereg_handle(&nmea_input_handle);
  if(ate_paths[0]) close(ate_output_fd);
  if(nmea_paths[0]) close(nmea_output_fd);
  return 0;
}
