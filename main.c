#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <at/engine.h>
#include <nmea/generator.h>
#include <devices/hci_usb.h>
#include <devices/serial_usb.h>

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
    else transport->input_handle.fd = -1;
  }
  if(transport->output_fd < 0){
    LOG("open %s -> err: %d, msg: %s", transport->tty_path, errno, strerror(errno));
    return false;
  }
  if(transport->input_handle.fd >= 0) io_reg_handle(&transport->input_handle);

  if(transport->slave_path){
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

static void on_hci_input(io_handle_t* handle, uint8_t fd_mode_mask){
  int rc = read(handle->fd, io_rx_buff, sizeof(io_rx_buff));
  if(rc > 0) hci_usb_device_input(handle->usr_data, io_rx_buff, rc);
  else handle_read_error((transport_t*)handle, rc);
}

static int on_hci_output(hci_usb_device_t* hci_dev, uint8_t* data, size_t length){
  transport_t* transport = hci_dev->usr_data;
  return io_write(transport->output_fd, data, length);
}

static void on_gsm_input(io_handle_t* handle, uint8_t fd_mode_mask){
  int rc = read(handle->fd, io_rx_buff, sizeof(io_rx_buff));
  if(rc > 0) serial_usb_device_input(handle->usr_data, io_rx_buff, rc);
  else handle_read_error((transport_t*)handle, rc);
}

static int on_gsm_output(serial_usb_device_t* serial_dev, uint8_t* data, size_t length){
  transport_t* transport = serial_dev->usr_data;
  return io_write(transport->output_fd, data, length);
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
  .talker_mask = {
    [NMEA_SENTENCE(RMC)] = TALKER_MASK(GN),
    [NMEA_SENTENCE(GGA)] = TALKER_MASK(GN),
    [NMEA_SENTENCE(GST)] = TALKER_MASK(GN),
    [NMEA_SENTENCE(QSA)] = TALKER_MASK(QZ),
    [NMEA_SENTENCE(GSA)] = TALKER_MASK(GP) | TALKER_MASK(GL) | TALKER_MASK(GA),
    [NMEA_SENTENCE(GSV)] = TALKER_MASK(GP) | TALKER_MASK(GL) | TALKER_MASK(GA) | TALKER_MASK(QZ),
  },
};

static hci_usb_device_t hci_dev = {};
static serial_usb_device_t gsm_dev = {};

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

static transport_t hci_transport = {
  .input_handle = {
    .fd_mode_mask = FD_READ | FD_EXCEPT,
    .usr_data = &hci_dev, .cb = on_hci_input,
  },
  .name = "bthci_device",
  .tty_path = "/dev/ptmx",
  .output_fd = -1, .baud = 0,
  .symlink_path = "/tmp/tty.bthci",
};

static transport_t gsm_transport = {
  .input_handle = {
    .fd_mode_mask = FD_READ | FD_EXCEPT,
    .usr_data = &gsm_dev, .cb = on_gsm_input,
  },
  .name = "gsm0_device",
  .tty_path = "/dev/ptmx",
  .output_fd = -1, .baud = 0,
  .symlink_path = "/tmp/tty.gsm0",
};

static usb_dev_info_t hci_devices[] = {
  {.vid = 0x2357, .pid = 0x0604}, // UB500
  {.vid = 0x8087, .pid = 0x0029}, // AX200
  {.vid = 0x0a5c, .pid = 0x22be}, // BCM2070
  {.vid = 0x0a12, .pid = 0x0001}, // CSR8510
};

static usb_dev_info_t gsm_devices[] = {
  {.vid = 0x2c7c, .pid = 0x0904}, // EC800G
  {.vid = 0x2c7c, .pid = 0x6026}, // L511
};

static bool is_device_present(usb_dev_info_t *dev_info, usb_dev_info_t* devices, size_t count){
  for(int i = 0; i < count; i++) if(dev_info->vid == devices[i].vid && dev_info->pid == devices[i].pid) return true;
  return false;
}

static void usb_on_device(usb_host_t* host, usb_dev_info_t *dev_info, bool added){
  LOG("%s bus: %u, addr: %u, vid: 0x%04x, pid: 0x%04x, name: %s", added ? "added" : "removed",
    dev_info->bus, dev_info->addr, dev_info->vid, dev_info->pid, dev_info->name);

  if(is_device_present(dev_info, hci_devices, countof(hci_devices))){
    bool rc = false;
    if(added) rc = hci_usb_device_init(&hci_dev, dev_info);
    else if(usb_device_match(&hci_dev.usb_device, dev_info)) rc = hci_usb_device_deinit(&hci_dev);
    if(rc){LOG("hci device %s", added ? "online" : "offline");}
  }
  if(is_device_present(dev_info, gsm_devices, countof(gsm_devices))){
    bool rc = false;
    if(added) rc = serial_usb_device_init(&gsm_dev, dev_info, dev_info->pid == 0x0904 ? 2 : 4);
    else if(usb_device_match(&gsm_dev.usb_device, dev_info)) rc = serial_usb_device_deinit(&gsm_dev);
    if(rc){LOG("serial device %s", added ? "online" : "offline");}
  }
}

static usb_host_t usb_host = {
  .on_device = usb_on_device,
};

static transport_t* transports[] = {&ate_transport, &nmea_transport, &hci_transport, &gsm_transport};

static void parse_args(int argc, char *argv[]){
  bool is_path = false, is_baud = false;
  for(char c; (c = getopt(argc, argv, "pa:ba:pn:bn:ph:bh")) != -1;){
    transport_t* transport = NULL;
    switch (c){
      case 'p': is_path = true; continue;
      case 'b': is_baud = true; continue;
      case 'a': transport = &ate_transport; break;
      case 'h': transport = &hci_transport; break;
      case 'n': transport = &nmea_transport; break;
      case '?': LOG("Unknown option: %c, %s", optopt, optarg); exit(1);
    }
    if(transport){
      if(is_path) transport->tty_path = optarg;
      else if(is_baud) transport->baud = atoi(optarg);
    }
  }
}

int main(int argc, char *argv[]){
  parse_args(argc, argv);

  engine.usr_data = &ate_transport;
  nmea_gen.usr_data = &nmea_transport;

  hci_dev.usr_data = &hci_transport;
  hci_dev.output = on_hci_output;
  hci_dev.usb_device.host = &usb_host;

  gsm_dev.usr_data = &gsm_transport;
  gsm_dev.output = on_gsm_output;
  gsm_dev.usb_device.host = &usb_host;

  io_init_loop();

  for(int i = 0; i < countof(transports); i++) if(!setup_transport(transports[i])) return -1;

  usb_host_init(&usb_host);
  usb_host_start(&usb_host);

  at_engine_init(&engine, ate_output);
  for(int i = 0; i < countof(cmds); i++) at_engine_add_cmd(&engine, &cmds[i]);
  at_engine_start(&engine);
  nmea_gen_start(&nmea_gen, 1000);

  io_run_loop();
  LOG("exitting...");
  at_engine_stop(&engine);
  nmea_gen_stop(&nmea_gen);
  hci_usb_device_deinit(&hci_dev);
  serial_usb_device_deinit(&gsm_dev);
  usb_host_deinit(&usb_host);
  usleep(10*1000);
  for(int i = 0; i < countof(transports); i++) close_transport(transports[i]);
  return 0;
}
