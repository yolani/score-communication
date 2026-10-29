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
#include <machine/atomic.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/refcount.h>
#include <sys/sysctl.h>
#include <sys/kthread.h>
#include <qnx/qnx_modload.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/mbuf.h>
#include <netinet/in.h>
// clang-format on

#include "experimental/someip_sd/iosock_event_module/module/log.h"
#include "experimental/someip_sd/iosock_event_module/module/module.h"
#include "experimental/someip_sd/iosock_event_module/module/wakeup_signal.h"

/* Define a memory allocation type for the SOME/IP event module.
   Can be used with malloc() and free() for accounting purposes and
   to track memory usage specific to this module.
*/
MALLOC_DEFINE(M_SOMEIP_MODULE, "someip_module", "SOME/IP module");

/* compilation check to ensure SOME/IP header fits into a pkthdr mbuf and memcpy does not write beyond the mbuf */
_Static_assert(SOMEIP_HEADER_SIZE <= MHLEN, "SOME/IP header must fit into a pkthdr mbuf");

/* Upper bound of page-contiguous payload fragments per notification; covers payloads of up to 3 pages. */
#define MAX_PAYLOAD_FRAGMENTS 4U

/* Polling fallback until every source signals the kernel thread. */
#define WAKEUP_TIMEOUT_MS 10U

/* Payloads up to one cluster are copied: drivers may free TX mbufs seconds later (some EMACs reclaim lazily),
 * and a zero-copy reference keeps the LoLa sample, of which the proxy holds only a few, until then. */
#define TX_COPY_THRESHOLD ((unsigned int)MCLBYTES)

/* Shared by all fragment mbufs of one payload; the TX context is released when the last fragment is freed. */
struct tx_payload
{
    volatile u_int fragments;
    void* context;
};

/* list containing all open event sockets */
static struct event_socket* sockets;

/**
 * \private This function is intended for internal use within the module only.
 * \brief Find the socket associated with the given port in the list of open event sockets.
 *
 * \param port (port number to find the socket for)
 * \return pointer to the socket associated with the given port, or NULL if not found
 */
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

/**
 * \private
 * \brief Receive upcall; runs in the network stack with the receive buffer locked, so it only marks and signals.
 */
static int receive_upcall(struct socket* so __unused, void* arg, int waitflag __unused)
{
    struct event_socket* entry = arg;
    atomic_store_rel_int(&entry->rx_pending, 1U);
    someip_wakeup_signal(SOMEIP_WAKEUP_RX);
    return SU_OK;
}

/**
 * \api This function is part of the module's public API.
 * \brief Create a UDP kernel socket for the given port and add it to the list of open event sockets.
 *
 * \param port (port number to create the socket for)
 * \return 0 on success, or an error code on failure
 */
