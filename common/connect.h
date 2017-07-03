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

#ifndef __CONNECT_H__
#define __CONNECT_H__

#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include "config.h"
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#ifndef SUN_LEN
#define SUN_LEN(ptr) ((socklen_t) (((struct sockaddr_un *) 0)->sun_path) \
                     + strlen ((ptr)->sun_path))
#endif

#include "common.h"
#include "config.h"

#ifdef __cplusplus
typedef bool     kgtk_bool;
#define KGTK_TRUE true
#define KGTK_FALSE false
#else
typedef gboolean kgtk_bool;
#define KGTK_TRUE TRUE
#define KGTK_FALSE FALSE
#endif


static int kdialogdSocket=-1;

/* From kdelibs/kdesu */
#ifdef KDIALOGD_APP
static int createSocketConnectionReal()
#else
static int createSocketConnection()
#endif
{
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x01) printf("KGTK::createSocketConnection A\n");
#endif
    int sockfd=-1;
    const char *sock=getSockName();
    struct sockaddr_un addr;

    if (access(sock, R_OK|W_OK))
    {
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x01) printf("KGTK::createSocketConnection - Could not access socket, %s\n", sock);
#endif
        return -1;
    }

    sockfd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x01) printf("KGTK::createSocketConnection - Could not create socket, %d\n", errno);
#endif
        return -1;
    }
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, sock);

    if (connect(sockfd, (struct sockaddr *) &addr, SUN_LEN(&addr)) < 0)
    {
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x01) printf("KGTK::createSocketConnection - Could not connect socket, %d\n", errno);
#endif
        close(sockfd);
        return -1;
    }

#if !defined(SO_PEERCRED) || !defined(HAVE_STRUCT_UCRED)
# if defined(HAVE_GETPEEREID)
    {
    uid_t euid;
    gid_t egid;
    /* Security: if socket exists, we must own it */
    if (getpeereid(sockfd, &euid, &egid) == 0)
    {
       if (euid != getuid())
       {
#ifdef KGTK_DEBUG
            if(kgtkDebug&0x01) printf("KGTK::createSocketConnection - socket not owned by me! socket uid %d\n", euid);
#endif
            close(sockfd);
            return -1;
       }
    }
    }
# else
#  ifdef __GNUC__
#   warning "Using sloppy security checks"
#  endif
    /* We check the owner of the socket after we have connected.
       If the socket was somehow not ours an attacker will be able
       to delete it after we connect but shouldn't be able to
       create a socket that is owned by us. */
    {
    struct stat s;
    if (lstat(sock, &s)!=0)
    {
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x01) printf("KGTK::createSocketConnection - stat failed %s\n", sock);
#endif
        close(sockfd);
        return -1;
    }
    if (s.st_uid != getuid())
    {
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x01) printf("KGTK::createSocketConnection - socket not owned by me! socket uid %d\n", s.st_uid);
#endif
        close(sockfd);
        return -1;
    }
    if (!S_ISSOCK(s.st_mode))
    {
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x01) printf("KGTK::createSocketConnection - socket is not a socket %s\n", sock);
#endif
        close(sockfd);
        return -1;
    }
    }
# endif
#else
    {
    struct ucred cred;
    socklen_t siz = sizeof(cred);

    /* Security: if socket exists, we must own it */
    if (getsockopt(sockfd, SOL_SOCKET, SO_PEERCRED, &cred, &siz) == 0)
    {
        if (cred.uid != getuid())
        {
#ifdef KGTK_DEBUG
            if(kgtkDebug&0x01) printf("KGTK::createSocketConnection - socket not owned by me! socket uid %d\n", cred.uid);
#endif
            close(sockfd);
            return -1;
        }
    }
    }
#endif

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x01) printf("KGTK::createSocketConnection - sockfd:%d\n", sockfd);
#endif
    return sockfd;
}

