/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioSettings.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFile>
#include <QFileInfo>
#include <QFontComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QHelpEvent>
#include <QToolTip>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <KConfig>
#include <KConfigGroup>
#include <KSharedConfig>
#include <kactioncollection.h>
#include <kfts_fuzzy_match.h>
#include <klocalizedstring.h>

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoColorSpaceEngine.h>
#include <KoConfigAuthorPage.h>
#include <KoFileDialog.h>
#include <KoResourcePaths.h>

#include "KisMainWindow.h"
#include "KisModernStyle.h"
#include "KisSettingsRegistry.h"
#include "KisSettingsTransfer.h"
#include "KisStudioGlass.h"
#include "KisStudioMotion.h"
#include "KisStudioTheme.h"
#include "widgets/KisStudioPicker.h"
#include "KisViewManager.h"
#include "KisView.h"
#include "kis_canvas2.h"
#include "dialogs/KisKeysPage.h"
#include "KisPlatformPluginInterfaceFactory.h"
#include "kis_color_button.h"
#include "kis_config.h"
#include "kis_config_notifier.h"
#include "kis_cubic_curve.h"
#include "kis_icon_utils.h"
#include "kis_image_config.h"
#include "kis_slider_spin_box.h"
#include "widgets/kis_curve_widget.h"

#ifdef Q_OS_WIN
#include "dialogs/KisDlgCustomTabletResolution.h"
#endif
#include "input/wintab/drawpile_tablettester/tablettester.h"

#include <cmath>
#include <type_traits>

namespace {

using R = KisSettingsRegistry;
using Setting = KisSettingsRegistry::Setting;
using Kind = KisSettingsRegistry::Kind;

const QString cardMime = QStringLiteral("application/x-krita-settings-card");
/// Your sections and Quick (JSON); see UserSections
const QString sectionsKey = QStringLiteral("StudioSettingsSections");
const QString quickId = QStringLiteral("quick");
const QString searchId = QStringLiteral("search");
const QString changedId = QStringLiteral("changed");
const QString keysSection = QStringLiteral("keys");

/// A card's drag or menu runs its own event loop: rebuilding the cards then would delete that card under it
int cardsBusy = 0;

struct Busy {
    Busy() { cardsBusy++; }
    ~Busy() { cardsBusy--; }
};

/// A dialog a card opened (a color's) is up: rebuilding would close it, as if canceled
bool cardDialogOpen()
{
    Q_FOREACH (QWidget *top, QApplication::topLevelWidgets()) {
        if (!qobject_cast<QDialog *>(top) || !top->isVisible()) continue;
        for (QWidget *w = top->parentWidget(); w; w = w->parentWidget()) {
            if (w->objectName() == QLatin1String("studioSettingsCard")) return true;
        }
    }
    return false;
}

constexpr int railWidth = 150;
constexpr int bodyWidth = 470;
constexpr int quickWidth = 430;
constexpr int controlWidth = 220;

QColor ink(const QWidget *widget, qreal alpha)
{
    QColor color = widget->palette().color(QPalette::WindowText);
    color.setAlphaF(alpha);
    return color;
}

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

QLabel *titleLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    return label;
}

QLabel *dimLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, KisModernStyle::secondaryInk(palette.color(QPalette::WindowText)));
    label->setPalette(palette);
    label->setWordWrap(true);
    return label;
}

/// A short message from Settings (or Quick): in the panel, or on the canvas
void say(QWidget *from, const QString &text)
{
    for (QWidget *w = from; w; w = w->parentWidget()) {
        if (auto *settings = qobject_cast<KisStudioSettings *>(w)) {
            settings->say(text);
            return;
        }
    }
    KisMainWindow *window = from ? qobject_cast<KisMainWindow *>(from->window()) : nullptr;
    if (!window && from && from->window()) window = qobject_cast<KisMainWindow *>(from->window()->parentWidget());
    if (window && window->viewManager()) window->viewManager()->showFloatingMessage(text, QIcon());
}

// ---------------------------------------------------------------------------
// controls

/// On or off: a pill with a knob, which slides
class Switch : public QAbstractButton
{
public:
    explicit Switch(QWidget *parent)
        : QAbstractButton(parent)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        m_slide.setDuration(120);
        m_slide.setEasingCurve(QEasingCurve::OutCubic);
        connect(&m_slide, &QVariantAnimation::valueChanged, this, [this]() { update(); });
        connect(this, &QAbstractButton::toggled, this, [this](bool on) {
            m_slide.stop();
            m_slide.setStartValue(m_position);
            m_slide.setEndValue(on ? 1.0 : 0.0);
            m_slide.start();
        });
    }

    /// Without the slide, nor toggled(): shown as it already is
    void show(bool on)
    {
        if (on == isChecked()) return;
        const QSignalBlocker blocker(this);
        setChecked(on);
        m_slide.stop();
        m_position = on ? 1.0 : 0.0;
        update();
    }

    QSize sizeHint() const override { return QSize(34, 20); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (m_slide.state() == QAbstractAnimation::Running) m_position = m_slide.currentValue().toReal();
        else m_position = isChecked() ? 1.0 : 0.0;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF track = QRectF(0, 0, 34, 20).translated((width() - 34) / 2.0, (height() - 20) / 2.0);
        QColor fill = mix(ink(this, underMouse() ? 0.3 : 0.22), ink(this, 1.0), m_position);
        if (!isEnabled()) fill.setAlphaF(fill.alphaF() * 0.4);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(track, 10, 10);
        // the knob: light on the faint track, dark on the bright one
        QColor dark = palette().color(QPalette::Window);
        dark.setAlpha(255);
        QColor knob = mix(ink(this, 1.0), dark, m_position);
        if (!isEnabled()) knob.setAlphaF(0.5);
        painter.setBrush(knob);
        painter.drawEllipse(QPointF(track.left() + 10 + 14 * m_position, track.center().y()), 7, 7);
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(palette().color(QPalette::Highlight), 1.5));
            painter.drawRoundedRect(track.adjusted(-2, -2, 2, 2), 12, 12);
        }
    }

    void enterEvent(QEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    QVariantAnimation m_slide;
    qreal m_position {0};
};

/// A few choices side by side, one of them picked
class Segmented : public QWidget
{
public:
    std::function<void(int)> picked;

    Segmented(const QStringList &texts, QWidget *parent)
        : QWidget(parent)
        , m_texts(texts)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setAttribute(Qt::WA_Hover);
        setMouseTracking(true);
    }

    static int widthFor(const QStringList &texts, const QFontMetrics &metrics)
    {
        int width = 4;
        Q_FOREACH (const QString &text, texts) width += metrics.horizontalAdvance(text) + 20;
        return width;
    }

    void setCurrent(int index)
    {
        if (index == m_current) return;
        m_current = index;
        update();
    }

    QSize sizeHint() const override { return QSize(widthFor(m_texts, fontMetrics()), 26); }

protected:
    QRectF part(int i) const
    {
        const qreal total = widthFor(m_texts, fontMetrics()) - 4;
        const qreal scale = (width() - 4) / qMax<qreal>(1, total);
        qreal x = 2;
        for (int j = 0; j < i; j++) x += (fontMetrics().horizontalAdvance(m_texts[j]) + 20) * scale;
        return QRectF(x, 2, (fontMetrics().horizontalAdvance(m_texts[i]) + 20) * scale, height() - 4);
    }

    int at(const QPoint &pos) const
    {
        for (int i = 0; i < m_texts.size(); i++) {
            if (part(i).contains(pos)) return i;
        }
        return -1;
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(ink(this, 0.14), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(frame, frame.height() / 2, frame.height() / 2);
        for (int i = 0; i < m_texts.size(); i++) {
            const QRectF r = part(i);
            if (i == m_current || i == m_hovered) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(ink(this, i == m_current ? 0.16 : 0.06));
                painter.drawRoundedRect(r, r.height() / 2, r.height() / 2);
            }
            painter.setPen(i == m_current ? palette().color(QPalette::WindowText) : KisModernStyle::secondaryInk(palette().color(QPalette::WindowText)));
            painter.drawText(r, Qt::AlignCenter, m_texts[i]);
        }
        if (hasFocus() && m_current >= 0) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(palette().color(QPalette::Highlight), 1.5));
            const QRectF r = part(m_current);
            painter.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int hovered = at(event->pos());
        if (hovered != m_hovered) {
            m_hovered = hovered;
            update();
        }
    }

    void leaveEvent(QEvent *) override
    {
        m_hovered = -1;
        update();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        choose(at(event->pos()));
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Left) choose(qMax(0, m_current - 1));
        else if (event->key() == Qt::Key_Right) choose(qMin(m_texts.size() - 1, m_current + 1));
        else QWidget::keyPressEvent(event);
    }

private:
    void choose(int index)
    {
        if (index < 0 || index == m_current) return;
        m_current = index;
        update();
        if (picked) picked(index);
    }

    QStringList m_texts;
    int m_current {-1};
    int m_hovered {-1};
};

/// Pictures to pick one of (a theme, a glass), each with its name under it
class Tiles : public QWidget
{
public:
    struct Tile {
        QString name;
        std::function<void(QPainter &, const QRectF &)> paint;
    };
    std::function<void(int)> picked;

    Tiles(const QVector<Tile> &tiles, const QSize &tileSize, QWidget *parent)
        : QWidget(parent)
        , m_tiles(tiles)
        , m_size(tileSize)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    void setCurrent(int index)
    {
        if (index == m_current) return;
        m_current = index;
        update();
    }

    QSize sizeHint() const override { return QSize(3 * m_size.width() + 16, heightFor(3 * m_size.width() + 16)); }

protected:
    /// Three across (fewer if there are fewer), as large as fits, up to their size
    int columns() const { return qMax(1, qMin(3, int(m_tiles.size()))); }
    QSize tileSize(int width) const
    {
        const int w = qMin(m_size.width(), (width - 8 * (columns() - 1)) / columns());
        return QSize(qMax(40, w), qMax(24, w * m_size.height() / m_size.width()));
    }
    int heightFor(int width) const
    {
        const int rows = (int(m_tiles.size()) + columns() - 1) / columns();
        return rows * (tileSize(width).height() + 26) + qMax(0, rows - 1) * 6;
    }

    void resizeEvent(QResizeEvent *) override
    {
        // as tall as its tiles are at this width (a scroll area's layout doesn't ask)
        setFixedHeight(heightFor(width()));
    }

    QRect tileRect(int i) const
    {
        const QSize tile = tileSize(width());
        return QRect((i % columns()) * (tile.width() + 8), (i / columns()) * (tile.height() + 32), tile.width(), tile.height() + 26);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        for (int i = 0; i < m_tiles.size(); i++) {
            const QRect cell = tileRect(i);
            const QSize tile = tileSize(width());
            const QRectF picture(cell.left() + 1, cell.top() + 1, tile.width() - 2, tile.height() - 2);
            painter.save();
            QPainterPath clip;
            clip.addPath(KisModernStyle::squircle(picture, 8));
            painter.setClipPath(clip);
            if (m_tiles[i].paint) m_tiles[i].paint(painter, picture);
            painter.restore();
            const bool current = i == m_current;
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(current ? (hasFocus() ? palette().color(QPalette::Highlight) : palette().color(QPalette::Link))
                                        : ink(this, i == m_hovered ? 0.35 : 0.14),
                                current ? 2 : 1));
            painter.drawPath(KisModernStyle::squircle(picture.adjusted(-0.5, -0.5, 0.5, 0.5), 8));
            painter.setPen(current ? palette().color(QPalette::WindowText) : KisModernStyle::secondaryInk(palette().color(QPalette::WindowText)));
            const QRect text(cell.left(), cell.top() + tile.height() + 2, cell.width(), 22);
            painter.drawText(text, Qt::AlignHCenter | Qt::AlignVCenter, fontMetrics().elidedText(m_tiles[i].name, Qt::ElideRight, text.width()));
        }
    }

    int at(const QPoint &pos) const
    {
        for (int i = 0; i < m_tiles.size(); i++) {
            if (tileRect(i).contains(pos)) return i;
        }
        return -1;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int hovered = at(event->pos());
        if (hovered != m_hovered) {
            m_hovered = hovered;
            update();
        }
    }

    void leaveEvent(QEvent *) override
    {
        m_hovered = -1;
        update();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        const int i = at(event->pos());
        if (i >= 0) choose(i);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        const int at = qMax(0, m_current);
        switch (event->key()) {
        case Qt::Key_Left: choose(qMax(0, at - 1)); break;
        case Qt::Key_Right: choose(qMin(int(m_tiles.size()) - 1, at + 1)); break;
        case Qt::Key_Up: choose(qMax(0, at - columns())); break;
        case Qt::Key_Down: choose(qMin(int(m_tiles.size()) - 1, at + columns())); break;
        default: QWidget::keyPressEvent(event); return;
        }
    }

    void focusInEvent(QFocusEvent *) override { update(); }
    void focusOutEvent(QFocusEvent *) override { update(); }

    void choose(int i)
    {
        if (i == m_current) return;
        m_current = i;
        update();
        if (picked) picked(i);
    }

private:
    QVector<Tile> m_tiles;
    QSize m_size;
    int m_current {-1};
    int m_hovered {-1};
};

/// KisSliderSpinBox or KisDoubleSliderSpinBox, without a frame
template<class Slider>
Slider *quietSlider(QWidget *parent)
{
    auto *slider = new Slider(parent);
    slider->setFrame(false);
    slider->setProperty(KisModernStyle::quietFieldProperty, true);
    slider->setButtonSymbols(QAbstractSpinBox::NoButtons);
    slider->setFixedWidth(controlWidth);
    slider->setMinimumHeight(28);
    return slider;
}

// ---------------------------------------------------------------------------
// cards

/// How a number is shown: what is stored is shown times \p scale (minutes for seconds: 1/60)
struct Number {
    Number(const QString &label = QString(), qreal scale = 1, int decimals = 0, const QString &suffix = QString(), qreal step = 1)
        : label(label), scale(scale), decimals(decimals), suffix(suffix), step(step)
    {
    }
    QString label;
    qreal scale;
    int decimals;
    QString suffix;
    qreal step;
    /// A narrower range than the setting's (shown values), if given
    qreal low {NAN};
    qreal high {NAN};
    /// Written when the slider is let go, not while it is dragged: what is costly to change (undo is cleared, threads restarted)
    bool onRelease {false};
};

/// Builds a card's rows, each bound to its setting both ways
struct Builder {
    QWidget *card {nullptr};
    QVBoxLayout *layout {nullptr};
    /// The card's title and the line it is on
    QLabel *title {nullptr};
    QHBoxLayout *header {nullptr};
    KisMainWindow *window {nullptr};
    QVector<std::function<void()>> refresh;

    R *registry() const { return R::instance(); }

    /// Runs \p show now and whenever a setting changes
    void follow(const std::function<void()> &show)
    {
        show();
        refresh << show;
    }

    Switch *boundSwitch(const QString &id)
    {
        auto *control = new Switch(card);
        QObject::connect(control, &QAbstractButton::toggled, card, [id](bool on) { R::instance()->setValue(id, on); });
        follow([control, id]() { control->show(R::instance()->value(id).toBool()); });
        return control;
    }

    QString labelOf(const QString &id) const
    {
        const Setting *s = registry()->setting(id);
        return s ? s->label : id;
    }

