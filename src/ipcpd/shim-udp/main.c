/*
 * Ouroboros - Copyright (C) 2016
 *
 * Shim IPC process over UDP
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
#include "ipcp.h"
#include "flow.h"
#include "shim_udp_config.h"
#include <ouroboros/shm_du_map.h>
#include <ouroboros/shm_ap_rbuff.h>
#include <ouroboros/list.h>
#include <ouroboros/utils.h>
#include <ouroboros/ipcp.h>
#include <ouroboros/dif_config.h>
#include <ouroboros/sockets.h>
#include <ouroboros/bitmap.h>
#include <ouroboros/dev.h>

#define OUROBOROS_PREFIX "ipcpd/shim-udp"

#include <ouroboros/logs.h>

#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/wait.h>

#define THIS_TYPE IPCP_SHIM_UDP
#define LISTEN_PORT htons(0x0D1F)
#define SHIM_UDP_BUF_SIZE 256
#define SHIM_UDP_MAX_SDU_SIZE 8980
#define DNS_TTL 86400

#define shim_data(type) ((struct ipcp_udp_data *) type->data)

#define local_ip (((struct ipcp_udp_data *)                     \
                   _ipcp->data)->s_saddr.sin_addr.s_addr)

/* global for trapping signal */
int irmd_pid;

/* this IPCP's data */
#ifdef MAKE_CHECK
extern struct ipcp * _ipcp; /* defined in test */
#else
struct ipcp * _ipcp;
#endif

/*
 * copied from ouroboros/dev. The shim needs access to the internals
 * because it doesn't follow all steps necessary steps to get
 * the info
 */

/* the shim needs access to these internals */
struct shim_ap_data {
        instance_name_t *     api;
        struct shm_du_map *   dum;
        struct bmp *          fds;

        struct shm_ap_rbuff * rb;
        struct flow           flows[AP_MAX_FLOWS];

        pthread_t mainloop;
        pthread_t sduloop;
        pthread_t handler;
        pthread_t sdu_reader[2];
        int       ping_pong;
} * _ap_instance;

int shim_ap_init(char * ap_name)
{
        _ap_instance = malloc(sizeof(struct shim_ap_data));
        if (_ap_instance == NULL) {
                return -1;
        }

        _ap_instance->api = instance_name_create();
        if (_ap_instance->api == NULL) {
                free(_ap_instance);
                return -1;
        }

        if (instance_name_init_from(_ap_instance->api,
                                    ap_name,
                                    getpid()) == NULL) {
                instance_name_destroy(_ap_instance->api);
                free(_ap_instance);
                return -1;
        }

        _ap_instance->fds = bmp_create(AP_MAX_FLOWS, 0);
        if (_ap_instance->fds == NULL) {
                instance_name_destroy(_ap_instance->api);
                free(_ap_instance);
                return -1;
        }

        _ap_instance->dum = shm_du_map_open();
        if (_ap_instance->dum == NULL) {
                instance_name_destroy(_ap_instance->api);
                bmp_destroy(_ap_instance->fds);
                free(_ap_instance);
                return -1;
        }

        _ap_instance->rb = shm_ap_rbuff_create();
        if (_ap_instance->rb == NULL) {
                instance_name_destroy(_ap_instance->api);
                bmp_destroy(_ap_instance->fds);
                free(_ap_instance);
                return -1;
        }

        return 0;
}

void shim_ap_fini()
{
        int i = 0;

        if (_ap_instance == NULL)
                return;
        if (_ap_instance->api != NULL)
                instance_name_destroy(_ap_instance->api);
        if (_ap_instance->fds != NULL)
                bmp_destroy(_ap_instance->fds);
        if (_ap_instance->dum != NULL)
                shm_du_map_close(_ap_instance->dum);
        if (_ap_instance->rb != NULL)
                shm_ap_rbuff_destroy(_ap_instance->rb);
        for (i = 0; i < AP_MAX_FLOWS; i ++)
                if (_ap_instance->flows[i].rb != NULL)
                        shm_ap_rbuff_close(_ap_instance->flows[i].rb);

        free(_ap_instance);
}

