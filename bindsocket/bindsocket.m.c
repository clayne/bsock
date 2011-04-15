/*
 * bindsocket - bind() sockets to restricted ports for lower-privilege daemons
 *
 * Copyright (c) 2011, Glue Logic LLC. All rights reserved. code()gluelogic.com
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Glue Logic LLC nor the names of its contributors
 *     may be used to endorse or promote products derived from this software
 *     without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <poll.h>
#include <pwd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

extern char **environ; /* avoid #define _GNU_SOURCE for visibility of environ */

#include <bindsocket_addrinfo.h>
#include <bindsocket_unixdomain.h>

#ifndef POLLRDHUP
#define POLLRDHUP 0
#endif

#ifndef BINDSOCKET_GROUP
#define BINDSOCKET_GROUP "daemon"
#endif

/* N.B. directory (and tree above it) must be writable only by root */
/* Unit test drivers not run as root should override this location at compile */
#ifndef BINDSOCKET_SOCKET_DIR
#define BINDSOCKET_SOCKET_DIR "/var/run/bindsocket"
#endif
#define BINDSOCKET_SOCKET BINDSOCKET_SOCKET_DIR "/socket"
/* <<<FUTURE: /etc/bindsocket for config file? */
#ifndef BINDSOCKET_CONFIG
#define BINDSOCKET_CONFIG BINDSOCKET_SOCKET_DIR "/config"
#endif

#ifndef BINDSOCKET_SYSLOG_IDENT
#define BINDSOCKET_SYSLOG_IDENT "bindsocket"
#endif
#ifndef BINDSOCKET_SYSLOG_FACILITY
#define BINDSOCKET_SYSLOG_FACILITY LOG_DAEMON
#endif

static int syslog_perror_level = 0;

static void
bindsocket_openlog (void)
{
    openlog(BINDSOCKET_SYSLOG_IDENT, LOG_NOWAIT, BINDSOCKET_SYSLOG_FACILITY);
}

/* GPS: remove 'static' for now */
void __attribute__((noinline)) __attribute__((cold))
syslog_perror (const char * const restrict str, const int errnum)
{
    /* (not written to use vsyslog(); lazy
     *  and not currently needed for error messages in this program) */

    /* syslog() always */
    if (0 != errno)
        syslog(LOG_ERR, "%s: %s", str, strerror(errnum));
    else
        syslog(LOG_ERR, "%s", str);

    if (0 == syslog_perror_level) { /*(stderr closed when daemon; skip perror)*/
        if (0 != errnum) {
            errno = errnum;
            perror(str);
        }
        else
            fprintf(stderr, "%s\n", str);
    }
}

/* retry_close() - make effort to avoid leaking open file descriptors
 *                 call perror() if error */
static int
retry_close (const int fd)
{
    int r;
    if (fd < 0) return 0;
    do {r = close(fd);} while (r != 0 && errno == EINTR);
    if (0 != r) syslog_perror("close", errno);
    return r;
}

