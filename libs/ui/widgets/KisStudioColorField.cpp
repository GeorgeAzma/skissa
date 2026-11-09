/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisStudioColorField.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTabletEvent>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

#include <KoColor.h>
#include <KisModernStyle.h>
#include <kis_cursor.h>
#include <kis_icon_utils.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisStudioColorPanel.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "canvas/kis_coordinates_converter.h"
#include "kis_canvas_resource_provider.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "widgets/KisStudioPopup.h"
#include "widgets/KisStudioValueRow.h"

namespace
{

KisCanvas2 *currentCanvas()
{
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    KisViewManager *view = window ? window->viewManager() : nullptr;
    return view ? view->canvasBase() : nullptr;
}

/// A color on a rounded square, over checks where it is see-through
void paintSwatch(QPainter &painter, const QRectF &rect, const QColor &color, const QPalette &palette)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPainterPath shape = KisModernStyle::squircle(rect, KisModernStyle::cornerRadius);
    if (color.alpha() < 255) {
        painter.setClipPath(shape);
        const qreal cell = 5;
        for (int y = 0; y * cell < rect.height(); y++) {
            for (int x = 0; x * cell < rect.width(); x++) {
                painter.fillRect(QRectF(rect.left() + x * cell, rect.top() + y * cell, cell, cell), (x + y) % 2 ? QColor(200, 200, 200) : QColor(Qt::white));
            }
        }
        painter.setClipping(false);
    }
    painter.fillPath(shape, color);
    QColor edge = palette.color(QPalette::WindowText);
    edge.setAlphaF(0.25);
    painter.setPen(QPen(edge, 1));
    painter.drawPath(shape);
    painter.restore();
}

QIcon swatchIcon(const QColor &color, const QPalette &palette, int side)
{
    QPixmap pixmap(side * 2, side * 2);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    paintSwatch(painter, QRectF(1, 1, side - 2, side - 2), color, palette);
    return QIcon(pixmap);
}

QString written(const QColor &color)
{
    return color.name(QColor::HexRgb).toUpper();
}

} // namespace

struct KisStudioColorField::Private {
    QColor color {Qt::white};
    QColor defaultColor;
    /// What the swatch shows while the eyedropper is over the image
    QColor picking;
    bool alpha {false};
    Sampler sampler;
    QPointer<QWidget> popup;
    QPointer<QObject> dropper;
};

/**
 * Takes a color from the image: while it lasts, the swatch shows what is under the pointer over the canvas; a click
 * there takes it; a click anywhere else, a right click or Esc leaves the color as it was. It reads only pixels inside
 * the image, and only where the canvas itself is under the pointer, not a panel over it.
 */
class KisStudioColorDropper : public QObject
{
public:
    explicit KisStudioColorDropper(KisStudioColorField *field)
        : QObject(field)
        , m_field(field)
    {
        qApp->installEventFilter(this);
        QApplication::setOverrideCursor(KisCursor::samplerCursor());
    }

    ~KisStudioColorDropper() override
    {
        qApp->removeEventFilter(this);
        if (!m_done) QApplication::restoreOverrideCursor();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        // input reaches a window, then the widget under the pointer: the widget it goes to says where it is
        QWidget *widget = qobject_cast<QWidget *>(watched);
        switch (event->type()) {
        case QEvent::MouseMove:
            if (!m_done && widget) preview(widget, static_cast<QMouseEvent *>(event)->globalPos());
            return false;
        case QEvent::TabletMove:
            if (!m_done && widget) preview(widget, static_cast<QTabletEvent *>(event)->globalPos());
            return false;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::TabletPress: {
            if (!widget) return false;
            if (m_done) return true;
            const bool tablet = event->type() == QEvent::TabletPress;
            const QPoint at = tablet ? static_cast<QTabletEvent *>(event)->globalPos() : static_cast<QMouseEvent *>(event)->globalPos();
            const Qt::MouseButton button = tablet ? static_cast<QTabletEvent *>(event)->button() : static_cast<QMouseEvent *>(event)->button();
            const bool onCanvas = canvasOf(widget);
            QColor taken;
            const bool takes = button == Qt::LeftButton && sample(widget, at, &taken);
            m_field->showWhilePicking(QColor());
            finish();
            if (takes) {
                QColor color = m_field->color();
                color.setRgb(taken.red(), taken.green(), taken.blue(), color.alpha());
                m_field->choose(color);
            }
            // a press elsewhere goes on to what it was meant for; one on the canvas only ends this
            return onCanvas;
        }
        case QEvent::MouseButtonRelease:
        case QEvent::TabletRelease:
            // the release of the press that ended it goes nowhere
            if (m_done && widget) {
                deleteLater();
                return true;
            }
            return false;
        case QEvent::KeyPress:
            if (!m_done && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
                m_field->showWhilePicking(QColor());
                finish();
                deleteLater();
                return true;
            }
            return false;
        default:
            return false;
        }
    }

private:
    /// Ends picking; this goes once the press's release has been swallowed, or soon in any case
    void finish()
    {
        m_done = true;
        QApplication::restoreOverrideCursor();
        QTimer::singleShot(1000, this, &QObject::deleteLater);
    }

