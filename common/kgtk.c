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

/*
  NOTES:

   1. Inkscape does not use the standard Gtk fileters to determine Save type
      ...it uses an extra combo, which we try to locate.
   2. Firefox. This has two filters with the same patterns - *.htm and *.html. We
      modify this so that one is *.htm and the other is *.html
   3. Glade-2 I noticed this crash a couple of times on loading - but not always.
      Not sure if this is a Glade problem or not...
*/

/*
TODO
  abiword: seems to call gtk_widget_show - but just overriding this cuases the dialog to
           appear twice, and then it still donest use result :-(
  Overload font picker!
  Overload normal old file selector? ie. in addtition to file chooser?
  Message boxes: Auto set alternative button order?
*/


/* Safely read GtkFileChooser filters. We do this by hooking up to the add_filter routines, and storing a copy
 * of their data. When GtkFileChooser is activated, we get its list of filter pointers and look up in our copy.
 * If this is not defined, then the unsafe way of using private structs, etc, is used.
 */
#define KGTK_SAFE_FILTER_LOOKUP

/* Safely support GtkFileChooserButton. Does not currently work, so we access private internals! */
/*
#define KGTK_SAFE_FILE_CHOOSER_BUTTON_SUPPORT
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <gtk/gtk.h>
#include <glib.h>
#include <gdk/gdkx.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pwd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <ctype.h>
#include "connect.h"
#include "config.h"

#ifndef KGTK_DLSYM_VERSION
#define KGTK_DLSYM_VERSION "GLIBC_2.0"
#endif

#define FUNC_ENTER printf("%s\n", __PRETTY_FUNCTION__);
#define FUNC_EXIT printf("%s\n", __PRETTY_FUNCTION__);

/*
 * For SWT apps (e.g. eclipse) we need to override dlsym, but we can only do this if
 * dlvsym is present in libdl. dlvsym is needed so that we can access the real dlsym
 * as well as our fake dlsym
 */
#ifdef HAVE_DLVSYM
static void * real_dlsym (void *handle, const char *name);
#else
#define real_dlsym(A, B) dlsym(A, B)
#endif

typedef enum
{
    APP_ANY,
    APP_GIMP,
    APP_INKSCAPE,
    APP_FIREFOX,
    APP_KINO,
    APP_LIBREOFFICE
} Application;

static const char  *kgtkAppName=NULL;
static gboolean    useKde=FALSE;
static gboolean    kdialogdError=FALSE;
static GMainLoop   *kdialogdLoop=NULL;
static const gchar *kgtkFileFilter=NULL;
static Application kgtkApp=APP_ANY;

#define MAX_DATA_LEN 4096
#define MAX_FILTER_LEN 256
#define MAX_LINE_LEN 1024
#define MAX_APP_NAME_LEN 32

static char * kgtk_get_app_name(int pid)
{
    static char app_name[MAX_APP_NAME_LEN+1]="\0";

    int  procFile=-1;
    char cmdline[MAX_LINE_LEN+1];

    sprintf(cmdline, "/proc/%d/cmdline",pid);

    if(-1!=(procFile=open(cmdline, O_RDONLY)))
    {
        if(read(procFile, cmdline, MAX_LINE_LEN)>2)
        {
            int len=strlen(cmdline),
                pos=0;

            for(pos=len-1; pos>0 && cmdline[pos] && cmdline[pos]!='/'; --pos)
                ;

            if(pos>=0 && pos<len)
            {
                strncpy(app_name, &cmdline[pos ? pos+1 : 0], MAX_APP_NAME_LEN);
                app_name[MAX_APP_NAME_LEN]='\0';
            }
        }
        close(procFile);
    }

    return app_name;
}

/* Try to get name of application executable - either from argv[0], or /proc */
static const gchar *getAppName(const gchar *app)
{
    static const char *appName=NULL;

    if(!appName)
    {
        /* Is there an app name set?  - if not read from /proc */
        const gchar *a=app ? app : kgtk_get_app_name(getpid());
        gchar       *slash;

        /* Was the cmdline app java? if so, try to use its parent name - just in case
           its run from a shell script, etc. - e.g. as eclipse does */
        if(a && 0==strcmp(a, "java"))
            a=kgtk_get_app_name(getppid());

        if(a && a[0]=='\0')
            a=NULL;

        appName=a && (slash=strrchr(a, '/')) && '\0'!=slash[1]
                    ? &(slash[1])
                    : a ? a : "GtkApp";
    }

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::getAppName: %s\n", appName);
#endif
    return appName;
}

static gboolean writeString(const char *s)
{
    unsigned int slen=strlen(s)+1;

    return writeBlock(kdialogdSocket, (char *)&slen, 4) &&
           writeBlock(kdialogdSocket, s, slen);
}

static gboolean writeBool(gboolean b)
{
    char bv=b ? 1 : 0;

    return writeBlock(kdialogdSocket, (char *)&bv, 1);
}

typedef struct
{
    GSList *res;
    gchar  *selFilter;
    gchar  *customRv;
} KGtkData;

static gpointer kdialogdMain(gpointer data)
{
    KGtkData *d=(KGtkData *)data;
    char     buffer[MAX_DATA_LEN+1]={'\0'};
    int      num=0;

    if(readBlock(kdialogdSocket, (char *)&num, 4))
    {
        int n;

        for(n=0; n<num && !kdialogdError; ++n)
        {
            int size=0;

            if(readBlock(kdialogdSocket, (char *)&size, 4))
            {
                if(size>0)
                {
                    if(size<=MAX_DATA_LEN && readBlock(kdialogdSocket, buffer, size))
                    {
                        /*buffer[size-1]='\0'; */
                        if('/'==buffer[0])
                            d->res=g_slist_prepend(d->res, g_filename_from_utf8(buffer, -1, NULL, NULL, NULL));
                        else if('@'==buffer[0] && '@'==buffer[1] && !(d->customRv))
                            d->customRv=g_strdup(buffer);
                        else if(!(d->selFilter))
                            d->selFilter=g_strdup(buffer);
                    }
                    else
                        kdialogdError=TRUE;
                }
            }
            else
                kdialogdError=TRUE;
        }
    }
    else
        kdialogdError=TRUE;

    if(g_main_loop_is_running(kdialogdLoop))
        g_main_loop_quit(kdialogdLoop);

    return 0L;
}

static gboolean sendMessage(GtkWidget *widget, Operation op, GSList **res, gchar **selFilter, gchar **customRv,
                            const char *title, const char *p1, const char *p2, const char *p3, gboolean overWrite)
{
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::sendMessage\n");
#endif

    if(connectToKDialogD(getAppName(kgtkAppName)))
    {
        char o=(char)op;
        int  xid=0;

        if(widget)
        {
            if(gtk_widget_get_parent(widget))
            {
#ifdef KGTK_DEBUG
                if(kgtkDebug&0x02) printf("KGTK::Dialog has a parent!\n");
#endif
                xid=GDK_WINDOW_XID(gtk_widget_get_window(gtk_widget_get_toplevel(gtk_widget_get_parent(widget))));
            }

            /*
                Inkscape's 0.44 export bitmap filechooser is set to be transient for the main window, and
                not the export dialog. This makes the KDE dialog appear underneath it! So, for inkscape
                dont try to get the window transient for...

                ...might need to remove this whole section, if it starts to affect too many apps
            */
            if(!xid && APP_INKSCAPE!=kgtkApp && GTK_IS_WINDOW(widget))
            {
                GtkWindow *win=gtk_window_get_transient_for(GTK_WINDOW(widget));

#ifdef KGTK_DEBUG
                if(kgtkDebug&0x02) printf("KGTK::Get window transient for...\n");
#endif
                if(win && gtk_window_get_focus(win))
                    xid=GDK_WINDOW_XID(gtk_widget_get_window(gtk_widget_get_toplevel(gtk_window_get_focus(win))));
            }
        }

        if(!xid)
        {
            GList *topWindows,
                  *node;
            int   prevX=0;

#ifdef KGTK_DEBUG
            if(kgtkDebug&0x02) printf("KGTK::No xid, need to traverse window list...\n");
#endif
            for(topWindows=node=gtk_window_list_toplevels(); node; node = node->next)
            {
                GtkWidget *w=node->data;

                if(w && GTK_IS_WIDGET(w) && gtk_widget_get_window(w))
                {
                   if(gtk_window_has_toplevel_focus(GTK_WINDOW(w)) &&
                      gtk_window_is_active(GTK_WINDOW(w)))
                    {
                        /* If the currently active window is a popup - then assume it is a popup-menu,
                         * so use the previous window as the one to be transient for...*/
                        if(GTK_WINDOW_POPUP==gtk_window_get_window_type(GTK_WINDOW(w)) && prevX)
                            xid=prevX;
                        else
                            xid=GDK_WINDOW_XID(gtk_widget_get_window(w));
                        if(xid)
                            break;
                    }
                    else
                        prevX=GDK_WINDOW_XID(gtk_widget_get_window(w));
                }
            }
            g_list_free(topWindows);
        }

        if(writeBlock(kdialogdSocket, &o, 1) &&
           writeBlock(kdialogdSocket, (char *)&xid, 4) &&
           writeString(title) &&
           (p1 ? writeString(p1) : TRUE) &&
           (p2 ? writeString(p2) : TRUE) &&
           (p3 ? writeString(p3) : TRUE) &&
           (OP_FILE_SAVE==op ? writeBool(overWrite) : TRUE))
        {
            GtkWidget *dlg=gtk_dialog_new();
            KGtkData  d;

            gtk_widget_set_name(dlg, "--kgtk-modal-dialog-hack--");
            d.res=NULL;
            d.selFilter=NULL;
            d.customRv=NULL;

            /* Create a tmporary, hidden, dialog so that the kde dialog appears as modal */
            g_object_ref(dlg);
            gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
            gtk_window_iconify(GTK_WINDOW(dlg));
#if !GTK_CHECK_VERSION(3, 0, 0) 
            gtk_dialog_set_has_separator(GTK_DIALOG(dlg), FALSE);
            gtk_window_set_has_frame(GTK_WINDOW(dlg), FALSE);
#endif
            gtk_window_set_decorated(GTK_WINDOW(dlg), FALSE);
            gtk_window_set_keep_below(GTK_WINDOW(dlg), TRUE);
            gtk_window_set_opacity(GTK_WINDOW(dlg), 100);
            gtk_window_set_type_hint(GTK_WINDOW(dlg), GDK_WINDOW_TYPE_HINT_DOCK);
            gtk_widget_show(dlg);
            gtk_window_move(GTK_WINDOW(dlg), 32768, 32768);
            gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dlg), TRUE);
            gtk_window_set_skip_pager_hint(GTK_WINDOW(dlg), TRUE);
            kdialogdLoop = g_main_loop_new (NULL, FALSE);
            kdialogdError=FALSE;
            g_thread_create(&kdialogdMain, &d, FALSE, NULL);

            GDK_THREADS_LEAVE();
            g_main_loop_run(kdialogdLoop);
            GDK_THREADS_ENTER();
            g_main_loop_unref(kdialogdLoop);
            kdialogdLoop = NULL;
            gtk_window_set_modal(GTK_WINDOW(dlg), FALSE);
            g_object_unref(dlg);
            gtk_widget_destroy(dlg);
            if(kdialogdError)
            {
                closeConnection();
                return FALSE;
            }
            if(d.res)
            {
                if(res)
                    *res=d.res;
                else
                    g_slist_free(d.res);
            }
            if(d.selFilter)
            {
                if(selFilter)
                    *selFilter=d.selFilter;
                else
                    g_free(d.selFilter);
            }
            if(d.customRv)
            {
                if(customRv)
                    *customRv=d.customRv;
                else
                    g_free(d.customRv);
            }
            return TRUE;
        }
    }

    return FALSE;
}

