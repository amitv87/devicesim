#ifndef REG_TALKER
#define REG_TALKER(...)
#endif

// Mixed GPS and GLONASS data, according to IEIC 61162-1
#ifdef USE_TALKER_GN
REG_TALKER(GN)
#endif

// Global Positioning System receiver
#ifdef USE_TALKER_GP
REG_TALKER(GP)
#endif

// GLONASS, according to IEIC 61162-1
#ifdef USE_TALKER_GL
REG_TALKER(GL)
#endif

// QZSS regional GPS augmentation system (Japan)
#ifdef USE_TALKER_QZ
REG_TALKER(QZ)
#endif

// BeiDou (China)
#ifdef USE_TALKER_BD
REG_TALKER(BD)
#endif

// Galileo Positioning System
#ifdef USE_TALKER_GA
REG_TALKER(GA)
#endif

#undef REG_TALKER
