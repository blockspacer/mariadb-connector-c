/************************************************************************************
   Copyright (C) 2015 MariaDB Corporation AB,
   
   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.
   
   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.
   
   You should have received a copy of the GNU Library General Public
   License along with this library; if not see <http://www.gnu.org/licenses>
   or write to the Free Software Foundation, Inc., 
   51 Franklin St., Fifth Floor, Boston, MA 02110, USA
*************************************************************************************/

/* 
   MariaDB virtual IO plugin for socket communication:

   The plugin handles connections via unix and network sockets. it is enabled by
   default and compiled into Connector/C.
*/

#include <my_global.h>
#include <my_sys.h>
#include <errmsg.h>
#include <mysql.h>
#include <mysql/client_plugin.h>
#include <my_context.h>
#include <mysql_async.h>
#include <ma_common.h>
#include <string.h>
#include <m_string.h>
#ifndef _WIN32
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_POLL
#include <sys/poll.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netdb.h>
#include <netinet/tcp.h>
#else
#define O_NONBLOCK 1
#endif

#ifndef SOCKET_ERROR
#define SOCKET_ERROR -1
#endif


/* Function prototypes */
my_bool pvio_socket_set_timeout(MARIADB_PVIO *pvio, enum enum_pvio_timeout type, int timeout);
int pvio_socket_get_timeout(MARIADB_PVIO *pvio, enum enum_pvio_timeout type);
size_t pvio_socket_read(MARIADB_PVIO *pvio, uchar *buffer, size_t length);
size_t pvio_socket_async_read(MARIADB_PVIO *pvio, uchar *buffer, size_t length);
size_t pvio_socket_write(MARIADB_PVIO *pvio, const uchar *buffer, size_t length);
size_t pvio_socket_async_write(MARIADB_PVIO *pvio, const uchar *buffer, size_t length);
int pvio_socket_wait_io_or_timeout(MARIADB_PVIO *pvio, my_bool is_read, int timeout);
my_bool pvio_socket_blocking(MARIADB_PVIO *pvio, my_bool value, my_bool *old_value);
my_bool pvio_socket_connect(MARIADB_PVIO *pvio, MA_PVIO_CINFO *cinfo);
my_bool pvio_socket_close(MARIADB_PVIO *pvio);
int pvio_socket_fast_send(MARIADB_PVIO *pvio);
int pvio_socket_keepalive(MARIADB_PVIO *pvio);
my_bool pvio_socket_get_handle(MARIADB_PVIO *pvio, void *handle);
my_bool pvio_socket_is_blocking(MARIADB_PVIO *pvio);
my_bool pvio_socket_is_alive(MARIADB_PVIO *pvio);

static int pvio_socket_init(char *unused1, 
                           size_t unused2, 
                           int unused3, 
                           va_list);
static int pvio_socket_end(void);

struct st_ma_pvio_methods pvio_socket_methods= {
  pvio_socket_set_timeout,
  pvio_socket_get_timeout,
  pvio_socket_read,
  pvio_socket_async_read,
  pvio_socket_write,
  pvio_socket_async_write,
  pvio_socket_wait_io_or_timeout,
  pvio_socket_blocking,
  pvio_socket_connect,
  pvio_socket_close,
  pvio_socket_fast_send,
  pvio_socket_keepalive,
  pvio_socket_get_handle,
  pvio_socket_is_blocking,
  pvio_socket_is_alive
};

#ifndef HAVE_SOCKET_DYNAMIC
MARIADB_PVIO_PLUGIN pvio_socket_plugin=
#else
MARIADB_PVIO_PLUGIN _mysql_client_plugin_declare_
#endif
{
  MARIADB_CLIENT_PVIO_PLUGIN,
  MARIADB_CLIENT_PVIO_PLUGIN_INTERFACE_VERSION,
  "pvio_socket",
  "Georg Richter",
  "MariaDB virtual IO plugin for socket communication",
  {1, 0, 0},
  "LGPL",
  &pvio_socket_init,
  &pvio_socket_end,
  &pvio_socket_methods
};

struct st_pvio_socket {
  my_socket socket;
  int fcntl_mode;
  MYSQL *mysql;
};

static my_bool pvio_socket_initialized= FALSE;

