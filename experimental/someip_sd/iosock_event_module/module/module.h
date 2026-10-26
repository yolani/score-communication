#ifndef MODULE_H
#define MODULE_H

#include <sys/uio.h>

#define SOMEIP_HEADER_SIZE 16
#define SOMEIP_HEADER_SIZE_U 16U

#define MAX_DIAGRAMS_RECEIVED_PER_PORT 32U
#define MAX_DATAGRAM_SIZE 65536U

/* The SDK forward-declares uio; this is the FreeBSD 13.5 layout used by io-sock. */
enum uio_rw /* https://github.com/freebsd/freebsd-src/blob/stable/13/sys/sys/_uio.h#L38 */
{
    UIO_READ,
    UIO_WRITE
};

// https://github.com/freebsd/freebsd-src/blob/stable/13/sys/sys/_uio.h#L44
enum uio_seg
{
    UIO_USERSPACE,
    UIO_SYSSPACE,
    UIO_NOCOPY
};

// https://github.com/freebsd/freebsd-src/blob/stable/13/sys/sys/uio.h#L54
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

// single linked list of event sockets, searchable by port
struct event_socket
{
    unsigned short port;
    struct socket* socket;
    struct event_socket* next;
};

#endif // MODULE_H