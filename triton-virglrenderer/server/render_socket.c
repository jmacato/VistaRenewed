/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "render_socket.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif /* SOCK_CLOEXEC */
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif /* MSG_CMSG_CLOEXEC */
#endif /* __APPLE__ */

#define RENDER_SOCKET_MAX_FD_COUNT 8

/* The socket pair between the server process and the client process is set up
 * by the client process (or yet another process).  Because render_server_run
 * does not poll yet, the fd is expected to be blocking.
 *
 * We also expect the fd to be always valid.  If the client process dies, the
 * fd becomes invalid and is considered a fatal error.
 *
 * There is also a socket pair between each context worker and the client
 * process.  The pair is set up by render_socket_pair here.
 *
 * The fd is also expected to be blocking.  When the client process closes its
 * end of the socket pair, the context worker terminates.
 */
bool
render_socket_pair(int out_fds[static 2])
{
   int type = SOCK_SEQPACKET;
#ifdef __APPLE__
   type = SOCK_STREAM;
#endif
   int ret = socketpair(AF_UNIX, type | SOCK_CLOEXEC, 0, out_fds);
   if (ret) {
      render_log("failed to create socket pair");
      return false;
   }

   return true;
}

bool
render_socket_is_valid(int fd)
{
   int type;
   socklen_t len = sizeof(type);
   if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len))
      return false;
#ifdef __APPLE__
   return type == SOCK_STREAM;
#else
   return type == SOCK_SEQPACKET;
#endif
}

void
render_socket_init(struct render_socket *socket, int fd)
{
   assert(fd >= 0);
   *socket = (struct render_socket){
      .fd = fd,
   };
}

void
render_socket_fini(struct render_socket *socket)
{
   close(socket->fd);
}

static const int *
get_received_fds(const struct msghdr *msg, int *out_count)
{
   const struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
   if (unlikely(!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
                cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len < CMSG_LEN(0))) {
      *out_count = 0;
      return NULL;
   }

   *out_count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
   return (const int *)CMSG_DATA(cmsg);
}

#ifdef __APPLE__
enum socket_state {
   SOCKET_STATE_FIRST_MSG,
   SOCKET_STATE_HEADER,
   SOCKET_STATE_DATA,
};
#endif

