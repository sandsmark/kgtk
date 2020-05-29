include(FindPkgConfig)

pkg_check_modules(GTK2 gtk+-2.0>=2.6)

if (GTK2_FOUND)
    message("** INFORMATION: Gtk2 LD_PRELOAD library will be built.")

    # set(LIB_SUFFIX "" CACHE STRING "Define suffix of directory name (32/64)" )
    set(LIB_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/lib${LIB_SUFFIX}  CACHE PATH "The subdirectory relative to the install prefix where libraries will be installed (default is /lib${LIB_SUFFIX})" FORCE)

    include_directories (${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR}/common ${CMAKE_BINARY_DIR} ${GTK2_INCLUDE_DIRS})
    set(kgtk2_SRCS ../common/kgtk.c)

    add_library(kgtk2 SHARED ${kgtk2_SRCS})
    set_target_properties(kgtk2 PROPERTIES C_VISIBILITY_PRESET default SOVERSION "5")
    target_link_libraries(kgtk2 ${GTK2_LDFLAGS} -lgthread-2.0 -lglib-2.0 -lc -ldl)

    install(TARGETS kgtk2 LIBRARY DESTINATION ${LIB_INSTALL_DIR}/kgtk NAMELINK_SKIP)

    configure_file (kgtk2-wrapper.cmake ${CMAKE_CURRENT_BINARY_DIR}/kgtk2-wrapper @ONLY)
    install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/kgtk2-wrapper DESTINATION bin)
else (GTK2_FOUND)
    message("** ERROR      : Could not locate Gtk2 headers, Gtk2 LD_PRELOAD library will not be built.")
endif (GTK2_FOUND)
