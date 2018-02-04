/*
 * Ouroboros - Copyright (C) 2016 - 2018
 *
 * The IPC Resource Manager
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

#define _POSIX_C_SOURCE 200812L
#define __XSI_VISIBLE   500

#include "config.h"

#define OUROBOROS_PREFIX "irmd"

#include <ouroboros/hash.h>
#include <ouroboros/errno.h>
#include <ouroboros/sockets.h>
#include <ouroboros/list.h>
#include <ouroboros/utils.h>
#include <ouroboros/irm.h>
#include <ouroboros/lockfile.h>
#include <ouroboros/shm_flow_set.h>
#include <ouroboros/shm_rbuff.h>
#include <ouroboros/shm_rdrbuff.h>
#include <ouroboros/bitmap.h>
#include <ouroboros/qos.h>
#include <ouroboros/time_utils.h>
#include <ouroboros/tpm.h>
#include <ouroboros/logs.h>
#include <ouroboros/version.h>

#include "utils.h"
#include "registry.h"
#include "irm_flow.h"
#include "proc_table.h"
#include "ipcp.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>

#ifdef HAVE_LIBGCRYPT
#include <gcrypt.h>
#endif

#define IRMD_CLEANUP_TIMER ((IRMD_FLOW_TIMEOUT / 20) * MILLION) /* ns */
#define SHM_SAN_HOLDOFF 1000 /* ms */
#define IPCP_HASH_LEN(e) hash_len(e->dir_hash_algo)
#define IB_LEN IRM_MSG_BUF_SIZE

enum init_state {
        IPCP_NULL = 0,
        IPCP_BOOT,
        IPCP_LIVE
};

struct ipcp_entry {
        struct list_head next;

        char *           name;
        pid_t            pid;
        enum ipcp_type   type;
        enum hash_algo   dir_hash_algo;
        char *           dif_name;

        enum init_state  init_state;
        pthread_cond_t   init_cond;
        pthread_mutex_t  init_lock;
};

enum irm_state {
        IRMD_NULL = 0,
        IRMD_RUNNING
};

struct cmd {
        struct list_head next;

        uint8_t          cbuf[IB_LEN];
        size_t           len;
        int              fd;
};

struct {
        struct list_head     registry;     /* registered names known     */

        struct list_head     ipcps;        /* list of ipcps in system    */

        struct list_head     proc_table;   /* processes                  */
        struct list_head     prog_table;   /* programs known             */
        struct list_head     spawned_pids; /* child processes            */
        pthread_rwlock_t     reg_lock;     /* lock for registration info */

        struct bmp *         port_ids;     /* port_ids for flows         */
        struct list_head     irm_flows;    /* flow information           */
        pthread_rwlock_t     flows_lock;   /* lock for flows             */

        struct lockfile *    lf;           /* single irmd per system     */
        struct shm_rdrbuff * rdrb;         /* rdrbuff for SDUs           */

        int                  sockfd;       /* UNIX socket                */

        struct list_head     cmds;         /* pending commands           */
        pthread_cond_t       cmd_cond;     /* cmd signal condvar         */
        pthread_mutex_t      cmd_lock;     /* cmd signal lock            */

        enum irm_state       state;        /* state of the irmd          */
        pthread_rwlock_t     state_lock;   /* lock for the entire irmd   */

        struct tpm *         tpm;          /* thread pool manager        */

        pthread_t            irm_sanitize; /* clean up irmd resources    */
        pthread_t            shm_sanitize; /* keep track of rdrbuff use  */
        pthread_t            acceptor;     /* accept new commands        */
} irmd;

static enum irm_state irmd_get_state(void)
{
        enum irm_state state;

        pthread_rwlock_rdlock(&irmd.state_lock);

        state = irmd.state;

        pthread_rwlock_unlock(&irmd.state_lock);

        return state;
}

static void irmd_set_state(enum irm_state state)
{
        pthread_rwlock_wrlock(&irmd.state_lock);

        irmd.state = state;

        pthread_rwlock_unlock(&irmd.state_lock);
}

static void clear_irm_flow(struct irm_flow * f) {
        ssize_t idx;

        assert(f);

        while ((idx = shm_rbuff_read(f->n_rb)) >= 0)
                shm_rdrbuff_remove(irmd.rdrb, idx);

        while ((idx = shm_rbuff_read(f->n_1_rb)) >= 0)
                shm_rdrbuff_remove(irmd.rdrb, idx);
}

static struct irm_flow * get_irm_flow(int port_id)
{
        struct list_head * pos = NULL;

        list_for_each(pos, &irmd.irm_flows) {
                struct irm_flow * e = list_entry(pos, struct irm_flow, next);
                if (e->port_id == port_id)
                        return e;
        }

        return NULL;
}

static struct irm_flow * get_irm_flow_n(pid_t n_pid)
{
        struct list_head * pos = NULL;

        list_for_each(pos, &irmd.irm_flows) {
                struct irm_flow * e = list_entry(pos, struct irm_flow, next);
                if (e->n_pid == n_pid &&
                    irm_flow_get_state(e) == FLOW_ALLOC_PENDING)
                        return e;
        }

        return NULL;
}

static struct ipcp_entry * ipcp_entry_create(void)
{
        struct ipcp_entry * e = malloc(sizeof(*e));
        if (e == NULL)
                return NULL;

        e->name = NULL;
        e->dif_name = NULL;

        list_head_init(&e->next);

        return e;
}

static void ipcp_entry_destroy(struct ipcp_entry * e)
{
        assert(e);

        pthread_mutex_lock(&e->init_lock);

        while (e->init_state == IPCP_BOOT)
                pthread_cond_wait(&e->init_cond, &e->init_lock);

        pthread_mutex_unlock(&e->init_lock);

        if (e->name != NULL)
                free(e->name);

        if (e->dif_name != NULL)
                free(e->dif_name);

        free(e);
}

static struct ipcp_entry * get_ipcp_entry_by_pid(pid_t pid)
{
        struct list_head * p = NULL;

        list_for_each(p, &irmd.ipcps) {
                struct ipcp_entry * e = list_entry(p, struct ipcp_entry, next);
                if (pid == e->pid)
                        return e;
        }

        return NULL;
}

static struct ipcp_entry * get_ipcp_entry_by_name(const char * name)
{
        struct list_head * p = NULL;

        list_for_each(p, &irmd.ipcps) {
                struct ipcp_entry * e = list_entry(p, struct ipcp_entry, next);
                if (strcmp(name, e->name) == 0)
                        return e;
        }

        return NULL;
}

static struct ipcp_entry * get_ipcp_by_dst_name(const char * name,
                                                pid_t        src)
{
        struct list_head * p;
        struct list_head * h;
        uint8_t *          hash;
        pid_t              pid;

        pthread_rwlock_rdlock(&irmd.reg_lock);

        list_for_each_safe(p, h, &irmd.ipcps) {
                struct ipcp_entry * e = list_entry(p, struct ipcp_entry, next);
                if (e->dif_name == NULL || e->pid == src)
                        continue;

                hash = malloc(IPCP_HASH_LEN(e));
                if  (hash == NULL)
                        return NULL;

                str_hash(e->dir_hash_algo, hash, name);

                pid = e->pid;

                pthread_rwlock_unlock(&irmd.reg_lock);

                if (ipcp_query(pid, hash, IPCP_HASH_LEN(e)) == 0) {
                        free(hash);
                        return e;
                }

                free(hash);

                pthread_rwlock_rdlock(&irmd.reg_lock);
        }

        pthread_rwlock_unlock(&irmd.reg_lock);

        return NULL;
}

static pid_t create_ipcp(char *         name,
                         enum ipcp_type ipcp_type)
{
        struct pid_el *     ppid  = NULL;
        struct ipcp_entry * tmp   = NULL;
        struct list_head *  p     = NULL;
        struct ipcp_entry * entry = NULL;
        int                 ret   = 0;
        pthread_condattr_t  cattr;
        struct timespec     dl;
        struct timespec     to = {SOCKET_TIMEOUT / 1000,
                                  (SOCKET_TIMEOUT % 1000) * MILLION};
        pid_t               ipcp_pid;

        ppid = malloc(sizeof(*ppid));
        if (ppid == NULL)
                return -ENOMEM;

        pthread_rwlock_wrlock(&irmd.reg_lock);

        entry = get_ipcp_entry_by_name(name);
        if (entry != NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                free(ppid);
                log_err("IPCP by that name already exists.");
                return -1;
        }

        ppid->pid = ipcp_create(name, ipcp_type);
        if (ppid->pid == -1) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                free(ppid);
                log_err("Failed to create IPCP.");
                return -1;
        }

        tmp = ipcp_entry_create();
        if (tmp == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                free(ppid);
                return -1;
        }

        list_head_init(&tmp->next);

        tmp->name = strdup(name);
        if (tmp->name == NULL) {
                ipcp_entry_destroy(tmp);
                pthread_rwlock_unlock(&irmd.reg_lock);
                free(ppid);
                return -1;
        }

        pthread_condattr_init(&cattr);
