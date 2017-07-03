include_directories (${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR}/common ${CMAKE_BINARY_DIR} ${KDE4_INCLUDE_DIR} ${QT_INCLUDE_DIR})
set(kdialogd4_bin_SRCS kdialogd.cpp)
kde4_add_executable(kdialogd4_bin ${kdialogd4_bin_SRCS})
set_target_properties(kdialogd4_bin PROPERTIES OUTPUT_NAME kdialogd4)
target_link_libraries(kdialogd4_bin ${KDE4_KDEUI_LIBS} ${KDE4_KIO_LIBS} ${KDE4_KIO_LIBS} ${KDE4_KDECORE_LIBRARY} kfile )
install(TARGETS kdialogd4_bin DESTINATION ${LIBEXEC_INSTALL_DIR} )
add_subdirectory(po)