    QWidget *row(const QString &label, QWidget *control, const QString &id = QString(), bool controlBelow = false)
    {
        auto *row = new QWidget(card);
        auto *text = new QLabel(label, row);
        text->setWordWrap(true);
        auto *box = controlBelow ? static_cast<QBoxLayout *>(new QVBoxLayout(row)) : static_cast<QBoxLayout *>(new QHBoxLayout(row));
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(controlBelow ? 4 : 10);
        // the label's line: the label, and the mark of one that waits for a restart
        auto *line = controlBelow ? new QHBoxLayout() : static_cast<QHBoxLayout *>(box);
        if (controlBelow) {
            line->setSpacing(10);
            box->addLayout(line);
        }
        line->addWidget(text, 1);
        if (const Setting *s = id.isEmpty() ? nullptr : registry()->setting(id)) {
            QStringList notes;
            if (!s->help.isEmpty()) notes << s->help;
            if (s->flags & R::Restart) notes << i18nc("@info:tooltip", "After a restart");
            if (s->flags & R::NewViews) notes << i18nc("@info:tooltip", "For images opened from now on");
            if (s->flags & R::Machine) notes << i18nc("@info:tooltip", "About this computer: not exported");
            if (!notes.isEmpty()) text->setToolTip(notes.join(QStringLiteral("\n\n")));
            // explained on hover: a mark says so
            if (!s->help.isEmpty()) {
                auto *info = new QLabel(row);
                info->setPixmap(KisIconUtils::loadIcon(QStringLiteral("studio_info")).pixmap(12, 12));
                info->setToolTip(text->toolTip());
                line->insertWidget(1, info);
            }
            if (s->flags & R::Restart) {
                auto *mark = new QLabel(row);
                mark->setPixmap(KisIconUtils::loadIcon(QStringLiteral("studio_refresh")).pixmap(12, 12));
                mark->setToolTip(i18nc("@info:tooltip", "After a restart"));
                line->insertWidget(1, mark);
            }
        }
        if (controlBelow) box->addWidget(control);
        else box->addWidget(control, 0, Qt::AlignRight | Qt::AlignVCenter);
        row->setMinimumHeight(controlBelow ? 0 : 30);
        layout->addWidget(row);
        return row;
    }

    QWidget *toggle(const QString &id, const QString &label = QString())
    {
        return row(label.isEmpty() ? labelOf(id) : label, boundSwitch(id), id);
    }

    QWidget *number(const QString &id, const Number &look = Number())
    {
        const Setting *s = registry()->setting(id);
        if (!s) return nullptr;
        const qreal low = std::isnan(look.low) ? qMin(s->min * look.scale, s->max * look.scale) : look.low;
        const qreal high = std::isnan(look.high) ? qMax(s->min * look.scale, s->max * look.scale) : look.high;
        QWidget *control = nullptr;
        if (look.decimals == 0) {
            auto *slider = quietSlider<KisSliderSpinBox>(card);
            slider->setRange(int(std::ceil(low - 1e-9)), int(std::floor(high + 1e-9)));
            slider->setSingleStep(int(look.step));
            control = bindSlider(slider, id, look);
        } else {
            auto *slider = quietSlider<KisDoubleSliderSpinBox>(card);
            slider->setRange(low, high, look.decimals);
            slider->setSingleStep(look.step);
            control = bindSlider(slider, id, look);
        }
        return row(look.label.isEmpty() ? s->label : look.label, control, id);
    }

    template<class Slider>
    Slider *bindSlider(Slider *slider, const QString &id, const Number &look)
    {
        using Value = decltype(slider->value());
        const qreal scale = look.scale;
        slider->setBlockUpdateSignalOnDrag(look.onRelease);
        if (!look.suffix.isEmpty()) slider->setSuffix(look.suffix);
        follow([slider, id, scale]() {
            const qreal shown = R::instance()->value(id).toDouble() * scale;
            const Value value = std::is_integral<Value>::value ? Value(qRound(shown)) : Value(shown);
            if (std::abs(slider->value() - value) < 1e-9) return;
            const QSignalBlocker blocker(slider);
            slider->setValue(value);
        });
        QObject::connect(slider, QOverload<Value>::of(&Slider::valueChanged), card, [id, scale](Value value) {
            const Setting *s = R::instance()->setting(id);
            const qreal stored = value / scale;
            R::instance()->setValue(id, s && s->fallback.userType() == QMetaType::Int ? QVariant(qRound(stored)) : QVariant(stored));
        });
        return slider;
    }

    /// Side by side when they are few and short, else a list
    QWidget *choice(const QString &id, const QString &label = QString())
    {
        const Setting *s = registry()->setting(id);
        if (!s) return nullptr;
        const QVector<R::Choice> choices = registry()->choices(*s);
        QStringList texts;
        Q_FOREACH (const R::Choice &c, choices) texts << c.text;
        auto indexOf = [choices](const QVariant &value) {
            for (int i = 0; i < choices.size(); i++) {
                if (choices[i].value == value) return i;
            }
            return -1;
        };
        const QString name = label.isEmpty() ? s->label : label;
        if (choices.size() <= 4 && Segmented::widthFor(texts, card->fontMetrics()) <= 300) {
            auto *control = new Segmented(texts, card);
            control->picked = [id, choices](int i) { R::instance()->setValue(id, choices[i].value); };
            follow([control, id, indexOf]() { control->setCurrent(indexOf(R::instance()->value(id))); });
            return row(name, control, id);
        }
        auto *combo = new QComboBox(card);
        Q_FOREACH (const R::Choice &c, choices) combo->addItem(c.text, c.value);
        combo->setMaximumWidth(controlWidth + 30);
        combo->setMinimumWidth(qMin(controlWidth, combo->sizeHint().width()));
        follow([combo, id, indexOf]() {
            const QSignalBlocker blocker(combo);
            combo->setCurrentIndex(indexOf(R::instance()->value(id)));
        });
        QObject::connect(combo, QOverload<int>::of(&QComboBox::activated), card, [combo, id](int i) {
            if (i >= 0) R::instance()->setValue(id, combo->itemData(i));
        });
        return row(name, combo, id);
    }

    QWidget *color(const QString &id, bool alpha = false, const QString &label = QString())
    {
        auto *button = new KisColorButton(card);
        button->setAlphaChannelEnabled(alpha);
        button->setFixedSize(46, 24);
        follow([button, id]() {
            const QColor value = R::instance()->value(id).value<QColor>();
            if (button->color().toQColor().rgba() == value.rgba()) return;
            const QSignalBlocker blocker(button);
            button->setColor(KoColor(value, KoColorSpaceRegistry::instance()->rgb8()));
        });
        QObject::connect(button, &KisColorButton::changed, card, [id](const KoColor &c) { R::instance()->setValue(id, c.toQColor()); });
        return row(label.isEmpty() ? labelOf(id) : label, button, id);
    }

    QWidget *text(const QString &id, const QString &label = QString())
    {
        auto *edit = new QLineEdit(card);
        edit->setFixedWidth(controlWidth / 2);
        QObject::connect(edit, &QLineEdit::editingFinished, card, [edit, id]() {
            if (!R::instance()->setValue(id, edit->text())) edit->setText(R::instance()->value(id).toString());
        });
        follow([edit, id]() {
            if (!edit->hasFocus()) edit->setText(R::instance()->value(id).toString());
        });
        return row(label.isEmpty() ? labelOf(id) : label, edit, id);
    }

    /// A folder, chosen with the file dialog; one that isn't writable is refused
    QWidget *folder(const QString &id)
    {
        auto *box = new QWidget(card);
        auto *line = new QHBoxLayout(box);
        line->setContentsMargins(0, 0, 0, 0);
        auto *path = dimLabel(QString(), box);
        path->setWordWrap(false);
        path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *choose = new QPushButton(i18nc("@action:button choose a folder", "Choose"), box);
        line->addWidget(path, 1);
        line->addWidget(choose);
        follow([path, id]() {
            const QString value = QDir::toNativeSeparators(R::instance()->value(id).toString());
            path->setText(path->fontMetrics().elidedText(value, Qt::ElideMiddle, 300));
            path->setToolTip(value);
        });
        QWidget *parent = card;
        QObject::connect(choose, &QPushButton::clicked, card, [parent, id]() {
            KoFileDialog dialog(parent, KoFileDialog::OpenDirectory, QStringLiteral("StudioSettingsFolder"));
            dialog.setDefaultDir(R::instance()->value(id).toString());
            const QString chosen = dialog.filename();
            if (!chosen.isEmpty() && !R::instance()->setValue(id, QDir::cleanPath(chosen))) {
                say(parent, i18nc("@info", "That folder can't be written to."));
            }
        });
        return row(labelOf(id), box, id, true);
    }

    QWidget *button(const QString &text, std::function<void()> clicked)
    {
        return buttons({qMakePair(text, clicked)});
    }

    /// Buttons side by side
    QWidget *buttons(const QVector<QPair<QString, std::function<void()>>> &list)
    {
        auto *holder = new QWidget(card);
        auto *line = new QHBoxLayout(holder);
        line->setContentsMargins(0, 2, 0, 2);
        for (const auto &entry : list) {
            auto *button = new QPushButton(entry.first, holder);
            QObject::connect(button, &QPushButton::clicked, card, entry.second);
            line->addWidget(button);
        }
        line->addStretch(1);
        layout->addWidget(holder);
        return holder;
    }

    QLabel *note(const QString &text)
    {
        QLabel *label = dimLabel(text, card);
        layout->addWidget(label);
        return label;
    }

    /// What the card is about, said on hover over its title, an (i) beside it
    void explain(const QString &text)
    {
        if (!title || text.isEmpty()) return;
        const QString before = title->toolTip();
        title->setToolTip(before.isEmpty() ? text : before + QStringLiteral("\n\n") + text);
        if (!title->property("explained").toBool()) {
            title->setProperty("explained", true);
            auto *info = new QLabel(card);
            info->setPixmap(KisIconUtils::loadIcon(QStringLiteral("studio_info")).pixmap(12, 12));
            header->insertWidget(header->indexOf(title) + 1, info);
            info->setToolTip(title->toolTip());
            title->setProperty("explainedMark", QVariant::fromValue<QObject *>(info));
        } else if (auto *info = qobject_cast<QLabel *>(title->property("explainedMark").value<QObject *>())) {
            info->setToolTip(title->toolTip());
        }
    }

    /// \p widget shown only while \p when holds
    void showWhen(QWidget *widget, std::function<bool()> when)
    {
        if (!widget) return;
        follow([widget, when]() { widget->setVisible(when()); });
    }

    std::function<bool()> isOn(const QString &id) const
    {
        return [id]() { return R::instance()->value(id).toBool(); };
    }
};

struct CardDef {
    QString id;
    QString section;
    QString title;
    QStringList settings; ///< what Reset resets, what marks it changed
    QString aliases;      ///< more words that find it
    std::function<void(Builder &)> build;
    /// One switch: Find shows it and flips it
    QString toggleId;
};

struct SectionDef {
    QString id;
    QString title;
    QString icon;
};

QVector<SectionDef> builtinSections()
{
    return {
        {QStringLiteral("appearance"), i18nc("settings section", "Appearance"), QStringLiteral("studio_effects")},
        {QStringLiteral("canvas"), i18nc("settings section", "Canvas"), QStringLiteral("studio_pointer")},
        {QStringLiteral("pen"), i18nc("settings section", "Pen"), QStringLiteral("studio_pressure")},
        {keysSection, i18nc("settings section", "Keys"), QStringLiteral("config-keyboard")},
        {QStringLiteral("tools"), i18nc("settings section", "Tools"), QStringLiteral("studio_tool_options")},
        {QStringLiteral("files"), i18nc("settings section", "Files"), QStringLiteral("document-save")},
        {QStringLiteral("color"), i18nc("settings section", "Color"), QStringLiteral("studio_color")},
        {QStringLiteral("speed"), i18nc("settings section", "Speed"), QStringLiteral("config-performance")},
    };
}

QString sectionTitle(const QString &id)
{
    Q_FOREACH (const SectionDef &section, builtinSections()) {
        if (section.id == id) return section.title;
    }
    return QString();
}

/// The colors of a color scheme file, for its picture
struct SchemeLook {
    QColor window, view, text, selection, button;
};

/// A tiny window in \p look's colors
void paintScheme(QPainter &painter, const QRectF &r, const SchemeLook &look)
{
    painter.fillRect(r, look.window);
    painter.setPen(Qt::NoPen);
    painter.setBrush(look.view);
    painter.drawRoundedRect(QRectF(r.left() + r.width() * 0.08, r.top() + r.height() * 0.2, r.width() * 0.56, r.height() * 0.66), 3, 3);
    painter.setBrush(look.button);
    painter.drawRoundedRect(QRectF(r.left() + r.width() * 0.7, r.top() + r.height() * 0.2, r.width() * 0.22, r.height() * 0.66), 3, 3);
    painter.setBrush(look.selection);
    painter.drawRoundedRect(QRectF(r.left() + r.width() * 0.12, r.top() + r.height() * 0.28, r.width() * 0.48, r.height() * 0.13), 2, 2);
    painter.setBrush(look.text);
    for (int i = 0; i < 3; i++) {
        painter.drawRoundedRect(QRectF(r.left() + r.width() * 0.12, r.top() + r.height() * (0.5 + 0.12 * i), r.width() * (0.4 - 0.08 * i), r.height() * 0.05), 1, 1);
    }
}

