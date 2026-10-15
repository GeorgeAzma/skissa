#
# SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
#
# SPDX-License-Identifier: BSD-3-Clause
#
# In place of ki18n_install(po): Krita's translations (po/<lang>/krita.po, kept as upstream has them)
# joined with Skissa's own (po-skissa/<lang>.po, from packaging/skissa-translations.py), and each
# language compiled only when its files change.

function(skissa_install_translations)
    if (NOT LOCALE_INSTALL_DIR)
        set(LOCALE_INSTALL_DIR share/locale)
    endif()
    get_filename_component(dirname ${LOCALE_INSTALL_DIR} NAME)
    get_filename_component(destname ${LOCALE_INSTALL_DIR} DIRECTORY)
    set(locale ${CMAKE_CURRENT_BINARY_DIR}/${dirname})
    set(script ${CMAKE_CURRENT_SOURCE_DIR}/packaging/skissa-translations.py)

    file(GLOB langs LIST_DIRECTORIES true RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/po ${CMAKE_CURRENT_SOURCE_DIR}/po/*)
    # a language the fork gains translations for is picked up without configuring again
    file(GLOB forks CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/po-skissa/*.po)
    set(outputs)
    foreach(lang ${langs})
        set(upstream ${CMAKE_CURRENT_SOURCE_DIR}/po/${lang}/krita.po)
        set(fork ${CMAKE_CURRENT_SOURCE_DIR}/po-skissa/${lang}.po)
        set(mo ${locale}/${lang}/LC_MESSAGES/krita.mo)
        if(NOT EXISTS ${upstream})
            continue()
        endif()
        if(fork IN_LIST forks)
            set(joined ${CMAKE_CURRENT_BINARY_DIR}/po-joined/${lang}/krita.po)
            add_custom_command(OUTPUT ${mo}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${locale}/${lang}/LC_MESSAGES
                COMMAND ${KI18N_PYTHON_EXECUTABLE} ${script} --join ${upstream} ${fork} ${joined}
                COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} -o ${mo} ${joined}
                DEPENDS ${upstream} ${fork} ${script}
                VERBATIM)
        else()
            add_custom_command(OUTPUT ${mo}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${locale}/${lang}/LC_MESSAGES
                COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} -o ${mo} ${upstream}
                DEPENDS ${upstream}
                VERBATIM)
        endif()
        list(APPEND outputs ${mo})
    endforeach()
    add_custom_target(pofiles ALL DEPENDS ${outputs})

    file(MAKE_DIRECTORY ${locale})
    install(DIRECTORY ${locale} DESTINATION ${destname})
endfunction()
