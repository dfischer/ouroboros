/*
 * Ouroboros - Copyright (C) 2016
 *
 * Local IPC process
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
#include <ouroboros/shm_du_map.h>
#include <ouroboros/shm_ap_rbuff.h>
#include <ouroboros/list.h>
#include <ouroboros/utils.h>
#include <ouroboros/ipcp.h>
#include <ouroboros/dif_config.h>
#include <ouroboros/sockets.h>
#include <ouroboros/bitmap.h>
#include <ouroboros/flow.h>
#include <ouroboros/dev.h>
#include <ouroboros/rw_lock.h>

#define OUROBOROS_PREFIX "ipcpd/local"

#include <ouroboros/logs.h>

#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/wait.h>
#include <fcntl.h>

#define THIS_TYPE IPCP_LOCAL

#define shim_data(type) ((struct ipcp_local_data *) type->data)

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

        int                   in_out[AP_MAX_FLOWS];

        struct flow           flows[AP_MAX_FLOWS];
        rw_lock_t             flows_lock;

        pthread_t             mainloop;
        pthread_t             sduloop;

} * _ap_instance;

static int shim_ap_init(char * ap_name)
{
        int i;

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
                shm_du_map_close(_ap_instance->dum);
                bmp_destroy(_ap_instance->fds);
                free(_ap_instance);
                return -1;
        }

        for (i = 0; i < AP_MAX_FLOWS; i ++) {
                _ap_instance->flows[i].rb = NULL;
                _ap_instance->flows[i].port_id = -1;
                _ap_instance->flows[i].state = FLOW_NULL;
                _ap_instance->in_out[i] = -1;
        }

        rw_lock_init(&_ap_instance->flows_lock);

        return 0;
}

void shim_ap_fini()
{
        int i = 0;

        if (_ap_instance == NULL)
                return;

        rw_lock_wrlock(&_ipcp->state_lock);

        if (_ipcp->state != IPCP_SHUTDOWN)
                LOG_WARN("Cleaning up AP while not in shutdown.");

        if (_ap_instance->api != NULL)
                instance_name_destroy(_ap_instance->api);
        if (_ap_instance->fds != NULL)
                bmp_destroy(_ap_instance->fds);
        if (_ap_instance->dum != NULL)
                shm_du_map_close(_ap_instance->dum);
        if (_ap_instance->rb != NULL)
                shm_ap_rbuff_destroy(_ap_instance->rb);

        rw_lock_wrlock(&_ap_instance->flows_lock);

        for (i = 0; i < AP_MAX_FLOWS; i ++)
                if (_ap_instance->flows[i].rb != NULL)
                        shm_ap_rbuff_close(_ap_instance->flows[i].rb);

        rw_lock_unlock(&_ap_instance->flows_lock);
        rw_lock_unlock(&_ipcp->state_lock);

        free(_ap_instance);
}

/* only call this under flows_lock */
static int port_id_to_fd(int port_id)
{
        int i;

        for (i = 0; i < AP_MAX_FLOWS; ++i) {
                if (_ap_instance->flows[i].port_id == port_id
                    && _ap_instance->flows[i].state != FLOW_NULL)
                        return i;
        }

        return -1;
}

/*
 * end copy from dev.c
 */

/* FIXME: if we move _ap_instance to dev.h, we can reuse it everywhere */
static void * ipcp_local_sdu_loop(void * o)
{

        while (true) {
                struct rb_entry * e;
                int fd;

                rw_lock_rdlock(&_ipcp->state_lock);

                if (_ipcp->state != IPCP_ENROLLED) {
                        rw_lock_unlock(&_ipcp->state_lock);
                        return (void *) 1; /* -ENOTENROLLED */
                }

                e = shm_ap_rbuff_read(_ap_instance->rb);
                if (e == NULL) {
                        rw_lock_unlock(&_ipcp->state_lock);
                        continue;
                }

                rw_lock_rdlock(&_ap_instance->flows_lock);
                fd = _ap_instance->in_out[port_id_to_fd(e->port_id)];
                if (fd == -1) {
                        rw_lock_unlock(&_ap_instance->flows_lock);
                        rw_lock_unlock(&_ipcp->state_lock);
                        free(e);
                        continue;
                }

                e->port_id = _ap_instance->flows[fd].port_id;

                while (shm_ap_rbuff_write(_ap_instance->flows[fd].rb, e) < 0)
                        ;
                rw_lock_unlock(&_ap_instance->flows_lock);
                rw_lock_unlock(&_ipcp->state_lock);
        }

        return (void *) 1;
}

