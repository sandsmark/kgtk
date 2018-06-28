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

#define USE_KWIN

#include "kdialogd.h"
#include <iostream>
#include <kaboutdata.h>
#include <qapplication.h>
#include <QtCore/QSocketNotifier>
#include <QX11Info>
#include <QBoxLayout>
#include <QCheckBox>
#include <QUrl>
#include <kio/statjob.h>
#include <kjobwidgets.h>
#include <kmessagebox.h>
#include <klocalizedstring.h>
#include <kconfig.h>
#include <kurlcombobox.h>
#include <kconfiggroup.h>
#ifdef USE_KWIN
#include <kwindowsystem.h>
#else
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <fixx11h.h>
#endif
#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <qdebug.h>
#ifdef KDIALOGD_APP
#include <QTimer>
#include <QCommandLineParser>
#include <kdbusservice.h>
#endif
#include <fstream>

KConfig *KDialogD::theirConfig=NULL;

#define CFG_KEY_DIALOG_SIZE "KDialogDSize"
#define CFG_KEY_URLS        "Urls"
#define CFG_TIMEOUT_GROUP   "General"
#ifdef KDIALOGD_APP
#define CFG_TIMEOUT_KEY     "Timeout"
#define DEFAULT_TIMEOUT     30
#endif

static QString groupName(const QString &app, bool fileDialog=true)
{
    return QString(fileDialog ? "KFileDialog " : "KDirSelectDialog ")+app;
}

// from kdebase/kdesu
typedef unsigned ksocklen_t;

static int createSocket()
{
    int         socketFd;
    ksocklen_t  addrlen;
    struct stat s;
    const char  *sock=getSockName();
    int stat_err=lstat(sock, &s);

    if(!stat_err && S_ISLNK(s.st_mode))
    {
        qWarning() << "Someone is running a symlink attack on you" ;
        if(unlink(sock))
        {
            qWarning() << "Could not delete symlink" ;
            return -1;
        }
    }

    if (!access(sock, R_OK|W_OK))
    {
        qWarning() << "stale socket exists" ;
        if (unlink(sock))
        {
            qWarning() << "Could not delete stale socket" ;
            return -1;
        }
    }

    socketFd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (socketFd < 0)
    {
        qCritical() << "socket(): " << strerror(errno);
        return -1;
    }

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path)-1);
    addr.sun_path[sizeof(addr.sun_path)-1] = '\000';
    addrlen = SUN_LEN(&addr);
    if (bind(socketFd, (struct sockaddr *)&addr, addrlen) < 0)
    {
        qCritical() << "bind(): " << strerror(errno);
        return -1;
    }

    struct linger lin;
    lin.l_onoff = lin.l_linger = 0;
    if (setsockopt(socketFd, SOL_SOCKET, SO_LINGER, (char *) &lin,
                   sizeof(linger)) < 0)
    {
        qCritical() << "setsockopt(SO_LINGER): " << strerror(errno);
        return -1;
    }

    int opt = 1;
    if (setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, (char *) &opt,
                   sizeof(opt)) < 0)
    {
        qCritical() << "setsockopt(SO_REUSEADDR): " << strerror(errno);
        return -1;
    }
    opt = 1;
    if (setsockopt(socketFd, SOL_SOCKET, SO_KEEPALIVE, (char *) &opt,
                   sizeof(opt)) < 0)
    {
        qCritical() << "setsockopt(SO_KEEPALIVE): " << strerror(errno);
        return -1;
    }
    chmod(sock, 0600);
    if (listen(socketFd, 1) < 0)
    {
        qCritical() << "listen(): " << strerror(errno);
        return -1;
    }

    return socketFd;
}

static QStringList urls2Local(const QList<QUrl> &urls, QWidget *parent)
{
    QStringList items;
    foreach (const QUrl &url, urls)
    {
        qDebug() << "URL" << url << " local? " << url.isLocalFile();
        if (url.isLocalFile()) items.append(url.path());
        else
        {
            KIO::StatJob *job = KIO::mostLocalUrl(url);
            KJobWidgets::setWindow(job, parent);
            job->exec();
            const QUrl localUrl = job->mostLocalUrl();
            qDebug() << "mostLocal" << localUrl << "local?" << localUrl.isLocalFile();
            if (localUrl.isLocalFile()) items.append(localUrl.path());
            else break;
        }
    }
    return items;
}

KDialogD::KDialogD(QObject *parent)
        : QObject(parent),
