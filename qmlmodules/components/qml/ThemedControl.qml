/*
 *  SPDX-FileCopyrightText: 2025 Wolthera van Hövell tot Westerflier <griffinvalley@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
import QtQuick 2.15
import QtQuick.Controls 2.15
import org.krita.components 1.0 as Kis

/**
  Dummy Control to work around QQuickPalette and QPalette being two completely different things.
  This could probably be replaced with Kirigami.Theme if we start using Kirigami.

  You will need to manually setup this control and its color group for each control that can be disabled,
  to ensure that the correct palette is retrieved. Qt6 has something more complex for this, where the
  active/inactive(unfocused)/disabled palettes can all be set, but in Qt5 this is the best we've got.
  */
Control {
    id: themedControl;

    // There doesn't seem to be any relevant difference between inactive and active, so we don't bother distinguishing between them.
    property int colorGroup: enabled? SystemPalette.Active: SystemPalette.Disabled;

    /*
      In a panel of glass (KisQQuickWidget, made so by KisStudioGlass): the fields are veils of
      white or black, as the widgets there are (KisStudioGlass::veilFor), the glass seen through
      them. The window color stays: popups are drawn with it.
      */
    property bool glass: typeof studioGlass !== "undefined" && studioGlass;

    // white or black, as opaque as it takes over the window's color to make c's lightness (a little more, at least)
    function lightness(c) {
        // HSL's, as QColor::lightnessF (the color type here has no lightness of its own)
        return (Math.max(c.r, c.g, c.b) + Math.min(c.r, c.g, c.b)) / 2;
    }
    function veil(c) {
        const l = lightness(c);
        const u = lightness(theme.window.backgroundColor);
        const lighter = l >= u;
        const room = lighter ? 1 - u : u;
        const amount = room > 0 ? Math.min(1, Math.max(0.04, Math.abs(l - u) / room)) : 0;
        return lighter ? Qt.rgba(1, 1, 1, amount) : Qt.rgba(0, 0, 0, amount);
    }

    property Kis.Theme theme: Kis.Theme {
        window.state: colorGroup;
        view.state: colorGroup;
        button.state: colorGroup;
        selection.state: colorGroup;
        tooltip.state: colorGroup;
    }

    Component.onCompleted: {
        // for some reason palette doesn't get assigned when the qquickwidget has no parent,
        // so instead we force slotUpdateThemes here. This is not an issue in qt6
        if (visible && palette.buttonText !== theme.button.textColor) {
            theme.slotUpdateThemes();
        }
    }

    palette.alternateBase: glass ? veil(theme.view.alternateBackgroundColor) : theme.view.alternateBackgroundColor;
    palette.base: glass ? veil(theme.view.backgroundColor) : theme.view.backgroundColor;
    // not the button color: the style makes the buttons' from it lighter and darker, which in Qt 5 makes a veil opaque
    palette.button: theme.button.backgroundColor;
    palette.buttonText: theme.button.textColor;
    palette.dark: theme.window.darkShadeColor;
    palette.highlight: theme.selection.backgroundColor;
    palette.highlightedText: theme.selection.textColor;
    palette.light: theme.window.lightShadeColor;


    palette.mid: theme.window.midShadeColor;
    palette.midlight: theme.window.midlightShadeColor;
    palette.shadow: theme.window.shadowShadeColor;
    palette.text: theme.view.textColor;
    palette.window: theme.window.backgroundColor;
    palette.windowText: theme.window.textColor;

    palette.link: theme.view.linkColor;
    palette.linkVisited: theme.view.linkVisitedColor;
    palette.toolTipBase: theme.tooltip.backgroundColor;
    palette.toolTipText: theme.tooltip.textColor;
    palette.brightText: Qt.lighter(theme.view.textColor);

}
