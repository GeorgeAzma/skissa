/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISPUPPETTEST_H
#define KISPUPPETTEST_H

#include <QtTest>

class KisPuppetTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testOnePinMoves();
    void testBendKeepsLength();
    void testAirLetsGo();
    void testSamePinsSamePose();
    void testPinStaysWhereAdded();
};

#endif
