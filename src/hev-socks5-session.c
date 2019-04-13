/*
 ============================================================================
 Name        : hev-socks5-session.c
 Author      : Heiher <r@hev.cc>
 Copyright   : Copyright (c) 2017 - 2019 everyone.
 Description : Socks5 session
 ============================================================================
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/netfilter_ipv4.h>

#include "hev-socks5-session.h"
#include "hev-memory-allocator.h"
#include "hev-task.h"
#include "hev-task-io.h"
#include "hev-task-io-socket.h"
#include "hev-config.h"

#define SESSION_HP (10)
#define TASK_STACK_SIZE (8192)

static void hev_socks5_session_task_entry (void *data);

typedef struct _Socks5AuthHeader Socks5AuthHeader;
typedef struct _Socks5ReqResHeader Socks5ReqResHeader;

enum
{
    STEP_NULL,
    STEP_DO_CONNECT,
    STEP_WRITE_REQUEST,
    STEP_READ_RESPONSE,
    STEP_DO_SPLICE,
    STEP_DO_FWD_DNS,
    STEP_CLOSE_SESSION,
};

struct _HevSocks5Session
{
    HevSocks5SessionBase base;

    int client_fd;
    int remote_fd;
    int ref_count;

    int dns_request_len;
    void *dns_request;
    struct sockaddr_in6 addr;

    HevSocks5SessionCloseNotify notify;
    void *notify_data;
};

struct _Socks5AuthHeader
{
    uint8_t ver;
    union
    {
        uint8_t method;
        uint8_t method_len;
    };
    uint8_t methods[256];
} __attribute__ ((packed));

struct _Socks5ReqResHeader
{
    uint8_t ver;
    union
    {
        uint8_t cmd;
        uint8_t rep;
    };
    uint8_t rsv;
    uint8_t atype;
    union
    {
        struct
        {
            uint32_t addr;
            uint16_t port;
        } ipv4;
        struct
        {
            uint8_t addr[16];
            uint16_t port;
        } ipv6;
        struct
        {
            uint8_t len;
            uint8_t addr[256 + 2];
        } domain;
    };
} __attribute__ ((packed));

static HevSocks5Session *
hev_socks5_session_new (int client_fd, HevSocks5SessionCloseNotify notify,
                        void *notify_data)
{
    HevSocks5Session *self;
    HevTask *task;

    self = hev_malloc0 (sizeof (HevSocks5Session));
    if (!self)
        return NULL;

    self->base.hp = SESSION_HP;

    self->ref_count = 1;
    self->remote_fd = -1;
    self->client_fd = client_fd;
    self->notify = notify;
    self->notify_data = notify_data;

    task = hev_task_new (TASK_STACK_SIZE);
    if (!task) {
        hev_free (self);
        return NULL;
    }

    self->base.task = task;
    hev_task_set_priority (task, 9);

    return self;
}

HevSocks5Session *
hev_socks5_session_new_tcp (int client_fd, HevSocks5SessionCloseNotify notify,
                            void *notify_data)
{
    HevSocks5Session *self;
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    struct sockaddr *addr;
    socklen_t addr_len;
    const int sopt = SO_ORIGINAL_DST;

    self = hev_socks5_session_new (client_fd, notify, notify_data);
    if (!self)
        return NULL;

    /* get socket address */
    addr = (struct sockaddr *)&addr6;
    addr_len = sizeof (addr6);
    if (getsockname (client_fd, addr, &addr_len) < 0) {
        hev_task_unref (self->base.task);
        hev_free (self);
        return NULL;
    }

    /* get original address */
    addr = (struct sockaddr *)&addr4;
    addr_len = sizeof (addr4);
    if (getsockopt (client_fd, SOL_IP, sopt, addr, &addr_len) == 0) {
        self->addr.sin6_port = addr4.sin_port;
        ((uint16_t *)&self->addr.sin6_addr)[5] = 0xffff;
        ((uint32_t *)&self->addr.sin6_addr)[3] = addr4.sin_addr.s_addr;
    } else {
        addr = (struct sockaddr *)&self->addr;
        addr_len = sizeof (self->addr);
        if (getsockopt (client_fd, SOL_IPV6, sopt, addr, &addr_len) < 0) {
            hev_task_unref (self->base.task);
            hev_free (self);
            return NULL;
        }
    }

    /* check is connect to self */
    if ((addr6.sin6_port == self->addr.sin6_port) &&
        (0 == __builtin_memcmp (&addr6.sin6_addr, &self->addr.sin6_addr, 16))) {
        hev_task_unref (self->base.task);
        hev_free (self);
        return NULL;
    }

    return self;
}

