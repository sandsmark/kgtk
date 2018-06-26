include_directories (${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR}/common ${CMAKE_BINARY_DIR} ${KDE4_INCLUDE_DIR} ${QT_INCLUDE_DIR})
set(kdialogd5_bin_SRCS kdialogd.cpp)
add_executable(kdialogd5_bin ${kdialogd5_bin_SRCS})
set_target_properties(kdialogd5_bin PROPERTIES OUTPUT_NAME kdialogd5)

target_link_libraries(kdialogd5_bin
    KF5::KIOWidgets
    KF5::KDELibs4Support
    Qt5::X11Extras
    Qt5::Widgets
    Qt5::Core
    )

install(TARGETS kdialogd5_bin DESTINATION ${LIBEXEC_INSTALL_DIR} )
add_subdirectory(po)

