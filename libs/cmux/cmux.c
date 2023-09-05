#include "cmux.h"
#include "../util/crc.h"

#define PF 0x10
#define SOF_MARKER 0xF9

typedef enum{
  FT_RR     = 0x01, // Receive Ready
  FT_UI     = 0x03, // Unnumbered Information
  FT_RNR    = 0x05, // Receive Not Ready
  FT_REJ    = 0x09, // Reject
  FT_DM     = 0x0F, // Disconnected Mode
  FT_SABM   = 0x2F, // Set Asynchronous Balanced Mode
  FT_DISC   = 0x43, // Disconnect
  FT_UA     = 0x63, // Unnumbered Acknowledgement
  FT_UIH    = 0xEF, // Unnumbered Information with Header check

  FT_OC     = 0x3F,
  FT_CC     = 0x53,
} cmux_frame_type;

typedef enum{
  CMD_NSC   = 0x04, //Non Supported Command Response
  CMD_TST   = 0x08, //Test Command
  CMD_PSC   = 0x10, //Power Saving Control
  CMD_FCOFF = 0x18, //Flow Control Off Command
  CMD_FCON  = 0x28, //Flow Control On Command
  CMD_CLD   = 0x30, //Multiplexer close down
  CMD_MSC   = 0x38, //Modem Status Command
} cmux_command;

typedef enum{
  CHANNEL_STATE_UNK,
  CHANNEL_STATE_OPEN,
  CHANNEL_STATE_CLOSE,
} cmux_channel_state;

typedef union{
  uint8_t address;
  struct{
    uint8_t ea    : 1;
    uint8_t cr    : 1;
    uint8_t dlci  : 6;
  };
} __PACKED__ cmux_address_t;

typedef struct{
  uint8_t  l_ea   : 1;
  uint16_t length :15;
  uint8_t  data[0];
} __PACKED__ cmux_ext_ld_t;
ASSERT_STRUCT_SIZE(cmux_ext_ld_t, 2);

typedef struct{
  union{
    uint8_t address;
    struct{
      uint8_t f_ea  : 1;
      uint8_t cr    : 1;
      uint8_t dlci  : 6;
    };
  };
  uint8_t control;
  union{
    struct{
      union{
        struct{
          uint8_t l_ea    : 1;
          uint8_t length  : 7;
        };
        uint8_t __length;
      };
      union{
        uint8_t data[0];
      };
    };
    cmux_ext_ld_t ext[0];
  };
} __PACKED__ cmux_frame_t;
ASSERT_STRUCT_SIZE(cmux_frame_t, 3);

typedef struct{
  union{
    uint8_t type;
    struct{
      uint8_t t_ea : 1;
      uint8_t t_cr : 1;
      uint8_t t_cmd: 6;
    };
  };
  uint8_t l_ea: 1;
  uint8_t length: 7;
  uint8_t data[];
} cmux_control_message_t;

#define GET_DATA(frame) (frame->l_ea ? frame->data : frame->ext->data)
#define GET_DLEN(frame) (frame->l_ea ? frame->length : frame->ext->length)
#define GET_HLEN(frame) (sizeof(cmux_frame_t) + (frame->l_ea ? 0 : 1))
#define GET_FLEN(frame) (GET_HLEN(frame) + GET_DLEN(frame) + 1)
#define GET_CSUM(frame) (frame->l_ea ? frame->data[GET_DLEN(frame)] : frame->ext->data[GET_DLEN(frame)])

static void send_frame(cmux_t *cmux, uint8_t channel, uint8_t frame_type, uint8_t* data, size_t length){
  uint8_t buff[1 + sizeof(cmux_frame_t) + (length <= 127 ? 0 : 1) + 1 + 1]; // sof + hdr + <ext> + csum + sof
  buff[0] = buff[sizeof(buff) - 1] = SOF_MARKER;
  cmux_frame_t* frame = (cmux_frame_t*)(buff + 1);
  frame->dlci = channel;
  frame->cr   = 1;
  frame->f_ea = 1;
  frame->control = frame_type | PF;
  frame->l_ea = length <= 127 ? 1 : 0;
  if(frame->l_ea) frame->length = length;
  else frame->ext->length = length;
  GET_DATA(frame)[0] = crc8(0, (uint8_t*)frame, GET_HLEN(frame));
  cmux->cb->tp_output(cmux, buff, length ? GET_HLEN(frame) + 1 : sizeof(buff));
  if(!length) return;
  cmux->cb->tp_output(cmux, data, length);
  cmux->cb->tp_output(cmux, buff + GET_HLEN(frame) + 1, 2);
}

