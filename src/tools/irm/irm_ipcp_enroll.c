/*
 * Ouroboros - Copyright (C) 2016 - 2017
 *
 * Enroll IPC Processes
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

#include <stdio.h>
#include <stdlib.h>

#include <ouroboros/irm.h>

#include "irm_ops.h"
#include "irm_utils.h"

static void usage(void)
{
        printf("Usage: irm ipcp enroll\n"
               "                name <ipcp name>\n"
               "                dif <dif to enroll in>\n"
               "                [autobind]\n");
}

int do_enroll_ipcp(int argc, char ** argv)
{
        char *  name     = NULL;
        char *  dif_name = NULL;
        pid_t * pids     = NULL;
        pid_t   pid;
        ssize_t len      = 0;
        int     i        = 0;
        bool    autobind = false;
        int     cargs;

        while (argc > 0) {
                cargs = 2;
                if (matches(*argv, "name") == 0) {
                        name = *(argv + 1);
                } else if (matches(*argv, "dif") == 0) {
                        dif_name = *(argv + 1);
                } else if (matches(*argv, "autobind") == 0) {
                        autobind = true;
                        cargs = 1;
                } else {
                        printf("\"%s\" is unknown, try \"irm "
                               "enroll_ipcp\".\n", *argv);
                        return -1;
                }

                argc -= cargs;
                argv += cargs;
        }

        if (dif_name == NULL || name == NULL) {
                usage();
                return -1;
        }

        len = irm_list_ipcps(name, &pids);
        if (len <= 0) {
                pid = irm_create_ipcp(name, IPCP_NORMAL);
                if (pid == 0)
                        return -1;
                len = irm_list_ipcps(name, &pids);
        }

        for (i = 0; i < len; i++) {
                if (autobind && irm_bind_process(pids[i], name)) {
                        free(pids);
                        return -1;
                }

                if (irm_enroll_ipcp(pids[i], dif_name)) {
                        if (autobind)
                                irm_unbind_process(pids[i], name);
                        free(pids);
                        return -1;
                }

                if (autobind && irm_bind_process(pids[i], dif_name)) {
                        printf("Failed to bind %d to %s.\n", pids[i], dif_name);
                        free(pids);
                        return -1;
                }
        }

        if (pids != NULL)
                free(pids);

        return 0;
}