void ipcp_sig_handler(int sig, siginfo_t * info, void * c)
{
        sigset_t  sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, SIGINT);

        switch(sig) {
        case SIGINT:
        case SIGTERM:
        case SIGHUP:
        case SIGQUIT:
                if (info->si_pid == irmd_pid) {
                        bool clean_threads = false;
                        LOG_DBG("Terminating by order of %d. Bye.",
                                info->si_pid);

                        rw_lock_wrlock(&_ipcp->state_lock);

                        if (_ipcp->state == IPCP_ENROLLED)
                                clean_threads = true;

                        _ipcp->state = IPCP_SHUTDOWN;

                        rw_lock_unlock(&_ipcp->state_lock);

                        if (clean_threads) {
                                pthread_cancel(_ap_instance->sduloop);
                                pthread_join(_ap_instance->sduloop, NULL);
                        }

                        pthread_cancel(_ap_instance->mainloop);
                }
        default:
                return;
        }
}

static int ipcp_local_bootstrap(struct dif_config * conf)
{
        if (conf->type != THIS_TYPE) {
                LOG_ERR("Config doesn't match IPCP type.");
                return -1;
        }

        if (_ipcp->state != IPCP_INIT) {
                rw_lock_unlock(&_ipcp->state_lock);
                LOG_ERR("IPCP in wrong state.");
                return -1;
        }

        rw_lock_wrlock(&_ipcp->state_lock);

        _ipcp->state = IPCP_ENROLLED;

        pthread_create(&_ap_instance->sduloop,
                       NULL,
                       ipcp_local_sdu_loop,
                       NULL);

        rw_lock_unlock(&_ipcp->state_lock);

        LOG_DBG("Bootstrapped local IPCP with pid %d.",
                getpid());

        return 0;
}

static int ipcp_local_name_reg(char * name)
{
        rw_lock_rdlock(&_ipcp->state_lock);

        if (_ipcp->state != IPCP_ENROLLED) {
                rw_lock_unlock(&_ipcp->state_lock);
                LOG_DBGF("Won't register with non-enrolled IPCP.");
                return -1; /* -ENOTENROLLED */
        }

        rw_lock_unlock(&_ipcp->state_lock);

        rw_lock_rdlock(&_ipcp->state_lock);

        if (ipcp_data_add_reg_entry(_ipcp->data, name)) {
                rw_lock_unlock(&_ipcp->state_lock);
                LOG_DBGF("Failed to add %s to local registry.", name);
                return -1;
        }

        rw_lock_unlock(&_ipcp->state_lock);

        LOG_DBG("Registered %s.", name);

        return 0;
}

static int ipcp_local_name_unreg(char * name)
{
        rw_lock_rdlock(&_ipcp->state_lock);

        ipcp_data_del_reg_entry(_ipcp->data, name);

        rw_lock_unlock(&_ipcp->state_lock);

        return 0;
}

static int ipcp_local_flow_alloc(pid_t         n_pid,
                                 int           port_id,
                                 char *        dst_name,
                                 char *        src_ap_name,
                                 char *        src_ae_name,
                                 enum qos_cube qos)
{
        int in_fd = -1;
        int out_fd = -1;

        struct shm_ap_rbuff * rb;

        LOG_INFO("Allocating flow from %s to %s.", src_ap_name, dst_name);

        if (dst_name == NULL || src_ap_name == NULL || src_ae_name == NULL)
                return -1;

        /* This ipcpd has all QoS */

        rw_lock_rdlock(&_ipcp->state_lock);

        if (_ipcp->state != IPCP_ENROLLED) {
                rw_lock_unlock(&_ipcp->state_lock);
                LOG_DBGF("Won't allocate flow with non-enrolled IPCP.");
                return -1; /* -ENOTENROLLED */
        }

        rb = shm_ap_rbuff_open(n_pid);
        if (rb == NULL)
                return -1; /* -ENORBUFF */

        rw_lock_wrlock(&_ap_instance->flows_lock);

        in_fd = bmp_allocate(_ap_instance->fds);
        if (!bmp_is_id_valid(_ap_instance->fds, in_fd)) {
                rw_lock_unlock(&_ap_instance->flows_lock);
                rw_lock_unlock(&_ipcp->state_lock);
                return -EMFILE;
        }

        _ap_instance->flows[in_fd].port_id = port_id;
        _ap_instance->flows[in_fd].state   = FLOW_PENDING;
        _ap_instance->flows[in_fd].rb      = rb;

