#include <math.h>
#include "generator.h"

#define FIELD_PRINTER_ARGS nmea_gen_t* gen, field_printer_t* printer, size_t field_idx, nmea_talker_type t_type, nmea_sentence_type s_type, io_buff_t* buff

typedef struct field_printer_s field_printer_t;
typedef void (*field_printer)(FIELD_PRINTER_ARGS);

typedef struct field_printer_s{
  size_t field_idx;
  size_t repeat_count;
  field_printer func;
} field_printer_t;

typedef struct{
  char* name;
  nmea_sentence_type type;
  size_t field_count;
  size_t printer_count;
  field_printer_t* printers;
} nmea_sentence_builder_t;

#define PRINT_FUNC(x) print_field_##x
#define FUNC_DECL(x) void PRINT_FUNC(x)(FIELD_PRINTER_ARGS)

#define REG_SENTENCE(x, fc, ...) __VA_ARGS__
#define REG_SENTENCE_FIELD(idx, name, ...) FUNC_DECL(name);
#define REG_SENTENCE_FIELD_EX(idx, name, ...) REG_SENTENCE_FIELD(idx, name)
#include "defs.h"

static const char* talker_strings[] = {
  #define REG_TALKER(x, ...) #x,
  #include "defs.h"
};

#define PRINT_FIELD(fmt, ...) buff->w_idx += snprintf((char*)buff->bytes + buff->w_idx, buff->len - buff->w_idx - 1, fmt, ##__VA_ARGS__)

FUNC_DECL(dummy){PRINT_FIELD("f%zu", field_idx);}

#define REG_SENTENCE(x, fc, ...) static field_printer_t printer_##x[] = {__VA_ARGS__};
#define REG_SENTENCE_FIELD(idx, name, ...) REG_SENTENCE_FIELD_EX(idx, name, 0)
#define REG_SENTENCE_FIELD_EX(idx, name, repeat, ...) {.field_idx = idx, .repeat_count = repeat, .func = PRINT_FUNC(name)},
#include "defs.h"

static nmea_sentence_builder_t builders[] = {
  #define REG_SENTENCE(x, fc, ...) {.name = #x, .type = NMEA_SENTENCE(x), .field_count = fc, .printer_count = countof(printer_##x), .printers = printer_##x},
  #include "defs.h"
};

#define MS_TO_KNOTS_MULT (1.94384)
#define MPI 3.14159265358979323846f
// float M_PI_IN_DEG = 180.0f / 3.14159265358979323846f;

static inline float deg2rad(float deg) {
  return (deg * M_PI) / 180;
}

static inline float rad2deg(float rad){
  return (rad * 180) / M_PI;
}

static float calc_distance(float ll1[2], float ll2[2]){
  int R = 6371000; // Radius of the earth in m
  float dLat = deg2rad(ll2[0]-ll1[0]);
  float dLon = deg2rad(ll2[1]-ll1[1]);
  float a = sin(dLat/2) * sin(dLat/2) + cos(deg2rad(ll1[0])) * cos(deg2rad(ll2[0])) * sin(dLon/2) * sin(dLon/2);
  float c = 2 * atan2(sqrt(a), sqrt(1-a));
  float d = R * c; // Distance in m
  return d;
}

static float calc_bearing(float ll1[2], float ll2[2]){
  float dLon = deg2rad(ll2[1]-ll1[0]);
  float y = sin(dLon) * cos(deg2rad(ll2[0]));
  float x = cos(deg2rad(ll1[1]))*sin(deg2rad(ll2[0])) - sin(deg2rad(ll1[1]))*cos(deg2rad(ll2[0]))*cos(dLon);
  float brng = rad2deg(atan2(y, x));
  return ((int)(brng + 360) % 360);
}

#include "printer.h"

static void timer_cb(io_timer_t* timer){
  nmea_gen_t* gen = timer->usr_data;

  pos_time_info_t prev_loc_info = gen->loc_info;
  gen->loc_info.ts = clock_now();
  gen->fetch_loc(gen, gen->loc_info.lat_lng);

  if(prev_loc_info.ts == 0) return;
  float dst = calc_distance(prev_loc_info.lat_lng, gen->loc_info.lat_lng);
  gen->loc_info.spd = (dst * 1000) / (gen->loc_info.ts - prev_loc_info.ts);
  gen->loc_info.brg = calc_bearing(prev_loc_info.lat_lng, gen->loc_info.lat_lng);

  uint8_t out_buff[200] = {0};

  io_buff_t _buff = {
    .bytes = out_buff,
    .len = sizeof(out_buff),
  };
  io_buff_t* buff = &_buff;

  for(size_t i = 0; i < countof(gen->talker_mask); i++){
    uint32_t talker_mask = gen->talker_mask[i];
    if(!talker_mask) continue;

    nmea_sentence_builder_t* builder = &builders[i];

    for(size_t j = 0; j < NMEA_TALKER(MAX) && talker_mask; j++){
      uint32_t talker_mask_flag = 1 << j;
      if(!(talker_mask & talker_mask_flag)) continue;
      talker_mask &= ~talker_mask_flag;

      gen->sat_groups[j].current_count = 0;

      do_again:
      buff->r_idx = buff->w_idx = 0;
      size_t printer_count = builder->printer_count, field_idx = 0;
      field_printer_t* printer = builder->printers;

      PRINT_FIELD("$%s%s", talker_strings[j], builder->name);
      field_idx += 1;

      while(field_idx <= builder->field_count){
        buff->bytes[buff->w_idx++] = ',';
        if(printer){
          while(printer_count && printer->field_idx + printer->repeat_count < field_idx) printer += 1, printer_count -= 1;
          // LOG("printer->field_idx: %zu, printer->repeat_count: %zu, field_idx: %zu", printer->field_idx, printer->repeat_count, field_idx);
          if(printer->field_idx <= field_idx && printer->field_idx + printer->repeat_count >= field_idx)
            printer->func(gen, printer, field_idx, j, builder->type, buff);
        }
        field_idx += 1;
      }

      uint8_t checksum = 0x00;
      for(size_t i = 1; i < buff->w_idx; i++) checksum ^= buff->bytes[i];
      PRINT_FIELD("*%02x\r\n", checksum);

      int rc = gen->output(gen, (char*)buff->bytes, buff->w_idx);
      if(rc != buff->w_idx) return;

      if(gen->sat_groups[j].current_count < gen->sat_groups[j].max_count && i == NMEA_SENTENCE(GSV)) goto do_again;
    }
  }
}

void nmea_gen_start(nmea_gen_t* gen, uint32_t update_interval_ms){
  if(gen->timer.usr_data) return;
  gen->timer = (io_timer_t){
    .usr_data = gen,
    .repeat = true,
    .run_now = true,
    .interval_ms = update_interval_ms,
    .cb = timer_cb,
  };
  io_add_timer(&gen->timer);
}

void nmea_gen_stop(nmea_gen_t* gen){
  io_rem_timer(&gen->timer);
  gen->timer.usr_data = NULL;
}

void nmea_set_talker(nmea_gen_t* gen, nmea_sentence_type sentence_type, nmea_talker_type talker_type, bool enable){
  if(sentence_type >= NMEA_SENTENCE(MAX) || talker_type >= NMEA_TALKER(MAX)) return;
  uint32_t talker_mask_flag = 1 << talker_type;
  if(enable) gen->talker_mask[sentence_type] |= talker_mask_flag;
  else gen->talker_mask[sentence_type] &= ~talker_mask_flag;
}