#ifndef __APPLE__
        pthread_condattr_setclock(&cattr, PTHREAD_COND_CLOCK);
#endif

        pthread_cond_init(&tmp->init_cond, &cattr);

        pthread_condattr_destroy(&cattr);

        pthread_mutex_init(&tmp->init_lock, NULL);

        tmp->pid           = ppid->pid;
        tmp->dif_name      = NULL;
        tmp->type          = ipcp_type;
        tmp->init_state    = IPCP_BOOT;
        tmp->dir_hash_algo = -1;
        ipcp_pid           = tmp->pid;

        list_for_each(p, &irmd.ipcps) {
                struct ipcp_entry * e = list_entry(p, struct ipcp_entry, next);
                if (e->type > ipcp_type)
                        break;
        }

        list_add_tail(&tmp->next, p);

        list_add(&ppid->next, &irmd.spawned_pids);

        pthread_rwlock_unlock(&irmd.reg_lock);

        pthread_mutex_lock(&tmp->init_lock);

        clock_gettime(PTHREAD_COND_CLOCK, &dl);
        ts_add(&dl, &to, &dl);

        while (tmp->init_state == IPCP_BOOT && ret != -ETIMEDOUT)
                ret = -pthread_cond_timedwait(&tmp->init_cond,
                                              &tmp->init_lock,
                                              &dl);

        if (ret == -ETIMEDOUT) {
                kill(tmp->pid, SIGKILL);
                tmp->init_state = IPCP_NULL;
                pthread_cond_signal(&tmp->init_cond);
                pthread_mutex_unlock(&tmp->init_lock);
                log_err("IPCP %d failed to respond.", ipcp_pid);
                return -1;
        }

        pthread_mutex_unlock(&tmp->init_lock);

        log_info("Created IPCP %d.", ipcp_pid);

        return ipcp_pid;
}

static int create_ipcp_r(pid_t pid,
                         int   result)
{
        struct list_head * pos = NULL;

        if (result != 0)
                return result;

        pthread_rwlock_rdlock(&irmd.reg_lock);

        list_for_each(pos, &irmd.ipcps) {
                struct ipcp_entry * e =
                        list_entry(pos, struct ipcp_entry, next);

                if (e->pid == pid) {
                        pthread_mutex_lock(&e->init_lock);
                        e->init_state = IPCP_LIVE;
                        pthread_cond_broadcast(&e->init_cond);
                        pthread_mutex_unlock(&e->init_lock);
                }
        }

        pthread_rwlock_unlock(&irmd.reg_lock);

        return 0;
}

static void clear_spawned_process(pid_t pid)
{
        struct list_head * pos = NULL;
        struct list_head * n   = NULL;

        list_for_each_safe(pos, n, &(irmd.spawned_pids)) {
                struct pid_el * a = list_entry(pos, struct pid_el, next);
                if (pid == a->pid) {
                        list_del(&a->next);
                        free(a);
                }
        }
}

static int destroy_ipcp(pid_t pid)
{
        struct list_head * pos = NULL;
        struct list_head * n   = NULL;

        pthread_rwlock_wrlock(&irmd.reg_lock);

        list_for_each_safe(pos, n, &(irmd.ipcps)) {
                struct ipcp_entry * tmp =
                        list_entry(pos, struct ipcp_entry, next);

                if (pid == tmp->pid) {
                        clear_spawned_process(pid);
                        if (ipcp_destroy(pid))
                                log_err("Could not destroy IPCP.");
                        list_del(&tmp->next);
                        ipcp_entry_destroy(tmp);

                        log_info("Destroyed IPCP %d.", pid);
                }
        }

        pthread_rwlock_unlock(&irmd.reg_lock);

        return 0;
}

static int bootstrap_ipcp(pid_t               pid,
                          ipcp_config_msg_t * conf)
{
        struct ipcp_entry * entry = NULL;
        struct dif_info     info;

        pthread_rwlock_wrlock(&irmd.reg_lock);

        entry = get_ipcp_entry_by_pid(pid);
        if (entry == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("No such IPCP.");
                return -1;
        }

        if (entry->type != (enum ipcp_type) conf->ipcp_type) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("Configuration does not match IPCP type.");
                return -1;
        }

        if (ipcp_bootstrap(entry->pid, conf, &info)) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("Could not bootstrap IPCP.");
                return -1;
        }

        entry->dif_name = strdup(info.dif_name);
        if (entry->dif_name == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_warn("Failed to set name of layer.");
                return -ENOMEM;
        }

        entry->dir_hash_algo = info.dir_hash_algo;

        pthread_rwlock_unlock(&irmd.reg_lock);

        log_info("Bootstrapped IPCP %d in layer %s.",
                 pid, conf->dif_info->dif_name);

        return 0;
}

static int enroll_ipcp(pid_t  pid,
                       char * dst_name)
{
        struct ipcp_entry * entry = NULL;
        struct dif_info     info;

        pthread_rwlock_wrlock(&irmd.reg_lock);

        entry = get_ipcp_entry_by_pid(pid);
        if (entry == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("No such IPCP.");
                return -1;
        }

        if (entry->dif_name != NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("IPCP in wrong state");
                return -1;
        }

        pthread_rwlock_unlock(&irmd.reg_lock);

        if (ipcp_enroll(pid, dst_name, &info) < 0) {
                log_err("Could not enroll IPCP %d.", pid);
                return -1;
        }

        pthread_rwlock_wrlock(&irmd.reg_lock);

        entry = get_ipcp_entry_by_pid(pid);
        if (entry == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("No such IPCP.");
                return -1;
        }

        entry->dif_name = strdup(info.dif_name);
        if (entry->dif_name == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("Failed to strdup dif_name.");
                return -ENOMEM;
        }

        entry->dir_hash_algo = info.dir_hash_algo;

        pthread_rwlock_unlock(&irmd.reg_lock);

        log_info("Enrolled IPCP %d in layer %s.",
                 pid, info.dif_name);

        return 0;
}

static int connect_ipcp(pid_t        pid,
                        const char * dst,
                        const char * component)
{
        struct ipcp_entry * entry = NULL;

        pthread_rwlock_rdlock(&irmd.reg_lock);

        entry = get_ipcp_entry_by_pid(pid);
        if (entry == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("No such IPCP.");
                return -EIPCP;
        }

        if (entry->type != IPCP_NORMAL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("Cannot establish connections for this IPCP type.");
                return -EIPCP;
        }

        pthread_rwlock_unlock(&irmd.reg_lock);

        log_dbg("Connecting %s to %s.", component, dst);

        if (ipcp_connect(pid, dst, component)) {
                log_err("Could not connect IPCP.");
                return -EPERM;
        }

        log_info("Established %s connection between IPCP %d and %s.",
                 component, pid, dst);

        return 0;
}

static int disconnect_ipcp(pid_t        pid,
                           const char * dst,
                           const char * component)
{
        struct ipcp_entry * entry = NULL;

        pthread_rwlock_rdlock(&irmd.reg_lock);

        entry = get_ipcp_entry_by_pid(pid);
        if (entry == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("No such IPCP.");
                return -EIPCP;
        }

        if (entry->type != IPCP_NORMAL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("Cannot tear down connections for this IPCP type.");
                return -EIPCP;
        }

        pthread_rwlock_unlock(&irmd.reg_lock);

        if (ipcp_disconnect(pid, dst, component)) {
                log_err("Could not disconnect IPCP.");
                return -EPERM;
        }

        log_info("%s connection between IPCP %d and %s torn down.",
                 component, pid, dst);

        return 0;
}

static int bind_program(char *   prog,
                        char *   name,
                        uint16_t flags,
                        int      argc,
                        char **  argv)
{
        char * progs;
        char * progn;
        char ** argv_dup = NULL;
        int i;
        char * name_dup = NULL;
        struct prog_entry * e = NULL;
        struct reg_entry * re = NULL;

        if (prog == NULL || name == NULL)
                return -EINVAL;

        pthread_rwlock_wrlock(&irmd.reg_lock);

        e = prog_table_get(&irmd.prog_table, path_strip(prog));

        if (e == NULL) {
                progs = strdup(path_strip(prog));
                if (progs == NULL) {
                        pthread_rwlock_unlock(&irmd.reg_lock);
                        return -ENOMEM;
                }

                progn = strdup(name);
                if (progn == NULL) {
                        pthread_rwlock_unlock(&irmd.reg_lock);
                        free(progs);
                        return -ENOMEM;
                }

                if ((flags & BIND_AUTO) && argc) {
                /* We need to duplicate argv and set argv[0] to prog. */
                        argv_dup = malloc((argc + 2) * sizeof(*argv_dup));
                        argv_dup[0] = strdup(prog);
                        for (i = 1; i <= argc; ++i) {
                                argv_dup[i] = strdup(argv[i - 1]);
                                if (argv_dup[i] == NULL) {
                                        pthread_rwlock_unlock(&irmd.reg_lock);
                                        argvfree(argv_dup);
                                        log_err("Failed to bind program %s to %s.",
                                                prog, name);
                                        free(progs);
                                        free(progn);
                                        return -ENOMEM;
                                }
                        }
                        argv_dup[argc + 1] = NULL;
                }
                e = prog_entry_create(progn, progs, flags, argv_dup);
                if (e == NULL) {
                        pthread_rwlock_unlock(&irmd.reg_lock);
                        free(progs);
                        free(progn);
                        argvfree(argv_dup);
                        return -ENOMEM;
                }

                prog_table_add(&irmd.prog_table, e);

        }

        name_dup = strdup(name);
        if (name_dup == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                return -ENOMEM;
        }

        if (prog_entry_add_name(e, name_dup)) {
                log_err("Failed adding name.");
                pthread_rwlock_unlock(&irmd.reg_lock);
                free(name_dup);
                return -ENOMEM;
        }

        re = registry_get_entry(&irmd.registry, name);
        if (re != NULL && reg_entry_add_prog(re, e) < 0)
                log_err("Failed adding program %s for name %s.", prog, name);

        pthread_rwlock_unlock(&irmd.reg_lock);

        log_info("Bound program %s to name %s.", prog, name);

        return 0;
}

