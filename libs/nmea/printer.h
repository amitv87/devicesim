// static int nmea_random(int min, int max){
//   int rand_val = rand();
//   int bounds = max - min;
//   int min + (rand_val * bounds) / RAND_MAX;
// }

typedef struct{
  int di, mi, mf;
} deg_mm_mmmm_t;

static void get_ddmm(float dfv, deg_mm_mmmm_t* val){
  val->di = dfv;
  float mfv = (dfv - val->di) * 60;
  val->mi = mfv;
  val->mf = (mfv - val->mi)*10000;
}

FUNC_DECL(time){     // UTC of position fix, hh is hours, mm is minutes, ss.sss is seconds.
  time_t now = gen->loc_info.ts/1000;
  uint16_t ms = gen->loc_info.ts%1000;
  struct tm *tt = gmtime(&now);
  PRINT_FIELD("%02d%02d%02d.%03u", tt->tm_hour, tt->tm_min, tt->tm_sec, ms);
}

FUNC_DECL(fixs){     // Status -> A: Valid, V: Warning
  PRINT_FIELD("A");
}

FUNC_DECL(lat){      // Latitude, dd is degrees. mm.mmmm is minutes.
  deg_mm_mmmm_t val;
  get_ddmm(gen->loc_info.lat_lng[0], &val);
  PRINT_FIELD("%02d%02d.%04d", val.di, val.mi, val.mf);
}

FUNC_DECL(latdir){   // Latitude direction, N or S (North or South)
  PRINT_FIELD("%c", gen->loc_info.lat_lng[0] >= 0 ? 'N' : 'S');
}

FUNC_DECL(lng){      // Longitude, dd is degrees, mm.mmmm is minutes
  deg_mm_mmmm_t val;
  get_ddmm(gen->loc_info.lat_lng[1], &val);
  PRINT_FIELD("%03d%02d.%04d", val.di, val.mi, val.mf);
}

FUNC_DECL(lngdir){   // Longitude direction, E or W (East or West)
  PRINT_FIELD("%c", gen->loc_info.lat_lng[1] >= 0 ? 'E' : 'W');
}

FUNC_DECL(spd){      // Speed over ground, knots
  PRINT_FIELD("%.1f", gen->loc_info.spd * MS_TO_KNOTS_MULT);
}

FUNC_DECL(brg){      // Track made good, degrees true
  PRINT_FIELD("%.1f", gen->loc_info.brg);
}

FUNC_DECL(date){     // Date, ddmmyy
  time_t now = gen->loc_info.ts/1000;
  struct tm *tt = gmtime(&now);
  PRINT_FIELD("%02d%02d%02d", tt->tm_mday, tt->tm_mon + 1, tt->tm_year - 100);
}

FUNC_DECL(magv){     // Magnetic Variation, degrees
  PRINT_FIELD("28.9");
}

FUNC_DECL(fixm){     // FAA mode indicator (NMEA 2.3 and later)
  PRINT_FIELD("A");
}

FUNC_DECL(navs){     // Nav Status (NMEA 4.1 and later) -> A: autonomous, D: differential, E: Estimated, M: Manual input mode N: not valid, S: Simulator, V: Valid
  PRINT_FIELD("A");
}

FUNC_DECL(fixq){     // GPS Quality Indicator (non null) -> 0: invalid, 1: SPS, 2: DGPS, 3: PPS, 4: RTK, 5: Float RTK, 6: estimated, 7: Manual, 8: Simulation
  PRINT_FIELD("1");
}

FUNC_DECL(satt){     // Number of satellites in use, 00 - 12
  int count = 0;
  for(int i = 0; i < countof(gen->sat_groups); i++){
    const_sat_group_t* grp = &gen->sat_groups[i];
    for(int j = 0; j < grp->max_count; j++) count += (grp->sats[j].prn && grp->sats[j].snr >= 20) ? 1 : 0;
  }
  PRINT_FIELD("%d", count);
}

FUNC_DECL(alt){      // Antenna Altitude above/below mean-sea-level (geoid) (in meters)
  PRINT_FIELD("57.3");
}

FUNC_DECL(altu){     // Units of antenna altitude, meters
  PRINT_FIELD("M");
}