static bool
bindsocket_is_authorized_addrinfo (const struct addrinfo * const restrict ai,
                                   const uid_t uid, const gid_t gid)
{
    /* Note: no process optimization implemented
     *       (numerous options for caching, improving performance if needed) */
    /* <<<FUTURE: better error messages for config file errors */

    char *username, *family, *socktype, *protocol, *service, *addr;
    FILE *cfg;
    struct passwd *pw;
    struct addrinfo *gai;
    struct addrinfo hints = {
      .ai_flags     = AI_V4MAPPED | AI_ADDRCONFIG,
      /* ai_family, ai_socktype, ai_protocol are filled in from config file */
      .ai_addrlen   = 0,
      .ai_addr      = NULL,
      .ai_canonname = NULL,
      .ai_next      = NULL
    };
    int r;
    const uid_t euid = geteuid();
    struct stat st;
    char line[256];
    bool rc = false;

    if (uid == 0 || gid == 0)  /* permit root or wheel */
        return true;

    if (NULL == (cfg = fopen(BINDSOCKET_CONFIG, "r"))) {
        syslog_perror(BINDSOCKET_CONFIG, errno);
        return false;
    }
    if (0 != fstat(fileno(cfg), &st)
        || st.st_uid != euid || (st.st_mode & (S_IWGRP|S_IWOTH))) {
        syslog_perror("ownership/permissions incorrect on "BINDSOCKET_CONFIG,0);
        return false;
    }

    while (!feof(cfg) && !ferror(cfg)) {
        if (NULL == fgets(line, sizeof(line), cfg))
            continue;  /* EOF or error reading file */
        if (*line == '#' || *line == '\n')
            continue;  /* comment or blank line */
        if (   NULL == (username = strtok(line, " "))
            || NULL == (family   = strtok(NULL, " "))
            || NULL == (socktype = strtok(NULL, " "))
            || NULL == (protocol = strtok(NULL, " "))
            || NULL == (service  = strtok(NULL, " "))
            || NULL == (addr     = strtok(NULL, " "))
            || NULL != (           strtok(NULL, " "))) {
            syslog_perror("bindsocket config file error", 0);
            continue;
        }
        hints.ai_family   = bindsocket_addrinfo_family_from_str(family);
        hints.ai_socktype = bindsocket_addrinfo_socktype_from_str(socktype);
        hints.ai_protocol = bindsocket_addrinfo_protocol_from_str(protocol);
        if ( NULL == (pw = getpwnam(username))
            || -1 == hints.ai_family
            || -1 == hints.ai_socktype
            || -1 == hints.ai_protocol) {
            syslog_perror("bindsocket config file error", 0);
            continue;
        }

        if (   pw->pw_uid != uid                 /* not unspecified by client */
            || (hints.ai_family  != ai->ai_family && AF_UNSPEC != ai->ai_family)
            || hints.ai_socktype != ai->ai_socktype
            || (hints.ai_protocol != ai->ai_protocol && 0 != ai->ai_protocol))
            continue;  /* not a match */

        if (hints.ai_family == AF_INET || hints.ai_family == AF_INET6) {
            if (0 == (r = getaddrinfo(addr, service, &hints, &gai))) {
                /* gai->ai_next *not* checked; not using hints.ai_flags = AI_ALL
                 * config file should have specific addrs that each match one */
                if (   gai->ai_addrlen == ai->ai_addrlen
                    || 0 == memcmp(gai->ai_addr, ai->ai_addr, ai->ai_addrlen)) {
                    freeaddrinfo(gai);
                    rc = true;
                    break;  /* match; success */
                } /* else not a match */
                freeaddrinfo(gai);
            }
            else
                syslog_perror("getaddrinfo", 0); /* gai_strerror(r) */
        }
        else if (hints.ai_family == AF_UNIX) {
            if (0 == strncmp(((struct sockaddr_un *)ai->ai_addr)->sun_path,
                             addr, ai->ai_addrlen)) {
                rc = true;
                break;  /* match; success */
            } /* else not a match */
        }
        /* (else not supported by bindsocket, or config file error) */
    }

    if (!rc) {
        syslog_perror("permission denied", 0);
    }
    fclose(cfg);  /* not required; bindsocket_client_session() exits soon */
    return rc;
}