static int bind_process(pid_t  pid,
                        char * name)
{
        char * name_dup = NULL;
        struct proc_entry * e = NULL;
        struct reg_entry * re = NULL;

        if (name == NULL)
                return -EINVAL;

        pthread_rwlock_wrlock(&irmd.reg_lock);

        e = proc_table_get(&irmd.proc_table, pid);
        if (e == NULL) {
                log_err("Process %d does not exist.", pid);
                pthread_rwlock_unlock(&irmd.reg_lock);
                return -1;
        }

        name_dup = strdup(name);
        if (name_dup == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                return -ENOMEM;
        }

        if (proc_entry_add_name(e, name_dup)) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("Failed to add name %s to process %d.", name, pid);
                free(name_dup);
                return -1;
        }

        re = registry_get_entry(&irmd.registry, name);
        if (re != NULL && reg_entry_add_pid(re, pid) < 0)
                log_err("Failed adding process %d for name %s.", pid, name);

        pthread_rwlock_unlock(&irmd.reg_lock);

        log_info("Bound process %d to name %s.", pid, name);

        return 0;
}

static int unbind_program(char * prog,
                          char * name)
{
        struct reg_entry * e;

        if (prog == NULL)
                return -EINVAL;

        pthread_rwlock_wrlock(&irmd.reg_lock);

        if (name == NULL)
                prog_table_del(&irmd.prog_table, prog);
        else {
                struct prog_entry * e = prog_table_get(&irmd.prog_table, prog);
                prog_entry_del_name(e, name);
        }

        e = registry_get_entry(&irmd.registry, name);
        if (e != NULL)
                reg_entry_del_prog(e, prog);

        pthread_rwlock_unlock(&irmd.reg_lock);

        if (name == NULL)
                log_info("Program %s unbound.", prog);
        else
                log_info("All names matching %s unbound for %s.", name, prog);

        return 0;
}

static int unbind_process(pid_t        pid,
                          const char * name)
{
        struct reg_entry * e;

        pthread_rwlock_wrlock(&irmd.reg_lock);

        if (name == NULL)
                proc_table_del(&irmd.proc_table, pid);
        else {
                struct proc_entry * e = proc_table_get(&irmd.proc_table, pid);
                proc_entry_del_name(e, name);
        }

        e = registry_get_entry(&irmd.registry, name);
        if (e != NULL)
                reg_entry_del_pid(e, pid);

        pthread_rwlock_unlock(&irmd.reg_lock);

        if (name == NULL)
                log_info("Process %d unbound.", pid);
        else
                log_info("All names matching %s unbound for %d.", name, pid);

        return 0;
}

static ssize_t list_ipcps(char *   name,
                          pid_t ** pids)
{
        struct list_head * pos = NULL;
        size_t count = 0;
        int i = 0;

        pthread_rwlock_rdlock(&irmd.reg_lock);

        list_for_each(pos, &irmd.ipcps) {
                struct ipcp_entry * tmp =
                        list_entry(pos, struct ipcp_entry, next);
                if (wildcard_match(name, tmp->name) == 0)
                        count++;
        }

        if (count == 0) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                return 0;
        }

        *pids = malloc(count * sizeof(**pids));
        if (*pids == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                return -1;
        }

        list_for_each(pos, &irmd.ipcps) {
                struct ipcp_entry * tmp =
                        list_entry(pos, struct ipcp_entry, next);
                if (wildcard_match(name, tmp->name) == 0)
                        (*pids)[i++] = tmp->pid;
        }

        pthread_rwlock_unlock(&irmd.reg_lock);

        return count;
}

static int name_reg(const char *  name,
                    char **       difs,
                    size_t        len)
{
        size_t i;
        int ret = 0;
        struct list_head * p = NULL;

        assert(name);
        assert(len);
        assert(difs);
        assert(difs[0]);

        pthread_rwlock_wrlock(&irmd.reg_lock);

        if (list_is_empty(&irmd.ipcps)) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                return -1;
        }

        if (!registry_has_name(&irmd.registry, name)) {
                struct reg_entry * re =
                        registry_add_name(&irmd.registry, name);
                if (re == NULL) {
                        log_err("Failed creating registry entry for %s.", name);
                        pthread_rwlock_unlock(&irmd.reg_lock);
                        return -1;
                }

                /* check the tables for client programs */
                list_for_each(p, &irmd.proc_table) {
                        struct list_head * q;
                        struct proc_entry * e =
                                list_entry(p, struct proc_entry, next);
                        list_for_each(q, &e->names) {
                                struct str_el * s =
                                        list_entry(q, struct str_el, next);
                                if (!strcmp(s->str, name))
                                        reg_entry_add_pid(re, e->pid);
                        }
                }

                list_for_each(p, &irmd.prog_table) {
                        struct list_head * q;
                        struct prog_entry * e =
                                list_entry(p, struct prog_entry, next);
                        list_for_each(q, &e->names) {
                                struct str_el * s =
                                        list_entry(q, struct str_el, next);
                                if (!strcmp(s->str, name))
                                        reg_entry_add_prog(re, e);
                        }
                }
        }

        list_for_each(p, &irmd.ipcps) {
                struct ipcp_entry * e = list_entry(p, struct ipcp_entry, next);
                if (e->dif_name == NULL)
                        continue;

                for (i = 0; i < len; ++i) {
                        uint8_t * hash;
                        pid_t     pid;
                        size_t    len;

                        if (wildcard_match(difs[i], e->dif_name))
                                continue;

                        hash = malloc(IPCP_HASH_LEN(e));
                        if (hash == NULL)
                                break;

                        str_hash(e->dir_hash_algo, hash, name);

                        pid = e->pid;
                        len = IPCP_HASH_LEN(e);

                        pthread_rwlock_unlock(&irmd.reg_lock);

                        if (ipcp_reg(pid, hash, len)) {
                                log_err("Could not register " HASH_FMT
                                        " with IPCP %d.",
                                        HASH_VAL(hash), pid);
                                pthread_rwlock_wrlock(&irmd.reg_lock);
                                free(hash);
                                break;
                        }

                        pthread_rwlock_wrlock(&irmd.reg_lock);

                        if (registry_add_name_to_dif(&irmd.registry,
                                                     name,
                                                     e->dif_name,
                                                     e->type) < 0)
                                log_warn("Registered unbound name %s. "
                                         "Registry may be corrupt.",
                                         name);
                        log_info("Registered %s in %s as " HASH_FMT ".",
                                 name, e->dif_name, HASH_VAL(hash));
                        ++ret;

                        free(hash);
                }
        }

        pthread_rwlock_unlock(&irmd.reg_lock);

        return (ret > 0 ? 0 : -1);
}

static int name_unreg(const char *  name,
                      char **       difs,
                      size_t        len)
{
        size_t i;
        int ret = 0;
        struct list_head * pos = NULL;

        assert(name);
        assert(len);
        assert(difs);
        assert(difs[0]);

        pthread_rwlock_wrlock(&irmd.reg_lock);

        list_for_each(pos, &irmd.ipcps) {
                struct ipcp_entry * e =
                        list_entry(pos, struct ipcp_entry, next);

                if (e->dif_name == NULL)
                        continue;

                for (i = 0; i < len; ++i) {
                        uint8_t * hash;
                        pid_t     pid;
                        size_t    len;

                        if (wildcard_match(difs[i], e->dif_name))
                                continue;

                        hash = malloc(IPCP_HASH_LEN(e));
                        if  (hash == NULL)
                                break;

                        str_hash(e->dir_hash_algo, hash, name);

                        pid = e->pid;
                        len = IPCP_HASH_LEN(e);

                        pthread_rwlock_unlock(&irmd.reg_lock);

                        if (ipcp_unreg(pid, hash, len)) {
                                log_err("Could not unregister %s with IPCP %d.",
                                        name, pid);
                                pthread_rwlock_wrlock(&irmd.reg_lock);
                                free(hash);
                                break;
                        }

                        pthread_rwlock_wrlock(&irmd.reg_lock);

                        registry_del_name_from_dif(&irmd.registry,
                                                   name,
                                                   e->dif_name);
                        log_info("Unregistered %s from %s.",
                                 name, e->dif_name);
                        ++ret;

                        free(hash);
                }
        }

        pthread_rwlock_unlock(&irmd.reg_lock);

        return (ret > 0 ? 0 : -1);
}

