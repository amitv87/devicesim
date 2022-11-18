#ifndef AT_CMD_H
#define AT_CMD_H

#include "../common.h"

#define CMD_TYPE(x)   CMD_##x

#define CMD_TYPE_MASK(x) (1 << CMD_TYPE(x))

typedef enum{
  #define REG_CMD_TYPE(x, ...) CMD_TYPE(x),
  #include "defs.h"
  CMD_TYPE(MAX),
} at_cmd_type;

const char* at_cmd_type_txt(at_cmd_type type);

#endif