static int
bindsocket_client_session (const int cfd,
                           const int argc, char * const * const restrict argv)
{
    /* <<<FUTURE: might add additional logging of request and success/failure */
    int fd;
    int rc = EXIT_FAILURE;
    uid_t euid;
    gid_t egid;
    int flag = 1;
    int addr[27];/* buffer for IPv4, IPv6, or AF_UNIX w/ up to 108 char path */
    struct addrinfo ai = {  /* init only fields used to pass buf and bufsize */
      .ai_addrlen = sizeof(addr),
      .ai_addr    = (struct sockaddr *)addr
    };

    if (0 != bindsocket_unixdomain_getpeereid(cfd, &euid, &egid)) {
        syslog_perror("getpeereid", errno);
        return EXIT_FAILURE;
    }

    /* syslog all connections to (or instantiations of) bindsocket daemon
     * <<<FUTURE: might write custom wrapper to platform-specific getpeereid()
     * and combine with syslog() call to log pid and other info, if available */
    syslog(LOG_INFO, "connect: uid:%d gid:%d", euid, egid);

    /* set alarm (uncaught here) to enforce time limit on blocking syscalls */
    alarm(2);

    /* receive addrinfo from client */
    if (!(5 != argc                               /*(-1 for infinite poll)*/
          ? bindsocket_unixdomain_recv_addrinfo(cfd, -1, &ai)
          : bindsocket_addrinfo_from_strings(&ai, argv[0], argv[1],
                                             argv[2], argv[3], argv[4]))) {
        alarm(0); /* not strictly needed since callers exit upon return */
        syslog_perror("recv addrinfo error or invalid addrinfo", errno);
        return EXIT_FAILURE;
    }

    /* check client credentials to authorize client request,
     * bind socket, send socket fd to client (no poll since send only one msg)*/
    if (bindsocket_is_authorized_addrinfo(&ai, euid, egid)) {
        if (   0 <= (fd = socket(ai.ai_family, ai.ai_socktype, ai.ai_protocol))
            && 0 == setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag))
            && 0 == bind(fd, (struct sockaddr *)ai.ai_addr, ai.ai_addrlen)) {
            rc = bindsocket_unixdomain_send_fd(cfd, fd)
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
            if (rc == EXIT_FAILURE && errno != EPIPE && errno != ECONNRESET)
                syslog_perror("sendmsg", errno);
        }
        else
            syslog_perror("socket,setsockopt,bind", errno);

        retry_close(fd);/* not strictly needed since callers exit upon return */
    }

    alarm(0); /* not strictly needed since callers exit upon return */
    return rc;
}

static bool
setuid_stdinit (void)
{
    /* Note: not retrying upon interruption; any fail to init means exit fail */

    /* Clear the environment */
    static char *empty_env[] = { NULL };
    environ = empty_env;

    /* Unblock all signals (regardless of what was inherited from parent) */
    sigset_t sigset_empty;
    if (0 != sigemptyset(&sigset_empty)
        || sigprocmask(0 != SIG_SETMASK, &sigset_empty, (sigset_t *) NULL)) {
        syslog_perror("sigprocmask", errno);
        return false;
    }

    return true;
}

static volatile int bindsocket_children = 0;

/* bindsocket high and low watermarks for in-flight forked children.
 * If hiwat is exceeded, then wait num children falls to lowat before continuing
 * to accept new connections.  bindsocket expects to be very fast and seldom
 * called, so detection of any outstanding behavior should be escalated. */
#define BINDSOCKET_CHILD_HIWAT 16
#define BINDSOCKET_CHILD_LOWAT  8

static void  __attribute__((noinline)) __attribute__((cold))
bindsocket_wait_children (void)
{
    /* syslog() once every 10 secs while excess pending children condition */
    static time_t prior = 0;
    const time_t t = time(NULL);
    if (prior+10 < t) {
        prior = t;
        syslog(LOG_CRIT, "pending children (%d) > hi watermark (%d)",
               bindsocket_children, BINDSOCKET_CHILD_HIWAT);
    }

    /* bindsocket_children is 'volatile' */
    while (bindsocket_children > BINDSOCKET_CHILD_LOWAT)
        poll(NULL, 0, 100);
}

static void
daemon_sa_chld (int signum)
{
    pid_t pid;
    int remaining = bindsocket_children; /* bindsocket_children is 'volatile' */
    do { pid = waitpid(-1, NULL, WNOHANG);
    } while (pid > 0 ? --remaining > 0 : (-1 == pid && errno == EINTR));
    bindsocket_children = (-1 == pid && errno == ECHILD) ? 0 : remaining;
}

static void
daemon_sa_handler (int signum)
{
    exit(EXIT_SUCCESS);  /* executes atexit() handlers */
}

