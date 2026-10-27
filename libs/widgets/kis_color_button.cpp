/*  This file is part of the KDE libraries
    SPDX-FileCopyrightText: 1997 Martin Jones (mjones@kde.org)
    SPDX-FileCopyrightText: 1999 Cristian Tibirna (ctibirna@kde.org)

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "kis_color_button.h"

#include <QPointer>
#include <QPainter>
#include <qdrawutil.h>
#include <QApplication>
#include <QColorDialog>
#include <QClipboard>
#include <QMimeData>
#include <QDrag>
#include <QStyle>
#include <QMouseEvent>
#include <QStyleOptionButton>

#include <KoColor.h>
#include <KisModernStyle.h>
#include <KisDlgInternalColorSelector.h>
#include <kconfiggroup.h>
#include <ksharedconfig.h>

class KisColorButton::KisColorButtonPrivate
{
public:
    KisColorButtonPrivate(KisColorButton *q);
    ~KisColorButtonPrivate() {
        if(m_dialog) {
            m_dialog->close();
        }
    }

    void _k_chooseColor();

    KisColorButton *q;
    KoColor m_defaultColor;
    bool m_bdefaultColor : 1;
    bool m_alphaChannel : 1;
    bool m_palette : 1;

    KoColor col;
    QPoint mPos;
    QPointer<QDialog> m_dialog = nullptr;

    void initStyleOption(QStyleOptionButton *opt) const;
};

/////////////////////////////////////////////////////////////////////
// Functions duplicated from KColorMimeData
// Should be kept in sync
void _k_populateMimeData(QMimeData *mimeData, const KoColor &color)
{
    mimeData->setColorData(color.toQColor());
    mimeData->setText(color.toQColor().name());
}

bool _k_canDecode(const QMimeData *mimeData)
{
    if (mimeData->hasColor()) {
        return true;
    }
    if (mimeData->hasText()) {
        const QString colorName = mimeData->text();
        if ((colorName.length() >= 4) && (colorName[0] == QLatin1Char('#'))) {
            return true;
        }
    }
    return false;
}

QColor _k_fromMimeData(const QMimeData *mimeData)
{
    if (mimeData->hasColor()) {
        return mimeData->colorData().value<QColor>();
    }
    if (_k_canDecode(mimeData)) {
        return QColor(mimeData->text());
    }
    return QColor();
}

QDrag *_k_createDrag(const KoColor &color, QObject *dragsource)
{
    QDrag *drag = new QDrag(dragsource);
    QMimeData *mime = new QMimeData;
    _k_populateMimeData(mime, color);
    drag->setMimeData(mime);
    QPixmap colorpix(25, 20);
    colorpix.fill(color.toQColor());
    QPainter p(&colorpix);
    p.setPen(Qt::black);
    p.drawRect(0, 0, 24, 19);
    p.end();
    drag->setPixmap(colorpix);
    drag->setHotSpot(QPoint(-5, -7));
    return drag;
}
/////////////////////////////////////////////////////////////////////

KisColorButton::KisColorButtonPrivate::KisColorButtonPrivate(KisColorButton *q)
    : q(q)
{
    m_bdefaultColor = false;
    m_alphaChannel = false;
    m_palette = true;
    q->setAcceptDrops(true);

    connect(q, SIGNAL(clicked()), q, SLOT(_k_chooseColor()));
}

KisColorButton::KisColorButton(QWidget *parent)
    : QPushButton(parent)
    , d(new KisColorButtonPrivate(this))
{
}

KisColorButton::KisColorButton(const KoColor &c, QWidget *parent)
    : QPushButton(parent)
    , d(new KisColorButtonPrivate(this))
{
    d->col = c;
}

KisColorButton::KisColorButton(const KoColor &c, const KoColor &defaultColor, QWidget *parent)
    : QPushButton(parent)
    , d(new KisColorButtonPrivate(this))
{
    d->col = c;
    setDefaultColor(defaultColor);
}

KisColorButton::~KisColorButton()
{
    delete d;
}

KoColor KisColorButton::color() const
{
    return d->col;
}

void KisColorButton::setColor(const KoColor &c)
{
    d->col = c;
    update();
    Q_EMIT changed(d->col);
}

void KisColorButton::setAlphaChannelEnabled(bool alpha)
{
    d->m_alphaChannel = alpha;
}

bool KisColorButton::isAlphaChannelEnabled() const
{
    return d->m_alphaChannel;
}

void KisColorButton::setPaletteViewEnabled(bool enable)
{
    d->m_palette = enable;
}

bool KisColorButton::paletteViewEnabled() const
{
    return d->m_palette;
}

KoColor KisColorButton::defaultColor() const
{
    return d->m_defaultColor;
}

void KisColorButton::setDefaultColor(const KoColor &c)
{
    d->m_bdefaultColor = true;
    d->m_defaultColor = c;
}

void KisColorButton::KisColorButtonPrivate::initStyleOption(QStyleOptionButton *opt) const
{
    opt->initFrom(q);
    opt->state |= q->isDown() ? QStyle::State_Sunken : QStyle::State_Raised;
    opt->features = QStyleOptionButton::None;
    if (q->isDefault()) {
        opt->features |= QStyleOptionButton::DefaultButton;
    }
    opt->text.clear();
    opt->icon = QIcon();
}

void KisColorButton::paintEvent(QPaintEvent *)
{
    // Skissa: the color as a squircle swatch, as the rest of the interface draws them
    QPainter painter(this);
    const QColor fill = isEnabled() ? d->col.toQColor() : QColor();
    KisModernStyle::paintSwatch(painter, QRectF(rect()).adjusted(2, 2, -2, -2), fill, palette(), underMouse(), isDown() || isChecked(),
                                hasFocus() && focusPolicy() != Qt::NoFocus);
}

QSize KisColorButton::sizeHint() const
{
    QStyleOptionButton opt;
    d->initStyleOption(&opt);
    return style()->sizeFromContents(QStyle::CT_PushButton, &opt, QSize(40, 15), this);
}

QSize KisColorButton::minimumSizeHint() const
{
    QStyleOptionButton opt;
    d->initStyleOption(&opt);
    return style()->sizeFromContents(QStyle::CT_PushButton, &opt, QSize(3, 3), this);
}

void KisColorButton::dragEnterEvent(QDragEnterEvent *event)
{
    event->setAccepted(_k_canDecode(event->mimeData()) && isEnabled());
}

void KisColorButton::dropEvent(QDropEvent *event)
{
    QColor c = _k_fromMimeData(event->mimeData());
    if (c.isValid()) {
        KoColor col;
        col.fromQColor(c);
        setColor(col);
    }
}

void KisColorButton::keyPressEvent(QKeyEvent *e)
{
    int key = e->key() | e->modifiers();

    if (QKeySequence::keyBindings(QKeySequence::Copy).contains(key)) {
        QMimeData *mime = new QMimeData;
        _k_populateMimeData(mime, color());
        QApplication::clipboard()->setMimeData(mime, QClipboard::Clipboard);
    } else if (QKeySequence::keyBindings(QKeySequence::Paste).contains(key)) {
        QColor color = _k_fromMimeData(QApplication::clipboard()->mimeData(QClipboard::Clipboard));
        KoColor col;
        col.fromQColor(color);
        setColor(col);
    } else {
        QPushButton::keyPressEvent(e);
    }
}

void KisColorButton::mousePressEvent(QMouseEvent *e)
{
    d->mPos = e->pos();
    QPushButton::mousePressEvent(e);
}

void KisColorButton::mouseMoveEvent(QMouseEvent *e)
{
    if ((e->buttons() & Qt::LeftButton) &&
            (e->pos() - d->mPos).manhattanLength() > QApplication::startDragDistance()) {
        _k_createDrag(color(), this)->exec();
        setDown(false);
    }
}

void KisColorButton::KisColorButtonPrivate::_k_chooseColor()
{
    KConfigGroup cfg =  KSharedConfig::openConfig()->group("colorselector");
    bool usePlatformDialog = cfg.readEntry("UsePlatformColorDialog", false);

    if (!usePlatformDialog) {
        KisDlgInternalColorSelector::Config cfg;
        cfg.paletteBox = q->paletteViewEnabled();
        KisDlgInternalColorSelector *dialog = new KisDlgInternalColorSelector(q, q->color(), cfg, i18n("Choose a color"));
        dialog->setPreviousColor(q->color());
        auto setColorFn = [this, dialog]() { q->setColor(dialog->getCurrentColor()); };
        connect(dialog, &KisDlgInternalColorSelector::signalForegroundColorChosen, setColorFn);
        connect(dialog, &QDialog::accepted, setColorFn);
        m_dialog = dialog;
    } else{
        QColorDialog *dialog = new QColorDialog(q);
        dialog->setOption(QColorDialog::ShowAlphaChannel, m_alphaChannel);
        dialog->setCurrentColor(q->color().toQColor());
        auto setColorFn = [this, dialog]()
                          {
                              KoColor c;
                              c.fromQColor(dialog->currentColor());
                              q->setColor(c);
                          };
        connect(dialog, &QColorDialog::currentColorChanged, setColorFn);
        connect(dialog, &QDialog::accepted, setColorFn);
        m_dialog = dialog;
    }
    // common functionality
    KoColor colorBeforeColorDialogChanges = col;
    connect(m_dialog, &QDialog::rejected, [colorBeforeColorDialogChanges, this](){ q->setColor(colorBeforeColorDialogChanges); });
    m_dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_dialog->show();
    m_dialog->raise();
    m_dialog->activateWindow();
}

#include "moc_kis_color_button.cpp"
