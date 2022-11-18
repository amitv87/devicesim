#ifndef NMEA_TYPES_H
#define NMEA_TYPES_H

#include "../common.h"

#define MAX_SAT_COUNT_PER_CONST 32

#define NMEA_TALKER(x)    NMEA_TALKER_##x
#define NMEA_SENTENCE(x)  NMEA_SENTENCE_##x

typedef enum{
  #define REG_TALKER(x, ...) NMEA_TALKER(x),
  #include "defs.h"
  NMEA_TALKER(MAX),
} nmea_talker_type;

typedef enum{
  #define REG_SENTENCE(x, ...) NMEA_SENTENCE(x),
  #include "defs.h"
  NMEA_SENTENCE(MAX),
} nmea_sentence_type;

typedef struct{
  uint8_t status;
  uint16_t prn, snr;
  uint16_t ele, azi;
} sat_stat_t;

typedef struct{
  size_t max_count;
  size_t current_count;
  sat_stat_t sats[MAX_SAT_COUNT_PER_CONST];
} const_sat_group_t;

typedef struct{
  uint64_t ts;
  float lat_lng[2];
  float spd, brg;
} pos_time_info_t;

#endif
