#
# SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Skissa's layer panel: its own rows, list and panel files, with the rest of Krita's layer docker as it is
# (../layerdocker, which isn't built itself). A Krita change to the files kept here has to be brought over by
# hand; packaging/skissa-update.py lists them after a merge.

set(KRITA_LAYERDOCKER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../layerdocker)

set(kritalayerdocker_static_SRCS
    LayerBox.cpp
    NodeDelegate.cpp
    NodeToolTip.cpp
    NodeView.cpp
    ${KRITA_LAYERDOCKER_DIR}/NodeViewVisibilityDelegate.cpp
    # Krita's header of a file kept here, for moc
    ${KRITA_LAYERDOCKER_DIR}/NodeToolTip.h
)

ki18n_wrap_ui(kritalayerdocker_static_SRCS
    ${KRITA_LAYERDOCKER_DIR}/WdgLayerBox.ui
)

if (QT_MAJOR_VERSION STREQUAL "6")
QT_WRAP_CPP(kritalayerdocker_HEADERS_MOC
    NodePropertyAction_p.h
    ${KRITA_LAYERDOCKER_DIR}/SyncButtonAndAction.h
)
else()
QT5_WRAP_CPP(kritalayerdocker_HEADERS_MOC
    NodePropertyAction_p.h
    ${KRITA_LAYERDOCKER_DIR}/SyncButtonAndAction.h
)
endif()

kis_add_library(kritalayerdocker_static STATIC ${kritalayerdocker_static_SRCS} ${kritalayerdocker_HEADERS_MOC})
target_include_directories(kritalayerdocker_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${KRITA_LAYERDOCKER_DIR})

generate_export_header(kritalayerdocker_static BASE_NAME kritalayerdocker)

target_link_libraries(kritalayerdocker_static kritaui)

# with Krita's header, for moc
kis_add_library(kritalayerdocker MODULE LayerDocker.cpp ${KRITA_LAYERDOCKER_DIR}/LayerDocker.h)

target_link_libraries(kritalayerdocker kritaui kritalayerdocker_static)
install(TARGETS kritalayerdocker  DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
