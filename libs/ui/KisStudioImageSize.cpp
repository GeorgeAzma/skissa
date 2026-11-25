/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioImageSize.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <KisModernStyle.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisStudioCanvasHandles.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "kis_icon_utils.h"
#include "kis_filter_strategy.h"
#include "kis_image.h"
#include "kis_image_manager.h"
#include "kis_paint_device.h"
#include "widgets/KisScaleFilterPreview.h"
#include "widgets/KisStudioPopup.h"
#include "widgets/KisStudioValueRow.h"

void KisStudioImageSize::open(KisViewManager *viewManager)
{
    KisImageSP image = viewManager ? viewManager->image() : nullptr;
    KisMainWindow *window = viewManager ? viewManager->mainWindow() : nullptr;
    if (!image || !window || !viewManager->blockUntilOperationsFinished(image)) return;

    const QSize original = image->size();
    const QRect bounds(QPoint(), original);
    QPointer<KisViewManager> view = viewManager;

    auto *popup = new KisStudioPopup(window);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(popup);
    popup->fitLayout(layout, KisModernStyle::space3);
    layout->setSpacing(KisModernStyle::space2);

    // from a sixteenth to four times as large: the row's length is spent where scaling is wanted
    auto rangeOf = [](int side) {
        KisStudioValueRow::Range range;
        range.minimum = qMax(1, side / 16);
        range.maximum = side * 4;
        range.suffix = i18nc("pixels, after a number", " px");
        range.size = true;
        return range;
    };
    auto *width = new KisStudioValueRow(i18nc("an image's width", "Width"), popup);
    auto *height = new KisStudioValueRow(i18nc("an image's height", "Height"), popup);
    width->setRange(rangeOf(original.width()));
    height->setRange(rangeOf(original.height()));
    width->setValue(original.width());
    height->setValue(original.height());
    width->setDefaultValue(original.width());
    height->setDefaultValue(original.height());

    // linked: the picture keeps its shape; unchained, each side on its own
    auto *chain = new QToolButton(popup);
    chain->setCheckable(true);
    chain->setChecked(true);
    chain->setAutoRaise(true);
    auto showChain = [chain]() {
        chain->setIcon(KisIconUtils::loadIcon(chain->isChecked() ? QStringLiteral("studio_link") : QStringLiteral("studio_unlink")));
    };
    showChain();
    QObject::connect(chain, &QToolButton::toggled, chain, showChain);
    chain->setToolTip(i18nc("@info:tooltip width and height change together", "Keep the shape"));

    auto *sides = new QHBoxLayout;
    auto *rows = new QVBoxLayout;
    rows->setSpacing(KisModernStyle::hairline);
    rows->addWidget(width);
    rows->addWidget(height);
    sides->addLayout(rows, 1);
    sides->addWidget(chain);
    layout->addLayout(sides);

    auto *ways = new KisScaleFilterPreview(popup);
    ways->setContent(image->projection(), bounds);
    ways->setTargetSize(original);
    layout->addWidget(ways);

    auto *scale = new QPushButton(i18nc("@action:button scale the image", "Scale"), popup);
    scale->setDefault(true);
    layout->addWidget(scale, 0, Qt::AlignRight);

    // what the tiles show and what Scale uses: the one picked, else the one that suits the picture
    auto filterId = std::make_shared<QString>();
    auto chosenFilter = [image, bounds, filterId]() -> KisFilterStrategy * {
        if (!filterId->isEmpty()) {
            if (KisFilterStrategy *strategy = KisFilterStrategyRegistry::instance()->value(*filterId)) return strategy;
        }
        return KisFilterStrategyRegistry::instance()->autoFilterStrategy(image->projection(), bounds);
    };
    ways->setCurrent(chosenFilter()->id());
    QObject::connect(ways, &KisScaleFilterPreview::picked, popup, [ways, filterId](const QString &id) {
        *filterId = id;
        ways->setCurrent(id);
    });

    auto sizeNow = [width, height]() { return QSize(qMax(1, qRound(width->value())), qMax(1, qRound(height->value()))); };
    auto follow = [=](KisStudioValueRow *changed, KisStudioValueRow *other, qreal ratio) {
        if (chain->isChecked()) other->setValue(qMax<qreal>(1, qRound(changed->value() * ratio)));
        ways->setTargetSize(sizeNow());
    };
    const qreal tall = qreal(original.height()) / original.width();
    QObject::connect(width, &KisStudioValueRow::valueChanged, popup, [=]() { follow(width, height, tall); });
    QObject::connect(height, &KisStudioValueRow::valueChanged, popup, [=]() { follow(height, width, 1 / tall); });

    // the new size on the picture, its corner to drag: the frame grows from the top left as the image would
    auto gone = std::make_shared<bool>(false);
    QPointer<KisStudioCanvasHandles> handles = KisStudioCanvasHandles::start(viewManager->canvasBase());
    if (handles) {
        popup->setPinned(true);
        auto place = [handles, sizeNow]() {
            if (handles) handles->setHandles({{QPointF(sizeNow().width(), sizeNow().height()), QColor(Qt::white)}});
        };
        place();
        handles->drawUnder = [sizeNow](QPainter &painter, const QTransform &toWidget) {
            const QPolygonF frame = toWidget.map(QPolygonF(QRectF(QPointF(), QSizeF(sizeNow()))));
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(0, 0, 0, 120), 3));
            painter.drawPolygon(frame);
            painter.setPen(QPen(Qt::white, 1.5));
            painter.drawPolygon(frame);
        };
        handles->moved = [=](int, const QPointF &at, Qt::KeyboardModifiers) {
            if (chain->isChecked()) {
                // the corner's place along the diagonal: the picture keeps its shape wherever the pointer is
                const qreal along = (at.x() * original.width() + at.y() * original.height())
                    / (qreal(original.width()) * original.width() + qreal(original.height()) * original.height());
                width->setValue(qMax<qreal>(1, qRound(original.width() * along)));
                height->setValue(qMax<qreal>(1, qRound(original.height() * along)));
            } else {
                width->setValue(qMax<qreal>(1, qRound(at.x())));
                height->setValue(qMax<qreal>(1, qRound(at.y())));
            }
            // the rows show a value set from here without telling anyone
            ways->setTargetSize(sizeNow());
            place();
        };
        // a press elsewhere on the picture, Esc or Enter there: done with the size
        QPointer<KisStudioPopup> shown = popup;
        handles->ended = [shown, gone]() {
            if (shown && !*gone) shown->close();
        };
        QObject::connect(width, &KisStudioValueRow::valueChanged, popup, place);
        QObject::connect(height, &KisStudioValueRow::valueChanged, popup, place);
        QObject::connect(popup, &QObject::destroyed, [handles, gone]() {
            *gone = true;
            if (handles) handles->end();
        });
    }

    QObject::connect(scale, &QPushButton::clicked, popup, [=]() {
        const QSize size = sizeNow();
        popup->close();
        if (!view || size == original) return;
        view->imageManager()->scaleCurrentImage(size, image->xRes(), image->yRes(), chosenFilter());
    });

    popup->adjustSize();
    const QRect frame = window->geometry();
    popup->popupAt(frame.center() - QPoint(popup->width() / 2, popup->height() / 2));
}
