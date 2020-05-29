include(FindPkgConfig)

pkg_check_modules(GTK3 gtk+-3.0>=3.0)

if (GTK3_FOUND)
    message("** INFORMATION: Gtk3 LD_PRELOAD library will be built.")

    # set(LIB_SUFFIX "" CACHE STRING "Define suffix of directory name (32/64)" )
    set(LIB_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/lib${LIB_SUFFIX}  CACHE PATH "The subdirectory relative to the install prefix where libraries will be installed (default is /lib${LIB_SUFFIX})" FORCE)

    include_directories (${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR}/common ${CMAKE_BINARY_DIR} ${GTK3_INCLUDE_DIRS})
    set(kgtk3_SRCS ../common/kgtk.c)

    add_library(kgtk3 SHARED ${kgtk3_SRCS})
    set_target_properties(kgtk3 PROPERTIES C_VISIBILITY_PRESET default SOVERSION "5")
    target_link_libraries(kgtk3 ${GTK3_LDFLAGS} -lgthread-2.0 -lglib-2.0 -lc -ldl)

    install(TARGETS kgtk3 LIBRARY DESTINATION ${LIB_INSTALL_DIR}/kgtk NAMELINK_SKIP)

    configure_file (kgtk3-wrapper.cmake ${CMAKE_CURRENT_BINARY_DIR}/kgtk3-wrapper @ONLY)
    install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/kgtk3-wrapper DESTINATION bin)
else (GTK3_FOUND)
    message("** ERROR      : Could not locate Gtk3 headers, Gtk3 LD_PRELOAD library will not be built.")
endif (GTK3_FOUND)
