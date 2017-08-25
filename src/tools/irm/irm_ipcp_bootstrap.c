/*
 * Ouroboros - Copyright (C) 2016 - 2017
 *
 * Bootstrap IPC Processes
 *
 *    Dimitri Staessens <dimitri.staessens@ugent.be>
 *    Sander Vrijders   <sander.vrijders@ugent.be>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., http://www.fsf.org/about/contact/.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#ifdef __FreeBSD__
#include <sys/socket.h>
#endif
#include <ouroboros/irm.h>
#include <ouroboros/ipcp.h>

#include "irm_ops.h"
#include "irm_utils.h"

#define NORMAL            "normal"
#define SHIM_UDP          "shim-udp"
#define SHIM_ETH_LLC      "shim-eth-llc"
#define LOCAL             "local"

#define MD5               "MD5"
#define SHA3_224          "SHA3_224"
#define SHA3_256          "SHA3_256"
#define SHA3_384          "SHA3_384"
#define SHA3_512          "SHA3_512"

#define DEFAULT_ADDR_SIZE  4
#define DEFAULT_FD_SIZE    2
#define DEFAULT_DDNS       0
#define DEFAULT_ADDR_AUTH  FLAT_RANDOM
#define DEFAULT_ROUTING    LINK_STATE
#define DEFAULT_HASH_ALGO  DIR_HASH_SHA3_256
#define ADDR_AUTH_FLAT     "flat"
#define ROUTING_LINK_STATE "link_state"

static void usage(void)
{
        /* FIXME: Add ipcp_config stuff */
        printf("Usage: irm ipcp bootstrap\n"
               "                name <ipcp name>\n"
               "                dif <DIF name>\n"
               "                type [TYPE]\n"
               "where TYPE = {" NORMAL " " LOCAL " "
               SHIM_UDP " " SHIM_ETH_LLC"},\n\n"
               "if TYPE == " NORMAL "\n"
               "                [addr <address size> (default: %d)]\n"
               "                [fd <fd size> (default: %d)]\n"
               "                [ttl (add time to live value in the PCI)]\n"
               "                [addr_auth <address policy> (default: %s)]\n"
               "                [routing <routing policy> (default: %s)]\n"
               "                [hash [ALGORITHM] (default: %s)]\n"
               "where ALGORITHM = {" SHA3_224 " " SHA3_256 " "
               SHA3_384 " " SHA3_512 "}\n"
               "if TYPE == " SHIM_UDP "\n"
               "                ip <IP address in dotted notation>\n"
               "                [dns <DDNS IP address in dotted notation>"
               " (default: none)]\n"
               "if TYPE == " SHIM_ETH_LLC "\n"
               "                if_name <interface name>\n",
               DEFAULT_ADDR_SIZE, DEFAULT_FD_SIZE,
               ADDR_AUTH_FLAT, ROUTING_LINK_STATE, SHA3_256);
}

