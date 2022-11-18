#ifndef PICO_CRC_H
#define PICO_CRC_H

#include <stdint.h>
#include <stddef.h>

#define __CRC_FUNC(name, type) type name(type crc, uint8_t* data, size_t length)
#ifndef CRC_FUNC
#define CRC_FUNC(name, type) __CRC_FUNC(name, type);
#endif
CRC_FUNC(crc8, uint8_t)
CRC_FUNC(crc16, uint16_t)
CRC_FUNC(crc32, uint32_t)
CRC_FUNC(crc32c, uint32_t)
#undef CRC_FUNC

uint32_t crc32_v2(uint32_t crc, uint8_t* data, size_t length);
uint16_t crc16_xmomdem(uint16_t crc, uint8_t* data, size_t length);

#endif
