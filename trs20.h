#ifndef TRS20_H
#define TRS20_H

#include <termios.h>

int open_device(const char *device, speed_t baud);
uint16_t crc16(uint16_t crc, uint8_t byte);
uint16_t crc(const uint8_t *data, size_t size);

#endif