#ifdef KDIALOGD_APP
          itsTimer(NULL),
          itsTimeoutVal(DEFAULT_TIMEOUT),
#endif
          itsFd(::createSocket()),
          itsNumConnections(0)
{
    if(itsFd<0)
    {
        qCritical() << "KDialogD could not create socket";
#ifdef KDIALOGD_APP
        QCoreApplication::exit(1);
#endif
    }
    else
    {
        std::ofstream f(getPidFileName());

        if(f)
        {
            f << getpid();
            f.close();
        }
        if(!theirConfig)
            theirConfig=new KConfig("kdialogd5rc"); // , KConfig::OnlyLocal);

        connect(new QSocketNotifier(itsFd, QSocketNotifier::Read, this),
                SIGNAL(activated(int)), this, SLOT(newConnection()));

#ifdef KDIALOGD_APP
        if(theirConfig->hasGroup(CFG_TIMEOUT_GROUP))
        {
            itsTimeoutVal=KConfigGroup(theirConfig, CFG_TIMEOUT_GROUP).readEntry(CFG_TIMEOUT_KEY, DEFAULT_TIMEOUT);
            if(itsTimeoutVal<0)
                itsTimeoutVal=DEFAULT_TIMEOUT;
        }
        qDebug() << "Timeout:" << itsTimeoutVal;
        if(itsTimeoutVal)
        {
            connect(itsTimer=new QTimer(this), SIGNAL(timeout()), this, SLOT(timeout()));
            itsTimer->setSingleShot(true);
        }
#endif
    }
}

KDialogD::~KDialogD()
{
    if(-1!=itsFd)
        close(itsFd);
    if(theirConfig)
        delete theirConfig;
    theirConfig=NULL;
}

void KDialogD::newConnection()
{
    qDebug() << "New connection";

    ksocklen_t         addrlen = 64;
    struct sockaddr_un clientname;
    int                connectedFD;

    if((connectedFD=::accept(itsFd, (struct sockaddr *) &clientname, &addrlen))>=0)
    {
        int appNameLen;

        if(readBlock(connectedFD, (char *)&appNameLen, 4))
        {
            bool     ok=true;
            QByteArray appName;

            if(0==appNameLen)
                appName="Generic";
            else
            {
                appName.resize(appNameLen);
                ok=readBlock(connectedFD, appName.data(), appNameLen);
            }

            if(ok)
            {
                itsNumConnections++;
                qDebug() << "now have" << itsNumConnections << "connections";
#ifdef KDIALOGD_APP
                if(itsTimer)
                    itsTimer->stop();
#endif
                connect(new KDialogDClient(connectedFD, appName, this),
                        SIGNAL(error(KDialogDClient *)),
                        this, SLOT(deleteConnection(KDialogDClient *)));
            }
        }
    }
}

void KDialogD::deleteConnection(KDialogDClient *client)
{
    qDebug() << "Delete client";
    client->deleteLater();

#ifdef KDIALOGD_APP
    if(0==--itsNumConnections)
    {
        qDebug() << "no connections, starting timer";
        if(itsTimeoutVal)
            itsTimer->start(itsTimeoutVal*1000);  // Only single shot...
        else
            timeout();
    }
    else qDebug() << "still have" << itsNumConnections << "connections";
#endif
}

void KDialogD::timeout()
{
#ifdef KDIALOGD_APP
    if(0==itsNumConnections)
    {
        if(grabLock(0)>0)   // 0=> no wait...
        {
            qDebug() << "Timeout and no connections, so exit";
            QCoreApplication::exit(0);
        }
        else     //...unlock lock file...
        {
            qDebug() << "Timeout, but unable to grab lock file - app must be connecting";
            releaseLock();
        }
    }
#endif
}

KDialogDClient::KDialogDClient(int sock, const QString &an, QObject *parent)
              : QObject(parent),
                itsFd(sock),
                itsDlg(NULL),
                itsXid(0),
                itsAccepted(false),
                itsAppName(an)
{
    qDebug() << "new client..." << itsAppName << " (" << itsFd << ")";
    connect(new QSocketNotifier(itsFd, QSocketNotifier::Read, this), SIGNAL(activated(int)), this, SLOT(read()));
    connect(new QSocketNotifier(itsFd, QSocketNotifier::Exception, this), SIGNAL(activated(int)), this, SLOT(close()));
}

