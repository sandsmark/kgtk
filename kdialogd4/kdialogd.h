#ifndef __KDIALOGD_H__
#define __KDIALOGD_H__

#include <kfile.h>
#include <kfiledialog.h>
#include <kfiledialog.h>
#include <kdirselectdialog.h>
#include "common.h"
#include "config.h"
#include <QtCore/QMap>

#ifdef KDIALOGD_APP
class QTimer;
#else
#include <kdedmodule.h>
#endif
class KDialog;
class KConfig;

class KDialogDFileDialog : public KFileDialog
{
    Q_OBJECT

    public:

    KDialogDFileDialog(QString &an, Operation op, const QString& startDir, const QString& filter,
                       const QString &customWidgets, bool confirmOw);
    virtual ~KDialogDFileDialog();

    public slots:

    void accept();

    signals:

    void ok(const QStringList &items);

    private:

    bool                     itsConfirmOw,
                             itsDone;
    QString                  &itsAppName;
    QMap<QString, QWidget *> itsCustom;
};

class KDialogDDirSelectDialog : public KDirSelectDialog
{
    Q_OBJECT

    public:

    KDialogDDirSelectDialog(QString &an, const QString &startDir = QString(),
                            bool localOnly = false,  QWidget *parent = 0L);
    virtual ~KDialogDDirSelectDialog();

    public slots:

    void slotOk();

    signals:

    void ok(const QStringList &items);

    private:

    QString &itsAppName;
};

class KDialogDClient : public QObject
{
    Q_OBJECT

    public:

    KDialogDClient(int sock, const QString &an, QObject *parent);
    virtual ~KDialogDClient();

    public slots:

    void read();
    void close();
    void ok(const QStringList &items);
    void finished();

    signals:

    void error(KDialogDClient *);

    private:

    void cancel();
    bool readData(QByteArray &buffer, int size);
    bool readData(char *buffer, int size)        { return readBlock(itsFd, buffer, size); }
    bool writeData(const char *buffer, int size) { return writeBlock(itsFd, buffer, size); }
    bool readString(QString &str);
    bool writeString(const QString &str);
    void initDialog(const QString &caption, QDialog *d);
    bool eventFilter(QObject *object, QEvent *event);

    private:

    int          itsFd;
    QDialog      *itsDlg;
    unsigned int itsXid;
    bool         itsAccepted;
    QString      itsAppName;
};

class KDialogD : public QObject
{
    Q_OBJECT

    public:

    KDialogD(QObject *parent=0L);
    virtual ~KDialogD();

    public slots:

    void newConnection();
    void deleteConnection(KDialogDClient *client);
    void timeout();

    static KConfig * config() { return theirConfig; }

    private:

#ifdef KDIALOGD_APP
    QTimer *itsTimer;
    int    itsTimeoutVal;
#endif
    int    itsFd,
           itsNumConnections;

    static KConfig *theirConfig;
};

#ifndef KDIALOGD_APP
class KDialogDKDED : public KDEDModule
{
    public:

    KDialogDKDED();
};
#endif

#endif