HevSocks5Session *
hev_socks5_session_new_dns (int client_fd, HevSocks5SessionCloseNotify notify,
                            void *notify_data)
{
    HevSocks5Session *self;
    struct sockaddr *addr;
    socklen_t addr_len;
    ssize_t s;

    self = hev_socks5_session_new (client_fd, notify, notify_data);
    if (!self)
        return NULL;

    self->dns_request = hev_malloc (2048);
    if (!self->dns_request) {
        hev_task_unref (self->base.task);
        hev_free (self);
        return NULL;
    }

    /* recv dns request */
    addr = (struct sockaddr *)&self->addr;
    addr_len = sizeof (self->addr);
    s = recvfrom (client_fd, self->dns_request, 2048, 0, addr, &addr_len);
    if (s == -1) {
        hev_free (self->dns_request);
        hev_task_unref (self->base.task);
        hev_free (self);
        return NULL;
    }

#ifdef _DEBUG
    {
        char buf[64], *sa = NULL;
        uint16_t port = 0;
        if (sizeof (self->addr) == addr_len) {
            sa = inet_ntop (AF_INET6, &self->addr.sin6_addr, buf, sizeof (buf));
            port = ntohs (self->addr.sin6_port);
        }
        printf ("Receive a DNS message from [%s]:%u\n", sa, port);
    }
#endif
    self->dns_request_len = s;

    return self;
}

HevSocks5Session *
hev_socks5_session_ref (HevSocks5Session *self)
{
    self->ref_count++;

    return self;
}

void
hev_socks5_session_unref (HevSocks5Session *self)
{
    self->ref_count--;
    if (self->ref_count)
        return;

    if (self->dns_request)
        hev_free (self->dns_request);
    hev_free (self);
}

void
hev_socks5_session_run (HevSocks5Session *self)
{
    hev_task_run (self->base.task, hev_socks5_session_task_entry, self);
}

static int
socks5_session_task_io_yielder (HevTaskYieldType type, void *data)
{
    HevSocks5Session *self = data;

    self->base.hp = SESSION_HP;

    hev_task_yield (type);

    return (self->base.hp > 0) ? 0 : -1;
}

static int
socks5_do_connect (HevSocks5Session *self)
{
    HevTask *task;
    struct sockaddr_in6 addr6 = { 0 };
    struct sockaddr *addr = (struct sockaddr *)&addr6;
    const socklen_t addr_len = sizeof (addr6);
    const char *address = hev_config_get_socks5_address ();

    self->remote_fd = hev_task_io_socket_socket (AF_INET6, SOCK_STREAM, 0);
    if (self->remote_fd == -1)
        return STEP_CLOSE_SESSION;

    task = hev_task_self ();
    hev_task_add_fd (task, self->remote_fd, POLLIN | POLLOUT);

    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons (hev_config_get_socks5_port ());
    if (inet_pton (AF_INET, address, &addr6.sin6_addr.s6_addr[12]) == 1) {
        ((uint16_t *)&addr6.sin6_addr)[5] = 0xffff;
    } else {
        if (inet_pton (AF_INET6, address, &addr6.sin6_addr) != 1)
            return STEP_CLOSE_SESSION;
    }

    /* connect */
    if (hev_task_io_socket_connect (self->remote_fd, addr, addr_len,
                                    socks5_session_task_io_yielder, self) < 0)
        return STEP_CLOSE_SESSION;

    return STEP_WRITE_REQUEST;
}

static int
socks5_write_request (HevSocks5Session *self)
{
    Socks5AuthHeader socks5_auth;
    Socks5ReqResHeader socks5_r;
    struct msghdr mh;
    struct iovec iov[2];
    ssize_t len;

    memset (&mh, 0, sizeof (mh));
    mh.msg_iov = iov;
    mh.msg_iovlen = 2;

    /* write socks5 auth method */
    socks5_auth.ver = 0x05;
    socks5_auth.method_len = 0x01;
    socks5_auth.methods[0] = 0x00;
    iov[0].iov_base = &socks5_auth;
    iov[0].iov_len = 3;

    /* write socks5 request */
    socks5_r.ver = 0x05;
    if (self->dns_request_len)
        socks5_r.cmd = 0x04;
    else
        socks5_r.cmd = 0x01;
    socks5_r.rsv = 0x00;
    if (IN6_IS_ADDR_V4MAPPED (&self->addr.sin6_addr)) {
        len = 10;
        socks5_r.atype = 0x01;
        socks5_r.ipv4.port = self->addr.sin6_port;
        socks5_r.ipv4.addr = ((uint32_t *)&self->addr.sin6_addr)[3];
    } else {
        len = 22;
        socks5_r.atype = 0x04;
        socks5_r.ipv6.port = self->addr.sin6_port;
        __builtin_memcpy (&socks5_r.ipv6.addr, &self->addr.sin6_addr, 16);
    }
    iov[1].iov_base = &socks5_r;
    iov[1].iov_len = len;

    len = hev_task_io_socket_sendmsg (self->remote_fd, &mh, MSG_WAITALL,
                                      socks5_session_task_io_yielder, self);
    if (len <= 0)
        return STEP_CLOSE_SESSION;

    return STEP_READ_RESPONSE;
}

