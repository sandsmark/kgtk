/*
 * KGtk
 *
 * Copyright 2006-2011 Craig Drummond <craig.p.drummond@gmail.com>
 *
 * ----
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __COMMON_H__
#define __COMMON_H__

#define KDIALOGD_APP

#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include "config.h"

#ifdef KGTK_DEBUG
static int kgtkDebug = 0;
#endif

typedef enum {
    OP_NULL                = 0,
    OP_FILE_OPEN           = 1,
    OP_FILE_OPEN_MULTIPLE  = 2,
    OP_FILE_SAVE           = 3,
    OP_FOLDER              = 4
} Operation;

#define PID_DIR  "kde-"
#define PID_NAME "kdialogd.pid"

static const char *getPidFileName()
{
    static char *pidfile = NULL;

    if (!pidfile) {
        char *user = getenv("USER");

        if (!user) {
            user = getenv("LOGNAME");
        }

        if (user) {
            char *tmp = getenv("KDETMP");

            if (!tmp || !tmp[0]) {
                tmp = getenv("TMPDIR");
            }

            if (!tmp || !tmp[0]) {
                tmp = (char *)"/tmp";
            }

            pidfile = (char *)malloc(strlen(tmp) + strlen(PID_DIR) + strlen(user) + strlen(PID_NAME) + 3); /* 2 slashes and null terminator */

#ifdef __KDIALOGD_H__
            /* We are kdialogd - so create socket folder if it does not exist... */
            sprintf(pidfile, "%s/%s%s", tmp, PID_DIR, user);
            QDir::root().mkpath(pidfile);
#endif

            /* CPD: TODO get dispaly number! */
            sprintf(pidfile, "%s/%s%s/%s", tmp, PID_DIR, user, PID_NAME);
        }
    }

    return pidfile;
}

#define SOCK_DIR  "ksocket-"
#define SOCK_NAME "kdialogd"

static const char *getSockName()
{
    static char *sock = NULL;

    if (!sock) {
        char *user = getenv("USER");

        if (!user) {
            user = getenv("LOGNAME");
        }

        if (user) {
            char *tmp = getenv("KDETMP");

            if (!tmp || !tmp[0]) {
                tmp = getenv("TMPDIR");
            }

            if (!tmp || !tmp[0]) {
                tmp = (char *)"/tmp";
            }

            sock = (char *)malloc(strlen(tmp) + strlen(SOCK_DIR) + strlen(user) + strlen(SOCK_NAME) + 4 + 32); /* 4=2 slashes, 1 dash, and null terminator */

#ifdef __KDIALOGD_H__
            /* We are kdialogd - so create socket folder if it does not exist... */
            sprintf(sock, "%s/%s%s", tmp, SOCK_DIR, user);
            QDir::root().mkpath(sock);
#endif

            /* CPD: TODO get dispaly number! */
            sprintf(sock, "%s/%s%s/%s-%d", tmp, SOCK_DIR, user, SOCK_NAME, 1);
        }
    }

    return sock;
}

static int readBlock(int fd, char *pData, int size)
{
    int bytesToRead = size;

    do {
        fd_set fdSet;

        FD_ZERO(&fdSet);
        FD_SET(fd, &fdSet);

        if (select(fd + 1, &fdSet, NULL, NULL, NULL) < 0) {
            return 0;
        }

        if (FD_ISSET(fd, &fdSet)) {
            int bytesRead = read(fd, &pData[size - bytesToRead], bytesToRead);

            if (bytesRead > 0) {
                bytesToRead -= bytesRead;
            } else {
                return 0;
            }
        }
    } while (bytesToRead > 0);

    return 1;
}

static int writeBlock(int fd, const char *pData, int size)
{
    int bytesToWrite = size;

    do {
        fd_set fdSet;

        FD_ZERO(&fdSet);
        FD_SET(fd, &fdSet);

        if (select(fd + 1, NULL, &fdSet, NULL, NULL) < 0) {
            return 0;
        }

        if (FD_ISSET(fd, &fdSet)) {
            int bytesWritten = write(fd, (char *)&pData[size - bytesToWrite], bytesToWrite);

            if (bytesWritten > 0) {
                bytesToWrite -= bytesWritten;
            } else {
                return 0;
            }
        }
    } while (bytesToWrite > 0);

    return 1;
}

#ifdef KDIALOGD_APP
/*
    So that kdailogd can terminate when the last app exits, need a way of synchronising the Gtk/Qt
    apps that may wish to connect, and the removal of the socket.

    To enable this, a lockfile is created, and used to guard around the critical sections
*/
static int lockFd = -1;

#define LOCK_EXT ".lock"

static const char *getLockName()
{
    static char *lockName = NULL;

    if (!lockName) {
        const char *sock = getSockName();

        if (sock) {
            lockName = (char *)malloc(strlen(sock) + strlen(LOCK_EXT) + 1);
            sprintf(lockName, "%s%s", sock, LOCK_EXT);
        }
    }

    return lockName;
}

/* Lock is stale if it does not exist or is older than 2 seconds */
static int isStale(const char *fname)
{
    struct stat stat_buf;

    return 0 != stat(fname, &stat_buf) ||
           labs(stat_buf.st_mtime - time(NULL)) > 2;
}

static int grabLock(int tries)
{
    do {
        lockFd = open(getLockName(), O_WRONLY | O_CREAT | O_EXCL, 0777);

        if (lockFd < 0 && errno == EEXIST) {
            /* Hmm, lock file already exists. Is it stale? */
            if (isStale(getLockName())) {
                tries++;  /* Increment tries so that we try again... */
                unlink(getLockName());
            } else if (tries) {
                usleep(100000);
            }
        }
    } while (lockFd < 0 && --tries);

    return lockFd;
}

static void releaseLock()
{
    if (lockFd > 0) {
        close(lockFd);
        unlink(getLockName());
    }
}
#endif

#endif
