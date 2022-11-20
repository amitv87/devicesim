#include "engine.h"
#include <string.h>
#include <unistd.h>

static const char* ch_modes[] = {
  #define REG_AT_CH_MODE(x, ...) #x,
  #include "defs.h"
};

const char* at_ch_mode_txt(at_ch_mode mode){
  if(mode < countof(ch_modes)) return ch_modes[mode];
  return "UNK";
}

static void reset_channel(at_channel_t* ch, at_ch_mode mode){
  if(mode == AT_CH_MODE(CLOSED)) ch->mode = mode;

  if(ch->pppd.usr_data == ch){
    ch->pppd.usr_data = NULL;
    io_spawn_wait(&ch->pppd, true);
    #ifdef USE_PPPD_PTY
    io_dereg_handle(&ch->pppd_handle);
    close(ch->pppd_handle.fd);
    ch->pppd_handle = (io_handle_t){.fd = -1};
    #endif
  }

  if(ch->mode == AT_CH_MODE(DATA)){
    at_cmd_resp_t *resp = NULL;
    at_cmd_raw_resp_t res = {.type = CMD_RES(DCE_RC), .value = DCE_RC(NOCARRIER)};
    at_cmd_resp(res, NULL, &resp);
    AT_OUTPUT_ARGS_LINE(ch, AT_NEWLINE, resp->txt)
  }

  ch->mode = mode;
  line_reader_reset(&ch->reader);
}

static void cmux_on_evt(cmux_t *cmux, size_t ch_id, cmux_event_type event){
  at_engine_t *engine = cmux->usr_data;
  LOG("ch%zu -> %s", ch_id, event ? "closed" : "open");
  if(ch_id < countof(engine->channels)){
    at_channel_t* ch = &engine->channels[ch_id];
    at_ch_mode mode = ch_id ? (event == CMUX_CHANNEL_OPEN ? AT_CH_MODE(CMD) : AT_CH_MODE(CLOSED)) :
      (ch->mode = event == CMUX_CHANNEL_OPEN ? AT_CH_MODE(CMUX) : AT_CH_MODE(CMD));
    reset_channel(ch, mode);

    if(ch_id == 0) for(size_t i = 1; i < countof(ch->engine->channels); i++) reset_channel(&ch->engine->channels[i], AT_CH_MODE(CLOSED));
  }
}

static void cmux_tp_out(cmux_t *cmux, uint8_t* data, size_t len){
  at_engine_t *engine = cmux->usr_data;
  engine->output(engine,  data, len);
}

static void cmux_ch_out(cmux_t *cmux, size_t ch_id, uint8_t* data, size_t len){
  // LOG("ch%zu -> %.*s", ch_id, (int)len, data);
  at_engine_input(cmux->usr_data, ch_id, data, len);
}

static cmux_cb_t cmux_cb = {
  .on_event = cmux_on_evt,
  .tp_output = cmux_tp_out,
  .ch_output = cmux_ch_out,
};