int do_bootstrap_ipcp(int argc, char ** argv)
{
        char *             name           = NULL;
        pid_t              api;
        struct ipcp_config conf;
        uint8_t            addr_size      = DEFAULT_ADDR_SIZE;
        uint8_t            fd_size        = DEFAULT_FD_SIZE;
        bool               has_ttl        = false;
        enum pol_addr_auth addr_auth_type = DEFAULT_ADDR_AUTH;
        enum pol_routing   routing_type   = DEFAULT_ROUTING;
        enum pol_dir_hash  hash_algo      = DEFAULT_HASH_ALGO;
        uint32_t           ip_addr        = 0;
        uint32_t           dns_addr       = DEFAULT_DDNS;
        char *             ipcp_type      = NULL;
        char *             dif_name       = NULL;
        char *             if_name        = NULL;
        pid_t *            apis           = NULL;
        ssize_t            len            = 0;
        int                i              = 0;

        while (argc > 0) {
                if (matches(*argv, "type") == 0) {
                        ipcp_type = *(argv + 1);
                } else if (matches(*argv, "dif") == 0) {
                        dif_name = *(argv + 1);
                } else if (matches(*argv, "name") == 0) {
                        name = *(argv + 1);
                } else if (matches(*argv, "hash") == 0) {
                        if (strcmp(*(argv + 1), SHA3_224) == 0)
                                hash_algo = DIR_HASH_SHA3_224;
                        else if (strcmp(*(argv + 1), SHA3_256) == 0)
                                hash_algo = DIR_HASH_SHA3_256;
                        else if (strcmp(*(argv + 1), SHA3_384) == 0)
                                hash_algo = DIR_HASH_SHA3_384;
                        else if (strcmp(*(argv + 1), SHA3_512) == 0)
                                hash_algo = DIR_HASH_SHA3_512;
                        else
                                goto unknown_param;
                } else if (matches(*argv, "ip") == 0) {
                        if (inet_pton (AF_INET, *(argv + 1), &ip_addr) != 1)
                                goto unknown_param;
                } else if (matches(*argv, "dns") == 0) {
                        if (inet_pton(AF_INET, *(argv + 1), &dns_addr) != 1)
                                goto unknown_param;
                } else if (matches(*argv, "if_name") == 0) {
                        if_name = *(argv + 1);
                } else if (matches(*argv, "addr") == 0) {
                        addr_size = atoi(*(argv + 1));
                } else if (matches(*argv, "fd") == 0) {
                        fd_size = atoi(*(argv + 1));
                } else if (matches(*argv, "ttl") == 0) {
                        has_ttl = true;
                        argc++;
                        argv--;
                } else if (matches(*argv, "addr_auth") == 0) {
                        if (strcmp(ADDR_AUTH_FLAT, *(argv + 1)) == 0)
                                addr_auth_type = FLAT_RANDOM;
                        else
                                goto unknown_param;
                } else if (matches(*argv, "routing") == 0) {
                        if (strcmp(ROUTING_LINK_STATE, *(argv + 1)) == 0)
                                routing_type = LINK_STATE;
                        else
                                goto unknown_param;
                } else {
                        printf("Unknown option: \"%s\".\n", *argv);
                        return -1;
                }

                argc -= 2;
                argv += 2;
        }

        if (name == NULL || dif_name == NULL || ipcp_type == NULL) {
                usage();
                return -1;
        }

        strcpy(conf.dif_info.dif_name, dif_name);

        if (strcmp(ipcp_type, NORMAL) == 0) {
                conf.type = IPCP_NORMAL;
                conf.addr_size = addr_size;
                conf.fd_size = fd_size;
                conf.has_ttl = has_ttl;
                conf.addr_auth_type = addr_auth_type;
                conf.routing_type = routing_type;
                conf.dif_info.dir_hash_algo = hash_algo;
        } else if (strcmp(ipcp_type, SHIM_UDP) == 0) {
                conf.type = IPCP_SHIM_UDP;
                if (ip_addr == 0) {
                        usage();
                        return -1;
                }
                conf.ip_addr = ip_addr;
                conf.dns_addr = dns_addr;
        } else if (strcmp(ipcp_type, LOCAL) == 0) {
                conf.type = IPCP_LOCAL;
        } else if (strcmp(ipcp_type, SHIM_ETH_LLC) == 0) {
                conf.type = IPCP_SHIM_ETH_LLC;
                if (if_name == NULL) {
                        usage();
                        return -1;
                }
                conf.if_name = if_name;
        } else {
                usage();
                return -1;
        }

        len = irm_list_ipcps(name, &apis);
        if (len <= 0) {
                api = irm_create_ipcp(name, conf.type);
                if (api == 0)
                        return -1;
                len = irm_list_ipcps(name, &apis);
        }

        for (i = 0; i < len; i++)
                if (irm_bootstrap_ipcp(apis[i], &conf)) {
                        free(apis);
                        return -1;
                }

        if (apis != NULL)
                free(apis);

        return 0;

 unknown_param:
        printf("Unknown parameter for %s: \"%s\".\n", *argv, *(argv + 1));
        return -1;
}