static gchar * firstEntry(GSList *files)
{
    gchar *file=NULL;

    if(files)
    {
        file=(gchar *)(files->data);
        files=g_slist_delete_link(files, files);
        if(files)
        {
            g_slist_foreach(files, (GFunc)g_free, NULL);
            g_slist_free(files);
            files=NULL;
        }
    }

    return file;
}

static const char * getTitle(const char *title)
{
    if(title && strlen(title))
        return title;

    return ".";
}

static gboolean openKdeDialog(GtkWidget *widget, const char *title, const char *p1, const char *p2, const char *p3,
                              Operation op, GSList **res, gchar **selFilter, gchar **customRv, gboolean overWrite)
{
    gboolean rv=sendMessage(widget, op, res, selFilter, customRv, getTitle(title), p1, p2, p3, overWrite);

     /* If we failed to talk to, or start kdialogd, then dont keep trying - just fall back to Gtk */
/*
     if(!rv) 
         useKde=FALSE;
*/

    return rv;
}

static void kgtkExit()
{
    if(useKde)
        closeConnection();
}

static gboolean isApp(const char *str, const char *app)
{
    /* Standard case... */
    if(0==strcmp(str, app))
        return TRUE;

    /* Autopackage'd app */
    #define AUTOPACKAGE_PROXY     ".proxy."
    #define AUTOPACKAGE_PROXY_LEN 7

    if(str==strstr(str, ".proxy.") && strlen(str)>AUTOPACKAGE_PROXY_LEN &&
       0==strcmp(&str[AUTOPACKAGE_PROXY_LEN], app))
        return TRUE;

    /* gimp and mozilla */
    {
    unsigned int app_len=strlen(app);

    if(strlen(str)>app_len && str==strstr(str, app) &&
       (0==memcmp(&str[app_len], "-2", 2) ||
        0==memcmp(&str[app_len], "-bin", 4)))
        return TRUE;
    }

    return FALSE;
}

static gboolean isMozApp(const char *app, const char *check)
{
    if(0==strcmp(app, check))
        return TRUE;
    else if(app==strstr(app, check))
    {
        int app_len=strlen(app),
            check_len=strlen(check);

        if(check_len+4 == app_len && 0==strcmp(&app[check_len], "-bin"))
            return TRUE;

        /* OK check for xulrunner-1.9 */
        {
        float dummy;
        if(app_len>(check_len+1) && 1==sscanf(&app[check_len+1], "%f", &dummy))
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean kgtkInit(const char *appName)
{
    static gboolean initialised=FALSE;

    if(!initialised)
    {
#ifdef KGTK_DEBUG
        char *env=getenv("KGTK_DEBUG");
        kgtkDebug=env ? strtoul(env, NULL, 0) : 0;
        if(kgtkDebug&0x02) printf("KGTK::Running under KDE? %d\n", NULL!=getenv("KDE_FULL_SESSION"));
#endif

        initialised=TRUE;
        kgtkAppName=getAppName(appName);
        useKde=connectToKDialogD(kgtkAppName);
        if(useKde)
        {
            const gchar *prg=getAppName(NULL);

            if(prg)
            {
#ifdef KGTK_DEBUG
                if(kgtkDebug&0x02) printf("KGTK::APP %s\n", prg);
#endif
                if(isApp(prg, "inkscape"))
                {
                    kgtkFileFilter="*.svg|Scalable Vector Graphic";
                    kgtkApp=APP_INKSCAPE;
#ifdef KGTK_DEBUG
                    if(kgtkDebug&0x02) printf("KGTK::Inkscape\n");
#endif
                }
                else if(isApp(prg, "gimp"))
                {
                    kgtkApp=APP_GIMP;
#ifdef KGTK_DEBUG
                    if(kgtkDebug&0x02) printf("KGTK::GIMP\n");
#endif
                }
                else if(isApp(prg, "kino"))
                {
                    kgtkApp=APP_KINO;
#ifdef KGTK_DEBUG
                    if(kgtkDebug&0x02) printf("KGTK::kino\n");
#endif
                }
                else if(isMozApp(prg, "firefox") || isMozApp(prg, "swiftfox") || isMozApp(prg, "iceweasel") || isMozApp(prg, "xulrunner"))
                {
                    kgtkApp=APP_FIREFOX;
#ifdef KGTK_DEBUG
                    if(kgtkDebug&0x02) printf("KGTK::Firefox\n");
#endif
                }
                else if(isApp(prg, "soffice.bin"))
                {
                    kgtkApp=APP_LIBREOFFICE;
#ifdef KGTK_DEBUG
                    if(kgtkDebug&0x02) printf("KGTK::LibreOffice\n");
#endif
                }
            }

            if(!g_threads_got_initialized)
                g_thread_init(NULL);
            atexit(&kgtkExit);
        }
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x02) printf("KGTK::kgtkInit useKde:%d\n", useKde);
#endif
    }

    return useKde;
}

static GtkWidget *
kgtk_file_chooser_dialog_new_valist (const gchar          *title,
                                    GtkWindow            *parent,
                                    GtkFileChooserAction  action,
                                    const gchar          *backend,
                                    const gchar          *first_button_text,
                                    va_list               varargs)
{
  GtkWidget *result;
  const char *button_text = first_button_text;
  gint response_id;

  result = g_object_new (GTK_TYPE_FILE_CHOOSER_DIALOG,
                         "title", title,
                         "action", action,
                         "file-system-backend", backend,
                         NULL);

  if (parent) {
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("Setting transient for window %p\n", parent);
#endif
    gtk_window_set_transient_for (GTK_WINDOW (result), parent);
  }

  while (button_text)
    {
      response_id = va_arg (varargs, gint);
      gtk_dialog_add_button (GTK_DIALOG (result), button_text, response_id);
      button_text = va_arg (varargs, const gchar *);
    }

  return result;
}
/* ......................... */

gboolean gtk_init_check(int *argc, char ***argv)
{
    static void * (*realFunction)() = NULL;

    gboolean rv=FALSE;

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_init_check");

    rv=realFunction(argc, argv) ? TRUE : FALSE;
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_init_check\n");
#endif
    if(rv)
        kgtkInit(argv && argc ? (*argv)[0] : NULL);
    return rv;
}

void gtk_init(int *argc, char ***argv)
{
    static void * (*realFunction)() = NULL;

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_init");

    realFunction(argc, argv);
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_init\n");
#endif
    kgtkInit(argv && argc ? (*argv)[0] : NULL);
}

/* Store a hash from widget pointer to folder/file list retried from KDialogD */
static GHashTable *fileDialogHash=NULL;

typedef struct
{
    gchar    *folder;
    gchar    *name;
    GSList   *files;
    int      ok,
             cancel;
    gboolean setOverWrite,
             doOverwrite;
} KGtkFileData;

static KGtkFileData * lookupHash(void *hash, gboolean create)
{
    KGtkFileData *rv=NULL;

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::lookupHash %p\n", hash);
#endif
    if(!fileDialogHash)
        fileDialogHash=g_hash_table_new(g_direct_hash, g_direct_equal);

    rv=(KGtkFileData *)g_hash_table_lookup(fileDialogHash, hash);

    if(!rv && create)
    {
        rv=(KGtkFileData *)malloc(sizeof(KGtkFileData));
        rv->folder=NULL;
        rv->files=NULL;
        rv->name=NULL;
        rv->ok=GTK_RESPONSE_OK;
        rv->cancel=GTK_RESPONSE_CANCEL;
        rv->setOverWrite=FALSE;
        rv->doOverwrite=FALSE;
        g_hash_table_insert(fileDialogHash, hash, rv);
        rv=g_hash_table_lookup(fileDialogHash, hash);
    }

    return rv;
}

static void freeHash(void *hash)
{
    KGtkFileData *data=NULL;

    if(!fileDialogHash)
        fileDialogHash=g_hash_table_new(g_direct_hash, g_direct_equal);

    data=(KGtkFileData *)g_hash_table_lookup(fileDialogHash, hash);

    if(data)
    {
        if(data->folder)
            g_free(data->folder);
        if(data->name)
            g_free(data->name);
        if(data->files)
        {
            g_slist_foreach(data->files, (GFunc)g_free, NULL);
            g_slist_free(data->files);
        }
        data->files=NULL;
        data->folder=NULL;
        data->name=NULL;
        g_hash_table_remove(fileDialogHash, hash);
    }
}

/* Some Gtk apps have filter pattern *.[Pp][Nn][Gg] - wherease Qt/KDE prefer *.png */
#define MAX_PATTERN_LEN 64
static gchar *modifyFilter(const gchar *filter)
{
    int         i=0;
    gboolean    brackets=FALSE;
    const char  *p;
    static char res[MAX_PATTERN_LEN+1];

    res[0]='\0';
    for(p=filter; p && *p && i<MAX_PATTERN_LEN; ++p)
        switch(*p)
        {
            case '[':
                p++;
                if(p)
                    res[i++]=tolower(*p);
                brackets=TRUE;
                break;
            case ']':
                brackets=FALSE;
                break;
            default:
                if(!brackets)
                    res[i++]=tolower(*p);
        }
    res[i++]='\0';

    return res;
}

static GtkWidget * getCombo(GtkWidget *widget, GSList *ignore)
{
    if(widget && GTK_IS_CONTAINER(widget))
    {
        GList     *children = gtk_container_get_children(GTK_CONTAINER(widget)),
                  *child    = children;
        GtkWidget *w        = 0;

        for(; child && !w; child=child->next)
        {
            GtkWidget *boxChild=(GtkWidget *)child->data;

            if(ignore && g_slist_find(ignore, boxChild))
                continue;
#if GTK_CHECK_VERSION(3, 0, 0)
            if(GTK_IS_COMBO_BOX_TEXT(boxChild))
#else
            if(GTK_IS_COMBO_BOX(boxChild))
#endif
                w=boxChild;
            else if(GTK_IS_CONTAINER(boxChild))
            {
                GtkWidget *box=getCombo(boxChild, ignore);

                if(box)
                    w=box;
            }
        }
        if(children)
            g_list_free(children);
        if(w)
            return w;
    }

    return NULL;
}

static void getToggleButtons(GtkWidget *widget, GSList **widgets)
{
    if(widget && GTK_IS_CONTAINER(widget))
    {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget)),
              *child    = children;

        for(; child; child=child->next)
        {
            GtkWidget *boxChild=(GtkWidget *)child->data;

            if(GTK_IS_CHECK_BUTTON(boxChild) && gtk_widget_get_visible(boxChild))
                *widgets=g_slist_append(*widgets, boxChild);
            else if(GTK_IS_CONTAINER(boxChild))
                getToggleButtons(boxChild, widgets);
        }
        if(children)
            g_list_free(children);
    }
}

