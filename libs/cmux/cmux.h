#ifndef AT_CMUX_H
#define AT_CMUX_H

#include "../common.h"

#define CMUX_BUFF_SIZE (1600) // 1(SOF) + 3(frame hdr) + (0-127)(data) + 1(FCS) + 1(SOF)
#define MAX_CHANNEL_COUNT (4)

typedef enum{
  CMUX_STATE_UNK,
  CMUX_STATE_CLOSED,
  CMUX_STATE_PRE_INIT,
  CMUX_STATE_INIT,
  CMUX_STATE_OPEN,
} cmux_state;

typedef enum{
  CMUX_CHANNEL_OPEN,
  CMUX_CHANNEL_CLOSE,
} cmux_event_type;

typedef struct cmux_s cmux_t;

typedef void (*cmux_on_event)(cmux_t *cmux, size_t ch_id, cmux_event_type event);
typedef void (*cmux_tp_output)(cmux_t *cmux, uint8_t* data, size_t length);
typedef void (*cmux_ch_output)(cmux_t *cmux, size_t ch_id, uint8_t* data, size_t length);

typedef struct{
  cmux_on_event on_event;
  cmux_tp_output tp_output;
  cmux_ch_output ch_output;
} cmux_cb_t;

typedef struct cmux_s{
  cmux_state state;
  cmux_cb_t* cb;
  void* usr_data;
  size_t max_frame_size;
  bool is_recving_frame;
  uint8_t channel_state[MAX_CHANNEL_COUNT + 1];
  io_buff_t frame_buff;
  uint8_t buff[CMUX_BUFF_SIZE];
} cmux_t;

void cmux_init(cmux_t *cmux);
void cmux_tp_input(cmux_t *cmux, uint8_t* data, size_t length);
void cmux_ch_input(cmux_t *cmux, size_t ch_id, uint8_t* data, size_t length);

#endif