static int port_id_to_fd(int port_id)
{
        int i;
        for (i = 0; i < AP_MAX_FLOWS; ++i)
                if (_ap_instance->flows[i].port_id == port_id
                        && _ap_instance->flows[i].state != FLOW_NULL)
                        return i;
        return -1;
}

static ssize_t ipcp_udp_flow_write(int fd, void * buf, size_t count)
{
        /* the AP chooses the amount of headspace and tailspace */
        size_t index = shm_create_du_buff(_ap_instance->dum,
                                          count,
                                          0,
                                          buf,
                                          count);
        struct rb_entry e = {index, _ap_instance->flows[fd].port_id};

        if (index == -1)
                return -1;

        if (shm_ap_rbuff_write(_ap_instance->flows[fd].rb, &e) < 0) {
                shm_release_du_buff(_ap_instance->dum, index);
                return -EPIPE;
        }

        return 0;
}

/*
 * end copy from dev.c
 */

struct ipcp_udp_data {
        /* keep ipcp_data first for polymorphism */
        struct ipcp_data ipcp_data;

        uint32_t ip_addr;
        uint32_t dns_addr;

        /* listen server */
        struct sockaddr_in s_saddr;
        int                s_fd;

        fd_set flow_fd_s;

        pthread_mutex_t lock;
};

struct ipcp_udp_data * ipcp_udp_data_create()
{
        struct ipcp_udp_data * udp_data;
        struct ipcp_data *     data;
        enum ipcp_type         ipcp_type;

        udp_data = malloc(sizeof *udp_data);
        if (udp_data == NULL) {
                LOG_DBGF("Failed to allocate.");
                return NULL;
        }

        ipcp_type = THIS_TYPE;
        data = (struct ipcp_data *) udp_data;
        if (ipcp_data_init(data, ipcp_type) == NULL) {
                free(udp_data);
                return NULL;
        }

        FD_ZERO(&udp_data->flow_fd_s);

        return udp_data;
}

void ipcp_udp_data_destroy(struct ipcp_udp_data * data)
{
        if (data == NULL)
                return;

        ipcp_data_destroy((struct ipcp_data *) data);
}

void ipcp_udp_destroy(struct ipcp * ipcp)
{
        ipcp_udp_data_destroy((struct ipcp_udp_data *) ipcp->data);
        shim_ap_fini();
        free(ipcp);
}

void ipcp_sig_handler(int sig, siginfo_t * info, void * c)
{
        switch(sig) {
        case SIGINT:
        case SIGTERM:
        case SIGHUP:
                if (info->si_pid == irmd_pid || info->si_pid == 0) {
                        LOG_DBG("Terminating by order of %d. Bye.",
                                info->si_pid);
                        pthread_cancel(_ap_instance->mainloop);
                        pthread_cancel(_ap_instance->handler);
                        pthread_cancel(_ap_instance->sdu_reader[0]);
                        pthread_cancel(_ap_instance->sdu_reader[1]);
                        pthread_cancel(_ap_instance->sduloop);
                        exit(0);
                }
        default:
                return;
        }
}