#if GTK_CHECK_VERSION(3, 0, 0) || (defined KGTK_SAFE_FILTER_LOOKUP)
static GHashTable *filterHash=NULL;
static int filterHashCount=0;

typedef struct
{
    GSList *mime_types;
    GSList *patterns;
    GSList *pixbuf_formats;
} KGtkFilterData;

static KGtkFilterData * lookupFilterHash(void *hash, gboolean create)
{
    KGtkFilterData *rv=NULL;

    if(!filterHash)
        filterHash=g_hash_table_new(g_direct_hash, g_direct_equal);

    rv=(KGtkFilterData *)g_hash_table_lookup(filterHash, hash);

    if(!rv && create)
    {
        rv=(KGtkFilterData *)malloc(sizeof(KGtkFilterData));
        rv->mime_types=0;
        rv->patterns=0;
        rv->pixbuf_formats=0;
        g_hash_table_insert(filterHash, hash, rv);
        rv=g_hash_table_lookup(filterHash, hash);
        filterHashCount++;
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x08) printf("KGTK::lookupFilterHash %p created new\n", hash);
#endif
    }
    return rv;
}

static void freeFilterHash(void *hash)
{
    KGtkFilterData *data=NULL;

    if(!filterHash)
        filterHash=g_hash_table_new(g_direct_hash, g_direct_equal);

    data=(KGtkFilterData *)g_hash_table_lookup(filterHash, hash);

    if(data)
    {
        if(data->mime_types)
        {
            g_slist_foreach(data->mime_types, (GFunc)g_free, NULL);
            g_slist_free(data->mime_types);
        }
        if(data->patterns)
        {
            g_slist_foreach(data->patterns, (GFunc)g_free, NULL);
            g_slist_free(data->patterns);
        }
        if(data->patterns)
            g_slist_free(data->pixbuf_formats);
        g_hash_table_remove(filterHash, hash);
        filterHashCount--;
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x08) printf("KGTK::freeFilterHash %p free'd\n", hash);
#endif
    }
}

void g_object_unref(gpointer object)
{
    static void * (*realFunction)() = NULL;

/*
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::g_object_unref %x\n", (void *)ptr);
#endif
*/
    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "g_object_unref");

    if(realFunction)
    {
        if(filterHashCount && G_IS_OBJECT(object) && 1==((GObject *)object)->ref_count)
            freeFilterHash(object);
        realFunction(object);
    }
}

void gtk_file_filter_add_mime_type(GtkFileFilter *filter, const gchar *mime_type)
{
    static void * (*realFunction)() = NULL;

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x08) printf("KGTK::gtk_file_filter_add_mime_type %p %s\n", filter, mime_type);
#endif
    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_file_filter_add_mime_type");

    if(realFunction)
    {
        KGtkFilterData *kgtkFilter=lookupFilterHash(filter, TRUE);
        realFunction(filter, mime_type);
        kgtkFilter->mime_types=g_slist_prepend(kgtkFilter->mime_types, g_strdup(mime_type));
    }
}

void gtk_file_filter_add_pattern(GtkFileFilter *filter, const gchar *pattern)
{
    static void * (*realFunction)() = NULL;

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x08) printf("KGTK::gtk_file_filter_add_pattern %p %s\n", filter, pattern);
#endif
    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_file_filter_add_pattern");

    if(realFunction)
    {
        KGtkFilterData *kgtkFilter=lookupFilterHash(filter, TRUE);
        realFunction(filter, pattern);
        kgtkFilter->patterns=g_slist_prepend(kgtkFilter->patterns, g_strdup(pattern));
    }
}

void gtk_file_filter_add_pixbuf_formats(GtkFileFilter *filter)
{
    static void * (*realFunction)() = NULL;

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x08) printf("KGTK::gtk_file_filter_add_pixbuf_formats %p\n", filter);
#endif
    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_file_filter_add_pixbuf_formats");

    if(realFunction)
    {
        KGtkFilterData *kgtkFilter=lookupFilterHash(filter, TRUE);
        realFunction(filter);
        if(kgtkFilter->pixbuf_formats)
            g_slist_free(kgtkFilter->pixbuf_formats);
        kgtkFilter->pixbuf_formats=gdk_pixbuf_get_formats();
    }
}

void gtk_file_filter_add_custom(GtkFileFilter *filter, GtkFileFilterFlags needed, GtkFileFilterFunc func, gpointer data, GDestroyNotify notify)
{
    static void * (*realFunction)() = NULL;

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x08) printf("KGTK::gtk_file_filter_add_custom %p %X %p\n", filter, (int)needed, (void *)data);
#endif
    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_file_filter_add_custom");

    if(realFunction)
    {
        realFunction(filter, needed, func, data, notify);
        if(APP_LIBREOFFICE==kgtkApp && 2==needed && data)
        {
            gchar *ext=(gchar *)data;
            gboolean ok=FALSE;
            int i;
            
            /* Try to check that data is actually a string! */
            for(i=0; i<16 && !ok; ++i)
                if('\0'==ext[i])
                    ok=TRUE;

            if(ok)
            {
                KGtkFilterData *kgtkFilter=lookupFilterHash(filter, TRUE);
                GString *pat=g_string_new('*'==ext[0] ? "" : "*.");
                pat=g_string_append(pat, ext);
#ifdef KGTK_DEBUG
                if(kgtkDebug&0x08) printf("KGTK::gtk_file_filter_add_custom %s\n", pat->str);
#endif
                kgtkFilter->patterns=g_slist_prepend(kgtkFilter->patterns, pat->str);
            }
        }
    }
}

static GString * getFilters(GtkDialog *dialog)
{
    GString *filter=NULL;
    GSList  *list=gtk_file_chooser_list_filters(GTK_FILE_CHOOSER(dialog));

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x04) printf("KGTK::Get list of filters...\n");
#endif

    if(list)
    {
        GSList *item;
        int    filterNum=0;
        gchar  *lastName=0;
        int    lastPos=0;

        for(item=list; item; item=g_slist_next(item), ++filterNum)
        {
            KGtkFilterData *f=lookupFilterHash(item->data, FALSE);

            if(f)
            {
                const gchar *name=gtk_file_filter_get_name((GtkFileFilter *)(item->data));
                GSList      *mime_type=f->mime_types;
                GSList      *pat=f->patterns;
                GString     *pattern=g_string_new("");
                
                for(; mime_type; mime_type=g_slist_next(mime_type))
                {
                    if(!filter)
                        filter=g_string_new("");
                    else if(filter->len)
                        filter=g_string_append(filter, "\n");
                    #ifdef KGTK_DEBUG
                    if(kgtkDebug&0x04) printf("KGTK:Add mime filter %s\n", (const gchar *)mime_type->data);
                    #endif
                    filter=g_string_append(filter, (const gchar *)mime_type->data);
                }
                for(; pat; pat=g_slist_next(pat), ++filterNum)
                {
                    const char *modPat=modifyFilter((const gchar *)pat->data);
                    /*
                        Firefox has:
                            *.htm *.html | Web page complete
                            *.htm *.html | HTML only
                            *.txt *.text | Text

                        We modify this to have:
                            *.htm        | Web page complete
                            *.html       | HTML only
                            *.txt *.text | Text
                    */

                    if(APP_FIREFOX!=kgtkApp || (strcmp(modPat, "*.html") ? filterNum!=1 : filterNum))
                    {
                        if(pattern->len)
                            pattern=g_string_append(pattern, " ");
                        pattern=g_string_append(pattern, modPat);
                    }
                }

                if(name && pattern && pattern->len)
                {
                    gchar *n=g_strdup(name),
                          *pat=strstr(n, " (*");

                    if(pat)
                        *pat='\0';

                    if(filter && lastName && 0==strcmp(n, lastName))
                    {
                        #ifdef KGTK_DEBUG
                        if(kgtkDebug&0x04) printf("KGTK::Duplicate name %s -> %s  [%d]\n", n, pattern->str, lastPos);
                        #endif
                        filter=g_string_insert(filter, lastPos, pattern->str);
                        filter=g_string_insert(filter, lastPos+pattern->len, " ");
                        #ifdef KGTK_DEBUG
                        if(kgtkDebug&0x04) printf("KGTK::%s -> %s\n", n, pattern->str);
                        #endif
                    }
                    else
                    {
                        if(!filter)
                            filter=g_string_new("");
                        else if(filter->len)
                            filter=g_string_append(filter, "\n");
                        lastPos=filter->len;
                        filter=g_string_append(filter, pattern->str);
                        filter=g_string_append(filter, "|");
                        filter=g_string_append(filter, n);
                        #ifdef KGTK_DEBUG
                        if(kgtkDebug&0x04) printf("KGTK::New name %s -> %s  [%d] (last:%s)\n", n, pattern->str, lastPos, lastName ? lastName : "<>");
                        #endif
                        if(lastName)
                            g_free(lastName), lastName=0;
                        lastName=n;
                    }
                }
                g_string_free(pattern, TRUE);
            }
        }
        if(lastName)
            g_free(lastName);
        g_slist_free(list);
    }

    if(!filter)
    {
        /* This is mainly the case for Inkscape save - but try for other apps too... */
        GtkWidget *combo=0;
        GSList *ignore=0;
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x04) printf("KGTK::No filters found, try to look for an extra combo widget...\n");
#endif

        while(!filter && (combo=getCombo(gtk_file_chooser_get_extra_widget(GTK_FILE_CHOOSER(dialog)), ignore)))
        {
            int i;

            for(i=0; i<128; ++i)
            {
                gchar *text=NULL;

                gtk_combo_box_set_active(GTK_COMBO_BOX(combo), i);
                if(i!=gtk_combo_box_get_active(GTK_COMBO_BOX(combo)))
                    break;

#if GTK_CHECK_VERSION(3, 0, 0)
                text=gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
#else
                text=gtk_combo_box_get_active_text(GTK_COMBO_BOX(combo));
#endif /* GTK_CHECK_VERSION(3, 0, 0) */

                if(text)
                {
                    gchar *pat=strstr(text, " (*");

                    printf("KGTK::text:%s\n", text);
                    if(pat)
                    {
                        gchar *close=strstr(pat, ")");

                        *pat='\0';

                        if(close)
                        {
                            *close='\0';

                            pat+=2; /* Skip past " (" */

                            if(!filter)
                                filter=g_string_new("");
                            else if(filter->len)
                                filter=g_string_append(filter, "\n");
                            filter=g_string_append(filter, pat);
                            filter=g_string_append(filter, "|");
                            filter=g_string_append(filter, text);
                        }
                    }
                    g_free(text);
                }
            }
            
            if(!filter)
                ignore=g_slist_append(ignore, combo);
        }
        
        if(ignore)
            g_slist_free(ignore);
    }
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x04) printf("KGTK::filters:%s\n", filter && filter->len ? filter->str : "<>");
#endif
    return filter;
}