static bool
daemon_signal_init (void)
{
    /* configure signal handlers for bindsocket desired behaviors
     *   SIGALRM: default handler
     *   SIGCLD:  ignore
     *   SIGHUP:  clean up and exit (for now)
     *   SIGINT:  clean up and exit
     *   SIGTERM: clean up and exit
     */
    struct sigaction act;
    (void) sigemptyset(&act.sa_mask);

    act.sa_handler = SIG_DFL;
    act.sa_flags = 0;  /* omit SA_RESTART */
    if (sigaction(SIGALRM, &act, (struct sigaction *) NULL) != 0) {
        syslog_perror("sigaction", errno);
        return false;
    }

    act.sa_handler = daemon_sa_chld;
    act.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &act, (struct sigaction *) NULL) != 0) {
        syslog_perror("sigaction", errno);
        return false;
    }

    act.sa_handler = daemon_sa_handler;
    act.sa_flags = SA_RESTART;
    if (sigaction(SIGHUP, &act, (struct sigaction *) NULL) != 0) {
        syslog_perror("sigaction", errno);
        return false;
    }

    act.sa_handler = daemon_sa_handler;
    act.sa_flags = 0;  /* omit SA_RESTART */
    if (   sigaction(SIGINT,  &act, (struct sigaction *) NULL) != 0
        || sigaction(SIGTERM, &act, (struct sigaction *) NULL) != 0) {
        syslog_perror("sigaction", errno);
        return false;
    }

    return true;
}

static bool
daemon_init (const int supervised)
{
    /* Note: not retrying upon interruption; any fail to init means exit fail */

    /* Change current working dir to / for sane cwd and to limit mounts in use*/
    if (0 != chdir("/")) {
        syslog_perror("chdir /", errno);
        return false;
    }

    /* Detach from parent (process to be inherited by init) unless supervised */
    if (supervised) {
        if (setsid() == (pid_t)-1) {
            syslog_perror("setsid", errno);
            return false;
        }
    }
    else {
        pid_t pid;

        /* Ensure that SIGCHLD is not ignored (might be inherited from caller)*/
        struct sigaction act;
        (void) sigemptyset(&act.sa_mask);
        act.sa_handler = SIG_DFL;
        act.sa_flags = SA_RESTART;
        if (sigaction(SIGCHLD, &act, (struct sigaction *) NULL) != 0) {
            syslog_perror("sigaction", errno);
            return false;
        }

        if ((pid = fork()) != 0) {   /* parent */
            int status = EXIT_FAILURE;
            if (pid > 0 && waitpid(pid, &status, 0) != pid)
                status = EXIT_FAILURE;
            _exit(status);
        }                            /* child */
        else if ((pid = setsid()) == (pid_t)-1 || (pid = fork()) != 0) {
            if ((pid_t)-1 == pid) syslog_perror("setsid,fork", errno);
            _exit((pid_t)-1 == pid);
        }                            /* grandchild falls through */
    }

    /* Close unneeded file descriptors */
    /* (not closing all fds > STDERR_FILENO; lazy and we check root is caller)
     * (if closing all fds, must then closelog(); bindsocket_openlog()) */
    if (0 != retry_close(STDIN_FILENO))  return false;
    if (0 != retry_close(STDOUT_FILENO)) return false;
    if (!supervised) {
        if (0 != retry_close(STDERR_FILENO)) return false;
        syslog_perror_level = 1;
    }
    else {
        /* STDERR_FILENO must be open so it is not reused for sockets */
        struct stat st;
        if (0 != fstat(STDERR_FILENO, &st)) {
            syslog_perror("stat STDERR_FILENO", errno);
            return false;
        }
    }

    /* Configure signal handlers for bindsocket desired behaviors */
    if (!daemon_signal_init())
        return false;

    return true;
}

static int bindsocket_daemon_pid = -1;

static void
bindsocket_daemon_atexit (void)
{
    if (getpid() == bindsocket_daemon_pid)
        unlink(BINDSOCKET_SOCKET);
}