/// The sample picture on what is around it: \p glow its colors lighting black, as on the canvas
void paintSurround(QPainter &painter, const QRectF &r, const QColor &around, bool glow)
{
    static const QImage sample = KisStudioPicker::samplePicture(QSize(120, 70));
    painter.fillRect(r, around);
    const QRectF image(r.left() + r.width() * 0.22, r.top() + r.height() * 0.2, r.width() * 0.56, r.height() * 0.6);
    if (glow) {
        const QImage spill = sample.scaled(6, 4, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        painter.save();
        painter.setOpacity(0.55);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(image.adjusted(-r.width() * 0.2, -r.height() * 0.2, r.width() * 0.2, r.height() * 0.2), spill);
        painter.restore();
        QRadialGradient fade(r.center(), r.width() * 0.62);
        fade.setColorAt(0.45, QColor(0, 0, 0, 0));
        fade.setColorAt(1.0, QColor(0, 0, 0, 255));
        painter.fillRect(r, fade);
    }
    painter.drawImage(image, sample);
}

QVector<CardDef> makeCards()
{
    QVector<CardDef> cards;
    auto add = [&cards](const QString &section, const QString &id, const QString &title, const QStringList &settings, const QString &aliases,
                        std::function<void(Builder &)> build) {
        cards << CardDef{QStringLiteral("%1.%2").arg(section, id), section, title, settings, aliases, build, QString()};
    };
    /// A card that is one switch, explained as its setting is
    auto addToggle = [&cards](const QString &section, const QString &id, const QString &title, const QString &setting, const QString &aliases) {
        cards << CardDef{QStringLiteral("%1.%2").arg(section, id), section, title, {setting}, aliases, std::function<void(Builder &)>(), setting};
    };

    // ---- appearance
    add(QStringLiteral("appearance"), QStringLiteral("theme"), i18nc("settings card", "Theme"),
        {QStringLiteral("[theme] Theme"), QStringLiteral("[StudioTheme] Lightness"), QStringLiteral("[StudioTheme] Tint"),
         QStringLiteral("[StudioTheme] TintAmount"), QStringLiteral("[StudioTheme] UseAccent"), QStringLiteral("[StudioTheme] Accent"),
         QStringLiteral("[StudioTheme] Contrast")},
        QStringLiteral("dark light colors color scheme palette accent tint contrast lightness"), [](Builder &b) {
            b.explain(i18nc("@info:tooltip", "Start from a preset, then make it yours: every choice below changes the theme."));
            // the presets, each pictured in its own colors
            const QVector<KisStudioTheme::Preset> presets = KisStudioTheme::presets();
            QVector<Tiles::Tile> tiles;
            Q_FOREACH (const KisStudioTheme::Preset &preset, presets) {
                const KisStudioTheme::Look l = KisStudioTheme::look(preset.params);
                const SchemeLook look{l.window, l.view, l.text, l.selection, l.button};
                tiles << Tiles::Tile{preset.name, [look](QPainter &painter, const QRectF &r) { paintScheme(painter, r, look); }};
            }
            auto *picker = new Tiles(tiles, QSize(96, 56), b.card);
            picker->picked = [presets](int i) { KisStudioTheme::apply(presets[i].params); };
            b.follow([picker, presets]() {
                int current = -1;
                if (R::instance()->value(QStringLiteral("[theme] Theme")).toString() == KisStudioTheme::schemeName()) {
                    const KisStudioTheme::Params now = KisStudioTheme::current();
                    for (int i = 0; i < presets.size() && current < 0; i++) {
                        if (presets[i].params == now) current = i;
                    }
                }
                picker->setCurrent(current);
            });
            b.layout->addWidget(picker);
            // the choices it is made of; costly to take, so taken when a slider is let go
            Number lightness(i18nc("theme", "Lightness"), 100, 0, QStringLiteral("%"));
            lightness.onRelease = true;
            b.number(QStringLiteral("[StudioTheme] Lightness"), lightness);
            b.color(QStringLiteral("[StudioTheme] Tint"), false, i18nc("theme", "Tint"));
            Number amount(i18nc("theme", "Tint strength"), 100, 0, QStringLiteral("%"));
            amount.onRelease = true;
            b.number(QStringLiteral("[StudioTheme] TintAmount"), amount);
            b.toggle(QStringLiteral("[StudioTheme] UseAccent"), i18nc("theme", "Accent color"));
            b.showWhen(b.color(QStringLiteral("[StudioTheme] Accent"), false, i18nc("theme", "Accent")), b.isOn(QStringLiteral("[StudioTheme] UseAccent")));
            Number contrast(i18nc("theme", "Contrast"), 100, 0, QStringLiteral("%"));
            contrast.onRelease = true;
            b.number(QStringLiteral("[StudioTheme] Contrast"), contrast);
        });
    add(QStringLiteral("appearance"), QStringLiteral("glass"), i18nc("settings card", "Glass"),
        {QStringLiteral("StudioGlass"), QStringLiteral("StudioGlassBlur"), QStringLiteral("StudioGlassOpacity"), QStringLiteral("StudioGlassRefraction")},
        QStringLiteral("transparency blur frosted opacity tint refraction see-through panels edge bends the canvas"), [](Builder &b) {
            b.toggle(QStringLiteral("StudioGlass"), i18nc("glass", "Panels of glass"));
            const auto on = b.isOn(QStringLiteral("StudioGlass"));
            b.showWhen(b.number(QStringLiteral("StudioGlassBlur"), {i18nc("glass", "Blur"), 100, 0, QStringLiteral("%")}), on);
            b.showWhen(b.number(QStringLiteral("StudioGlassOpacity"), {i18nc("glass", "Opacity"), 100, 0, QStringLiteral("%")}), on);
            b.showWhen(b.number(QStringLiteral("StudioGlassRefraction"), {i18nc("glass", "Edge bends the canvas"), 100, 0, QStringLiteral("%")}), on);
        });
    add(QStringLiteral("appearance"), QStringLiteral("surround"), i18nc("settings card", "Around the image"), {QStringLiteral("canvasBorderColor")},
        QStringLiteral("canvas border background color ambient glow"), [](Builder &b) {
            auto *picker = new Tiles({{i18nc("around the image", "Glow"), [](QPainter &p, const QRectF &r) { paintSurround(p, r, Qt::black, true); }},
                                      {i18nc("around the image", "Color"), [](QPainter &p, const QRectF &r) {
                                           // the color it is, or the one it would take (black is the glow)
                                           const QColor c = R::instance()->value(QStringLiteral("canvasBorderColor")).value<QColor>();
                                           paintSurround(p, r, c == QColor(Qt::black) ? QColor(48, 48, 52) : c, false);
                                       }}},
                                     QSize(128, 72), b.card);
            // black is the glow (KisStudioGlass::ambientSurround)
            auto current = []() { return R::instance()->value(QStringLiteral("canvasBorderColor")).value<QColor>() == QColor(Qt::black) ? 0 : 1; };
            picker->picked = [](int i) {
                R::instance()->setValue(QStringLiteral("canvasBorderColor"), i == 0 ? QColor(Qt::black) : QColor(48, 48, 52));
            };
            b.follow([picker, current]() {
                picker->setCurrent(current());
                picker->update();
            });
            b.layout->addWidget(picker);
            b.showWhen(b.color(QStringLiteral("canvasBorderColor"), false, i18nc("around the image", "Color")), [current]() { return current() == 1; });
            b.explain(i18nc("@info:tooltip", "Glow: the picture's colors light the dark around it. Color: one color all around."));
        });
    add(QStringLiteral("appearance"), QStringLiteral("font"), i18nc("settings card", "Interface font"),
        {QStringLiteral("use_custom_system_font"), QStringLiteral("custom_system_font"), QStringLiteral("custom_font_size")},
        QStringLiteral("text size typeface font"), [](Builder &b) {
            b.toggle(QStringLiteral("use_custom_system_font"));
            auto *family = new QFontComboBox(b.card);
            family->setMaximumWidth(controlWidth + 30);
            b.follow([family]() {
                QString name = R::instance()->value(QStringLiteral("custom_system_font")).toString();
                if (name.isEmpty()) name = qApp->font().family();
                const QSignalBlocker blocker(family);
                family->setCurrentFont(QFont(name));
            });
            QObject::connect(family, &QFontComboBox::currentFontChanged, b.card,
                             [](const QFont &font) { R::instance()->setValue(QStringLiteral("custom_system_font"), font.family()); });
            QWidget *familyRow = b.row(i18nc("interface font", "Font"), family, QStringLiteral("custom_system_font"));
            Number sizeLook(i18nc("interface font", "Size"), 1, 0, i18nc("font size unit", " pt"));
            // 6 and less is the system's size (KisUiFont)
            sizeLook.low = 7;
            sizeLook.high = 30;
            // every step lays the whole window out again
            sizeLook.onRelease = true;
            QWidget *size = b.number(QStringLiteral("custom_font_size"), sizeLook);
            b.showWhen(familyRow, b.isOn(QStringLiteral("use_custom_system_font")));
            b.showWhen(size, b.isOn(QStringLiteral("use_custom_system_font")));
        });
    add(QStringLiteral("appearance"), QStringLiteral("brushes"), i18nc("settings card", "Brushes"), {QStringLiteral("PresetStrokeThumbnails")},
        QStringLiteral("brush thumbnails strokes presets"), [](Builder &b) {
            b.toggle(QStringLiteral("PresetStrokeThumbnails"), i18nc("setting", "Show brushes as strokes"));
        });
    if (R::instance()->setting(QStringLiteral("kritadisplayrc EnableHiDPIFractionalScaling"))) {
        addToggle(QStringLiteral("appearance"), QStringLiteral("screen"), i18nc("settings card", "Fractional scaling"),
                  QStringLiteral("kritadisplayrc EnableHiDPIFractionalScaling"), QStringLiteral("hidpi scaling dpi screen scale 125% 150%"));
    }

    // ---- canvas
    add(QStringLiteral("canvas"), QStringLiteral("cursor"), i18nc("settings card", "Brush cursor"),
        {QStringLiteral("newCursorStyle"), QStringLiteral("newOutlineStyle"), QStringLiteral("ShowOutlineWhilePainting"), QStringLiteral("cursorMaincColor")},
        QStringLiteral("pointer outline crosshair color while painting"), [](Builder &b) {
            b.choice(QStringLiteral("newOutlineStyle"), i18nc("brush cursor", "Outline"));
            b.choice(QStringLiteral("newCursorStyle"), i18nc("brush cursor", "Pointer"));
            b.toggle(QStringLiteral("ShowOutlineWhilePainting"));
            b.color(QStringLiteral("cursorMaincColor"));
        });
    add(QStringLiteral("canvas"), QStringLiteral("eraser"), i18nc("settings card", "Eraser cursor"),
        {QStringLiteral("separateEraserCursor"), QStringLiteral("eraserCursorStyle"), QStringLiteral("eraserOutlineStyle"),
         QStringLiteral("ShowEraserOutlineWhilePainting"), QStringLiteral("eraserCursorMaincColor")},
        QStringLiteral("pointer outline"), [](Builder &b) {
            b.toggle(QStringLiteral("separateEraserCursor"));
            const auto on = b.isOn(QStringLiteral("separateEraserCursor"));
            b.showWhen(b.choice(QStringLiteral("eraserOutlineStyle"), i18nc("brush cursor", "Outline")), on);
            b.showWhen(b.choice(QStringLiteral("eraserCursorStyle"), i18nc("brush cursor", "Pointer")), on);
            b.showWhen(b.toggle(QStringLiteral("ShowEraserOutlineWhilePainting"), i18nc("setting", "Outline while painting")), on);
            b.showWhen(b.color(QStringLiteral("eraserCursorMaincColor"), false, i18nc("setting", "Outline color")), on);
        });
    add(QStringLiteral("canvas"), QStringLiteral("sampler"), i18nc("settings card", "Color sampler"),
        {QStringLiteral("colorSamplerPreviewStyle"), QStringLiteral("colorSamplerPreviewCircleDiameter"),
         QStringLiteral("colorSamplerPreviewCircleExtraCirclesEnabled"), QStringLiteral("colorSamplerZoomPreviewEnabled"),
         QStringLiteral("colorSamplerZoomPreviewScale")},
        QStringLiteral("eyedropper picker preview ring size magnifier magnification show the old color too"), [](Builder &b) {
            b.choice(QStringLiteral("colorSamplerPreviewStyle"), i18nc("color sampler", "Preview"));
            auto ring = []() { return R::instance()->value(QStringLiteral("colorSamplerPreviewStyle")).toInt() == 1; };
            b.showWhen(b.number(QStringLiteral("colorSamplerPreviewCircleDiameter"), {i18nc("color sampler", "Ring size"), 1, 0, i18nc("unit", " px")}), ring);
            b.showWhen(b.toggle(QStringLiteral("colorSamplerPreviewCircleExtraCirclesEnabled"), i18nc("color sampler", "Show the old color too")), ring);
            b.showWhen(b.toggle(QStringLiteral("colorSamplerZoomPreviewEnabled"), i18nc("color sampler", "Magnifier")), ring);
            auto magnified = [ring]() { return ring() && R::instance()->value(QStringLiteral("colorSamplerZoomPreviewEnabled")).toBool(); };
            b.showWhen(b.number(QStringLiteral("colorSamplerZoomPreviewScale"), {i18nc("color sampler", "Magnification"), 1, 0, QStringLiteral("x")}), magnified);
        });
    add(QStringLiteral("canvas"), QStringLiteral("checkerboard"), i18nc("settings card", "Checkerboard"),
        {QStringLiteral("checkerscolor"), QStringLiteral("checkerscolor2"), QStringLiteral("checksize")},
        QStringLiteral("transparency checkers background transparent squares other squares size"), [](Builder &b) {
            b.color(QStringLiteral("checkerscolor"), false, i18nc("checkerboard", "Squares"));
            b.color(QStringLiteral("checkerscolor2"), false, i18nc("checkerboard", "Other squares"));
            b.number(QStringLiteral("checksize"), {i18nc("checkerboard", "Size"), 1, 0, i18nc("unit", " px")});
        });
    add(QStringLiteral("canvas"), QStringLiteral("pixelgrid"), i18nc("settings card", "Pixel grid"),
        {QStringLiteral("pixelGridColor"), QStringLiteral("pixelGridDrawingThreshold")}, QStringLiteral("grid pixels zoom color shown from zoom"), [](Builder &b) {
            b.color(QStringLiteral("pixelGridColor"), false, i18nc("pixel grid", "Color"));
            b.number(QStringLiteral("pixelGridDrawingThreshold"), {i18nc("pixel grid", "Shown from zoom"), 100, 0, QStringLiteral("%"), 100});
        });
    add(QStringLiteral("canvas"), QStringLiteral("selection"), i18nc("settings card", "Selection"),
        {QStringLiteral("selectionOutlineOpacity"), QStringLiteral("selectionOverlayMaskColor"), QStringLiteral("selectionActionBar")},
        QStringLiteral("marching ants outline mask overlay color actions bar"), [](Builder &b) {
            b.number(QStringLiteral("selectionOutlineOpacity"), {i18nc("selection", "Outline"), 100, 0, QStringLiteral("%")});
            b.color(QStringLiteral("selectionOverlayMaskColor"), true, i18nc("selection", "Mask color"));
            b.toggle(QStringLiteral("selectionActionBar"), i18nc("selection", "Actions bar"));
        });
    add(QStringLiteral("canvas"), QStringLiteral("zoom"), i18nc("settings card", "Zoom"),
        {QStringLiteral("zoomSteps"), QStringLiteral("zoomHorizontal")}, QStringLiteral("zooming steps per doubling dragging sideways"),
        [](Builder &b) {
            b.number(QStringLiteral("zoomSteps"), {i18nc("zoom", "Steps per doubling")});
            b.toggle(QStringLiteral("zoomHorizontal"));
        });
    add(QStringLiteral("canvas"), QStringLiteral("drawing"), i18nc("settings card", "Drawing"),
        {QStringLiteral("OpenGLFilterMode"), QStringLiteral("showSingleChannelAsColor")},
        QStringLiteral("scaling filtering smooth pixel art nearest channels"), [](Builder &b) {
            b.choice(QStringLiteral("OpenGLFilterMode"), i18nc("canvas", "Scaling"));
            b.toggle(QStringLiteral("showSingleChannelAsColor"));
        });
    addToggle(QStringLiteral("canvas"), QStringLiteral("messages"), i18nc("settings card", "Messages on the canvas"),
              QStringLiteral("showOnCanvasMessages"), QStringLiteral("notifications floating messages popups"));
    // ---- pen
    add(QStringLiteral("pen"), QStringLiteral("pressure"), i18nc("settings card", "Pressure"), {QStringLiteral("tabletPressureCurve")},
        QStringLiteral("pressure curve tablet stylus sensitivity"), [](Builder &b) {
            auto *curve = new KisCurveWidget(b.card);
            curve->setMinimumHeight(150);
            curve->setMaximumSize(QWIDGETSIZE_MAX, 190);
            curve->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            b.follow([curve]() {
                const QString value = R::instance()->value(QStringLiteral("tabletPressureCurve")).toString();
                if (curve->curve().toString() == value) return;
                const QSignalBlocker blocker(curve);
                curve->setCurve(KisCubicCurve(value));
            });
            QObject::connect(curve, &KisCurveWidget::curveChanged, b.card,
                             [](const KisCubicCurve &c) { R::instance()->setValue(QStringLiteral("tabletPressureCurve"), c.toString()); });
            b.layout->addWidget(curve);
            b.explain(i18nc("@info:tooltip", "Across: how hard you press. Up: how much it paints."));
        });
    add(QStringLiteral("pen"), QStringLiteral("tablet"), i18nc("settings card", "Tablet"),
        {QStringLiteral("kritadisplayrc useWin8PointerInput"), QStringLiteral("useRightMiddleTabletButtonWorkaround")},
        QStringLiteral("wintab windows ink pointer api driver tester buttons"), [](Builder &b) {
            if (R::instance()->setting(QStringLiteral("kritadisplayrc useWin8PointerInput"))) {
                b.toggle(QStringLiteral("kritadisplayrc useWin8PointerInput"));
            }
            b.toggle(QStringLiteral("useRightMiddleTabletButtonWorkaround"));
            KisMainWindow *window = b.window;
            QVector<QPair<QString, std::function<void()>>> buttons;
            buttons << qMakePair(i18nc("@action:button", "Test the pen"), std::function<void()>([window]() {
                                     TabletTestDialog dialog(window);
                                     dialog.exec();
                                 }));
#ifdef Q_OS_WIN
            // where on the screens the tablet reaches: WinTab's
            buttons << qMakePair(i18nc("@action:button", "Tablet area"), std::function<void()>([window]() {
                                     KisDlgCustomTabletResolution dialog(window);
                                     dialog.exec();
                                 }));
#endif
            b.buttons(buttons);
        });
    add(QStringLiteral("pen"), QStringLiteral("tilt"), i18nc("settings card", "Tilt"), {QStringLiteral("tiltDirectionOffset")},
        QStringLiteral("tilt angle direction offset"), [](Builder &b) {
            // kept clockwise, shown counterclockwise as angles are
            b.number(QStringLiteral("tiltDirectionOffset"), {i18nc("tilt", "Direction offset"), -1, 0, QStringLiteral("°")});
        });
    add(QStringLiteral("pen"), QStringLiteral("touch"), i18nc("settings card", "Touch"),
        {QStringLiteral("touchPainting"), QStringLiteral("useTouchPressureSensitivity"), QStringLiteral("longPressEnabled")},
        QStringLiteral("finger touchscreen long press hold"), [](Builder &b) {
            b.choice(QStringLiteral("touchPainting"));
            b.showWhen(b.toggle(QStringLiteral("useTouchPressureSensitivity")),
                       []() { return R::instance()->value(QStringLiteral("touchPainting")).toInt() != 2; });
            b.toggle(QStringLiteral("longPressEnabled"));
        });
    addToggle(QStringLiteral("pen"), QStringLiteral("kinetic"), i18nc("settings card", "Kinetic scrolling"), QStringLiteral("KineticScrollingEnabled"),
              QStringLiteral("scrolling flick inertia"));

    // ---- tools
    add(QStringLiteral("tools"), QStringLiteral("selecting"), i18nc("settings card", "Selecting and pasting"),
        {QStringLiteral("switchSelectionCtrlAlt"), QStringLiteral("activateTransformToolAfterPaste")}, QStringLiteral("modifiers ctrl alt paste transform"),
        [](Builder &b) {
            b.toggle(QStringLiteral("switchSelectionCtrlAlt"));
            b.toggle(QStringLiteral("activateTransformToolAfterPaste"));
        });
    add(QStringLiteral("tools"), QStringLiteral("preview"), i18nc("settings card", "Instant preview"),
        {QStringLiteral("[filterdialog] forceLodMode"), QStringLiteral("[KisToolTransform] forceLodMode"), QStringLiteral("[KritaTransform/KisToolMove] forceLodMode")},
        QStringLiteral("lod level of detail preview fast effects transform move"), [](Builder &b) {
            b.toggle(QStringLiteral("[filterdialog] forceLodMode"), i18nc("instant preview", "Effects"));
            b.toggle(QStringLiteral("[KisToolTransform] forceLodMode"), i18nc("instant preview", "Transform"));
            b.toggle(QStringLiteral("[KritaTransform/KisToolMove] forceLodMode"), i18nc("instant preview", "Move"));
        });
    add(QStringLiteral("tools"), QStringLiteral("undo"), i18nc("settings card", "Undo"),
        {QStringLiteral("undoStackLimit"), QStringLiteral("useCumulativeUndoRedo")},
        QStringLiteral("history redo steps cumulative merge strokes"), [](Builder &b) {
            Number steps(i18nc("undo", "Steps"), 1, 0, QString(), 5);
            steps.onRelease = true;
            b.number(QStringLiteral("undoStackLimit"), steps);
            b.toggle(QStringLiteral("useCumulativeUndoRedo"));
        });
    add(QStringLiteral("tools"), QStringLiteral("layers"), i18nc("settings card", "Layers"),
        {QStringLiteral("StudioLayersRememberBrushes"), QStringLiteral("ConvertToImageColorSpaceOnImport")},
        QStringLiteral("brush import color space"), [](Builder &b) {
            b.toggle(QStringLiteral("StudioLayersRememberBrushes"));
            b.toggle(QStringLiteral("ConvertToImageColorSpaceOnImport"));
        });
    add(QStringLiteral("tools"), QStringLiteral("layernames"), i18nc("settings card", "Layer names"),
        {KisImageConfig::layerNameKey(KisImageConfig::CopiedLayer, true), KisImageConfig::layerNameKey(KisImageConfig::CopiedLayer, false),
         KisImageConfig::layerNameKey(KisImageConfig::PastedLayer, true), KisImageConfig::layerNameKey(KisImageConfig::PastedLayer, false),
         KisImageConfig::layerNameKey(KisImageConfig::MergedLayer, true), KisImageConfig::layerNameKey(KisImageConfig::MergedLayer, false)},
        QStringLiteral("copy of duplicate pasted merged rename prefix suffix naming"), [](Builder &b) {
            b.explain(i18nc("@info:tooltip", "The words a copied, pasted or merged layer gets around the name it comes from. Empty: none."));
            // [words before] Layer [words after], as the name will read
            auto *grid = new QGridLayout();
            grid->setHorizontalSpacing(6);
            grid->setVerticalSpacing(4);
            const QString kinds[] = {i18nc("layer names", "Copy"), i18nc("layer names", "Pasted"), i18nc("layer names", "Merged")};
            for (KisImageConfig::LayerNaming kind : {KisImageConfig::CopiedLayer, KisImageConfig::PastedLayer, KisImageConfig::MergedLayer}) {
                grid->addWidget(new QLabel(kinds[kind], b.card), kind, 0);
                for (bool before : {true, false}) {
                    const QString id = KisImageConfig::layerNameKey(kind, before);
                    auto *edit = new QLineEdit(b.card);
                    edit->setPlaceholderText(before ? i18nc("layer names: no words", "none") : i18nc("layer names: no words", "none"));
                    edit->setAlignment(before ? Qt::AlignRight : Qt::AlignLeft);
                    edit->setMaximumWidth(110);
                    QObject::connect(edit, &QLineEdit::editingFinished, b.card, [edit, id]() {
                        if (!R::instance()->setValue(id, edit->text().trimmed())) edit->setText(R::instance()->value(id).toString());
                    });
                    b.follow([edit, id]() {
                        if (!edit->hasFocus()) edit->setText(R::instance()->value(id).toString());
                    });
                    grid->addWidget(edit, kind, before ? 1 : 3);
                }
                // the name it comes from, as a chip
                auto *name = new QLabel(i18nc("a layer's name, in an example", "Layer"), b.card);
                name->setAlignment(Qt::AlignCenter);
                name->setContentsMargins(8, 2, 8, 2);
                name->setStyleSheet(QStringLiteral("QLabel { border-radius: 6px; background: palette(highlight); color: palette(highlighted-text); }"));
                grid->addWidget(name, kind, 2);
            }
            grid->setColumnStretch(1, 1);
            grid->setColumnStretch(3, 1);
            b.layout->addLayout(grid);
        });
    addToggle(QStringLiteral("tools"), QStringLiteral("effects"), i18nc("settings card", "Effects stay editable"), QStringLiteral("StudioEffectsEditable"),
              QStringLiteral("filter mask editable effects"));
    add(QStringLiteral("tools"), QStringLiteral("animation"), i18nc("settings card", "Animation"),
        {QStringLiteral("autoPinLayers"), QStringLiteral("adaptivePlaybackRange"), QStringLiteral("autoZoomTimelineToPlaybackRange")},
        QStringLiteral("timeline playback keyframes"), [](Builder &b) {
            b.toggle(QStringLiteral("autoPinLayers"));
            b.toggle(QStringLiteral("adaptivePlaybackRange"));
            b.toggle(QStringLiteral("autoZoomTimelineToPlaybackRange"));
        });
    add(QStringLiteral("tools"), QStringLiteral("brushengine"), i18nc("settings card", "Painting"),
        {QStringLiteral("maximumBrushSize")}, QStringLiteral("brush size largest"), [](Builder &b) {
            b.number(QStringLiteral("maximumBrushSize"), {QString(), 1, 0, i18nc("unit", " px"), 100});
        });

    // ---- files
    add(QStringLiteral("files"), QStringLiteral("autosave"), i18nc("settings card", "Autosave"),
        {QStringLiteral("AutoSaveInterval")}, QStringLiteral("auto save recovery crash interval every minutes"), [](Builder &b) {
            auto *on = new Switch(b.card);
            auto *minutes = quietSlider<KisSliderSpinBox>(b.card);
            minutes->setRange(1, 1440);
            minutes->setSuffix(i18nc("unit", " min"));
            b.follow([on, minutes]() {
                const int seconds = R::instance()->value(QStringLiteral("AutoSaveInterval")).toInt();
                on->show(seconds > 0);
                // off: turned on, it is Krita's interval again
                const int shown = seconds > 0 ? qMax(1, seconds / 60) : R::instance()->defaultValue(QStringLiteral("AutoSaveInterval")).toInt() / 60;
                if (minutes->value() != shown) {
                    const QSignalBlocker blocker(minutes);
                    minutes->setValue(shown);
                }
            });
            QObject::connect(on, &QAbstractButton::toggled, b.card, [minutes](bool checked) {
                R::instance()->setValue(QStringLiteral("AutoSaveInterval"), checked ? minutes->value() * 60 : 0);
            });
            QObject::connect(minutes, QOverload<int>::of(&KisSliderSpinBox::valueChanged), b.card,
                             [](int value) { R::instance()->setValue(QStringLiteral("AutoSaveInterval"), value * 60); });
            b.row(i18nc("autosave", "On"), on, QStringLiteral("AutoSaveInterval"));
            auto saving = []() { return R::instance()->value(QStringLiteral("AutoSaveInterval")).toInt() > 0; };
            b.showWhen(b.row(i18nc("autosave", "Every"), minutes), saving);
        });
    add(QStringLiteral("files"), QStringLiteral("backup"), i18nc("settings card", "Backups"),
        {QStringLiteral("CreateBackupFile"), QStringLiteral("backupfilelocation"), QStringLiteral("numberofbackupfiles")},
        QStringLiteral("backup copy old version"), [](Builder &b) {
            b.toggle(QStringLiteral("CreateBackupFile"));
            const auto on = b.isOn(QStringLiteral("CreateBackupFile"));
            b.showWhen(b.choice(QStringLiteral("backupfilelocation")), on);
            b.showWhen(b.number(QStringLiteral("numberofbackupfiles")), on);
        });
    add(QStringLiteral("files"), QStringLiteral("saving"), i18nc("settings card", "Saving"),
        {QStringLiteral("defaultExportMimeType"), QStringLiteral("compressLayersInKra"), QStringLiteral("TrimKra")},
        QStringLiteral("kra compression zip64 export format trim"), [](Builder &b) {
            b.choice(QStringLiteral("defaultExportMimeType"));
            b.toggle(QStringLiteral("compressLayersInKra"));
            b.toggle(QStringLiteral("TrimKra"));
        });
    add(QStringLiteral("files"), QStringLiteral("pasting"), i18nc("settings card", "Pasting from other apps"),
        {QStringLiteral("pasteFormat"), QStringLiteral("pasteBehaviour")}, QStringLiteral("clipboard paste drop browser take"), [](Builder &b) {
            b.choice(QStringLiteral("pasteFormat"), i18nc("pasting", "Take"));
            b.choice(QStringLiteral("pasteBehaviour"));
        });
    add(QStringLiteral("files"), QStringLiteral("sessions"), i18nc("settings card", "Starting"),
        {QStringLiteral("sessionOnStartup"), QStringLiteral("saveSessionOnQuit"), QStringLiteral("[File Dialogs] DontUseNativeFileDialog")},
        QStringLiteral("session startup start quit file dialogs native"), [](Builder &b) {
            b.choice(QStringLiteral("sessionOnStartup"));
            b.toggle(QStringLiteral("saveSessionOnQuit"));
            b.toggle(QStringLiteral("[File Dialogs] DontUseNativeFileDialog"));
        });
    add(QStringLiteral("files"), QStringLiteral("author"), i18nc("settings card", "Author"), {}, QStringLiteral("name profile contact metadata nickname"),
        [](Builder &b) {
            QLabel *who = b.note(QString());
            b.follow([who]() {
                const QString active = KSharedConfig::openConfig()->group("Author").readEntry("active-profile", QString());
                who->setText(active.isEmpty() ? i18nc("@info", "Documents are saved without an author's name.")
                                              : i18nc("@info", "Documents are saved by %1.", active));
            });
            KisMainWindow *window = b.window;
            b.button(i18nc("@action:button", "Edit authors"), [window]() {
                QDialog dialog(window);
                dialog.setWindowTitle(i18nc("@title:window", "Authors"));
                auto *layout = new QVBoxLayout(&dialog);
                auto *page = new KoConfigAuthorPage();
                layout->addWidget(page);
                auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
                QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
                QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
                layout->addWidget(buttons);
                if (dialog.exec() == QDialog::Accepted) {
                    page->apply();
                    // the File menu's list of authors
                    KisConfigNotifier::instance()->notifyConfigChanged();
                }
            });
        });
    add(QStringLiteral("files"), QStringLiteral("resources"), i18nc("settings card", "Resource folder"), {QStringLiteral("ResourceDirectory")},
        QStringLiteral("resources bundles brushes location data folder"), [](Builder &b) {
            b.folder(QStringLiteral("ResourceDirectory"));
        });

    // ---- color
    add(QStringLiteral("color"), QStringLiteral("newimages"), i18nc("settings card", "New images"),
        {QStringLiteral("useDefaultColorSpace"), QStringLiteral("workingColorSpace")}, QStringLiteral("color model depth default working space new images"),
        [](Builder &b) {
            b.toggle(QStringLiteral("useDefaultColorSpace"));
            b.showWhen(b.choice(QStringLiteral("workingColorSpace"), i18nc("new images", "Model")), b.isOn(QStringLiteral("useDefaultColorSpace")));
        });
    add(QStringLiteral("color"), QStringLiteral("display"), i18nc("settings card", "Display"),
        {QStringLiteral("renderIntent"), QStringLiteral("useBlackPointCompensation"), QStringLiteral("allowLCMSOptimization")},
        QStringLiteral("rendering intent black point lcms monitor display"), [](Builder &b) {
            b.choice(QStringLiteral("renderIntent"), i18nc("display", "Intent"));
            b.toggle(QStringLiteral("useBlackPointCompensation"));
            b.toggle(QStringLiteral("allowLCMSOptimization"));
        });
    add(QStringLiteral("color"), QStringLiteral("misc"), i18nc("settings card", "Palettes and EXR"),
        {QStringLiteral("colorsettings/forcepalettecolors"), QStringLiteral("ExrDefaultColorProfile")}, QStringLiteral("palette exr profile"), [](Builder &b) {
            b.toggle(QStringLiteral("colorsettings/forcepalettecolors"));
            b.choice(QStringLiteral("ExrDefaultColorProfile"), i18nc("color", "EXR files"));
        });
    {
        // each screen's profile (or how the system is handed the canvas), and the window's format
        QStringList screenSettings;
        Q_FOREACH (const Setting &s, R::instance()->settings()) {
            if (s.id.startsWith(QLatin1String("monitorProfile screen")) || s.id == QLatin1String("enableCanvasSurfaceColorSpaceManagement")
                || s.id == QLatin1String("canvasSurfaceColorSpaceManagementMode") || s.id == QLatin1String("canvasSurfaceBitDepthMode")
                || s.id == QLatin1String("kritadisplayrc rootSurfaceFormat")) {
                screenSettings << s.id;
            }
        }
        add(QStringLiteral("color"), QStringLiteral("screens"), i18nc("settings card", "Screens"), screenSettings,
            QStringLiteral("monitor profile screen display icc hdr calibration surface"), [screenSettings](Builder &b) {
                b.explain(i18nc("@info:tooltip", "The color profile of each screen, as calibrated: what makes the canvas show its colors right."));
                const bool osManaged = KisPlatformPluginInterfaceFactory::instance()->surfaceColorManagedByOS();
                Q_FOREACH (const QString &id, screenSettings) {
                    if (id == QLatin1String("canvasSurfaceColorSpaceManagementMode") || id == QLatin1String("canvasSurfaceBitDepthMode")) {
                        b.showWhen(b.choice(id), b.isOn(QStringLiteral("enableCanvasSurfaceColorSpaceManagement")));
                    } else if (R::instance()->setting(id)->kind == Kind::Toggle) {
                        b.toggle(id);
                    } else {
                        b.choice(id);
                    }
                }
                if (!osManaged) {
                    QWidget *card = b.card;
                    b.button(i18nc("@action:button", "Add profiles"), [card]() {
                        KoFileDialog dialog(card, KoFileDialog::OpenFiles, QStringLiteral("OpenDocumentICC"));
                        dialog.setCaption(i18nc("@title:window", "Add Color Profiles"));
                        dialog.setDefaultDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
                        dialog.setMimeTypeFilters({QStringLiteral("application/vnd.iccprofile")}, QStringLiteral("application/vnd.iccprofile"));
                        KoColorSpaceEngine *icc = KoColorSpaceEngineRegistry::instance()->get(QStringLiteral("icc"));
                        if (!icc) return;
                        const QString saveLocation = KoResourcePaths::saveLocation("icc_profiles");
                        Q_FOREACH (const QString &file, dialog.filenames()) {
                            const QString target = saveLocation + QFileInfo(file).fileName();
                            if (QFile::copy(file, target)) icc->addProfile(target);
                        }
                        // the new ones among the choices, every card's
                        R::instance()->refreshChoices();
                        Q_EMIT R::instance()->valueChanged(QString());
                    });
                }
            });
    }
    add(QStringLiteral("color"), QStringLiteral("proofing"), i18nc("settings card", "Soft proofing"),
        {QStringLiteral("defaultProofingProfileName"), QStringLiteral("defaultProofingConversionIntent"), QStringLiteral("defaultProofingBlackpointCompensation"),
         QStringLiteral("defaultProofingDisplayMode"), QStringLiteral("defaultProofingGamutwarning")},
        QStringLiteral("soft proofing print preview gamut warning paper cmyk printer"), [](Builder &b) {
            b.explain(i18nc("@info:tooltip", "How new images are proofed: shown as they would print (View > Soft Proofing)."));
            b.choice(QStringLiteral("defaultProofingProfileName"), i18nc("proofing", "Profile"));
            b.choice(QStringLiteral("defaultProofingConversionIntent"), i18nc("proofing", "Intent"));
            b.toggle(QStringLiteral("defaultProofingBlackpointCompensation"), i18nc("proofing", "Black point compensation"));
            b.choice(QStringLiteral("defaultProofingDisplayMode"), i18nc("proofing", "White"));
            b.color(QStringLiteral("defaultProofingGamutwarning"), false, i18nc("proofing", "Out of gamut"));
        });

    // ---- speed
    add(QStringLiteral("speed"), QStringLiteral("renderer"), i18nc("settings card", "Drawing the canvas"),
        {QStringLiteral("kritadisplayrc OpenGLRenderer")}, QStringLiteral("opengl angle direct3d renderer gpu acceleration graphics"), [](Builder &b) {
            b.choice(QStringLiteral("kritadisplayrc OpenGLRenderer"));
        });
    add(QStringLiteral("speed"), QStringLiteral("memory"), i18nc("settings card", "Memory"),
        {QStringLiteral("memoryHardLimitPercent"), QStringLiteral("memorySoftLimitPercent"), QStringLiteral("maxSwapSize"), QStringLiteral("swaplocation")},
        QStringLiteral("ram swap disk limit"), [](Builder &b) {
            const int ram = KisImageConfig::totalRAM();
            b.explain(i18nc("@info:tooltip", "This computer has %1 GB.", QLocale().toString(ram / 1024.0, 'f', 1)));
            b.number(QStringLiteral("memoryHardLimitPercent"), {QString(), 1, 0, QStringLiteral("%")});
            b.number(QStringLiteral("memorySoftLimitPercent"), {QString(), 1, 0, QStringLiteral("%")});
            b.number(QStringLiteral("maxSwapSize"), {QString(), 1.0 / 1024, 0, i18nc("unit", " GB")});
            b.folder(QStringLiteral("swaplocation"));
        });
    add(QStringLiteral("speed"), QStringLiteral("threads"), i18nc("settings card", "Processor"),
        {QStringLiteral("maxNumberOfThreads"), QStringLiteral("frameRenderingClones")},
        QStringLiteral("cpu threads cores rendering frames"), [](Builder &b) {
            Number threads;
            threads.onRelease = true;
            b.number(QStringLiteral("maxNumberOfThreads"), threads);
            b.number(QStringLiteral("frameRenderingClones"), threads);
        });
    add(QStringLiteral("speed"), QStringLiteral("framerate"), i18nc("settings card", "Frame rate"),
        {QStringLiteral("detectFpsLimit"), QStringLiteral("fpsLimit")}, QStringLiteral("fps refresh hz"), [](Builder &b) {
            b.toggle(QStringLiteral("detectFpsLimit"));
            b.showWhen(b.number(QStringLiteral("fpsLimit"), {QString(), 1, 0, i18nc("unit", " fps")}),
                       []() { return !R::instance()->value(QStringLiteral("detectFpsLimit")).toBool(); });
        });
    add(QStringLiteral("speed"), QStringLiteral("animationcache"), i18nc("settings card", "Animation cache"),
        {QStringLiteral("calculateAnimationCacheInBackground"), QStringLiteral("useOnDiskAnimationCacheSwapping")}, QStringLiteral("playback frames cache"),
        [](Builder &b) {
            b.toggle(QStringLiteral("calculateAnimationCacheInBackground"));
            b.toggle(QStringLiteral("useOnDiskAnimationCacheSwapping"));
        });
    add(QStringLiteral("speed"), QStringLiteral("diagnostics"), i18nc("settings card", "If Krita crashes when painting"),
        {QStringLiteral("disableAVXOptimizations"), QStringLiteral("amdDisableVectorWorkaround")}, QStringLiteral("crash avx vector instructions workaround"),
        [](Builder &b) {
            if (R::instance()->setting(QStringLiteral("disableAVXOptimizations"))) b.toggle(QStringLiteral("disableAVXOptimizations"));
            b.toggle(QStringLiteral("amdDisableVectorWorkaround"));
        });

    // settings this build doesn't have (another platform's) are left out of the cards
    for (CardDef &card : cards) {
        card.settings.erase(std::remove_if(card.settings.begin(), card.settings.end(),
                                           [](const QString &id) { return !R::instance()->setting(id); }),
                            card.settings.end());
    }
    return cards;
}

const QVector<CardDef> &allCards()
{
    static const QVector<CardDef> cards = makeCards();
    return cards;
}

const CardDef *cardDef(const QString &id)
{
    Q_FOREACH (const CardDef &card, allCards()) {
        if (card.id == id) return &card;
    }
    return nullptr;
}

bool cardChanged(const CardDef &card)
{
    Q_FOREACH (const QString &id, card.settings) {
        if (!R::instance()->isDefault(id)) return true;
    }
    return false;
}

/// A card's menu, Reset first (which it does itself)
class CardMenu : public QMenu
{
public:
    CardMenu(const CardDef &def, QWidget *parent)
        : QMenu(parent)
        , m_def(def)
    {
        m_reset = addAction(i18nc("@action:inmenu", "Reset"));
        m_reset->setEnabled(cardChanged(def));
    }

    /// What was chosen, other than Reset
    QAction *run(const QPoint &pos)
    {
        QAction *chosen = nullptr;
        {
            const Busy busy;
            chosen = exec(pos);
        }
        if (chosen != m_reset) return chosen;
        Q_FOREACH (const QString &id, m_def.settings) R::instance()->reset(id);
        return nullptr;
    }

private:
    const CardDef &m_def;
    QAction *m_reset {nullptr};
};

/// The index of a section of yours ("user:<n>"), -1 for the others
int userIndex(const QString &sectionId)
{
    return sectionId.startsWith(QLatin1String("user:")) ? sectionId.mid(5).toInt() : -1;
}

/// Your sections, and Quick, in kritarc
struct UserSections {
    QStringList quick;
    struct Section {
        QString name;
        QStringList cards;
    };
    QVector<Section> sections;

    static UserSections load()
    {
        UserSections us;
        const KConfigGroup group = KSharedConfig::openConfig()->group(QString());
        if (!group.hasKey(sectionsKey)) {
            // until changed: what is reached for most
            us.quick = QStringList{QStringLiteral("pen.pressure"), QStringLiteral("canvas.cursor"), QStringLiteral("appearance.glass"), QStringLiteral("canvas.pixelgrid")};
            return us;
        }
        const QJsonObject root = QJsonDocument::fromJson(group.readEntry(sectionsKey, QString()).toUtf8()).object();
        auto known = [](const QJsonArray &array) {
            QStringList ids;
            Q_FOREACH (const QJsonValue &value, array) {
                if (cardDef(value.toString()) && !ids.contains(value.toString())) ids << value.toString();
            }
            return ids;
        };
        us.quick = known(root.value(QStringLiteral("quick")).toArray());
        Q_FOREACH (const QJsonValue &value, root.value(QStringLiteral("sections")).toArray()) {
            const QJsonObject section = value.toObject();
            const QString name = section.value(QStringLiteral("name")).toString().trimmed();
            if (!name.isEmpty()) us.sections << Section{name.left(40), known(section.value(QStringLiteral("cards")).toArray())};
        }
        return us;
    }

    void save() const
    {
        QJsonObject root;
        root.insert(QStringLiteral("quick"), QJsonArray::fromStringList(quick));
        QJsonArray list;
        Q_FOREACH (const Section &section, sections) {
            QJsonObject object;
            object.insert(QStringLiteral("name"), section.name);
            object.insert(QStringLiteral("cards"), QJsonArray::fromStringList(section.cards));
            list.append(object);
        }
        root.insert(QStringLiteral("sections"), list);
        KisConfig(false).writeEntry(sectionsKey, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    }

    /// The cards of a section of yours or Quick; null for the others
    QStringList *cardsOf(const QString &sectionId)
    {
        if (sectionId == quickId) return &quick;
        const int i = userIndex(sectionId);
        return i >= 0 && i < sections.size() ? &sections[i].cards : nullptr;
    }

    /// A new section of yours, saved; its id
    static QString add(const QString &name, const QStringList &cards)
    {
        UserSections us = load();
        us.sections << Section{name, cards};
        us.save();
        return QStringLiteral("user:%1").arg(us.sections.size() - 1);
    }
};

/// A card: its title, its rows, a mark while any of its settings is changed
class Card : public QFrame
{
public:
    std::function<void(const QPoint &globalPos)> menuRequested;
    bool draggable {false};

    Card(const CardDef &def, KisMainWindow *window, QWidget *parent)
        : QFrame(parent)
        , m_def(def)
    {
        setObjectName(QStringLiteral("studioSettingsCard"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 8, 10, 10);
        layout->setSpacing(2);
        m_builder.card = this;
        m_builder.layout = layout;
        m_builder.window = window;

        auto *header = new QHBoxLayout();
        header->setSpacing(6);
        m_title = titleLabel(def.title, this);
        header->addWidget(m_title);
        m_builder.title = m_title;
        m_builder.header = header;
        m_mark = new QLabel(QStringLiteral("●"), this);
        QPalette palette = m_mark->palette();
        palette.setColor(QPalette::WindowText, palette.color(QPalette::Link));
        m_mark->setPalette(palette);
        QFont markFont = m_mark->font();
        markFont.setPointSizeF(markFont.pointSizeF() * 0.6);
        m_mark->setFont(markFont);
        m_mark->setToolTip(i18nc("@info:tooltip", "Changed from Krita's default"));
        header->addWidget(m_mark);
        header->addStretch(1);
        // a card that is one switch: it, by its title
        if (!def.toggleId.isEmpty()) header->addWidget(m_builder.boundSwitch(def.toggleId));
        m_more = new QToolButton(this);
        m_more->setAutoRaise(true);
        m_more->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_more")));
        m_more->setIconSize(QSize(14, 14));
        m_more->setToolTip(i18nc("@info:tooltip", "Reset, pin to Quick, add to a section"));
        connect(m_more, &QToolButton::clicked, this, [this]() {
            if (menuRequested) menuRequested(m_more->mapToGlobal(QPoint(0, m_more->height())));
        });
        header->addWidget(m_more);
        layout->addLayout(header);
        if (def.build) def.build(m_builder);
        if (const Setting *s = def.toggleId.isEmpty() ? nullptr : R::instance()->setting(def.toggleId)) m_builder.explain(s->help);

        connect(R::instance(), &R::valueChanged, this, [this]() { refresh(); });
        connect(KisConfigNotifier::instance(), &KisConfigNotifier::configChanged, this, [this]() { refresh(); });
        refresh();
    }

    ~Card() override
    {
        // what goes with it can still speak (a color dialog closing sets the color back): not to a card half gone
        Q_FOREACH (QWidget *child, findChildren<QWidget *>()) child->blockSignals(true);
        disconnect(R::instance(), nullptr, this, nullptr);
        disconnect(KisConfigNotifier::instance(), nullptr, this, nullptr);
    }

    const CardDef &def() const { return m_def; }

    void refresh()
    {
        // one of them can end up rebuilding the cards, this one too
        const QPointer<Card> alive(this);
        const QVector<std::function<void()>> followers = m_builder.refresh;
        for (const auto &f : followers) {
            f();
            if (!alive) return;
        }
        m_mark->setVisible(cardChanged(m_def));
    }

    /// Marks it for a moment: the one that was asked for
    void flash()
    {
        auto *animation = new QVariantAnimation(this);
        animation->setDuration(1400);
        animation->setStartValue(1.0);
        animation->setEndValue(0.0);
        connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_flash = value.toReal();
            update();
        });
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor fill = ink(this, 0.045);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.drawPath(KisModernStyle::squircle(r, 10));
        if (m_flash > 0) {
            QColor link = palette().color(QPalette::Link);
            link.setAlphaF(0.9 * m_flash);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(link, 1.5));
            painter.drawPath(KisModernStyle::squircle(r.adjusted(0.5, 0.5, -0.5, -0.5), 10));
        }
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (menuRequested) menuRequested(event->globalPos());
        event->accept();
    }

    /// Dragged by its title: onto a section of yours, or within one
    void mousePressEvent(QMouseEvent *event) override
    {
        m_pressed = event->button() == Qt::LeftButton && event->pos().y() < m_title->geometry().bottom() + 6 ? event->pos() : QPoint(-1, -1);
        QFrame::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!draggable || m_pressed.x() < 0 || !(event->buttons() & Qt::LeftButton)
            || (event->pos() - m_pressed).manhattanLength() < QApplication::startDragDistance()) {
            QFrame::mouseMoveEvent(event);
            return;
        }
        m_pressed = QPoint(-1, -1);
        auto *drag = new QDrag(this);
        auto *mime = new QMimeData();
        mime->setData(cardMime, m_def.id.toUtf8());
        drag->setMimeData(mime);
        QPixmap picture = grab(QRect(0, 0, width(), qMin(height(), 120)));
        if (picture.width() > 240) picture = picture.scaledToWidth(240, Qt::SmoothTransformation);
        drag->setPixmap(picture);
        drag->setHotSpot(QPoint(16, 12));
        const Busy busy;
        drag->exec(Qt::CopyAction | Qt::MoveAction);
    }

    void enterEvent(QEvent *) override { m_title->setCursor(draggable ? Qt::OpenHandCursor : Qt::ArrowCursor); }