static int pvio_socket_init(char *errmsg,
                           size_t errmsg_length, 
                           int unused, 
                           va_list va)
{
  pvio_socket_initialized= TRUE;
  return 0;
}

static int pvio_socket_end(void)
{
  if (!pvio_socket_initialized)
    return 1;
  return 0;
}

/* {{{ pvio_socket_set_timeout */
/*
   set timeout value

   SYNOPSIS
     pvio_socket_set_timeout
     pvio            PVIO
     type           timeout type (connect, read, write)
     timeout        timeout in seconds

   DESCRIPTION
     Sets timeout values for connection-, read or write time out.
     PVIO internally stores all timeout values in milliseconds, but 
     accepts and returns all time values in seconds (like api does).

   RETURNS
     0              Success
     1              Error
*/
my_bool pvio_socket_set_timeout(MARIADB_PVIO *pvio, enum enum_pvio_timeout type, int timeout)
{
  if (!pvio)
    return 1;
  pvio->timeout[type]= (timeout > 0) ? timeout * 1000 : -1;
  return 0;
}
/* }}} */

/* {{{ pvio_socket_get_timeout */
/*
   get timeout value

   SYNOPSIS
     pvio_socket_get_timeout
     pvio            PVIO
     type           timeout type (connect, read, write)

   DESCRIPTION
     Returns timeout values for connection-, read or write time out.
     PVIO internally stores all timeout values in milliseconds, but 
     accepts and returns all time values in seconds (like api does).

   RETURNS
      0...n         time out value
     -1             error
*/
int pvio_socket_get_timeout(MARIADB_PVIO *pvio, enum enum_pvio_timeout type)
{
  if (!pvio)
    return -1;
  return pvio->timeout[type] / 1000;
}
/* }}} */

/* {{{ pvio_socket_read */
/*
   read from socket

   SYNOPSIS
   pvio_socket_read()
     pvio             PVIO
     buffer          read buffer
     length          buffer length

   DESCRIPTION
     reads up to length bytes into specified buffer. In the event of an
     error erno is set to indicate it.

   RETURNS
      1..n           number of bytes read
      0              peer has performed shutdown
     -1              on error
                     
*/   
size_t pvio_socket_read(MARIADB_PVIO *pvio, uchar *buffer, size_t length)
{
  ssize_t r= -1;
#ifndef _WIN32
#ifdef __APPLE__
  int read_flags= 0;
#else
  /* don't ignore SIGPIPE globally like in libmysql!! */
  int read_flags= MSG_NOSIGNAL;
#endif
#endif
  struct st_pvio_socket *csock= NULL;

  if (!pvio || !pvio->data)
    return -1;

  csock= (struct st_pvio_socket *)pvio->data;

#ifndef _WIN32
  do {
    r= recv(csock->socket, (void *)buffer, length, read_flags);
  } while (r == -1 && errno == EINTR);

  while (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)
                      && pvio->timeout[PVIO_READ_TIMEOUT] > 0)
  {
    if (pvio_socket_wait_io_or_timeout(pvio, TRUE, pvio->timeout[PVIO_READ_TIMEOUT]) < 1)
      return -1;
    do {
      r= recv(csock->socket, (void *)buffer, length, read_flags);
    } while (r == -1 && errno == EINTR);
  }
#else
  {
    WSABUF wsaData;
    DWORD flags= 0,
          dwBytes= 0;

    /* clear error */
    errno= 0;
    wsaData.len = (u_long)length;
    wsaData.buf = (char*) buffer; 

    r = WSARecv(csock->socket, &wsaData, 1, &dwBytes, &flags, NULL, NULL);
    if (r == SOCKET_ERROR)
    {
      errno= WSAGetLastError();
      return -1;
    }
    r= dwBytes;
  }
#endif
  return r;
}
/* }}} */

