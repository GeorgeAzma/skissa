/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISRESAMPLINGTEST_H
#define KISRESAMPLINGTEST_H

#include <QtTest>

class KisResamplingTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testPixelCopiesWholeScales();
    void testPixelKeepsPixelsEven();
    void testSmoothHasNoHalos();
    void testShrinkingKeepsLight();
    void testShrinkingKeepsThinLines();
    void testTellsPixelArt();
};

#endif
