typedef union{
  struct{
    uint8_t nib2 : 4;
    uint8_t nib1 : 4;
  } __PACKED__;
  uint8_t b[4];
  uint32_t val;
} __PACKED__ ip_addr_v4_t;

typedef struct{
  ip_addr_v4_t addr;
  char typ[10];
  char apn[32];
} pdp_context_t;

typedef struct{
  uint16_t cbc;
  uint8_t csq;
  uint8_t cfun;
  uint8_t creg;
  uint8_t cgreg;
  uint8_t cgatt;
  uint8_t eng;
  uint8_t cpin;
  uint8_t ctzu;

  char* spn;
  uint64_t imei;
  uint64_t ccid;

  pdp_context_t pdp_ctx[3];
} modem_state_t;

static char tbuff[128] = {0};

static modem_state_t mdm_state = {
  .cbc = 3980,
  .csq = 23,
  .cfun = 1,
  .creg = 1,
  .cgreg = 1,
  .cgatt = 0,
  .eng = 0,
  .cpin = 1,
  .ctzu = 1,
  .spn = "airtel",
  .imei = 866897058905912,
  .ccid = 8991922204034601042,
  .pdp_ctx = {
    {.typ = "IP", .apn = "internet1"},
    {.typ = "IPV4", .apn = "internet2"},
    {.typ = "IPV6", .apn = "internet3"},
  }
};

#define PRINT_BUFF(fmt, ...) {int rc = snprintf(tbuff, sizeof(tbuff) - 1, fmt,  ##__VA_ARGS__); tbuff[rc] = 0;}

#define HANDLER_FUNC(cmd) cmd##_handler
#define FUNC_DECL(cmd) at_cmd_raw_resp_t HANDLER_FUNC(cmd)(AT_CMD_HANDLER_ARGS)

#define REG_CMD(cmd, ...) FUNC_DECL(cmd);
#include "at_cmd_defs.h"

#define RET_DCE(x) return (at_cmd_raw_resp_t){.type = CMD_RES(DCE_RC), .value = DCE_RC(x)}

#define FUNC_IMPL(cmd, ...) FUNC_DECL(cmd){__VA_ARGS__; RET_DCE(OK);}

#define QUOTE_STRING(x) "\"", x, "\""

#define IF_REQ(x) if(result->type == CMD_TYPE(x))

#define IF_TST_REQ() if(result->type == CMD_TYPE(TST))
#define IF_GET_REQ() if(result->type == CMD_TYPE(GET))
#define IF_SET_REQ() if(result->type == CMD_TYPE(SET))
#define IF_EXE_REQ() if(result->type == CMD_TYPE(EXE))

FUNC_IMPL(A)

FUNC_IMPL(D,{
  if(strcmp("*99#", result->args.argv[0].value) == 0 && at_engine_start_ppp_server(ch, 1)) RET_DCE(CONNECT);
  else RET_DCE(NOCARRIER);
})

FUNC_IMPL(E, ch->echo = result->args.argc ? (atoi(result->args.argv[0].value) > 0) : true;)

FUNC_IMPL(H)

FUNC_IMPL(I, AT_OUTPUT_ARGS(ch, "GSM_SIMULATOR" AT_NEWLINE "Revision: 1" AT_NEWLINE))

FUNC_IMPL(CBC,{
  PRINT_BUFF("%u", mdm_state.cbc)
  AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, "0,100,", tbuff)
})

FUNC_IMPL(CGSN,{
  PRINT_BUFF("%llu", mdm_state.imei)
  AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, tbuff)
})

FUNC_IMPL(CFUN,{
  IF_REQ(GET){
    PRINT_BUFF("%u", mdm_state.cfun)
    AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, tbuff)
  }
  else IF_REQ(SET){
    mdm_state.cfun = atoi(result->args.argv[0].value);
    switch(mdm_state.cfun){
      case 0: mdm_state.cpin = mdm_state.creg = mdm_state.cgreg = mdm_state.cgatt = 0; break;
      case 1: mdm_state.cpin = mdm_state.creg = mdm_state.cgreg = mdm_state.cgatt = 1; break;
      case 4: mdm_state.cpin = 1, mdm_state.creg = mdm_state.cgreg = mdm_state.cgatt = 0; break;
    }
  }
})

FUNC_IMPL(CPIN,{
  IF_REQ(GET){
    AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, mdm_state.cpin ? "READY" : "NOT READY")
  }
})