static int proc_announce(pid_t  pid,
                         char * prog)
{
        struct proc_entry * e = NULL;
        struct prog_entry * a = NULL;
        char * prog_dup;
        if (prog == NULL)
                return -EINVAL;

        prog_dup = strdup(prog);
        if (prog_dup == NULL) {
                return -ENOMEM;
        }

        e = proc_entry_create(pid, prog_dup);
        if (e == NULL) {
                return -ENOMEM;
        }

        pthread_rwlock_wrlock(&irmd.reg_lock);

        proc_table_add(&irmd.proc_table, e);

        /* Copy listen names from program if it exists. */

        a = prog_table_get(&irmd.prog_table, e->prog);
        if (a != NULL) {
                struct list_head * p;
                list_for_each(p, &a->names) {
                        struct str_el * s = list_entry(p, struct str_el, next);
                        struct str_el * n = malloc(sizeof(*n));
                        if (n == NULL) {
                                pthread_rwlock_unlock(&irmd.reg_lock);
                                return -ENOMEM;
                        }

                        n->str = strdup(s->str);
                        if (n->str == NULL) {
                                pthread_rwlock_unlock(&irmd.reg_lock);
                                free(n);
                                return -ENOMEM;
                        }

                        list_add(&n->next, &e->names);
                        log_dbg("Process %d inherits name %s from program %s.",
                                pid, n->str, e->prog);
                }
        }

        pthread_rwlock_unlock(&irmd.reg_lock);

        return 0;
}

static int flow_accept(pid_t              pid,
                       struct timespec *  timeo,
                       struct irm_flow ** fl)
{
        struct irm_flow  *  f  = NULL;
        struct proc_entry * e  = NULL;
        struct reg_entry *  re = NULL;
        struct list_head *  p  = NULL;

        pid_t pid_n1;
        pid_t pid_n;
        int   port_id;
        int   ret;

        pthread_rwlock_wrlock(&irmd.reg_lock);

        e = proc_table_get(&irmd.proc_table, pid);
        if (e == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("Unknown process %d calling accept.", pid);
                return -EINVAL;
        }

        log_dbg("New instance (%d) of %s added.", pid, e->prog);
        log_dbg("This process accepts flows for:");

        list_for_each(p, &e->names) {
                struct str_el * s = list_entry(p, struct str_el, next);
                log_dbg("        %s", s->str);
                re = registry_get_entry(&irmd.registry, s->str);
                if (re != NULL)
                        reg_entry_add_pid(re, pid);
        }

        pthread_rwlock_unlock(&irmd.reg_lock);

        ret = proc_entry_sleep(e, timeo);
        if (ret == -ETIMEDOUT)
                return -ETIMEDOUT;

        if (ret == -1)
                return -EPIPE;

        if (irmd_get_state() != IRMD_RUNNING) {
                reg_entry_set_state(re, REG_NAME_NULL);
                return -EIRMD;
        }

        pthread_rwlock_rdlock(&irmd.flows_lock);

        f = get_irm_flow_n(pid);
        if (f == NULL) {
                pthread_rwlock_unlock(&irmd.flows_lock);
                log_warn("Port_id was not created yet.");
                return -EPERM;
        }

        pid_n   = f->n_pid;
        pid_n1  = f->n_1_pid;
        port_id = f->port_id;

        pthread_rwlock_unlock(&irmd.flows_lock);
        pthread_rwlock_rdlock(&irmd.reg_lock);

        e = proc_table_get(&irmd.proc_table, pid);
        if (e == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                pthread_rwlock_wrlock(&irmd.flows_lock);
                list_del(&f->next);
                bmp_release(irmd.port_ids, f->port_id);
                pthread_rwlock_unlock(&irmd.flows_lock);
                ipcp_flow_alloc_resp(pid_n1, port_id, pid_n, -1);
                clear_irm_flow(f);
                irm_flow_set_state(f, FLOW_NULL);
                irm_flow_destroy(f);
                log_dbg("Process gone while accepting flow.");
                return -EPERM;
        }

        pthread_mutex_lock(&e->lock);

        re = e->re;

        pthread_mutex_unlock(&e->lock);

        if (reg_entry_get_state(re) != REG_NAME_FLOW_ARRIVED) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                pthread_rwlock_wrlock(&irmd.flows_lock);
                list_del(&f->next);
                bmp_release(irmd.port_ids, f->port_id);
                pthread_rwlock_unlock(&irmd.flows_lock);
                ipcp_flow_alloc_resp(pid_n1, port_id, pid_n, -1);
                clear_irm_flow(f);
                irm_flow_set_state(f, FLOW_NULL);
                irm_flow_destroy(f);
                log_err("Entry in wrong state.");
                return -EPERM;
        }

        registry_del_process(&irmd.registry, pid);

        pthread_rwlock_unlock(&irmd.reg_lock);

        if (ipcp_flow_alloc_resp(pid_n1, port_id, pid_n, 0)) {
                pthread_rwlock_wrlock(&irmd.flows_lock);
                list_del(&f->next);
                pthread_rwlock_unlock(&irmd.flows_lock);
                log_dbg("Failed to respond to alloc. Port_id invalidated.");
                clear_irm_flow(f);
                irm_flow_set_state(f, FLOW_NULL);
                irm_flow_destroy(f);
                return -EPERM;
        }

        irm_flow_set_state(f, FLOW_ALLOCATED);

        log_info("Flow on port_id %d allocated.", f->port_id);

        *fl = f;

        return 0;
}

static int flow_alloc(pid_t              pid,
                      const char *       dst,
                      qoscube_t          cube,
                      struct timespec *  timeo,
                      struct irm_flow ** e)
{
        struct irm_flow *   f;
        struct ipcp_entry * ipcp;
        int                 port_id;
        int                 state;
        uint8_t *           hash;

        ipcp = get_ipcp_by_dst_name(dst, pid);
        if (ipcp == NULL) {
                log_info("Destination %s unreachable.", dst);
                return -1;
        }

        pthread_rwlock_wrlock(&irmd.flows_lock);
        port_id = bmp_allocate(irmd.port_ids);
        if (!bmp_is_id_valid(irmd.port_ids, port_id)) {
                pthread_rwlock_unlock(&irmd.flows_lock);
                log_err("Could not allocate port_id.");
                return -EBADF;
        }

        f = irm_flow_create(pid, ipcp->pid, port_id, cube);
        if (f == NULL) {
                bmp_release(irmd.port_ids, port_id);
                pthread_rwlock_unlock(&irmd.flows_lock);
                log_err("Could not allocate port_id.");
                return -ENOMEM;
        }

        list_add(&f->next, &irmd.irm_flows);

        pthread_rwlock_unlock(&irmd.flows_lock);

        assert(irm_flow_get_state(f) == FLOW_ALLOC_PENDING);

        hash = malloc(IPCP_HASH_LEN(ipcp));
        if  (hash == NULL)
                /* sanitizer cleans this */
                return -ENOMEM;

        str_hash(ipcp->dir_hash_algo, hash, dst);

        if (ipcp_flow_alloc(ipcp->pid, port_id, pid, hash,
                            IPCP_HASH_LEN(ipcp), cube)) {
                /* sanitizer cleans this */
                log_info("Flow_allocation failed.");
                free(hash);
                return -EAGAIN;
        }

        free(hash);

        state = irm_flow_wait_state(f, FLOW_ALLOCATED, timeo);
        if (state != FLOW_ALLOCATED) {
                if (state == -ETIMEDOUT) {
                        log_dbg("Flow allocation timed out");
                        return -ETIMEDOUT;
                }

                log_info("Pending flow to %s torn down.", dst);
                return -EPIPE;
        }

        assert(irm_flow_get_state(f) == FLOW_ALLOCATED);

        *e = f;

        log_info("Flow on port_id %d allocated.", port_id);

        return 0;
}