private:
    CardDef m_def;
    Builder m_builder;
    QLabel *m_title {nullptr};
    QLabel *m_mark {nullptr};
    QToolButton *m_more {nullptr};
    QPoint m_pressed {-1, -1};
    qreal m_flash {0};
};

/// The cards of a section, one under another; in a section of yours, dragged to reorder or dropped in
class Page : public QWidget
{
public:
    /// Dropped here: \p cardId at \p index (in a section of yours)
    std::function<void(const QString &cardId, int index)> dropped;

    explicit Page(QWidget *parent)
        : QWidget(parent)
    {
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(0, 0, 4, 8);
        m_layout->setSpacing(8);
    }

    QVBoxLayout *box() const { return m_layout; }
    QVector<Card *> cards() const { return findChildren<Card *>(QString(), Qt::FindDirectChildrenOnly).toVector(); }

    void heading(const QString &text)
    {
        auto *label = new QLabel(text, this);
        QFont font = label->font();
        font.setPointSizeF(font.pointSizeF() * 0.82);
        font.setBold(true);
        font.setCapitalization(QFont::AllUppercase);
        font.setLetterSpacing(QFont::PercentageSpacing, 106);
        label->setFont(font);
        QPalette palette = label->palette();
        palette.setColor(QPalette::WindowText, KisModernStyle::secondaryInk(palette.color(QPalette::WindowText)));
        label->setPalette(palette);
        label->setContentsMargins(4, 8, 0, 0);
        m_layout->addWidget(label);
    }

protected:
    int dropIndex(const QPoint &pos) const
    {
        const QVector<Card *> list = cards();
        for (int i = 0; i < list.size(); i++) {
            if (pos.y() < list[i]->geometry().center().y()) return i;
        }
        return list.size();
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (dropped && event->mimeData()->hasFormat(cardMime)) event->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (!dropped || !event->mimeData()->hasFormat(cardMime)) return;
        event->acceptProposedAction();
        const int index = dropIndex(event->pos());
        if (index != m_dropAt) {
            m_dropAt = index;
            update();
        }
    }