static void setFilter(gchar *filter, GtkDialog *dialog)
{
    GtkFileFilter *found=0;
    GSList        *list=gtk_file_chooser_list_filters(GTK_FILE_CHOOSER(dialog));
    char          *sep=filter ? strchr(filter, '|') : NULL;
    char          *name=NULL;
    unsigned int  flen=0;

    if(sep)
    {
        name=sep+1;
        *sep='\0';
    }
    
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x04) printf("KGTK::Need to locate filter: extension:%s name:%s\n", filter ? filter : "NULL", name ? name : "NULL");
#endif

    flen=strlen(filter);
    if(list)
    {
        int try=name ? 0 : 1;
        
        for(; try<2 && !found; ++try)
        {
            GSList *item;
            int    filterNum=0;

            for(item=list; item && !found; item=g_slist_next(item), filterNum++)
            {
                const gchar *fname=0==try ? gtk_file_filter_get_name((GtkFileFilter *)(item->data)) : 0;
            
                if(0!=try || 0==strcmp(fname, name))
                {
                    KGtkFilterData *f=lookupFilterHash(item->data, FALSE);

                    if(f)
                    {
                        GSList *pat=f->patterns;

                        for(; pat; pat=g_slist_next(pat), ++filterNum)
                        {
                            const char *filt=modifyFilter((const gchar *)pat->data);
                            char       *start=NULL;
                            /*
                                Firefox has:
                                    *.htm *.html | Web page complete
                                    *.htm *.html | HTML only
                                    *.txt *.text | Text

                                We modify this to have:
                                    *.htm        | Web page complete
                                    *.html       | HTML only
                                    *.txt *.text | Text
                            */

                            if((APP_FIREFOX!=kgtkApp || (strcmp(filt, "*.html") ? filterNum!=1 : filterNum)) && (start=strstr(filter, filt)))
                            {
                                unsigned int slen=strlen(filt);

                                if(((start-filter)+slen)<=flen &&
                                    (' '==start[slen] || '\t'==start[slen] ||
                                    '\n'==start[slen] || '\0'==start[slen]))
                                {
#ifdef KGTK_DEBUG
                                    if(kgtkDebug&0x04) printf("KGTK::FOUND FILTER (%d)\n", try);
#endif
                                    found=(GtkFileFilter *)(item->data);
                                }
                            }
                        }
                    }
                }
            }
        }
        g_slist_free(list);
    }

    if(found)
        gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(dialog), found);
    else
    {
        /* This is mainly the case for Inkscape save - but try for other apps too... */
        GtkWidget *combo=0;
        GSList    *ignore=0;

#ifdef KGTK_DEBUG
        if(kgtkDebug&0x04) printf("KGTK::No filters found, try to look for an extra combo widget...\n");
#endif
        while((combo=getCombo(gtk_file_chooser_get_extra_widget(GTK_FILE_CHOOSER(dialog)), ignore)))
        {
            int i;

            for(i=0; i<128; ++i)
            {
                gchar *text=NULL;

                gtk_combo_box_set_active(GTK_COMBO_BOX(combo), i);
                if(i!=gtk_combo_box_get_active(GTK_COMBO_BOX(combo)))
                    break;

#if GTK_CHECK_VERSION(3, 0, 0)
                text=gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
#else
                text=gtk_combo_box_get_active_text(GTK_COMBO_BOX(combo));
#endif
                if(text)
                {
                    gchar *pat=strstr(text, filter);

                    if(pat)
                    {
                        if(pat>text && (' '==pat[-1] || '('==pat[-1]) &&
                           (' '==pat[flen] || ')'==pat[flen]))
                        {
                            if(ignore)
                                g_slist_free(ignore);
                            return; /* found a match, so just return - filter is set */
                        }
                    }
                    g_free(text);
                }
            }

#if 0
            /* No match :-( set to last filter... */
            for(i=0; i<128; ++i)
            {
                gtk_combo_box_set_active(GTK_COMBO_BOX(combo), i);
                if(i!=gtk_combo_box_get_active(GTK_COMBO_BOX(combo)))
                    break;
            }
#endif
            ignore=g_slist_append(ignore, combo);
        }
        
        if(ignore)
            g_slist_free(ignore);
    }
}

static GString * getCustomWidgets(GtkDialog *dialog)
{
    GString *custom=NULL;
    GtkWidget *extra=gtk_file_chooser_get_extra_widget(GTK_FILE_CHOOSER(dialog));

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x10) printf("KGTK::Get custom widgets...\n");
#endif

    if(extra)
    {
        GSList *list=0,
               *child=0;
        getToggleButtons(extra, &list);


        for(child=list; child; child=child->next)
        {
            GtkWidget *widget=(GtkWidget *)child->data;

            if(GTK_IS_CHECK_BUTTON(widget))
            {
                if(!custom)
                    custom=g_string_new("@@");
                else if(custom->len)
                    custom=g_string_append(custom, "@@");
                custom=g_string_append(custom, gtk_button_get_label(GTK_BUTTON(widget)));
                custom=g_string_append(custom, "||");
                custom=g_string_append(custom, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) ? "true" : "false");
            }
        }
        
        if(list)
            g_slist_free(list);
    }

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x10) printf("KGTK::custom:%s\n", custom && custom->len ? custom->str : "<>");
#endif
    return custom;
}

static void setCustomWidgets(gchar *custom, GtkDialog *dialog)
{
    GtkWidget *extra=custom ? gtk_file_chooser_get_extra_widget(GTK_FILE_CHOOSER(dialog)) : 0;

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x10) printf("KGTK::Set custom widgets:%s\n", custom ? custom : "<>");
#endif
    
    if(extra)
    {
        GSList *list=0;
        getToggleButtons(extra, &list);

        if(list)
        {
            gchar **customWidgets=g_strsplit(custom, "@@", 512);
            
            if(customWidgets)
            {
                int   i;
                for(i=0; customWidgets[i]; ++i)
                {
                    gchar **parts=g_strsplit(customWidgets[i], "||", 2);
#ifdef KGTK_DEBUG
                    if(kgtkDebug&0x10) printf("KGTK::Custom: \"%s\"\n", customWidgets[i] ? customWidgets[i] : "<>");
#endif 
                    if(parts && parts[0] && parts[1])
                    {
                        GSList *child=0;
#ifdef KGTK_DEBUG
                        if(kgtkDebug&0x10) printf("KGTK::Set custom widget: \"%s\" -> \"%s\"\n", parts[0] ? parts[0] : "<>", parts[1] ? parts[1] : "<>");
#endif 
                        for(child=list; child; child=child->next)
                        {
                            GtkWidget *widget=(GtkWidget *)child->data;

                            if(GTK_IS_CHECK_BUTTON(widget) && !strcmp(gtk_button_get_label(GTK_BUTTON(widget)), parts[0]))
                            {
#ifdef KGTK_DEBUG
                                if(kgtkDebug&0x10) printf("KGTK::Setting checkbox: %s -> %s\n", parts[0], parts[1]);
#endif 
                                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), !strcmp(parts[1], "true"));
                            }
                        }
                        g_strfreev(parts);
                    }
                }
                g_strfreev(customWidgets);
            }
            g_slist_free(list);
        }
    }
}

#else

static GString * getCustomWidgets(GtkDialog *dialog)
{
    return NULL;
}

static void setCustomWidgets(gchar *filter, GtkDialog *dialog)
{
}

struct _GtkFileFilter
{
  GtkObject parent_instance;

  gchar *name;
  GSList *rules;

  GtkFileFilterFlags needed;
};

typedef enum {
  FILTER_RULE_PATTERN,
  FILTER_RULE_MIME_TYPE,
  FILTER_RULE_PIXBUF_FORMATS,
  FILTER_RULE_CUSTOM
} FilterRuleType;

struct _FilterRule
{
  FilterRuleType type;
  GtkFileFilterFlags needed;

  union {
    gchar *pattern;
    gchar *mime_type;
    GSList *pixbuf_formats;
    struct {
      GtkFileFilterFunc func;
      gpointer data;
      GDestroyNotify notify;
    } custom;
  } u;
};