static int
bindsocket_daemon_init_socket (void)
{
    struct passwd *pw;
    struct stat st;
    int sfd;
    const uid_t euid = geteuid();
    mode_t mask;

    /* sanity check ownership and permissions on dir that will contain socket */
    /* (note: not checking entire tree above BINDSOCKET_SOCKET_DIR; TOC-TOU) */
    if (0 != stat(BINDSOCKET_SOCKET_DIR, &st)) {
        syslog_perror(BINDSOCKET_SOCKET_DIR, errno);
        return -1;
    }
    if (st.st_uid != euid || (st.st_mode & (S_IWGRP|S_IWOTH))) {
        syslog_perror("ownership/permissions incorrect on "
                      BINDSOCKET_SOCKET_DIR, 0);
        return -1;
    }

    mask = umask(0177); /* create socket with very restricted permissions */
    sfd = bindsocket_unixdomain_socket_bind_listen(BINDSOCKET_SOCKET);
    umask(mask);        /* restore prior umask */
    if (-1 == sfd) {
        syslog_perror("socket,bind,listen", errno);
        return -1;
    }

    bindsocket_daemon_pid = getpid();
    atexit(bindsocket_daemon_atexit);

    if (NULL != (pw = getpwnam(BINDSOCKET_GROUP))
        && 0 == chown(BINDSOCKET_SOCKET, euid, pw->pw_gid)
        && 0 == chmod(BINDSOCKET_SOCKET, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP))
        return sfd;

    syslog_perror("getpwnam,chown,chmod", errno);
    return -1;
}

int
main (int argc, char *argv[])
{
    int sfd, cfd, daemon = false, supervised = false;

    /* setuid safety measures must be performed before anything else */
    if (!setuid_stdinit())
        return EXIT_FAILURE;

    /* openlog() for syslog() */
    bindsocket_openlog();

    /* parse arguments */
    optind = 1;
    while ((sfd = getopt(argc, argv, "dhF")) != -1) {
        switch (sfd) {
          case 'd': daemon = true; break;
          case 'F': supervised = true; break;
          default:  syslog(LOG_ERR, "bad arguments sent by uid %d", getuid());
                    fprintf(stderr, "\nerror: invalid arguments\n");/*fallthru*/
          case 'h': fprintf(stdout, "\n"
                            "  bindsocket -h\n"
                            "  bindsocket -d [-F]\n"
                            "  bindsocket <addr_family> <socktype> <protocol> "
                                        "<service_or_port> <addr>\n\n");
                    return (sfd == 'h' ? EXIT_SUCCESS : EXIT_FAILURE);
        }
    }

    /*
     * one-shot mode; handle single request and exit
     */

    if (!daemon) {
        struct stat st;
        if (0 != fstat(STDIN_FILENO, &st)) {
            syslog_perror("fstat stdin", errno);
            return EXIT_FAILURE;
        }
        if (S_ISSOCK(st.st_mode))
            return bindsocket_client_session(STDIN_FILENO,
                                             argc-optind, argv+optind);
        syslog_perror("invalid socket on bindsocket stdin", 0);
        return EXIT_FAILURE; /* STDIN_FILENO must be socket for one-shot mode */
    }

    /*
     * daemon mode
     */

    if (getuid() != geteuid()) {
        /* do not permit setuid privileges to initiate daemon mode */
        syslog_perror(BINDSOCKET_SYSLOG_IDENT
                      " daemon can not be started via setuid", 0);
        return EXIT_FAILURE;
    }

    if (!daemon_init(supervised))
        return EXIT_FAILURE;

    sfd = bindsocket_daemon_init_socket();
    if (-1 == sfd)
        return EXIT_FAILURE;

    /* daemon event loop
     * parent: accept and fork
     * child: close listen sfd, check credentials, bind socket, send sock fd
     * (Note: by virtue of daemon_init() which detaches from calling process,
     *  bindsocket has no child processes at this point.  (If supervised, then
     *  bindsocket started as root and bindsocket should not have any children))
     * (Note: technically, bindsocket_children increment should be made atomic)
     */
    bindsocket_children = 0;
    do {
        if (bindsocket_children > BINDSOCKET_CHILD_HIWAT)
            bindsocket_wait_children();
        if (-1 != (cfd = accept(sfd, NULL, NULL))) {
            ++bindsocket_children;
            if (0 == fork()) {
                retry_close(sfd);
                _exit(bindsocket_client_session(cfd, 0, NULL));
            }
            retry_close(cfd);
        }
        else if (errno != EINTR && errno != ECONNABORTED)
            break;
    } while (1);
    syslog_perror("accept", errno);
    return EXIT_FAILURE;
}