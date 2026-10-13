/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioEmptyStates.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QTimer>

#include <klocalizedstring.h>

#include <KisModernStyle.h>

namespace
{

const char *const watchedProperty = "studioEmptyWatched";

/// What \p view says when empty; a null string: it is not one that says anything
QString emptyText(QAbstractItemView *view)
{
    const QVariant own = view->property("studioEmptyText");
    if (own.isValid()) return own.toString();
    if (view->inherits("KisResourceItemListView")) {
        // a search on the way to it that has text: it found nothing
        for (QWidget *w = view->parentWidget(); w; w = w->parentWidget()) {
            Q_FOREACH (QLineEdit *edit, w->findChildren<QLineEdit *>()) {
                if (edit->isVisible() && !edit->text().isEmpty()) return i18nc("@info an empty list", "Nothing matches");
            }
            if (w->isWindow() || w->inherits("QDockWidget")) break;
        }
        return i18nc("@info an empty list", "Nothing here yet");
    }
    if (view->inherits("KisPaletteView")) return i18nc("@info an empty palette", "No colors yet");
    return QString();
}

class EmptyLabel : public QLabel
{
public:
    explicit EmptyLabel(QAbstractItemView *view)
        : QLabel(view->viewport())
        , m_view(view)
    {
        setAlignment(Qt::AlignCenter);
        setWordWrap(true);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        QPalette quiet = palette();
        quiet.setColor(QPalette::WindowText, KisModernStyle::secondaryInk(quiet.color(QPalette::WindowText)));
        setPalette(quiet);
        hide();
    }

    /// Later: a reset is followed by the rows it brings, and a search's text by its filter
    void refreshLater()
    {
        QTimer::singleShot(0, this, [this]() { refresh(); });
    }

    void refresh()
    {
        if (!m_view) return;
        QAbstractItemModel *model = m_view->model();
        // the view may have been given another model
        if (model != m_model) {
            if (m_model) disconnect(m_model, nullptr, this, nullptr);
            m_model = model;
            if (model) {
                connect(model, &QAbstractItemModel::modelReset, this, [this]() { refreshLater(); });
                connect(model, &QAbstractItemModel::layoutChanged, this, [this]() { refreshLater(); });
                connect(model, &QAbstractItemModel::rowsInserted, this, [this]() { refreshLater(); });
                connect(model, &QAbstractItemModel::rowsRemoved, this, [this]() { refreshLater(); });
            }
        }
        const bool empty = model && model->rowCount(m_view->rootIndex()) == 0;
        const QString text = empty ? emptyText(m_view) : QString();
        setText(text);
        setGeometry(m_view->viewport()->rect().adjusted(12, 12, -12, -12));
        setVisible(!text.isEmpty());
    }

private:
    QPointer<QAbstractItemView> m_view;
    QPointer<QAbstractItemModel> m_model;
};

EmptyLabel *labelOf(QAbstractItemView *view)
{
    Q_FOREACH (QObject *child, view->viewport()->children()) {
        if (auto *label = dynamic_cast<EmptyLabel *>(child)) return label;
    }
    return nullptr;
}

class Watcher : public QObject
{
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const QEvent::Type type = event->type();
        if (type != QEvent::Show && type != QEvent::Resize) return false;
        auto *view = qobject_cast<QAbstractItemView *>(watched);
        if (!view) {
            // a viewport resized: its label with it
            auto *widget = qobject_cast<QWidget *>(watched);
            view = widget ? qobject_cast<QAbstractItemView *>(widget->parentWidget()) : nullptr;
            if (!view || view->viewport() != widget) return false;
        }
        EmptyLabel *label = labelOf(view);
        if (!label) {
            if (view->property(watchedProperty).toBool() || emptyText(view).isNull()) return false;
            view->setProperty(watchedProperty, true);
            label = new EmptyLabel(view);
        }
        label->refreshLater();
        return false;
    }
};

} // namespace

void KisStudioEmptyStates::install()
{
    static Watcher *watcher = nullptr;
    if (watcher) return;
    watcher = new Watcher();
    watcher->setParent(qApp);
    qApp->installEventFilter(watcher);
}
