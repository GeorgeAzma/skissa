#
# SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Skissa's palette panel, in place of Krita's (../palettedocker, which isn't built itself, and whose plugin
# description is used as it is). A Krita change to the files kept here has to be brought over by hand;
# packaging/skissa-update.py lists them after a merge.

set(KRITA_PALETTEDOCKER_SOURCES
    palettedocker.cpp palettedocker_dock.cpp
    # Krita's header of the file kept here, for moc
    ../palettedocker/palettedocker.h
    )

kis_add_library(kritapalettedocker MODULE ${KRITA_PALETTEDOCKER_SOURCES})
target_include_directories(kritapalettedocker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../palettedocker)
target_link_libraries(kritapalettedocker kritaui)
install(TARGETS kritapalettedocker  DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
