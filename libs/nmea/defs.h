#ifndef REG_TALKER
#define REG_TALKER(...)
#endif

REG_TALKER(GP) // Global Positioning System receiver
REG_TALKER(GL) // GLONASS, according to IEIC 61162-1
REG_TALKER(QZ) // QZSS regional GPS augmentation system (Japan)
REG_TALKER(BD) // BeiDou (China)
REG_TALKER(GA) // Galileo Positioning System
REG_TALKER(GN) // Mixed GPS and GLONASS data, according to IEIC 61162-1

#undef REG_TALKER

#ifndef REG_SENTENCE
#define REG_SENTENCE(...)
#endif
#ifndef REG_SENTENCE_FIELD
#define REG_SENTENCE_FIELD(...)
#endif
#ifndef REG_SENTENCE_FIELD_EX
#define REG_SENTENCE_FIELD_EX(...)
#endif

/*
https://gpsd.gitlab.io/gpsd/NMEA.html
*/

REG_SENTENCE(RMC, 13, /* Recommended Minimum Navigation Information
         1          2 3         4 5          6 7    8     9      10  11
         |          | |         | |          | |    |     |      |   |
  $--RMC,hhmmss.sss,A,ddmm.mmmm,a,dddmm.mmmm,a,x.xx,xx.xx,xxxxxx,x.x,a*hh<CR><LF>
  $--RMC,hhmmss.sss,A,ddmm.mmmm,a,dddmm.mmmm,a,x.xx,xx.xx,xxxxxx,x.x,a,m*hh<CR><LF>   NMEA 2.3:
  $--RMC,hhmmss.sss,A,ddmm.mmmm,a,dddmm.mmmm,a,x.xx,xx.xx,xxxxxx,x.x,a,m,s*hh<CR><LF> NMEA 4.1:
  $GNRMC,102335.000,A,1900.5566,N,07301.8018,E,0.35,36.03,171122,5.1,E,A,A*4E*/
  REG_SENTENCE_FIELD(1, time)     // UTC of position fix, hh is hours, mm is minutes, ss.sss is seconds.
  REG_SENTENCE_FIELD(2, fixs)     // Status -> A: Valid, V: Warning
  REG_SENTENCE_FIELD(3, lat)      // Latitude, dd is degrees. mm.mmmm is minutes.
  REG_SENTENCE_FIELD(4, latdir)   // Latitude direction, N or S (North or South)
  REG_SENTENCE_FIELD(5, lng)      // Longitude, dd is degrees, mm.mmmm is minutes
  REG_SENTENCE_FIELD(6, lngdir)   // Longitude direction, E or W (East or West)
  REG_SENTENCE_FIELD(7, spd)      // Speed over ground, knots
  REG_SENTENCE_FIELD(8, brg)      // Track made good, degrees true
  REG_SENTENCE_FIELD(9, date)     // Date, ddmmyy
  REG_SENTENCE_FIELD(10, magv)    // Magnetic Variation, degrees
  REG_SENTENCE_FIELD(11, lngdir)  // Longitude direction, E or W (East or West)
  REG_SENTENCE_FIELD(12, fixm)    // FAA mode indicator (NMEA 2.3 and later)
  REG_SENTENCE_FIELD(13, navs)    // Nav Status (NMEA 4.1 and later) -> A: autonomous, D: differential, E: Estimated, M: Manual input mode N: not valid, S: Simulator, V: Valid
)

REG_SENTENCE(GGA, 14, /* Fix Data
         1          2         3 4          5 6 7  8    9   10 11   12 13  14
         |          |         | |          | | |  |    |    | |     | |   |
  $--GGA,hhmmss.sss,ddmm.mmmm,a,dddmm.mmmm,a,x,xx,x.xx,x.xx,M,±xx.x,M,x.x,xxxx*hh<CR><LF>
  $GNGGA,102335.000,1900.5566,N,07301.8018,E,1,10,0.99,37.3,M,-63.1,M,5.1,0310*4F*/
  REG_SENTENCE_FIELD(1, time)     // UTC of this position report, hh is hours, mm is minutes, ss.sss is seconds.
  REG_SENTENCE_FIELD(2, lat)      // Latitude, ddd is degrees, mm.mmmm is minutes
  REG_SENTENCE_FIELD(3, latdir)   // Latitude direction, N or S (North or South)
  REG_SENTENCE_FIELD(4, lng)      // Longitude, dd is degrees, mm.mmmm is minutes
  REG_SENTENCE_FIELD(5, lngdir)   // Longitude direction, E or W (East or West)
  REG_SENTENCE_FIELD(6, fixq)     // GPS Quality Indicator (non null) -> 0: invalid, 1: SPS, 2: DGPS, 3: PPS, 4: RTK, 5: Float RTK, 6: estimated, 7: Manual, 8: Simulation
  REG_SENTENCE_FIELD(7, satt)     // Number of satellites in use, 00 - 12
  REG_SENTENCE_FIELD(8, hdop)     // Horizontal Dilution of precision
  REG_SENTENCE_FIELD(9, alt)      // Antenna Altitude above/below mean-sea-level (geoid) (in meters)
  REG_SENTENCE_FIELD(10, altu)    // Units of antenna altitude, meters
  REG_SENTENCE_FIELD(11, gsep)    // Geoidal separation, the difference between the WGS-84 earth ellipsoid and mean-sea-level (geoid), "-" means mean-sea-level below ellipsoid
  REG_SENTENCE_FIELD(12, gsepu)   // Units of geoidal separation, meters
  REG_SENTENCE_FIELD(13, dgage)   // Age of differential GPS data, time in seconds since last SC104 type 1 or 9 update, null field when DGPS is not used
  REG_SENTENCE_FIELD(14, dgstid)  // Differential reference station ID, 0000-1023
)