void cmux_init(cmux_t *cmux){
  cmux->state = CMUX_STATE_INIT;
  cmux->is_recving_frame = false;
  cmux->frame_buff = (io_buff_t){.bytes = cmux->buff, .len = sizeof(cmux->buff)};
  memset(&cmux->channel_state[1], 0, sizeof(cmux->channel_state) - sizeof(cmux->channel_state[0]));
}

void cmux_tp_input(cmux_t *cmux, uint8_t* data, size_t length){
  io_buff_t* frame_buff = &cmux->frame_buff;
  cmux_frame_t* frame = (cmux_frame_t*)frame_buff->bytes;

  retry:;
  bool got_frame = false;
  while(length && !got_frame){
    uint8_t byte = data[0];
    if(cmux->is_recving_frame){
      if(frame_buff->w_idx == 0 && byte == SOF_MARKER) goto skip_read;
      frame_buff->bytes[frame_buff->w_idx++] = data[0];
      if(frame_buff->w_idx == GET_FLEN(frame)) cmux->is_recving_frame = false;
    }
    else if(byte == SOF_MARKER){
      if(frame_buff->w_idx) got_frame = true;
      else cmux->is_recving_frame = true;
      frame_buff->w_idx = 0;
    }
    skip_read:
    data += 1, length -= 1;
  }

  if(got_frame){
    uint8_t channel = frame->dlci;
    uint8_t frame_type = frame->control & ~PF;

    if(channel > countof(cmux->channel_state)) goto end;
    uint8_t channel_state = cmux->channel_state[channel];

    switch(frame_type){
      case FT_SABM:
        if(channel_state != CHANNEL_STATE_OPEN){
          cmux->channel_state[channel] = channel_state = CHANNEL_STATE_OPEN;
          if(channel) cmux->cb->on_event(cmux, channel, CMUX_CHANNEL_OPEN);
          else{
            send_frame(cmux, channel, FT_UA, NULL, 0);
            cmux->state = CMUX_STATE_OPEN;
          }
        }
        else if(channel){
          cmux->channel_state[channel] = channel_state = CHANNEL_STATE_CLOSE;
          cmux->cb->on_event(cmux, channel, CMUX_CHANNEL_CLOSE);
        }
        break;
      case FT_DISC:
        if(!channel) cmux->state = CMUX_STATE_CLOSED;
        send_frame(cmux, channel, FT_DM, NULL, 0);
        cmux->cb->on_event(cmux, channel, CMUX_CHANNEL_CLOSE);
        break;
      case FT_UI:
      case FT_UIH:
        if(channel_state != CHANNEL_STATE_OPEN){
          cmux->channel_state[channel] = channel_state = CHANNEL_STATE_OPEN;
          if(channel) cmux->cb->on_event(cmux, channel, CMUX_CHANNEL_OPEN);
        }
        if(channel) cmux->cb->ch_output(cmux, channel, GET_DATA(frame), GET_DLEN(frame));
        else{
          cmux_control_message_t* msg = (cmux_control_message_t*)GET_DATA(frame);
          switch(msg->t_cmd){
            case CMD_MSC:{
              cmux_address_t* addr = (cmux_address_t*)msg->data;
              LOG("CMD_MSC on channel%u", addr->dlci);
            }
          }
        }
        break;
    }
    end:
    if(length) goto retry;
  }
}

void cmux_ch_input(cmux_t *cmux, size_t ch_id, uint8_t* data, size_t length){
  if(ch_id > countof(cmux->channel_state)) return;
  int olength = length;
  while(length > 0){
    size_t to_send = cmux->max_frame_size;
    if(length < to_send) to_send = length;
    send_frame(cmux, ch_id, FT_UIH, data, to_send);
    data += to_send;
    length -= to_send;
  }
}
