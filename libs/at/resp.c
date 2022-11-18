#include "resp.h"

#define CMD_RESP(x,y) {.txt = #x, .code = y},

static char* resp_types[] = {
  #define REG_CMD_RES_TYPE(x, y, ...) y,
  #include "defs.h"
};

static at_cmd_resp_t DCE_RC_values[] = {
  #define REG_DCE_RC(x, y, ...) CMD_RESP(x, y)
  #include "defs.h"
};

static at_cmd_resp_t CME_ERR_values[] = {
  #define REG_CME_ERR(x, y, ...) CMD_RESP(x, y)
  #include "defs.h"
};

static at_cmd_resp_t CMS_ERR_values[] = {
  #define REG_CME_ERR(x, y, ...) CMD_RESP(x, y)
  #include "defs.h"
};

typedef struct {
  at_cmd_resp_t* resps;
  size_t count;
} resp_info_t;

static resp_info_t resps[] = {
  #define REG_CMD_RES_TYPE(x, ...) {.resps = x##_values, .count = countof(x##_values)},
  #include "defs.h"
};

bool at_cmd_resp(at_cmd_raw_resp_t raw_resp, char** type, at_cmd_resp_t** resp){
  if(raw_resp.type >= CMD_RES(MAX)) return false;
  resp_info_t* resp_values = &resps[raw_resp.type];
  if(raw_resp.value >= resp_values->count) return false;
  if(type) *type = resp_types[raw_resp.type];
  if(resp) *resp = &resp_values->resps[raw_resp.value];
  return true;
}