/*DOP and active satellites
       1 2 3                    15   16   17
       | | |                     |    |    |
$--GSA,a,a,xx,<repating prns....>,x.xx,x.xx,x.xx*hh<CR><LF>
$GPGSA,A,3,16,07,09,04,30,,,,,,,,1.25,0.99,0.77*0A*/
#define NMEA_GSA_FIELDS(seed) \
  REG_SENTENCE_FIELD(1+seed, fixs)         /* Selection mode: M=Manual, forced to operate in 2D or 3D, A=Automatic, 2D/3D*/ \
  REG_SENTENCE_FIELD(2+seed, fixt)         /* Mode (1 = no fix, 2 = 2D fix, 3 = 3D fix)*/ \
  REG_SENTENCE_FIELD_EX(3+seed, track, 11) /* ID of 1st to 12th satellite used for fix*/ \
  REG_SENTENCE_FIELD(15+seed, pdop)        /* Position Dilution of precision*/ \
  REG_SENTENCE_FIELD(16+seed, hdop)        /* Horizontal Dilution of precision*/ \
  REG_SENTENCE_FIELD(17+seed, vdop)        /* Vertical Dilution of precision*/

REG_SENTENCE(GSA, 17, NMEA_GSA_FIELDS(0))
REG_SENTENCE(QSA, 18, REG_SENTENCE_FIELD(1, qunk) NMEA_GSA_FIELDS(1))

#define NMEA_FIELD_SAT_STAT(seq) \
  REG_SENTENCE_FIELD((4+(4*seq)), prn) /*Satellite ID or PRN number (leading zeros sent)*/ \
  REG_SENTENCE_FIELD((5+(4*seq)), ele) /*Elevation in degrees (-90 to 90) (leading zeros sent)*/ \
  REG_SENTENCE_FIELD((6+(4*seq)), azi) /*Azimuth in degrees to true north (000 to 359) (leading zeros sent)*/ \
  REG_SENTENCE_FIELD((7+(4*seq)), snr) /*SNR in dB (00-99) (leading zeros sent) more satellite info quadruples like 4-7 n-1) Signal ID (NMEA 4.11) n) checksum*/

REG_SENTENCE(GSV, 19, /* Satellites in view
         1 2 3  4  5  6   7  n
         | | |  |  |  |   |  |
  $--GSV,x,x,xx,xx,xx,xxx,xx,<repating sat info...................>*hh<CR><LF>
  $GPGSV,3,1,12,04,67,039,36,09,45,336,43,08,39,132,17,03,38,191,19*76*/
  REG_SENTENCE_FIELD(1, trow) // Total number of GSV sentences to be transmitted in this group
  REG_SENTENCE_FIELD(2, curr) // Sentence number, 1-9 of this GSV message within current group
  REG_SENTENCE_FIELD(3, tsat) // Total number of satellites in view (leading zeros sent)
  NMEA_FIELD_SAT_STAT(0)
  NMEA_FIELD_SAT_STAT(1)
  NMEA_FIELD_SAT_STAT(2)
  NMEA_FIELD_SAT_STAT(3)
)

REG_SENTENCE(GST, 8, /* Pseudorange Noise Statistics
         1          2  3  4   5    6  7   8
         |          |  |  |   |    |  |   |
  $--GST,hhmmss.sss,xx,xx,x.x,xx.x,xx,x.x,xx*hh<CR><LF>
  $GPGST,102335.000,11,13,7.5,21.8,13,8.5,30*56*/
  REG_SENTENCE_FIELD(1, time)     // UTC time of associated GGA fix
  REG_SENTENCE_FIELD(2, trms)     // Total RMS standard deviation of ranges inputs to the navigation solution
  REG_SENTENCE_FIELD(3, sdsma)    // Standard deviation (meters) of semi-major axis of error ellipse
  REG_SENTENCE_FIELD(4, sdsmi)    // Standard deviation (meters) of semi-minor axis of error ellipse
  REG_SENTENCE_FIELD(5, orsma)    // Orientation of semi-major axis of error ellipse (true north degrees)
  REG_SENTENCE_FIELD(6, laterr)   // Standard deviation (meters) of latitude error
  REG_SENTENCE_FIELD(7, lngerr)   // Standard deviation (meters) of longitude error
  REG_SENTENCE_FIELD(8, alterr)   // Standard deviation (meters) of altitude error
)

#undef REG_SENTENCE
#undef REG_SENTENCE_FIELD
#undef REG_SENTENCE_FIELD_EX