/* {{{ pvio_socket_async_read */
/*
   read from socket

   SYNOPSIS
   pvio_socket_async_read()
     pvio             PVIO
     buffer          read buffer
     length          buffer length

   DESCRIPTION
     reads up to length bytes into specified buffer. In the event of an
     error erno is set to indicate it.

   RETURNS
      1..n           number of bytes read
      0              peer has performed shutdown
     -1              on error
                     
*/   
size_t pvio_socket_async_read(MARIADB_PVIO *pvio, uchar *buffer, size_t length)
{
  ssize_t r= -1;
#ifndef _WIN32
#ifdef __APPLE__
  int read_flags= MSG_DONTWAIT;
#else
  int read_flags= MSG_NOSIGNAL | MSG_DONTWAIT;
#endif
#endif
  struct st_pvio_socket *csock= NULL;

  if (!pvio || !pvio->data)
    return -1;

  csock= (struct st_pvio_socket *)pvio->data;

#ifndef _WIN32
  r= recv(csock->socket,(void *)buffer, length, read_flags);
#else
  /* Windows doesn't support MSG_DONTWAIT, so we need to set
     socket to non blocking */
  pvio_socket_blocking(pvio, 0, 0);
  r= recv(csock->socket, (char *)buffer, (int)length, 0);
#endif
  return r;
}
/* }}} */

/* {{{ pvio_socket_async_write */
/*
   write to socket

   SYNOPSIS
   pvio_socket_async_write()
     pvio             PVIO
     buffer          read buffer
     length          buffer length

   DESCRIPTION
     writes up to length bytes to socket. In the event of an
     error erno is set to indicate it.

   RETURNS
      1..n           number of bytes read
      0              peer has performed shutdown
     -1              on error
                     
*/   
size_t pvio_socket_async_write(MARIADB_PVIO *pvio, const uchar *buffer, size_t length)
{
  ssize_t r= -1;
#ifndef _WIN32
#ifdef __APPLE__
  int write_flags= MSG_DONTWAIT;
#else
  int write_flags= MSG_NOSIGNAL | MSG_DONTWAIT;
#endif
#endif
  struct st_pvio_socket *csock= NULL;

  if (!pvio || !pvio->data)
    return -1;

  csock= (struct st_pvio_socket *)pvio->data;

#ifndef WIN32
  r= send(csock->socket, buffer, length, write_flags);
#else
  /* Windows doesn't support MSG_DONTWAIT, so we need to set
     socket to non blocking */
  pvio_socket_blocking(pvio, 0, 0);
  r= send(csock->socket, buffer, (int)length, 0);
#endif
  return r;
}
/* }}} */


/* {{{ pvio_socket_write */
/*
   write to socket

   SYNOPSIS
   pvio_socket_write()
     pvio             PVIO
     buffer          read buffer
     length          buffer length

   DESCRIPTION
     writes up to length bytes to socket. In the event of an
     error erno is set to indicate it.

   RETURNS
      1..n           number of bytes read
      0              peer has performed shutdown
     -1              on error
                     
*/
size_t pvio_socket_write(MARIADB_PVIO *pvio, const uchar *buffer, size_t length)
{
  ssize_t r= -1;
#ifndef _WIN32
#ifdef __APPLE__
  int send_flags= MSG_DONTWAIT;
#else
  int send_flags= MSG_NOSIGNAL | MSG_DONTWAIT;
#endif
#endif
  struct st_pvio_socket *csock= NULL;
  if (!pvio || !pvio->data)
    return -1;

  csock= (struct st_pvio_socket *)pvio->data;

#ifndef _WIN32
  do {
    r= send(csock->socket, buffer, length, send_flags);
  } while (r == -1 && errno == EINTR);

  while (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
         pvio->timeout[PVIO_WRITE_TIMEOUT] != 0)
  {
    if (pvio_socket_wait_io_or_timeout(pvio, FALSE, pvio->timeout[PVIO_WRITE_TIMEOUT]) < 1)
      return -1;
    do {
      r= send(csock->socket, buffer, length, send_flags);
    } while (r == -1 && errno == EINTR);
  }
#else
  {
    WSABUF wsaData;
    DWORD dwBytes= 0;

    wsaData.len = (u_long)length;
    wsaData.buf = (char*) buffer;

    r = WSASend(csock->socket, &wsaData, 1, &dwBytes, 0, NULL, NULL);
    if (r == SOCKET_ERROR) {
      errno= WSAGetLastError();
      return -1;
    }
    r= dwBytes;
  }
#endif
  return r;
}
/* }}} */

