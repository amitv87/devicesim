#ifndef AT_RESP_H
#define AT_RESP_H

#include "../common.h"

#define CMD_RES(x) CMD_RES_##x
#define DCE_RC(x)   DCE_RC_##x
#define CME_ERR(x)  CME_ERR_##x
#define CMS_ERR(x)  CMS_ERR_##x

typedef enum{
  #define REG_CMD_RES_TYPE(x, ...) CMD_RES(x),
  #include "defs.h"
  CMD_RES(MAX),
} at_cmd_res_type;

typedef enum{
  #define REG_DCE_RC(x, ...) DCE_RC(x),
  #include "defs.h"
  DCE_RC(MAX),
} at_cmd_res_dce_rc;

typedef enum{
  #define REG_CME_ERR(x, ...) CME_ERR(x),
  #include "defs.h"
  CME_ERR(MAX),
} at_cmd_res_cme_err;

typedef enum{
  #define REG_CMS_ERR(x, ...) CMS_ERR(x),
  #include "defs.h"
  CMS_ERR(MAX),
} at_cmd_res_cms_err;

typedef struct{
  uint8_t type;
  uint8_t value;
} at_cmd_raw_resp_t;

typedef struct{
  char* txt;
  uint16_t code;
} at_cmd_resp_t;

bool at_cmd_resp(at_cmd_raw_resp_t raw_resp, char** type, at_cmd_resp_t** resp);

#endif