        LOG_DBGF("Pending local flow with port_id %d.", port_id);

        /* reply to IRM */
        port_id = ipcp_flow_req_arr(getpid(),
                                    dst_name,
                                    src_ap_name,
                                    src_ae_name);

        if (port_id < 0) {
                rw_lock_unlock(&_ipcp->state_lock);
                LOG_ERR("Could not get port id from IRMd");
                /* shm_ap_rbuff_close(n_pid); */
                return -1;
        }

        out_fd = bmp_allocate(_ap_instance->fds);
        if (!bmp_is_id_valid(_ap_instance->fds, out_fd)) {
                /* shm_ap_rbuff_close(n_pid); */
                rw_lock_unlock(&_ap_instance->flows_lock);
                rw_lock_unlock(&_ipcp->state_lock);
                return -1; /* -ENOMOREFDS */
        }

        _ap_instance->flows[out_fd].port_id = port_id;
        _ap_instance->flows[out_fd].rb      = NULL;
        _ap_instance->flows[out_fd].state   = FLOW_PENDING;

        _ap_instance->in_out[in_fd]  = out_fd;
        _ap_instance->in_out[out_fd] = in_fd;

        rw_lock_unlock(&_ap_instance->flows_lock);
        rw_lock_unlock(&_ipcp->state_lock);

        LOG_DBGF("Pending local allocation request, port_id %d.", port_id);

        return 0;
}

static int ipcp_local_flow_alloc_resp(pid_t n_pid,
                                      int   port_id,
                                      int   response)
{
        struct shm_ap_rbuff * rb;
        int in_fd = -1;\
        int out_fd = -1;
        int ret = -1;

        if (response)
                return 0;

        rw_lock_rdlock(&_ipcp->state_lock);

        /* awaken pending flow */

        rw_lock_wrlock(&_ap_instance->flows_lock);

        in_fd = port_id_to_fd(port_id);
        if (in_fd < 0) {
                rw_lock_unlock(&_ap_instance->flows_lock);
                rw_lock_unlock(&_ipcp->state_lock);
                LOG_DBGF("Could not find flow with port_id %d.", port_id);
                return -1;
        }

        if (_ap_instance->flows[in_fd].state != FLOW_PENDING) {
                rw_lock_unlock(&_ap_instance->flows_lock);
                rw_lock_unlock(&_ipcp->state_lock);
                LOG_DBGF("Flow was not pending.");
                return -1;
        }

        rb = shm_ap_rbuff_open(n_pid);
        if (rb == NULL) {
                LOG_ERR("Could not open N + 1 ringbuffer.");
                _ap_instance->flows[in_fd].state   = FLOW_NULL;
                _ap_instance->flows[in_fd].port_id = -1;
                _ap_instance->in_out[in_fd] = -1;
                rw_lock_unlock(&_ap_instance->flows_lock);
                rw_lock_unlock(&_ipcp->state_lock);
                return -1;
        }

        _ap_instance->flows[in_fd].state = FLOW_ALLOCATED;
        _ap_instance->flows[in_fd].rb    = rb;

        LOG_DBGF("Accepted flow, port_id %d on fd %d.", port_id, in_fd);

        out_fd = _ap_instance->in_out[in_fd];
        if (out_fd < 0) {
                rw_lock_unlock(&_ap_instance->flows_lock);
                rw_lock_unlock(&_ipcp->state_lock);
                LOG_DBGF("No pending local flow with port_id %d.", port_id);
                return -1;
        }

        if (_ap_instance->flows[out_fd].state != FLOW_PENDING) {
                 /* FIXME: clean up other end */
                rw_lock_unlock(&_ap_instance->flows_lock);
                rw_lock_unlock(&_ipcp->state_lock);
                LOG_DBGF("Flow was not pending.");
                return -1;
        }

        _ap_instance->flows[out_fd].state = FLOW_ALLOCATED;

        rw_lock_unlock(&_ap_instance->flows_lock);
        rw_lock_unlock(&_ipcp->state_lock);

        if ((ret = ipcp_flow_alloc_reply(getpid(),
                                         _ap_instance->flows[out_fd].port_id,
                                         response)) < 0) {
                return -1; /* -EPIPE */
        }

        LOG_INFO("Flow allocation completed, port_ids (%d, %d).",
                 _ap_instance->flows[out_fd].port_id,
                 _ap_instance->flows[in_fd].port_id);

        return ret;
}

