/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "proxy_server.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#include <spawn.h>
#endif

#include "server/render_protocol.h"

#ifdef ENABLE_SAME_PROCESS_RENDER_SERVER
#include "server/render_context.h"
#include "server/render_server.h"
#endif

int
proxy_server_connect(struct proxy_server *srv)
{
   int client_fd = srv->client_fd;
   /* transfer ownership */
   srv->client_fd = -1;
   return client_fd;
}

void
proxy_server_destroy(struct proxy_server *srv)
{
   if (srv->pid >= 0) {
      kill(srv->pid, SIGKILL);

      siginfo_t siginfo = { 0 };
      waitid(P_PID, srv->pid, &siginfo, WEXITED);
   }

   if (srv->client_fd >= 0)
      close(srv->client_fd);

#ifdef ENABLE_SAME_PROCESS_RENDER_SERVER
   thrd_join(srv->thread, NULL);
#endif

   free(srv);
}

#ifndef ENABLE_SAME_PROCESS_RENDER_SERVER

#ifdef __APPLE__

/* On macOS the render server is launched with posix_spawn rather than
 * fork()+execve().
 *
 * POSIX_SPAWN_CLOEXEC_DEFAULT closes every fd in the child; only the server
 * socket and the std streams are explicitly inherited (the socketpair fds
 * are not O_CLOEXEC, but the default flag would otherwise close them, and
 * this also avoids leaking any other fd the library holds).  Detaching the
 * process group (POSIX_SPAWN_SETPGROUP, pgroup 0) keeps the server from
 * receiving terminal signals, matching the fork() path's setpgid(0, 0).
 */
static pid_t
proxy_server_spawn(const char *exec_path, char *const argv[], int remote_fd)
{
   posix_spawnattr_t attr;
   if (posix_spawnattr_init(&attr) != 0)
      return -1;
   posix_spawnattr_setflags(&attr,
                            POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP);
   posix_spawnattr_setpgroup(&attr, 0);

   posix_spawn_file_actions_t file_actions;
   if (posix_spawn_file_actions_init(&file_actions) != 0) {
      posix_spawnattr_destroy(&attr);
      return -1;
   }
   posix_spawn_file_actions_addinherit_np(&file_actions, STDIN_FILENO);
   posix_spawn_file_actions_addinherit_np(&file_actions, STDOUT_FILENO);
   posix_spawn_file_actions_addinherit_np(&file_actions, STDERR_FILENO);
   posix_spawn_file_actions_addinherit_np(&file_actions, remote_fd);

   pid_t pid;
   int ret = posix_spawn(&pid, exec_path, &file_actions, &attr, argv,
                         *_NSGetEnviron());

   posix_spawn_file_actions_destroy(&file_actions);
   posix_spawnattr_destroy(&attr);

   if (ret != 0) {
      proxy_log("failed to posix_spawn %s: %s", exec_path, strerror(ret));
      return -1;
   }

   return pid;
}

#endif /* __APPLE__ */

static bool
proxy_server_fork(struct proxy_server *srv)
{
   int socket_fds[2];
   if (!proxy_socket_pair(socket_fds))
      return false;
   const int client_fd = socket_fds[0];
   const int remote_fd = socket_fds[1];

   char fd_str[16];
   snprintf(fd_str, sizeof(fd_str), "%d", remote_fd);

   /* for devenv without installing server */
   char *const server_path = getenv("RENDER_SERVER_EXEC_PATH");
   char *const argv[] = {
      server_path ? server_path : RENDER_SERVER_EXEC_PATH,
      "--socket-fd",
      fd_str,
      NULL,
   };

#ifdef __APPLE__
   const pid_t pid = proxy_server_spawn(argv[0], argv, remote_fd);
   if (pid < 0) {
      close(client_fd);
      close(remote_fd);
      return false;
   }

   srv->pid = pid;
   srv->client_fd = client_fd;
   close(remote_fd);
#else
   const pid_t pid = fork();
   if (pid < 0) {
      proxy_log("failed to fork proxy server");
      close(client_fd);
      close(remote_fd);
      return false;
   }

   if (pid > 0) {
      srv->pid = pid;
      srv->client_fd = client_fd;
      close(remote_fd);
   } else {
      close(client_fd);

      /* do not receive signals from terminal */
      setpgid(0, 0);

      execv(argv[0], argv);

      proxy_log("failed to exec %s: %s", argv[0], strerror(errno));
      close(remote_fd);
      exit(-1);
   }
#endif

   return true;
}

static bool
proxy_server_init_fd(struct proxy_server *srv)
{
   /* the fd represents a connection to the server */
   srv->client_fd = proxy_renderer.cbs->get_server_fd(RENDER_SERVER_VERSION);
   if (srv->client_fd < 0)
      return false;

   return true;
}

#else  /* ENABLE_SAME_PROCESS_RENDER_SERVER */

static int
proxy_server_start_thread(void *args)
{
   int remote_fd = (int)(uintptr_t)args;
   char fd_str[16];
   snprintf(fd_str, sizeof(fd_str), "%d", remote_fd);
   char *argv[] = {
      RENDER_SERVER_EXEC_PATH,
      "--socket-fd",
      fd_str,
      NULL,
   };
   struct render_context_args ctx_args;

   /* must reset the global getopt index */
   optind = 1;

   bool ok = render_server_main(ARRAY_SIZE(argv) - 1, argv, &ctx_args);

   return ok ? 0 : -1;
}

static bool
proxy_server_init_thread(struct proxy_server *srv)
{
   int socket_fds[2];

   if (!proxy_socket_pair(socket_fds))
      return false;

   const int client_fd = socket_fds[0];
   const int remote_fd = socket_fds[1];

   bool ok = thrd_create(&srv->thread, proxy_server_start_thread,
                         (void *)(uintptr_t)remote_fd) == thrd_success;

   if (ok) {
      srv->client_fd = client_fd;
   } else {
      close(client_fd);
      close(remote_fd);
   }

   return ok;
}
#endif /* ENABLE_SAME_PROCESS_RENDER_SERVER */

struct proxy_server *
proxy_server_create(void)
{
   struct proxy_server *srv = calloc(1, sizeof(*srv));
   if (!srv)
      return NULL;

   srv->pid = -1;

#ifdef ENABLE_SAME_PROCESS_RENDER_SERVER
   if (!proxy_server_init_thread(srv)) {
      free(srv);
      return NULL;
   }
#else
   if (!proxy_server_init_fd(srv)) {
      /* start the render server on demand when the client does not provide a
       * server fd
       */
      if (!proxy_server_fork(srv)) {
         free(srv);
         return NULL;
      }
   }
#endif

   if (!proxy_socket_is_valid(srv->client_fd)) {
      proxy_log("invalid client fd type");
      close(srv->client_fd);
      free(srv);
      return NULL;
   }

   proxy_log("proxy server with pid %d", srv->pid);

   return srv;
}