FUNC_DECL(gsep){     // Geoidal separation, the difference between the WGS-84 earth ellipsoid and mean-sea-level (geoid), "-" means mean-sea-level below ellipsoid
}
FUNC_DECL(gsepu){    // Units of geoidal separation, meters
}
FUNC_DECL(dgage){    // Age of differential GPS data, time in seconds since last SC104 type 1 or 9 update, null field when DGPS is not used
}
FUNC_DECL(dgstid){   // Differential reference station ID, 0000-1023
}
FUNC_DECL(fixt){     // Mode (1 = no fix, 2 = 2D fix, 3 = 3D fix)
  PRINT_FIELD("3");
}
FUNC_DECL(track){    // ID of 1st to 12th satellite used for fix
  int id = 0;
  const_sat_group_t* grp = &gen->sat_groups[t_type];

  while(id == 0){
    if(grp->current_count < grp->max_count){
      sat_stat_t *stat = &grp->sats[grp->current_count++];
      if(stat->snr >= 20) id = stat->prn;
    }
    else break;
  }

  if(id) PRINT_FIELD("%d", id);
}

FUNC_DECL(pdop){     // Position Dilution of precision
  PRINT_FIELD("1.25");
}

FUNC_DECL(hdop){     // Horizontal Dilution of precision
  PRINT_FIELD("0.99");
}

FUNC_DECL(vdop){     // Vertical Dilution of precision
  PRINT_FIELD("0.77");
}

FUNC_DECL(qunk){
}
FUNC_DECL(prn){      // Satellite ID or PRN number (leading zeros sent)
  const_sat_group_t* grp = &gen->sat_groups[t_type];
  while(grp->current_count < grp->max_count){
    sat_stat_t *stat = &grp->sats[grp->current_count];
    if(stat->prn){
      PRINT_FIELD("%02d", stat->prn);
      break;
    }
    else grp->current_count += 1;
  }
}

FUNC_DECL(ele){      // Elevation in degrees (-90 to 90) (leading zeros sent)
  const_sat_group_t* grp = &gen->sat_groups[t_type];
  if(grp->current_count < grp->max_count){
    sat_stat_t *stat = &grp->sats[grp->current_count];
    if(stat->ele) PRINT_FIELD("%02d", stat->ele);
  }
}
FUNC_DECL(azi){      // Azimuth in degrees to true north (000 to 359) (leading zeros sent)
  const_sat_group_t* grp = &gen->sat_groups[t_type];
  if(grp->current_count < grp->max_count){
    sat_stat_t *stat = &grp->sats[grp->current_count];
    if(stat->azi) PRINT_FIELD("%03d", stat->azi);
  }
}
FUNC_DECL(snr){      // SNR in dB (00-99) (leading zeros sent) more satellite info quadruples like 4-7 n-1) Signal ID (NMEA 4.11) n) checksum
  const_sat_group_t* grp = &gen->sat_groups[t_type];
  if(grp->current_count < grp->max_count){
    sat_stat_t *stat = &grp->sats[grp->current_count++];
    if(stat->snr) PRINT_FIELD("%02d", stat->snr);
  }
}
FUNC_DECL(trow){     // Total number of GSV sentences to be transmitted in this group
  int count = 0;
  const_sat_group_t* grp = &gen->sat_groups[t_type];
  for(int j = 0; j < grp->max_count; j++) count += (grp->sats[j].prn) ? 1 : 0;
  PRINT_FIELD("%d", count/4 + ((count%4) ? 1 : 0));
}
FUNC_DECL(curr){     // Sentence number, 1-9 of this GSV message within current group
  int count = 1;
  const_sat_group_t* grp = &gen->sat_groups[t_type];
  for(int j = 0; j < grp->current_count; j++) count += (grp->sats[j].prn) ? 1 : 0;
  PRINT_FIELD("%d", count/4 + ((count%4) ? 1 : 0));
}
FUNC_DECL(tsat){     // Total number of satellites in view (leading zeros sent)
  int count = 0;
  const_sat_group_t* grp = &gen->sat_groups[t_type];
  for(int j = 0; j < grp->max_count; j++) count += (grp->sats[j].prn) ? 1 : 0;
  PRINT_FIELD("%02d", count);
}

FUNC_DECL(trms){     // Total RMS standard deviation of ranges inputs to the navigation solution
  PRINT_FIELD("11");
}
FUNC_DECL(sdsma){    // Standard deviation (meters) of semi-major axis of error ellipse
  PRINT_FIELD("13");
}
FUNC_DECL(sdsmi){    // Standard deviation (meters) of semi-minor axis of error ellipse
  PRINT_FIELD("7.5");
}
FUNC_DECL(orsma){    // Orientation of semi-major axis of error ellipse (true north degrees)
  PRINT_FIELD("21.8");
}
FUNC_DECL(laterr){   // Standard deviation (meters) of latitude error
  PRINT_FIELD("13");
}
FUNC_DECL(lngerr){   // Standard deviation (meters) of longitude error
  PRINT_FIELD("8.5");
}
FUNC_DECL(alterr){   // Standard deviation (meters) of altitude error
  PRINT_FIELD("30");
}
