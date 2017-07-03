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

if [ "`locale | grep 'LANG=' | grep -i 'utf-8' | wc -l`" = "0" ] ; then
    export G_BROKEN_FILENAMES=1
fi

app=`basename $0`

if [ "$app" = "kgtk2-wrapper" ] ; then
    LD_PRELOAD=@CMAKE_INSTALL_PREFIX@/lib/kgtk/libkgtk2.so:$LD_PRELOAD "$@"
else
    dir=`dirname $0`
    oldPath=$PATH
    PATH=`echo $PATH | sed s:$dir::g`
    real=`which $app`
    PATH=$oldPath

    if [ "$real" != "" ] && [ "`dirname $real`" != "$dir" ] ; then
        LD_PRELOAD=@CMAKE_INSTALL_PREFIX@/lib@LIB_SUFFIX@/kgtk/libkgtk2.so:$LD_PRELOAD $real "$@"
    fi
fi