int someip_event_open_udp_socket(unsigned short port)
{
    if (find_socket(port) != NULL)
    {
        return 0;
    }
    struct event_socket* entry = malloc(
        sizeof(*entry),
        M_SOMEIP_MODULE,  /* type tag, see MALLOC_DEFINE() above , free() must use the same type */
        M_NOWAIT | M_ZERO /* Allocate memory without waiting, aka immediately return NULL on failure, and zero-initialize it */
    );
    if (entry == NULL)
    {
        someip_log(SOMEIP_LOG_ERROR, "Failed to allocate memory for event socket entry");
        return ENOMEM;
    }
    int ret = socreate(
        AF_INET,             /* Address family: IPv4 */
        &entry->socket,      /* Pointer to the socket structure to be created */
        SOCK_DGRAM,          /* Socket type: datagram (UDP) */
        IPPROTO_UDP,         /* Protocol: UDP */
        curthread->td_ucred, /* User credentials for the current thread */
        curthread            /* Process for the current thread */
    );

    if (ret != 0)
    {
        someip_log(SOMEIP_LOG_ERROR, "Failed to create UDP socket on port %u: %d", port, ret);
        free(entry, M_SOMEIP_MODULE);
        return ret;
    }
    /* TODO(production): revisit socket options before productization:
     * - SO_REUSEADDR: likely unnecessary (UDP has no TIME_WAIT; unload is refused). For UDP unicast it only
     *   permits overlapping binds on different local addresses, not two INADDR_ANY binds (needs SO_REUSEPORT),
     *   and can hide port conflicts. Remove unless such overlap is intended.
     * - SO_RCVBUF: size for event rate x event task latency x message size (max 32 datagrams/port/iteration);
     *   UDP silently drops on overflow. SO_SNDBUF: avoid ENOBUFS/EWOULDBLOCK on fan-out bursts.
     * - IP_TOS (DSCP) / IP_VLAN_PCP: network QoS, often mandated in vehicle Ethernet.
     * - IP_RECVDSTADDR / IP_RECVIF: per-datagram destination/interface when bound to INADDR_ANY (multi-homed/VLAN).
     * - SO_TIMESTAMP: optional RX timestamps for latency measurement.
     * - IP_ADD_MEMBERSHIP / IP_MULTICAST_IF (+ SO_REUSEPORT): only if multicast event delivery is added.
     * Also consider binding the SD-announced unicast IP instead of INADDR_ANY.
     */
    int enable = 1;
    struct sockopt option;
    memset(&option, 0, sizeof(option));
    option.sopt_dir = SOPT_SET;     /* Set a socket option, as opposed to sogetopt() */
    option.sopt_level = SOL_SOCKET; /* set option on socket level */
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
    else
    {
        someip_log(SOMEIP_LOG_ERROR, "Failed to set socket option SO_REUSEADDR on port %u: %d", port, ret);
    }
    if (ret != 0)
    {
        someip_log(SOMEIP_LOG_ERROR, "Failed to bind UDP socket on port %u: %d", port, ret);
        soclose(entry->socket);
        free(entry, M_SOMEIP_MODULE);
        return ret;
    }
    entry->port = port;
    /* Datagrams may have arrived before the upcall is installed; the event task drains them in this iteration. */
    entry->rx_pending = 1U;
    SOCKBUF_LOCK(&entry->socket->so_rcv);
    soupcall_set(entry->socket, SO_RCV, receive_upcall, entry);
    SOCKBUF_UNLOCK(&entry->socket->so_rcv);
    entry->next = sockets;
    sockets = entry;
    return 0;
}

/**
 * \api
 * \brief Copy data from an mbuf chain representing a SOME/IP event packet to a destination buffer.
 * \param packet Pointer to the SOME/IP event packet.
 * \param offset Offset within the packet to start copying from.
 * \param size Number of bytes to copy.
 * \param destination Destination buffer to copy the data into.
 */
void someip_event_copy_packet(const void* packet, unsigned int offset, unsigned int size, void* destination)
{
    m_copydata((const struct mbuf*)packet, (int)offset, (int)size, (char*)destination);
}

/**
 * \private
 * \brief Callback invoked when a payload fragment mbuf is freed; releases the TX context with the last fragment.
 * \param packet Pointer to the mbuf being freed.
 */
static void payload_freed(struct mbuf* packet)
{
    struct tx_payload* tx = packet->m_ext.ext_arg1;
    if (refcount_release(&tx->fragments))
    {
        someip_event_release_tx(tx->context);
        free(tx, M_SOMEIP_MODULE);
    }
}

/**
 * \private
 * \brief Number of virtual pages spanned by a buffer.
 */
static unsigned int page_count(const unsigned char* buffer, unsigned int size)
{
    const uintptr_t first = (uintptr_t)buffer >> PAGE_SHIFT;
    const uintptr_t last = ((uintptr_t)buffer + size - 1U) >> PAGE_SHIFT;
    return (unsigned int)(last - first) + 1U;
}

