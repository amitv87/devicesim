#include "cmd.h"

static const char* cmd_types[] = {
  #define REG_CMD_TYPE(x, ...) #x,
  #include "defs.h"
};

const char* at_cmd_type_txt(at_cmd_type type){
  if(type < countof(cmd_types)) return cmd_types[type];
  return "UNK";
}
