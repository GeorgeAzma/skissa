#
# SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Skissa's own files in kritaui, compiled as a part of their own: changing one doesn't make Qt's moc
# check every kritaui header again (a quarter of a million timestamps, seconds on every build), and
# merges from Krita don't meet the fork's lines in kritaui's list.

set(skissa_ui_SRCS
    KisColorDrop.cpp
    KisLastStrokeReplay.cpp
    KisSettingsRegistry.cpp
    KisSettingsTransfer.cpp
    KisSettingsUndo.cpp
    KisStudioBarButtons.cpp
    KisStudioBrushEditor.cpp
    KisStudioBrushMemory.cpp
    KisStudioBrushPicker.cpp
    KisStudioBrushSheet.cpp
    KisStudioCanvasHud.cpp
    KisStudioCanvasRing.cpp
    KisStudioColorPanel.cpp
    KisStudioControls.cpp
    KisStudioEffectOverlay.cpp
    KisStudioEffectsPicker.cpp
    KisStudioEmptyStates.cpp
    KisStudioFind.cpp
    KisStudioGlass.cpp
    KisStudioHoldKeys.cpp
    KisStudioItemCard.cpp
    KisStudioLayout.cpp
    KisStudioMakeBrush.cpp
    KisStudioMixing.cpp
    KisStudioMotion.cpp
    KisStudioPanelPicker.cpp
    KisStudioPenPopup.cpp
    KisStudioRings.cpp
    KisStudioScrubber.cpp
    KisStudioSelectionAdjust.cpp
    KisStudioSettings.cpp
    KisStudioSmoothing.cpp
    KisStudioStartPage.cpp
    KisStudioTips.cpp
    KisStudioSymmetry.cpp
    KisStudioTheme.cpp
    KisStudioToolRail.cpp
    KisStudioToolStrip.cpp
    KisStudioWindowFrame.cpp
    KisUpdatePacer.cpp
    dialogs/KisKeysPage.cpp
    input/KisDragValueAction.cpp
    opengl/KisOpenGLGlass.cpp
    tool/KisStrokeShape.cpp
    widgets/KisBlendModePicker.cpp
    widgets/KisCurvePresetStrip.cpp
    widgets/KisPopupCloser.cpp
    widgets/KisPresetStrokeThumbnails.cpp
    widgets/KisStudioChips.cpp
    widgets/KisStudioDabPictures.cpp
    widgets/KisStudioInlineEdit.cpp
    widgets/KisStudioPicker.cpp
    widgets/KisStudioPopup.cpp
    widgets/KisStudioValueRow.cpp
)

kis_add_library(kritaui_skissa OBJECT ${skissa_ui_SRCS} PCH_FILE kis_image_pch.h)
# built as kritaui itself: its exports, defines, include paths and dependencies
target_compile_definitions(kritaui_skissa PRIVATE kritaui_EXPORTS $<TARGET_PROPERTY:kritaui,COMPILE_DEFINITIONS>)
target_include_directories(kritaui_skissa PRIVATE $<TARGET_PROPERTY:kritaui,INCLUDE_DIRECTORIES>)
target_compile_options(kritaui_skissa PRIVATE $<TARGET_PROPERTY:kritaui,COMPILE_OPTIONS>)
target_link_libraries(kritaui_skissa PRIVATE $<TARGET_PROPERTY:kritaui,LINK_LIBRARIES>)
target_sources(kritaui PRIVATE $<TARGET_OBJECTS:kritaui_skissa>)
# headers made from kritaui's own forms that these files include
add_custom_target(kritaui_skissa_forms DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ui_KisWelcomePage.h ${CMAKE_CURRENT_BINARY_DIR}/ui_KisNewsPage.h)
add_dependencies(kritaui_skissa kritaui_skissa_forms)