static int flow_dealloc(pid_t pid,
                        int   port_id)
{
        pid_t n_1_pid = -1;
        int   ret = 0;

        struct irm_flow * f = NULL;

        pthread_rwlock_wrlock(&irmd.flows_lock);

        f = get_irm_flow(port_id);
        if (f == NULL) {
                pthread_rwlock_unlock(&irmd.flows_lock);
                log_dbg("Deallocate unknown port %d by %d.", port_id, pid);
                return 0;
        }

        if (pid == f->n_pid) {
                f->n_pid = -1;
                n_1_pid = f->n_1_pid;
        } else if (pid == f->n_1_pid) {
                f->n_1_pid = -1;
        } else {
                pthread_rwlock_unlock(&irmd.flows_lock);
                log_dbg("Dealloc called by wrong process.");
                return -EPERM;
        }

        if (irm_flow_get_state(f) == FLOW_DEALLOC_PENDING) {
                list_del(&f->next);
                if ((kill(f->n_pid, 0) < 0 && f->n_1_pid == -1) ||
                    (kill(f->n_1_pid, 0) < 0 && f->n_pid == -1))
                        irm_flow_set_state(f, FLOW_NULL);
                clear_irm_flow(f);
                irm_flow_destroy(f);
                bmp_release(irmd.port_ids, port_id);
                log_info("Completed deallocation of port_id %d by process %d.",
                         port_id, pid);
        } else {
                irm_flow_set_state(f, FLOW_DEALLOC_PENDING);
                log_dbg("Partial deallocation of port_id %d by process %d.",
                        port_id, pid);
        }

        pthread_rwlock_unlock(&irmd.flows_lock);

        if (n_1_pid != -1)
                ret = ipcp_flow_dealloc(n_1_pid, port_id);

        return ret;
}

static pid_t auto_execute(char ** argv)
{
        pid_t       pid;
        struct stat s;

        if (stat(argv[0], &s) != 0) {
                log_warn("Program %s does not exist.", argv[0]);
                return -1;
        }

        if (!(s.st_mode & S_IXUSR)) {
                log_warn("Program %s is not executable.", argv[0]);
                return -1;
        }

        pid = fork();
        if (pid == -1) {
                log_err("Failed to fork");
                return pid;
        }

        if (pid != 0) {
                log_info("Instantiated %s as process %d.", argv[0], pid);
                return pid;
        }

        execv(argv[0], argv);

        log_err("Failed to execute %s.", argv[0]);

        exit(EXIT_FAILURE);
}

static struct irm_flow * flow_req_arr(pid_t           pid,
                                      const uint8_t * hash,
                                      qoscube_t       cube)
{
        struct reg_entry *  re = NULL;
        struct prog_entry * a  = NULL;
        struct proc_entry * e  = NULL;
        struct irm_flow *   f  = NULL;

        struct pid_el *     c_pid;
        struct ipcp_entry * ipcp;
        pid_t               h_pid   = -1;
        int                 port_id = -1;

        struct timespec wt = {IRMD_REQ_ARR_TIMEOUT / 1000,
                              (IRMD_REQ_ARR_TIMEOUT % 1000) * MILLION};

        log_dbg("Flow req arrived from IPCP %d for " HASH_FMT ".",
                pid, HASH_VAL(hash));

        pthread_rwlock_rdlock(&irmd.reg_lock);

        ipcp = get_ipcp_entry_by_pid(pid);
        if (ipcp == NULL) {
                log_err("IPCP died.");
                return NULL;
        }

        re = registry_get_entry_by_hash(&irmd.registry, ipcp->dir_hash_algo,
                                        hash, IPCP_HASH_LEN(ipcp));
        if (re == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("Unknown hash: " HASH_FMT ".", HASH_VAL(hash));
                return NULL;
        }

        log_info("Flow request arrived for %s.", re->name);

        pthread_rwlock_unlock(&irmd.reg_lock);

        /* Give the process a bit of slop time to call accept */
        if (reg_entry_leave_state(re, REG_NAME_IDLE, &wt) == -1) {
                log_err("No processes for " HASH_FMT ".", HASH_VAL(hash));
                return NULL;
        }

        pthread_rwlock_wrlock(&irmd.reg_lock);

        switch (reg_entry_get_state(re)) {
        case REG_NAME_IDLE:
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("No processes for " HASH_FMT ".", HASH_VAL(hash));
                return NULL;
        case REG_NAME_AUTO_ACCEPT:
                c_pid = malloc(sizeof(*c_pid));
                if (c_pid == NULL) {
                        pthread_rwlock_unlock(&irmd.reg_lock);
                        return NULL;
                }

                reg_entry_set_state(re, REG_NAME_AUTO_EXEC);
                a = prog_table_get(&irmd.prog_table,
                                   reg_entry_get_prog(re));

                if (a == NULL || (c_pid->pid = auto_execute(a->argv)) < 0) {
                        reg_entry_set_state(re, REG_NAME_AUTO_ACCEPT);
                        pthread_rwlock_unlock(&irmd.reg_lock);
                        log_err("Could not start program for reg_entry %s.",
                                re->name);
                        free(c_pid);
                        return NULL;
                }

                list_add(&c_pid->next, &irmd.spawned_pids);

                pthread_rwlock_unlock(&irmd.reg_lock);

                if (reg_entry_leave_state(re, REG_NAME_AUTO_EXEC, NULL))
                        return NULL;

                pthread_rwlock_wrlock(&irmd.reg_lock);
                /* FALLTHRU */
        case REG_NAME_FLOW_ACCEPT:
                h_pid = reg_entry_get_pid(re);
                if (h_pid == -1) {
                        pthread_rwlock_unlock(&irmd.reg_lock);
                        log_err("Invalid process id returned.");
                        return NULL;
                }

                break;
        default:
                pthread_rwlock_unlock(&irmd.reg_lock);
                log_err("IRMd in wrong state.");
                return NULL;
        }

        pthread_rwlock_unlock(&irmd.reg_lock);
        pthread_rwlock_wrlock(&irmd.flows_lock);
        port_id = bmp_allocate(irmd.port_ids);
        if (!bmp_is_id_valid(irmd.port_ids, port_id)) {
                pthread_rwlock_unlock(&irmd.flows_lock);
                return NULL;
        }

        f = irm_flow_create(h_pid, pid, port_id, cube);
        if (f == NULL) {
                bmp_release(irmd.port_ids, port_id);
                pthread_rwlock_unlock(&irmd.flows_lock);
                log_err("Could not allocate port_id.");
                return NULL;
        }

        list_add(&f->next, &irmd.irm_flows);

        pthread_rwlock_unlock(&irmd.flows_lock);
        pthread_rwlock_rdlock(&irmd.reg_lock);

        reg_entry_set_state(re, REG_NAME_FLOW_ARRIVED);

        e = proc_table_get(&irmd.proc_table, h_pid);
        if (e == NULL) {
                pthread_rwlock_unlock(&irmd.reg_lock);
                pthread_rwlock_wrlock(&irmd.flows_lock);
                clear_irm_flow(f);
                bmp_release(irmd.port_ids, f->port_id);
                list_del(&f->next);
                pthread_rwlock_unlock(&irmd.flows_lock);
                log_err("Could not get process table entry for %d.", h_pid);
                irm_flow_destroy(f);
                return NULL;
        }

        proc_entry_wake(e, re);

        pthread_rwlock_unlock(&irmd.reg_lock);

        reg_entry_leave_state(re, REG_NAME_FLOW_ARRIVED, NULL);

        return f;
}

static int flow_alloc_reply(int port_id,
                            int response)
{
        struct irm_flow * f;

        pthread_rwlock_rdlock(&irmd.flows_lock);

        f = get_irm_flow(port_id);
        if (f == NULL) {
                pthread_rwlock_unlock(&irmd.flows_lock);
                return -1;
        }

        if (!response)
                irm_flow_set_state(f, FLOW_ALLOCATED);
        else
                irm_flow_set_state(f, FLOW_NULL);

        pthread_rwlock_unlock(&irmd.flows_lock);

        return 0;
}

static void irm_fini(void)
{
        struct list_head * p;
        struct list_head * h;

        if (irmd_get_state() != IRMD_NULL)
                log_warn("Unsafe destroy.");

        pthread_rwlock_wrlock(&irmd.flows_lock);

        if (irmd.port_ids != NULL)
                bmp_destroy(irmd.port_ids);

        pthread_rwlock_unlock(&irmd.flows_lock);

        close(irmd.sockfd);

        if (unlink(IRM_SOCK_PATH))
                log_dbg("Failed to unlink %s.", IRM_SOCK_PATH);

        pthread_rwlock_wrlock(&irmd.reg_lock);
        /* Clear the lists. */
        list_for_each_safe(p, h, &irmd.ipcps) {
                struct ipcp_entry * e = list_entry(p, struct ipcp_entry, next);
                list_del(&e->next);
                ipcp_entry_destroy(e);
        }

        list_for_each(p, &irmd.spawned_pids) {
                struct pid_el * e = list_entry(p, struct pid_el, next);
                if (kill(e->pid, SIGTERM))
                        log_dbg("Could not send kill signal to %d.", e->pid);
        }

        list_for_each_safe(p, h, &irmd.spawned_pids) {
                struct pid_el * e = list_entry(p, struct pid_el, next);
                int status;
                if (waitpid(e->pid, &status, 0) < 0)
                        log_dbg("Error waiting for %d to exit.", e->pid);
                list_del(&e->next);
                registry_del_process(&irmd.registry, e->pid);
                free(e);
        }

        list_for_each_safe(p, h, &irmd.prog_table) {
                struct prog_entry * e = list_entry(p, struct prog_entry, next);
                list_del(&e->next);
                prog_entry_destroy(e);
        }

        registry_destroy(&irmd.registry);

        pthread_rwlock_unlock(&irmd.reg_lock);

        if (irmd.rdrb != NULL)
                shm_rdrbuff_destroy(irmd.rdrb);

        if (irmd.lf != NULL)
                lockfile_destroy(irmd.lf);

        pthread_mutex_destroy(&irmd.cmd_lock);
        pthread_cond_destroy(&irmd.cmd_cond);
        pthread_rwlock_destroy(&irmd.reg_lock);
        pthread_rwlock_destroy(&irmd.state_lock);

#ifdef HAVE_FUSE
        if (rmdir(FUSE_PREFIX))
                log_dbg("Failed to remove " FUSE_PREFIX);
#endif
}