    void dragLeaveEvent(QDragLeaveEvent *) override
    {
        m_dropAt = -1;
        update();
    }

    void dropEvent(QDropEvent *event) override
    {
        const int index = dropIndex(event->pos());
        m_dropAt = -1;
        update();
        if (!dropped || !event->mimeData()->hasFormat(cardMime)) return;
        event->acceptProposedAction();
        const QString id = QString::fromUtf8(event->mimeData()->data(cardMime));
        // after the drag is over: the card dragged may be rebuilt
        QTimer::singleShot(0, this, [this, id, index]() { dropped(id, index); });
    }

    void paintEvent(QPaintEvent *) override
    {
        if (m_dropAt < 0) return;
        const QVector<Card *> list = cards();
        int y = 2;
        if (!list.isEmpty()) y = m_dropAt < list.size() ? list[m_dropAt]->geometry().top() - 4 : list.last()->geometry().bottom() + 4;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Link));
        painter.drawRoundedRect(QRectF(6, y - 1.5, width() - 16, 3), 1.5, 1.5);
    }

private:
    QVBoxLayout *m_layout {nullptr};
    int m_dropAt {-1};
};

/// A section's name typed in place: Enter keeps it, Escape keeps the old one
class RenameEdit : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;
    std::function<void()> cancelled;
    bool finished {false};

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            event->accept();
            if (cancelled) cancelled();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }
};

