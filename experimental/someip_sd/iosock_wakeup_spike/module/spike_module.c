/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/

/*
 * Wakeup spike, FreeBSD side: a worker that sleeps until it is woken by one of three primitives
 * (semaphore, taskqueue, msleep/wakeup), and the socket upcall as one of the wake sources.
 * The worker echoes datagrams of the kernel UDP socket to prove sosend/soreceive in its context.
 */

#include "experimental/someip_sd/iosock_wakeup_spike/module/spike_plugin.h"

// clang-format off
// The devs/ FreeBSD kernel headers are not self-contained and must be included in this order.
#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/priority.h>
#include <sys/proc.h>
#include <sys/kthread.h>
#include <sys/taskqueue.h>
#include <qnx/qnx_modload.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/mbuf.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <sys/slogcodes.h>
#include <sys/slog.h>
// clang-format on

// Mirrors FreeBSD 13.5 sys/_uio.h; the QNX SDK only forward-declares struct uio.
enum uio_rw
{
    UIO_READ,
    UIO_WRITE
};

enum uio_seg
{
    UIO_USERSPACE,
    UIO_SYSSPACE,
    UIO_NOCOPY
};

struct uio
{
    struct iovec* uio_iov;
    int uio_iovcnt;
    off_t uio_offset;
    ssize_t uio_resid;
    enum uio_seg uio_segflg;
    enum uio_rw uio_rw;
    struct thread* uio_td;
};

/* Values of spike_protocol.h */
#define SPIKE_UDP_PORT 30750U
#define SPIKE_SOURCE_SOCKET_UPCALL 1U
#define SPIKE_PRIMITIVE_SEMAPHORE 1U
#define SPIKE_PRIMITIVE_TASKQUEUE 2U
#define SPIKE_PRIMITIVE_MSLEEP 3U

#define SPIKE_WAIT_TIMEOUT_MS 1000U
#define SPIKE_DATAGRAM_SIZE 256U

static struct socket* spike_socket = NULL;
static struct mtx msleep_mutex;
static int msleep_pending = 0;
static struct taskqueue* spike_taskqueue = NULL;
static struct task spike_task;

/* Drains the kernel socket and echoes each datagram to its sender. */
static void spike_work(void)
{
    if (spike_plugin_source() != SPIKE_SOURCE_SOCKET_UPCALL)
    {
        return;
    }
    for (;;)
    {
        char buffer[SPIKE_DATAGRAM_SIZE];
        struct iovec iov = {.iov_base = buffer, .iov_len = sizeof(buffer)};
        struct uio receive;
        memset(&receive, 0, sizeof(receive));
        receive.uio_iov = &iov;
        receive.uio_iovcnt = 1;
        receive.uio_resid = sizeof(buffer);
        receive.uio_segflg = UIO_SYSSPACE;
        receive.uio_rw = UIO_READ;
        receive.uio_td = curthread;
        struct sockaddr* from = NULL;
        int flags = MSG_DONTWAIT;
        const int ret = soreceive(spike_socket, &from, &receive, NULL, NULL, &flags);
        if ((ret != 0) || (from == NULL))
        {
            if (from != NULL)
            {
                free(from, M_SONAME);
            }
            if ((ret != 0) && (ret != EWOULDBLOCK) && (ret != EAGAIN))
            {
                slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[iosock_spike] soreceive failed: %d\n", ret);
            }
            return;
        }
        const size_t received = sizeof(buffer) - (size_t)receive.uio_resid;
        struct iovec echo_iov = {.iov_base = buffer, .iov_len = received};
        struct uio send;
        memset(&send, 0, sizeof(send));
        send.uio_iov = &echo_iov;
        send.uio_iovcnt = 1;
        send.uio_resid = (ssize_t)received;
        send.uio_segflg = UIO_SYSSPACE;
        send.uio_rw = UIO_WRITE;
        send.uio_td = curthread;
        if (sosend(spike_socket, from, &send, NULL, NULL, MSG_DONTWAIT, curthread) == 0)
        {
            spike_plugin_note_echo();
        }
        free(from, M_SONAME);
    }
}

void spike_signal(void)
{
    const unsigned int primitive = spike_plugin_primitive();
    spike_plugin_note_signal();
    switch (primitive)
    {
        case SPIKE_PRIMITIVE_SEMAPHORE:
            spike_semaphore_post();
            break;
        case SPIKE_PRIMITIVE_TASKQUEUE:
            taskqueue_enqueue(spike_taskqueue, &spike_task);
            break;
        case SPIKE_PRIMITIVE_MSLEEP:
            mtx_lock(&msleep_mutex);
            msleep_pending = 1;
            wakeup(&msleep_pending);
            mtx_unlock(&msleep_mutex);
            break;
        default:
            break;
    }
}