static int
socks5_read_response (HevSocks5Session *self)
{
    Socks5AuthHeader socks5_auth;
    Socks5ReqResHeader socks5_r;
    ssize_t len;

    /* read socks5 auth method */
    len = hev_task_io_socket_recv (self->remote_fd, &socks5_auth, 2,
                                   MSG_WAITALL, socks5_session_task_io_yielder,
                                   self);
    if (len <= 0)
        return STEP_CLOSE_SESSION;
    /* check socks5 version and auth method */
    if (socks5_auth.ver != 0x05 || socks5_auth.method != 0x00)
        return STEP_CLOSE_SESSION;

    /* read socks5 response header */
    len = hev_task_io_socket_recv (self->remote_fd, &socks5_r, 4, MSG_WAITALL,
                                   socks5_session_task_io_yielder, self);
    if (len <= 0)
        return STEP_CLOSE_SESSION;
    /* check socks5 version, rep */
    if (socks5_r.ver != 0x05 || socks5_r.rep != 0x00)
        return STEP_CLOSE_SESSION;

    switch (socks5_r.atype) {
    case 0x01:
        len = 6;
        break;
    case 0x04:
        len = 18;
        break;
    default:
        return STEP_CLOSE_SESSION;
    }

    /* read socks5 response body */
    len = hev_task_io_socket_recv (self->remote_fd, &socks5_r, len, MSG_WAITALL,
                                   socks5_session_task_io_yielder, self);
    if (len <= 0)
        return STEP_CLOSE_SESSION;

    return (self->dns_request_len) ? STEP_DO_FWD_DNS : STEP_DO_SPLICE;
}

static int
socks5_do_splice (HevSocks5Session *self)
{
    hev_task_io_splice (self->client_fd, self->client_fd, self->remote_fd,
                        self->remote_fd, 8192, socks5_session_task_io_yielder,
                        self);

    return STEP_CLOSE_SESSION;
}

static int
socks5_do_fwd_dns (HevSocks5Session *self)
{
    uint8_t buf[2048];
    ssize_t len;
    struct msghdr mh;
    struct iovec iov[2];
    struct sockaddr *addr = (struct sockaddr *)&self->addr;
    const socklen_t addr_len = sizeof (self->addr);
    uint16_t dns_len;

    memset (&mh, 0, sizeof (mh));
    mh.msg_iov = iov;
    mh.msg_iovlen = 2;

    /* write dns request length */
    dns_len = htons (self->dns_request_len);
    iov[0].iov_base = &dns_len;
    iov[0].iov_len = 2;

    /* write dns request */
    iov[1].iov_base = self->dns_request;
    iov[1].iov_len = self->dns_request_len;

    /* send dns request */
    len = hev_task_io_socket_sendmsg (self->remote_fd, &mh, MSG_WAITALL,
                                      socks5_session_task_io_yielder, self);
    if (len <= 0)
        return STEP_CLOSE_SESSION;

    /* read dns response length */
    len = hev_task_io_socket_recv (self->remote_fd, &dns_len, 2, MSG_WAITALL,
                                   socks5_session_task_io_yielder, self);
    if (len <= 0)
        return STEP_CLOSE_SESSION;
    dns_len = ntohs (dns_len);

    /* check dns response length */
    if (dns_len >= 2048)
        return STEP_CLOSE_SESSION;

    /* read dns response */
    len = hev_task_io_socket_recv (self->remote_fd, buf, dns_len, MSG_WAITALL,
                                   socks5_session_task_io_yielder, self);
    if (len <= 0)
        return STEP_CLOSE_SESSION;

    /* send dns response */
    hev_task_io_socket_sendto (self->client_fd, buf, len, 0, addr, addr_len,
                               socks5_session_task_io_yielder, self);

    return STEP_CLOSE_SESSION;
}

static int
socks5_close_session (HevSocks5Session *self)
{
    if (self->remote_fd >= 0)
        close (self->remote_fd);
    if (self->dns_request_len == 0)
        close (self->client_fd);

    self->notify (self, self->notify_data);
    hev_socks5_session_unref (self);

    return STEP_NULL;
}

static void
hev_socks5_session_task_entry (void *data)
{
    HevTask *task = hev_task_self ();
    HevSocks5Session *self = data;
    int step = STEP_DO_CONNECT;

    hev_task_add_fd (task, self->client_fd, POLLIN | POLLOUT);

    while (step) {
        switch (step) {
        case STEP_DO_CONNECT:
            step = socks5_do_connect (self);
            break;
        case STEP_WRITE_REQUEST:
            step = socks5_write_request (self);
            break;
        case STEP_READ_RESPONSE:
            step = socks5_read_response (self);
            break;
        case STEP_DO_SPLICE:
            step = socks5_do_splice (self);
            break;
        case STEP_DO_FWD_DNS:
            step = socks5_do_fwd_dns (self);
            break;
        case STEP_CLOSE_SESSION:
            step = socks5_close_session (self);
            break;
        default:
            step = STEP_NULL;
            break;
        }
    }
}
