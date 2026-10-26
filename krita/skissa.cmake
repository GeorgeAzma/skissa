# The program is skissa.exe (skissa.com in a console). krita.exe only starts it, for what still points there: pins,
# shortcuts, "Open with", and the update of a Skissa from before the rename, whose script starts bin\krita.exe.
if (WIN32)
    set_target_properties(krita_windows_stub_exe PROPERTIES OUTPUT_NAME "skissa")
    set_target_properties(krita_windows_stub_com PROPERTIES OUTPUT_NAME "skissa")

    add_executable(skissa_krita_bridge WIN32
        skissa_bridge.cpp
        ${krita_windows_stub_ICONS_SRCS}
        ${CMAKE_CURRENT_BINARY_DIR}/versioninfo.rc
    )
    set_target_properties(skissa_krita_bridge PROPERTIES OUTPUT_NAME "krita")
    install(TARGETS skissa_krita_bridge ${INSTALL_TARGETS_DEFAULT_ARGS})
endif ()