KDialogDClient::~KDialogDClient()
{
    qDebug() << "Deleted client" << itsAppName;
    if(-1!=itsFd)
        ::close(itsFd);
    itsDlg=NULL;
    if(KDialogD::config())
        KDialogD::config()->sync();
}

void KDialogDClient::close()
{
    qDebug() << "close" << itsFd;

    if(itsDlg)
    {
        itsDlg->close();
        itsDlg->deleteLater();
        itsDlg=NULL;
        itsXid=0;
    }

    if (itsFd!=-1)
    {
        ::close(itsFd);
        itsFd=-1;
        emit error(this);
    }
}

void KDialogDClient::read()
{
    qDebug() << "read" << itsFd;

    if(-1==itsFd)
        return;

    char         request;
    QString      caption;

    if(!itsDlg && readData(&request, 1) && request>=(char)OP_FILE_OPEN && request<=(char)OP_FOLDER &&
       readData((char *)&itsXid, 4) && readString(caption))
    {
        if("."==caption)
            switch((Operation)request)
            {
                case OP_FILE_OPEN:
                case OP_FILE_OPEN_MULTIPLE:
                    caption=i18n("Open");
                    break;
                case OP_FILE_SAVE:
                    caption=i18n("Save As");
                    break;
                case OP_FOLDER:
                    caption=i18n("Select Folder");
                    break;
                default:
                    break;
            }

        if(OP_FOLDER==(Operation)request)
        {
            QString intialFolder;

            if(readString(intialFolder))
            {
                initDialog(caption, new KDialogDDirSelectDialog(itsAppName, intialFolder, true, 0L));
                return;
            }
        }
        else
        {
            QString intialFolder,
                    filter,
                    customWidgets;
            char    overW=0;

            if(readString(intialFolder) && readString(filter) && readString(customWidgets) &&
               (OP_FILE_SAVE!=(Operation)request || readData(&overW, 1)))
            {
                // LibreOffice has some "/" chars in its filternames - this seems to mess KFileDialog up, and we
                // get blank names! So, foreach filtername we need to replace "/" with "\/"
                if(!filter.isEmpty())
                {
                    QStringList filters=filter.split("\n", QString::SkipEmptyParts);
                    QStringList modified;
                    foreach(QString f, filters)
                    {
                        int sep=f.indexOf('|');
                        if(-1==sep)
                        {
                            modified.append(f);
                        }
                        else
                        {
                            QString ext=f.left(sep+1);
                            QString text=f.mid(sep+1);
                            text.replace("/", "\\/");
                            modified.append(ext+text);
                        }
                    }
                    filter=modified.join("\n");
                }
                
                initDialog(caption, new KDialogDFileDialog(itsAppName, (Operation)request, intialFolder,
                                                           filter, customWidgets, overW ? true : false));
                return;
            }
        }
    }

    qDebug() << "Comms error, closing connection..." << itsFd;
    // If we get here something was wrong, close connection...
    close();
}

void KDialogDClient::finished()
{
    if(-1==itsFd)
        return;

    //
    // * finished is emitted when a dialog is ok'ed/cancel'ed/closed
    // * if the user just closes the dialog - neither ok nor cancel are emitted
    // * the dir select dialog doesnt seem to set the QDialog result parameter
    //   when it is accepted - so for this reason if ok is clicked we store an
    //   'accepted' value there, and check for that after the dialog is finished.
    qDebug() << "finished " << (void *)itsDlg << itsAccepted << (itsDlg ? QDialog::Accepted==itsDlg->result() : false);

    if(itsDlg && !(itsAccepted || QDialog::Accepted==itsDlg->result()))
        cancel();
}

void KDialogDClient::ok(const QStringList &items)
{
    qDebug();

    int                        num=items.count();
    QStringList::ConstIterator it(items.begin()),
                               end(items.end());
    bool                       error=!writeData((char *)&num, 4);

    for(; !error && it!=end; ++it)
    {
        qDebug() << "writeString " << *it;
        error=!writeString(*it);
    }

    if(error)
        close();
    else
        itsAccepted=true;
    if(itsDlg)
        itsDlg->deleteLater();
    itsDlg=NULL;
}

void KDialogDClient::cancel()
{
    qDebug();

    if(itsDlg)
    {
        qDebug() << "send cancel";

        int rv=0;

        if(!writeData((char *)&rv, 4))
        {
            qDebug() << "failed to write data!";
            close();
        }
        if(itsDlg)
            itsDlg->deleteLater();
        itsDlg=NULL;
    }
}