/// The sections, down the panel's side: Quick, yours, then Krita's
class Rail : public QWidget
{
public:
    struct Item {
        QString id; ///< quickId, "user:<n>", a section's id, or "add"
        QString title;
        QString icon;
        QRect rect;
    };
    std::function<void(const QString &id)> chosen;
    std::function<void(const QString &id, const QString &cardId)> dropped;
    std::function<void(const QString &id, const QPoint &globalPos)> menuRequested;

    explicit Rail(QWidget *parent)
        : QWidget(parent)
    {
        setFixedWidth(railWidth);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAcceptDrops(true);
        setCursor(Qt::PointingHandCursor);
    }

    void setItems(const QVector<Item> &items)
    {
        m_items = items;
        int y = 4;
        for (int i = 0; i < m_items.size(); i++) {
            // a gap before Krita's own sections
            if (i > 0 && !isOwn(m_items[i].id) && isOwn(m_items[i - 1].id)) y += 12;
            m_items[i].rect = QRect(6, y, width() - 12, 30);
            y += 32;
        }
        setMinimumHeight(y + 4);
        update();
    }

    void setCurrent(const QString &id)
    {
        m_current = id;
        update();
    }

    /// Only the icons, where there is no room for the names
    void setCompact(bool compact)
    {
        if (compact == m_compact) return;
        m_compact = compact;
        setFixedWidth(compact ? compactWidth : railWidth);
        setItems(m_items);
    }
    static constexpr int compactWidth = 44;

    QRect rectOf(const QString &id) const
    {
        Q_FOREACH (const Item &item, m_items) {
            if (item.id == id) return item.rect;
        }
        return QRect();
    }

    static bool isOwn(const QString &id) { return id == quickId || userIndex(id) >= 0 || id == QLatin1String("add"); }

protected:
    int at(const QPoint &pos) const
    {
        for (int i = 0; i < m_items.size(); i++) {
            if (m_items[i].rect.contains(pos)) return i;
        }
        return -1;
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        for (int i = 0; i < m_items.size(); i++) {
            const Item &item = m_items[i];
            const bool current = item.id == m_current;
            const bool target = i == m_dropTarget;
            if (current || i == m_hovered || target) {
                painter.setPen(target ? QPen(palette().color(QPalette::Link), 1.5) : Qt::NoPen);
                painter.setBrush(ink(this, current ? 0.13 : 0.06));
                painter.drawPath(KisModernStyle::squircle(QRectF(item.rect).adjusted(0.5, 0.5, -0.5, -0.5), 8));
            }
            const QRect iconRect(item.rect.left() + 8, item.rect.center().y() - 8, 16, 16);
            painter.setOpacity(current ? 1.0 : 0.8);
            if (item.id == quickId) {
                // a star: what you keep at hand
                QPolygonF star;
                for (int k = 0; k < 10; k++) {
                    const qreal radius = (k % 2 ? 0.42 : 1.0) * 7;
                    const qreal angle = M_PI / 2 + k * M_PI / 5;
                    star << QPointF(iconRect.center()) + QPointF(radius * std::cos(angle), -radius * std::sin(angle));
                }
                painter.setPen(QPen(palette().color(QPalette::WindowText), 1.1));
                painter.setBrush(Qt::NoBrush);
                painter.drawPolygon(star);
            } else if (!item.icon.isEmpty()) {
                KisIconUtils::loadIcon(item.icon).paint(&painter, iconRect);
            }
            painter.setOpacity(1.0);
            if (m_compact) continue;
            painter.setPen(current ? palette().color(QPalette::WindowText) : KisModernStyle::secondaryInk(palette().color(QPalette::WindowText)));
            const QRect text = item.rect.adjusted(32, 0, -6, 0);
            painter.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(item.title, Qt::ElideRight, text.width()));
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int hovered = at(event->pos());
        if (hovered != m_hovered) {
            m_hovered = hovered;
            update();
        }
    }

    void leaveEvent(QEvent *) override
    {
        m_hovered = -1;
        update();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const int i = at(event->pos());
        if (i < 0) return;
        if (event->button() == Qt::LeftButton && chosen) chosen(m_items[i].id);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        const int i = at(event->pos());
        if (i >= 0 && menuRequested) menuRequested(m_items[i].id, event->globalPos());
    }

    bool event(QEvent *event) override
    {
        // with only icons, the name on hover
        if (event->type() == QEvent::ToolTip) {
            const int i = at(static_cast<QHelpEvent *>(event)->pos());
            if (m_compact && i >= 0) QToolTip::showText(static_cast<QHelpEvent *>(event)->globalPos(), m_items[i].title, this);
            else QToolTip::hideText();
            return true;
        }
        return QWidget::event(event);
    }

    /// Up and down go through the sections
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() != Qt::Key_Up && event->key() != Qt::Key_Down) {
            QWidget::keyPressEvent(event);
            return;
        }
        int i = 0;
        for (; i < m_items.size() && m_items[i].id != m_current; i++) {}
        i = qBound(0, i + (event->key() == Qt::Key_Down ? 1 : -1), int(m_items.size()) - 1);
        // past "New section", which would make one
        if (m_items[i].id == QLatin1String("add")) i = qBound(0, i + (event->key() == Qt::Key_Down ? 1 : -1), int(m_items.size()) - 1);
        if (chosen && m_items[i].id != QLatin1String("add")) chosen(m_items[i].id);
    }

    int dropTargetAt(const QPoint &pos) const
    {
        const int i = at(pos);
        return i >= 0 && isOwn(m_items[i].id) ? i : -1;
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasFormat(cardMime)) event->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        const int target = dropTargetAt(event->pos());
        if (target != m_dropTarget) {
            m_dropTarget = target;
            update();
        }
        if (target >= 0) event->acceptProposedAction();
        else event->ignore();
    }

    void dragLeaveEvent(QDragLeaveEvent *) override
    {
        m_dropTarget = -1;
        update();
    }

    void dropEvent(QDropEvent *event) override
    {
        const int target = dropTargetAt(event->pos());
        m_dropTarget = -1;
        update();
        if (target < 0 || !dropped) return;
        event->acceptProposedAction();
        const QString id = m_items[target].id;
        const QString card = QString::fromUtf8(event->mimeData()->data(cardMime));
        QTimer::singleShot(0, this, [this, id, card]() { dropped(id, card); });
    }

private:
    QVector<Item> m_items;
    QString m_current;
    int m_hovered {-1};
    int m_dropTarget {-1};
    bool m_compact {false};
};

/// How well \p query fits \p text: its start beats a word's start beats the middle; 0 is not at all
int matchScore(const QString &query, const QString &text)
{
    const int at = text.indexOf(query, 0, Qt::CaseInsensitive);
    if (at < 0) return 0;
    const bool wordStart = at == 0 || !text.at(at - 1).isLetterOrNumber();
    return 1000 - (wordStart ? 0 : 200) - at;
}

/// How well one word of a search fits a card: its title, its settings' names, its words, its section
int wordScore(const CardDef &card, const QString &word)
{
    int score = matchScore(word, card.title) * 2;
    Q_FOREACH (const QString &id, card.settings) {
        if (const Setting *s = R::instance()->setting(id)) score = qMax(score, matchScore(word, s->label));
    }
    score = qMax(score, matchScore(word, card.aliases) / 2);
    return qMax(score, matchScore(word, sectionTitle(card.section)) / 4);
}

/// What finds a card: each word of \p query somewhere in it ("pen pressure", "undo steps"), or a typo of its title
int cardScore(const CardDef &card, const QString &query)
{
    int score = 0;
    Q_FOREACH (const QString &word, query.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const int one = wordScore(card, word);
        if (!one) {
            score = 0;
            break;
        }
        score += one;
    }
    if (!score) {
        int fuzzy = 0;
        if (kfts::fuzzy_match(query, card.title, fuzzy)) score = qMax(1, fuzzy / 10);
    }
    return score;
}

/// The small window of Quick, by the Settings button
class QuickWindow : public QFrame
{
public:
    std::function<void()> openAll;
    std::function<void(const QString &cardId, const QPoint &globalPos)> cardMenu;

    QuickWindow(KisMainWindow *window)
        : QFrame(window, KisStudioGlass::popupWindowType() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
        , m_window(window)
    {
        setAttribute(Qt::WA_TranslucentBackground, KisStudioGlass::seeThroughWindows());
        const int margin = shadowMargin();
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(margin + 10, margin + 8, margin + 10, margin + 10);
        layout->setSpacing(6);
        auto *header = new QHBoxLayout();
        header->addWidget(titleLabel(i18nc("settings section", "Quick"), this), 1);
        auto *all = new QPushButton(i18nc("@action:button", "All settings"), this);
        all->setFlat(true);
        all->setCursor(Qt::PointingHandCursor);
        connect(all, &QPushButton::clicked, this, [this]() {
            hide();
            if (openAll) openAll();
        });
        header->addWidget(all);
        layout->addLayout(header);
        m_scroll = new QScrollArea(this);
        m_scroll->setFrameShape(QFrame::NoFrame);
        m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll->setWidgetResizable(true);
        m_scroll->viewport()->setAutoFillBackground(false);
        layout->addWidget(m_scroll);
        KisStudioGlass::setSeeThrough(this, true);
        qApp->installEventFilter(this);
    }

    void popupBy(QWidget *anchor)
    {
        rebuild();
        const int margin = shadowMargin();
        QScreen *screen = anchor->screen();
        const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
        const int contents = m_page ? m_page->sizeHint().height() : 60;
        const int height = qMin(contents + 90 + 2 * margin, int(available.height() * 0.75));
        resize(quickWidth + 2 * margin, height);
        QPoint pos = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height())) - QPoint(width() - margin, margin - 6);
        pos.setX(qBound(available.left(), pos.x(), available.right() - width()));
        pos.setY(qBound(available.top(), pos.y(), available.bottom() - height));
        move(pos);
        m_anchor = anchor;
        KisStudioMotion::showFaded(this);
        raise();
        activateWindow();
    }

    void rebuild()
    {
        if (cardsBusy || cardDialogOpen()) {
            QTimer::singleShot(50, this, [this]() { rebuild(); });
            return;
        }
        auto *page = new Page(m_scroll);
        page->setAutoFillBackground(false);
        const UserSections sections = UserSections::load();
        Q_FOREACH (const QString &id, sections.quick) {
            const CardDef *def = cardDef(id);
            if (!def) continue;
            auto *card = new Card(*def, m_window, page);
            const QString cardId = def->id;
            card->menuRequested = [this, cardId](const QPoint &pos) {
                if (cardMenu) cardMenu(cardId, pos);
            };
            page->box()->addWidget(card);
        }
        if (sections.quick.isEmpty()) {
            page->box()->addWidget(dimLabel(i18nc("@info", "Pin settings here from their ⋯ menu in Settings, or drag them onto Quick."), page));
        }
        page->box()->addStretch(1);
        m_scroll->setWidget(page);
        m_page = page;
    }

protected:
    int shadowMargin() const { return testAttribute(Qt::WA_TranslucentBackground) ? 16 : 0; }

    bool isInside(QWidget *widget) const
    {
        for (QWidget *window = widget ? widget->window() : nullptr; window; window = window->parentWidget() ? window->parentWidget()->window() : nullptr) {
            if (window == this) return true;
        }
        return false;
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!isVisible()) return false;
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TabletPress) {
            QWidget *widget = qobject_cast<QWidget *>(watched);
            if (widget && !isInside(widget)) {
                hide();
                // a press on the button that opened it closes it, without opening it again (nor its right click after)
                const bool onAnchor = m_anchor && (widget == m_anchor || m_anchor->isAncestorOf(widget));
                if (onAnchor) m_closedByAnchor.start();
                return onAnchor;
            }
        } else if (event->type() == QEvent::WindowDeactivate && watched == this) {
            // to another application, or a window not of this one (a dialog of its own stays)
            QTimer::singleShot(0, this, [this]() {
                QWidget *active = QApplication::activeWindow();
                if (isVisible() && !isInside(active) && !isInside(QApplication::activeModalWidget()) && !isInside(QApplication::activePopupWidget())) hide();
            });
        }
        return false;
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            hide();
            return;
        }
        QFrame::keyPressEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const int margin = shadowMargin();
        const QRectF panel = QRectF(rect()).adjusted(margin, margin, -margin, -margin);
        if (margin) KisStudioGlass::paintShadow(painter, panel, 12, margin, 34, 4);
        const QPainterPath shape = KisStudioGlass::paint(painter, this, panel.adjusted(0.5, 0.5, -0.5, -0.5), 12);
        KisStudioGlass::paintBorder(painter, this, shape, ink(this, 0.08));
    }

public:
    /// Closed a moment ago by a press on its button: that press's right click is not to open it again
    bool closedByAnchorJustNow() const { return m_closedByAnchor.isValid() && m_closedByAnchor.elapsed() < 800; }

private:
    QElapsedTimer m_closedByAnchor;
    KisMainWindow *m_window {nullptr};
    QScrollArea *m_scroll {nullptr};
    QPointer<Page> m_page;
    QPointer<QWidget> m_anchor;
};

} // namespace

struct KisStudioSettings::Private
{
    KisStudioSettings *q {nullptr};
    KisMainWindow *mainWindow {nullptr};
    QPointer<QWidget> area;
    std::function<QRect(const QSize &)> placement;

    Rail *rail {nullptr};
    QScrollArea *railScroll {nullptr};
    QLineEdit *search {nullptr};
    QToolButton *changedButton {nullptr};
    QStackedWidget *stack {nullptr};
    QScrollArea *scroll {nullptr};
    QPointer<Page> page;
    QWidget *keysHolder {nullptr};
    QPointer<KisKeysPage> keys;
    QWidget *review {nullptr};
    QLabel *footer {nullptr};
    QPushButton *undo {nullptr};
    QLabel *status {nullptr};
    QTimer statusTimer;
    QPointer<QuickWindow> quick;

    QString section {QStringLiteral("appearance")};
    /// What had the focus before it opened: it has it again when it closes (else the canvas)
    QPointer<QWidget> focusBefore;
    bool showChanged {false};
    /// The values when it opened: Undo goes back to them
    QHash<QString, QVariant> opened;
    QTimer refreshFooter;

    QString shownSection() const
    {
        if (!search->text().trimmed().isEmpty()) return searchId;
        if (showChanged) return changedId;
        return section;
    }

    /// Keys, left: applied and let go. A page is good for one apply (it reloads the profiles it points into),
    /// and would write back over shortcuts changed elsewhere meanwhile: a new one is made next time.
    void applyKeys()
    {
        if (!keys) return;
        keys->apply();
        keys->hide();
        keys->deleteLater();
        keys = nullptr;
    }

    /// Shows \p widget in the stack, Keys applied if it was shown
    void showInStack(QWidget *widget)
    {
        if (stack->currentWidget() == keysHolder && widget != keysHolder) applyKeys();
        stack->setCurrentWidget(widget);
    }

