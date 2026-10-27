/* This file is part of the KDE project
 *
 * SPDX-FileCopyrightText: 2010 Justin Noel <justin@ics.com>
 * SPDX-FileCopyrightText: 2010 Cyrille Berger <cberger@cberger.net>
 * SPDX-FileCopyrightText: 2015 Moritz Molch <kde@moritzmolch.de>
 * SPDX-FileCopyrightText: 2021 Deif Lou <ginoba@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef KISSLIDERSPINBOXPRIVATE_H
#define KISSLIDERSPINBOXPRIVATE_H

#include <QObject>
#include <QLineEdit>
#include <QStyleOptionSpinBox>
#include <QPainter>
#include <QEvent>
#include <QInputMethodQueryEvent>
#include <QApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QStyleHints>
#include <QMenu>
#include <QVariantAnimation>
#include <QPointer>

#include <cmath>
#include <algorithm>
#include <utility>
#include <type_traits>

#include <ksharedconfig.h>
#include <kconfiggroup.h>
#include <kis_cursor.h>
#include <kis_num_parser.h>
#include <klocalizedstring.h>
#include <kis_painting_tweaks.h>
#include <kis_int_parse_spin_box.h>
#include <kis_double_parse_spin_box.h>
#include <kis_algebra_2d.h>
#include <kis_signal_compressor_with_param.h>
#include "KisModernStyle.h"

template <typename SpinBoxTypeTP, typename BaseSpinBoxTypeTP>
class Q_DECL_HIDDEN KisSliderSpinBoxPrivate : public QObject
{
public:
    using SpinBoxType = SpinBoxTypeTP;
    using BaseSpinBoxType = BaseSpinBoxTypeTP;
    using ValueType = decltype(std::declval<SpinBoxType>().value());

    enum ValueUpdateMode
    {
        ValueUpdateMode_NoChange,
        ValueUpdateMode_UseLastValidValue,
        ValueUpdateMode_UseValueBeforeEditing
    };

    KisSliderSpinBoxPrivate(SpinBoxType *q)
        : m_q(q)
        , m_lineEdit(m_q->lineEdit())
        , m_startEditingSignalProxy(std::bind(&KisSliderSpinBoxPrivate::startEditing, this))
    {
        m_q->installEventFilter(this);

        m_lineEdit->setReadOnly(true);
        m_lineEdit->setAlignment(Qt::AlignCenter);
        m_lineEdit->setAutoFillBackground(false);
        m_lineEdit->setCursor(KisCursor::splitHCursor());
        m_lineEdit->installEventFilter(this);

        m_widgetRangeToggle = new QWidget(m_q);
        m_widgetRangeToggle->hide();
        m_widgetRangeToggle->installEventFilter(this);

        m_sliderAnimation.setStartValue(0.0);
        m_sliderAnimation.setEndValue(1.0);
        m_sliderAnimation.setEasingCurve(KisModernStyle::motionCurve());
        connect(&m_sliderAnimation, &QVariantAnimation::valueChanged, m_lineEdit, QOverload<>::of(&QLineEdit::update));
        connect(&m_sliderAnimation, &QVariantAnimation::valueChanged, m_widgetRangeToggle, QOverload<>::of(&QLineEdit::update));
        m_quietHover.setStartValue(0.0);
        m_quietHover.setEndValue(0.0);
        m_quietHover.setEasingCurve(KisModernStyle::motionCurve());
        connect(&m_quietHover, &QVariantAnimation::valueChanged, m_lineEdit, QOverload<>::of(&QLineEdit::update));

        m_rangeToggleHoverAnimation.setStartValue(0.0);
        m_rangeToggleHoverAnimation.setEndValue(1.0);
        m_rangeToggleHoverAnimation.setEasingCurve(KisModernStyle::motionCurve());
        connect(&m_rangeToggleHoverAnimation, &QVariantAnimation::valueChanged, m_widgetRangeToggle, QOverload<>::of(&QLineEdit::update));
    }

    void startEditing()
    {
        m_pressedOnQuietText = false;
        if (isEditModeActive()) {
            return;
        }
        // Store the current value
        m_valueBeforeEditing = m_q->value();
        m_lineEdit->setReadOnly(false);
        m_q->selectAll();
        m_lineEdit->setFocus(Qt::OtherFocusReason);
        m_lineEdit->setCursor(KisCursor::ibeamCursor());
        updateQuietHover();
    }

    void endEditing(ValueUpdateMode updateMode = ValueUpdateMode_UseLastValidValue)
    {
        if (!isEditModeActive()) {
            return;
        }
        if (updateMode == ValueUpdateMode_UseLastValidValue) {
            // without keyboard tracking the typed text isn't the value yet; Enter, eaten here, would have made it so
            m_q->interpretText();
            setValue(m_q->value(), false, false, true);
        } else if (updateMode == ValueUpdateMode_UseValueBeforeEditing) {
            setValue(m_valueBeforeEditing, false, false, true);
        }
        // Restore palette colors
        QPalette pal = m_lineEdit->palette();
        pal.setBrush(QPalette::Text, m_q->palette().text());
        m_lineEdit->setPalette(pal);
        m_rightClickCounter = 0;
        m_lineEdit->setReadOnly(true);
        m_lineEdit->setCursor(KisCursor::splitHCursor());
        m_lineEdit->update();
        m_q->update();
        // what is under it too: over a see-through panel the editing's veil otherwise stays behind
        if (QWidget *under = m_q->parentWidget()) under->update(m_q->geometry());
        updateQuietHover();
    }
    
    bool isEditModeActive() const
    {
        return !m_lineEdit->isReadOnly();
    }

    // Compute a new value as a function of the x and y coordinates relative
    // to the lineedit, and some combination of modifiers
    ValueType valueForPoint(const QPoint &p, Qt::KeyboardModifiers modifiers) const
    {
        const QRectF rect(m_lineEdit->rect());
        const QPointF center(
            static_cast<double>(
                m_lastMousePressPosition.x() + (m_useRelativeDragging ? m_relativeDraggingOffset : 0)
             ),
            rect.height() / 2.0
        );
        const bool useSoftRange = isSoftRangeValid() && (m_softRangeViewMode == SoftRangeViewMode_AlwaysShowSoftRange || m_isSoftRangeActive);
        const double minimum = static_cast<double>(useSoftRange ? m_softMinimum : m_q->minimum());
        const double maximum = static_cast<double>(useSoftRange ? m_softMaximum : m_q->maximum());
        const double rangeSize = maximum - minimum;
        // Get the distance relative to the line edit center and transformed
        // so that it starts counting 32px away from the widget. If the position
        // is inside the widget or that 32px area the distance will be 0 so that
        // the value change will be the same near the widget
        const double distanceY =
            std::max(
                0.0,
                std::abs(static_cast<double>(p.y()) - center.y()) - center.y() - constantDraggingMargin
            );
        // Get the scale
        double scale;
        if (modifiers & Qt::ShiftModifier) {
            // If the shift key is pressed we scale the distanceY value to make the scale
            // have a stronger effect and also offset it so that the minimum
            // scale will be 5x (1x + 4x).
            // function
            scale = (rect.width() + 2.0 * distanceY * 10.0) / rect.width() + 4.0;
        } else {
            // Get the scale as a function of the vertical position
            scale = (rect.width() + 2.0 * distanceY * 2.0) / rect.width();
        }
        // Scale the horizontal coordinates around where we first clicked 
        // as a function of the y coordinate
        const double scaledRectLeft = (0.0 - center.x()) * scale + center.x();
        const double scaledRectRight = (rect.width() - center.x()) * scale + center.x();
        // Map the current horizontal position to the new rect
        const double scaledRectWidth = scaledRectRight - scaledRectLeft;
        const double posX = static_cast<double>(p.x()) - scaledRectLeft;
        // Normalize
        const double normalizedPosX = qBound(0.0, posX / scaledRectWidth, 1.0);
        // Final value
        const double normalizedValue = std::pow(normalizedPosX, m_exponentRatio);
        double value = normalizedValue * rangeSize + minimum;
        // If key CTRL is pressed, round to the closest step.
        if (modifiers & Qt::ControlModifier) {
            value = std::round(value / m_fastSliderStep) * m_fastSliderStep;
        }
        //Return the value
        if (std::is_same<ValueType, double>::value) {
            return value;
        } else {
            return static_cast<ValueType>(std::round(value));
        }
    }

    QPoint pointForValue(ValueType value) const
    {
        ValueType min, max;
        if (isSoftRangeValid()) {
            if (m_softRangeViewMode == SoftRangeViewMode_ShowBothRanges) {
                if (m_isSoftRangeActive) {
                    min = softMinimum();
                    max = softMaximum();
                } else {
                    min = m_q->minimum();
                    max = m_q->maximum();
                }
            } else {
                min = softMinimum();
                max = softMaximum();
            }
        } else {
            min = m_q->minimum();
            max = m_q->maximum();
        }
        return QPoint(static_cast<int>(qRound(computeSliderWidth(min, max, value))), 0);
    }

    // Custom setValue that allows disabling signal emission
    void setValue(ValueType newValue,
                  bool blockSignals = false,
                  bool emitSignalsEvenWhenValueNotChanged = false,
                  bool overwriteExpression = false)
    {
        if (blockSignals) {
            m_q->blockSignals(true);
            m_q->BaseSpinBoxType::setValue(newValue, overwriteExpression);
            m_q->blockSignals(false);
        } else {
            ValueType v = m_q->value();
            m_q->BaseSpinBoxType::setValue(newValue, overwriteExpression);
            if (v == m_q->value() && emitSignalsEvenWhenValueNotChanged) {
                emitSignals();
            }
        }
        if (!m_q->hasFocus()) {
            endEditing(ValueUpdateMode_NoChange);
        }
    }

    void resetRangeMode()
    {
        if (isSoftRangeValid() && m_softRangeViewMode == SoftRangeViewMode_ShowBothRanges) {
            if (m_isSoftRangeActive) {
                makeSoftRangeActive();
            } else {
                makeHardRangeActive();
            }
            updateWidgetRangeToggleTooltip();
            m_widgetRangeToggle->show();
        } else {
            m_sliderAnimation.stop();
            m_widgetRangeToggle->hide();
        }
        qResizeEvent(nullptr);
    }

    template <typename U = SpinBoxTypeTP, typename = typename std::enable_if<std::is_same<ValueType, int>::value, U>::type>
    void setRange(int newMinimum, int newMaximum, bool computeNewFastSliderStep)
    {
        m_q->BaseSpinBoxType::setRange(newMinimum, newMaximum);
        if (computeNewFastSliderStep) {
            // Behavior taken from the old slider spinbox. Kind of arbitrary
            m_fastSliderStep = (m_q->maximum() - m_q->minimum()) / 20;
            if (m_fastSliderStep == 0) {
                m_fastSliderStep = 1;
            }
        }
        m_softMinimum = qBound(m_q->minimum(), m_softMinimum, m_q->maximum());
        m_softMaximum = qBound(m_q->minimum(), m_softMaximum, m_q->maximum());
        resetRangeMode();
        m_q->update();
    }

    template <typename U = SpinBoxTypeTP, typename = typename std::enable_if<std::is_same<ValueType, double>::value, U>::type>
    void setRange(double newMinimum, double newMaximum, int newNumberOfDecimals, bool computeNewFastSliderStep)
    {
        m_q->setDecimals(newNumberOfDecimals);
        m_q->BaseSpinBoxType::setRange(newMinimum, newMaximum);
        if (computeNewFastSliderStep) {
            // Behavior taken from the old slider. Kind of arbitrary
            const double rangeSize = m_q->maximum() - m_q->minimum();
            if (rangeSize >= 2.0 || newNumberOfDecimals <= 0) {
                m_fastSliderStep = 1.0;
            } else if (newNumberOfDecimals == 1) {
                m_fastSliderStep = rangeSize / 10.0;
            } else {
                m_fastSliderStep = rangeSize / 20.0;
            }
        }
        m_softMinimum = qBound(m_q->minimum(), m_softMinimum, m_q->maximum());
        m_softMaximum = qBound(m_q->minimum(), m_softMaximum, m_q->maximum());
        resetRangeMode();
        m_lineEdit->update();
    }

    void setBlockUpdateSignalOnDrag(bool newBlockUpdateSignalOnDrag)
    {
        m_blockUpdateSignalOnDrag = newBlockUpdateSignalOnDrag;
    }

    void setFastSliderStep(ValueType newFastSliderStep)
    {
        m_fastSliderStep = newFastSliderStep;
    }

    // Set the soft range. Set newSoftMinimum = newSoftMaximum to signal that
    // the soft range must not be used
    void setSoftRange(ValueType newSoftMinimum, ValueType newSoftMaximum)
    {
        if ((newSoftMinimum != newSoftMaximum) &&
            (newSoftMinimum > newSoftMaximum || newSoftMinimum < m_q->minimum() || newSoftMaximum > m_q->maximum())) {
            return;
        }
        m_softMinimum = newSoftMinimum;
        m_softMaximum = newSoftMaximum;
        resetRangeMode();
        m_lineEdit->update();
    }

    bool isSoftRangeValid() const
    {
        return m_softMaximum > m_softMinimum;
    }

    ValueType fastSliderStep() const
    {
        return m_fastSliderStep;
    }

    ValueType softMinimum() const
    {
        return m_softMinimum;
    }

    ValueType softMaximum() const
    {
        return m_softMaximum;
    }

    bool isDragging() const
    {
        return m_isDragging;
    }
    
    void makeSoftRangeActive()
    {
        m_sliderAnimation.stop();
        m_isSoftRangeActive = true;
        // scale the animation duration in case the animation is in the middle
        const int animationDuration =
            static_cast<int>(std::round(m_sliderAnimation.currentValue().toReal() * fullAnimationDuration));
        m_sliderAnimation.setStartValue(m_sliderAnimation.currentValue());
        m_sliderAnimation.setEndValue(0.0);
        m_sliderAnimation.setDuration(animationDuration);
        m_sliderAnimation.start();
    }

    void makeHardRangeActive()
    {
        m_sliderAnimation.stop();
        m_isSoftRangeActive = false;
        // scale the duration in case the animation is in the middle
        const int animationDuration =
            static_cast<int>(std::round((1.0 - m_sliderAnimation.currentValue().toReal()) * fullAnimationDuration));
        m_sliderAnimation.setStartValue(m_sliderAnimation.currentValue());
        m_sliderAnimation.setEndValue(1.0);
        m_sliderAnimation.setDuration(animationDuration);
        m_sliderAnimation.start();
    }

    void setExponentRatio(double newExponentRatio)
    {
        m_exponentRatio = newExponentRatio;
        m_lineEdit->update();
    }

    void updateWidgetRangeToggleTooltip()
    {
        m_widgetRangeToggle->setToolTip(
            i18nc(
                "@info:tooltip toggle between soft and hard range in the slider spin box",
                "Range: %3 to %4, or %1 to %2",
                QString::number(m_q->minimum()),
                QString::number(m_q->maximum()),
                QString::number(m_softMinimum),
                QString::number(m_softMaximum)
            )
        );
    }

    QSize sizeHint() const
    {
        QSize hint = m_q->BaseSpinBoxType::sizeHint();
        return
            (isSoftRangeValid() && m_softRangeViewMode == SoftRangeViewMode_ShowBothRanges)
            ? QSize(hint.width() + widthOfRangeModeToggle, hint.height())
            : hint;
    }

    QSize minimumSizeHint() const
    {
        QSize hint = m_q->BaseSpinBoxType::minimumSizeHint();
        return
            (isSoftRangeValid() && m_softRangeViewMode == SoftRangeViewMode_ShowBothRanges)
            ? QSize(hint.width() + widthOfRangeModeToggle, hint.height())
            : hint;
    }

    void emitSignals() const
    {
#if QT_VERSION >= QT_VERSION_CHECK(5,14,0)
        Q_EMIT m_q->textChanged(m_q->text());
#else
        Q_EMIT m_q->valueChanged(m_q->text());
#endif
        Q_EMIT m_q->valueChanged(m_q->value());
    }

    bool qResizeEvent(QResizeEvent*)
    {
        // When resizing the spinbox, perform style specific positioning
        // of the lineedit
        
        // Get the default rect for the lineedit widget
        QStyleOptionSpinBox spinBoxOptions;
        m_q->initStyleOption(&spinBoxOptions);
        QRect rect = m_q->style()->subControlRect(QStyle::CC_SpinBox, &spinBoxOptions, QStyle::SC_SpinBoxEditField);
        // Offset the rect to make it take all the available space inside the
        // spinbox, without overlapping the buttons
        QString style = qApp->property(currentUnderlyingStyleNameProperty).toString().toLower();
        if (style == "breeze") {
            rect.adjust(-4, -4, 0, 4);
        } else if (style == "fusion") {
            rect.adjust(-2, -1, 2, 1);
        }
        // Set the rect
        if (isSoftRangeValid() && m_softRangeViewMode == SoftRangeViewMode_ShowBothRanges) {
            m_lineEdit->setGeometry(rect.adjusted(0, 0, -widthOfRangeModeToggle, 0));
            m_widgetRangeToggle->setGeometry(rect.adjusted(rect.width() - widthOfRangeModeToggle, 0, 0, 0));
        } else {
            m_lineEdit->setGeometry(rect);
        }
        if (isQuiet()) {
            // edited where the number is shown
            m_lineEdit->setAlignment(isQuietWide() ? (Qt::AlignRight | Qt::AlignVCenter) : Qt::AlignCenter);
        }

        return true;
    }

    bool qFocusOutEvent(QFocusEvent*)
    {
        // If the focus is lost then the edition stops, unless the focus
        // was lost because the menu was shown
        if (m_focusLostDueToMenu) {
            m_focusLostDueToMenu = false;
        } else {
            if (m_q->isLastValid()) {
                endEditing();
            }
        }
        return false;
    }

    bool qMousePressEvent(QMouseEvent*)
    {
        // If we click in any part of the spinbox outside the lineedit
        // then the edition stops
        endEditing();
        return false;
    }

    bool qKeyPressEvent(QKeyEvent *e)
    {
        switch (e->key()) {
            // If the lineedit is not in edition mode, make the left and right
            // keys have the same effect as the down and up keys. This replicates
            // the behaviour of the old slider spinbox
            case Qt::Key_Right:
                if (!isEditModeActive()) {
                    qApp->postEvent(m_q, new QKeyEvent(QEvent::KeyPress, Qt::Key_Up, e->modifiers()));
                    return true;
                }
                break;
            case Qt::Key_Left:
                if (!isEditModeActive()) {
                    qApp->postEvent(m_q, new QKeyEvent(QEvent::KeyPress, Qt::Key_Down, e->modifiers()));
                    return true;
                }
                break;
            // The enter key can be used to enter the edition mode if the
            // lineedit is not in it or to commit the entered value and leave
            // the edition mode if we are in it
            case Qt::Key_Enter:
            case Qt::Key_Return:
                if (!e->isAutoRepeat()) {
                    if (!isEditModeActive()) {
                        startEditing();
                    } else {
                        if (m_q->isLastValid()) {
                            endEditing();
                        } else {
                            return false;
                        }
                    }
                }
                return true;
            // The escape key can be used to leave the edition mode rejecting
            // the written value 
            case Qt::Key_Escape:
                if (isEditModeActive()) {
                    endEditing(ValueUpdateMode_UseValueBeforeEditing);
                    return true;
                }
                break;
            // If we press a number key when in slider mode, then start the edit
            // mode and resend the key event so that the key is processed again
            // in edit mode. Since entering edit mode selects all text, the new
            // event will replace the text by the new key text
            default:
                if (!isEditModeActive() && e->key() >= Qt::Key_0 && e->key() <= Qt::Key_9) {
                    startEditing();
                    qApp->postEvent(m_q, new QKeyEvent(QEvent::KeyPress, e->key(), e->modifiers(), e->text(), e->isAutoRepeat()));
                    return true;
                }
                break;
        }
        return false;
    }

    bool qContextMenuEvent(QContextMenuEvent *e)
    {
        // Shows a menu. Code inspired by the QAbstractSpinBox
        // contextMenuEvent function
        
        // Return if we are in slider mode and the mouse is in the line edit
        if (!isEditModeActive() && m_lineEdit->rect().contains(e->pos())) {
            return true;
        }
        // Create the menu
        QPointer<QMenu> menu;
        // If we are in edit mode, then add the line edit
        // actions
        if (isEditModeActive()) {
            menu = m_lineEdit->createStandardContextMenu();
            m_focusLostDueToMenu = true;
        } else {
            menu = new QMenu;
        }
        if (!menu) {
            return true;
        }
        // Override select all action
        QAction *selectAllAction = nullptr;
        if (isEditModeActive()) {
            selectAllAction = new QAction(i18nc("Menu action to select all text in the slider spin box", "&Select All"), menu);
#if QT_CONFIG(shortcut)
            selectAllAction->setShortcut(QKeySequence::SelectAll);
#endif
            menu->removeAction(menu->actions().last());
            menu->addAction(selectAllAction);
            menu->addSeparator();
        }
        // Add step up and step down actions
        const uint stepEnabled = m_q->stepEnabled();
        QAction *stepUpAction = menu->addAction(i18nc("Menu action to step up in the slider spin box", "&Step up"));
        stepUpAction->setEnabled(stepEnabled & SpinBoxType::StepUpEnabled);
        QAction *stepDown = menu->addAction(i18nc("Menu action to step down in the slider spin box", "Step &down"));
        stepDown->setEnabled(stepEnabled & SpinBoxType::StepDownEnabled);
        menu->addSeparator();
        // This code is taken from QAbstractSpinBox. Use a QPointer in case the
        // spin box is destroyed while the menu is shown??
        const QPointer<SpinBoxType> spinbox = m_q;
        const QPoint pos =
            (e->reason() == QContextMenuEvent::Mouse)
            ? e->globalPos()
            : m_q->mapToGlobal(QPoint(e->pos().x(), 0)) + QPoint(m_q->width() / 2, m_q->height() / 2);
        const QAction *action = menu->exec(pos);
        delete static_cast<QMenu *>(menu);
        if (spinbox && action) {
            if (action == stepUpAction) {
                m_q->stepBy(static_cast<ValueType>(1));
            } else if (action == stepDown) {
                m_q->stepBy(static_cast<ValueType>(-1));
            } else if (action == selectAllAction) {
                m_q->selectAll();
            }
        }
        e->accept();
        return true;
    }

    bool blockInputMethodQuery(QInputMethodQueryEvent *e)
    {
        // On Android, pressing on a text input shows text selection handles,
        // see QAndroidInputContext::updateSelectionHandles. We don't want that
        // if a slider is not in edit mode, so we respond with "not enabled".
        if (e->queries().testFlag(Qt::ImEnabled) && !isEditModeActive()) {
            e->setValue(Qt::ImEnabled, false);
            e->accept();
            return true;
        } else {
            return false;
        }
    }

    // Generic "style aware" helper function to draw a rect
    void paintSliderRect(QPainter &painter, const QRectF &rect, const QBrush &brush)
    {
        painter.save();
        painter.setBrush(brush);
        painter.setPen(Qt::NoPen);
        if (qobject_cast<const KisModernStyle *>(m_q->style())) {
            const qreal radius = KisModernStyle::cornerRadius - 1;
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.drawPath(KisModernStyle::squircle(rect, radius));
        } else if (qApp->property(currentUnderlyingStyleNameProperty).toString().toLower() == "fusion") {
            painter.drawRoundedRect(rect, 1, 1);
        } else {
            painter.drawRoundedRect(rect, 0, 0);
        }
        painter.restore();
    }

    void paintSliderText(QPainter &painter, const QString &text, const QRectF &rect, const QRectF &clipRect, const QColor &color, const QTextOption &textOption)
    {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(color);
        painter.setClipping(true);
        painter.setClipRect(clipRect);
        painter.drawText(rect, text, textOption);
        painter.setClipping(false);
    };

    /// The fill is neutral: one text color reads over it and the background
    void paintGenericSliderText(QPainter &painter, const QString &text, const QRectF &rect)
    {
        QTextOption textOption(Qt::AlignAbsolute | Qt::AlignHCenter | Qt::AlignVCenter);
        textOption.setWrapMode(QTextOption::NoWrap);
        paintSliderText(painter, text, rect, rect, m_lineEdit->palette().text().color(), textOption);
    };

    /// A neutral fill: a value, not a selection
    QColor sliderFillColor() const
    {
        return KisPaintingTweaks::blendColors(m_q->palette().text().color(), m_q->palette().base().color(), 0.24);
    }

    /// Quiet (KisModernStyle::quietFieldProperty, frameless): a line with the number by it; hovered, it thickens and grows a knob
    bool isQuiet() const
    {
        return m_q->property(KisModernStyle::quietFieldProperty).toBool() && !m_q->hasFrame()
            && !(isSoftRangeValid() && m_softRangeViewMode == SoftRangeViewMode_ShowBothRanges);
    }

    /// The widest value's width (at least a few digits): the line neither jumps nor ends unevenly in a column
    qreal quietTextWidth() const
    {
        QString widest;
        if constexpr (std::is_same<ValueType, double>::value) {
            widest = QString::number(m_q->maximum(), 'f', m_q->decimals());
        } else {
            widest = QString::number(m_q->maximum());
        }
        const QFontMetrics metrics = m_lineEdit->fontMetrics();
        return std::max({metrics.horizontalAdvance(m_q->text()), metrics.horizontalAdvance(m_q->prefix() + widest + m_q->suffix()),
                     metrics.horizontalAdvance(QStringLiteral("0000.00"))});
    }

    /// Wide: the line beside the number; else under it
    bool isQuietWide() const
    {
        return m_lineEdit->width() >= 3 * quietTextWidth() + 20;
    }

    QRectF quietTrack(qreal thickness) const
    {
        // inset by the hovered knob's radius, so it is never cut off at either end
        const QRectF rect = m_lineEdit->rect();
        if (isQuietWide()) {
            const qreal knob = 9;
            return QRectF(rect.left() + knob, rect.center().y() - thickness / 2, rect.width() - quietTextWidth() - 14 - 2 * knob, thickness);
        }
        const qreal knob = 5;
        return QRectF(rect.left() + knob, rect.bottom() - thickness - 3, rect.width() - 2 * knob, thickness);
    }

    /// A position on the line mapped to the whole field, where values are computed
    QPoint toSliderSpace(const QPoint &p) const
    {
        if (!isQuiet()) return p;
        const QRectF track = quietTrack(0);
        return QPoint(qRound((p.x() - track.left()) * m_lineEdit->width() / qMax(1.0, track.width())), p.y());
    }

    void updateQuietHover()
    {
        if (!isQuiet()) return;
        const qreal target = (m_q->underMouse() || m_isDragging || isEditModeActive()) ? 1.0 : 0.0;
        const qreal from = m_quietHover.currentValue().toReal();
        if (m_quietHover.endValue().toReal() == target && (m_quietHover.state() == QAbstractAnimation::Running || from == target)) return;
        m_quietHover.stop();
        m_quietHover.setStartValue(from);
        m_quietHover.setEndValue(target);
        m_quietHover.setDuration(qMax(1, qRound(KisModernStyle::changeDuration * qAbs(target - from))));
        m_quietHover.start();
    }

    void paintQuietSlider(QPainter &painter, const QString &text, double sliderWidth)
    {
        const QRectF rect = m_lineEdit->rect();
        const QColor ink = m_lineEdit->palette().text().color();
        const qreal hover = m_quietHover.currentValue().toReal();
        const bool wide = isQuietWide();
        const qreal thickness = 2.0 + hover * (wide ? 2.0 : 1.0);
        const QRectF track = quietTrack(thickness);
        const qreal end = track.left() + track.width() * sliderWidth / qMax(1.0, rect.width());
        QColor rail = ink;
        rail.setAlphaF(0.14 + 0.04 * hover);
        QColor fill = ink;
        fill.setAlphaF(0.55 + 0.25 * hover);
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.setBrush(rail);
        painter.drawRoundedRect(track, thickness / 2, thickness / 2);
        painter.setBrush(fill);
        painter.drawRoundedRect(QRectF(track.left(), track.top(), qMax(thickness, end - track.left()), thickness), thickness / 2, thickness / 2);
        if (hover > 0.0) {
            const qreal radius = thickness / 2 + hover * (wide ? 4.0 : 2.0);
            painter.setBrush(ink);
            painter.drawEllipse(QPointF(qMax(track.left(), end), track.center().y()), radius, radius);
        }
        painter.restore();
        if (text.isNull()) return; // editing: the line edit draws it
        QTextOption textOption(Qt::AlignAbsolute | (wide ? Qt::AlignRight : Qt::AlignHCenter) | Qt::AlignVCenter);
        textOption.setWrapMode(QTextOption::NoWrap);
        paintSliderText(painter, text, wide ? rect.adjusted(0, 0, -2, 0) : rect.adjusted(0, -1, 0, -1), rect, ink, textOption);
    }

    void paintSlider(QPainter &painter, const QString &text, double slider01Width, double slider02Width = -1.0)
    {
        const QRectF rect = m_lineEdit->rect();
        const QColor highlightColor = sliderFillColor();
        if (isQuiet()) {
            paintQuietSlider(painter, text, slider01Width);
            return;
        }
        if (slider02Width < 0.0) {
            const QRectF sliderRect = rect.adjusted(0, 0, -(rect.width() - slider01Width), 0);
            paintSliderRect(painter, sliderRect, highlightColor);
            if (!text.isNull()) {
                paintGenericSliderText(painter, text, rect);
            }
        } else {
            static constexpr double heightOfCollapsedSliderPlusSpace = heightOfCollapsedSlider + heightOfSpaceBetweenSliders;
            const double heightBetween = rect.height() - 2.0 * heightOfCollapsedSlider - heightOfSpaceBetweenSliders;
            const double animationPos = m_sliderAnimation.currentValue().toReal();
            const double a = heightOfCollapsedSliderPlusSpace;
            const double b = heightOfCollapsedSliderPlusSpace + heightBetween;
            // Paint background text
            QTextOption textOption(Qt::AlignAbsolute | Qt::AlignHCenter | Qt::AlignVCenter);
            textOption.setWrapMode(QTextOption::NoWrap);
            paintSliderText(painter, text, rect, rect, m_lineEdit->palette().text().color(), textOption);
            // Paint soft range slider
            const QColor softSliderColor = KisPaintingTweaks::blendColors(m_q->palette().text().color(), m_q->palette().base().color(), 0.1);
            const double softSliderAdjustment = -KisAlgebra2D::lerp(a, b, animationPos);
            paintSliderRect(
                painter,
                rect.adjusted(0, 0, -(rect.width() - slider02Width), softSliderAdjustment),
                softSliderColor
            );
            // Paint hard range slider
            const double hardSliderAdjustment = KisAlgebra2D::lerp(a, b, 1.0 - animationPos);
            paintSliderRect(
                painter,
                rect.adjusted(0, hardSliderAdjustment, -(rect.width() - slider01Width), 0),
                highlightColor
            );
            // Paint text in the sliders
            paintSliderText(
                painter, text, rect,
                rect.adjusted(0, 0, -(rect.width() - slider02Width), softSliderAdjustment),
                m_lineEdit->palette().text().color(),
                textOption
            );
            paintSliderText(
                painter, text, rect,
                rect.adjusted(0, hardSliderAdjustment, -(rect.width() - slider01Width), 0),
                m_lineEdit->palette().text().color(),
                textOption
            );
        }
    }

    double computeSliderWidth(double min, double max, double value) const
    {
        const double rangeSize = max - min;
        const double localPosition = value - min;
        const double normalizedValue = std::pow(localPosition / rangeSize, 1.0 / m_exponentRatio);
        const double width = static_cast<double>(m_lineEdit->width());
        return qBound(0.0, std::round(normalizedValue * width), width);
    }

    bool lineEditPaintEvent(QPaintEvent*)
    {
        QPainter painter(m_lineEdit);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const double value = m_q->value();
        
        // If we are not editing, just draw the text, otherwise draw a 
        // semi-transparent rect to dim the background and let the QLineEdit
        // draw the rest (text, selection, cursor, etc.)
        const double hardSliderWidth = computeSliderWidth(static_cast<double>(m_q->minimum()), static_cast<double>(m_q->maximum()), value);
        const double softSliderWidth = computeSliderWidth(m_softMinimum, m_softMaximum, value);
        if (!isEditModeActive()) {
            QString text = m_q->text();
            if (isSoftRangeValid()) {
                if (m_softRangeViewMode == SoftRangeViewMode_AlwaysShowSoftRange) {
                    paintSlider(painter, text, softSliderWidth);
                } else {
                    paintSlider(painter, text, hardSliderWidth, softSliderWidth);
                }
            } else {
                // Draw the slider
                paintSlider(painter, text, hardSliderWidth);
            }
        } else {
            // Draw the slider
            if (isSoftRangeValid()) {
                if (m_softRangeViewMode == SoftRangeViewMode_AlwaysShowSoftRange) {
                    paintSlider(painter, QString(), softSliderWidth);
                } else {
                    paintSlider(painter, QString(), hardSliderWidth, softSliderWidth);
                }
            } else {
                paintSlider(painter, QString(), hardSliderWidth);
            }
            // Paint the overlay with the base color
            QColor color = m_q->palette().base().color();
            color.setAlpha(128);
            // a quiet slider (on glass, in cards) becomes a faint field of its ink, not a pale slab
            if (isQuiet()) {
                color = m_lineEdit->palette().text().color();
                color.setAlphaF(0.1);
            }
            paintSliderRect(painter, m_lineEdit->rect(), color);
        }
        // If we are editing the text then return false and let the QLineEdit
        // paint all the edit related stuff (e.g. selection)
        return !isEditModeActive();
    }

    bool lineEditMousePressEvent(QMouseEvent *e, bool edit)
    {
        if (!m_q->isEnabled()) {
            return false;
        }
        if (!isEditModeActive()) {
            if (e->button() == Qt::LeftButton) {
                // a press on the number beside a wide quiet line edits it instead of jumping to the maximum
                m_pressedOnQuietText = isQuiet() && isQuietWide() && e->pos().x() > quietTrack(0).right() + 6;
                if (m_pressedOnQuietText) {
                    QTimer::singleShot(0, &m_startEditingSignalProxy, SLOT(start()));
                    return true;
                }
                m_lastMousePressPosition = toSliderSpace(e->pos());
                const QPoint currentValuePosition = pointForValue(m_q->value());
                m_relativeDraggingOffset = currentValuePosition.x() - m_lastMousePressPosition.x();
                m_useRelativeDragging = (e->modifiers() & Qt::ShiftModifier);
                if (edit) {
                    QTimer::singleShot(0, &m_startEditingSignalProxy, SLOT(start()));
                } else {
                    lineEditMouseMoveEvent(e);
                }
            }
            return true;
        }
        return false;
    }

    bool lineEditMouseReleaseEvent(QMouseEvent *e)
    {
        if (!m_q->isEnabled()) {
            return false;
        }
        if (!isEditModeActive()) {
            // Releasing the right mouse button makes the lineedit enter
            // the edition mode if we are not editing
            if (e->button() == Qt::RightButton) {
                // a slider that knows its default goes back to it (see KisSliderSpinBox::defaultValueProperty);
                // one that does not starts editing
                const QVariant fallback = m_q->property("kisDefaultValue");
                if (fallback.isValid()) {
                    setValue(fallback.value<ValueType>(), false, true);
                    return true;
                }
                // If we call startEditing() right from the eventFilter(),
                // then the mouse release event will be somehow be passed
                // to Qt further and generate ContextEvent on Windows.
                // Therefore we should call it from a normal timer event.
                QTimer::singleShot(0, &m_startEditingSignalProxy, SLOT(start()));
            // Releasing the left mouse button stops the dragging. If signals
            // must be blocked when dragging then we set the value here and
            // Q_EMIT a signal
            } else if (e->button() == Qt::LeftButton) {
                if (m_pressedOnQuietText) {
                    m_pressedOnQuietText = false;
                    return true;
                }
                if (m_blockUpdateSignalOnDrag) {
                    const QPoint pos = toSliderSpace(e->pos());
                    const QPoint p(m_useRelativeDragging ? pos.x() + m_relativeDraggingOffset : pos.x(), pos.y());
                    setValue(valueForPoint(p, e->modifiers()), false, true);
                } else {
                    if (!m_isDragging) {
                        setValue(valueForPoint(toSliderSpace(e->pos()), e->modifiers()), false, true);
                    }
                }

                m_isDragging = false;
                updateQuietHover();
                Q_EMIT m_q->draggingFinished();
            }
            return true;
        }
        return false;
    }

    bool lineEditMouseMoveEvent(QMouseEvent *e)
    {
        if (!m_q->isEnabled()) {
            return false;
        }
        if (!isEditModeActive()) {
            if (e->buttons() & Qt::LeftButton) {
                if (m_pressedOnQuietText) {
                    return true;
                }
                if (!m_isDragging) {
                    m_isDragging = true;
                    updateQuietHover();
                }
                // At this point we are dragging so record the position and set
                // the value
                const QPoint pos = toSliderSpace(e->pos());
                const QPoint p(m_useRelativeDragging ? pos.x() + m_relativeDraggingOffset : pos.x(), pos.y());
                setValue(valueForPoint(p, e->modifiers()), m_blockUpdateSignalOnDrag);
                return true;
            }
        }
        return false;
    }

    bool widgetRangeTogglePaintEvent(QPaintEvent*)
    {
        QPainter painter(m_widgetRangeToggle);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // Compute sizes and positions
        const double width = static_cast<double>(m_widgetRangeToggle->width());
        const double height = static_cast<double>(m_widgetRangeToggle->height());
        constexpr double marginX = 4.0;
        const double toggleWidth = width - 2.0 * marginX;
        const double centerX = width * 0.5;
        const double centerY = height * 0.5;
        const double bigRadius = centerX - std::floor(centerX - (toggleWidth * 0.5)) + 0.5;
        const double smallRadius = bigRadius * 0.5;
        const double sliderAnimationPos = m_sliderAnimation.currentValue().toReal();
        const double radius = smallRadius + sliderAnimationPos * (bigRadius - smallRadius);
        // Compute color
        const double rangeToggleHoverAnimationPos = m_rangeToggleHoverAnimation.currentValue().toReal();
        const QColor baseColor = m_q->palette().base().color();
        const QColor textColor = m_q->palette().text().color();
        const QColor color = KisPaintingTweaks::blendColors(baseColor, textColor, 1.0 - (0.60 + 0.40 * rangeToggleHoverAnimationPos));
        // Paint outer circle
        painter.setPen(color);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(centerX, centerY), bigRadius, bigRadius);
        // Paint dot
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(centerX, centerY), radius, radius);
        return true;
    }

    bool widgetRangeToggleMouseReleaseEvent(QMouseEvent *e)
    {
        if (!m_q->isEnabled()) {
            return false;
        }
        if (e->button() == Qt::LeftButton) {
            if (!m_isSoftRangeActive) {
                makeSoftRangeActive();
            } else {
                makeHardRangeActive();
            }
            return true;
        }
        return false;
    }

    bool widgetRangeToggleEnterEvent(QEvent*)
    {
        m_rangeToggleHoverAnimation.stop();
        // scale the animation duration in case the animation is in the middle
        const int animationDuration =
            static_cast<int>(std::round(m_rangeToggleHoverAnimation.currentValue().toReal() * fullAnimationDuration));
        m_rangeToggleHoverAnimation.setStartValue(m_rangeToggleHoverAnimation.currentValue());
        m_rangeToggleHoverAnimation.setEndValue(1.0);
        m_rangeToggleHoverAnimation.setDuration(animationDuration);
        m_rangeToggleHoverAnimation.start();
        return false;
    }

    bool widgetRangeToggleLeaveEvent(QEvent*)
    {
        m_rangeToggleHoverAnimation.stop();
        // scale the animation duration in case the animation is in the middle
        const int animationDuration =
            static_cast<int>(std::round(m_rangeToggleHoverAnimation.currentValue().toReal() * fullAnimationDuration));
        m_rangeToggleHoverAnimation.setStartValue(m_rangeToggleHoverAnimation.currentValue());
        m_rangeToggleHoverAnimation.setEndValue(0.0);
        m_rangeToggleHoverAnimation.setDuration(animationDuration);
        m_rangeToggleHoverAnimation.start();
        return false;
    }

    bool eventFilter(QObject * o, QEvent * e) override
    {
        if (!o || !e) {
            return false;
        }
        if (o == m_q) {
            switch (e->type()) {
                case QEvent::Resize : return qResizeEvent(static_cast<QResizeEvent*>(e));
                case QEvent::FocusOut : return qFocusOutEvent(static_cast<QFocusEvent*>(e));
                case QEvent::MouseButtonPress : return qMousePressEvent(static_cast<QMouseEvent*>(e));
                case QEvent::KeyPress : return qKeyPressEvent(static_cast<QKeyEvent*>(e));
                case QEvent::ContextMenu : return qContextMenuEvent(static_cast<QContextMenuEvent*>(e));
                case QEvent::InputMethodQuery: return blockInputMethodQuery(static_cast<QInputMethodQueryEvent *>(e));
                case QEvent::Enter:
                case QEvent::Leave:
                    updateQuietHover();
                    break;
                default: break;
            }
        } else if (o == m_lineEdit) {
            switch (e->type()) {
                case QEvent::Paint : return lineEditPaintEvent(static_cast<QPaintEvent*>(e));
                case QEvent::MouseButtonPress : return lineEditMousePressEvent(static_cast<QMouseEvent*>(e), false);
                case QEvent::MouseButtonDblClick : return lineEditMousePressEvent(static_cast<QMouseEvent*>(e), true);
                case QEvent::MouseButtonRelease : return lineEditMouseReleaseEvent(static_cast<QMouseEvent*>(e));
                case QEvent::MouseMove : return lineEditMouseMoveEvent(static_cast<QMouseEvent*>(e));
                default: break;
            }
        } else if (o == m_widgetRangeToggle) {
            switch (e->type()) {
                case QEvent::Paint : return widgetRangeTogglePaintEvent(static_cast<QPaintEvent*>(e));
                case QEvent::MouseButtonRelease: return widgetRangeToggleMouseReleaseEvent(static_cast<QMouseEvent*>(e));
                case QEvent::Enter: return widgetRangeToggleEnterEvent(e);
                case QEvent::Leave: return widgetRangeToggleLeaveEvent(e);
                case QEvent::InputMethodQuery: return blockInputMethodQuery(static_cast<QInputMethodQueryEvent *>(e));
                default: break;
            }
        }
        return false;
    }

private:
    // Margin around the spinbox for which the dragging gives same results,
    // regardless of the vertical distance
    static constexpr double constantDraggingMargin{32.0};
    // Height of the collapsed slider bar
    static constexpr double heightOfCollapsedSlider{3.0};
    // Height of the space between the soft and hard range sliders
    static constexpr double heightOfSpaceBetweenSliders{0.0};
    // Width of the area to activate soft/hard range
    static constexpr double widthOfRangeModeToggle{16.0};
    // The duration of the animation 
    static constexpr double fullAnimationDuration{double(KisModernStyle::changeDuration)};

    SpinBoxType *m_q {nullptr};
    QLineEdit *m_lineEdit {nullptr};
    QWidget *m_widgetRangeToggle {nullptr};
    ValueType m_softMinimum {static_cast<ValueType>(0)};
    ValueType m_softMaximum {static_cast<ValueType>(0)};
    double m_exponentRatio {1.0};
    bool m_blockUpdateSignalOnDrag {false};
    ValueType m_fastSliderStep {static_cast<ValueType>(5)};
    mutable ValueType m_valueBeforeEditing {static_cast<ValueType>(0)};
    bool m_isDragging {false};
    bool m_pressedOnQuietText {false};
    bool m_useRelativeDragging {false};
    int m_relativeDraggingOffset {0};
    QPoint m_lastMousePressPosition;
    int m_rightClickCounter {0};
    bool m_focusLostDueToMenu {false};
    bool m_isSoftRangeActive {true};
    QVariantAnimation m_sliderAnimation;
    QVariantAnimation m_rangeToggleHoverAnimation;
    QVariantAnimation m_quietHover;
    SignalToFunctionProxy m_startEditingSignalProxy;

    enum SoftRangeViewMode
    {
        SoftRangeViewMode_AlwaysShowSoftRange,
        SoftRangeViewMode_ShowBothRanges
    } m_softRangeViewMode{SoftRangeViewMode_ShowBothRanges};
};

#endif // KISSLIDERSPINBOXPRIVATE_H
