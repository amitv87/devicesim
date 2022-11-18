#include <string.h>

#include "param.h"
// #include "split_argv.h"

typedef struct{
  at_cmd_type type;
  const char* pattern;
  bool terminating;
} type_info_t;

static type_info_t type_infos[] = {
  #define REG_CMD_TYPE(x, y, z) {.type = CMD_TYPE(x), .pattern = y, .terminating = z},
  #include "defs.h"
};

#define SS_FLAG_ESCAPE 0x8

typedef enum {
    /* parsing the space between arguments */
    SS_DELIM = 0x0,
    /* parsing an argument which isn't quoted */
    SS_ARG = 0x1,
    /* parsing a quoted argument */
    SS_QUOTED_ARG = 0x2,
    /* parsing an escape sequence within unquoted argument */
    SS_ARG_ESCAPED = SS_ARG | SS_FLAG_ESCAPE,
    /* parsing an escape sequence within a quoted argument */
    SS_QUOTED_ARG_ESCAPED = SS_QUOTED_ARG | SS_FLAG_ESCAPE,
} split_state_t;

/* helper macro, called when done with an argument */
#define END_ARG() do { \
    char_out = 0; \
    if(args->argv) args->argv[argc++].value = next_arg_start; else argc += 1; \
    state = SS_DELIM; \
} while(0)

size_t at_split_args(char *line, char delimiter, at_cmd_args_t* args){
  char QUOTE = '"';
  char ESCAPE = '\\';

  split_state_t state = SS_DELIM;
  size_t argc = 0;
  char *next_arg_start = line;
  char *out_ptr = line;
  for (char *in_ptr = line; argc < args->argc; ++in_ptr) {
      int char_in = (unsigned char) *in_ptr;
      if (char_in == 0) {
          break;
      }
      int char_out = -1;

      switch (state) {
      case SS_DELIM:
          if (char_in == delimiter) {
              /* skip space */
          } else if (char_in == QUOTE) {
              next_arg_start = out_ptr;
              state = SS_QUOTED_ARG;
          } else if (char_in == ESCAPE) {
              next_arg_start = out_ptr;
              state = SS_ARG_ESCAPED;
          } else {
              next_arg_start = out_ptr;
              state = SS_ARG;
              char_out = char_in;
          }
          break;

      case SS_QUOTED_ARG:
          if (char_in == QUOTE) {
              END_ARG();
          } else if (char_in == ESCAPE) {
              state = SS_QUOTED_ARG_ESCAPED;
          } else {
              char_out = char_in;
          }
          break;

      case SS_ARG_ESCAPED:
      case SS_QUOTED_ARG_ESCAPED:
          if (char_in == ESCAPE || char_in == QUOTE || char_in == delimiter) {
              char_out = char_in;
          } else {
              /* unrecognized escape character, skip */
          }
          state = (split_state_t) (state & (~SS_FLAG_ESCAPE));
          break;

      case SS_ARG:
          if (char_in == delimiter) {
              END_ARG();
          } else if (char_in == ESCAPE) {
              state = SS_ARG_ESCAPED;
          } else {
              char_out = char_in;
          }
          break;
      }
      /* need to output anything? */
      if (char_out >= 0) {
          *out_ptr = char_out;
          ++out_ptr;
      }
  }
  /* make sure the final argument is terminated */
  *out_ptr = 0;
  /* finalize the last argument */
  if (state != SS_DELIM && argc < args->argc) {
      if(args->argv) args->argv[argc++].value = next_arg_start; else argc += 1;
  }
  /* add a NULL at the end of argv */
  return argc;
}

bool at_cmd_parse(char* line, size_t len, at_cmd_parse_result_t* result){
  bool rc = false;
  if(!len) return rc;
  size_t read_pos = 0;

  char cmd_firstchar = line[0];

  if(len >= 1 && cmd_firstchar != '+' && cmd_firstchar != '#'){
    rc = true;
    line[-1] = cmd_firstchar;
    result->cmd = line - 1;
    line[0] = 0;
    result->type = CMD_TYPE(EXE);
    result->args.argc = len >= 2 ? 1 : 0;
    result->args.argv[0].value = line + 1;
    return rc;
  }

  for(size_t i = 0; i < countof(type_infos); i++){
    type_info_t* info = &type_infos[i];
    size_t pattern_len = info->pattern ? strlen(info->pattern) : 0;


    if(pattern_len > len) continue;

    if(!pattern_len){
      read_pos = len;
      goto found;
    }

    char* beg_match = strstr(line, info->pattern);

    // LOG("info->pattern: %s, beg_match: %s", info->pattern, beg_match);

    if(beg_match){
      if(info->terminating){
        if(strcmp(line + (len - pattern_len), info->pattern) == 0){
          read_pos = len;
          goto found;
        }
      }
      else{
        read_pos = (beg_match - line) + pattern_len;
        goto found;
      }
    }

    continue;

    found:
    if(result){
      line[read_pos - pattern_len] = 0;
      result->cmd = line;
      result->type = info->type;
    }
    break;
  }

  rc = read_pos > 0;
  result->args.argc = rc && len ? at_split_args(line + read_pos, ',', &result->args) : 0;
  return rc;
}