/**
 * \api
 * \brief Send a SOME/IP event packet to a remote destination.
 * \param local_port Local UDP port to send from.
 * \param remote_ip Remote IP address to send to.
 * \param remote_port Remote UDP port to send to.
 * \param header SOME/IP header to include in the packet.
 * \param payload Pointer to the payload data.
 * \param size Size of the payload data.
 * \param context Context pointer passed to the payload release callback.
 * \return 0 on success, or an error code on failure.
 */
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
        someip_log(SOMEIP_LOG_ERROR, "No socket found for local port %u", local_port);
        someip_event_release_tx(context);
        return ENOTCONN;
    }
    struct mbuf* packet = m_gethdr(M_NOWAIT, MT_DATA);
    if (packet == NULL)
    {
        someip_log(SOMEIP_LOG_ERROR, "Failed to allocate mbuf for packet");
        someip_event_release_tx(context);
        return ENOMEM;
    }
    /* Place header at the end of the mbuf so IP/UDP/link headers can be prepended without another allocation. */
    M_ALIGN(packet, SOMEIP_HEADER_SIZE);
    memcpy(mtod(packet, void*), header, SOMEIP_HEADER_SIZE_U);
    packet->m_len = SOMEIP_HEADER_SIZE;
    packet->m_pkthdr.len = SOMEIP_HEADER_SIZE_U + size;

    if (size == 0U)
    {
        someip_event_release_tx(context);
    }
    else if (size <= TX_COPY_THRESHOLD)
    {
        /* m_append adds the appended length to m_pkthdr.len itself. */
        packet->m_pkthdr.len = SOMEIP_HEADER_SIZE;
        const int appended = m_append(packet, (int)size, (c_caddr_t)payload);
        someip_event_release_tx(context);
        if (appended == 0)
        {
            someip_log(SOMEIP_LOG_ERROR, "Failed to copy %u payload bytes", size);
            m_freem(packet);
            return ENOMEM;
        }
    }
    else
    {
        /* Drivers resolve the physical address of each external buffer (m_phys) and panic if it is not physically
         * contiguous; shared memory is only contiguous within a page, so attach one fragment per page. */
        const unsigned int fragments = page_count(payload, size);
        struct mbuf* data[MAX_PAYLOAD_FRAGMENTS] = {NULL};
        struct tx_payload* tx = NULL;
        int allocated = (fragments <= MAX_PAYLOAD_FRAGMENTS) ? 1 : 0;
        for (unsigned int index = 0U; (allocated != 0) && (index < fragments); ++index)
        {
            data[index] = m_get(M_NOWAIT, MT_DATA);
            allocated = (data[index] != NULL) ? 1 : 0;
        }
        if (allocated != 0)
        {
            tx = malloc(sizeof(*tx), M_SOMEIP_MODULE, M_NOWAIT);
            allocated = (tx != NULL) ? 1 : 0;
        }
        if (allocated == 0)
        {
            someip_log(SOMEIP_LOG_ERROR, "Failed to allocate %u payload fragment(s)", fragments);
            for (unsigned int index = 0U; index < MAX_PAYLOAD_FRAGMENTS; ++index)
            {
                if (data[index] != NULL)
                {
                    m_free(data[index]);
                }
            }
            m_freem(packet);
            someip_event_release_tx(context);
            return (fragments <= MAX_PAYLOAD_FRAGMENTS) ? ENOMEM : EMSGSIZE;
        }
        tx->context = context;
        refcount_init(&tx->fragments, fragments);

        struct mbuf* tail = packet;
        const unsigned char* fragment = payload;
        unsigned int remaining = size;
        for (unsigned int index = 0U; index < fragments; ++index)
        {
            const unsigned int to_page_end = PAGE_SIZE - (unsigned int)((uintptr_t)fragment & PAGE_MASK);
            const unsigned int length = MIN(remaining, to_page_end);
            // inject payload pointer into mbuf using external storage mechanism
            MEXTADD(
                data[index],          /* mbuf to attach external storage to */
                (char*)fragment,      /* pointer to the external storage */
                length,               /* size of the external storage */
                payload_freed,        /* callback to free the external storage */
                tx,                   /* shared fragment counter and TX context */
                NULL,                 /* unused */
                M_RDONLY,             /* read-only flag */
                EXT_MOD_TYPE          /* type of external storage */
            );
            data[index]->m_len = (int)length;
            tail->m_next = data[index];
            tail = data[index];
            fragment += length;
            remaining -= length;
        }
    }

    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_len = sizeof(destination);
    destination.sin_family = AF_INET;
    memcpy(&destination.sin_addr.s_addr, remote_ip, 4U);
    destination.sin_port = htons(remote_port);
    /* sosend consumes the entire chain, including on error. */
    return sosend(
        socket,
        (struct sockaddr*)&destination,
        NULL /* unused uio */,
        packet,
        NULL, /* unused control data */
        MSG_DONTWAIT, /* non-blocking send */
        curthread
    );
}