static bool
render_socket_recvmsg(struct render_socket *socket, struct msghdr *msg, size_t *out_size)
{
#ifdef __APPLE__
   enum socket_state state = SOCKET_STATE_FIRST_MSG;
   struct render_context_socket_header hdr = {0};
   ssize_t want = sizeof(hdr);
   struct msghdr _msg = {
      .msg_iov =
         &(struct iovec){
            .iov_base = &hdr,
            .iov_len = want,
         },
      .msg_iovlen = 1,
      .msg_control = msg->msg_control,
      .msg_controllen = msg->msg_controllen,
   };
   socklen_t _msg_controllen;

   assert(msg->msg_iovlen == 1);

   *out_size = 0;
   do {
      const ssize_t s = recvmsg(socket->fd, &_msg, MSG_CMSG_CLOEXEC);
      if (unlikely(s < 0)) {
         if (errno == EAGAIN || errno == EINTR)
            continue;

         render_log("failed to receive message: %s", strerror(errno));
         return false;
      }
      if (unlikely(s == 0)) {
         /* Peer closed the socket. */
         render_log("socket disconnected");
         return false;
      }

      if (state == SOCKET_STATE_FIRST_MSG) {
         _msg_controllen = _msg.msg_controllen;
         state = SOCKET_STATE_HEADER;
      } else {
         /* retain the cmsg from first message */
         assert(_msg.msg_controllen == 0);
      }

      if (unlikely(_msg.msg_flags & MSG_CTRUNC)) {
         render_log("failed to receive message: truncated");

         int fd_count;
         const int *fds = get_received_fds(&_msg, &fd_count);
         for (int i = 0; i < fd_count; i++)
            close(fds[i]);

         return false;
      }

      if (s <= want) {
         _msg.msg_iov[0].iov_base = (char *)_msg.msg_iov[0].iov_base + s;
         _msg.msg_iov[0].iov_len -= s;
         want -= s;
      }

      if (state == SOCKET_STATE_DATA) {
         *out_size += s;
      }

      if (!want && state == SOCKET_STATE_HEADER) {
         want = ntohl(hdr.length);
         _msg.msg_iov[0].iov_base = msg->msg_iov[0].iov_base;
         _msg.msg_iov[0].iov_len = want;
         state = SOCKET_STATE_DATA;
      } else if (!want && state == SOCKET_STATE_DATA) {
         msg->msg_controllen = _msg_controllen;
         break;
      }
   } while (true);
#else
   do {
      const ssize_t s = recvmsg(socket->fd, msg, MSG_CMSG_CLOEXEC);
      if (unlikely(s <= 0)) {
         if (!s)
            return false;

         if (errno == EAGAIN || errno == EINTR)
            continue;

         render_log("failed to receive message: %s", strerror(errno));
         return false;
      }

      if (unlikely(msg->msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
         render_log("failed to receive message: truncated");

         int fd_count;
         const int *fds = get_received_fds(msg, &fd_count);
         for (int i = 0; i < fd_count; i++)
            close(fds[i]);

         return false;
      }

      *out_size = s;
      return true;
   } while (true);
#endif

   return true;
}

static bool
render_socket_receive_request_internal(struct render_socket *socket,
                                       void *data,
                                       size_t max_size,
                                       size_t *out_size,
                                       int *fds,
                                       int max_fd_count,
                                       int *out_fd_count)
{
   assert(data && max_size);
   struct msghdr msg = {
      .msg_iov =
         &(struct iovec){
            .iov_base = data,
            .iov_len = max_size,
         },
      .msg_iovlen = 1,
   };

   char cmsg_buf[CMSG_SPACE(sizeof(*fds) * RENDER_SOCKET_MAX_FD_COUNT)];
   if (max_fd_count) {
      assert(fds && max_fd_count <= RENDER_SOCKET_MAX_FD_COUNT);
      msg.msg_control = cmsg_buf;
      msg.msg_controllen = CMSG_SPACE(sizeof(*fds) * max_fd_count);

      struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
      memset(cmsg, 0, sizeof(*cmsg));
   }

   if (!render_socket_recvmsg(socket, &msg, out_size))
      return false;

   if (max_fd_count) {
      int received_fd_count;
      const int *received_fds = get_received_fds(&msg, &received_fd_count);
      assert(received_fd_count <= max_fd_count);

      memcpy(fds, received_fds, sizeof(*fds) * received_fd_count);
      *out_fd_count = received_fd_count;
   } else if (out_fd_count) {
      *out_fd_count = 0;
   }

   return true;
}

bool
render_socket_receive_request(struct render_socket *socket,
                              void *data,
                              size_t max_size,
                              size_t *out_size)
{
   return render_socket_receive_request_internal(socket, data, max_size, out_size, NULL,
                                                 0, NULL);
}

bool
render_socket_receive_request_with_fds(struct render_socket *socket,
                                       void *data,
                                       size_t max_size,
                                       size_t *out_size,
                                       int *fds,
                                       int max_fd_count,
                                       int *out_fd_count)
{
   return render_socket_receive_request_internal(socket, data, max_size, out_size, fds,
                                                 max_fd_count, out_fd_count);
}

bool
render_socket_receive_data(struct render_socket *socket, void *data, size_t size)
{
   size_t received_size;
   if (!render_socket_receive_request(socket, data, size, &received_size))
      return false;

   if (size != received_size) {
      render_log("failed to receive data: expected %zu but received %zu", size,
                 received_size);
      return false;
   }

   return true;
}

static bool
render_socket_sendmsg(struct render_socket *socket, const struct msghdr *msg)
{
#ifdef __APPLE__
   enum socket_state state = SOCKET_STATE_FIRST_MSG;
   struct render_context_socket_header hdr = {
      .length = htonl(msg->msg_iov[0].iov_len),
   };
   ssize_t want = sizeof(hdr);
   struct msghdr _msg = {
      .msg_iov =
         &(struct iovec){
            .iov_base = &hdr,
            .iov_len = want,
         },
      .msg_iovlen = 1,
      .msg_control = msg->msg_control,
      .msg_controllen = msg->msg_controllen,
   };

   assert(msg->msg_iovlen == 1);

   do {
      const ssize_t s = sendmsg(socket->fd, &_msg, MSG_NOSIGNAL);
      if (unlikely(s < 0)) {
         if (errno == EAGAIN || errno == EINTR)
            continue;

         render_log("failed to send message: %s", strerror(errno));
         return false;
      }
      if (unlikely(s == 0)) {
         render_log("failed to send message: socket disconnected");
         return false;
      }

      if (state == SOCKET_STATE_FIRST_MSG) {
         _msg.msg_controllen = 0;
         _msg.msg_control = NULL;
         state = SOCKET_STATE_HEADER;
      }

      if (s <= want) {
         _msg.msg_iov[0].iov_base = (char *)_msg.msg_iov[0].iov_base + s;
         _msg.msg_iov[0].iov_len -= s;
         want -= s;
      }

      if (!want && state == SOCKET_STATE_HEADER) {
         want = ntohl(hdr.length);
         _msg.msg_iov[0].iov_base = msg->msg_iov[0].iov_base;
         _msg.msg_iov[0].iov_len = want;
         state = SOCKET_STATE_DATA;
      } else if (!want && state == SOCKET_STATE_DATA) {
         return true;
      }
   } while (true);
#else
   do {
      const ssize_t s = sendmsg(socket->fd, msg, MSG_NOSIGNAL);
      if (unlikely(s < 0)) {
         if (errno == EAGAIN || errno == EINTR)
            continue;

         render_log("failed to send message: %s", strerror(errno));
         return false;
      }

      /* no partial send since the socket type is SOCK_SEQPACKET */
      assert(msg->msg_iovlen == 1 && msg->msg_iov[0].iov_len == (size_t)s);
      return true;
   } while (true);
#endif
}

static inline bool
render_socket_send_reply_internal(struct render_socket *socket,
                                  const void *data,
                                  size_t size,
                                  const int *fds,
                                  int fd_count)
{
   assert(data && size);
   struct msghdr msg = {
      .msg_iov =
         &(struct iovec){
            .iov_base = (void *)data,
            .iov_len = size,
         },
      .msg_iovlen = 1,
   };

   char cmsg_buf[CMSG_SPACE(sizeof(*fds) * RENDER_SOCKET_MAX_FD_COUNT)];
   if (fd_count) {
      assert(fds && fd_count <= RENDER_SOCKET_MAX_FD_COUNT);
      msg.msg_control = cmsg_buf;
      msg.msg_controllen = CMSG_SPACE(sizeof(*fds) * fd_count);

      struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN(sizeof(*fds) * fd_count);
      memcpy(CMSG_DATA(cmsg), fds, sizeof(*fds) * fd_count);
   }

   return render_socket_sendmsg(socket, &msg);
}

bool
render_socket_send_reply(struct render_socket *socket, const void *data, size_t size)
{
   return render_socket_send_reply_internal(socket, data, size, NULL, 0);
}

bool
render_socket_send_reply_with_fds(struct render_socket *socket,
                                  const void *data,
                                  size_t size,
                                  const int *fds,
                                  int fd_count)
{
   return render_socket_send_reply_internal(socket, data, size, fds, fd_count);
}
