#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/event.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "trs20.h"

#define STATE_CONSOLEIO         0               // just doing regular old console IO
#define STATE_PATCHING          1               // faux-ymodem patch upload
#define STATE_YMODEM            2               // ymodem transmit

#define PATCH_Y                 0               // sent 'y', waiting a bit
#define PATCH_PREAMBLE          1               // sending SOH/STX, 00, FF
#define PATCH_XMIT              2               // transmitting file data
#define PATCH_WAIT              3               // wait for a byte back
#define PATCH_ABORT             4               // aborting transfer

#define YMODEM_WAIT_C           0               // waiting for initial 'C'
#define YMODEM_METADATA         1               // sending metadata packet
#define YMODEM_META_ACK         2               // waiting for ACK
#define YMODEM_WAIT_START       3               // waiting for file data C
#define YMODEM_FILEDATA         4               // sending file data
#define YMODEM_DATA_ACK         5               // waiting for ACK
#define YMODEM_EOT              6               // sending EOT
#define YMODEM_EOT_ACK          7               // waiting for ACK
#define YMODEM_FINAL_C          8               // waiting for C to start next file
#define YMODEM_TERMINATING      9               // sending NULL metadata
#define YMODEM_FINAL_ACK        10              // waiting for final ACK

typedef struct __attribute__((__packed__)) ym_packet
{
    uint8_t type;
    uint8_t seqno;
    uint8_t seqcpl;
    uint8_t payload[128];
    uint16_t crc;
} ym_packet;

typedef struct ymodem_state
{
    int state;                  // YMODEM_XXX constant
    FILE *input;                // The source file
    ym_packet packet;           // The packet in flight
    uint8_t packet_idx;         // index into the packet data
    int cans;                   // CAN count
} ymodem_state;

volatile int quitit = 0;
void sigint(int _unused) {
    quitit = 1;
}

// returns 0 if the ymodem transfer is over
int ymodem_input(ymodem_state *state, uint8_t input)
{

    // Process CANcel bytes first
    if (input == 0x18) {
        if (++state->cans >= 2) {
            return 0;
        } else {
            return 1;
        }
    } else {
        state->cans = 0;
    }

    switch (state->state) {
        case YMODEM_WAIT_C:
            if (input == 'C')
                state->state = YMODEM_METADATA;
            break;
        case YMODEM_META_ACK:
            if (input == 6)
                state->state = YMODEM_WAIT_START;
            else if (input == 'C')
                state->state = YMODEM_METADATA;
            break;
        case YMODEM_WAIT_START:
            if (input == 'C') {
                state->packet.type = 1;
                state->packet.seqno++;
                state->packet.seqcpl = ~state->packet.seqno;
                memset(state->packet.payload, 0, 128);
                fread(state->packet.payload, 1, 128, state->input);
                state->packet.crc = htons(crc(state->packet.payload, 128));
                state->packet_idx = 0;
                state->state = YMODEM_FILEDATA;
            }
            break;
        case YMODEM_DATA_ACK:
            if (input == 6) {
                if (feof(state->input)) {
                    state->packet.type = 1;
                    state->packet.seqno = 0;
                    state->packet.seqcpl = 0xff;
                    memset(state->packet.payload, 0, 128);
                    state->packet.crc = htons(crc(state->packet.payload, 128));
                    state->packet_idx = 0;
                    state->state = YMODEM_EOT;
                } else {
                    state->packet.type = 1;
                    state->packet.seqno++;
                    state->packet.seqcpl = ~state->packet.seqno;
                    memset(state->packet.payload, 0, 128);
                    fread(state->packet.payload, 1, 128, state->input);
                    state->packet.crc = htons(crc(state->packet.payload, 128));
                    state->packet_idx = 0;
                    state->state = YMODEM_FILEDATA;
                }
            } else if (input == 0x15) {
                state->packet_idx = 0;
                state->state = YMODEM_FILEDATA;
            }
            break;
        case YMODEM_EOT_ACK:
            if (input == 6)
                state->state = YMODEM_FINAL_C;
            else if (input == 0x15)
                state->state = YMODEM_EOT;
            break;
        case YMODEM_FINAL_C:
            if (input == 'C')
                state->state = YMODEM_TERMINATING;
            break;
        case YMODEM_FINAL_ACK:
            if (input == 6)
                return 0;
            else if (input == 0x15)
                state->state = YMODEM_TERMINATING;
            break;
    }

    return 1;
}

