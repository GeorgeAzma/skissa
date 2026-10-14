/*
 *  widgets/kis_cmb_composite.h - part of KImageShop/Krayon/Krita
 *
 *  SPDX-FileCopyrightText: 2004 Boudewijn Rempt (boud@valdyas.org)
 *  SPDX-FileCopyrightText: 2011 Silvio Heinrich <plassy@web.de>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_COMPOSITEOP_WIDGETS_H_
#define KIS_COMPOSITEOP_WIDGETS_H_

#include <KisSqueezedComboBox.h>
#include <kritaui_export.h>
#include "kis_categorized_list_view.h"
#include "KisBlendModePicker.h"

#include <functional>

class KoID;
class KoColorSpace;
class KisSortedCompositeOpListModel;
class KisAction;
class KisActionManager;

class KRITAUI_EXPORT KisCompositeOpListWidget: public KisCategorizedListView
{
    Q_OBJECT
public:
     KisCompositeOpListWidget(QWidget* parent = 0);
     ~KisCompositeOpListWidget() override;

    KoID selectedCompositeOp() const;

public Q_SLOTS:
    void setCompositeOp(const KoID &id);

private:
    KisSortedCompositeOpListModel *m_model;
};


class KRITAUI_EXPORT KisCompositeOpComboBox: public KisSqueezedComboBox
{
    Q_OBJECT
public:
     KisCompositeOpComboBox(QWidget* parent = 0);
     KisCompositeOpComboBox(bool limitToLayerStyles, QWidget* parent = 0);
    ~KisCompositeOpComboBox() override;

    void hidePopup() override;

    /// The blend mode picker instead of the plain list, but for layer styles
    void showPopup() override;

    void validate(const KoColorSpace *cs);
    void selectCompositeOp(const KoID &op);
    KoID selectedCompositeOp() const;

    void connectBlendmodeActions(KisActionManager *manager);

    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

    /// What the picker's tiles show the modes doing, made when it opens; without, small swatches
    void setPreviewPictures(std::function<KisBlendModePicker::Pictures()> pictures);

protected:
    /// A drag sideways steps through the modes, previewing them; a click opens the picker
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    void hideEvent(QHideEvent *e) override;

public:
Q_SIGNALS:
    /// A mode to preview, e.g. on the active layer; empty when the preview is over (a pick then comes via activated())
    void sigPreviewBlendMode(const QString &id);

private Q_SLOTS:
    void slotCategoryToggled(const QModelIndex& index, bool toggled);
    void slotEntryChecked(const QModelIndex& index);

    void slotNextBlendingMode();
    void slotPreviousBlendingMode();
    void slotNormal();
    void slotDissolve();
    void slotBehind();
    void slotClear();
    void slotDarken();
    void slotMultiply();
    void slotColorBurn();
    void slotLinearBurn();
    void slotLighten();
    void slotScreen();
    void slotColorDodge();
    void slotLinearDodge();
    void slotOverlay();
    void slotHardOverlay();
    void slotSoftLight();
    void slotHardLight();
    void slotVividLight();
    void slotLinearLight();
    void slotPinLight();
    void slotHardMix();
    void slotDifference();
    void slotExclusion();
    void slotHue();
    void slotSaturation();
    void slotColor();
    void slotLuminosity();

private:
    void selectNeighbouringBlendMode(bool down);
    /// Whether the mode can be chosen in the current color space
    bool isUsable(const QString &id) const;
    /// Chooses it as the user would in the list
    void pick(const KoID &op);
    /// Ends a press; a scrub picks the mode it reached
    void endScrub();

private:
    KisSortedCompositeOpListModel *m_model;
    KisCategorizedListView *m_view;
    bool m_allowToHidePopup;
    bool m_limitToLayerStyles {false};
    std::function<KisBlendModePicker::Pictures()> m_pictures;
    /// A click or a scrub, until released
    bool m_pressed {false};
    bool m_scrubbing {false};
    QPoint m_pressPos;
    int m_scrubStart {-1};
    QString m_scrubId;
};

class KRITAUI_EXPORT KisLayerStyleCompositeOpComboBox: public KisCompositeOpComboBox
{
public:
    KisLayerStyleCompositeOpComboBox(QWidget* parent = 0);
};

#endif // KIS_COMPOSITEOP_WIDGETS_H_