static GString * getFilters(GtkDialog *dialog)
{
    GString *filter=NULL;
    GSList  *list=gtk_file_chooser_list_filters(GTK_FILE_CHOOSER(dialog));

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::Get list of filters...\n");
#endif

    if(list)
    {
        GSList *item;
        int    filterNum=0;

        for(item=list; item; item=g_slist_next(item), ++filterNum)
        {
            GtkFileFilter *f=(GtkFileFilter *)(item->data);

            if(f)
            {
                const gchar *name=gtk_file_filter_get_name(f);
                GSList      *rule=((struct _GtkFileFilter *)f)->rules;
                GString     *pattern=g_string_new("");

                for(; rule; rule=g_slist_next(rule))
                    switch(((struct _FilterRule *)rule->data)->type)
                    {
                        case FILTER_RULE_PATTERN:
                        {
                            const char *modPat=
                                         modifyFilter(((struct _FilterRule *)rule->data)->u.pattern);

                            /*
                               Firefox has:
                                  *.htm *.html | Web page complete
                                  *.htm *.html | HTML only
                                  *.txt *.text | Text

                               We modify this to have:
                                  *.htm        | Web page complete
                                  *.html       | HTML only
                                  *.txt *.text | Text
                            */

                            if(APP_FIREFOX!=kgtkApp || (strcmp(modPat, "*.html") ? filterNum!=1
                                                                                 : filterNum))
                            {
                                if(pattern->len)
                                    pattern=g_string_append(pattern, " ");
                                pattern=g_string_append(pattern, modPat);
                            }
                            break;
                        }
                        case FILTER_RULE_MIME_TYPE:
                            if(!filter)
                                filter=g_string_new("");
                            else if(filter->len)
                                filter=g_string_append(filter, "\n");
                            filter=g_string_append(filter,
                                                   ((struct _FilterRule *)rule->data)->u.mime_type);
                            break;
                        default:
                            break;
                    }

                if(name && pattern && pattern->len)
                {
                    gchar *n=g_strdup(name),
                          *pat=strstr(n, " (*");

                    if(pat)
                        *pat='\0';

                    if(!filter)
                        filter=g_string_new("");
                    else if(filter->len)
                        filter=g_string_append(filter, "\n");
                    filter=g_string_append(filter, pattern->str);
                    filter=g_string_append(filter, "|");
                    filter=g_string_append(filter, n);
                    g_free(n);
                }
                g_string_free(pattern, TRUE);
            }
        }
        g_slist_free(list);
    }

    if(!filter)
    {
        /* This is mainly the case for Inkscape save - but try for other apps too... */
        GtkWidget *combo=getCombo(gtk_file_chooser_get_extra_widget(GTK_FILE_CHOOSER(dialog)), NULL);

#ifdef KGTK_DEBUG
        if(kgtkDebug&0x02) printf("KGTK::No filters found, try to look for an extra combo widget...\n");
#endif

        if(combo)
        {
            int i;

            for(i=0; i<64; ++i)
            {
                gchar *text=NULL;

                gtk_combo_box_set_active(GTK_COMBO_BOX(combo), i);
                if(i!=gtk_combo_box_get_active(GTK_COMBO_BOX(combo)))
                    break;

                text=gtk_combo_box_get_active_text(GTK_COMBO_BOX(combo));

                if(text)
                {
                    gchar *pat=strstr(text, " (*");

                    if(pat)
                    {
                        gchar *close=strstr(pat, ")");

                        *pat='\0';

                        if(close)
                            *close='\0';

                        pat+=2; /* Skip past " (" */
                        if(!filter)
                            filter=g_string_new("");
                        else if(filter->len)
                            filter=g_string_append(filter, "\n");
                        filter=g_string_append(filter, pat);
                        filter=g_string_append(filter, "|");
                        filter=g_string_append(filter, text);
                    }
                    g_free(text);
                }
            }
        }
    }

    return filter;
}

static void setFilter(const gchar *filter, GtkDialog *dialog)
{
    gboolean found=FALSE;
    GSList   *list=gtk_file_chooser_list_filters(GTK_FILE_CHOOSER(dialog));

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::Need to locate filter:%s\n", filter ? filter : "Null");
#endif

    if(list)
    {
        GSList       *item;
        unsigned int flen=strlen(filter);
        int          filterNum=0;

        for(item=list; item && !found; item=g_slist_next(item), filterNum++)
        {
            GtkFileFilter *f=(GtkFileFilter *)(item->data);

            if(f)
            {
                GSList *rule=((struct _GtkFileFilter *)f)->rules;
                char   *start=NULL;

                for(; rule && !found; rule=g_slist_next(rule))
                    if(FILTER_RULE_PATTERN==((struct _FilterRule *)rule->data)->type)
                    {
                        char *filt=modifyFilter(((struct _FilterRule *)rule->data)->u.pattern);

                        /*
                           Firefox has:
                              *.htm *.html | Web page complete
                              *.htm *.html | HTML only
                              *.txt *.text | Text

                           We modify this to have:
                              *.htm        | Web page complete
                              *.html       | HTML only
                              *.txt *.text | Text
                        */

                        if((APP_FIREFOX!=kgtkApp || (strcmp(filt, "*.html") ? filterNum!=1
                                                                            : filterNum)) &&
                           (start=strstr(filter, filt)))
                        {
                            unsigned int slen=strlen(filt);

                            if(((start-filter)+slen)<=flen &&
                                (' '==start[slen] || '\t'==start[slen] ||
                                '\n'==start[slen] || '\0'==start[slen]))
                            {
#ifdef KGTK_DEBUG
                                if(kgtkDebug&0x02) printf("KGTK::FOUND FILTER\n");
#endif
                                found=TRUE;
                                gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(dialog), f);
                            }
                        }
                    }
            }
        }
        g_slist_free(list);
    }

    if(!found)
    {
        /* This is mainly the case for Inkscape save - but try for other apps too... */
        GtkWidget *combo=getCombo(gtk_file_chooser_get_extra_widget(GTK_FILE_CHOOSER(dialog)), NULL);

#ifdef KGTK_DEBUG
        if(kgtkDebug&0x02) printf("KGTK::No filters found, try to look for an extra combo widget...\n");
#endif
        if(combo)
        {
            int i,
                flen=strlen(filter);

            for(i=0; i<64; ++i)
            {
                gchar *text=NULL;

                gtk_combo_box_set_active(GTK_COMBO_BOX(combo), i);
                if(i!=gtk_combo_box_get_active(GTK_COMBO_BOX(combo)))
                    break;

                text=gtk_combo_box_get_active_text(GTK_COMBO_BOX(combo));

                if(text)
                {
                    gchar *pat=strstr(text, filter);

                    if(pat)
                    {
                        if(pat>text && (' '==pat[-1] || '('==pat[-1]) &&
                           (' '==pat[flen] || ')'==pat[flen]))
                            return; /* found a match, so just return - filter is set */
                    }
                    g_free(text);
                }
            }

            /* No match :-( set to last filter... */
            for(i=0; i<64; ++i)
            {
                gtk_combo_box_set_active(GTK_COMBO_BOX(combo), i);
                if(i!=gtk_combo_box_get_active(GTK_COMBO_BOX(combo)))
                    break;
            }
        }
    }
}

#endif /* GTK_CHECK_VERSION(3, 0, 0) */

static GSList * addProtocols(GSList *files)
{
    GSList *item=files;

    for(; item; item=g_slist_next(item))
    {
        gchar *cur=item->data;
        item->data=g_filename_to_uri(item->data, NULL, NULL);
        g_free(cur);
    }

    return files;
}

void gtk_window_present(GtkWindow *window)
{
    static void * (*realFunction)() = NULL;

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_window_present");

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_window_present %s %d\n", g_type_name(G_OBJECT_TYPE(window)),
           GTK_IS_FILE_CHOOSER(window));
#endif
    if(GTK_IS_FILE_CHOOSER(window)) /* || (APP_GIMP==kgtkApp &&
       0==strcmp(g_type_name(G_OBJECT_TYPE(window)), "GimpFileDialog")))*/
        gtk_dialog_run(GTK_DIALOG(window));
    else
        realFunction(window);
}

void gtk_widget_show(GtkWidget *widget)
{
    static void * (*realFunction)() = NULL;

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_widget_show");

    if(widget && !GTK_IS_FILE_CHOOSER_BUTTON(widget) && GTK_IS_FILE_CHOOSER(widget))
    {
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x02) printf("KGTK::gtk_widget_show %s %d\n", g_type_name(G_OBJECT_TYPE(widget)),
               GTK_IS_FILE_CHOOSER(widget));
#endif
        gtk_dialog_run(GTK_DIALOG(widget));
        /* GTK_OBJECT_FLAGS(widget)|=GTK_REALIZED; */
        gtk_widget_set_realized(widget, TRUE);
    }
    else
        realFunction(widget);
}

void gtk_widget_hide(GtkWidget *widget)
{
    static void * (*realFunction)() = NULL;

    FUNC_ENTER

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_widget_hide");

    if(widget && !GTK_IS_FILE_CHOOSER_BUTTON(widget) && GTK_IS_FILE_CHOOSER(widget))
    {
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x02) printf("KGTK::gtk_widget_hide %s %d\n", g_type_name(G_OBJECT_TYPE(widget)),
               GTK_IS_FILE_CHOOSER(widget));
#endif
        if(gtk_widget_get_realized(widget))
            gtk_widget_set_realized(widget, FALSE);
        /*if(GTK_OBJECT_FLAGS(widget)&GTK_REALIZED)
            GTK_OBJECT_FLAGS(widget)-=GTK_REALIZED;*/
    }
    else
        realFunction(widget);

    FUNC_EXIT
}

gboolean gtk_file_chooser_get_do_overwrite_confirmation(GtkFileChooser *widget)
{
    static gboolean (*realFunction)(GtkFileChooser *chooser) = NULL;

    gboolean rv=FALSE;

    if(!realFunction)
        realFunction = real_dlsym(RTLD_NEXT, "gtk_file_chooser_get_do_overwrite_confirmation");

    if(realFunction)
    {
        KGtkFileData *data=lookupHash(widget, FALSE);

        if(data)
        {
            if(!data->setOverWrite)
            {
                data->setOverWrite=TRUE;
                data->doOverwrite=(gboolean) realFunction(widget);
            }
            rv=data->doOverwrite;
        }
        else
            rv=(gboolean) realFunction(widget);
    }

    return rv;
}

/* ext => called from app, not kgtk */
void kgtkFileChooserSetDoOverwriteConfirmation(GtkFileChooser *widget, gboolean v, gboolean ext)
{
    static void * (*realFunction)() = NULL;

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_file_chooser_set_do_overwrite_confirmation");

    if(realFunction)
    {
        realFunction(widget, v);
        if(ext)
        {
            KGtkFileData *data=lookupHash(widget, FALSE);

            if(data)
            {
                data->setOverWrite=TRUE;
                data->doOverwrite=v;
            }
        }
    }
}