bool KDialogDClient::readData(QByteArray &buffer, int size)
{
    qDebug() << "readData" << itsFd;
    buffer.resize(size);
    return ::readBlock(itsFd, buffer.data(), size);
}

bool KDialogDClient::readString(QString &str)
{
    qDebug() << "readString" << itsFd;

    int size;

    if(!readData((char *)&size, 4))
        return false;

    QByteArray buffer;
    buffer.resize(size);

    if(!readData(buffer.data(), size))
        return false;

    str=QString::fromUtf8(buffer.data());
    return true;
}

bool KDialogDClient::writeString(const QString &str)
{
    qDebug() << "writeString" << itsFd;

    QByteArray utf8(str.toUtf8());

    int size=utf8.length()+1;

    return writeData((char *)&size, 4) && writeData(utf8.data(), size);
}

void KDialogDClient::initDialog(const QString &caption, QDialog *d)
{
    qDebug() << "initDialog" << itsFd;

    itsAccepted=false;
    itsDlg=d;

    if(!caption.isEmpty())
        itsDlg->setWindowTitle(caption);

    if(itsXid)
        itsDlg->installEventFilter(this);

    connect(itsDlg, SIGNAL(ok(const QStringList &)), this, SLOT(ok(const QStringList &)));
    connect(itsDlg, SIGNAL(finished(int)), this, SLOT(finished()));
    itsDlg->show();
}

bool KDialogDClient::eventFilter(QObject *object, QEvent *event)
{
    if(object==itsDlg && QEvent::ShowToParent==event->type())
    {
#ifdef USE_KWIN
        KWindowSystem::setMainWindow(itsDlg, itsXid);
        KWindowSystem::setState(itsDlg->winId(), NET::Modal|NET::SkipTaskbar|NET::SkipPager);

        itsDlg->activateWindow();
        itsDlg->raise();
#if 0
        KWindowInfo wi(KWindowSystem::windowInfo(itsXid, NET::WMGeometry, NET::WM2UserTime));
        QRect       geom(wi.geometry());
        int         rx=geom.x(),
                    ry=geom.y();

        rx=(rx+(geom.width()/2))-(itsDlg->width()/2);
        if(rx<0)
            rx=0;
        ry=(ry+(geom.height()/2))-(itsDlg->height()/2);
        if(ry<0)
            ry=0;
        itsDlg->move(rx, ry);
#endif
        QPixmap icon=KWindowSystem::icon(itsXid, 16, 16, true, KWindowSystem::NETWM | KWindowSystem::WMHints);
        if(!icon.isNull())
            itsDlg->setWindowIcon(QIcon(icon));
#else
        XSetTransientForHint(QX11Info::display(), itsDlg->winId(), itsXid);
#if 0
        XWindowAttributes attr;
        int               rx, ry;
        Window            junkwin;

        if(XGetWindowAttributes(QX11Info::display(), itsXid, &attr))
        {
            XTranslateCoordinates(QX11Info::display(), itsXid, attr.root,
                                    -attr.border_width, -16,
                                    &rx, &ry, &junkwin);

            rx=(rx+(attr.width/2))-(itsDlg->width()/2);
            if(rx<0)
                rx=0;
            ry=(ry+(attr.height/2))-(itsDlg->height()/2);
            if(ry<0)
                ry=0;
            itsDlg->move(rx, ry);
        }
#endif

//         unsigned long num;
//         unsigned long *data = NULL; 
//         Atom prop = XInternAtom(QX11Info::display(), "_NET_WM_ICON", False);
//         Atom typeRet;
//         int formatRet;
//         unsigned long afterRet;
//         if(XGetWindowProperty(QX11Info::display(), itsXid, prop, 0, 0x7FFFFFFF, False, XA_CARDINAL,
//                               &typeRet, &formatRet, &num, &afterRet, (unsigned char **)&data))
//         {
//             qDebug() << "GOT ICON!!!";
//         }
//         else
//             qDebug() << "FAILED TO GET ICON!!!";
#endif
        itsDlg->removeEventFilter(this);
    }

    return false;
}

static QUrl resolveStartDir(const QString &startDir)
{
    return QUrl(startDir.isEmpty() || "~"==startDir ? QDir::homePath() : startDir);
}