void ymodem_output(ymodem_state *state, int fd)
{

    uint8_t *buffer = (uint8_t *)&state->packet;
    uint8_t output;

    switch (state->state) {
        case YMODEM_METADATA:
            if (write(fd, buffer + state->packet_idx, 1) == 1) {
                if (++(state->packet_idx) == sizeof(ym_packet)) {
                    state->packet_idx = 0;      // reset to zero in case of retransmit
                    state->state = YMODEM_META_ACK;
                }
            }
            break;
        case YMODEM_FILEDATA:
            if (write(fd, buffer + state->packet_idx, 1) == 1) {
                if (++(state->packet_idx) == sizeof(ym_packet)) {
                    state->packet_idx = 0;      // reset to zero in case of retransmit
                    state->state = YMODEM_DATA_ACK;
                }
            }
            break;
        case YMODEM_EOT:
            output = 4;
            if (write(fd, &output, 1) == 1) {
                state->state = YMODEM_EOT_ACK;
            }
            break;
        case YMODEM_TERMINATING:
            if (write(fd, buffer + state->packet_idx, 1) == 1) {
                if (++(state->packet_idx) == sizeof(ym_packet)) {
                    state->packet_idx = 0;      // reset to zero in case of retransmit
                    state->state = YMODEM_FINAL_ACK;
                }
            }
            break;
    }

}

// attempt to open a file for ymodem transmit, returns 1 if successful
int ymodem_open(ymodem_state *state, char *filename)
{
    state->input = fopen(filename, "r");

    if (!state->input) {
        perror("fopen");
        return 0;
    }

    state->state = YMODEM_WAIT_C;
    state->packet.type = 1;
    state->packet.seqno = 0;
    state->packet.seqcpl = 0xff;
    memset(state->packet.payload, 0, 128);
    strncpy((char *)state->packet.payload, filename, 128);
    state->packet.crc = htons(crc(state->packet.payload, 128));
    printf("metadata packet crc = %04x\n", state->packet.crc);
    state->packet_idx = 0;

    return 1;
}

char **files_only(const char *text, int start, int end)
{
    rl_filename_completion_desired = 1;
    rl_completion_append_character = '\0';
    return NULL;
}