    void preview(QWidget *widget, const QPoint &global)
    {
        QColor color;
        m_field->showWhilePicking(sample(widget, global, &color) ? QColor(color.red(), color.green(), color.blue(), m_field->color().alpha()) : QColor());
    }

    /// The canvas, if \p widget is it or on it
    static QWidget *canvasOf(QWidget *widget)
    {
        KisCanvas2 *canvas = currentCanvas();
        QWidget *canvasWidget = canvas ? canvas->canvasWidget() : nullptr;
        if (!canvasWidget || !widget) return nullptr;
        return widget == canvasWidget || canvasWidget->isAncestorOf(widget) ? canvasWidget : nullptr;
    }

    bool sample(QWidget *widget, const QPoint &global, QColor *color) const
    {
        QWidget *canvasWidget = canvasOf(widget);
        KisCanvas2 *canvas = currentCanvas();
        if (!canvasWidget || !canvas || !canvas->image()) return false;
        const QPointF at = canvas->coordinatesConverter()->widgetToImage(QPointF(canvasWidget->mapFromGlobal(global)));
        const QPoint pixel(int(std::floor(at.x())), int(std::floor(at.y())));
        if (!canvas->image()->bounds().contains(pixel)) return false;
        if (m_field->d->sampler) return m_field->d->sampler(pixel, color);
        KoColor taken;
        canvas->image()->projection()->pixel(pixel.x(), pixel.y(), &taken);
        *color = taken.toQColor();
        return true;
    }

    KisStudioColorField *m_field;
    bool m_done {false};
};

/// The field's picker: the color panel's wheel, the color written out, the paint colors, the eyedropper, opacity
class KisStudioColorFieldPopup : public KisStudioPopup
{
public:
    explicit KisStudioColorFieldPopup(KisStudioColorField *field)
        : KisStudioPopup(field)
        , m_field(field)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        auto *layout = new QVBoxLayout(this);
        fitLayout(layout, KisModernStyle::space2);
        layout->setSpacing(KisModernStyle::space2);

        m_wheel = new KisStudioColorWheel(this);
        m_wheel->setFixedSize(qMax(220, 16 * fontMetrics().averageCharWidth()), qMax(188, 11 * fontMetrics().height()));
        m_wheel->setColor(field->color());
        layout->addWidget(m_wheel);
        connect(m_wheel, &KisStudioColorWheel::colorPicked, this, [this](const QColor &picked) {
            QColor color = picked;
            color.setAlpha(m_field->color().alpha());
            m_field->choose(color);
            m_text->setText(written(color));
        });

        auto *row = new QHBoxLayout();
        row->setSpacing(KisModernStyle::space1);
        m_text = new QLineEdit(written(field->color()), this);
        m_text->setToolTip(i18nc("@info:tooltip", "Hex, rgb, hsv or oklch"));
        connect(m_text, &QLineEdit::editingFinished, this, [this]() {
            // the focus leaving the field is not an edit
            if (!m_text->isModified()) return;
            m_text->setModified(false);
            QColor color = KisStudioColorWheel::colorOfText(m_text->text());
            if (!color.isValid()) {
                m_text->setText(written(m_field->color()));
                return;
            }
            color.setAlpha(m_field->color().alpha());
            m_field->choose(color);
            m_wheel->setColor(color);
            m_text->setText(written(color));
        });
        row->addWidget(m_text, 1);

