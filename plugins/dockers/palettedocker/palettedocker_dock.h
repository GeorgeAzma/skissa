/*
 *  SPDX-FileCopyrightText: 2013 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */


#ifndef PALETTEDOCKER_DOCK_H
#define PALETTEDOCKER_DOCK_H

#include <QDockWidget>
#include <QModelIndex>
#include <QScopedPointer>
#include <QPointer>
#include <QTimer>

#include <KoResourceServer.h>
#include <resources/KoColorSet.h>

#include <kis_canvas2.h>
#include <kis_mainwindow_observer.h>
#include <KisView.h>


class KisViewManager;
class KisCanvasResourceProvider;
class KisPaletteChooser;
class KisPaletteModel;
class KisPaletteEditor;
class KisPopupButton;
class PaletteGrid;
class RecentStrip;
class QToolButton;

/**
 * The palette: its swatches, filling the panel's width, and its name.
 *
 * - click a swatch to paint with it; the swatch of the current color has a ring
 * - "+" after the last swatch adds the current color, no questions asked
 * - drag a swatch onto another place to move it there (two swap), off the
 *   panel to remove it; undo is next to the name while there is something to undo
 * - right click or long press a swatch for its card: use the current color
 *   there, edit its name and color, remove it
 * - click the name for the other palettes, right click it to edit, lock,
 *   save or export this one
 *
 * It can be made as short as one row (the wheel scrolls a row at a time):
 * one or two rows of a palette under the brushes is a fine place for it.
 */
class PaletteDockerDock : public QDockWidget, public KisMainwindowObserver, public KoResourceServerObserver<KoColorSet>
{
    Q_OBJECT
public:
    PaletteDockerDock();
    ~PaletteDockerDock() override;

public: // QDockWidget
    void setCanvas(KoCanvasBase *canvas) override;
    void unsetCanvas() override;

public: // KisMainWindowObserver
    void setViewManager(KisViewManager* kisview) override;

public: //KoResourceServerObserver
    void unsetResourceServer() override;
    void resourceAdded(QSharedPointer<KoColorSet> resource) override;
    void removingResource(QSharedPointer<KoColorSet> resource) override;
    void resourceChanged(QSharedPointer<KoColorSet> resource) override;

private Q_SLOTS:
    void slotAddPalette();
    void slotRemovePalette(KoColorSetSP );
    void slotImportPalette();
    void slotExportPalette(KoColorSetSP );
    void slotSetColorSet(KoColorSetSP colorSet);
    void slotFGColorResourceChanged(const KoColor& color);
    /// A color painted with: first among the recent ones
    void slotColorUsed(const KoColor &color);
    void slotStoragesChanged(const QString &location);
    void updateHeader();

private:
    bool canEdit() const;
    bool isGlobal() const;
    KisSwatch newSwatch(const KoColor &color) const;
    void pickSwatch(const QModelIndex &index);
    void addCurrentColor(const QModelIndex &emptySlot);
    /// In \p emptySlot, or a new row under the first group when it is invalid
    void addColor(const KoColor &color, const QModelIndex &emptySlot);
    void updateRecentStrip();
    void pickRecent(int index);
    void showRecentCard(int index, QWidget *anchor);
    void moveSwatch(const QModelIndex &from, const QModelIndex &to);
    void removeSwatch(const QModelIndex &index);
    void showSwatchCard(const QModelIndex &index, QWidget *anchor);
    void showPaletteCard();

private /* member variables */:
    KisPaletteModel *m_model;
    KisPaletteChooser *m_paletteChooser;
    PaletteGrid *m_grid {nullptr};
    RecentStrip *m_recentStrip {nullptr};
    /// The colors last painted with, newest first
    QVector<KoColor> m_recent;
    /// What the row shows: the active layer's colors, or the recent ones
    QVector<KoColor> m_shownRecent;
    QMetaObject::Connection m_logConnection;
    QTimer m_recentUpdate;
    KisPopupButton *m_paletteButton {nullptr};
    QToolButton *m_undo {nullptr};
    QToolButton *m_redo {nullptr};

    QPointer<KisViewManager> m_view;
    KisCanvasResourceProvider *m_resourceProvider {nullptr};

    KoResourceServer<KoColorSet> * const m_rServer;

    QPointer<KisDocument> m_activeDocument;
    QSharedPointer<KoColorSet> m_currentColorSet;
    QScopedPointer<KisPaletteEditor> m_paletteEditor;
    /// To the current palette's undo stack
    QList<QMetaObject::Connection> m_paletteConnections;

    bool m_colorSelfUpdate {false};
};


#endif
