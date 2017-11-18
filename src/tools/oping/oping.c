/*
 * Ouroboros - Copyright (C) 2016 - 2017
 *
 * Ouroboros ping application
 *
 *    Dimitri Staessens <dimitri.staessens@ugent.be>
 *    Sander Vrijders   <sander.vrijders@ugent.be>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided
 * with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _POSIX_C_SOURCE 199506L
#define __XSI_VISIBLE   500

#include <ouroboros/endian.h>
#include <ouroboros/fqueue.h>
#include <ouroboros/dev.h>

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>

#define OPING_BUF_SIZE 1500

#define ECHO_REQUEST 0
#define ECHO_REPLY 1

#define OPING_MAX_FLOWS 256

struct c {
        char *   s_apn;
        int      interval;
        uint32_t count;
        int      size;

        /* stats */
        uint32_t sent;
        uint32_t rcvd;

        double rtt_min;
        double rtt_max;
        double rtt_avg;
        double rtt_m2;

        pthread_t reader_pt;
        pthread_t writer_pt;
} client;

struct s {
        struct timespec times[OPING_MAX_FLOWS];
        fset_t *        flows;
        fqueue_t *      fq;
        pthread_mutex_t lock;

        pthread_t cleaner_pt;
        pthread_t accept_pt;
        pthread_t server_pt;
} server;

struct oping_msg {
        uint32_t type;
        uint32_t id;
        uint64_t tv_sec;
        uint64_t tv_nsec;
} __attribute__((packed));


#include "oping_client.c"
#include "oping_server.c"

static void usage(void)
{
        printf("Usage: oping [OPTION]...\n"
               "Checks liveness between a client and a server\n"
               "and reports the Round Trip Time (RTT)\n\n"
               "  -l, --listen              Run in server mode\n"
               "\n"
               "  -c, --count               Number of packets (default 1000)\n"
               "  -i, --interval            Interval (ms, default 1000)\n"
               "  -n, --server-apn          Name of the oping server\n"
               "  -s, --size                Payload size (B, default 64)\n"
               "      --help                Display this help text and exit\n");
}

int main(int argc, char ** argv)
{
        int ret = -1;
        char * rem = NULL;
        bool serv = false;

        argc--;
        argv++;

        client.s_apn = NULL;
        client.interval = 1000;
        client.size = 64;
        client.count = 1000;

        while (argc > 0) {
                if (strcmp(*argv, "-i") == 0 ||
                    strcmp(*argv, "--interval") == 0) {
                        client.interval = strtol(*(++argv), &rem, 10);
                        --argc;
                } else if (strcmp(*argv, "-n") == 0 ||
                           strcmp(*argv, "--server_apn") == 0) {
                        client.s_apn = *(++argv);
                        --argc;
                } else if (strcmp(*argv, "-c") == 0 ||
                           strcmp(*argv, "--count") == 0) {
                        client.count = strtol(*(++argv), &rem, 10);
                        --argc;
                } else if (strcmp(*argv, "-s") == 0 ||
                           strcmp(*argv, "--size") == 0) {
                        client.size = strtol(*(++argv), &rem, 10);
                        --argc;
                } else if (strcmp(*argv, "-l") == 0 ||
                           strcmp(*argv, "--listen") == 0) {
                        serv = true;
                } else {
                        usage();
                        exit(EXIT_SUCCESS);
                }
                argc--;
                argv++;
        }

        if (serv) {
                ret = server_main();
        } else {
                if (client.s_apn == NULL) {
                        printf("No server specified.\n");
                        usage();
                        exit(EXIT_SUCCESS);
                }
                if (client.interval > 10000) {
                        printf("Ping interval truncated to 10s.\n");
                        client.interval = 10000;
                }
                if (client.size > OPING_BUF_SIZE) {
                        printf("Packet size truncated to %d bytes.\n",
                               OPING_BUF_SIZE);
                        client.size = OPING_BUF_SIZE;
                }

                if (client.size < 64) {
                        printf("Packet size set to 64 bytes.\n");
                        client.size = 64;
                }

                ret = client_main();
        }

        if (ret < 0)
                exit(EXIT_FAILURE);

        exit(EXIT_SUCCESS);
}