static void on_cmd_line(line_reader_t *reader, char* data, size_t length){
  if(length < 2 || strncasecmp("AT", data, 2) != 0) return;
  data += 2, length -= 2;

  char* type = NULL;
  at_cmd_resp_t *resp = NULL;
  at_channel_t* ch = reader->usr_data;

  at_cmd_paramt_t argv[10] = {0};
  at_cmd_parse_result_t result = {.args = {.argc = sizeof(argv), .argv = argv}};

  at_engine_output(ch->engine, ch->ch_id, (uint8_t*)AT_NEWLINE, sizeof(AT_NEWLINE) - 1);

  if(!length){
    at_cmd_raw_resp_t res = {.type = CMD_RES(DCE_RC), .value = DCE_RC(OK)};
    at_cmd_resp(res, &type, &resp);
    AT_OUTPUT_ARGS_LINE(ch, resp->txt)
    return;
  }
  else{
    at_cmd_paramt_t argv[10] = {0};
    at_cmd_args_t cmds = {.argc = 10};
    size_t num_cmds = at_split_args(data, ';', &cmds);
    int len = length;
    // AT+CFUN?;+CBC;+CREG?;+CGREG?;+CSQ;+CCLK?;I
    // LOG("num_cmds: %zu, data: %s, len: %d", num_cmds, data, len);
    while(num_cmds && len > 0){
      // LOG("\t%zu -> %s", num_cmds, data);

      size_t cmd_len = strlen(data);
      at_cmd_parse_result_t result = {.args = {.argc = sizeof(argv), .argv = argv}};
      if(at_cmd_parse(data, cmd_len, &result)){

        // LOG("ch%zu cmd: %s, type: %s, argc: %zu", ch->ch_id, result.cmd, at_cmd_type_txt(result.type), result.args.argc);
        // for(size_t j = 0; j < result.args.argc; j++) LOG("%3zu -> %s", j, result.args.argv[j].value);

        at_cmd_t* cmd = ch->engine->cmd_list;
        while(cmd){
          if(strcasecmp(cmd->name, result.cmd) == 0){
            at_cmd_raw_resp_t res;
            if(cmd->cmd_type_mask & (1 << result.type)){
              if(result.args.argc == 0 && result.type == CMD_TYPE(SET)) res = (at_cmd_raw_resp_t){.type = CMD_RES(CME_ERR), .value = CME_ERR(PARAM_INVALID)};
              else res = cmd->handler(ch, cmd, &result);
            }
            else res = (at_cmd_raw_resp_t){
              .type = CMD_RES(CME_ERR),
              .value = CME_ERR(OPERATION_NOT_SUPPORTED),
            };

            at_cmd_resp(res, &type, &resp);

            if(res.type == CMD_RES(DCE_RC)) AT_OUTPUT_ARGS_LINE(ch, resp->txt)
            else{
              char code[7] = {0};
              snprintf(code, sizeof(code) - 1, "%u", resp->code);
              AT_OUTPUT_ARGS_LINE(ch, "+", type, ": ", code)
            }

            if(ch->engine->cmux.state == CMUX_STATE_PRE_INIT){
              cmux_t* cmux = &ch->engine->cmux;
              *cmux = (cmux_t){
                .usr_data = ch->engine,
                .cb = &cmux_cb,
              };
              cmux_init(cmux);
              ch->mode = AT_CH_MODE(CMUX);
            }
            goto do_next;
          }
          cmd = cmd->item.next;
        }

        /*{
          at_cmd_raw_resp_t res = {.type = CMD_RES(CME_ERR), .value = CME_ERR(OPERATION_NOT_SUPPORTED)};
          at_cmd_resp(res, &type, &resp);
          char code[7] = {0};
          snprintf(code, sizeof(code) - 1, "%u", resp->code);
          AT_OUTPUT_ARGS_LINE(ch, "+", type, ": ", code)
          return;
        }*/

        at_cmd_raw_resp_t res = {.type = CMD_RES(DCE_RC), .value = DCE_RC(ERROR)};
        at_cmd_resp(res, &type, &resp);
        AT_OUTPUT_ARGS_LINE(ch, resp->txt)
      }

      do_next:
      cmd_len += 1;
      num_cmds -= 1;
      data += cmd_len, len -= cmd_len;
    }
  }
}

void at_engine_stop(at_engine_t *engine){
  for(size_t i = 0; i < countof(engine->channels); i++) reset_channel(&engine->channels[i], AT_CH_MODE(CLOSED));
}

void at_engine_start(at_engine_t *engine){
  at_engine_stop(engine);
  at_channel_t* ch = &engine->channels[0];
  ch->mode = AT_CH_MODE(CMD);
  AT_OUTPUT_ARGS(ch, AT_NEWLINE "RDY" AT_NEWLINE)
}

void at_engine_init(at_engine_t *engine, at_engine_output_tp output){
  engine->output = output;
  cmux_init(&engine->cmux);
  for(size_t i = 0; i < countof(engine->channels); i++){
    at_channel_t* ch = &engine->channels[i];
    *ch = (at_channel_t){0};
    ch->ch_id = i;
    ch->echo = true;
    ch->mode = AT_CH_MODE(CLOSED);
    ch->engine = engine;
    ch->reader = (line_reader_t){
      .usr_data = ch,
      .buff = {.bytes = ch->__buff, .len = sizeof(ch->__buff)},
      .on_data = on_cmd_line,
    };
    #ifdef USE_PPPD_PTY
    ch->pppd_handle = (io_handle_t){.fd = -1};
    #endif
    line_reader_reset(&ch->reader);
  }
}

void at_engine_add_cmd(at_engine_t *engine, at_cmd_t* cmd){
  LIST_ADD(&engine->cmd_list, cmd);
}

void at_engine_rem_cmd(at_engine_t *engine, at_cmd_t* cmd){
  LIST_REM(&engine->cmd_list, cmd);
}

void at_engine_input(at_engine_t *engine, size_t ch_id, uint8_t* data, size_t len){
  // LOG("ch%zu -> %.*s", ch_id, (int)len, data);
  at_channel_t* ch = &engine->channels[ch_id];
  switch(ch->mode){
    case AT_CH_MODE(CMD): if(ch->echo) at_engine_output(engine, ch_id, data, len); line_reader_read(&ch->reader, data, len); break;
    case AT_CH_MODE(CMUX): cmux_tp_input(&engine->cmux, data, len); break;
    case AT_CH_MODE(DATA): {
      if(len == 1 && data[0] == '+' && (ch->plus_cnt += 1) && ch->plus_cnt >= 3){
        reset_channel(ch, AT_CH_MODE(CMD));
        break;
      }
      #ifdef USE_PPPD_PTY
      write(ch->pppd_handle.fd, data, len);
      #else
      io_spawn_input(&ch->pppd, data, len);
      #endif
      break;
    }
  }
}

