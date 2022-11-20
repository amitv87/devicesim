#ifndef AT_ENGINE_H
#define AT_ENGINE_H

#include "param.h"
#include "../cmux/cmux.h"
#include "../util/list.h"
#include "../util/spawn.h"
#include "../util/line_reader.h"

#define USE_PPPD_PTY
#define MAX_CHANNEL_COUNT (4)

#define CMD_TYPE_MASKS(tst, get, set, exe) (tst<<CMD_TYPE(TST))|(get<<CMD_TYPE(GET))|(set<<CMD_TYPE(SET))|(exe<<CMD_TYPE(EXE))

#define AT_CMD(nm, func, tst, get, set, exe) {.name = nm, .cmd_type_mask = CMD_TYPE_MASKS(tst, get, set, exe), .handler = func}

#define AT_CMD_HANDLER_CALL_ARGS ch, cmd, result
#define AT_CMD_HANDLER_ARGS at_channel_t* ch, at_cmd_t* cmd, at_cmd_parse_result_t* result

#define AT_CH_MODE(x) CH_MODE_##x

#define AT_SEP      ": "
#define AT_NEWLINE  "\r\n"

typedef enum{
  #define REG_AT_CH_MODE(x, ...) AT_CH_MODE(x),
  #include "defs.h"
  AT_CH_MODE(MAX),
} at_ch_mode;

typedef struct at_cmd_s at_cmd_t;
typedef struct at_engine_s at_engine_t;
typedef struct at_channel_s at_channel_t;

typedef int (*at_engine_output_tp)(at_engine_t *engine, uint8_t* data, size_t len);
typedef at_cmd_raw_resp_t (*at_cmd_handler)(AT_CMD_HANDLER_ARGS);

typedef struct at_channel_s{
  at_engine_t* engine;
  size_t ch_id;
  bool echo;
  int plus_cnt;
  io_spawn_t pppd;
  #ifdef USE_PPPD_PTY
  io_handle_t pppd_handle;
  #endif
  at_ch_mode mode;
  line_reader_t reader;
  uint8_t __buff[1024];
} at_channel_t;

typedef struct at_engine_s{
  at_cmd_t* cmd_list;
  cmux_t cmux;
  void* usr_data;
  void* modem_state;
  at_engine_output_tp output;
  at_channel_t channels[MAX_CHANNEL_COUNT + 1];
} at_engine_t;

typedef struct at_cmd_s{
  list_item_t item;
  void* priv_data;
  const char* name;
  uint8_t cmd_type_mask;
  at_cmd_handler handler;
} at_cmd_t;

const char* at_ch_mode_txt(at_ch_mode mode);

void at_engine_stop(at_engine_t *engine);
void at_engine_start(at_engine_t *engine);
void at_engine_init(at_engine_t *engine, at_engine_output_tp output);
void at_engine_add_cmd(at_engine_t *engine, at_cmd_t* cmd);
void at_engine_rem_cmd(at_engine_t *engine, at_cmd_t* cmd);
void at_engine_input(at_engine_t *engine, size_t ch_id, uint8_t* data, size_t len);
void at_engine_output(at_engine_t *engine, size_t ch_id, uint8_t* data, size_t len);

bool at_engine_start_ppp_server(at_channel_t* ch, uint8_t ctx_id);

#define AT_OUTPUT_ARGS(ch, ...) {char* argv[] = {__VA_ARGS__}; at_output_args(ch, countof(argv), argv);}
#define AT_OUTPUT_ARGS_LINE(ch, ...) AT_OUTPUT_ARGS(ch, __VA_ARGS__ , AT_NEWLINE)

static inline void at_output_newline(at_channel_t* ch){
  return at_engine_output(ch->engine, ch->ch_id,  (uint8_t*)AT_NEWLINE, sizeof(AT_NEWLINE) - 1);
}

static inline void at_output_args(at_channel_t* ch, size_t argc, char* argv[]){
  for(size_t i = 0; i < argc; i++) at_engine_output(ch->engine, ch->ch_id,  (uint8_t*)argv[i], strlen(argv[i]));
}

#endif
