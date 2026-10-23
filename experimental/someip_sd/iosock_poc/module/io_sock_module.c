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
 * FreeBSD kernel module glue of the io-sock PoC: io-sock loads the shared object via
 * "mount -T io-sock <path>" and calls the module event handler. The module owns a kernel UDP
 * socket served by a kthread (FreeBSD kernel API); everything else lives in the C++ part
 * (poc_plugin.cpp), which only uses QNX/POSIX APIs.
 */

#include "experimental/someip_sd/iosock_poc/module/poc_plugin.h"

// clang-format off
// The devs/ FreeBSD kernel headers are not self-contained and must be included in this order.
#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/kthread.h>
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

#define POC_UDP_PORT 30700U
#define POC_RX_BUFFER_SIZE 2048U

static int poc_started = 0;
static struct socket* poc_socket = NULL;
static volatile int poc_rx_stop = 0;
static volatile int poc_rx_running = 0;

static int poc_set_socket_option(int level, int name, void* value, socklen_t size)
{
    struct sockopt option;
    memset(&option, 0, sizeof(option));
    option.sopt_dir = SOPT_SET;
    option.sopt_level = level;
    option.sopt_name = name;
    option.sopt_val = value;
    option.sopt_valsize = size;
    option.sopt_td = NULL; /* kernel-space value */
    return sosetopt(poc_socket, &option);
}