int pvio_socket_wait_io_or_timeout(MARIADB_PVIO *pvio, my_bool is_read, int timeout)
{
  int rc;
  struct st_pvio_socket *csock= NULL;

#ifndef _WIN32
  struct pollfd p_fd;
#else
  struct timeval tv= {0,0};
  fd_set fds, exc_fds;
#endif

  if (!pvio || !pvio->data)
    return 0;

  csock= (struct st_pvio_socket *)pvio->data;
  {
#ifndef _WIN32
    memset(&p_fd, 0, sizeof(p_fd));
    p_fd.fd= csock->socket;
    p_fd.events= (is_read) ? POLLIN : POLLOUT;

    do {
      rc= poll(&p_fd, 1, timeout);
    } while (rc == -1 || errno == EINTR);

    if (rc == 0)
      errno= ETIMEDOUT;
#else
    FD_ZERO(&fds);
    FD_ZERO(&exc_fds);

    FD_SET(csock->socket, &fds);
    FD_SET(csock->socket, &exc_fds);

    if (timeout >= 0)
    {
      tv.tv_sec= timeout / 1000;
      tv.tv_usec= (timeout % 1000) * 1000;
    }

    rc= select(0, (is_read) ? &fds : NULL,
                  (is_read) ? NULL : &fds,
                  &exc_fds, 
                  (timeout >= 0) ? &tv : NULL);
    if (rc == SOCKET_ERROR)
      errno= WSAGetLastError();
    if (rc == 0)
      errno= ETIMEDOUT;
#endif
  }
  return rc;
}

my_bool pvio_socket_blocking(MARIADB_PVIO *pvio, my_bool block, my_bool *previous_mode)
{
  int *sd_flags, save_flags;
  my_bool tmp;
  struct st_pvio_socket *csock= NULL;

  if (!pvio || !pvio->data)
    return 1;

  csock= (struct st_pvio_socket *)pvio->data;
  sd_flags= &csock->fcntl_mode;
  save_flags= csock->fcntl_mode;

  if (!previous_mode)
    previous_mode= &tmp;

#ifdef _WIN32
  *previous_mode= (*sd_flags & O_NONBLOCK) != 0;
  *sd_flags = (block) ? *sd_flags & ~O_NONBLOCK : *sd_flags | O_NONBLOCK;
  {
    ulong arg= 1 - block;
    if (ioctlsocket(csock->socket, FIONBIO, (void *)&arg))
    {
      csock->fcntl_mode= save_flags;
      return(WSAGetLastError());
    }
  }
#else
#if defined(O_NONBLOCK)
  *previous_mode= (*sd_flags & O_NONBLOCK) != 0;
  *sd_flags = (block) ? *sd_flags & ~O_NONBLOCK : *sd_flags | O_NONBLOCK;
#elif defined(O_NDELAY)
  *previous_mode= (*sd_flags & O_NODELAY) != 0;
  *sd_flags = (block) ? *sd_flags & ~O_NODELAY : *sd_flags | O_NODELAY;
#elif defined(FNDELAY)
  *previous_mode= (*sd_flags & O_FNDELAY) != 0;
  *sd_flags = (block) ? *sd_flags & ~O_FNDELAY : *sd_flags | O_FNDELAY;
#else
#error socket blocking is not supported on this platform
#endif
  if (fcntl(csock->socket, F_SETFL, *sd_flags) == -1)
  {
    csock->fcntl_mode= save_flags;
    return errno;
  }
#endif
  return 0;
}

static int pvio_socket_internal_connect(MARIADB_PVIO *pvio,
                                       const struct sockaddr *name, 
                                       size_t namelen)
{
  int rc= 0;
  struct st_pvio_socket *csock= NULL;
  int timeout;

  if (!pvio || !pvio->data)
    return 1;

  csock= (struct st_pvio_socket *)pvio->data;
  timeout= pvio->timeout[PVIO_CONNECT_TIMEOUT];

  /* set non blocking */
  pvio_socket_blocking(pvio, 0, 0);

#ifndef _WIN32
  do {
    rc= connect(csock->socket, (struct sockaddr*) name, (int)namelen);
  } while (rc == -1 && errno == EINTR);
  /* in case a timeout values was set we need to check error values
     EINPROGRESS and EAGAIN */
  if (timeout != 0 && rc == -1 && 
     (errno == EINPROGRESS || errno == EAGAIN))
  {
    rc= pvio_socket_wait_io_or_timeout(pvio, FALSE, timeout);
    if (rc < 1)
      return -1;
    {
      int error;
      socklen_t error_len= sizeof(error);
      if ((rc = getsockopt(csock->socket, SOL_SOCKET, SO_ERROR, 
                           (char *)&error, &error_len)) < 0)
        return errno;
      else if (error) 
        return error;
    }
  }
#ifdef __APPLE__
  if (csock->socket)
  {
    int val= 1;
    setsockopt(csock->socket, SOL_SOCKET, SO_NOSIGPIPE, (void *)&val, sizeof(int));
  }
#endif
#else
  rc= connect(csock->socket, (struct sockaddr*) name, (int)namelen);
  if (rc == SOCKET_ERROR)
  {
    if (WSAGetLastError() == WSAEWOULDBLOCK)
    {
      if (pvio_socket_wait_io_or_timeout(pvio, FALSE, timeout) < 0)
        return -1;
      rc= 0;
    }
  }
#endif
  return rc;
}

