#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <arpa/inet.h>

#include "trs20.h"

struct __attribute__((__packed__)) ym_packet {
    uint8_t type;
    uint8_t seqno;
    uint8_t seqcpl;
    uint8_t payload[128];
    uint16_t crc;
} ym_packet;

int main(int argc, const char * argv[])
{
    struct ym_packet data;
    const char *buffer = (const char *)&data;
    data.type = 0x01;
    data.seqno = 0;
    data.seqcpl = 0xff;
    memset(data.payload, 0, 128);
    strncpy((char *)data.payload, argv[2], 128);
    data.crc = htons(crc(data.payload, 128));
    printf("initial packet crc = %04x\n", ntohs(data.crc));
    printf("crc in memory: %02x%02x\n", (uint8_t)buffer[131], (uint8_t)buffer[132]);

    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <device> <patch> <payload>\n", argv[0]);
        return 1;
    }

    const char *device = argv[1];
    int fd = open_device(device, B57600);

    FILE *input = fopen(argv[2], "r");
    if (!input) {
        perror(argv[2]);
        exit(errno);
    }

    int kq = kqueue();
    struct kevent evset[2];
    EV_SET(&evset[0], fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    EV_SET(&evset[1], fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
    kevent(kq, evset, 2, NULL, 0, NULL);

    size_t index = 0;
    uint8_t sequence = 0;

    size_t bootstrapped = 0;
    uint8_t closed = 0;

    usleep(150000);

    printf("Temporarily out of action\n");

    close(fd);
    fclose(input);

    return 0;
}