        // the paint colors, a click away
        if (KisMainWindow *window = KisPart::instance()->currentMainwindow()) {
            if (KisViewManager *view = window->viewManager()) {
                if (KisCanvasResourceProvider *resources = view->canvasResourceProvider()) {
                    addChip(row, resources->fgColor().toQColor(), i18nc("@info:tooltip", "Paint color"));
                    addChip(row, resources->bgColor().toQColor(), i18nc("@info:tooltip", "Second color"));
                }
            }
        }
        auto *dropper = new QToolButton(this);
        dropper->setAutoRaise(true);
        dropper->setFocusPolicy(Qt::NoFocus);
        dropper->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_sampler")));
        dropper->setToolTip(i18nc("@info:tooltip", "Take from the image"));
        connect(dropper, &QToolButton::clicked, this, [this]() {
            KisStudioColorField *field = m_field;
            close();
            field->startPicking();
        });
        row->addWidget(dropper);
        layout->addLayout(row);

        if (field->d->alpha) {
            auto *opacity = new KisStudioValueRow(i18nc("color field: how see-through the color is", "Opacity"), this);
            KisStudioValueRow::Range range;
            range.maximum = 100;
            range.suffix = QStringLiteral("%");
            opacity->setRange(range);
            opacity->setDefaultValue(100);
            opacity->setClickSetsValue(true);
            opacity->setValue(qRound(field->color().alphaF() * 100));
            connect(opacity, &KisStudioValueRow::valueChanged, this, [this](qreal value) {
                QColor color = m_field->color();
                color.setAlphaF(qBound(0.0, value / 100.0, 1.0));
                m_field->choose(color);
            });
            layout->addWidget(opacity);
        }
    }

private:
    void addChip(QHBoxLayout *row, const QColor &color, const QString &tip)
    {
        auto *chip = new QToolButton(this);
        chip->setAutoRaise(true);
        chip->setFocusPolicy(Qt::NoFocus);
        const int side = qMax(20, fontMetrics().height() + 4);
        chip->setIconSize(QSize(side, side));
        chip->setIcon(swatchIcon(color, palette(), side));
        chip->setToolTip(tip);
        connect(chip, &QToolButton::clicked, this, [this, color]() {
            QColor chosen = color;
            chosen.setAlpha(m_field->color().alpha());
            m_field->choose(chosen);
            m_wheel->setColor(chosen);
            m_text->setText(written(chosen));
        });
        row->addWidget(chip);
    }

    KisStudioColorField *m_field;
    KisStudioColorWheel *m_wheel {nullptr};
    QLineEdit *m_text {nullptr};
};

KisStudioColorField::KisStudioColorField(QWidget *parent)
    : QWidget(parent)
    , d(new Private)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
}

KisStudioColorField::~KisStudioColorField()
{
}

void KisStudioColorField::setColor(const QColor &color)
{
    if (color == d->color) return;
    d->color = color;
    update();
}

QColor KisStudioColorField::color() const
{
    return d->color;
}

void KisStudioColorField::setDefaultColor(const QColor &color)
{
    d->defaultColor = color;
}

void KisStudioColorField::setAlphaEnabled(bool enabled)
{
    d->alpha = enabled;
}

void KisStudioColorField::setSampler(Sampler sampler)
{
    d->sampler = sampler;
}

QSize KisStudioColorField::sizeHint() const
{
    const int height = qMax(22, fontMetrics().height() + 6);
    return QSize(height * 8 / 5, height);
}

QSize KisStudioColorField::minimumSizeHint() const
{
    return sizeHint();
}

void KisStudioColorField::choose(const QColor &color)
{
    if (color == d->color) return;
    d->color = color;
    update();
    Q_EMIT colorChanged(color);
}

void KisStudioColorField::startPicking()
{
    if (d->dropper) return;
    d->dropper = new KisStudioColorDropper(this);
    update();
}

void KisStudioColorField::showWhilePicking(const QColor &color)
{
    if (color == d->picking) return;
    d->picking = color;
    update();
}

void KisStudioColorField::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QColor shown = d->picking.isValid() ? d->picking : d->color;
    paintSwatch(painter, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), shown, palette());
    if (d->dropper) {
        // the eyedropper is out: a ring of the text color says this swatch is taking
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor ring = palette().color(QPalette::WindowText);
        ring.setAlphaF(0.8);
        painter.setPen(QPen(ring, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(1, 1, -1, -1), KisModernStyle::cornerRadius));
    }
}

void KisStudioColorField::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (d->popup) return;
    auto *popup = new KisStudioColorFieldPopup(this);
    d->popup = popup;
    popup->popupBeside(this, Qt::BottomEdge);
}

void KisStudioColorField::contextMenuEvent(QContextMenuEvent *event)
{
    event->accept();
    if (d->defaultColor.isValid()) choose(d->defaultColor);
}
