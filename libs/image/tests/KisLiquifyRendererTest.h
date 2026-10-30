/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISLIQUIFYRENDERERTEST_H
#define KISLIQUIFYRENDERERTEST_H

#include <QtTest>

class KisLiquifyRendererTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testSameAsKrita_data();
    void testSameAsKrita();
    void testChanges_data();
    void testChanges();
    void testSavedConfigs_data();
    void testSavedConfigs();
};

#endif