gboolean isOnFileChooser(GtkWidget *w)
{
    return  w
                ? GTK_IS_FILE_CHOOSER(w)
                    ? TRUE
                    : isOnFileChooser(gtk_widget_get_parent(w))
                : FALSE;
}

int gtk_combo_box_get_active(GtkComboBox *combo)
{
    int rv=0;

    if(APP_KINO==kgtkApp && isOnFileChooser(GTK_WIDGET(combo)))
        return 1;
    else
    {
        static int (*realFunction)(GtkComboBox *combo_box) = NULL;

        if(!realFunction)
            realFunction = real_dlsym(RTLD_NEXT, "gtk_combo_box_get_active");

        rv=realFunction(combo);
    }

    return rv;
}

gint gtk_dialog_run(GtkDialog *dialog)
{
    static gint (*realFunction)(GtkDialog *dialog) = NULL;

    if(!realFunction)
        realFunction = real_dlsym(RTLD_NEXT, "gtk_dialog_run");

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_dialog_run %s \n", dialog ? g_type_name(G_OBJECT_TYPE(dialog)) : "<null>");
#endif

    if(kgtkInit(NULL) && GTK_IS_FILE_CHOOSER(dialog))
    {
        static gboolean running=FALSE;

        KGtkFileData *data=lookupHash(dialog, TRUE);

#ifdef KGTK_DEBUG
        if(kgtkDebug&0x02) printf("KGTK::run file chooser, already running? %d\n", running);
#endif
        if(!running)
        {
            GtkFileChooserAction act=gtk_file_chooser_get_action(GTK_FILE_CHOOSER(dialog));
            gchar                *current=NULL,
                                 *selFilter=NULL,
                                 *customRv=NULL;
            const gchar          *title=gtk_window_get_title(GTK_WINDOW(dialog));
            GString              *filter=NULL;
            GString              *custom=NULL;
            gint                 resp=data->cancel;
            gboolean             origOverwrite=
                                     gtk_file_chooser_get_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog));

            running=TRUE;

            if(GTK_FILE_CHOOSER_ACTION_OPEN==act || GTK_FILE_CHOOSER_ACTION_SAVE==act)
                filter=getFilters(dialog), custom=getCustomWidgets(dialog);
            else /* GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER==act ||
                 GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER==act */
                if(NULL==(current=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog))))
                    current=gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(dialog));

            kgtkFileChooserSetDoOverwriteConfirmation(GTK_FILE_CHOOSER(dialog), FALSE, FALSE);

            switch(act)
            {
                case GTK_FILE_CHOOSER_ACTION_OPEN:
                {
#ifdef KGTK_DEBUG
                    if(kgtkDebug&0x02) printf("KGTK::run file chooser GTK_FILE_CHOOSER_ACTION_OPEN\n");
#endif
                    if(gtk_file_chooser_get_select_multiple(GTK_FILE_CHOOSER(dialog)))
                    {
                        GSList *files=NULL;

                        openKdeDialog(GTK_WIDGET(dialog), title ? title : "",
                                      data->folder ? data->folder : "",
                                      filter && filter->len
                                          ? filter->str
                                          : kgtkFileFilter
                                               ? kgtkFileFilter
                                               : "", 
                                      custom && custom->len ? custom->str : "",
                                      OP_FILE_OPEN_MULTIPLE, &files, &selFilter, &customRv, FALSE);

                        if(files)
                        {
                            GSList *c;

                            gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(dialog));
                            for(c=files; c; c=g_slist_next(c))
                                gtk_file_chooser_select_filename(GTK_FILE_CHOOSER(dialog),
                                                                 (gchar *)(c->data));
                            g_slist_foreach(files, (GFunc)g_free, NULL);
                            g_slist_free(files);

                            resp=data->ok;
                        }
                    }
                    else
                    {
                        gchar  *file=NULL;
                        GSList *res=NULL;

                        openKdeDialog(GTK_WIDGET(dialog), title ? title : "",
                                      data->folder ? data->folder : "",
                                      filter && filter->len
                                          ? filter->str
                                          : kgtkFileFilter
                                               ? kgtkFileFilter
                                               : "", 
                                      custom && custom->len ? custom->str : "",
                                      OP_FILE_OPEN, &res, &selFilter, &customRv, FALSE);
                        file=firstEntry(res);

                        if(file)
                        {
                            gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(dialog));
                            gtk_file_chooser_select_filename(GTK_FILE_CHOOSER(dialog), file);
                            g_free(file);
                            resp=data->ok;
                        }
                    }
                    break;
                }
                case GTK_FILE_CHOOSER_ACTION_SAVE:
                {
                    gchar  *file=NULL;
                    GSList *res=NULL;

#ifdef KGTK_DEBUG
                    if(kgtkDebug&0x02) printf("KGTK::run file chooser GTK_FILE_CHOOSER_ACTION_SAVE\n");
#endif
                    if(data->name)
                    {
                        GString *cur=g_string_new(data->folder ? data->folder
                                                               : get_current_dir_name());

                        cur=g_string_append(cur, "/");
                        cur=g_string_append(cur, data->name);
                        current=g_string_free(cur, FALSE);
                    }

                    openKdeDialog(GTK_WIDGET(dialog), title ? title : "",
                                  current ? current : (data->folder ? data->folder : ""),
                                  filter && filter->len
                                          ? filter->str
                                          : kgtkFileFilter
                                               ? kgtkFileFilter
                                               : "", 
                                  custom && custom->len ? custom->str : "",
                                  OP_FILE_SAVE, &res, &selFilter, &customRv, origOverwrite);
                    file=firstEntry(res);

                    if(file)
                    {
                        /* Firefox crashes when we save to an existing name -> so just delete it first! */
                        if(APP_FIREFOX==kgtkApp && origOverwrite)
                        {
                            struct stat info;

                            if(0==lstat(file, &info))
                                unlink(file);
                        }
                        gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(dialog));
                        gtk_file_chooser_select_filename(GTK_FILE_CHOOSER(dialog), file);
                        g_free(file);
                        resp=data->ok;
                    }
                    break;
                }
                case GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER:
                case GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER:
                {
                    GSList *res=NULL;
                    gchar  *folder=NULL;

#ifdef KGTK_DEBUG
                    if(GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER==act)
                    {
                        if(kgtkDebug&0x02) printf("KGTK::run file chooser GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER\n");
                    }
                    else
                    {
                        if(kgtkDebug&0x02) printf("KGTK::run file chooser GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER\n");
                    }
#endif
                    openKdeDialog(GTK_WIDGET(dialog), title ? title : "",
                                  data->folder ? data->folder : "", NULL, NULL,
                                  OP_FOLDER, &res, NULL, NULL, FALSE);
                    folder=firstEntry(res);

                    if(folder)
                    {
                        gtk_file_chooser_select_filename(GTK_FILE_CHOOSER(dialog), folder);
                        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), folder);
                        g_free(folder);
                        resp=data->ok;
                    }
                }
            }

            if(current)
                g_free(current);

            if(filter)
                g_string_free(filter, TRUE);

            if(custom)
                g_string_free(custom, TRUE);

            if(selFilter)
            {
                setFilter(selFilter, dialog);
                g_free(selFilter);
            }

            if(customRv)
            {
                setCustomWidgets(customRv, dialog);
                g_free(customRv);
            }

#ifdef KGTK_DEBUG
            if(kgtkDebug&0x02) printf("KGTK::RETURN RESP:%d\n", resp);
#endif
            g_signal_emit_by_name(dialog, "response", resp);
            running=FALSE;
            return resp;
        }
#ifdef KGTK_DEBUG
        if(kgtkDebug&0x02) printf("KGTK::ALREADY RUNNING SO RETURN RESP:%d\n", data->cancel);
#endif
        g_signal_emit_by_name(dialog, "response", data->cancel);
        return data->cancel;
    }
    return (gint)realFunction(dialog);
}

void gtk_widget_destroy(GtkWidget *widget)
{
    static void * (*realFunction)() = NULL;

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_widget_destroy");

    if(fileDialogHash && GTK_IS_FILE_CHOOSER(widget))
        freeHash(widget);

    realFunction(widget);
}

gchar * gtk_file_chooser_get_filename(GtkFileChooser *chooser)
{
    KGtkFileData *data=lookupHash(chooser, FALSE);

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_get_filename %d %s\n", data ? g_slist_length(data->files) : 12345,
           data && data->files && data->files->data ? (const char *)data->files->data : "<>");
#endif
    return data && data->files && data->files->data ? g_strdup(data->files->data) : NULL;
}

gboolean gtk_file_chooser_select_filename(GtkFileChooser *chooser, const char *filename)
{
    KGtkFileData *data=lookupHash(chooser, TRUE);
    static void * (*realFunction)() = NULL;

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_file_chooser_select_filename");
    realFunction(chooser, filename);

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_select_filename %s, %d\n", filename,
           data ? g_slist_length(data->files) : 12345);
#endif
    if(data && filename)
    {
        GSList *c=NULL;

        for(c=data->files; c; c=g_slist_next(c))
            if(c->data && 0==strcmp((char *)(c->data), filename))
                break;

        if(!c)
        {
            gchar *folder=g_path_get_dirname(filename);

            data->files=g_slist_prepend(data->files, g_strdup(filename));

            if(folder && (!data->folder || strcmp(folder, data->folder)))
            {
                gtk_file_chooser_set_current_folder(chooser, folder);
                g_free(folder);
            }
        }
    }

    return TRUE;
}

void gtk_file_chooser_unselect_all(GtkFileChooser *chooser)
{
    KGtkFileData *data=lookupHash(chooser, TRUE);
    static void * (*realFunction)() = NULL;

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_file_chooser_unselect_all");
    realFunction(chooser);

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_unselect_all %d\n", data ? g_slist_length(data->files) : 12345);
#endif
    if(data && data->files)
    {
        g_slist_foreach(data->files, (GFunc)g_free, NULL);
        g_slist_free(data->files);
        data->files=NULL;
    }
}

gboolean gtk_file_chooser_set_filename(GtkFileChooser *chooser, const char *filename)
{
    KGtkFileData *data=lookupHash(chooser, TRUE);
    static void * (*realFunction)() = NULL;

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_file_chooser_set_filename");
    realFunction(chooser, filename);

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_set_filename %s %d\n", filename,
           data ? g_slist_length(data->files) : 12345);
