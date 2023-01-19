#include <unistd.h>
#include <ctype.h>
#include <stdarg.h>
#include "../common.h"

#define BYTES_PER_LINE 16
static char hd_buffer[10 + 3 + (BYTES_PER_LINE * 3) + 3 + BYTES_PER_LINE + 1 + 2];

void hexdump(uint8_t* data, size_t length, const char* header, ...){
  if(length == 0) return;

  size_t bytes_per_line = min(BYTES_PER_LINE, length);
  size_t bytes_cur_line = bytes_per_line;
  // size_t bytes_cur_line = length > bytes_per_line ? bytes_per_line : length;
  char* ptr_hd = hd_buffer;
  if(header){
    va_list va;
    va_start(va, header);
    vfprintf(stderr, header, va);
    va_end(va);

    int rc = sprintf(hd_buffer, "%p", data);
    for(size_t i = 0; i < rc; i++) *ptr_hd++ = '-';

    for(size_t i = 0; i < bytes_per_line; i++) {
      if ((i&7)==0 ) ptr_hd += sprintf(ptr_hd, " " );
      if (i < bytes_cur_line) ptr_hd += sprintf(ptr_hd, "  %1zx", i);
      else ptr_hd += sprintf(ptr_hd, "   ");
    }
    ptr_hd += sprintf(ptr_hd, "   ");
    for(size_t i = 0; i < bytes_cur_line; i++) ptr_hd += sprintf(ptr_hd, "%1zx", i);
    ptr_hd += sprintf(ptr_hd, "\r\n");
    write(STDERR_FILENO, hd_buffer, ptr_hd - hd_buffer);
  }
  do {
    ptr_hd = hd_buffer;
    bytes_cur_line = length > bytes_per_line ? bytes_per_line : length;

    ptr_hd += sprintf(ptr_hd, "%p", data);
    for(size_t i = 0; i < bytes_per_line; i++) {
      if ((i&7)==0 ) ptr_hd += sprintf(ptr_hd, " ");
      if (i < bytes_cur_line) ptr_hd += sprintf(ptr_hd, " %02x", data[i]);
      else ptr_hd += sprintf(ptr_hd, "   ");
    }

    ptr_hd += sprintf(ptr_hd, "  |");

    for(size_t i = 0; i < bytes_cur_line; i++){
      if(isprint(data[i])) ptr_hd += sprintf(ptr_hd, "%c", data[i]);
      else ptr_hd += sprintf(ptr_hd, ".");
    }

    ptr_hd += sprintf(ptr_hd, "|\r\n");

    write(STDERR_FILENO, hd_buffer, ptr_hd - hd_buffer);
    data += bytes_cur_line;
    length -= bytes_cur_line;
  } while(length);
  return;
}
