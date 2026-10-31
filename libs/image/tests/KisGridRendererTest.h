/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISGRIDRENDERERTEST_H
#define KISGRIDRENDERERTEST_H

#include <QtTest>

class KisGridRendererTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testSameAsKrita_data();
    void testSameAsKrita();
    void testChanges_data();
    void testChanges();
    void testWarpSameAsKrita_data();
    void testWarpSameAsKrita();
    void testSavedConfigs_data();
    void testSavedConfigs();
};

#endif