static void * ipcp_udp_listener()
{
        char buf[SHIM_UDP_BUF_SIZE];
        int  n = 0;

        struct sockaddr_in f_saddr;
        struct sockaddr_in c_saddr;
        struct hostent  *  hostp;
        int                sfd = shim_data(_ipcp)->s_fd;

        while (true) {
                int fd;
                n = sizeof c_saddr;
                n = recvfrom(sfd, buf, SHIM_UDP_BUF_SIZE, 0,
                             (struct sockaddr *) &c_saddr, (unsigned *) &n);
                if (n < 0)
                        continue;

                /* flow alloc request from other host */
                hostp = gethostbyaddr((const char *) &c_saddr.sin_addr.s_addr,
                                      sizeof(c_saddr.sin_addr.s_addr), AF_INET);
                if (hostp == NULL)
                        continue;

                fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

                memset((char *) &f_saddr, 0, sizeof f_saddr);
                f_saddr.sin_family      = AF_INET;
                f_saddr.sin_addr.s_addr = local_ip;

                /*
                 * FIXME: we could have a port dedicated per registered AP
                 * Not that critical for UDP, but will be for LLC
                 */

                f_saddr.sin_port        = 0;

                /*
                 * store the remote address in the file descriptor
                 * this avoids having to store the sockaddr_in in
                 * the flow structure
                 */

                if (connect(fd,
                            (struct sockaddr *) &c_saddr, sizeof c_saddr) < 0) {
                        close(fd);
                        continue;
                }

                /* echo back the packet */
                if (send(fd, buf, strlen(buf), 0) < 0) {
                        LOG_ERR("Failed to echo back the packet.");
                        close(fd);
                        continue;
                }

                /* reply to IRM */

                _ap_instance->flows[fd].port_id = ipcp_flow_req_arr(getpid(),
                                                                    buf,
                                                                    UNKNOWN_AP,
                                                                    UNKNOWN_AE);
                if (_ap_instance->flows[fd].port_id < 0) {
                        LOG_ERR("Could not get port id from IRMd");
                        close(fd);
                        continue;
                }

                _ap_instance->flows[fd].rb     = NULL;
                _ap_instance->flows[fd].state  = FLOW_PENDING;

                LOG_DBG("Pending allocation request, port_id %u, UDP fd %d.",
                         _ap_instance->flows[fd].port_id, fd);
        }

        return 0;
}

static void * ipcp_udp_sdu_reader()
{
        int n;
        int fd;
        char buf[SHIM_UDP_MAX_SDU_SIZE];

        struct sockaddr_in r_saddr;

        while (true) {
                if (select(FD_SETSIZE,
                           &shim_data(_ipcp)->flow_fd_s,
                           NULL, NULL, NULL)
                    < 0)
                        continue;

                for (fd = 0; fd < FD_SETSIZE; ++fd) {
                        if (!FD_ISSET(fd, &shim_data(_ipcp)->flow_fd_s))
                                continue;

                        n = sizeof r_saddr;
                        n = recvfrom(fd,
                                     buf,
                                     SHIM_UDP_MAX_SDU_SIZE,
                                     0,
                                     (struct sockaddr *) &r_saddr,
                                     (unsigned *) &n);

                        /* send the sdu to the correct port_id */
                        ipcp_udp_flow_write(fd, buf, n);
                }
        }

        return (void *) 0;
}

