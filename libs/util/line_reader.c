#include "line_reader.h"

#define ESCAPE_CHAR '\\'

void line_reader_reset(line_reader_t *reader){
  reader->buff.w_idx = 0;
  reader->is_escaped = false;
}

void line_reader_read(line_reader_t *reader, uint8_t* data, size_t length){
  uint8_t* buff = reader->buff.bytes;
  size_t curr_idx = reader->buff.w_idx;
  for(size_t i = 0; i < length; i++){
    char c = data[i];
    if(!reader->is_escaped){
      if(c == ESCAPE_CHAR) reader->is_escaped = true;
      else if(c == '\n' || c == '\r'){
        if(curr_idx > 0){
          on_line:
          buff[curr_idx] = 0;
          reader->on_data(reader, (char*)buff, curr_idx);
          curr_idx = 0;
        }
      }
      else goto copy;
    }
    else{
      copy:
      if(reader->is_escaped && (c != '\n' && c != '\r')) buff[curr_idx++] = ESCAPE_CHAR;
      buff[curr_idx++] = c;
      reader->is_escaped = false;
      if(curr_idx >= reader->buff.len - 3) goto on_line;
    }
  }
  reader->buff.w_idx = curr_idx;
}