void irmd_sig_handler(int         sig,
                      siginfo_t * info,
                      void *      c)
{
        (void) info;
        (void) c;

        switch(sig) {
        case SIGINT:
        case SIGTERM:
        case SIGHUP:
                if (irmd_get_state() == IRMD_NULL) {
                        log_info("Patience is bitter, but its fruit is sweet.");
                        return;
                }

                log_info("IRMd shutting down...");
                irmd_set_state(IRMD_NULL);
                break;
        case SIGPIPE:
                log_dbg("Ignored SIGPIPE.");
        default:
                return;
        }
}

void * shm_sanitize(void * o)
{
        struct list_head * p = NULL;
        struct timespec ts = {SHM_SAN_HOLDOFF / 1000,
                              (SHM_SAN_HOLDOFF % 1000) * MILLION};
        ssize_t idx;

        (void) o;

        while (irmd_get_state() == IRMD_RUNNING) {
                if (shm_rdrbuff_wait_full(irmd.rdrb, &ts) == -ETIMEDOUT)
                        continue;

                pthread_rwlock_wrlock(&irmd.flows_lock);

                list_for_each(p, &irmd.irm_flows) {
                        struct irm_flow * f =
                                list_entry(p, struct irm_flow, next);
                        if (kill(f->n_pid, 0) < 0) {
                                while ((idx = shm_rbuff_read(f->n_rb)) >= 0)
                                        shm_rdrbuff_remove(irmd.rdrb, idx);
                                continue;
                        }

                        if (kill(f->n_1_pid, 0) < 0) {
                                while ((idx = shm_rbuff_read(f->n_1_rb)) >= 0)
                                        shm_rdrbuff_remove(irmd.rdrb, idx);
                                continue;
                        }
                }

                pthread_rwlock_unlock(&irmd.flows_lock);
        }

        return (void *) 0;
}

void * irm_sanitize(void * o)
{
        struct timespec now;
        struct list_head * p = NULL;
        struct list_head * h = NULL;

        struct timespec timeout = {IRMD_CLEANUP_TIMER / BILLION,
                                   IRMD_CLEANUP_TIMER % BILLION};
        int s;

        (void) o;

        while (true) {
                if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
                        log_warn("Failed to get time.");

                if (irmd_get_state() != IRMD_RUNNING)
                        return (void *) 0;

                pthread_rwlock_wrlock(&irmd.reg_lock);

                list_for_each_safe(p, h, &irmd.spawned_pids) {
                        struct pid_el * e = list_entry(p, struct pid_el, next);
                        waitpid(e->pid, &s, WNOHANG);
                        if (kill(e->pid, 0) >= 0)
                                continue;
                        log_dbg("Child process %d died, error %d.", e->pid, s);
                        list_del(&e->next);
                        free(e);
                }

                list_for_each_safe(p, h, &irmd.proc_table) {
                        struct proc_entry * e =
                                list_entry(p, struct proc_entry, next);
                        if (kill(e->pid, 0) >= 0)
                                continue;
                        log_dbg("Dead process removed: %d.", e->pid);
                        list_del(&e->next);
                        proc_entry_destroy(e);
                }

                list_for_each_safe(p, h, &irmd.ipcps) {
                        struct ipcp_entry * e =
                                list_entry(p, struct ipcp_entry, next);
                        if (kill(e->pid, 0) >= 0)
                                continue;
                        log_dbg("Dead IPCP removed: %d.", e->pid);
                        list_del(&e->next);
                        ipcp_entry_destroy(e);
                }

                list_for_each_safe(p, h, &irmd.registry) {
                        struct list_head * p2;
                        struct list_head * h2;
                        struct reg_entry * e =
                                list_entry(p, struct reg_entry, next);
                        list_for_each_safe(p2, h2, &e->reg_pids) {
                                struct pid_el * a =
                                        list_entry(p2, struct pid_el, next);
                                if (kill(a->pid, 0) >= 0)
                                        continue;
                                log_dbg("Dead process removed from: %d %s.",
                                        a->pid, e->name);
                                reg_entry_del_pid_el(e, a);
                        }
                }

                pthread_rwlock_unlock(&irmd.reg_lock);
                pthread_rwlock_wrlock(&irmd.flows_lock);

                list_for_each_safe(p, h, &irmd.irm_flows) {
                        int ipcpi;
                        int port_id;
                        struct irm_flow * f =
                                list_entry(p, struct irm_flow, next);

                        if (irm_flow_get_state(f) == FLOW_ALLOC_PENDING
                            && ts_diff_ms(&f->t0, &now) > IRMD_FLOW_TIMEOUT) {
                                log_dbg("Pending port_id %d timed out.",
                                         f->port_id);
                                f->n_pid = -1;
                                irm_flow_set_state(f, FLOW_DEALLOC_PENDING);
                                ipcpi   = f->n_1_pid;
                                port_id = f->port_id;
                                continue;
                        }

                        if (kill(f->n_pid, 0) < 0) {
                                struct shm_flow_set * set;
                                log_dbg("Process %d gone, deallocating flow %d.",
                                         f->n_pid, f->port_id);
                                set = shm_flow_set_open(f->n_pid);
                                if (set != NULL)
                                        shm_flow_set_destroy(set);
                                f->n_pid = -1;
                                irm_flow_set_state(f, FLOW_DEALLOC_PENDING);
                                ipcpi   = f->n_1_pid;
                                port_id = f->port_id;
                                pthread_rwlock_unlock(&irmd.flows_lock);
                                ipcp_flow_dealloc(ipcpi, port_id);
                                pthread_rwlock_wrlock(&irmd.flows_lock);
                                continue;
                        }

                        if (kill(f->n_1_pid, 0) < 0) {
                                struct shm_flow_set * set;
                                log_err("IPCP %d gone, flow %d removed.",
                                        f->n_1_pid, f->port_id);
                                set = shm_flow_set_open(f->n_pid);
                                if (set != NULL)
                                        shm_flow_set_destroy(set);
                                f->n_1_pid = -1;
                                irm_flow_set_state(f, FLOW_DEALLOC_PENDING);
                        }
                }

                pthread_rwlock_unlock(&irmd.flows_lock);

                nanosleep(&timeout, NULL);
        }
}

static void * acceptloop(void * o)
{
        int            csockfd;
        struct timeval tv = {(SOCKET_TIMEOUT / 1000),
                             (SOCKET_TIMEOUT % 1000) * 1000};
#if defined(__FreeBSD__) || defined(__APPLE__)
        fd_set         fds;
        struct timeval timeout = {(IRMD_ACCEPT_TIMEOUT / 1000),
                                  (IRMD_ACCEPT_TIMEOUT % 1000) * 1000};
#endif
        (void) o;

        while (irmd_get_state() == IRMD_RUNNING) {
                struct cmd * cmd;

#if defined(__FreeBSD__) || defined(__APPLE__)
                FD_ZERO(&fds);
                FD_SET(irmd.sockfd, &fds);
                if (select(irmd.sockfd + 1, &fds, NULL, NULL, &timeout) <= 0)
                        continue;
#endif
                csockfd = accept(irmd.sockfd, 0, 0);
                if (csockfd < 0)
                        continue;

                if (setsockopt(csockfd, SOL_SOCKET, SO_RCVTIMEO,
                               (void *) &tv, sizeof(tv)))
                        log_warn("Failed to set timeout on socket.");

                cmd = malloc(sizeof(*cmd));
                if (cmd == NULL) {
                        log_err("Out of memory.");
                        close(csockfd);
                        break;
                }

                cmd->len = read(csockfd, cmd->cbuf, IRM_MSG_BUF_SIZE);
                if (cmd->len <= 0) {
                        log_err("Failed to read from socket.");
                        close(csockfd);
                        free(cmd);
                        continue;
                }

                cmd->fd  = csockfd;

                pthread_mutex_lock(&irmd.cmd_lock);

                list_add(&cmd->next, &irmd.cmds);

                pthread_cond_signal(&irmd.cmd_cond);

                pthread_mutex_unlock(&irmd.cmd_lock);
        }

        return (void *) 0;
}

