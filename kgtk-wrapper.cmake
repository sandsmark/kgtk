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

if [ "`locale | grep 'LANG=' | grep -i 'utf-8' | wc -l`" = "0" ] ; then
    export G_BROKEN_FILENAMES=1
fi

app=`basename $0`
useApp=1

if [ "$app" = "kgtk-wrapper" ] ; then
    app=`basename $1`
    useApp=0
fi

dir=$(cd "$(dirname "$0")"; pwd)
if [ $useApp -eq 1 ] ; then
    oldPath=$PATH
    PATH=`echo $PATH | sed s:$dir::g | sed "s|::*|:|g"`
fi

realApp=`which $app`

if [ -z $realApp ] ; then
    realApp=`which ./$app`
fi

if [ $useApp -eq 1 ] ; then
   PATH=$oldPath
fi

toolkit=`kreadconfig --file kgtkrc --group 'Apps' --key "$app"`

if [ "$toolkit" = "" ] ; then
    case $app in
        libreoffice | lowriter | localc | lobase | lodraw | loffice | lomath | loweb)
            export OOO_FORCE_DESKTOP=gnome
            toolkit="gtk2" ;;
        eclipse | gimp | inkscape | firefox | kino | iceweasel | swiftfox | azureus | mozilla* | thunderbird)
            toolkit="gtk2" ;;
        abiword) # Non-working
            toolkit="x" ;;
    esac
fi

if [ "$toolkit" = "" ] && [ ! -z "$realApp" ] ; then
    libs=`ldd $realApp 2>/dev/null`

    if [ ! -z "$libs" ] ; then

        if [ "0" != "`echo $libs | grep libgtk-x11-2 | wc -l`" ] ; then
            toolkit="gtk2"
        elif [ "0" != "`echo $libs | grep libgtk-3 | wc -l`" ] ; then
            toolkit="gtk3"
        fi
    fi
fi

if [ "$toolkit" = "x" ] ; then
    toolkit=""
fi

if [ ! -f "@CMAKE_INSTALL_PREFIX@/lib@LIB_SUFFIX@/kgtk/libk${toolkit}.so" ] ; then
    if [ $useApp -eq 1 ] ; then
        $realApp "$@"
    else
        "$@"
    fi
else
    if [ "$useApp" ] && [ "`dirname $realApp`" != "$dir" ] ; then
        LD_PRELOAD=@CMAKE_INSTALL_PREFIX@/lib@LIB_SUFFIX@/kgtk/libk${toolkit}.so:$LD_PRELOAD $realApp "$@"
    else
        LD_PRELOAD=@CMAKE_INSTALL_PREFIX@/lib@LIB_SUFFIX@/kgtk/libk${toolkit}.so:$LD_PRELOAD "$@"
    fi
fi
