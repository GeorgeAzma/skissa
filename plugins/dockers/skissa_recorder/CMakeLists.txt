#
# SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Skissa's time-lapse panel: its own panel and settings files, with the rest of Krita's recorder as it is
# (../recorder, which isn't built itself). A Krita change to the files kept here has to be brought over by hand;
# packaging/skissa-update.py lists them after a merge.

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

set(KRITA_RECORDER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../recorder)

set(KRITA_RECORDERDOCKER_SOURCES
    recorder_config.cpp
    recorderdocker.cpp
    recorderdocker_dock.cpp
    # Krita's header of a file kept here, for moc
    ${KRITA_RECORDER_DIR}/recorderdocker.h
    ${KRITA_RECORDER_DIR}/recorder_const.cpp
    ${KRITA_RECORDER_DIR}/recorder_export_config.cpp
    ${KRITA_RECORDER_DIR}/recorder_format.cpp
    ${KRITA_RECORDER_DIR}/recorder_writer.cpp
    ${KRITA_RECORDER_DIR}/recorder_export.cpp
    ${KRITA_RECORDER_DIR}/recorder_snapshots_manager.cpp
    ${KRITA_RECORDER_DIR}/recorder_snapshots_scanner.cpp
    ${KRITA_RECORDER_DIR}/recorder_directory_cleaner.cpp
    ${KRITA_RECORDER_DIR}/recorder_export_preprocessor.cpp
    ${KRITA_RECORDER_DIR}/recorder_image_copying_job.cpp
)

set(KRITA_RECORDERDOCKER_UIS
    ${KRITA_RECORDER_DIR}/recorder_export.ui
    ${KRITA_RECORDER_DIR}/recorder_snapshots_manager.ui
)

if(NOT ANDROID)
    list(APPEND KRITA_RECORDERDOCKER_SOURCES ${KRITA_RECORDER_DIR}/recorder_profile_settings.cpp)
    list(APPEND KRITA_RECORDERDOCKER_UIS ${KRITA_RECORDER_DIR}/recorder_profile_settings.ui)
endif()

ki18n_wrap_ui(KRITA_RECORDERDOCKER_SOURCES ${KRITA_RECORDERDOCKER_UIS})

install(FILES
    ${KRITA_RECORDER_DIR}/recorder.action

    DESTINATION ${KDE_INSTALL_DATADIR}/krita/actions
)

kis_add_library(kritarecorderdocker MODULE ${KRITA_RECORDERDOCKER_SOURCES})
target_include_directories(kritarecorderdocker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${KRITA_RECORDER_DIR})
target_link_libraries(kritarecorderdocker kritaui Threads::Threads)

install(TARGETS kritarecorderdocker  DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