static void close_ptr(void * o)
{
        close(*((int *) o));
}

static void free_msg(void * o)
{
        irm_msg__free_unpacked((irm_msg_t *) o, NULL);
}

static void * mainloop(void * o)
{
        int             sfd;
        irm_msg_t *     msg;
        buffer_t        buffer;

        (void) o;

        while (true) {
                irm_msg_t         ret_msg = IRM_MSG__INIT;
                struct irm_flow * e       = NULL;
                pid_t *           pids    = NULL;
                struct timespec * timeo   = NULL;
                struct timespec   ts      = {0, 0};
                struct cmd *      cmd;

                ret_msg.code = IRM_MSG_CODE__IRM_REPLY;

                pthread_mutex_lock(&irmd.cmd_lock);

                pthread_cleanup_push((void *)(void *) pthread_mutex_unlock,
                                     &irmd.cmd_lock);

                while (list_is_empty(&irmd.cmds))
                        pthread_cond_wait(&irmd.cmd_cond, &irmd.cmd_lock);

                cmd = list_last_entry(&irmd.cmds, struct cmd, next);
                list_del(&cmd->next);

                pthread_cleanup_pop(true);

                msg = irm_msg__unpack(NULL, cmd->len, cmd->cbuf);
                sfd = cmd->fd;

                free(cmd);

                if (msg == NULL) {
                        close(sfd);
                        continue;
                }

                tpm_dec(irmd.tpm);

                if (msg->has_timeo_sec) {
                        assert(msg->has_timeo_nsec);

                        ts.tv_sec  = msg->timeo_sec;
                        ts.tv_nsec = msg->timeo_nsec;
                        timeo = &ts;
                }

                pthread_cleanup_push(close_ptr, &sfd);
                pthread_cleanup_push(free_msg, msg);

                switch (msg->code) {
                case IRM_MSG_CODE__IRM_CREATE_IPCP:
                        ret_msg.has_result = true;
                        ret_msg.result = create_ipcp(msg->dst_name,
                                                     msg->ipcp_type);
                        break;
                case IRM_MSG_CODE__IPCP_CREATE_R:
                        ret_msg.has_result = true;
                        ret_msg.result = create_ipcp_r(msg->pid, msg->result);
                        break;
                case IRM_MSG_CODE__IRM_DESTROY_IPCP:
                        ret_msg.has_result = true;
                        ret_msg.result = destroy_ipcp(msg->pid);
                        break;
                case IRM_MSG_CODE__IRM_BOOTSTRAP_IPCP:
                        ret_msg.has_result = true;
                        ret_msg.result = bootstrap_ipcp(msg->pid, msg->conf);
                        break;
                case IRM_MSG_CODE__IRM_ENROLL_IPCP:
                        ret_msg.has_result = true;
                        ret_msg.result = enroll_ipcp(msg->pid,
                                                     msg->dif_name[0]);
                        break;
                case IRM_MSG_CODE__IRM_CONNECT_IPCP:
                        ret_msg.has_result = true;
                        ret_msg.result = connect_ipcp(msg->pid,
                                                      msg->dst_name,
                                                      msg->comp_name);
                        break;
                case IRM_MSG_CODE__IRM_DISCONNECT_IPCP:
                        ret_msg.has_result = true;
                        ret_msg.result = disconnect_ipcp(msg->pid,
                                                         msg->dst_name,
                                                         msg->comp_name);
                        break;
                case IRM_MSG_CODE__IRM_BIND_PROGRAM:
                        ret_msg.has_result = true;
                        ret_msg.result = bind_program(msg->prog_name,
                                                      msg->dst_name,
                                                      msg->opts,
                                                      msg->n_args,
                                                      msg->args);
                        break;
                case IRM_MSG_CODE__IRM_UNBIND_PROGRAM:
                        ret_msg.has_result = true;
                        ret_msg.result = unbind_program(msg->prog_name,
                                                        msg->dst_name);
                        break;
                case IRM_MSG_CODE__IRM_PROC_ANNOUNCE:
                        ret_msg.has_result = true;
                        ret_msg.result = proc_announce(msg->pid,
                                                       msg->prog_name);
                        break;
                case IRM_MSG_CODE__IRM_BIND_PROCESS:
                        ret_msg.has_result = true;
                        ret_msg.result = bind_process(msg->pid, msg->dst_name);
                        break;
                case IRM_MSG_CODE__IRM_UNBIND_PROCESS:
                        ret_msg.has_result = true;
                        ret_msg.result = unbind_process(msg->pid,
                                                        msg->dst_name);
                        break;
                case IRM_MSG_CODE__IRM_LIST_IPCPS:
                        ret_msg.has_result = true;
                        ret_msg.n_pids = list_ipcps(msg->dst_name, &pids);
                        ret_msg.pids = pids;
                        break;
                case IRM_MSG_CODE__IRM_REG:
                        ret_msg.has_result = true;
                        ret_msg.result = name_reg(msg->dst_name,
                                                  msg->dif_name,
                                                  msg->n_dif_name);
                        break;
                case IRM_MSG_CODE__IRM_UNREG:
                        ret_msg.has_result = true;
                        ret_msg.result = name_unreg(msg->dst_name,
                                                    msg->dif_name,
                                                    msg->n_dif_name);
                        break;
                case IRM_MSG_CODE__IRM_FLOW_ACCEPT:
                        ret_msg.has_result = true;
                        ret_msg.result = flow_accept(msg->pid, timeo, &e);
                        if (ret_msg.result == 0) {
                                ret_msg.has_port_id = true;
                                ret_msg.port_id     = e->port_id;
                                ret_msg.has_pid     = true;
                                ret_msg.pid         = e->n_1_pid;
                                ret_msg.has_qoscube = true;
                                ret_msg.qoscube     = e->qc;
                        }
                        break;
                case IRM_MSG_CODE__IRM_FLOW_ALLOC:
                        ret_msg.has_result = true;
                        ret_msg.result = flow_alloc(msg->pid, msg->dst_name,
                                                    msg->qoscube, timeo, &e);
                        if (ret_msg.result == 0) {
                                ret_msg.has_port_id = true;
                                ret_msg.port_id     = e->port_id;
                                ret_msg.has_pid     = true;
                                ret_msg.pid         = e->n_1_pid;
                        }
                        break;
                case IRM_MSG_CODE__IRM_FLOW_DEALLOC:
                        ret_msg.has_result = true;
                        ret_msg.result = flow_dealloc(msg->pid, msg->port_id);
                        break;
                case IRM_MSG_CODE__IPCP_FLOW_REQ_ARR:
                        e = flow_req_arr(msg->pid,
                                         msg->hash.data,
                                         msg->qoscube);
                        ret_msg.has_result = true;
                        if (e == NULL) {
                                ret_msg.result = -1;
                                break;
                        }
                        ret_msg.has_port_id = true;
                        ret_msg.port_id     = e->port_id;
                        ret_msg.has_pid     = true;
                        ret_msg.pid         = e->n_pid;
                        break;
                case IRM_MSG_CODE__IPCP_FLOW_ALLOC_REPLY:
                        ret_msg.has_result = true;
                        ret_msg.result = flow_alloc_reply(msg->port_id,
                                                          msg->response);
                        break;
                default:
                        log_err("Don't know that message code.");
                        break;
                }

                pthread_cleanup_pop(true);
                pthread_cleanup_pop(false);

                if (ret_msg.result == -EPIPE || !ret_msg.has_result) {
                        close(sfd);
                        tpm_inc(irmd.tpm);
                        continue;
                }

                buffer.len = irm_msg__get_packed_size(&ret_msg);
                if (buffer.len == 0) {
                        log_err("Failed to calculate length of reply message.");
                        if (pids != NULL)
                                free(pids);
                        close(sfd);
                        tpm_inc(irmd.tpm);
                        continue;
                }

                buffer.data = malloc(buffer.len);
                if (buffer.data == NULL) {
                        if (pids != NULL)
                                free(pids);
                        close(sfd);
                        tpm_inc(irmd.tpm);
                        continue;
                }

                irm_msg__pack(&ret_msg, buffer.data);

                if (pids != NULL)
                        free(pids);

                pthread_cleanup_push(close_ptr, &sfd);

                if (write(sfd, buffer.data, buffer.len) == -1)
                        if (ret_msg.result != -EIRMD)
                                log_warn("Failed to send reply message.");

                free(buffer.data);

                pthread_cleanup_pop(true);

                tpm_inc(irmd.tpm);
        }

        return (void *) 0;
}

