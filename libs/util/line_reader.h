#ifndef PICO_LINE_READER_H
#define PICO_LINE_READER_H

#include "../common.h"

typedef struct line_reader_s line_reader_t;

typedef void (*line_reader_on_data)(line_reader_t *reader, char* data, size_t length);

typedef struct line_reader_s{
  void* usr_data;
  io_buff_t buff;
  bool is_escaped;
  line_reader_on_data on_data;
} line_reader_t;

void line_reader_reset(line_reader_t *reader);
void line_reader_read(line_reader_t *reader, uint8_t* data, size_t length);

#endif
