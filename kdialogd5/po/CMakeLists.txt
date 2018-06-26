find_package(Msgfmt REQUIRED)

# .po to .gmo stuff
file(GLOB _pofiles *.po)

foreach(_file ${_pofiles})
    get_filename_component(_file_we ${_file} NAME_WE)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/${_file_we}.gmo")
    set(_in "${_file_we}.po")
    add_custom_command(OUTPUT ${_out} COMMAND ${MSGFMT_EXECUTABLE} -o ${_out} ${_file} DEPENDS ${_file})
    install(FILES ${_out} DESTINATION share/locale/${_file_we}/LC_MESSAGES/ RENAME kdialogd5.mo)
    set(_outputs ${_outputs} ${_out})
endforeach(_file)

add_custom_target(pofiles ALL DEPENDS ${_outputs})

# Stuff to generate the .pot
set(POT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../kdialogd.cpp)
set(POT_OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/kdialogd5.pot)

# Find xgettext
find_program(XGETTEXT_PATH NAMES "xgettext" PATHS "/usr/bin" "/usr/local/bin")
if(${XGETTEXT_PATH} STREQUAL "XGETTEXT_PATH-NOTFOUND")
    message(STATUS "xgettext not found.  You will not be able to run 'make extract_messages' in the 'po' directory.")
else(${XGETTEXT_PATH} STREQUAL "XGETTEXT_PATH-NOTFOUND")
    message(STATUS "Found xgettext: ${XGETTEXT_PATH}")
endif(${XGETTEXT_PATH} STREQUAL "XGETTEXT_PATH-NOTFOUND")

if(EXISTS ${KDE4_INCLUDE_DIR}/kde.pot)
    add_custom_command(
        OUTPUT ${POT_OUTPUT}
        COMMAND ${XGETTEXT_PATH} --foreign-user -C -ci18n -ki18n -ktr2i18n -kI18N_NOOP -kI18N_NOOP2 -kaliasLocale -x "${KDE4_INCLUDE_DIR}/kde.pot" -o ${POT_OUTPUT} ${POT_SOURCES}
    )
else (EXISTS ${KDE4_INCLUDE_DIR}/kde.pot)
    add_custom_command(
        OUTPUT ${POT_OUTPUT}
        COMMAND ${XGETTEXT_PATH} --foreign-user -C -ci18n -ki18n -ktr2i18n -kI18N_NOOP -kI18N_NOOP2 -kaliasLocale -o ${POT_OUTPUT} ${POT_SOURCES}
    )
endif (EXISTS ${KDE4_INCLUDE_DIR}/kde.pot)

add_custom_target(extract_messages DEPENDS ${POT_OUTPUT})