    void updateRail()
    {
        QVector<Rail::Item> items;
        items << Rail::Item{quickId, i18nc("settings section", "Quick"), QString(), QRect()};
        const UserSections sections = UserSections::load();
        for (int i = 0; i < sections.sections.size(); i++) {
            items << Rail::Item{QStringLiteral("user:%1").arg(i), sections.sections[i].name, QStringLiteral("studio_layers"), QRect()};
        }
        items << Rail::Item{QStringLiteral("add"), i18nc("@action new settings section", "New section"), QStringLiteral("list-add"), QRect()};
        Q_FOREACH (const SectionDef &s, builtinSections()) items << Rail::Item{s.id, s.title, s.icon, QRect()};
        rail->setItems(items);
        rail->setCurrent(shownSection() == section ? section : QString());
    }

    void updateChangedCount()
    {
        int count = 0;
        Q_FOREACH (const CardDef &card, allCards()) {
            if (cardChanged(card)) count++;
        }
        changedButton->setText(count ? i18nc("@action:button settings changed from default, with a count", "Changed %1", count)
                                     : i18nc("@action:button settings changed from default", "Changed"));
        changedButton->setEnabled(count > 0 || showChanged);
    }

    void updateFooter()
    {
        // the review page has its own buttons where these would be
        if (stack->currentWidget() == review) return;
        QStringList waiting;
        Q_FOREACH (const QString &id, R::instance()->waitingForRestart()) {
            if (const Setting *s = R::instance()->setting(id)) waiting << s->label;
        }
        footer->setText(waiting.isEmpty() ? QString() : i18nc("@info", "After a restart: %1", waiting.join(QStringLiteral(", "))));
        footer->setVisible(!waiting.isEmpty());
        bool differs = false;
        for (auto it = opened.constBegin(); it != opened.constEnd() && !differs; ++it) {
            const Setting *s = R::instance()->setting(it.key());
            differs = s && !R::instance()->sameValue(*s, R::instance()->value(it.key()), it.value());
        }
        undo->setVisible(differs);
        updateChangedCount();
    }

    Card *makeCard(const CardDef &def, Page *on, const QString &inSection)
    {
        auto *card = new Card(def, mainWindow, on);
        card->draggable = true;
        const QString id = def.id;
        card->menuRequested = [this, id, inSection](const QPoint &pos) { cardMenu(id, inSection, pos); };
        return card;
    }

    void cardMenu(const QString &cardId, const QString &inSection, const QPoint &pos)
    {
        const CardDef *def = cardDef(cardId);
        if (!def) return;
        CardMenu menu(*def, q);
        UserSections sections = UserSections::load();
        const bool pinned = sections.quick.contains(cardId);
        QAction *pin = menu.addAction(pinned ? i18nc("@action:inmenu", "Unpin from Quick") : i18nc("@action:inmenu", "Pin to Quick"));
        QMenu *addTo = menu.addMenu(i18nc("@action:inmenu", "Add to"));
        QVector<QPair<QAction *, int>> targets;
        for (int i = 0; i < sections.sections.size(); i++) {
            QAction *target = addTo->addAction(sections.sections[i].name);
            target->setEnabled(!sections.sections[i].cards.contains(cardId));
            targets << qMakePair(target, i);
        }
        if (!sections.sections.isEmpty()) addTo->addSeparator();
        QAction *addNew = addTo->addAction(i18nc("@action:inmenu", "New section"));
        QAction *remove = userIndex(inSection) >= 0 ? menu.addAction(i18nc("@action:inmenu", "Remove from this section")) : nullptr;
        QAction *chosen = menu.run(pos);
        if (!chosen) return;
        if (chosen == pin) {
            if (pinned) sections.quick.removeAll(cardId);
            else sections.quick << cardId;
        } else if (chosen == addNew) {
            const QString id = UserSections::add(def->title, {cardId});
            updateRail();
            renameSection(id);
            return;
        } else if (chosen == remove) {
            if (QStringList *cards = sections.cardsOf(inSection)) cards->removeAll(cardId);
        } else {
            for (const auto &target : targets) {
                if (chosen == target.first) sections.sections[target.second].cards << cardId;
            }
        }
        sections.save();
        updateRail();
        // later: this runs in the card's button's click, the card is not to go yet
        if (shownSection() == quickId || userIndex(shownSection()) >= 0) QTimer::singleShot(0, q, [this]() { rebuild(); });
    }

    void renameSection(const QString &id)
    {
        const QRect r = rail->rectOf(id);
        if (r.isNull()) return;
        auto *edit = new RenameEdit(rail);
        edit->setGeometry(r.width() < 60 ? QRect(r.left(), r.top() + 3, railWidth - 12, r.height() - 6) : r.adjusted(28, 3, -4, -3));
        UserSections sections = UserSections::load();
        const int index = userIndex(id);
        if (index < 0 || index >= sections.sections.size()) return;
        edit->setText(sections.sections[index].name);
        edit->selectAll();
        edit->show();
        edit->setFocus();
        QPointer<RenameEdit> guard = edit;
        edit->cancelled = [this, guard]() {
            if (!guard || guard->finished) return;
            guard->finished = true;
            guard->hide();
            guard->deleteLater();
            updateRail();
        };
        auto done = [this, guard, index]() {
            if (!guard || guard->finished) return;
            guard->finished = true;
            const QString name = guard->text().trimmed().left(40);
            guard->hide();
            guard->deleteLater();
            UserSections sections = UserSections::load();
            if (!name.isEmpty() && index < sections.sections.size()) {
                sections.sections[index].name = name;
                sections.save();
            }
            updateRail();
        };
        QObject::connect(edit, &QLineEdit::editingFinished, edit, done);
    }

    void railMenu(const QString &id, const QPoint &pos)
    {
        const int index = userIndex(id);
        if (index < 0) return;
        QMenu menu(q);
        QAction *rename = menu.addAction(i18nc("@action:inmenu", "Rename"));
        QAction *remove = menu.addAction(i18nc("@action:inmenu", "Delete section"));
        QAction *chosen = menu.exec(pos);
        if (chosen == rename) {
            renameSection(id);
        } else if (chosen == remove) {
            UserSections sections = UserSections::load();
            if (index < sections.sections.size()) {
                sections.sections.remove(index);
                sections.save();
            }
            if (userIndex(section) >= 0) section = QStringLiteral("appearance");
            updateRail();
            rebuild();
        }
    }

    void dropOnRail(const QString &id, const QString &cardId)
    {
        const CardDef *def = cardDef(cardId);
        if (!def) return;
        if (id == QLatin1String("add")) {
            const QString added = UserSections::add(def->title, {cardId});
            updateRail();
            renameSection(added);
            return;
        }
        UserSections sections = UserSections::load();
        QStringList *cards = sections.cardsOf(id);
        if (!cards || cards->contains(cardId)) return;
        *cards << cardId;
        sections.save();
        if (shownSection() == id) rebuild();
    }

    void choose(const QString &id)
    {
        if (id == QLatin1String("add")) {
            section = UserSections::add(i18nc("a new settings section's name", "Mine"), {});
            search->clear();
            showChanged = false;
            changedButton->setChecked(false);
            updateRail();
            rebuild();
            renameSection(section);
            return;
        }
        section = id;
        showChanged = false;
        changedButton->setChecked(false);
        if (!search->text().isEmpty()) {
            const QSignalBlocker blocker(search);
            search->clear();
        }
        rail->setCurrent(id);
        rebuild();
    }

    /// The cards of what is shown now
    void rebuild()
    {
        if (cardsBusy || cardDialogOpen()) {
            // once the card's drag, menu or dialog is over
            QTimer::singleShot(50, q, [this]() { rebuild(); });
            return;
        }
        const QString shown = shownSection();
        if (shown == keysSection) {
            if (!keys) {
                keys = new KisKeysPage(keysHolder);
                keysHolder->layout()->addWidget(keys);
            }
            showInStack(keysHolder);
            rail->setCurrent(keysSection);
            q->reposition();
            return;
        }
        showInStack(scroll);
        auto *fresh = new Page(scroll);
        fresh->setAutoFillBackground(false);
        UserSections sections = UserSections::load();

        if (shown == searchId || shown == changedId) {
            const QString query = search->text().trimmed();
            QVector<QPair<int, const CardDef *>> found;
            Q_FOREACH (const CardDef &def, allCards()) {
                const int score = shown == searchId ? cardScore(def, query) : (cardChanged(def) ? 1 : 0);
                if (score > 0) found << qMakePair(score, &def);
            }
            if (shown == searchId) {
                // Keys is a section, not a card: a way to it when it is what is looked for
                const QString keysWords = i18nc("find result", "Keys and shortcuts") + QStringLiteral(" keyboard hotkeys keybindings mouse held rings");
                bool keysFound = !query.isEmpty();
                Q_FOREACH (const QString &word, query.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                    keysFound = keysFound && matchScore(word, keysWords) > 0;
                }
                if (keysFound) {
                    auto *keysButton = new QPushButton(i18nc("@action:button", "Keys and shortcuts"), fresh);
                    QObject::connect(keysButton, &QPushButton::clicked, q, [this]() { QTimer::singleShot(0, q, [this]() { choose(keysSection); }); });
                    fresh->box()->addWidget(keysButton, 0, Qt::AlignLeft);
                }
                std::stable_sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
                Q_FOREACH (const auto &match, found) fresh->box()->addWidget(makeCard(*match.second, fresh, QString()));
            } else {
                QString heading;
                Q_FOREACH (const auto &match, found) {
                    if (match.second->section != heading) {
                        heading = match.second->section;
                        fresh->heading(sectionTitle(heading));
                    }
                    fresh->box()->addWidget(makeCard(*match.second, fresh, QString()));
                }
            }
            if (found.isEmpty() && fresh->box()->count() == 0) {
                fresh->box()->addWidget(dimLabel(shown == searchId ? i18nc("@info", "No setting is called that.")
                                                                   : i18nc("@info", "Everything is as Krita comes."), fresh));
            }
            rail->setCurrent(QString());
        } else if (QStringList *own = sections.cardsOf(shown)) {
            Q_FOREACH (const QString &id, *own) {
                if (const CardDef *def = cardDef(id)) fresh->box()->addWidget(makeCard(*def, fresh, shown));
            }
            if (own->isEmpty()) {
                fresh->box()->addWidget(dimLabel(shown == quickId ? i18nc("@info", "Drag settings here by their titles, or pin them from their ⋯ menu. A right click on the Settings button shows them.")
                                                                  : i18nc("@info", "Drag settings here by their titles, or add them from their ⋯ menu."),
                                                 fresh));
            }
            const QString target = shown;
            fresh->dropped = [this, target](const QString &cardId, int index) {
                UserSections sections = UserSections::load();
                QStringList *cards = sections.cardsOf(target);
                if (!cards || !cardDef(cardId)) return;
                const int from = cards->indexOf(cardId);
                if (from >= 0) {
                    cards->removeAt(from);
                    if (index > from) index--;
                }
                cards->insert(qBound(0, index, cards->size()), cardId);
                sections.save();
                // later: this runs in the page that is to be replaced
                QTimer::singleShot(0, q, [this]() { rebuild(); });
            };
            fresh->setAcceptDrops(true);
            rail->setCurrent(shown);
        } else {
            Q_FOREACH (const CardDef &def, allCards()) {
                if (def.section == shown) fresh->box()->addWidget(makeCard(def, fresh, shown));
            }
            rail->setCurrent(shown);
        }
        fresh->box()->addStretch(1);
        scroll->setWidget(fresh);
        page = fresh;
        updateFooter();
        q->reposition();
    }

    void showCard(const QString &cardId)
    {
        const CardDef *def = cardDef(cardId);
        if (!def) return;
        choose(def->section);
        if (!page) return;
        Q_FOREACH (Card *card, page->cards()) {
            if (card->def().id != cardId) continue;
            // once laid out
            QPointer<Card> guard = card;
            QTimer::singleShot(0, q, [this, guard]() {
                if (!guard) return;
                scroll->ensureWidgetVisible(guard, 0, 24);
                guard->flash();
            });
        }
    }

    // ---- moving settings

    void showPlan(const KisSettingsTransfer::Plan &plan, const QString &title, const QString &explanation, const QString &applyText,
                  std::function<void(const QVector<KisSettingsTransfer::Change> &, bool sections)> apply)
    {
        if (!plan.error.isEmpty()) {
            q->say(plan.error);
            return;
        }
        if (plan.changes.isEmpty() && plan.sections.isEmpty()) {
            q->say(i18nc("@info", "Nothing would change: they're the same already."));
            return;
        }
        delete review->layout();
        Q_FOREACH (QWidget *child, review->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) child->deleteLater();
        auto *layout = new QVBoxLayout(review);
        layout->setContentsMargins(0, 0, 4, 0);
        layout->addWidget(titleLabel(title, review));
        if (!explanation.isEmpty()) layout->addWidget(dimLabel(explanation, review));
        auto *list = new QScrollArea(review);
        list->setFrameShape(QFrame::NoFrame);
        list->setWidgetResizable(true);
        list->viewport()->setAutoFillBackground(false);
        auto *rows = new QWidget(list);
        rows->setAutoFillBackground(false);
        auto *rowsLayout = new QVBoxLayout(rows);
        rowsLayout->setContentsMargins(0, 4, 0, 4);
        QVector<QPair<QCheckBox *, KisSettingsTransfer::Change>> boxes;
        Q_FOREACH (const KisSettingsTransfer::Change &change, plan.changes) {
            const Setting *s = R::instance()->setting(change.id);
            if (!s) continue;
            auto *box = new QCheckBox(i18nc("@option:check a setting's change: name, from, to", "%1: %2 → %3", s->label,
                                            R::instance()->display(*s, change.from), R::instance()->display(*s, change.to)),
                                      rows);
            box->setChecked(true);
            rowsLayout->addWidget(box);
            boxes << qMakePair(box, change);
        }
        QCheckBox *sectionsBox = nullptr;
        if (!plan.sections.isEmpty()) {
            sectionsBox = new QCheckBox(i18nc("@option:check", "Quick and your sections, from the file"), rows);
            rowsLayout->addWidget(sectionsBox);
        }
        if (!plan.skipped.isEmpty()) {
            rowsLayout->addSpacing(8);
            rowsLayout->addWidget(dimLabel(i18nc("@info", "Left as they are:") + QStringLiteral("\n") + plan.skipped.join(QLatin1Char('\n')), rows));
        }
        rowsLayout->addStretch(1);
        list->setWidget(rows);
        layout->addWidget(list, 1);
        auto *buttons = new QHBoxLayout();
        buttons->addStretch(1);
        auto *cancel = new QPushButton(i18nc("@action:button", "Cancel"), review);
        auto *ok = new QPushButton(applyText, review);
        ok->setDefault(true);
        buttons->addWidget(cancel);
        buttons->addWidget(ok);
        layout->addLayout(buttons);
        QObject::connect(cancel, &QPushButton::clicked, review, [this]() { rebuild(); });
        QObject::connect(ok, &QPushButton::clicked, review, [boxes, sectionsBox, apply]() {
            QVector<KisSettingsTransfer::Change> chosen;
            for (const auto &box : boxes) {
                if (box.first->isChecked()) chosen << box.second;
            }
            apply(chosen, sectionsBox && sectionsBox->isChecked());
        });
        showInStack(review);
        rail->setCurrent(QString());
        // nothing else at its foot, where a click meant for Apply could land
        footer->hide();
        undo->hide();
    }

    void applyHere(const QVector<KisSettingsTransfer::Change> &changes, const QByteArray &sections)
    {
        int applied = 0;
        Q_FOREACH (const KisSettingsTransfer::Change &change, changes) {
            if (R::instance()->setValue(change.id, change.to)) applied++;
        }
        if (!sections.isEmpty()) {
            KisConfig(false).writeEntry(sectionsKey, QString::fromUtf8(sections));
            updateRail();
        }
        R::instance()->flush();
        q->say(i18ncp("@info", "%1 setting changed. Undo puts back what changed since Settings opened.",
                      "%1 settings changed. Undo puts back what changed since Settings opened.", applied));
        rebuild();
        updateFooter();
    }