int pvio_socket_keepalive(MARIADB_PVIO *pvio)
{
  int opt= 1;
  struct st_pvio_socket *csock= NULL;

  if (!pvio || !pvio->data)
    return 1;

  csock= (struct st_pvio_socket *)pvio->data;

  return setsockopt(csock->socket, SOL_SOCKET, SO_KEEPALIVE,
#ifndef _WIN32
               (const void *)&opt, sizeof(opt));
#else
               (char *)&opt, (int)sizeof(opt));
#endif
}

int pvio_socket_fast_send(MARIADB_PVIO *pvio)
{
  int r= 0;
  struct st_pvio_socket *csock= NULL;

  if (!pvio || !pvio->data)
    return 1;

  csock= (struct st_pvio_socket *)pvio->data;

/* Setting IP_TOS is not recommended on Windows. See 
   http://msdn.microsoft.com/en-us/library/windows/desktop/ms738586(v=vs.85).aspx
*/
#ifndef _WIN32
#ifdef IPTOS_THROUGHPUT
  {
    int tos = IPTOS_THROUGHPUT;
    r= setsockopt(csock->socket, IPPROTO_IP, IP_TOS,
	                         (const void *)&tos, sizeof(tos));
  }
#endif                                    /* IPTOS_THROUGHPUT */
#endif
  if (!r)
  {
    int opt = 1;
    /* turn off nagle algorithm */
    r= setsockopt(csock->socket, IPPROTO_TCP, TCP_NODELAY,
#ifdef _WIN32
                  (const char *)&opt, (int)sizeof(opt));
#else
                  (const void *)&opt, sizeof(opt));
#endif
  }
  return r;
}

static int
pvio_socket_connect_sync_or_async(MARIADB_PVIO *pvio,
                          const struct sockaddr *name, uint namelen)
{
  MYSQL *mysql= pvio->mysql;
  if (mysql->options.extension && mysql->options.extension->async_context &&
      mysql->options.extension->async_context->active)
  {
    /* even if we are not connected yet, application needs to check socket
     * via mysql_get_socket api call, so we need to assign pvio */
    mysql->options.extension->async_context->pvio= pvio;
    pvio_socket_blocking(pvio, 0, 0);
    return my_connect_async(pvio, name, namelen, pvio->timeout[PVIO_CONNECT_TIMEOUT]);
  }

  return pvio_socket_internal_connect(pvio, name, namelen);
}