FUNC_IMPL(CSPN, AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, QUOTE_STRING((mdm_state.cpin ? mdm_state.spn : (char*)"")), ",0"))

FUNC_IMPL(CCID,{
  PRINT_BUFF("%llu", mdm_state.cpin ? mdm_state.ccid : 0)
  AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, tbuff)
})

FUNC_IMPL(CENG,{
  IF_REQ(GET){
    PRINT_BUFF("%u", mdm_state.eng)
    AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, tbuff, ",0")
  }
  else IF_REQ(SET) mdm_state.eng = atoi(result->args.argv[0].value);
})

FUNC_IMPL(CTZU,{
  IF_REQ(GET){
    PRINT_BUFF("%u", mdm_state.ctzu)
    AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, tbuff)
  }
  else IF_REQ(SET) mdm_state.ctzu = atoi(result->args.argv[0].value);
})

FUNC_IMPL(CREG,{
  IF_REQ(GET){
    PRINT_BUFF("%u", mdm_state.creg)
    AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, "0,", tbuff)
  }
})
FUNC_IMPL(CGREG,{
  IF_REQ(GET){
    PRINT_BUFF("%u", mdm_state.cgreg)
    AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, "0,", tbuff)
  }
})

FUNC_IMPL(CSQ,{
  PRINT_BUFF("%u", mdm_state.csq)
  AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, tbuff)
})

FUNC_IMPL(CCLK,{
  IF_REQ(GET){// +CCLK: "22/06/10,17:26:37+22"
    struct tm tm;
    time_t now = clock_now()/1000;
    localtime_r(&now, &tm);
    PRINT_BUFF("%02d/%02d/%02d,%02d:%02d:%02d%+03d", tm.tm_year - 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, 0);
    AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, QUOTE_STRING(tbuff))
  }

})

FUNC_IMPL(CMUX,{
  IF_REQ(GET){
    AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, "0")
  }
  else IF_REQ(SET){
    if(ch->ch_id == 0) ch->engine->cmux.state = CMUX_STATE_PRE_INIT;
  }
})

FUNC_IMPL(CLAC,{
  cmd = ch->engine->cmd_list;
  while(cmd){
    AT_OUTPUT_ARGS_LINE(ch, "AT", (char*)cmd->name)
    cmd = cmd->item.next;
  }
})

FUNC_IMPL(QGSN, return HANDLER_FUNC(CGSN)(AT_CMD_HANDLER_CALL_ARGS);)
FUNC_IMPL(QSPN, return HANDLER_FUNC(CSPN)(AT_CMD_HANDLER_CALL_ARGS);)
FUNC_IMPL(QENG, return HANDLER_FUNC(CENG)(AT_CMD_HANDLER_CALL_ARGS);)
FUNC_IMPL(QCCID, return HANDLER_FUNC(CCID)(AT_CMD_HANDLER_CALL_ARGS);)

FUNC_IMPL(CGATT,{
  IF_REQ(GET){
    PRINT_BUFF("%u", mdm_state.cgatt)
    AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, tbuff)
  }
  else IF_REQ(SET) mdm_state.cgatt = atoi(result->args.argv[0].value) > 0 ? 1 : 0;
})

FUNC_IMPL(CGDCONT,{
  // +CGDCONT: 1,"IP","CMNET"
  IF_REQ(GET){
    for(size_t i = 0; i < countof(mdm_state.pdp_ctx); i++){
      char id[2] = {'0' + i+1, 0};
      pdp_context_t* ctx = &mdm_state.pdp_ctx[i];
      AT_OUTPUT_ARGS_LINE(ch, (char*)cmd->name, AT_SEP, id, ",", QUOTE_STRING(ctx->typ), ",", QUOTE_STRING(ctx->apn))
    }
  }
  else IF_REQ(SET){
    if(result->args.argc >= 3){
      size_t idx = atoi(result->args.argv[0].value) - 1;
      if(idx < countof(mdm_state.pdp_ctx)){
        pdp_context_t* ctx = &mdm_state.pdp_ctx[idx];
        strncpy(ctx->typ, result->args.argv[1].value, sizeof(ctx->typ) - 1);
        strncpy(ctx->apn, result->args.argv[2].value, sizeof(ctx->apn) - 1);
      }
    }
  }
})

static at_cmd_t cmds[] = {
  #define REG_CMD(cmd, pfx, ...) AT_CMD(pfx #cmd, HANDLER_FUNC(cmd), __VA_ARGS__),
  #include "at_cmd_defs.h"
};