    void exportFile()
    {
        KoFileDialog dialog(q, KoFileDialog::SaveFile, QStringLiteral("StudioSettingsExport"));
        dialog.setCaption(i18nc("@title:window", "Export Settings"));
        dialog.setNameFilter(i18nc("file type", "Krita settings (*.json)"));
        dialog.setDefaultDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/krita-settings.json"));
        QString path = dialog.filename();
        if (path.isEmpty()) return;
        if (!path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive)) path += QStringLiteral(".json");
        R::instance()->flush();
        QString error;
        if (KisSettingsTransfer::writeFile(path, KisSettingsTransfer::exportSettings(), &error)) {
            q->say(i18nc("@info", "Settings saved to %1", QFileInfo(path).fileName()));
        } else {
            q->say(i18nc("@info", "Couldn't save: %1", error));
        }
    }

    void importFile()
    {
        KoFileDialog dialog(q, KoFileDialog::OpenFile, QStringLiteral("StudioSettingsExport"));
        dialog.setCaption(i18nc("@title:window", "Import Settings"));
        dialog.setNameFilter(i18nc("file type", "Krita settings (*.json)"));
        const QString path = dialog.filename();
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
            q->say(i18nc("@info", "That file couldn't be read."));
            return;
        }
        const KisSettingsTransfer::Plan plan = KisSettingsTransfer::planImport(file.readAll());
        showPlan(plan, i18nc("@title", "Import from %1", QFileInfo(path).fileName()), QString(), i18nc("@action:button", "Import"),
                 [this, plan](const QVector<KisSettingsTransfer::Change> &chosen, bool sections) {
                     applyHere(chosen, sections ? plan.sections : QByteArray());
                 });
    }

    void fromKrita()
    {
        const KisSettingsTransfer::Plan plan = KisSettingsTransfer::planFromKrita();
        showPlan(plan, i18nc("@title", "Krita's settings, here"), i18nc("@info", "What Krita has set that differs here."), i18nc("@action:button", "Take them"),
                 [this](const QVector<KisSettingsTransfer::Change> &chosen, bool) { applyHere(chosen, QByteArray()); });
    }

    void toKrita()
    {
        R::instance()->flush();
        const KisSettingsTransfer::Plan plan = KisSettingsTransfer::planToKrita();
        const QDateTime kept = KisSettingsTransfer::kritaBackupTime();
        const QString explanation = kept.isValid()
            ? i18nc("@info", "Only these change in Krita; its other settings stay. Its settings from before %1 are kept, for Restore Krita's settings.",
                    QLocale().toString(kept, QLocale::ShortFormat))
            : i18nc("@info", "Only these change in Krita; its other settings stay. A copy of its settings is kept first, for Restore Krita's settings.");
        showPlan(plan, i18nc("@title", "These settings, in Krita"), explanation, i18nc("@action:button", "Give them"),
                 [this](const QVector<KisSettingsTransfer::Change> &chosen, bool) {
                     QString error;
                     if (chosen.isEmpty()) {
                         rebuild();
                     } else if (KisSettingsTransfer::applyToKrita(chosen, &error)) {
                         q->say(i18ncp("@info", "Krita has %1 more of your settings.", "Krita has %1 more of your settings.", chosen.size()));
                         rebuild();
                     } else {
                         q->say(error);
                     }
                 });
    }

    void restoreKrita()
    {
        QString error;
        if (KisSettingsTransfer::restoreKrita(&error)) {
            q->say(i18nc("@info", "Krita's settings are back as they were."));
        } else {
            q->say(error);
        }
    }

    void moreMenu(QWidget *anchor)
    {
        QMenu menu(q);
        QAction *exportAction = menu.addAction(i18nc("@action:inmenu", "Export settings"));
        QAction *importAction = menu.addAction(i18nc("@action:inmenu", "Import settings"));
        QAction *from = nullptr;
        QAction *to = nullptr;
        QAction *restore = nullptr;
        if (!KisSettingsTransfer::kritaSettingsPath().isEmpty()) {
            menu.addSeparator();
            from = menu.addAction(i18nc("@action:inmenu", "Take Krita's settings"));
            to = menu.addAction(i18nc("@action:inmenu", "Give Krita these settings"));
            if (KisSettingsTransfer::kritaBackupTime().isValid()) restore = menu.addAction(i18nc("@action:inmenu", "Restore Krita's settings"));
        }
        QAction *chosen = menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
        if (!chosen) return;
        if (chosen == exportAction) exportFile();
        else if (chosen == importAction) importFile();
        else if (chosen == from) fromKrita();
        else if (chosen == to) toKrita();
        else if (chosen == restore) restoreKrita();
    }
};

KisStudioSettings::KisStudioSettings(KisMainWindow *mainWindow, QWidget *canvasArea)
    // a child of the window, not of the canvas area: over the pills and panels there, which raise themselves as they are placed
    : QWidget(mainWindow)
    , d(new Private)
{
    d->q = this;
    d->mainWindow = mainWindow;
    d->area = canvasArea;
    setAttribute(Qt::WA_TranslucentBackground);
    setObjectName(QStringLiteral("studioSettings"));
    // its buttons' right clicks are its own (a card's menu), not their shortcut cards
    setProperty("ownContextMenu", true);
    setFocusPolicy(Qt::StrongFocus);
    hide();

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 8, 8, 8);
    layout->setSpacing(6);

    d->rail = new Rail(this);
    auto *railScroll = new QScrollArea(this);
    railScroll->setFrameShape(QFrame::NoFrame);
    railScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    railScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    railScroll->setWidgetResizable(true);
    railScroll->setFixedWidth(railWidth);
    railScroll->viewport()->setAutoFillBackground(false);
    railScroll->setWidget(d->rail);
    d->railScroll = railScroll;
    layout->addWidget(railScroll);
    d->rail->chosen = [this](const QString &id) { d->choose(id); };
    d->rail->dropped = [this](const QString &id, const QString &card) { d->dropOnRail(id, card); };
    d->rail->menuRequested = [this](const QString &id, const QPoint &pos) { d->railMenu(id, pos); };

    auto *body = new QVBoxLayout();
    body->setSpacing(6);
    auto *header = new QHBoxLayout();
    header->setSpacing(4);
    d->search = new QLineEdit(this);
    d->search->setPlaceholderText(i18nc("@info:placeholder", "Find a setting"));
    d->search->setClearButtonEnabled(true);
    d->search->setMinimumHeight(30);
    header->addWidget(d->search, 1);
    d->changedButton = new QToolButton(this);
    d->changedButton->setCheckable(true);
    d->changedButton->setAutoRaise(true);
    d->changedButton->setToolTip(i18nc("@info:tooltip", "What differs from how Krita comes"));
    header->addWidget(d->changedButton);
    auto *more = new QToolButton(this);
    more->setAutoRaise(true);
    more->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_more")));
    more->setToolTip(i18nc("@info:tooltip", "Export, import, Krita's settings"));
    header->addWidget(more);
    auto *close = new QToolButton(this);
    close->setAutoRaise(true);
    close->setText(QStringLiteral("✕"));
    close->setToolTip(i18nc("@info:tooltip", "Close (Esc)"));
    header->addWidget(close);
    body->addLayout(header);

    d->stack = new QStackedWidget(this);
    d->scroll = new QScrollArea(d->stack);
    d->scroll->setFrameShape(QFrame::NoFrame);
    // in a narrow window, what doesn't fit can still be scrolled to
    d->scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    d->scroll->setWidgetResizable(true);
    d->scroll->viewport()->setAutoFillBackground(false);
    d->stack->addWidget(d->scroll);
    d->keysHolder = new QWidget(d->stack);
    auto *keysLayout = new QVBoxLayout(d->keysHolder);
    keysLayout->setContentsMargins(0, 0, 0, 0);
    d->stack->addWidget(d->keysHolder);
    d->review = new QWidget(d->stack);
    d->stack->addWidget(d->review);
    body->addWidget(d->stack, 1);

    d->status = new QLabel(this);
    d->status->setWordWrap(true);
    d->status->hide();
    body->addWidget(d->status);
    d->statusTimer.setSingleShot(true);
    d->statusTimer.setInterval(6000);
    connect(&d->statusTimer, &QTimer::timeout, d->status, &QWidget::hide);

    auto *footer = new QHBoxLayout();
    d->footer = dimLabel(QString(), this);
    footer->addWidget(d->footer, 1);
    d->undo = new QPushButton(i18nc("@action:button undo the changes made since Settings opened", "Undo"), this);
    d->undo->setToolTip(i18nc("@info:tooltip", "Puts back what changed since Settings opened"));
    footer->addWidget(d->undo, 0, Qt::AlignRight);
    body->addLayout(footer);
    layout->addLayout(body, 1);

    connect(d->search, &QLineEdit::textChanged, this, [this]() { d->rebuild(); });
    connect(d->changedButton, &QToolButton::toggled, this, [this](bool on) {
        d->showChanged = on;
        if (on && !d->search->text().isEmpty()) {
            const QSignalBlocker blocker(d->search);
            d->search->clear();
        }
        d->rebuild();
    });
    connect(more, &QToolButton::clicked, this, [this, more]() { d->moreMenu(more); });
    connect(close, &QToolButton::clicked, this, &QWidget::hide);
    connect(d->undo, &QPushButton::clicked, this, [this]() {
        R::instance()->restore(d->opened);
        R::instance()->flush();
        d->updateFooter();
    });

    // what changed shows below once things settle, not at every step of a drag
    d->refreshFooter.setSingleShot(true);
    d->refreshFooter.setInterval(60);
    connect(&d->refreshFooter, &QTimer::timeout, this, [this]() { d->updateFooter(); });
    connect(R::instance(), &R::valueChanged, this, [this]() {
        if (isVisible()) d->refreshFooter.start();
    });

    canvasArea->installEventFilter(this);
    KisStudioGlass::setSeeThrough(this, true);
    KisStudioGlass::setOverOverlays(this);
}

KisStudioSettings::~KisStudioSettings()
{
    delete d->quick.data();
}

void KisStudioSettings::setPlacement(std::function<QRect(const QSize &)> placement)
{
    d->placement = placement;
    if (isVisible()) reposition();
}

void KisStudioSettings::reposition()
{
    QWidget *area = d->area;
    if (!area || !parentWidget()) return;
    int width = railWidth + bodyWidth + 20;
    if (d->stack->currentWidget() == d->keysHolder && d->keys) {
        width = qMax(width, railWidth + qMax(d->keys->sizeHint().width(), d->keys->minimumSizeHint().width()) + 80);
    }
    const QSize wanted(width, area->height());
    const QRect inArea = d->placement ? d->placement(wanted) : QRect(area->width() - wanted.width() - 6, 6, wanted.width(), area->height() - 12);
    const QRect place(area->mapTo(parentWidget(), inArea.topLeft()), inArea.size());
    // not room for the sections' names: their icons
    const bool compact = place.width() < railWidth + bodyWidth + 20;
    d->rail->setCompact(compact);
    d->railScroll->setFixedWidth(d->rail->width());
    if (geometry() != place) setGeometry(place);
    raise();
}

void KisStudioSettings::open(const QString &cardId)
{
    if (!isVisible()) {
        QWidget *focus = QApplication::focusWidget();
        d->focusBefore = focus && !isAncestorOf(focus) ? focus : nullptr;
        d->opened = R::instance()->snapshot();
        d->updateRail();
        d->rebuild();
        reposition();
        KisStudioMotion::showFaded(this);
        raise();
        d->updateFooter();
    }
    if (cardId == keysSection) {
        d->choose(keysSection);
    } else if (!cardId.isEmpty()) {
        d->showCard(cardId);
    } else {
        d->search->setFocus();
    }
}

void KisStudioSettings::say(const QString &text)
{
    d->status->setText(text);
    d->status->show();
    d->statusTimer.start();
}

void KisStudioSettings::toggle()
{
    if (isVisible()) hide();
    else open();
}

void KisStudioSettings::showQuick(QWidget *anchor)
{
    if (!d->quick) {
        d->quick = new QuickWindow(d->mainWindow);
        d->quick->openAll = [this]() { open(); };
        d->quick->cardMenu = [this](const QString &cardId, const QPoint &pos) {
            const CardDef *def = cardDef(cardId);
            if (!def) return;
            CardMenu menu(*def, d->quick);
            QAction *unpin = menu.addAction(i18nc("@action:inmenu", "Unpin from Quick"));
            if (menu.run(pos) == unpin) {
                UserSections sections = UserSections::load();
                sections.quick.removeAll(cardId);
                sections.save();
                // after the menu is gone: the card it came from is rebuilt
                QTimer::singleShot(0, d->quick, [this]() {
                    if (d->quick) d->quick->rebuild();
                });
            }
        };
    }
    if (d->quick->isVisible()) {
        d->quick->hide();
        return;
    }
    if (d->quick->closedByAnchorJustNow()) return;
    d->quick->popupBy(anchor);
}

QVector<KisStudioSettings::CardInfo> KisStudioSettings::cardInfos()
{
    QVector<CardInfo> infos;
    infos << CardInfo{keysSection, i18nc("find result", "Keys and shortcuts"), sectionTitle(keysSection),
                      QStringLiteral("keyboard hotkeys keybindings canvas input mouse pan zoom rotate held rings"), QString()};
    Q_FOREACH (const CardDef &card, allCards()) {
        QStringList words{card.aliases};
        Q_FOREACH (const QString &id, card.settings) {
            if (const Setting *s = R::instance()->setting(id)) words << s->label;
        }
        infos << CardInfo{card.id, card.title, sectionTitle(card.section), words.join(QLatin1Char(' ')), card.toggleId};
        if (!card.toggleId.isEmpty()) continue;
        // each of its switches, found by its own name and flipped right there
        Q_FOREACH (const QString &id, card.settings) {
            const Setting *s = R::instance()->setting(id);
            if (s && s->kind == R::Kind::Toggle) infos << CardInfo{card.id, s->label, card.title, card.aliases, id};
        }
    }
    return infos;
}

bool KisStudioSettings::flip(const QString &settingId)
{
    const Setting *s = R::instance()->setting(settingId);
    if (!s || s->kind != R::Kind::Toggle) return false;
    const bool ok = R::instance()->setValue(settingId, !R::instance()->value(settingId).toBool());
    R::instance()->flush();
    return ok;
}

bool KisStudioSettings::event(QEvent *event)
{
    if (event->type() == QEvent::ApplicationPaletteChange) {
        // another theme: what was made in the old colors (dimmed text, marks, icons), made again
        QTimer::singleShot(0, this, [this]() {
            QPalette dim = d->footer->palette();
            dim.setColor(QPalette::WindowText, KisModernStyle::secondaryInk(palette().color(QPalette::WindowText)));
            d->footer->setPalette(dim);
            d->updateRail();
            if (isVisible() && d->stack->currentWidget() != d->review) d->rebuild();
            if (d->quick && d->quick->isVisible()) d->quick->rebuild();
            update();
        });
    }
    return QWidget::event(event);
}

bool KisStudioSettings::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == d->area && (event->type() == QEvent::Resize || event->type() == QEvent::Move) && isVisible()) reposition();
    return QWidget::eventFilter(watched, event);
}

void KisStudioSettings::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPainterPath shape = KisStudioGlass::paint(painter, this, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12);
    KisStudioGlass::paintBorder(painter, this, shape, ink(this, 0.12));
}

void KisStudioSettings::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        if (!d->search->text().isEmpty()) d->search->clear();
        else hide();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void KisStudioSettings::hideEvent(QHideEvent *event)
{
    // the focus not to the next widget of the window (a panel's field) but back where it was: the canvas, mostly
    if (isAncestorOf(QApplication::focusWidget())) {
        QWidget *back = d->focusBefore;
        if (!back && d->mainWindow->activeView() && d->mainWindow->activeView()->canvasBase()) {
            back = d->mainWindow->activeView()->canvasBase()->canvasWidget();
        }
        if (back) back->setFocus(Qt::OtherFocusReason);
    }
    if (d->stack->currentWidget() == d->keysHolder) d->applyKeys();
    R::instance()->flush();
    QWidget::hideEvent(event);
}