my_bool pvio_socket_connect(MARIADB_PVIO *pvio, MA_PVIO_CINFO *cinfo)
{
  struct st_pvio_socket *csock= NULL;

  if (!pvio || !cinfo)
    return 1;

  if (!(csock= (struct st_pvio_socket *)my_malloc(sizeof(struct st_pvio_socket),
                                      MYF(MY_WME | MY_ZEROFILL))))
  {
    PVIO_SET_ERROR(cinfo->mysql, CR_OUT_OF_MEMORY, unknown_sqlstate, 0, "");
    return 1;
  }
  pvio->data= (void *)csock;
  csock->socket= -1;
  pvio->mysql= cinfo->mysql;
  pvio->type= cinfo->type;

  if (cinfo->type == PVIO_TYPE_UNIXSOCKET)
  {
#ifndef _WIN32
#ifdef HAVE_SYS_UN_H
    struct sockaddr_un UNIXaddr;
    if ((csock->socket = socket(AF_UNIX,SOCK_STREAM,0)) == SOCKET_ERROR)
    {
      PVIO_SET_ERROR(cinfo->mysql, CR_SOCKET_CREATE_ERROR, unknown_sqlstate, 0, errno);
      goto error;
    }
    bzero((char*) &UNIXaddr,sizeof(UNIXaddr));
    UNIXaddr.sun_family = AF_UNIX;
    strmov(UNIXaddr.sun_path, cinfo->unix_socket);
    if (pvio_socket_connect_sync_or_async(pvio, (struct sockaddr *) &UNIXaddr, 
                                    sizeof(UNIXaddr)))
    {
      PVIO_SET_ERROR(cinfo->mysql, CR_CONNECTION_ERROR, SQLSTATE_UNKNOWN, 
                    ER(CR_CONNECTION_ERROR), cinfo->unix_socket, socket_errno);
      goto error;
    }
    if (pvio_socket_blocking(pvio, 1, 0) == SOCKET_ERROR)
    {
      goto error;
    }
#else
/* todo: error, not supported */
#endif
#endif
  } else if (cinfo->type == PVIO_TYPE_SOCKET)
  {
    struct addrinfo hints, *save_res= 0, *bind_res= 0, *res= 0, *bres= 0;
    char server_port[NI_MAXSERV];
    int gai_rc;
    int rc= 0;

    bzero(&server_port, NI_MAXSERV);
    my_snprintf(server_port, NI_MAXSERV, "%d", cinfo->port);

    /* set hints for getaddrinfo */
    bzero(&hints, sizeof(hints));
    hints.ai_protocol= IPPROTO_TCP; /* TCP connections only */
    hints.ai_family= AF_UNSPEC;     /* includes: IPv4, IPv6 or hostname */
    hints.ai_socktype= SOCK_STREAM;

    /* if client has multiple interfaces, we will bind socket to given
     * bind_address */
    if (cinfo->mysql->options.bind_address)
    {
      gai_rc= getaddrinfo(cinfo->mysql->options.bind_address, 0,
                          &hints, &bind_res);
      if (gai_rc != 0)
      {
        PVIO_SET_ERROR(cinfo->mysql, CR_BIND_ADDR_FAILED, SQLSTATE_UNKNOWN, 
                     CER(CR_BIND_ADDR_FAILED), cinfo->mysql->options.bind_address, gai_rc);
        goto error;
      }
    }
    /* Get the address information for the server using getaddrinfo() */
    gai_rc= getaddrinfo(cinfo->host, server_port, &hints, &res);
    if (gai_rc != 0)
    {
      PVIO_SET_ERROR(cinfo->mysql, CR_UNKNOWN_HOST, SQLSTATE_UNKNOWN, 
                   ER(CR_UNKNOWN_HOST), cinfo->host, gai_rc);
      if (bind_res)
        freeaddrinfo(bind_res);
      goto error;
    }

    /* res is a linked list of addresses for the given hostname. We loop until
       we are able to connect to one address or all connect attempts failed */
    for (save_res= res; save_res; save_res= save_res->ai_next)
    {
      csock->socket= socket(save_res->ai_family, save_res->ai_socktype, 
                            save_res->ai_protocol);
      if (csock->socket == SOCKET_ERROR)
        /* Errors will be handled after loop finished */
        continue;

      if (bind_res)
      {
        for (bres= bind_res; bres; bres= bres->ai_next)
        {
          if (!(rc= bind(csock->socket, bres->ai_addr, (int)bres->ai_addrlen)))
            break;
        }
        if (rc)
        {
          closesocket(csock->socket);
          continue;
        }
      }

      rc= pvio_socket_connect_sync_or_async(pvio, save_res->ai_addr, (uint)save_res->ai_addrlen);
      if (!rc)
      {
        MYSQL *mysql= pvio->mysql;
        if (mysql->options.extension && mysql->options.extension->async_context &&
             mysql->options.extension->async_context->active)
          break;
        if (pvio_socket_blocking(pvio, 0, 0) == SOCKET_ERROR)
        {
          closesocket(csock->socket);
          continue;
        }
        break; /* success! */
      }
    }
 
    freeaddrinfo(res);
    if (bind_res)
      freeaddrinfo(bind_res);

    if (csock->socket == SOCKET_ERROR)
    {
      PVIO_SET_ERROR(cinfo->mysql, CR_IPSOCK_ERROR, SQLSTATE_UNKNOWN, ER(CR_IPSOCK_ERROR),
                         socket_errno);
      goto error;
    }

    /* last call to connect 2 failed */
    if (rc)
    {
      PVIO_SET_ERROR(cinfo->mysql, CR_CONN_HOST_ERROR, SQLSTATE_UNKNOWN, ER(CR_CONN_HOST_ERROR),
                           cinfo->host, socket_errno);
      goto error;
    }
    if (pvio_socket_blocking(pvio, 1, 0) == SOCKET_ERROR)
      goto error;
  }
#ifdef _WIN32
  /* apply timeouts */
  if (pvio->timeout[PVIO_WRITE_TIMEOUT] > 0)
    setsockopt(csock->socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&pvio->timeout[PVIO_WRITE_TIMEOUT], sizeof(int));
  if (pvio->timeout[PVIO_READ_TIMEOUT] > 0)
    setsockopt(csock->socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&pvio->timeout[PVIO_READ_TIMEOUT], sizeof(int));
#endif
  return 0;
error:
  if (pvio->data)
  {
    my_free((gptr)pvio->data);
    pvio->data= NULL;
  }
  return 1;
}

