#ifndef NMEA_GENERATOR_H
#define NMEA_GENERATOR_H

#include "types.h"
#include "../util/io.h"

typedef struct nmea_gen_s nmea_gen_t;
typedef int (*nmea_output)(nmea_gen_t *gen, char* sentence, size_t length);
typedef void (*nmea_fetch_loc_info)(nmea_gen_t *gen, float lat_lng[2]);

typedef struct nmea_gen_s{
  void* usr_data;
  io_timer_t timer;
  nmea_output output;
  nmea_fetch_loc_info fetch_loc;
  pos_time_info_t loc_info;
  uint32_t talker_mask[NMEA_SENTENCE(MAX)];
  const_sat_group_t sat_groups[NMEA_TALKER(GN)];
} nmea_gen_t;

void nmea_gen_start(nmea_gen_t* gen, uint32_t update_interval_ms);
void nmea_gen_stop(nmea_gen_t* gen);
void nmea_set_talker(nmea_gen_t* gen, nmea_sentence_type sentence_type, nmea_talker_type talker_type, bool enable);

#endif
