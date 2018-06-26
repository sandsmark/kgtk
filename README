Introduction
------------

KGtk is a quick-n-dirty *HACK* to allow *some* Gtk2, and Gtk3, applications
to use KDE Frameworks 5 file dialogs.

KGtk is composed of the following pieces:

1. An application called kdialogd5. This is the KDE app that will show the
   file selector.
2. LD_PRELOAD libraries that are used to override the Gtk2 and Gtk3 file
   dialogs.

If you start an application using the following command:
    kgtk-wrapper gimp

...the the following occurs:

1. kgtk-wrapper determines whether the application is a Gtk2 or Gtk3
   application. It then sets the LD_PRELOAD environment variable to point to 
   the approriate KGtk library.
2. When 'gimp' now tries to open a file dialog, the KGtk library
   intercepts this, and asks kdialogd to open a file dialog instead.

There will only ever be one instance of kdialogd, and all apps communicate with
the same instance - and it termiantes itself 30 seconds after the last app has
disconnected. This timeout can be changed by editing kdialogdrc and 
setting/changing

    [General]
    Timeout=10


Installation
------------
As of v0.9.1, kgtk uses CMake in place of autotools.

This is accomplished as follows:

1. mkdir build
2. cd build
3. cmake .. -DCMAKE_INSTALL_PREFIX=/usr
4. make
5. sudo make install

* For 64 bit systems, also append -DLIB_SUFFIX=64


Notes
-----

The library has been tested with the following applications:

Reported to work:

  1. Firefox (7.0.1)
  2. Thunderbird 7.0.1
  3. Inkscape
  4. GIMP
  5. Kino
  6. Eclipse
  7. Azureus
  8. Galde-2
  9. Streamtuner
 10. Avidemux2

Reported *not* working:

  1. AbiWord