/**
 * \private
 * \brief Receives datagrams from the specified event socket, handles them appropriately and frees resources as needed.
 * \param entry The event socket to receive datagrams from.
 * \return 1 if the per-port budget was used up before the receive buffer was empty, 0 otherwise.
 */
static int receive_udp_datagrams(struct event_socket* entry)
{
    /* Bound work per port so a busy RX socket cannot starve control commands or TX. */
    for (unsigned int count = 0; count < MAX_DIAGRAMS_RECEIVED_PER_PORT; ++count)
    {
        struct uio receive;
        memset(&receive, 0, sizeof(receive));
        receive.uio_resid = MAX_DATAGRAM_SIZE; /* maximum datagram size */
        receive.uio_segflg = UIO_SYSSPACE; /* data is in kernel space */
        receive.uio_rw = UIO_READ; /* read operation */
        receive.uio_td = curthread;
        struct sockaddr* from = NULL;
        struct mbuf* packet = NULL;
        int flags = MSG_DONTWAIT;
        const int ret = soreceive(entry->socket, &from, &receive, &packet, NULL, &flags);
        const unsigned int length = (packet != NULL) ? m_length(packet, NULL) : 0U;
        if (   (ret == 0)      /* successful receive */
            && (from != NULL)  /* sender address is always set for UDP; guards the dereference below */
            && (length >= SOMEIP_HEADER_SIZE_U) /* drops empty datagrams and those without a full SOME/IP header */
            && ((flags & MSG_TRUNC) == 0) /* datagram was not truncated */
        )
        {
            const struct sockaddr_in* sender = (const struct sockaddr_in*)from;
            someip_event_datagram(entry->port,
                                  (const unsigned char*)&sender->sin_addr.s_addr,
                                  ntohs(sender->sin_port),
                                  packet,
                                  length);
        }
        else if ((ret == 0) && ((flags & MSG_TRUNC) != 0))
        {
            /* MAX_DATAGRAM_SIZE exceeded */
            someip_log(SOMEIP_LOG_WARNING, "truncated datagram on port %u", entry->port);
        }
        else if ((ret == 0) && (length < SOMEIP_HEADER_SIZE_U))
        {
            /* Datagram too short */
            someip_log(SOMEIP_LOG_WARNING, "short datagram on port %u", entry->port);
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
            if ((ret != EWOULDBLOCK) && (ret != EAGAIN)) /* Ignore non-fatal errors, aka nothing received */
            {
                someip_log(SOMEIP_LOG_ERROR, "soreceive port %u failed: %d", entry->port, ret);
            }
            return 0; /* receive bufffer empty, nothing more to read */
        }
    }
    return 1;
}

/**
 * \private
 * \brief Event task that handles incoming and outgoing SOME/IP datagrams.
 */
static void event_task(void* arg __unused)
{
    someip_log(SOMEIP_LOG_INFO, "event task started");

    /* TODO: currently one loop handling control channel, rx, tx and sleep at the end
     * this needs to be properly designed, probably
     * - exclude control channel from the thread
     * - wake up tx handling through lola receive callback
     * - use asynchronous lola service discovery in control channel
     */
    for (;;)
    {
        /* TODO: someip_event_poll() actually handles control channel messages first and then
         * forwards lola samples to someip. This should probably be split up, made clear through
         * proper function names etc.
         */
        someip_event_poll(); /* Tx and control channel handling */
        for (struct event_socket* entry = sockets; entry != NULL; entry = entry->next)
        {
            /* Cleared before draining, so datagrams arriving meanwhile mark the socket again. */
            if ((atomic_readandclear_int(&entry->rx_pending) != 0U) && (receive_udp_datagrams(entry) != 0))
            {
                atomic_store_rel_int(&entry->rx_pending, 1U);
                someip_wakeup_signal(SOMEIP_WAKEUP_RX);
            }
        }
        (void)someip_wakeup_wait(WAKEUP_TIMEOUT_MS);
    }
}

