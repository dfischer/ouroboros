/*
 * Ouroboros - Copyright (C) 2016
 *
 * IPC process main loop
 *
 *    Dimitri Staessens <dimitri.staessens@intec.ugent.be>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <ouroboros/config.h>
#include <ouroboros/ipcp.h>
#include <ouroboros/time_utils.h>

#define OUROBOROS_PREFIX "ipcpd/ipcp"
#include <ouroboros/logs.h>

#include <string.h>
#include <sys/socket.h>
#include <stdlib.h>
#include "ipcp.h"

struct ipcp * ipcp_instance_create()
{
        pthread_condattr_t cattr;

        struct ipcp * i = malloc(sizeof *i);
        if (i == NULL)
                return NULL;

        i->data    = NULL;
        i->ops     = NULL;
        i->irmd_fd = -1;
        i->state   = IPCP_INIT;

        pthread_rwlock_init(&i->state_lock, NULL);
        pthread_mutex_init(&i->state_mtx, NULL);
        pthread_condattr_init(&cattr);
#ifndef __APPLE__
        pthread_condattr_setclock(&cattr, PTHREAD_COND_CLOCK);
#endif
        pthread_cond_init(&i->state_cond, &cattr);

        return i;
}

void ipcp_set_state(struct ipcp * ipcp,
                    enum ipcp_state state)
{
        if (ipcp == NULL)
            return;

        pthread_mutex_lock(&ipcp->state_mtx);

        ipcp->state = state;

        pthread_cond_broadcast(&ipcp->state_cond);
        pthread_mutex_unlock(&ipcp->state_mtx);
}

enum ipcp_state ipcp_get_state(struct ipcp * ipcp)
{
        enum ipcp_state state;

        if (ipcp == NULL)
                return IPCP_NULL;

        pthread_mutex_lock(&ipcp->state_mtx);

        state = ipcp->state;

        pthread_mutex_unlock(&ipcp->state_mtx);

        return state;
}

int ipcp_wait_state(struct ipcp *           ipcp,
                    enum ipcp_state         state,
                    const struct timespec * timeout)
{
        struct timespec abstime;

        clock_gettime(PTHREAD_COND_CLOCK, &abstime);
        ts_add(&abstime, timeout, &abstime);

        pthread_mutex_lock(&ipcp->state_mtx);

        while (ipcp->state != state && ipcp->state != IPCP_SHUTDOWN) {
                int ret;
                if (timeout == NULL)
                        ret = pthread_cond_wait(&ipcp->state_cond,
                                                &ipcp->state_mtx);
                else
                        ret = pthread_cond_timedwait(&ipcp->state_cond,
                                                     &ipcp->state_mtx,
                                                     &abstime);
                if (ret) {
                        pthread_mutex_unlock(&ipcp->state_mtx);
                        return -ret;
                }
        }

        pthread_mutex_unlock(&ipcp->state_mtx);

        return 0;
}

int ipcp_parse_arg(int argc, char * argv[])
{
        char * log_file;
        size_t len = 0;

        if (!(argc == 3 || argc == 2))
                return -1;

        /* argument 1: api of irmd */
        if (atoi(argv[1]) == 0)
                return -1;

        if (argv[2] == NULL)
                return 0;

        len += strlen(INSTALL_PREFIX);
        len += strlen(LOG_DIR);
        len += strlen(argv[2]);

        log_file = malloc(len + 1);
        if (log_file == NULL) {
                LOG_ERR("Failed to malloc");
                return -1;
        }

        strcpy(log_file, INSTALL_PREFIX);
        strcat(log_file, LOG_DIR);
        strcat(log_file, argv[2]);
        log_file[len] = '\0';

        if (set_logfile(log_file))
                LOG_ERR("Cannot open %s, falling back to stdout for logs.",
                        log_file);

        free(log_file);

        return 0;
}

