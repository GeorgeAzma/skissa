/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioMaskEditor.h"

#include <QCursor>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisGlobalResourcesInterface.h>
#include <KisModernStyle.h>
#include <commands/kis_change_filter_command.h>
#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <kis_config_widget.h>
#include <kis_filter_mask.h>
#include <kis_icon_utils.h>
#include <kis_layer.h>
#include <kis_undo_adapter.h>

#include "KisDocument.h"
#include "KisEffectPanels.h"
#include "KisMainWindow.h"
#include "KisStudioTools.h"
#include "KisViewManager.h"
#include "widgets/KisStudioPopup.h"

namespace
{

class MaskPanel : public KisStudioPopup
{
public:
    MaskPanel(KisViewManager *view, KisFilterMaskSP mask, KisConfigWidget *settings, QWidget *parent)
        : KisStudioPopup(parent)
        , m_view(view)
        , m_mask(mask)
        , m_before(mask->filter()->cloneWithResourcesSnapshot())
        , m_settings(settings)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        // it stays while the picture is panned, zoomed or painted on
        setPinned(true);

        auto *layout = new QVBoxLayout(this);
        layout->setSpacing(KisModernStyle::space2);
        auto *title = new QHBoxLayout();
        title->setSpacing(KisModernStyle::space1);
        auto *name = new QLabel(mask->name(), this);
        QFont bold = name->font();
        bold.setWeight(QFont::DemiBold);
        name->setFont(bold);
        title->addWidget(name, 1);
        auto button = [this, title](const QString &icon, const QString &tip, std::function<void()> act) {
            auto *b = new QToolButton(this);
            b->setAutoRaise(true);
            b->setFocusPolicy(Qt::NoFocus);
            b->setIcon(KisIconUtils::loadIcon(icon));
            b->setToolTip(tip);
            connect(b, &QToolButton::clicked, this, act);
            title->addWidget(b);
        };
        button(QStringLiteral("studio_cancel"), i18nc("@info:tooltip", "Cancel"), [this]() { finish(false); });
        button(QStringLiteral("studio_done"), i18nc("@info:tooltip", "Done"), [this]() { finish(true); });
        layout->addLayout(title);

        auto *scroll = new QScrollArea(this);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        settings->setParent(scroll);
        scroll->setWidget(settings);
        // on glass: setWidget turns the background on
        settings->setAutoFillBackground(false);
        scroll->viewport()->setAutoFillBackground(false);
        const QSize wanted = settings->sizeHint();
        scroll->setMinimumWidth(qMax(320, wanted.width()));
        scroll->setMinimumHeight(qMin(wanted.height(), 560));
        layout->addWidget(scroll, 1);
        fitLayout(layout, KisModernStyle::space3);

        // a change shows on the mask at once, a frame at most
        m_apply.setSingleShot(true);
        m_apply.setInterval(16);
        connect(&m_apply, &QTimer::timeout, this, [this]() { apply(); });
        connect(settings, &KisConfigWidget::sigConfigurationItemChanged, this, [this]() {
            if (!m_apply.isActive()) m_apply.start();
        });
    }

    ~MaskPanel() override
    {
        if (!m_finished) finish(false);
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            finish(false);
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            finish(true);
            return;
        }
        KisStudioPopup::keyPressEvent(event);
    }

private:
    KisFilterConfigurationSP now() const
    {
        return KisFilterConfigurationSP(dynamic_cast<KisFilterConfiguration *>(m_settings->configuration().data()));
    }

    void apply()
    {
        const KisFilterConfigurationSP config = now();
        if (!config || !m_mask) return;
        m_mask->setFilter(config->cloneWithResourcesSnapshot());
        m_mask->setDirty();
    }

    void finish(bool keep)
    {
        if (m_finished) return;
        m_finished = true;
        m_apply.stop();
        if (m_mask) {
            const KisFilterConfigurationSP after = keep ? now() : KisFilterConfigurationSP();
            if (after && after->toXML() != m_before->toXML()) {
                // the mask shows the new settings; the step sets them, and takes back to the old
                m_mask->setFilter(m_before->cloneWithResourcesSnapshot());
                if (m_view && m_view->undoAdapter()) {
                    m_view->undoAdapter()->addCommand(new KisChangeFilterCmd(m_mask, m_before->cloneWithResourcesSnapshot(), after->cloneWithResourcesSnapshot()));
                    if (m_view->document()) m_view->document()->setModified(true);
                } else {
                    m_mask->setFilter(after->cloneWithResourcesSnapshot());
                }
            } else {
                m_mask->setFilter(m_before->cloneWithResourcesSnapshot());
            }
            m_mask->setDirty();
        }
        KisStudioTools::focusCanvas();
        close();
    }

    QPointer<KisViewManager> m_view;
    KisFilterMaskSP m_mask;
    KisFilterConfigurationSP m_before;
    KisConfigWidget *m_settings;
    QTimer m_apply;
    bool m_finished {false};
};

} // namespace

bool KisStudioMaskEditor::edit(KisViewManager *view, KisFilterMaskSP mask)
{
    if (!view || !mask || !mask->filter()) return false;
    KisLayerSP layer = qobject_cast<KisLayer *>(mask->parent().data());
    KisPaintDeviceSP device = layer ? layer->original() : KisPaintDeviceSP();
    KisFilterSP filter = KisFilterRegistry::instance()->value(mask->filter()->name());
    if (!device || !filter) return false;
    QWidget *window = view->mainWindowAsQWidget();
    KisConfigWidget *settings = KisEffectPanels::create(filter->id(), window, device);
    if (!settings) settings = filter->createConfigurationWidget(window, device, true);
    if (!settings) return false;
    settings->setView(view);
    settings->blockSignals(true);
    settings->setConfiguration(mask->filter());
    settings->blockSignals(false);
    settings->setUpdateDelay(10);

    auto *panel = new MaskPanel(view, mask, settings, window);
    // beside the pointer, toward the window's middle, not over the picture's middle
    panel->adjustSize();
    const QPoint pointer = QCursor::pos();
    const QRect frame = window->frameGeometry();
    panel->popupAt(QPoint(pointer.x() > frame.center().x() ? pointer.x() - panel->width() - 24 : pointer.x() + 24,
                          pointer.y() - panel->height() / 3));
    panel->keepOnScreen();
    return true;
}