#endif
    if(data && filename)
    {
        gchar *folder=g_path_get_dirname(filename),
              *name=g_path_get_basename(filename);

        if(data->files)
        {
            g_slist_foreach(data->files, (GFunc)g_free, NULL);
            g_slist_free(data->files);
            data->files=NULL;
        }

        data->files=g_slist_prepend(data->files, g_strdup(filename));

        if(name && (!data->name || strcmp(name, data->name)))
            gtk_file_chooser_set_current_name(chooser, name);
        if(name)
            g_free(name);
        if(folder && (!data->folder || strcmp(folder, data->folder)))
            gtk_file_chooser_set_current_folder(chooser, folder);
        if(folder)
            g_free(folder);
    }

    return TRUE;
}

void gtk_file_chooser_set_current_name(GtkFileChooser *chooser, const char *filename)
{
    KGtkFileData         *data=lookupHash(chooser, TRUE);
    GtkFileChooserAction act=gtk_file_chooser_get_action(chooser);

    if(GTK_FILE_CHOOSER_ACTION_SAVE==act || GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER==act)
    {
        static void * (*realFunction)() = NULL;

        if(!realFunction)
            realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_file_chooser_set_current_name");
        realFunction(chooser, filename);
    }

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_set_current_name %s %d\n", filename,
           data ? g_slist_length(data->files) : 12345);
#endif
    if(data && filename)
    {
        if(data->name)
            g_free(data->name);
        data->name=g_strdup(filename);
    }
}

GSList * gtk_file_chooser_get_filenames(GtkFileChooser *chooser)
{
    KGtkFileData *data=lookupHash(chooser, FALSE);
    GSList       *rv=NULL;

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_get_filenames %d\n", data ? g_slist_length(data->files) : 12345);
#endif
    if(data && data->files)
    {
        GSList *item=data->files;

        for(; item; item=g_slist_next(item))
        {
#ifdef KGTK_DEBUG
            if(kgtkDebug&0x02) printf("KGTK::FILE:%s\n", (const char *)item->data);
#endif
            if(item->data)
                rv=g_slist_prepend(rv, g_strdup(item->data));
        }
    }
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_get_filenames END\n");
#endif
    return rv;
}

gboolean gtk_file_chooser_set_current_folder(GtkFileChooser *chooser, const gchar *folder)
{
    KGtkFileData *data=lookupHash(chooser, TRUE);
    static void * (*realFunction)() = NULL;

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_file_chooser_set_current_folder");
    realFunction(chooser, folder);

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_set_current_folder %s %d\n", folder,
           data ? g_slist_length(data->files) : 12345);
#endif
    if(data && folder)
    {
        if(data->folder)
            g_free(data->folder);
        data->folder=g_strdup(folder);
    }
        g_signal_emit_by_name(chooser, "current-folder-changed", 0);

    return TRUE;
}

gchar * gtk_file_chooser_get_current_folder(GtkFileChooser *chooser)
{
    KGtkFileData *data=lookupHash(chooser, FALSE);

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_get_current_folder %d\n",
           data ? g_slist_length(data->files) : 12345);
#endif
    if(!data)
    {
        gtk_file_chooser_set_current_folder(chooser, get_current_dir_name());
        data=g_hash_table_lookup(fileDialogHash, chooser);
    }

    return data && data->folder ? g_strdup(data->folder) : NULL;
}

gchar * gtk_file_chooser_get_uri(GtkFileChooser *chooser)
{
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_get_uri\n");
#endif
    gchar *filename=gtk_file_chooser_get_filename(chooser);

    if(filename)
    {
        gchar *uri=g_filename_to_uri(filename, NULL, NULL);

        g_free(filename);
        return uri;
    }

    return NULL;
}

gboolean gtk_file_chooser_set_uri(GtkFileChooser *chooser, const char *uri)
{
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_set_uri\n");
#endif

    gchar    *file=g_filename_from_uri(uri, NULL, NULL);
    gboolean rv=FALSE;

    if(file)
    {
        rv=gtk_file_chooser_set_filename(chooser, file);

        g_free(file);
    }
    return rv;
}

GSList * gtk_file_chooser_get_uris(GtkFileChooser *chooser)
{
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_get_uris\n");
#endif
    return addProtocols(gtk_file_chooser_get_filenames(chooser));
}

gboolean gtk_file_chooser_set_current_folder_uri(GtkFileChooser *chooser, const gchar *uri)
{
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_set_current_folder_uri\n");
#endif
    gchar    *folder=g_filename_from_uri(uri, NULL, NULL);
    gboolean rv=FALSE;

    if(folder)
    {
        rv=gtk_file_chooser_set_current_folder(chooser, folder);

        g_free(folder);
    }
    return rv;
}

gchar * gtk_file_chooser_get_current_folder_uri(GtkFileChooser *chooser)
{
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_get_current_folder_uri\n");
#endif

    gchar *folder=gtk_file_chooser_get_current_folder(chooser);

    if(folder)
    {
        gchar *uri=g_filename_to_uri(folder, NULL, NULL);

        g_free(folder);
        return uri;
    }

    return NULL;
}

void g_signal_stop_emission_by_name(gpointer instance, const gchar *detailed_signal)
{
    static void * (*realFunction)() = NULL;

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "g_signal_stop_emission_by_name");

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::g_signal_stop_emission_by_name %s  %s (check)\n", g_type_name(G_OBJECT_TYPE(instance)), detailed_signal);
#endif

    if(!GTK_IS_FILE_CHOOSER(instance) || strcmp(detailed_signal, "response"))
        realFunction(instance, detailed_signal);
#ifdef KGTK_DEBUG
    else
        if(kgtkDebug&0x02) printf("KGTK::g_signal_stop_emission_by_name %s  %s\n", g_type_name(G_OBJECT_TYPE(instance)), detailed_signal);
#endif
}

GtkWidget * gtk_file_chooser_dialog_new(const gchar *title, GtkWindow *parent,
                             GtkFileChooserAction action, const gchar *first_button_text,
                             ...)
{
    GtkWidget    *dlg=NULL;
    KGtkFileData *data=NULL;
    const char   *text=first_button_text;
    gint         id;
    va_list      varargs;

    va_start(varargs, first_button_text);
    dlg=kgtk_file_chooser_dialog_new_valist(title, parent, action, NULL, first_button_text, varargs);
    va_end(varargs);

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_dialog_new\n");
#endif
    data=lookupHash(dlg, TRUE);
    va_start(varargs, first_button_text);
    while(text)
    {
        id = va_arg(varargs, gint);

        if(text && (0==strcmp(text, GTK_STOCK_CANCEL) || 0==strcmp(text, GTK_STOCK_CLOSE) ||
                    0==strcmp(text, GTK_STOCK_QUIT) || 0==strcmp(text, GTK_STOCK_NO)))
            data->cancel=id;
        else if(text && (0==strcmp(text, GTK_STOCK_OK) || 0==strcmp(text, GTK_STOCK_OPEN) ||
                         0==strcmp(text, GTK_STOCK_SAVE) || 0==strcmp(text, GTK_STOCK_YES)))
            data->ok=id;
      text=va_arg(varargs, const gchar *);
    }
    va_end(varargs);
    return dlg;
}

#ifdef KGTK_SAFE_FILE_CHOOSER_BUTTON_SUPPORT

/* How to *safely* get access to fielchoderbutton's filedialog????? */
static GtkWidget * getChild(GtkWidget *widget, GType type)
{
    GList     *children = gtk_container_get_children(GTK_CONTAINER(widget)),
              *child    = children;
    GtkWidget *w        = 0;

    for(; child && !w; child=child->next)
    {
        GtkWidget *boxChild=(GtkWidget *)child->data;

        if(G_TYPE_CHECK_INSTANCE_TYPE(boxChild, type))
            w=boxChild;
    }
    if(children)
        g_list_free(children);
    return w;
}

static void dumpChildren(GtkWidget *widget, int level)
{
    if(level<5)
    {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget)),
              *child    = children;

        for(; child; child=child->next)
        {
            GtkWidget *boxChild=(GtkWidget *)child->data;

            printf(":[%d]%s:", level, g_type_name(G_OBJECT_TYPE(boxChild)));
            if(GTK_IS_BOX(boxChild))
                dumpChildren(boxChild, ++level);
        }
        if(children)
            g_list_free(children);
    }
}

static void handleGtkFileChooserButtonClicked(GtkButton *button, gpointer user_data)
{
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::handleGtkFileChooserButtonClicked\n");
#endif
    gtk_dialog_run(GTK_DIALOG(GTK_FILE_CHOOSER_BUTTON(user_data)->priv->dialog));
}

static void handleGtkFileChooserComboChanged(GtkComboBox *combo_box, gpointer user_data)
{
    static gboolean handle=TRUE;
    GtkTreeIter iter;

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::handleGtkFileChooserComboChanged (handle:%d)\n", handle);
#endif
    if(!handle)
        return;

    if(gtk_combo_box_get_active_iter (combo_box, &iter))
    {
        GtkFileChooserButtonPrivate *priv=GTK_FILE_CHOOSER_BUTTON(user_data)->priv;
        gchar type=ROW_TYPE_INVALID;

        gtk_tree_model_get(priv->filter_model, &iter, TYPE_COLUMN, &type, -1);

        if(ROW_TYPE_OTHER==type)
            gtk_dialog_run(GTK_DIALOG(GTK_FILE_CHOOSER_BUTTON(user_data)->priv->dialog));
        else
        {
            g_signal_handler_unblock(priv->combo_box, priv->combo_box_changed_id);
            handle=FALSE;
            g_signal_emit_by_name(priv->combo_box, "changed");
            handle=TRUE;
            g_signal_handler_block(priv->combo_box, priv->combo_box_changed_id);
        }
    }
}

GtkWidget * gtk_file_chooser_button_new(const gchar *title, GtkFileChooserAction action)
{
    static void * (*realFunction)() = NULL;

    GtkWidget *button=NULL;

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_file_chooser_button_new");

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_button_new\n");
#endif

    if(kgtkInit(NULL))
    {
        GtkFileChooserButtonPrivate *priv=NULL;
        GtkWidget *privButton=0;
        GtkWidget *privCombo=0;

        button=realFunction(title, action);
        privButton=getChild(button, GTK_TYPE_BUTTON);
        privCombo=getChild(button, GTK_TYPE_COMBO_BOX);

        if(privButton)
        {
            g_signal_handlers_disconnect_matched(privButton,
                                                 G_SIGNAL_MATCH_DATA,0, 0, NULL, NULL, button);

            g_signal_connect(privButton, "clicked",
                             G_CALLBACK(handleGtkFileChooserButtonClicked),
                             GTK_FILE_CHOOSER_BUTTON(button));
        }
        if(privCombo)
        {
            /*g_signal_handler_block(privCombo, priv->combo_box_changed_id); */
            g_signal_handlers_disconnect_matched(privCombo,
                                                 G_SIGNAL_MATCH_DATA,0, 0, NULL, NULL, button);
            
            g_signal_connect(privCombo, "changed",
                             G_CALLBACK(handleGtkFileChooserComboChanged),
                             GTK_FILE_CHOOSER_BUTTON(button));
        }
    }
    return button;
}

