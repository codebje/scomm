#include <stdlib.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>

int open_device(const char *device, speed_t baud)
{
    int fd = open(device, O_RDWR | O_NONBLOCK | O_NOCTTY);
    if (fd == -1)
    {
        perror("can't open device");
        exit(errno);
    }

    struct termios config;
    if (tcgetattr(fd, &config) < 0)
    {
        perror("can't get serial attributes");
        exit(errno);
    }

    /* setup for non-canonical mode */
    cfmakeraw(&config);

    cfsetspeed(&config, baud);

    config.c_cflag |= (CRTSCTS | CREAD);
    config.c_cflag &= ~CSTOPB;
    config.c_cflag &= ~CLOCAL;

    config.c_cc[VMIN]  = 1;
    config.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSAFLUSH, &config) < 0)
    {
        perror("can't set serial attributes");
        exit(errno);
    }

    return fd;
}

static const uint16_t ym_crc_tab[32] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x0000, 0x1231, 0x2462, 0x3653, 0x48c4, 0x5af5, 0x6ca6, 0x7e97,
    0x9188, 0x83b9, 0xb5ea, 0xa7db, 0xd94c, 0xcb7d, 0xfd2e, 0xef1f,
};

uint16_t crc16(uint16_t crc, uint8_t byte)
{
    uint8_t pos = ((uint8_t)(crc >> 8)) ^ byte;
    return (crc << 8) ^ ym_crc_tab[pos & 0xf] ^ ym_crc_tab[(pos >> 4) + 16];
}

uint16_t crc(const uint8_t *data, size_t size)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < size; i++) {
        crc = crc16(crc, data[i]);
    }
    return crc;
}
