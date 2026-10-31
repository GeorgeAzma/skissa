/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISTRANSFORMPERSPECTIVETEST_H
#define KISTRANSFORMPERSPECTIVETEST_H

#include <simpletest.h>

class KisTransformPerspectiveTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testFit();
    void testFitRotatedBounds();
    void testFlattening();
    void testVanishingPoint();
    void testVanishingLines();
};

#endif