KDialogDFileDialog::KDialogDFileDialog(QString &an, Operation op, const QString &startDir,
                                       const QString &filter, const QString &customWidgets, bool confirmOw)
                  : QFileDialog(NULL),
                    itsConfirmOw(confirmOw),
                    itsDone(false),
                    itsAppName(an)
{
    setModal(false);
    if (!confirmOw) setOption(QFileDialog::DontConfirmOverwrite);
    setDirectoryUrl(resolveStartDir(startDir));

    // Need to convert the filter list from KDE to Qt format
    const QStringList filterList = filter.split('\n');
    QStringList qtFilters;
    foreach (const QString &filt, filterList)
    {
        int idx = filt.indexOf('|');
        if (idx!=-1) qtFilters.append(filt.mid(idx+1)+" ("+filt.left(idx)+')');
        else qtFilters.append(filt);
    }
    setNameFilters(qtFilters);

    switch(op)
    {
        case OP_FILE_OPEN:
            setAcceptMode(QFileDialog::AcceptOpen);
            setFileMode(QFileDialog::ExistingFile);
            break;
        case OP_FILE_OPEN_MULTIPLE:
            setAcceptMode(QFileDialog::AcceptOpen);
            setFileMode(QFileDialog::ExistingFiles);
            break;
        case OP_FILE_SAVE:
            setAcceptMode(QFileDialog::AcceptSave);
            setFileMode(QFileDialog::AnyFile);
            break;
        default:
            break;
    }

    if(KDialogD::config())
    {
        KConfigGroup cfg(KDialogD::config(), groupName(itsAppName));

        selectUrl(cfg.readEntry(CFG_KEY_URLS, QStringList()).value(0));
        resize(cfg.readEntry(CFG_KEY_DIALOG_SIZE, QSize(600, 400)));
    }

    if(!customWidgets.isEmpty())
    {
        qWarning() << "Client" << itsAppName << "requests custom widgets, which are not currently supported";

        QWidget *custom=0L;
        QBoxLayout *layout=0;
        QStringList widgets=customWidgets.split("@@", QString::SkipEmptyParts);
        foreach(const QString &wid, widgets)
        {
            QStringList parts=wid.split("||", QString::SkipEmptyParts);
            
            if(2==parts.length())
            {
                QString name=parts[0];
                name.replace("_", "&");
                
                if(!custom)
                {
                    custom=new QWidget();
                    layout=new QBoxLayout(QBoxLayout::TopToBottom, custom);
                    layout->setMargin(0);
                }
                QCheckBox *cb=new QCheckBox(name, custom);
                cb->setChecked("true"==parts[1]);
                layout->addWidget(cb);
                itsCustom.insert(parts[0], cb);
            }
        }
        
        // TODO: support for custom widgets
        //if(custom)
        //    fileWidget()->setCustomWidget(QString(), custom);
    }
}

void KDialogDFileDialog::accept()
{
    QFileDialog::accept();

    QList<QUrl> urls(selectedUrls());
    qDebug() << urls.count() << acceptMode() << urls;
    bool        good=true;

    if(urls.count())
    {
        QStringList items = urls2Local(urls, this);

        if(urls.count()!=items.count())
        {
            KMessageBox::sorry(this, i18n("You can only select local files."),
                               i18n("Remote Files Not Accepted"));
            good=false;
        }
        else if(itsConfirmOw && QFileDialog::AcceptSave==acceptMode())
        {
            KIO::StatJob *job = KIO::stat(urls.first(), KIO::StatJob::DestinationSide, 0);
            KJobWidgets::setWindow(job, this);
            if (job->exec())				// destination exists
            {
                int result = KMessageBox::warningContinueCancel(this,
                                            i18n("File %1 exits.\nDo you want to replace it?")
                                                                           .arg(urls.first().toDisplayString()),
                                            i18n("File Exists"),
                                            KGuiItem(i18n("Replace"), "filesaveas"), KStandardGuiItem::cancel(), QString(),
                                            KMessageBox::Notify|KMessageBox::PlainCaption);

                good = (result==KMessageBox::Continue);
            }
        }

        if(good)
        {
            QString filter = selectedNameFilter();
            if(!filter.isEmpty())
            {
                // Convert the Qt format filter back to KDE format
                int idx = filter.lastIndexOf(" (");
                if (idx!=-1)
                {
                    if (filter.endsWith(')')) filter.chop(1);
                    filter = filter.mid(idx+2)+'|'+filter.left(idx);
                }
                items.append(filter);
            }

            if(itsCustom.count())
            {
                QString custom;
                QMap<QString, QWidget *>::ConstIterator it(itsCustom.constBegin()),
                                                        end(itsCustom.constEnd());
                                                        
                for(; it!=end; ++it)
                    if(qobject_cast<const QCheckBox *>(it.value()))
                        custom=custom+"@@"+it.key()+"||"+QString(static_cast<const QCheckBox *>(it.value())->isChecked() ? "true" : "false");
                if(!custom.isEmpty())
                    items.append(custom);
            }
            
            emit ok(items);
            hide();
       }
       else
            setResult(QDialog::Rejected);
    }
}