static int poc_open_socket(void)
{
    int ret = socreate(AF_INET, &poc_socket, SOCK_DGRAM, IPPROTO_UDP, curthread->td_ucred, curthread);
    if (ret != 0)
    {
        poc_socket = NULL;
        return ret;
    }

    /* Bounded soreceive() so that the kthread observes the stop flag. */
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
    ret = poc_set_socket_option(SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (ret != 0)
    {
        slogf(_SLOGC_NETWORK, _SLOG_WARNING, "[iosock_poc] SO_RCVTIMEO failed: %d\n", ret);
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(POC_UDP_PORT);
    ret = sobind(poc_socket, (struct sockaddr*)&address, curthread);
    if (ret != 0)
    {
        soclose(poc_socket);
        poc_socket = NULL;
    }
    return ret;
}

static void poc_tx_freed(struct mbuf* packet)
{
    iosock_poc_release_tx(packet->m_ext.ext_arg1);
}

static void poc_app_tx_freed(struct mbuf* packet)
{
    iosock_poc_release_app_tx(packet->m_ext.ext_arg1);
}

static void poc_send_app_sample(void)
{
    struct mbuf* packet = m_gethdr(M_NOWAIT, MT_DATA);
    if (packet == NULL)
    {
        return;
    }
    const unsigned char* payload = NULL;
    unsigned int size = 0;
    void* context = iosock_poc_take_app_tx(&payload, &size);
    if (context == NULL)
    {
        m_freem(packet);
        return;
    }
    MEXTADD(packet, (char*)payload, size, poc_app_tx_freed, context, NULL, M_RDONLY, EXT_MOD_TYPE);
    packet->m_len = size;
    packet->m_pkthdr.len = size;
    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_len = sizeof(destination);
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(30701U);
    const int ret = sosend(poc_socket, (struct sockaddr*)&destination, NULL, packet, NULL, 0, curthread);
    if (ret != 0)
    {
        slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[iosock_poc] application TX sosend failed: %d\n", ret);
    }
}

/* Receives datagrams, hands them to the C++ part (LoLa) and echoes them back to the sender. */
static void poc_rx_task(void* arg __unused)
{
    static char buffer[POC_RX_BUFFER_SIZE];
    slogf(_SLOGC_NETWORK, _SLOG_INFO, "[iosock_poc] rx kthread started\n");
    while (poc_rx_stop == 0)
    {
        poc_send_app_sample();
        struct iovec rx_iov = {.iov_base = buffer, .iov_len = sizeof(buffer)};
        struct uio rx_uio;
        memset(&rx_uio, 0, sizeof(rx_uio));
        rx_uio.uio_iov = &rx_iov;
        rx_uio.uio_iovcnt = 1;
        rx_uio.uio_resid = sizeof(buffer);
        rx_uio.uio_segflg = UIO_SYSSPACE;
        rx_uio.uio_rw = UIO_READ;
        rx_uio.uio_td = curthread;

        struct sockaddr* from = NULL;
        int flags = 0;
        int ret = soreceive(poc_socket, &from, &rx_uio, NULL, NULL, &flags);
        if ((ret != 0) || (from == NULL))
        {
            if (from != NULL)
            {
                free(from, M_SONAME);
            }
            if ((ret != EWOULDBLOCK) && (ret != EAGAIN) && (ret != 0))
            {
                slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[iosock_poc] soreceive failed: %d\n", ret);
                pause("pocrx", hz / 10);
            }
            continue;
        }

        const size_t received = sizeof(buffer) - (size_t)rx_uio.uio_resid;
        struct mbuf* packet = m_gethdr(M_NOWAIT, MT_DATA);
        const unsigned char* payload = NULL;
        void* context = packet == NULL ? NULL :
            iosock_poc_prepare_tx((const unsigned char*)buffer, (unsigned int)received, &payload);
        if (context == NULL)
        {
            if (packet != NULL)
            {
                m_freem(packet);
            }
            free(from, M_SONAME);
            continue;
        }
        MEXTADD(packet, (char*)payload, received, poc_tx_freed, context, NULL, M_RDONLY, EXT_MOD_TYPE);
        packet->m_len = received;
        packet->m_pkthdr.len = received;
        ret = sosend(poc_socket, from, NULL, packet, NULL, 0, curthread);
        if (ret != 0)
        {
            slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[iosock_poc] sosend failed: %d\n", ret);
        }
        free(from, M_SONAME);
    }
    slogf(_SLOGC_NETWORK, _SLOG_INFO, "[iosock_poc] rx kthread exiting\n");
    poc_rx_running = 0;
    kthread_exit();
}

static void poc_stop_socket(void)
{
    poc_rx_stop = 1;
    for (int waited_ms = 0; (poc_rx_running != 0) && (waited_ms < 2000); waited_ms += 100)
    {
        pause("pocstop", hz / 10);
    }
    if (poc_rx_running != 0)
    {
        slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[iosock_poc] rx kthread did not exit, socket left open\n");
        return;
    }
    if (poc_socket != NULL)
    {
        soclose(poc_socket);
        poc_socket = NULL;
    }
}

static int poc_start_socket(void)
{
    int ret = poc_open_socket();
    if (ret != 0)
    {
        slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[iosock_poc] kernel socket setup failed: %d\n", ret);
        return ret;
    }
    struct thread* thread = NULL;
    poc_rx_stop = 0;
    poc_rx_running = 1;
    ret = kthread_add(poc_rx_task, NULL, NULL, &thread, 0, 0, "iosock_poc_rx");
    if (ret != 0)
    {
        poc_rx_running = 0;
        soclose(poc_socket);
        poc_socket = NULL;
        slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[iosock_poc] kthread_add failed: %d\n", ret);
        return ret;
    }
    slogf(_SLOGC_NETWORK, _SLOG_INFO, "[iosock_poc] kernel UDP socket on port %u\n", POC_UDP_PORT);
    return 0;
}

static void poc_stop_once(void)
{
    if (poc_started != 0)
    {
        poc_started = 0;
        poc_stop_socket();
        iosock_poc_stop();
        slogf(_SLOGC_NETWORK, _SLOG_INFO, "[iosock_poc] stopped\n");
    }
}

static int poc_module_event_handler(module_t mod __unused, int what, void* arg __unused)
{
    switch (what)
    {
        case MOD_LOAD:
            if (iosock_poc_start() != 0)
            {
                slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[iosock_poc] start failed\n");
                return (EINVAL);
            }
            poc_started = 1;
            if (poc_start_socket() != 0)
            {
                poc_stop_once();
                return (EINVAL);
            }
            slogf(_SLOGC_NETWORK, _SLOG_INFO, "[iosock_poc] started\n");
            break;
        case MOD_UNLOAD:
        case MOD_SHUTDOWN:
        case MOD_QUIESCE:
            poc_stop_once();
            break;
        default:
            return (EOPNOTSUPP);
    }
    return (0);
}

static moduledata_t poc_moduledata = {"iosock_poc", poc_module_event_handler, NULL};

MODULE_VERSION(iosock_poc, 1);
DECLARE_MODULE(iosock_poc, poc_moduledata, SI_SUB_PSEUDO, SI_ORDER_ANY);

struct _iosock_module_version iosock_module_version = IOSOCK_MODULE_VER_SYM_INIT;