void * ipcp_main_loop(void * o)
{
        int     lsockfd;
        int     sockfd;
        uint8_t buf[IPCP_MSG_BUF_SIZE];
        struct ipcp * _ipcp = (struct ipcp *) o;

        ipcp_msg_t * msg;
        ssize_t      count;
        buffer_t     buffer;
        ipcp_msg_t   ret_msg = IPCP_MSG__INIT;

        dif_config_msg_t * conf_msg;
        struct dif_config  conf;

        char * sock_path;
        char * msg_name_dup;

        struct timeval tv = {(IPCP_ACCEPT_TIMEOUT / 1000),
                             (IPCP_ACCEPT_TIMEOUT % 1000) * 1000};

        struct timeval ltv = {(SOCKET_TIMEOUT / 1000),
                             (SOCKET_TIMEOUT % 1000) * 1000};


        if (_ipcp == NULL) {
                LOG_ERR("Invalid ipcp struct.");
                return (void *) 1;
        }

        sock_path = ipcp_sock_path(getpid());
        if (sock_path == NULL)
                return (void *) 1;

        sockfd = server_socket_open(sock_path);
        if (sockfd < 0) {
                LOG_ERR("Could not open server socket.");
                free(sock_path);
                return (void *) 1;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,
                       (void *) &tv, sizeof(tv)))
                LOG_WARN("Failed to set timeout on socket.");

        while (true) {
                pthread_rwlock_rdlock(&_ipcp->state_lock);
                if (ipcp_get_state(_ipcp) == IPCP_SHUTDOWN) {
                        pthread_rwlock_unlock(&_ipcp->state_lock);
                        break;
                }

                pthread_rwlock_unlock(&_ipcp->state_lock);

                ret_msg.code = IPCP_MSG_CODE__IPCP_REPLY;

                lsockfd = accept(sockfd, 0, 0);
                if (lsockfd < 0)
                        continue;

                if (setsockopt(lsockfd, SOL_SOCKET, SO_RCVTIMEO,
                               (void *) &ltv, sizeof(ltv)))
                        LOG_WARN("Failed to set timeout on socket.");

                count = read(lsockfd, buf, IPCP_MSG_BUF_SIZE);
                if (count <= 0) {
                        LOG_ERR("Failed to read from socket");
                        close(lsockfd);
                        continue;
                }

                msg = ipcp_msg__unpack(NULL, count, buf);
                if (msg == NULL) {
                        close(lsockfd);
                        continue;
                }

                switch (msg->code) {
                case IPCP_MSG_CODE__IPCP_BOOTSTRAP:
                        if (_ipcp->ops->ipcp_bootstrap == NULL) {
                                LOG_ERR("Bootstrap unsupported.");
                                break;
                        }
                        conf_msg = msg->conf;
                        conf.type = conf_msg->ipcp_type;
                        conf.dif_name = strdup(conf_msg->dif_name);
                        if (conf.dif_name == NULL) {
                                ret_msg.has_result = true;
                                ret_msg.result = -1;
                                break;
                        }
                        if (conf_msg->ipcp_type == IPCP_NORMAL) {
                                conf.addr_size = conf_msg->addr_size;
                                conf.cep_id_size = conf_msg->cep_id_size;
                                conf.pdu_length_size
                                        = conf_msg->pdu_length_size;
                                conf.qos_id_size     = conf_msg->qos_id_size;
                                conf.seqno_size      = conf_msg->seqno_size;
                                conf.ttl_size        = conf_msg->seqno_size;
                                conf.chk_size        = conf_msg->chk_size;
                                conf.min_pdu_size    = conf_msg->min_pdu_size;
                                conf.max_pdu_size    = conf_msg->max_pdu_size;
                        }
                        if (conf_msg->ipcp_type == IPCP_SHIM_UDP) {
                                conf.ip_addr  = conf_msg->ip_addr;
                                conf.dns_addr = conf_msg->dns_addr;
                        }
                        if (conf_msg->ipcp_type == IPCP_SHIM_ETH_LLC)
                                conf.if_name = conf_msg->if_name;

                        ret_msg.has_result = true;
                        ret_msg.result = _ipcp->ops->ipcp_bootstrap(&conf);
                        if (ret_msg.result < 0)
                                free(conf.dif_name);
                        break;
                case IPCP_MSG_CODE__IPCP_ENROLL:
                        if (_ipcp->ops->ipcp_enroll == NULL) {
                                LOG_ERR("Enroll unsupported.");
                                break;
                        }
                        ret_msg.has_result = true;
                        ret_msg.result = _ipcp->ops->ipcp_enroll(msg->dif_name);

                        break;
                case IPCP_MSG_CODE__IPCP_NAME_REG:
                        if (_ipcp->ops->ipcp_name_reg == NULL) {
                                LOG_ERR("Ap_reg unsupported.");
                                break;
                        }
                        msg_name_dup = strdup(msg->name);
                        ret_msg.has_result = true;
                        ret_msg.result =
                                _ipcp->ops->ipcp_name_reg(msg_name_dup);
                        if (ret_msg.result < 0)
                                free(msg_name_dup);
                        break;
                case IPCP_MSG_CODE__IPCP_NAME_UNREG:
                        if (_ipcp->ops->ipcp_name_unreg == NULL) {
                                LOG_ERR("Ap_unreg unsupported.");
                                break;
                        }
                        ret_msg.has_result = true;
                        ret_msg.result =
                                _ipcp->ops->ipcp_name_unreg(msg->name);
                        break;
                case IPCP_MSG_CODE__IPCP_FLOW_ALLOC:
                        if (_ipcp->ops->ipcp_flow_alloc == NULL) {
                                LOG_ERR("Flow_alloc unsupported.");
                                break;
                        }
                        ret_msg.has_result = true;
                        ret_msg.result =
                                _ipcp->ops->ipcp_flow_alloc(msg->api,
                                                            msg->port_id,
                                                            msg->dst_name,
                                                            msg->src_ae_name,
                                                            msg->qos_cube);
                        break;
                case IPCP_MSG_CODE__IPCP_FLOW_ALLOC_RESP:
                        if (_ipcp->ops->ipcp_flow_alloc_resp == NULL) {
                                LOG_ERR("Flow_alloc_resp unsupported.");
                                break;
                        }
                        ret_msg.has_result = true;
                        ret_msg.result =
                                _ipcp->ops->ipcp_flow_alloc_resp(msg->api,
                                                                 msg->port_id,
                                                                 msg->result);
                        break;
                case IPCP_MSG_CODE__IPCP_FLOW_DEALLOC:
                        if (_ipcp->ops->ipcp_flow_dealloc == NULL) {
                                LOG_ERR("Flow_dealloc unsupported.");
                                break;
                        }
                        ret_msg.has_result = true;
                        ret_msg.result =
                                _ipcp->ops->ipcp_flow_dealloc(msg->port_id);
                        break;
                default:
                        LOG_ERR("Don't know that message code");
                        break;
                }

                ipcp_msg__free_unpacked(msg, NULL);

                buffer.len = ipcp_msg__get_packed_size(&ret_msg);
                if (buffer.len == 0) {
                        LOG_ERR("Failed to send reply message");
                        close(lsockfd);
                        continue;
                }

                buffer.data = malloc(buffer.len);
                if (buffer.data == NULL) {
                        close(lsockfd);
                        continue;
                }

                ipcp_msg__pack(&ret_msg, buffer.data);

                if (write(lsockfd, buffer.data, buffer.len) == -1) {
                        free(buffer.data);
                        close(lsockfd);
                        continue;
                }

                free(buffer.data);
                close(lsockfd);
        }

        close(sockfd);
        if (unlink(sock_path))
                LOG_DBG("Could not unlink %s.", sock_path);

        free(sock_path);

        return (void *) 0;
}