void at_engine_output(at_engine_t *engine, size_t ch_id, uint8_t* data, size_t len){
  // LOG("ch%zu -> %.*s", ch_id, (int)len, data);
  at_channel_t* ch = &engine->channels[ch_id];
  if(ch->mode == AT_CH_MODE(CLOSED)) return;
  if(ch_id > 0 || ch->mode == AT_CH_MODE(CMUX)) cmux_ch_input(&engine->cmux, ch_id,  data, len);
  else engine->output(engine,  data, len);
}

static void pppd_on_state(io_spawn_t* spawn, io_spawn_state state, void* info){
  LOG("state: %d, info: %d", state, *(int*)info);
  at_channel_t* ch = spawn->usr_data;
  if(!ch) return;
  if(state == SPAWN_STOPPED) reset_channel(ch, AT_CH_MODE(CMD));
}

static void pppd_on_activity(io_spawn_t* pppd, io_std_type type, uint8_t* data, size_t size){
  at_channel_t* ch = pppd->usr_data;
  if(!ch) return;
  #ifdef USE_PPPD_PTY
  data[size] = 0;
  char* str = NULL;
  if((str = strstr((char*)data, "local  IP address")) || (str = strstr((char*)data, "remote IP address"))) LOG("%s", str);
  #else
  if(type == IO_STDOUT && ch->mode == AT_CH_MODE(DATA))
  at_engine_output(ch->engine, ch->ch_id, data, size);
  else LOG("type: %d, %.*s", type, (int)size, data);
  #endif
}

static void on_pppd_in(io_handle_t* handle, uint8_t fd_mode_mask){
  // LOG("fd_mode_mask: 0x%0x", fd_mode_mask);
  at_channel_t* ch = handle->usr_data;
  if(fd_mode_mask & FD_READ){
    int rc = read(handle->fd, io_rx_buff, sizeof(io_rx_buff) - 2);
    if(!rc) goto except;
    else if(ch->mode == AT_CH_MODE(DATA)) at_engine_output(ch->engine, ch->ch_id, io_rx_buff, rc);
  }
  if(fd_mode_mask & FD_EXCEPT) except: io_spawn_wait(&ch->pppd, false);
}

static io_spawn_cb_t pppd_cb = {
  .on_state = pppd_on_state,
  .on_activity = pppd_on_activity,
};

bool at_engine_start_ppp_server(at_channel_t* ch, uint8_t ctx_id){
  bool rc = false;
  if(ch->mode != AT_CH_MODE(CMD)) return rc;

  #ifdef USE_PPPD_PTY
  char* pppd_tty_path = NULL;
  int fd = io_tty_open("/dev/ptmx", 0, &pppd_tty_path);
  if(fd < 0 || !pppd_tty_path) return rc;
  LOG("using: %s", pppd_tty_path);
  #endif

  char* argv[] = {
    "pppd",
    #ifdef USE_PPPD_PTY
    pppd_tty_path,
    #else
    "notty", // pppd fails with stdin over pipe hence using notty (isatty check on stdin fails wihtout this)
    #endif
    // "debug",
    // "persist",
    // "passive",
    // "record", "ppp_dump.pcap",
    "nodefaultroute",
    "local",
    "nodetach",
    "noccp",
    "novj",
    "nocrtscts",
    "noauth",
    "asyncmap",
    "0",
    "ipcp-accept-local",
    "ipcp-accept-remote",
    "netmask",
    "255.255.255.0",
    "ms-dns",
    "8.8.8.8",
    "172.10.10.1:172.10.10.2",
    0
  };

  ch->pppd.argc = countof(argv), ch->pppd.argv = argv;
  ch->pppd.cb = &pppd_cb;

  rc = io_spawn_start(&ch->pppd); LOG("io_spawn_start -> %d", rc);

  if(rc){
    ch->plus_cnt = 0;
    ch->mode = AT_CH_MODE(DATA);

    ch->pppd.usr_data = ch;

    #ifdef USE_PPPD_PTY
    ch->pppd_handle = (io_handle_t){
      .cb = on_pppd_in,
      .usr_data = ch,
      .fd = fd,
      .fd_mode_mask = FD_READ | FD_EXCEPT,
    };

    io_reg_handle(&ch->pppd_handle);
    #endif
  }
  else{
    #ifdef USE_PPPD_PTY
    close(fd);
    #endif
  }

  return rc;
}