#ifdef KDIALOGD_APP
static int createSocketConnection()
{
    int rv=-1,
        tries=0;

    do
    {
        if(-1==(rv=createSocketConnectionReal()))
            usleep(100000);
    }
    while(-1==rv && ++tries<50);

    if(-1==rv)
        fprintf(stderr, "ERROR: Could not talk to KDialogD!!!\n");
    return rv;
}
#endif

static int kdialogdPid=-1;

static kgtk_bool processIsRunning()
{
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x01) printf("KGTK::processIsRunning\n");
#endif

    if(-1!=kdialogdPid && 0==kill(kdialogdPid, 0))
    {
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x01) printf("KGTK::processIsRunning (%d) YES\n", kdialogdPid);
#endif
        return KGTK_TRUE;
    }
    else
    {
        FILE *f=fopen(getPidFileName(), "r");

        if(f)
        {
            int pid=0;

            if(1==fscanf(f, "%d", &pid))
            {
                fclose(f);

                if(-1!=kdialogdPid && kdialogdPid!=pid)
                {
#ifdef KGTK_DEBUG
                    if(kgtkDebug&0x01) printf("KGTK::processIsRunning pid has changed from:%d to %d - need to reconnect\n", kdialogdPid, pid);
#endif
                    kdialogdPid=pid;
                    return KGTK_FALSE;
                }
#ifdef KGTK_DEBUG
                if(kgtkDebug&0x01) printf("KGTK::processIsRunning file has pid:%d\n", pid);
#endif
                if(0==kill(pid, 0))
                {
#ifdef KGTK_DEBUG
                    if(kgtkDebug&0x01) printf("KGTK::processIsRunning (file:%d) YES\n", pid);
#endif
                    kdialogdPid=pid;
                    return KGTK_TRUE;
                }

                kdialogdPid=-1; /* Process is not running! */
            }
        }
    }
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x01) printf("KGTK::processIsRunning NO\n");
#endif
    return KGTK_FALSE;
}

static void closeConnection()
{
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x01) printf("KGTK::closeConnection\n");
#endif
    close(kdialogdSocket);
    kdialogdSocket=-1;
}

/* Note: Calling 'fork' seems to mess things up with eclipse! */
#define KGTK_USE_SYSTEM_CALL

static kgtk_bool connectToKDialogD(const char *appName)
{
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x01) printf("KGTK::connectToKDialogD %s\n", appName ? appName : "<null>");
#endif
    if(!processIsRunning())
        closeConnection();

    if(-1!=kdialogdSocket)
        return KGTK_TRUE;
    else
    {
        unsigned int slen=strlen(appName);
        kgtk_bool    rv=KGTK_TRUE;

        if(slen)
            slen++;

#ifdef KGTK_DEBUG
        if(kgtkDebug&0x01) printf("KGTK::connectToKDialogD - start app\n");
#endif

#ifdef KDIALOGD_APP
        grabLock(5);
#ifdef KGTK_USE_SYSTEM_CALL
        system(KDIALOGD_LOCATION"/kdialogd4 &");
#else
        switch(fork())
        {
            case -1:
                rv=KGTK_FALSE;
                printf("ERROR: Could not start fork :-(\n");
                break;
            case 0:
                execl(KDIALOGD_LOCATION"/kdialogd4", "kdialogd4", (char *)NULL);
                break;
            default:
            {
                int status=0;
                wait(&status);
            }
        }
#endif
        releaseLock();
#endif

        if(!rv)
            return rv;

        rv=
#ifdef KDIALOGD_APP
           grabLock(3)>0 &&
#else
           0==system("dcop kded kded loadModule kdialogd") &&
#endif
           -1!=(kdialogdSocket=createSocketConnection()) &&
           writeBlock(kdialogdSocket, (char *)&slen, 4) &&
           (0==slen || writeBlock(kdialogdSocket, appName, slen));
#ifdef KDIALOGD_APP
        releaseLock();
#endif
        return rv;
    }
}

#endif