/* Upcalls run in the network stack with the receive buffer locked: only signal here. */
static int spike_receive_upcall(struct socket* so __unused, void* arg __unused, int waitflag __unused)
{
    if (spike_plugin_source() == SPIKE_SOURCE_SOCKET_UPCALL)
    {
        spike_signal();
    }
    return SU_OK;
}

static void spike_task_run(void* context __unused, int pending __unused)
{
    spike_plugin_note_wake(0);
    spike_work();
}

/* Worker for the semaphore and msleep primitives; the taskqueue has its own thread. */
static void spike_worker(void* arg __unused)
{
    slogf(_SLOGC_NETWORK, _SLOG_INFO, "[iosock_spike] worker started\n");
    for (;;)
    {
        const unsigned int primitive = spike_plugin_primitive();
        if (primitive == SPIKE_PRIMITIVE_SEMAPHORE)
        {
            const int signaled = spike_semaphore_wait(SPIKE_WAIT_TIMEOUT_MS);
            spike_plugin_note_wake(signaled == 0);
            spike_work();
        }
        else if (primitive == SPIKE_PRIMITIVE_MSLEEP)
        {
            int timed_out = 0;
            mtx_lock(&msleep_mutex);
            while (msleep_pending == 0)
            {
                if (msleep(&msleep_pending, &msleep_mutex, 0, "spikew", (int)(hz * SPIKE_WAIT_TIMEOUT_MS / 1000U)) ==
                    EWOULDBLOCK)
                {
                    timed_out = 1;
                    break;
                }
            }
            msleep_pending = 0;
            mtx_unlock(&msleep_mutex);
            spike_plugin_note_wake(timed_out);
            spike_work();
        }
        else
        {
            pause("spikeidle", hz / 10);
        }
    }
}

static int spike_open_socket(void)
{
    int ret = socreate(AF_INET, &spike_socket, SOCK_DGRAM, IPPROTO_UDP, curthread->td_ucred, curthread);
    if (ret != 0)
    {
        spike_socket = NULL;
        return ret;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(SPIKE_UDP_PORT);
    ret = sobind(spike_socket, (struct sockaddr*)&address, curthread);
    if (ret != 0)
    {
        soclose(spike_socket);
        spike_socket = NULL;
        return ret;
    }
    SOCKBUF_LOCK(&spike_socket->so_rcv);
    soupcall_set(spike_socket, SO_RCV, spike_receive_upcall, NULL);
    SOCKBUF_UNLOCK(&spike_socket->so_rcv);
    return 0;
}

static int spike_load(void)
{
    if (spike_plugin_start() != 0)
    {
        slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[iosock_spike] plugin start failed\n");
        return EINVAL;
    }
    mtx_init(&msleep_mutex, "spike_msleep", NULL, MTX_DEF);
    int ret = spike_open_socket();
    if (ret != 0)
    {
        slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[iosock_spike] socket setup failed: %d\n", ret);
        return ret;
    }
    TASK_INIT(&spike_task, 0, spike_task_run, NULL);
    spike_taskqueue = taskqueue_create("spike_tq", M_WAITOK, taskqueue_thread_enqueue, &spike_taskqueue);
    ret = taskqueue_start_threads(&spike_taskqueue, 1, PWAIT, "spike_tq");
    if (ret != 0)
    {
        slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[iosock_spike] taskqueue_start_threads failed: %d\n", ret);
        return ret;
    }
    struct thread* thread = NULL;
    ret = kthread_add(spike_worker, NULL, NULL, &thread, 0, 0, "spike_worker");
    if (ret != 0)
    {
        slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[iosock_spike] kthread_add failed: %d\n", ret);
        return ret;
    }
    slogf(_SLOGC_NETWORK, _SLOG_INFO, "[iosock_spike] loaded, UDP port %u\n", SPIKE_UDP_PORT);
    return 0;
}

static int spike_module_event_handler(module_t mod __unused, int what, void* arg __unused)
{
    switch (what)
    {
        case MOD_LOAD:
            return spike_load();
        case MOD_SHUTDOWN:
            return 0;
        default:
            /* No coordinated stop of the worker, taskqueue and upcall: refuse unloading. */
            return EOPNOTSUPP;
    }
}

static moduledata_t spike_moduledata = {"iosock_spike", spike_module_event_handler, NULL};

MODULE_VERSION(iosock_spike, 1);
DECLARE_MODULE(iosock_spike, spike_moduledata, SI_SUB_PSEUDO, SI_ORDER_ANY);

struct _iosock_module_version iosock_module_version = IOSOCK_MODULE_VER_SYM_INIT;