#elif GTK_CHECK_VERSION(2, 6, 0)
typedef struct _GtkFileSystem      GtkFileSystem;
typedef struct _GtkFilePath        GtkFilePath;
typedef struct _GtkFileSystemModel GtkFileSystemModel;

struct _GtkFileChooserButtonPrivate
{
  GtkWidget *dialog;
  GtkWidget *button;
  GtkWidget *image;
  GtkWidget *label;
  GtkWidget *combo_box;

  GtkCellRenderer *icon_cell;
  GtkCellRenderer *name_cell;

  GtkTreeModel *model;
  GtkTreeModel *filter_model;

#if !GTK_CHECK_VERSION(3, 0, 0)
  gchar *backend;
#endif
  GtkFileSystem *fs;
  GtkFilePath *old_path;

  gulong combo_box_changed_id;
  /* ...and others...
  gulong dialog_file_activated_id;
  gulong dialog_folder_changed_id;
  gulong dialog_selection_changed_id;
  gulong fs_volumes_changed_id;
  gulong fs_bookmarks_changed_id;
  */
};

/* TreeModel Columns */
enum
{
  ICON_COLUMN,
  DISPLAY_NAME_COLUMN,
  TYPE_COLUMN,
  DATA_COLUMN,
#if GTK_CHECK_VERSION(3, 0, 0)
  IS_FOLDER_COLUMN,
  CANCELLABLE_COLUMN,
#endif
  NUM_COLUMNS
};

/* TreeModel Row Types */
typedef enum
{
  ROW_TYPE_SPECIAL,
  ROW_TYPE_VOLUME,
  ROW_TYPE_SHORTCUT,
  ROW_TYPE_BOOKMARK_SEPARATOR,
  ROW_TYPE_BOOKMARK,
  ROW_TYPE_CURRENT_FOLDER_SEPARATOR,
  ROW_TYPE_CURRENT_FOLDER,
  ROW_TYPE_OTHER_SEPARATOR,
  ROW_TYPE_OTHER,

  ROW_TYPE_INVALID = -1
}
RowType;

static void handleGtkFileChooserButtonClicked(GtkButton *button, gpointer user_data)
{
#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::handleGtkFileChooserButtonClicked\n");
#endif
    gtk_dialog_run(GTK_DIALOG(GTK_FILE_CHOOSER_BUTTON(user_data)->priv->dialog));
}

static void handleGtkFileChooserComboChanged(GtkComboBox *combo_box, gpointer user_data)
{
    static gboolean handle=TRUE;
    GtkTreeIter iter;

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::handleGtkFileChooserComboChanged (handle:%d)\n", handle);
#endif
    if(!handle)
        return;

    if(gtk_combo_box_get_active_iter (combo_box, &iter))
    {
        GtkFileChooserButtonPrivate *priv=GTK_FILE_CHOOSER_BUTTON(user_data)->priv;
        gchar type=ROW_TYPE_INVALID;

        gtk_tree_model_get(priv->filter_model, &iter, TYPE_COLUMN, &type, -1);

        if(ROW_TYPE_OTHER==type)
            gtk_dialog_run(GTK_DIALOG(GTK_FILE_CHOOSER_BUTTON(user_data)->priv->dialog));
        else
        {
            g_signal_handler_unblock(priv->combo_box, priv->combo_box_changed_id);
            handle=FALSE;
            g_signal_emit_by_name(priv->combo_box, "changed");
            handle=TRUE;
            g_signal_handler_block(priv->combo_box, priv->combo_box_changed_id);
        }
    }
}

GtkWidget * gtk_file_chooser_button_new(const gchar *title, GtkFileChooserAction action)
{
    static void * (*realFunction)() = NULL;

    GtkWidget *button=NULL;

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "gtk_file_chooser_button_new");

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x02) printf("KGTK::gtk_file_chooser_button_new\n");
#endif

    if(kgtkInit(NULL))
    {
        GtkFileChooserButtonPrivate *priv=NULL;

        button=realFunction(title, action);
        priv=GTK_FILE_CHOOSER_BUTTON(button)->priv;

        if(priv->button)
        {
            g_signal_handlers_disconnect_matched(priv->button,
                                                 G_SIGNAL_MATCH_DATA,0, 0, NULL, NULL, button);

            g_signal_connect(priv->button, "clicked",
                             G_CALLBACK(handleGtkFileChooserButtonClicked),
                             GTK_FILE_CHOOSER_BUTTON(button));
        }
        if(priv->combo_box)
        {
            g_signal_handler_block(priv->combo_box, priv->combo_box_changed_id);

            g_signal_connect(priv->combo_box, "changed",
                             G_CALLBACK(handleGtkFileChooserComboChanged),
                             GTK_FILE_CHOOSER_BUTTON(button));
        }
    }
    return button;
}
#endif /*KGTK_SAFE_FILE_CHOOSER_BUTTON_SUPPORT */

static gboolean isGtk(const char *str)
{
    return 'g'==str[0] && 't'==str[1] && 'k'==str[2] && '_'==str[3];
}

static void * kgtk_get_fnptr(const char *raw_name)
{
    if(raw_name && isGtk(raw_name) && kgtkInit(NULL))
    {
        if(0==strcmp(raw_name, "gtk_file_chooser_get_filename"))
            return &gtk_file_chooser_get_filename;

        else if(0==strcmp(raw_name, "gtk_file_chooser_select_filename"))
            return &gtk_file_chooser_select_filename;

        else if(0==strcmp(raw_name, "gtk_file_chooser_unselect_all"))
            return &gtk_file_chooser_unselect_all;

        else if(0==strcmp(raw_name, "gtk_file_chooser_set_filename"))
            return &gtk_file_chooser_set_filename;

        else if(0==strcmp(raw_name, "gtk_file_chooser_set_current_name"))
            return &gtk_file_chooser_set_current_name;

        else if(0==strcmp(raw_name, "gtk_file_chooser_get_filenames"))
            return &gtk_file_chooser_get_filenames;

        else if(0==strcmp(raw_name, "gtk_file_chooser_set_current_folder"))
            return &gtk_file_chooser_set_current_folder;

        else if(0==strcmp(raw_name, "gtk_file_chooser_get_current_folder"))
            return &gtk_file_chooser_get_current_folder;

        else if(0==strcmp(raw_name, "gtk_file_chooser_get_uri"))
            return &gtk_file_chooser_get_uri;

        else if(0==strcmp(raw_name, "gtk_file_chooser_set_uri"))
            return &gtk_file_chooser_set_uri;

        else if(0==strcmp(raw_name, "gtk_file_chooser_get_uris"))
            return &gtk_file_chooser_get_uris;

        else if(0==strcmp(raw_name, "gtk_file_chooser_set_current_folder_uri"))
            return &gtk_file_chooser_set_current_folder_uri;

        else if(0==strcmp(raw_name, "gtk_file_chooser_get_current_folder_uri"))
            return &gtk_file_chooser_get_current_folder_uri;

        else if(0==strcmp(raw_name, "gtk_file_chooser_dialog_new"))
            return &gtk_file_chooser_dialog_new;

        else if(0==strcmp(raw_name, "gtk_file_chooser_button_new"))
            return &gtk_file_chooser_button_new;

/*
        else if(0==strcmp(raw_name, "gtk_init_check"))
            return &gtk_init_check;
*/
    }

    return NULL;
}

const gchar * kgtk_g_module_check_init(GModule *module)
{
    return gtk_check_version(GTK_MAJOR_VERSION, GTK_MINOR_VERSION, GTK_MICRO_VERSION - GTK_INTERFACE_AGE);
}

/* Mozilla specific */
void * PR_FindFunctionSymbol(struct PR_LoadLibrary *lib, const char *raw_name)
{
    static void * (*realFunction)() = NULL;

    void *rv=NULL;

    if(!realFunction)
        realFunction = (void *(*)()) real_dlsym(RTLD_NEXT, "PR_FindFunctionSymbol");

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x20) printf("KGTK::PR_FindFunctionSymbol : %s\n", raw_name);
#endif

    rv=kgtk_get_fnptr(raw_name);

    if(!rv)
    {
        if (0==strcmp(raw_name, "g_module_check_init"))
            rv=&kgtk_g_module_check_init;
        else if (isGtk(raw_name))
            rv=real_dlsym(RTLD_NEXT, raw_name);
    }

    return rv ? rv : realFunction(lib, raw_name);
}

#ifdef HAVE_DLVSYM
/* Overriding dlsym is required for SWT - which dlsym's the gtk_file_chooser functions! */
static void * real_dlsym(void *handle, const char *name)
{
    static void * (*realFunction)() = NULL;

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x20) printf("KGTK::real_dlsym : %s\n", name);
#endif

    if (!realFunction)
    {
        void *ldHandle=dlopen("libdl.so", RTLD_NOW);

#ifdef KGTK_DEBUG
        if(kgtkDebug&0x20) printf("KGTK::real_dlsym : %s\n", name);
#endif

        if(ldHandle)
        {
            static const char * versions[]={KGTK_DLSYM_VERSION, "GLIBC_2.3", "GLIBC_2.2.5",
                                            "GLIBC_2.2", "GLIBC_2.1", "GLIBC_2.0", NULL};

            int i;

            for(i=0; versions[i] && !realFunction; ++i)
                realFunction=dlvsym(ldHandle, "dlsym", versions[i]);
        }
    }

    return realFunction(handle, name);
}

void * dlsym(void *handle, const char *name)
{
    void *rv=NULL;

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x20) printf("KGTK::dlsym : (%p) %s\n", handle, name);
#endif
    rv=kgtk_get_fnptr(name);

    if(!rv)
        rv=real_dlsym(handle, name);

    if(!rv && 0==strcmp(name, "g_module_check_init"))
        rv=&kgtk_g_module_check_init;

#ifdef KGTK_DEBUG
    if(kgtkDebug&0x20) printf("KGTK::dlsym found? %d\n", rv ? 1 : 0);
#endif
    return rv;
}
#endif
