#ifndef AT_PARSE_H
#define AT_PARSE_H

#include "cmd.h"
#include "resp.h"

typedef enum{
  AT_CMD_PARAM_TYPE_EMPTY,
  AT_CMD_PARAM_TYPE_RAW,
  AT_CMD_PARAM_TYPE_PARSED_STRING,
  AT_CMD_PARAM_TYPE_PARSED_DTMF,
} at_cmd_param_type;

typedef struct{
  uint8_t type;    // parameter type, used by AT engine internally
  uint16_t length; // value length
  char* value;     // value, the real size is variable
} at_cmd_paramt_t;

typedef struct{
  size_t argc;
  at_cmd_paramt_t* argv;
} at_cmd_args_t;

typedef struct{
  at_cmd_type type;
  char* cmd;
  at_cmd_args_t args;
} at_cmd_parse_result_t;

size_t at_split_args(char *line, char delimiter, at_cmd_args_t* args);
bool at_cmd_parse(char* line, size_t len, at_cmd_parse_result_t* result);

#endif