/* {{{ my_bool pvio_socket_close() */
my_bool pvio_socket_close(MARIADB_PVIO *pvio)
{
  struct st_pvio_socket *csock= NULL;
  int r= 0;

  if (!pvio)
    return 1;

  if (pvio->data)
  {
    csock= (struct st_pvio_socket *)pvio->data;
    if (csock && csock->socket != -1)
    {
      r= shutdown(csock->socket ,2);
      r= closesocket(csock->socket);
      csock->socket= -1;
    }
    my_free((gptr)pvio->data);
    pvio->data= NULL;
  }
  return r;
}
/* }}} */

/* {{{ my_socket pvio_socket_get_handle */
my_bool pvio_socket_get_handle(MARIADB_PVIO *pvio, void *handle)
{
  if (pvio && pvio->data && handle)
  {
    *(my_socket *)handle= ((struct st_pvio_socket *)pvio->data)->socket;
    return 0;
  }
  return 1;
}
/* }}} */

/* {{{ my_bool pvio_socket_is_blocking(MARIADB_PVIO *pvio) */
my_bool pvio_socket_is_blocking(MARIADB_PVIO *pvio)
{
  struct st_pvio_socket *csock= NULL;
  my_bool r;

  if (!pvio || !pvio->data)
    return 0;

  csock= (struct st_pvio_socket *)pvio->data;
  r = !(csock->fcntl_mode & O_NONBLOCK);
  return r;
}
/* }}} */

/* {{{ my_bool pvio_socket_is_alive(MARIADB_PVIO *pvio) */
my_bool pvio_socket_is_alive(MARIADB_PVIO *pvio)
{
  struct st_pvio_socket *csock= NULL;
 #ifndef _WIN32
  struct pollfd poll_fd;
#else
  FD_SET sfds;
  struct timeval tv= {0,0};
#endif
  int res;

  if (!pvio || !pvio->data)
    return 0;

  csock= (struct st_pvio_socket *)pvio->data;
#ifndef _WIN32
  memset(&poll_fd, 0, sizeof(struct pollfd));
  poll_fd.events= POLLPRI | POLLIN;
  poll_fd.fd= csock->socket;

  res= poll(&poll_fd, 1, 0);
  if (res <= 0) /* timeout or error */
    return FALSE;
  if (!(poll_fd.revents & (POLLIN | POLLPRI)))
    return FALSE;
  return TRUE;
#else
  /* We can't use the WSAPoll function, it's broken :-(
     (see Windows 8 Bugs 309411 - WSAPoll does not report failed connections)
     Instead we need to use select function:
     If TIMEVAL is initialized to {0, 0}, select will return immediately; 
     this is used to poll the state of the selected sockets.
  */
  FD_ZERO(&sfds);
  FD_SET(csock->socket, &sfds);

  res= select((int)+csock->socket + 1, &sfds, NULL, NULL, &tv);
  if (res > 0 && FD_ISSET(csock->socket, &sfds))
    return TRUE;
  return FALSE;
#endif
}
/* }}} */