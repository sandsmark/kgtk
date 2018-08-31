#!/bin/bash

#
# This script is part of the KGtk package.
#
# Copyright 2006-2011 Craig Drummond <craig.p.drummond@gmail.com>
#
#
# --
# Released under the GPL v2 or later
# --
#
# This script attempts to determine which KGtk library (if any) should
# be used when launching the app
#

case "$KDE_SESSION_VERSION"
in
5)	KRC="kreadconfig${KDE_SESSION_VERSION}"
	LIBSUFF=".${KDE_SESSION_VERSION}"
	;;

*)	KRC="kreadconfig"
	LIBSUFF=""
	;;
esac

if [ "$(locale | grep 'LANG=' | grep -i 'utf-8' | wc -l)" = "0" ] ; then
    export G_BROKEN_FILENAMES=1
fi

app_arg="$1"
shift

app_arg_path="$(dirname "$app_arg")"
app_name="$(basename "$app_arg")"

if [ -n "$app_arg_path" ] && [ -x "$app_arg" ] ; then
    app_abspath="$app_arg"
else
    app_abspath="$(which "$app_name")"
	if [ -z "$app_abspath" ] ; then
		app_abspath="$(which "./$app_name")"
	fi
fi

toolkit="$($KRC --file kgtkrc --group 'Apps' --key "$app_name")"

if [ "$toolkit" = "" ] ; then
    case "$app_name" in
        libreoffice | lowriter | localc | lobase | lodraw | loffice | lomath | loweb)
            export OOO_FORCE_DESKTOP=gnome
            toolkit="gtk2" ;;
        eclipse | gimp | inkscape | kino | iceweasel | swiftfox | azureus | mozilla* | thunderbird)
            toolkit="gtk2" ;;
        abiword) # Non-working
            toolkit="x" ;;
    esac
fi

if [ "$toolkit" = "" ] && [ ! -z "$app_abspath" ] ; then
    libs="$(ldd "$app_abspath" 2>/dev/null)"

    if [ ! -z "$libs" ] ; then
        if [ "0" != "$(echo "$libs" | grep libgtk-x11-2 | wc -l)" ] ; then
            toolkit="gtk2"
        elif [ "0" != "$(echo "$libs" | grep libgtk-3 | wc -l)" ] ; then
            toolkit="gtk3"
        fi
    fi
fi

if [ "$toolkit" = "x" ] ; then
    toolkit=""
fi

libkgtk_path="@CMAKE_INSTALL_PREFIX@/lib@LIB_SUFFIX@/kgtk/libk${toolkit}.so${LIBSUFF}"
if [ -f "$libkgtk_path" ] ; then
    export LD_PRELOAD="$libkgtk_path:$LD_PRELOAD"
fi
exec "$app_abspath" "$@"
