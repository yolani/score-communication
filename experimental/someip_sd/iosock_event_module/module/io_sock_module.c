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
#include "experimental/someip_sd/iosock_event_module/module/event_plugin.h"

// clang-format off
/* io-sock's kernel headers require this include order. */
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

/* The SDK forward-declares uio; this is the FreeBSD 13.5 layout used by io-sock. */
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

struct event_socket
{
    unsigned short port;
    struct socket* socket;
    struct event_socket* next;
};

static struct event_socket* sockets;

static struct socket* find_socket(unsigned short port)
{
    for (struct event_socket* entry = sockets; entry != NULL; entry = entry->next)
    {
        if (entry->port == port)
        {
            return entry->socket;
        }
    }
    return NULL;
}

int someip_event_open_socket(unsigned short port)
{
    if (find_socket(port) != NULL)
    {
        return 0;
    }
    struct event_socket* entry = malloc(sizeof(*entry), M_TEMP, M_NOWAIT | M_ZERO);
    if (entry == NULL)
    {
        return ENOMEM;
    }
    int ret = socreate(AF_INET, &entry->socket, SOCK_DGRAM, IPPROTO_UDP, curthread->td_ucred, curthread);
    if (ret != 0)
    {
        free(entry, M_TEMP);
        return ret;
    }
    int enable = 1;
    struct sockopt option;
    memset(&option, 0, sizeof(option));
    option.sopt_dir = SOPT_SET;
    option.sopt_level = SOL_SOCKET;
    option.sopt_name = SO_REUSEADDR;
    option.sopt_val = &enable;
    option.sopt_valsize = sizeof(enable);
    ret = sosetopt(entry->socket, &option);
    if (ret == 0)
    {
        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_len = sizeof(address);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        ret = sobind(entry->socket, (struct sockaddr*)&address, curthread);
    }
    if (ret != 0)
    {
        soclose(entry->socket);
        free(entry, M_TEMP);
        return ret;
    }
    entry->port = port;
    entry->next = sockets;
    sockets = entry;
    return 0;
}

void someip_event_copy_packet(const void* packet, unsigned int offset, unsigned int size, void* destination)
{
    m_copydata((const struct mbuf*)packet, (int)offset, (int)size, (char*)destination);
}

static void payload_freed(struct mbuf* packet)
{
    someip_event_release_tx(packet->m_ext.ext_arg1);
}

int someip_event_send(unsigned short local_port,
                      const unsigned char remote_ip[4],
                      unsigned short remote_port,
                      const unsigned char header[16],
                      const unsigned char* payload,
                      unsigned int size,
                      void* context)
{
    struct socket* socket = find_socket(local_port);
    if (socket == NULL)
    {
        someip_event_release_tx(context);
        return ENOTCONN;
    }
    struct mbuf* packet = m_gethdr(M_NOWAIT, MT_DATA);
    struct mbuf* data = m_get(M_NOWAIT, MT_DATA);
    if ((packet == NULL) || (data == NULL))
    {
        if (packet != NULL)
        {
            m_freem(packet);
        }
        if (data != NULL)
        {
            m_freem(data);
        }
        someip_event_release_tx(context);
        return ENOMEM;
    }
    memcpy(mtod(packet, void*), header, 16U);
    packet->m_len = 16;
    packet->m_pkthdr.len = 16U + size;
    MEXTADD(data, (char*)payload, MAX(1U, size), payload_freed, context, NULL, M_RDONLY, EXT_MOD_TYPE);
    data->m_len = size;
    packet->m_next = data;
    if (mtod(data, const unsigned char*) != payload)
    {
        m_freem(packet);
        return EFAULT;
    }

    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_len = sizeof(destination);
    destination.sin_family = AF_INET;
    memcpy(&destination.sin_addr.s_addr, remote_ip, 4U);
    destination.sin_port = htons(remote_port);
    /* sosend consumes the entire chain, including on error. */
    return sosend(socket, (struct sockaddr*)&destination, NULL, packet, NULL, MSG_DONTWAIT, curthread);
}

static void receive_datagrams(struct event_socket* entry)
{
    /* Bound work per port so a busy RX socket cannot starve control commands or TX. */
    for (unsigned int count = 0; count < 32U; ++count)
    {
        struct uio receive;
        memset(&receive, 0, sizeof(receive));
        receive.uio_resid = 65536;
        receive.uio_segflg = UIO_SYSSPACE;
        receive.uio_rw = UIO_READ;
        receive.uio_td = curthread;
        struct sockaddr* from = NULL;
        struct mbuf* packet = NULL;
        int flags = MSG_DONTWAIT;
        const int ret = soreceive(entry->socket, &from, &receive, &packet, NULL, &flags);
        if ((ret == 0) && (packet != NULL) && (from != NULL) && (from->sa_family == AF_INET) &&
            (from->sa_len >= sizeof(struct sockaddr_in)) && ((flags & MSG_TRUNC) == 0))
        {
            const struct sockaddr_in* sender = (const struct sockaddr_in*)from;
            someip_event_datagram(entry->port,
                                  (const unsigned char*)&sender->sin_addr.s_addr,
                                  ntohs(sender->sin_port),
                                  packet,
                                  m_length(packet, NULL));
        }
        else if ((ret == 0) && ((flags & MSG_TRUNC) != 0))
        {
            slogf(_SLOGC_NETWORK, _SLOG_WARNING, "[someip_event] truncated datagram on port %u", entry->port);
        }
        if (packet != NULL)
        {
            m_freem(packet);
        }
        if (from != NULL)
        {
            free(from, M_SONAME);
        }
        if (ret != 0)
        {
            if ((ret != EWOULDBLOCK) && (ret != EAGAIN))
            {
                slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[someip_event] soreceive port %u failed: %d", entry->port, ret);
            }
            break;
        }
    }
}

static void event_task(void* arg __unused)
{
    someip_event_ready();
    for (;;)
    {
        someip_event_poll();
        for (struct event_socket* entry = sockets; entry != NULL; entry = entry->next)
        {
            receive_datagrams(entry);
        }
        pause("someipevent", MAX(1, hz / 100));
    }
}

static int event_module_handler(module_t mod __unused, int what, void* arg __unused)
{
    switch (what)
    {
        case MOD_LOAD:
        {
            const int ret = someip_event_start();
            if (ret != 0)
            {
                slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[someip_event] initialization failed: %d", ret);
                return ret;
            }
            struct thread* thread = NULL;
            const int thread_error = kthread_add(event_task, NULL, NULL, &thread, 0, 0, "someip_event");
            if (thread_error != 0)
            {
                slogf(_SLOGC_NETWORK, _SLOG_ERROR, "[someip_event] kthread_add failed: %d", thread_error);
                someip_event_abort_start();
            }
            return thread_error;
        }
        case MOD_QUIESCE:
        case MOD_UNLOAD:
            /* QNX does not support generic-module detach; never unload live mbuf callbacks. */
            return EOPNOTSUPP;
        case MOD_SHUTDOWN:
            return 0;
        default:
            return EOPNOTSUPP;
    }
}

static moduledata_t event_moduledata = {"someip_event", event_module_handler, NULL};
MODULE_VERSION(someip_event, 1);
DECLARE_MODULE(someip_event, event_moduledata, SI_SUB_PSEUDO, SI_ORDER_ANY);
struct _iosock_module_version iosock_module_version = IOSOCK_MODULE_VER_SYM_INIT;