static int event_module_handler(module_t mod __unused, int what, void* arg __unused)
{
    switch (what)
    {
        case MOD_LOAD:
        {
            const int wakeup_error = someip_wakeup_init();
            if (wakeup_error != 0)
            {
                someip_log(SOMEIP_LOG_ERROR, "wakeup signal initialization failed: %d", wakeup_error);
                return wakeup_error;
            }
            const int ret = someip_event_start();
            if (ret != 0)
            {
                someip_log(SOMEIP_LOG_ERROR, "initialization failed: %d", ret);
                return ret;
            }
            else
            {
                someip_log(SOMEIP_LOG_INFO, "initialization succeeded");
            }
            struct thread* thread = NULL;
            const int thread_error = kthread_add(event_task, NULL, NULL, &thread, 0, 0, "someip_event");
            if (thread_error != 0)
            {
                someip_log(SOMEIP_LOG_ERROR, "kthread_add failed: %d", thread_error);
                someip_event_abort_start();
            }
            return thread_error;
        }
        case MOD_QUIESCE:
        case MOD_UNLOAD:
            /* This module has no coordinated worker stop or mbuf callback drain; refuse unloading. */
            return EOPNOTSUPP;
        case MOD_SHUTDOWN:
            return 0;
        default:
            return EOPNOTSUPP;
    }
}

/* Module name, lifecycle event handler and optional handler-private data (unused here). */
static moduledata_t someip_moduledata = {"someip", event_module_handler, NULL};

/* Our module's revision in FreeBSD module metadata, not the io-sock API version. */
MODULE_VERSION(someip, 1);

/* Register metadata and a SYSINIT entry that registers the module and invokes MOD_LOAD.
 * SI_SUB_PSEUDO selects the pseudo-device phase; SI_ORDER_ANY sorts last within that phase.
 */
DECLARE_MODULE(someip, someip_moduledata, SI_SUB_PSEUDO, SI_ORDER_ANY);

/* Required exported symbol used by the io-sock loader for compatibility checks.
 * The SDK initializer supplies provides=IOSOCK_VERSION_CUR, requires=IOSOCK_VERSION_MIN,
 * the generic module type and build flags (including INVARIANTS).
 */
struct _iosock_module_version iosock_module_version = IOSOCK_MODULE_VER_SYM_INIT;

/* Diagnostic SDK API version only; this variable does not control loader compatibility. */
static int someip_module_version = IOSOCK_VERSION_CUR;

/* Expose qnx.module.someip as a read-only integer with an automatically assigned OID.
 * The value comes from someip_module_version; the constant-value argument (0) is unused
 * when a variable pointer is supplied. The final string is the sysctl description.
 */
SYSCTL_INT(_qnx_module, OID_AUTO, someip, CTLFLAG_RD, &someip_module_version, 0, "Version");

static void someip_module_uninit(void* arg __unused)
{
    /* TODO: enable module unloading
    * If code is added to sam_uninit, then SI_SUB_DUMMY needs to be
    * changed to SI_SUB_DRIVERS.  With SI_SUB_DUMMY, sam_uninit does
    * not get called.
    */
}

/* QNX sample-style linker-set placeholder: SI_SUB_DUMMY prevents this callback from running.
 * An active hook (e.g. SI_SUB_DRIVERS) requires coordinated worker shutdown and callback draining
 * before unloading can be allowed. SI_ORDER_ANY specifies its order; NULL is callback-private data.
 */
SYSUNINIT(someip_module_uninit, SI_SUB_DUMMY, SI_ORDER_ANY, someip_module_uninit, NULL);