static int irm_init(void)
{
        struct stat        st;
        struct timeval     timeout = {(IRMD_ACCEPT_TIMEOUT / 1000),
                                      (IRMD_ACCEPT_TIMEOUT % 1000) * 1000};
        pthread_condattr_t cattr;

        memset(&st, 0, sizeof(st));

        if (pthread_rwlock_init(&irmd.state_lock, NULL)) {
                log_err("Failed to initialize rwlock.");
                goto fail_state_lock;
        }

        if (pthread_rwlock_init(&irmd.reg_lock, NULL)) {
                log_err("Failed to initialize rwlock.");
                goto fail_reg_lock;
        }

        if (pthread_rwlock_init(&irmd.flows_lock, NULL)) {
                log_err("Failed to initialize rwlock.");
                goto fail_flows_lock;
        }

        if (pthread_mutex_init(&irmd.cmd_lock, NULL)) {
                log_err("Failed to initialize mutex.");
                goto fail_cmd_lock;
        }

        if (pthread_condattr_init(&cattr)) {
                log_err("Failed to initialize mutex.");
                goto fail_cmd_lock;
        }

#ifndef __APPLE__
        pthread_condattr_setclock(&cattr, PTHREAD_COND_CLOCK);
#endif
        if (pthread_cond_init(&irmd.cmd_cond, &cattr)) {
                log_err("Failed to initialize condvar.");
                pthread_condattr_destroy(&cattr);
                goto fail_cmd_cond;
        }

        pthread_condattr_destroy(&cattr);

        list_head_init(&irmd.ipcps);
        list_head_init(&irmd.proc_table);
        list_head_init(&irmd.prog_table);
        list_head_init(&irmd.spawned_pids);
        list_head_init(&irmd.registry);
        list_head_init(&irmd.irm_flows);
        list_head_init(&irmd.cmds);

        irmd.port_ids = bmp_create(SYS_MAX_FLOWS, 0);
        if (irmd.port_ids == NULL) {
                log_err("Failed to create port_ids bitmap.");
                goto fail_port_ids;
        }

        if ((irmd.lf = lockfile_create()) == NULL) {
                if ((irmd.lf = lockfile_open()) == NULL) {
                        log_err("Lockfile error.");
                        goto fail_lockfile;
                }

                if (kill(lockfile_owner(irmd.lf), 0) < 0) {
                        log_info("IRMd didn't properly shut down last time.");
                        shm_rdrbuff_purge();
                        log_info("Stale resources cleaned.");
                        lockfile_destroy(irmd.lf);
                        irmd.lf = lockfile_create();
                } else {
                        log_info("IRMd already running (%d), exiting.",
                                 lockfile_owner(irmd.lf));
                        lockfile_close(irmd.lf);
                        goto fail_lockfile;
                }
        }

        if (stat(SOCK_PATH, &st) == -1) {
                if (mkdir(SOCK_PATH, 0777)) {
                        log_err("Failed to create sockets directory.");
                        goto fail_stat;
                }
        }

        irmd.sockfd = server_socket_open(IRM_SOCK_PATH);
        if (irmd.sockfd < 0) {
                log_err("Failed to open server socket.");
                goto fail_sock_path;
        }

        if (setsockopt(irmd.sockfd, SOL_SOCKET, SO_RCVTIMEO,
                       (char *) &timeout, sizeof(timeout)) < 0) {
                log_err("Failed setting socket option.");
                goto fail_sock_opt;
        }

        if (chmod(IRM_SOCK_PATH, 0666)) {
                log_err("Failed to chmod socket.");
                goto fail_sock_opt;
        }

        if (irmd.lf == NULL) {
                log_err("Failed to create lockfile.");
                goto fail_sock_opt;
        }

        if ((irmd.rdrb = shm_rdrbuff_create()) == NULL) {
                log_err("Failed to create rdrbuff.");
                goto fail_rdrbuff;
        }
#ifdef HAVE_FUSE
        if (stat(FUSE_PREFIX, &st) != -1)
                log_warn(FUSE_PREFIX " already exists...");
        else
                mkdir(FUSE_PREFIX, 0777);
#endif

#ifdef HAVE_LIBGCRYPT
        if (gcry_control(GCRYCTL_ANY_INITIALIZATION_P))
                goto fail_gcry_control;

        gcry_control(GCRYCTL_INITIALIZATION_FINISHED);
#endif

        irmd.state   = IRMD_RUNNING;

        log_info("Ouroboros IPC Resource Manager daemon started...");

        return 0;

#ifdef HAVE_LIBGCRYPT
 fail_gcry_control:
        shm_rdrbuff_destroy(irmd.rdrb);
#endif
 fail_rdrbuff:
        shm_rdrbuff_destroy(irmd.rdrb);
 fail_sock_opt:
        close(irmd.sockfd);
 fail_sock_path:
        unlink(IRM_SOCK_PATH);
 fail_stat:
        lockfile_destroy(irmd.lf);
 fail_lockfile:
        bmp_destroy(irmd.port_ids);
 fail_port_ids:
        pthread_cond_destroy(&irmd.cmd_cond);
 fail_cmd_cond:
        pthread_mutex_destroy(&irmd.cmd_lock);
 fail_cmd_lock:
        pthread_rwlock_destroy(&irmd.flows_lock);
 fail_flows_lock:
        pthread_rwlock_destroy(&irmd.reg_lock);
 fail_reg_lock:
        pthread_rwlock_destroy(&irmd.state_lock);
 fail_state_lock:
        return -1;
}

static void usage(void)
{
        printf("Usage: irmd \n"
               "         [--stdout  (Log to stdout instead of system log)]\n"
               "         [--version (Print version number and exit)]\n"
               "\n");
}

int main(int     argc,
         char ** argv)
{
        struct sigaction sig_act;
        sigset_t  sigset;
        bool use_stdout = false;

        sigemptyset(&sigset);
        sigaddset(&sigset, SIGINT);
        sigaddset(&sigset, SIGQUIT);
        sigaddset(&sigset, SIGHUP);
        sigaddset(&sigset, SIGPIPE);

        argc--;
        argv++;
        while (argc > 0) {
                if (strcmp(*argv, "--stdout") == 0) {
                        use_stdout = true;
                        argc--;
                        argv++;
                } else if (strcmp(*argv, "--version") == 0) {
                        printf("Ouroboros version %d.%d.%d\n",
                               OUROBOROS_VERSION_MAJOR,
                               OUROBOROS_VERSION_MINOR,
                               OUROBOROS_VERSION_PATCH);
                        exit(EXIT_SUCCESS);
                } else {
                        usage();
                        exit(EXIT_FAILURE);
                }
        }

        if (geteuid() != 0) {
                printf("IPC Resource Manager must be run as root.\n");
                exit(EXIT_FAILURE);
        }

        /* Init sig_act. */
        memset(&sig_act, 0, sizeof sig_act);

        /* Install signal traps. */
        sig_act.sa_sigaction = &irmd_sig_handler;
        sig_act.sa_flags     = SA_SIGINFO;

        if (sigaction(SIGINT,  &sig_act, NULL) < 0)
                exit(EXIT_FAILURE);
        if (sigaction(SIGTERM, &sig_act, NULL) < 0)
                exit(EXIT_FAILURE);
        if (sigaction(SIGHUP,  &sig_act, NULL) < 0)
                exit(EXIT_FAILURE);
        if (sigaction(SIGPIPE, &sig_act, NULL) < 0)
                exit(EXIT_FAILURE);

        log_init(!use_stdout);

        if (irm_init() < 0)
                goto fail_irm_init;

        irmd.tpm = tpm_create(IRMD_MIN_THREADS, IRMD_ADD_THREADS,
                              mainloop, NULL);
        if (irmd.tpm == NULL) {
                irmd_set_state(IRMD_NULL);
                goto fail_tpm_create;
        }

        if (tpm_start(irmd.tpm)) {
                irmd_set_state(IRMD_NULL);
                goto fail_tpm_start;
        }

        if (pthread_create(&irmd.irm_sanitize, NULL, irm_sanitize, NULL)) {
                irmd_set_state(IRMD_NULL);
                goto fail_irm_sanitize;
        }

        if (pthread_create(&irmd.shm_sanitize, NULL, shm_sanitize, irmd.rdrb)) {
                irmd_set_state(IRMD_NULL);
                goto fail_shm_sanitize;
        }

        if (pthread_create(&irmd.acceptor, NULL, acceptloop, NULL)) {
                irmd_set_state(IRMD_NULL);
                goto fail_acceptor;
        }

        pthread_join(irmd.acceptor, NULL);
        pthread_join(irmd.irm_sanitize, NULL);
        pthread_join(irmd.shm_sanitize, NULL);

        tpm_stop(irmd.tpm);

        tpm_destroy(irmd.tpm);

        pthread_sigmask(SIG_BLOCK, &sigset, NULL);

        irm_fini();

        pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);

        log_info("Bye.");

        log_fini();

        exit(EXIT_SUCCESS);

 fail_acceptor:
        pthread_join(irmd.shm_sanitize, NULL);
 fail_shm_sanitize:
        pthread_join(irmd.irm_sanitize, NULL);
 fail_irm_sanitize:
        tpm_stop(irmd.tpm);
 fail_tpm_start:
        tpm_destroy(irmd.tpm);
 fail_tpm_create:
        irm_fini();
 fail_irm_init:
        log_fini();
        exit(EXIT_FAILURE);
}