int main(int argc, const char * argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <device>\n", argv[0]);
        return 1;
    }

    rl_readline_name = "trs20comm";
    rl_attempted_completion_function = files_only;

    const char *device = argv[1];
    int fd = open_device(device, B57600);

    struct termios config;
    struct termios stdin_settings;
    tcgetattr(STDIN_FILENO, &config);
    stdin_settings = config;
    signal(SIGINT, sigint);
    signal(SIGQUIT, sigint);
    config.c_lflag &=(~ICANON & ~ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &config);

    int kq = kqueue();
    struct kevent evset[3];
    EV_SET(&evset[0], fd, EVFILT_WRITE, EV_ADD | EV_DISABLE, 0, 0, NULL);
    EV_SET(&evset[1], fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    EV_SET(&evset[2], STDIN_FILENO, EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(kq, evset, 3, NULL, 0, NULL);

    int state = STATE_CONSOLEIO;

    char console_buffer[1024];
    size_t console_idx = 0;

    // transmitting a patchfile
    char patch_data[1024];
    size_t patch_idx = 0;
    ssize_t patch_size = 0;
    int patch_state;
    char patch_preamble[3] = { 0x01, 0x00, 0xff };
    int patch_preidx = 0;

    ymodem_state ym;

    struct kevent evList[32];
    int write_state = EV_DISABLE;
    int write_change = EV_DISABLE;
    int write_enable = 0;
    int write_disable = 0;
    while (!quitit) {
        int changes = 0;
        if (write_state != write_change) {
            EV_SET(&evset[0], fd, EVFILT_WRITE, EV_ADD | write_change, 0, 0, NULL);
            changes = 1;
            write_state = write_change;
        }
        int nev = kevent(kq, evset, changes, evList, 32, NULL);

        for (int i = 0; i < nev; i++) {
            if (evList[i].ident == STDIN_FILENO) {
                char input;
                if (read(STDIN_FILENO, &input, 1) == 1) {
                    if (input == '~') {
                        printf("\n"); fflush(stdout);
                        tcsetattr(STDIN_FILENO, TCSANOW, &stdin_settings);
                        char *input = readline("COMM> ");
                        if (input) {
                            if (*input) {
                                add_history(input);
                                if (strncmp(input, "p ", 2) == 0) {
                                    if (state == STATE_CONSOLEIO) {
                                        struct stat filestat;
                                        if (stat(input+2, &filestat) != 0) {
                                            perror("stat");
                                            continue;
                                        }
                                        if ((filestat.st_mode & S_IFREG) == 0) {
                                            fprintf(stderr, "patchfile must be a regular file\n");
                                            continue;
                                        }
                                        if (filestat.st_size > 1024) {
                                            fprintf(stderr, "patchfile must be at most 1024 bytes\n");
                                            continue;
                                        }
                                        if (filestat.st_size == 0) {
                                            fprintf(stderr, "patchfile is empty - nothing to transmit\n");
                                            continue;
                                        }
                                        FILE *f = fopen(input+2, "r");
                                        if (!f) {
                                            perror("open");
                                            continue;
                                        }
                                        patch_size = fread(patch_data, 1, 1024, f);
                                        fclose(f);
                                        if (patch_size < 0) {
                                            perror("fread");
                                            continue;
                                        }
                                        if (patch_size == 0) {
                                            fprintf(stderr, "unable to read all of file input, aborting\n");
                                            continue;
                                        }
                                        patch_preamble[0] = patch_size <= 128 ? 1 : 2;
                                        state = STATE_PATCHING;
                                        patch_state = PATCH_Y;
                                        patch_idx = 0;
                                        printf("Beginning patch upload: %ld bytes\n", patch_size);
                                        write_change = EV_ENABLE;
                                    } else {
                                        printf("Unable to patch: transfer in progress\n");
                                    }
                                } else if (strncmp(input, "y ", 2) == 0) {
                                    if (state == STATE_CONSOLEIO) {
                                        if (ymodem_open(&ym, input+2)) {
                                            printf("YModem transfer start\n");
                                            write_change = EV_ENABLE;
                                            state = STATE_YMODEM;
                                        }
                                    } else {
                                        printf("Unable to transfer: transfer already in progress\n");
                                    }
                                }
                            }
                            free(input);
                        }
                        tcsetattr(STDIN_FILENO, TCSANOW, &config);
                    } else {
                        // bung it in the console output buffer for writing to the device
                        if (console_idx < 1024) {
                            console_buffer[console_idx++] = input;
                            write_change = EV_ENABLE;
                        } else {
                            char bel = 7;
                            write(STDOUT_FILENO, &bel, 1);
                        }
                    }
                }
            } else {
                if (evList[i].flags & EV_EOF) {
                    printf("\n\nEOF on TTY device\n");
                    quitit = 1;
                } else if (evList[i].filter == EVFILT_READ) {
                    char input;
                    while (read(fd, &input, 1) == 1) {
                        if (state == STATE_PATCHING && patch_state == PATCH_WAIT) {
                            // any old input will do - terminate the transfer now
                            patch_state = PATCH_ABORT;
                            patch_preidx = 0;
                        }
                        if (state == STATE_YMODEM && !ymodem_input(&ym, input)) {
                            if (console_idx == 0) write_change = EV_DISABLE;
                            state = STATE_CONSOLEIO;
                        }
                        if (isprint(input) || input == 13 || input == 10) {
                            write(STDOUT_FILENO, &input, 1);
                        } else {
                            char msg[5];
                            sprintf(msg, "<%02x>", input & 0xff);
                            write(STDOUT_FILENO, msg, 4);
                        }
                        usleep(10000);
                    }
                } else if (evList[i].filter == EVFILT_WRITE) {
                    char byte;
                    ssize_t sent;
                    switch (state) {
                        case STATE_CONSOLEIO:
                            if (console_idx > 0 && write(fd, console_buffer, 1) == 1) {
                                console_idx--;
                                memcpy(console_buffer, console_buffer+1, console_idx);
                                if (console_idx == 0) write_change = EV_DISABLE;
                            }
                            break;
                        case STATE_YMODEM:
                            ymodem_output(&ym, fd);
                            usleep(15000);
                            break;
                        case STATE_PATCHING:
                            switch (patch_state) {
                                case PATCH_Y:
                                    byte = 'y';
                                    if (write(fd, &byte, 1) == 1) {
                                        patch_state = PATCH_PREAMBLE;
                                        patch_preidx = 0;
                                        usleep(50000);
                                    }
                                    break;
                                case PATCH_PREAMBLE:
                                    if (write(fd, patch_preamble + patch_preidx, 1) == 1) {
                                        byte = 'P';
                                        write(STDOUT_FILENO, &byte, 1);
                                        usleep(25000);
                                        if (++patch_preidx == 3) {
                                            patch_state = PATCH_XMIT;
                                        }
                                    }
                                    break;
                                case PATCH_XMIT:
                                    if (write(fd, patch_data + patch_idx, 1) == 1) {
                                        byte = '.';
                                        write(STDOUT_FILENO, &byte, 1);
                                        usleep(25000);
                                        if (++patch_idx == patch_size) {
                                            patch_state = PATCH_WAIT;
                                        }
                                    }
                                    break;
                                case PATCH_ABORT:
                                    byte = 0x18;
                                    if (write(fd, &byte, 1) == 1) {
                                        byte = 'X';
                                        write(STDOUT_FILENO, &byte, 1);
                                        usleep(5000);
                                        if (++patch_preidx > 1) {
                                            state = STATE_CONSOLEIO;
                                            if (console_idx == 0) write_change = EV_DISABLE;
                                        }
                                    }
                                    break;
                            }
                    }
                }
            }
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &stdin_settings);
    close(fd);

    return 0;
}