static int ipcp_local_flow_dealloc(int port_id)
{
        int fd = -1;
        struct shm_ap_rbuff * rb;

        rw_lock_rdlock(&_ipcp->state_lock);
        rw_lock_wrlock(&_ap_instance->flows_lock);

        fd = port_id_to_fd(port_id);
        if (fd < 0) {
                rw_lock_unlock(&_ap_instance->flows_lock);
                rw_lock_unlock(&_ipcp->state_lock);
                LOG_DBGF("Could not find flow with port_id %d.", port_id);
                return 0;
        }

        bmp_release(_ap_instance->fds, fd);

        _ap_instance->in_out[_ap_instance->in_out[fd]] = -1;
        _ap_instance->in_out[fd] = -1;

        _ap_instance->flows[fd].state   = FLOW_NULL;
        _ap_instance->flows[fd].port_id = -1;
        rb = _ap_instance->flows[fd].rb;
        _ap_instance->flows[fd].rb      = NULL;

        rw_lock_unlock(&_ap_instance->flows_lock);

        if (rb != NULL)
                shm_ap_rbuff_close(rb);

        rw_lock_unlock(&_ipcp->state_lock);

        LOG_DBGF("Flow with port_id %d deallocated.", port_id);

        return 0;
}

static struct ipcp * ipcp_local_create()
{
        struct ipcp * i;
        struct ipcp_ops *  ops;

        i = ipcp_instance_create();
        if (i == NULL)
                return NULL;

        i->data = ipcp_data_create();
        if (i->data == NULL) {
                free(i);
                return NULL;
        }

        if (ipcp_data_init(i->data, THIS_TYPE) == NULL) {
                free(i->data);
                free(i);
                return NULL;
        }

        ops = malloc(sizeof(*ops));
        if (ops == NULL) {
                free(i->data);
                free(i);
                return NULL;
        }

        ops->ipcp_bootstrap       = ipcp_local_bootstrap;
        ops->ipcp_enroll          = NULL;                       /* shim */
        ops->ipcp_reg             = NULL;                       /* shim */
        ops->ipcp_unreg           = NULL;                       /* shim */
        ops->ipcp_name_reg        = ipcp_local_name_reg;
        ops->ipcp_name_unreg      = ipcp_local_name_unreg;
        ops->ipcp_flow_alloc      = ipcp_local_flow_alloc;
        ops->ipcp_flow_alloc_resp = ipcp_local_flow_alloc_resp;
        ops->ipcp_flow_dealloc    = ipcp_local_flow_dealloc;

        i->ops = ops;

        i->state = IPCP_INIT;

        return i;
}

#ifndef MAKE_CHECK

int main (int argc, char * argv[])
{
        /* argument 1: pid of irmd ? */
        /* argument 2: ap name */
        struct sigaction sig_act;
        sigset_t  sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, SIGINT);
        sigaddset(&sigset, SIGQUIT);
        sigaddset(&sigset, SIGHUP);
        sigaddset(&sigset, SIGPIPE);

        if (ipcp_arg_check(argc, argv)) {
                LOG_ERR("Wrong arguments.");
                exit(1);
        }

        if (shim_ap_init(argv[2]) < 0)
                exit(1);

        /* store the process id of the irmd */
        irmd_pid = atoi(argv[1]);

        /* init sig_act */
        memset(&sig_act, 0, sizeof(sig_act));

        /* install signal traps */
        sig_act.sa_sigaction = &ipcp_sig_handler;
        sig_act.sa_flags     = SA_SIGINFO;

        sigaction(SIGINT,  &sig_act, NULL);
        sigaction(SIGTERM, &sig_act, NULL);
        sigaction(SIGHUP,  &sig_act, NULL);
        sigaction(SIGPIPE, &sig_act, NULL);

        _ipcp = ipcp_local_create();
        if (_ipcp == NULL) {
                LOG_ERR("Won't.");
                exit(1);
        }

        rw_lock_wrlock(&_ipcp->state_lock);

        pthread_sigmask(SIG_BLOCK, &sigset, NULL);

        pthread_create(&_ap_instance->mainloop, NULL, ipcp_main_loop, _ipcp);

        pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);

        rw_lock_unlock(&_ipcp->state_lock);

        pthread_join(_ap_instance->mainloop, NULL);

        shim_ap_fini();

        free(_ipcp->data);
        free(_ipcp->ops);
        free(_ipcp);

        exit(EXIT_SUCCESS);
}

#endif /* MAKE_CHECK */