KDialogDFileDialog::~KDialogDFileDialog()
{
    qDebug();

    if(KDialogD::config())
    {
        KConfigGroup cfg(KDialogD::config(), groupName(itsAppName));

        cfg.writeEntry(CFG_KEY_URLS, selectedUrls());
        cfg.writeEntry(CFG_KEY_DIALOG_SIZE, size());
    }
}

KDialogDDirSelectDialog::KDialogDDirSelectDialog(QString &an, const QString &startDir, bool localOnly,
                                                 QWidget *parent)
                       : QFileDialog(parent),
                         itsAppName(an)
{
    setModal(false);
    setAcceptMode(QFileDialog::AcceptOpen);
    setFileMode(QFileDialog::Directory);
    setOption(QFileDialog::ShowDirsOnly);
    setDirectoryUrl(resolveStartDir(startDir));
    if (localOnly) setSupportedSchemes(QStringList() << "file");

    if(KDialogD::config())
    {
        KConfigGroup cfg(KDialogD::config(), groupName(itsAppName, false));

        //TODO !!! readConfig(KDialogD::config(), grp);
        resize(cfg.readEntry(CFG_KEY_DIALOG_SIZE, QSize(600, 400)));
    }
}

KDialogDDirSelectDialog::~KDialogDDirSelectDialog()
{
    qDebug();

    if(KDialogD::config())
    {
        KConfigGroup cfg(KDialogD::config(), groupName(itsAppName, false));

        //TODO !!! writeConfig(KDialogD::config(), grp);
        cfg.writeEntry(CFG_KEY_DIALOG_SIZE, size());
    }
}

void KDialogDDirSelectDialog::slotOk()
{
    qDebug();

    QList<QUrl> urls = selectedUrls();
    QStringList items = urls2Local(urls, this);
    if(urls.count()!=items.count())
            KMessageBox::sorry(this, i18n("You can only select local folders."),
                               i18n("Remote Folders Not Accepted"));
    else
    {
        emit ok(items);
        hide();
    }
}

#ifdef KDIALOGD_APP
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    KAboutData about("kdialogd5",					// componentName
                     i18n("KDialog Daemon"),				// displayName
                     VERSION,						// version
                     i18n("Use KDE dialogs from GTK apps."),		// shortDescription
                     KAboutLicense::GPL,				// licenseType,
                     i18n("(c) Craig Drummond 2006-2011"),		// copyrightStatement
                     QString(),						// otherText
                     i18n("https://github.com/sandsmark/kgtk"), 	// homePageAddress
                     i18n("https://github.com/sandsmark/kgtk/issues"));	// bugAddress

    about.setOrganizationDomain("kde.org");
    about.addAuthor(i18n("Craig Drummond"),		// name
                    i18n("Original implementation"));	// task
    about.addCredit(i18n("Martin Sandsmark"),		// name
                    i18n("KF5 port"));			// task
    about.addCredit(i18n("Jonathan Marten"),		// name
                    i18n("KF5 port"));			// task

    KAboutData::setApplicationData(about);
    QCommandLineParser parser;
    about.setupCommandLine(&parser);
    parser.process(app);
    if (parser.isSet("version")) return 0;
    if (parser.isSet("author")) return 0;
    if (parser.isSet("license")) return 0;

    KDBusService service(KDBusService::Unique);
    // get here only if the first instance of the daemon
    KDialogD kdialogd;
    int rv = app.exec();
    unlink(getSockName());
    releaseLock();
    return rv;
}
#else
extern "C"
{
    KDE_EXPORT KDEDModule *create_kdialogd()
    {
        return new KDialogDKDED();
    }
};

KDialogDKDED::KDialogDKDED()
            : KDEDModule()
{
    new KDialogD(this);
}
#endif