int ipcp_udp_bootstrap(struct dif_config * conf)
{
        char ipstr[INET_ADDRSTRLEN];
        char dnsstr[INET_ADDRSTRLEN];
        int enable = 1;

        if (conf->type != THIS_TYPE) {
                LOG_ERR("Config doesn't match IPCP type.");
                return -1;
        }

        if (_ipcp->state != IPCP_INIT) {
                LOG_ERR("IPCP in wrong state.");
                return -1;
        }

        if (inet_ntop(AF_INET,
                      &conf->ip_addr,
                      ipstr,
                      INET_ADDRSTRLEN) == NULL) {
                LOG_ERR("Failed to convert IP address");
                return -1;
        }

        if (conf->dns_addr != 0) {
                if (inet_ntop(AF_INET,
                              &conf->dns_addr,
                              dnsstr,
                              INET_ADDRSTRLEN) == NULL) {
                        LOG_ERR("Failed to convert DNS address");
                        return -1;
                }
#ifndef CONFIG_OUROBOROS_ENABLE_DNS
                LOG_WARN("DNS address ignored since shim-udp was "
                         "compiled without DNS support.");
#endif
        } else
                strcpy(dnsstr, "not set");

        shim_data(_ipcp)->ip_addr  = conf->ip_addr;
        shim_data(_ipcp)->dns_addr = conf->dns_addr;

        /* UDP listen server */

        if ((shim_data(_ipcp)->s_fd =
             socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
                LOG_DBGF("Can't create socket.");
                return -1;
        }

        if (setsockopt(shim_data(_ipcp)->s_fd,
                       SOL_SOCKET,
                        SO_REUSEADDR,
                        &enable,
                       sizeof(int)) < 0) {
                LOG_DBGF("Setsockopt(SO_REUSEADDR) failed.");
        }

        shim_data(_ipcp)->s_saddr.sin_family      = AF_INET;
        shim_data(_ipcp)->s_saddr.sin_addr.s_addr = conf->ip_addr;
        shim_data(_ipcp)->s_saddr.sin_port        = LISTEN_PORT;

        if (bind(shim_data(_ipcp)->s_fd,
                 (struct sockaddr *) &shim_data(_ipcp)->s_saddr,
                 sizeof shim_data(_ipcp)->s_saddr ) < 0) {
                LOG_ERR("Couldn't bind to %s.", ipstr);
                return -1;
        }

        FD_CLR(shim_data(_ipcp)->s_fd, &shim_data(_ipcp)->flow_fd_s);

        pthread_create(&_ap_instance->handler,
                       NULL,
                       ipcp_udp_listener,
                       NULL);
        pthread_create(&_ap_instance->sdu_reader[0],
                       NULL,
                       ipcp_udp_sdu_reader,
                       NULL);

        pthread_create(&_ap_instance->sdu_reader[1],
                       NULL,
                       ipcp_udp_sdu_reader,
                       NULL);

        _ap_instance->ping_pong = 0;

        _ipcp->state = IPCP_ENROLLED;

        LOG_DBG("Bootstrapped shim IPCP over UDP with pid %d.",
                getpid());

        LOG_DBG("Bound to IP address %s.", ipstr);
        LOG_DBG("DNS server address is %s.", dnsstr);

        return 0;
}

#ifdef CONFIG_OUROBOROS_ENABLE_DNS
/* FIXME: Dependency on nsupdate to be removed in the end */
/* NOTE: Disgusted with this crap */
static int ddns_send(char * cmd)
{
        pid_t pid = 0;
        int wstatus;
        int pipe_fd[2];
        char * argv[] = {NSUPDATE_EXEC, 0};
        char * envp[] = {0};

        if (pipe(pipe_fd)) {
                LOG_ERR("Failed to create pipe.");
                return -1;
        }

        pid = fork();
        if (pid == -1) {
                LOG_ERR("Failed to fork.");
                return -1;
        }

        if (pid == 0) {
                close(pipe_fd[1]);
                dup2(pipe_fd[0], 0);
                execve(argv[0], &argv[0], envp);
        }

        close(pipe_fd[0]);

        if (write(pipe_fd[1], cmd, strlen(cmd)) == -1) {
                LOG_ERR("Failed to communicate with nsupdate.");
                close(pipe_fd[1]);
                return -1;
        }

        waitpid(pid, &wstatus, 0);
        if (WIFEXITED(wstatus) == true &&
            WEXITSTATUS(wstatus) == 0)
                LOG_DBGF("Succesfully communicated with DNS server.");
        else
                LOG_ERR("Failed to register with DNS server.");

        close(pipe_fd[1]);
        return 0;
}


static uint32_t ddns_resolve(char * name, uint32_t dns_addr)
{
        pid_t pid = 0;
        int wstatus;
        int pipe_fd[2];
        char dnsstr[INET_ADDRSTRLEN];
        char buf[SHIM_UDP_BUF_SIZE];
        ssize_t count = 0;
        char * substr = NULL;
        char * substr2 = NULL;
        char * addr_str = "Address:";
        uint32_t ip_addr = 0;

        if (inet_ntop(AF_INET, &dns_addr, dnsstr, INET_ADDRSTRLEN) == NULL) {
                return 0;
        }


        if (pipe(pipe_fd)) {
                LOG_ERR("Failed to create pipe.");
                return 0;
        }

        pid = fork();
        if (pid == -1) {
                LOG_ERR("Failed to fork.");
                return 0;
        }

        if (pid == 0) {
                char * argv[] = {NSLOOKUP_EXEC, name, dnsstr, 0};
                char * envp[] = {0};

                close(pipe_fd[0]);
                dup2(pipe_fd[1], 1);
                execve(argv[0], &argv[0], envp);
        }

        close(pipe_fd[1]);

        count = read(pipe_fd[0], buf, SHIM_UDP_BUF_SIZE);
        if (count <= 0) {
                LOG_ERR("Failed to communicate with nslookup.");
                close(pipe_fd[0]);
                return 0;
        }

        close(pipe_fd[0]);

        waitpid(pid, &wstatus, 0);
        if (WIFEXITED(wstatus) == true &&
            WEXITSTATUS(wstatus) == 0)
                LOG_DBGF("Succesfully communicated with nslookup.");
        else
                LOG_ERR("Failed to resolve DNS address.");

        buf[count] = '\0';
        substr = strtok(buf, "\n");
        while (substr != NULL) {
                substr2 = substr;
                substr = strtok(NULL, "\n");
        }

        if (strstr(substr2, addr_str) == NULL) {
                LOG_ERR("Failed to resolve DNS address.");
                return 0;
        }

        if (inet_pton(AF_INET, substr2 + strlen(addr_str) + 1, &ip_addr) != 1) {
                LOG_ERR("Failed to resolve DNS address.");
                return 0;
        }

        return ip_addr;
}
#endif

int ipcp_udp_name_reg(char * name)
{
#ifdef CONFIG_OUROBOROS_ENABLE_DNS
        char ipstr[INET_ADDRSTRLEN];
        char dnsstr[INET_ADDRSTRLEN];
        /* max DNS name length + max IP length + command length */
        char cmd[100];
        uint32_t dns_addr;
        uint32_t ip_addr;
#endif

        if (_ipcp->state != IPCP_ENROLLED) {
                LOG_DBGF("Won't register with non-enrolled IPCP.");
                return -1;
        }

        if (strlen(name) > 24) {
                LOG_ERR("DNS names cannot be longer than 24 chars.");
                return -1;
        }

        if (ipcp_data_add_reg_entry(_ipcp->data, name)) {
                LOG_ERR("Failed to add %s to local registry.", name);
                return -1;
        }

#ifdef CONFIG_OUROBOROS_ENABLE_DNS
        /* register application with DNS server */

        dns_addr = shim_data(_ipcp)->dns_addr;
        if (dns_addr != 0) {
                ip_addr = shim_data(_ipcp)->ip_addr;

                if (inet_ntop(AF_INET, &ip_addr,
                              ipstr, INET_ADDRSTRLEN) == NULL) {
                        return -1;
                }

                if (inet_ntop(AF_INET, &dns_addr,
                              dnsstr, INET_ADDRSTRLEN) == NULL) {
                        return -1;
                }

                sprintf(cmd, "server %s\nupdate add %s %d A %s\nsend\nquit\n",
                        dnsstr, name, DNS_TTL, ipstr);

                if (ddns_send(cmd)) {
                        ipcp_data_del_reg_entry(_ipcp->data, name);
                        return -1;
                }
        }
#endif

        LOG_DBG("Registered %s.", name);

        return 0;
}

int ipcp_udp_name_unreg(char * name)
{
#ifdef CONFIG_OUROBOROS_ENABLE_DNS
        char dnsstr[INET_ADDRSTRLEN];
        /* max DNS name length + max IP length + max command length */
        char cmd[100];
        uint32_t dns_addr;
#endif

        if (strlen(name) > 24) {
                LOG_ERR("DNS names cannot be longer than 24 chars.");
                return -1;
        }

#ifdef CONFIG_OUROBOROS_ENABLE_DNS
        /* unregister application with DNS server */

        dns_addr = shim_data(_ipcp)->dns_addr;
        if (dns_addr != 0) {
                if (inet_ntop(AF_INET, &dns_addr, dnsstr, INET_ADDRSTRLEN)
                    == NULL) {
                        return -1;
                }
                sprintf(cmd, "server %s\nupdate delete %s A\nsend\nquit\n",
                        dnsstr, name);

                ddns_send(cmd);
        }
#endif

        ipcp_data_del_reg_entry(_ipcp->data, name);

        LOG_DBG("Unregistered %s.", name);

        return 0;
}

int ipcp_udp_flow_alloc(int               port_id,
                        pid_t             n_pid,
                        char *            dst_name,
                        char *            src_ap_name,
                        char *            src_ae_name,
                        struct qos_spec * qos)
{
        struct sockaddr_in l_saddr;
        struct sockaddr_in r_saddr;
        struct sockaddr_in rf_saddr;
        int                fd;
        int                n;
        char *             recv_buf = NULL;
        struct hostent *   h;
        uint32_t           ip_addr = 0;
#ifdef CONFIG_OUROBOROS_ENABLE_DNS
        uint32_t           dns_addr = 0;
#endif

        if (dst_name == NULL || src_ap_name == NULL || src_ae_name == NULL)
                return -1;

        if (strlen(dst_name) > 255
            || strlen(src_ap_name) > 255
            || strlen(src_ae_name) > 255) {
                LOG_ERR("Name too long for this shim.");
                return -1;
        }

        if (qos != NULL)
                LOG_DBGF("QoS requested. UDP/IP can't do that.");

        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        /* this socket is for the flow */
        memset((char *) &l_saddr, 0, sizeof l_saddr);
        l_saddr.sin_family      = AF_INET;
        l_saddr.sin_addr.s_addr = local_ip;
        l_saddr.sin_port        = 0;

        if (bind(fd, (struct sockaddr *) &l_saddr, sizeof l_saddr) < 0) {
                close(fd);
                return -1;
        }

#ifdef CONFIG_OUROBOROS_ENABLE_DNS
        dns_addr = shim_data(_ipcp)->dns_addr;
        if (dns_addr != 0) {
                ip_addr = ddns_resolve(dst_name, dns_addr);
                if (ip_addr == 0) {
                        LOG_DBGF("Could not resolve %s.", dst_name);
                        close(fd);
                        return -1;
                }
        } else {
#endif
                h = gethostbyname(dst_name);
                if (h == NULL) {
                        LOG_DBGF("Could not resolve %s.", dst_name);
                        close(fd);
                        return -1;
                }

                ip_addr = *((uint32_t *) (h->h_addr_list[0]));
#ifdef CONFIG_OUROBOROS_ENABLE_DNS
        }
#endif

        memset((char *) &r_saddr, 0, sizeof r_saddr);
        r_saddr.sin_family      = AF_INET;
        r_saddr.sin_addr.s_addr = ip_addr;
        r_saddr.sin_port        = LISTEN_PORT;


        /* at least try to get the packet on the wire */
        if (sendto(fd, dst_name, strlen(dst_name), 0,
                      (struct sockaddr *) &r_saddr, sizeof r_saddr) < 0) {
                LOG_ERR("Failed to send packet");
                close(fd);
                return -1;
        }

        /* wait for the other shim IPCP to respond */

        recv_buf = malloc(strlen(dst_name) + 1);
        n = sizeof(rf_saddr);
        n = recvfrom(fd,
                     recv_buf,
                     strlen(dst_name) + 1,
                     0,
                     (struct sockaddr *) &rf_saddr,
                     (unsigned *) &n);

        if (connect(fd,
                    (struct sockaddr *) &rf_saddr,
                    sizeof rf_saddr)
            < 0) {
                close(fd);
                free(recv_buf);
                return -1;
        }

        if (strcmp(recv_buf, dst_name))
                LOG_WARN("Incorrect echo from server");

        free(recv_buf);

        _ap_instance->flows[fd].port_id = port_id;
        _ap_instance->flows[fd].state   = FLOW_ALLOCATED;
        _ap_instance->flows[fd].rb      = shm_ap_rbuff_open(n_pid);
        if (_ap_instance->flows[fd].rb == NULL) {
                LOG_ERR("Could not open N + 1 ringbuffer.");
                close(fd);
                return -1;
        }

        /* tell IRMd that flow allocation "worked" */

        if (ipcp_flow_alloc_reply(getpid(), port_id, 0)) {
                LOG_ERR("Failed to notify IRMd about flow allocation reply");
                close(fd);
                shm_ap_rbuff_close(_ap_instance->flows[fd].rb);
                return -1;
        }

        FD_SET(fd, &shim_data(_ipcp)->flow_fd_s);

        pthread_cancel(_ap_instance->sdu_reader[_ap_instance->ping_pong]);
        pthread_create(&_ap_instance->sdu_reader[_ap_instance->ping_pong],
                       NULL,
                       ipcp_udp_sdu_reader,
                       NULL);
        _ap_instance->ping_pong = !_ap_instance->ping_pong;

        LOG_DBG("Allocated flow with port_id %d on UDP fd %d.", port_id, fd);

        return fd;
}

int ipcp_udp_flow_alloc_resp(int   port_id,
                             pid_t n_pid,
                             int   response)
{
        int fd = port_id_to_fd(port_id);
        if (fd < 0) {
                LOG_DBGF("Could not find flow with port_id %d.", port_id);
                return 0;
        }

        if (response)
                return 0;

        /* awaken pending flow */

        if (_ap_instance->flows[fd].state != FLOW_PENDING) {
                LOG_DBGF("Flow was not pending.");
                return -1;
        }

        _ap_instance->flows[fd].state = FLOW_ALLOCATED;
        _ap_instance->flows[fd].rb    = shm_ap_rbuff_open(n_pid);
        if (_ap_instance->flows[fd].rb == NULL) {
                LOG_ERR("Could not open N + 1 ringbuffer.");
                _ap_instance->flows[fd].state   = FLOW_NULL;
                _ap_instance->flows[fd].port_id = 0;
                return 0;
        }

        FD_SET(fd, &shim_data(_ipcp)->flow_fd_s);

        pthread_cancel(_ap_instance->sdu_reader[_ap_instance->ping_pong]);
        pthread_create(&_ap_instance->sdu_reader[_ap_instance->ping_pong],
                       NULL,
                       ipcp_udp_sdu_reader,
                       NULL);
        _ap_instance->ping_pong = !_ap_instance->ping_pong;

        LOG_DBG("Accepted flow, port_id %d on UDP fd %d.", port_id, fd);

        return 0;
}

int ipcp_udp_flow_dealloc(int port_id)
{
        int fd = port_id_to_fd(port_id);
        if (fd < 0) {
                LOG_DBGF("Could not find flow with port_id %d.", port_id);
                return 0;
        }

        _ap_instance->flows[fd].state   = FLOW_NULL;
        _ap_instance->flows[fd].port_id = 0;
        if (_ap_instance->flows[fd].rb != NULL)
                shm_ap_rbuff_close(_ap_instance->flows[fd].rb);

        FD_CLR(fd, &shim_data(_ipcp)->flow_fd_s);
        close(fd);
        return 0;
}

/* FIXME: may be crap, didn't think this one through */
int ipcp_udp_flow_dealloc_arr(int port_id)
{
        int fd = port_id_to_fd(port_id);
        if (fd < 0) {
                LOG_DBGF("Could not find flow with port_id %d.", port_id);
                return 0;
        }

        _ap_instance->flows[fd].state   = FLOW_NULL;
        _ap_instance->flows[fd].port_id = 0;
        if (_ap_instance->flows[fd].rb != NULL)
                shm_ap_rbuff_close(_ap_instance->flows[fd].rb);

        FD_CLR(fd, &shim_data(_ipcp)->flow_fd_s);

        return ipcp_flow_dealloc(0, port_id);
}

struct ipcp * ipcp_udp_create(char * ap_name)
{
        struct ipcp * i;
        struct ipcp_udp_data * data;
        struct ipcp_ops *      ops;

        if (shim_ap_init(ap_name) < 0)
                return NULL;

        i = malloc(sizeof *i);
        if (i == NULL)
                return NULL;

        data = ipcp_udp_data_create();
        if (data == NULL) {
                free(i);
                return NULL;
        }

        ops = malloc(sizeof *ops);
        if (ops == NULL) {
                free(data);
                free(i);
                return NULL;
        }

        ops->ipcp_bootstrap       = ipcp_udp_bootstrap;
        ops->ipcp_enroll          = NULL;                       /* shim */
        ops->ipcp_reg             = NULL;                       /* shim */
        ops->ipcp_unreg           = NULL;                       /* shim */
        ops->ipcp_name_reg        = ipcp_udp_name_reg;
        ops->ipcp_name_unreg      = ipcp_udp_name_unreg;
        ops->ipcp_flow_alloc      = ipcp_udp_flow_alloc;
        ops->ipcp_flow_alloc_resp = ipcp_udp_flow_alloc_resp;
        ops->ipcp_flow_dealloc    = ipcp_udp_flow_dealloc;

        i->data = (struct ipcp_data *) data;
        i->ops  = ops;

        i->state = IPCP_INIT;

        return i;
}

#ifndef MAKE_CHECK

/* FIXME: if we move _ap_instance to dev.h, we can reuse it everywhere */
/* FIXME: stop eating the CPU */
void * ipcp_udp_sdu_loop(void * o)
{
        while (true) {
                struct rb_entry * e = shm_ap_rbuff_read(_ap_instance->rb);
                int fd;
                int len = 0;
                char * buf;
                if (e == NULL)
                        continue;

                len = shm_du_map_read_sdu((uint8_t **) &buf,
                                          _ap_instance->dum,
                                          e->index);
                if (len == -1)
                        continue;

                fd = port_id_to_fd(e->port_id);

                if (fd == -1)
                        continue;

                if (len == 0)
                        continue;

                send(fd, buf, len, 0);

                shm_release_du_buff(_ap_instance->dum, e->index);
        }

        return (void *) 1;
}

int main (int argc, char * argv[])
{
        /* argument 1: pid of irmd ? */
        /* argument 2: ap name */
        struct sigaction sig_act;

        if (ipcp_arg_check(argc, argv)) {
                LOG_ERR("Wrong arguments.");
                exit(1);
        }

        /* store the process id of the irmd */
        irmd_pid = atoi(argv[1]);

        /* init sig_act */
        memset(&sig_act, 0, sizeof sig_act);

        /* install signal traps */
        sig_act.sa_sigaction = &ipcp_sig_handler;
        sig_act.sa_flags     = SA_SIGINFO;

        sigaction(SIGINT,  &sig_act, NULL);
        sigaction(SIGTERM, &sig_act, NULL);
        sigaction(SIGHUP,  &sig_act, NULL);
        sigaction(SIGPIPE, &sig_act, NULL);

        _ipcp = ipcp_udp_create(argv[2]);
        if (_ipcp == NULL) {
                LOG_ERR("Won't.");
                exit(1);
        }

        pthread_create(&_ap_instance->mainloop, NULL, ipcp_main_loop, _ipcp);
        pthread_create(&_ap_instance->sduloop, NULL, ipcp_udp_sdu_loop, NULL);

        pthread_join(_ap_instance->sduloop, NULL);
        pthread_join(_ap_instance->mainloop, NULL);
        pthread_join(_ap_instance->handler, NULL);
        pthread_join(_ap_instance->sdu_reader[0], NULL);
        pthread_join(_ap_instance->sdu_reader[1], NULL);

        ipcp_udp_destroy(_ipcp);

        shim_ap_fini();

        exit(0);
}

#endif /* MAKE_CHECK */