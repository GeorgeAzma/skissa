/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisDevTour.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGroupBox>
#include <QHelpEvent>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeView>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>

#include <climits>
#include <functional>

#include <kactioncollection.h>
#include <KoToolFactoryBase.h>
#include <KoToolManager.h>
#include <KoToolRegistry.h>

#include "KisDocument.h"
#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisStudioLayout.h"
#include "KisView.h"
#include "kis_canvas2.h"

namespace
{
void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/// Rich text as plain text, on one line
QString plain(const QString &text)
{
    QString s = Qt::mightBeRichText(text) ? QTextDocumentFragment::fromHtml(text).toPlainText() : text;
    return s.simplified();
}

/// An action's or a button's text without its mnemonic
QString label(QString text)
{
    text.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
    text.remove(QLatin1Char('&'));
    text.replace(QStringLiteral("\x01"), QStringLiteral("&"));
    return plain(text);
}

QString fileName(QString name)
{
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]")), QStringLiteral(" "));
    name = name.simplified();
    if (name.size() > 110) name = name.left(110).trimmed();
    while (name.endsWith(QLatin1Char('.'))) name.chop(1);
    return name.isEmpty() ? QStringLiteral("untitled") : name;
}

QRect globalRect(const QWidget *w)
{
    return QRect(w->mapToGlobal(QPoint(0, 0)), w->size());
}

int layerOf(QWidget *w)
{
    if (qobject_cast<KisMainWindow *>(w)) return 0;
    switch (w->windowType()) {
    case Qt::Tool: return 1;
    case Qt::Popup: return 3;
    case Qt::ToolTip: return 4;
    default: return 2;
    }
}

QList<QWidget *> visibleTops()
{
    QList<QWidget *> tops;
    Q_FOREACH (QWidget *w, QApplication::topLevelWidgets()) {
        if (w->isVisible() && !w->isMinimized()) tops << w;
    }
    std::stable_sort(tops.begin(), tops.end(), [](QWidget *a, QWidget *b) { return layerOf(a) < layerOf(b); });
    return tops;
}

QList<QWidget *> visibleTips()
{
    QList<QWidget *> tips;
    Q_FOREACH (QWidget *w, QApplication::topLevelWidgets()) {
        if (w->isVisible() && w->windowType() == Qt::ToolTip) tips << w;
    }
    return tips;
}

void hideTips()
{
    QToolTip::hideText();
    Q_FOREACH (QWidget *tip, visibleTips()) tip->hide();
}

bool isViewport(QWidget *w)
{
    auto *area = qobject_cast<QAbstractScrollArea *>(w->parentWidget());
    return area && area->viewport() == w;
}

/// A widget that is part of another one (the edit of a spin box, the items of a list)
bool isInner(QWidget *w, QWidget *top)
{
    for (QWidget *p = w->parentWidget(); p && p != top; p = p->parentWidget()) {
        if (qobject_cast<QAbstractSpinBox *>(p) || qobject_cast<QComboBox *>(p) || qobject_cast<QAbstractItemView *>(p)) return true;
    }
    return false;
}

QString describe(QWidget *w)
{
    if (auto *b = qobject_cast<QAbstractButton *>(w)) {
        if (!b->text().isEmpty()) return label(b->text());
    }
    if (!w->accessibleName().isEmpty()) return w->accessibleName();
    if (auto *l = qobject_cast<QLabel *>(w)) {
        if (!l->text().isEmpty()) return plain(l->text()).left(40);
    }
    if (!w->toolTip().isEmpty()) return plain(w->toolTip()).left(40);
    if (!w->objectName().isEmpty() && !w->objectName().startsWith(QLatin1String("qt_"))) return w->objectName();
    return QString::fromLatin1(w->metaObject()->className());
}

QString shortcutOf(QAction *a)
{
    QAction *target = a;
    if (auto *named = qobject_cast<QAction *>(a->property("kisShortcutTarget").value<QObject *>())) target = named;
    return target->shortcut().toString(QKeySequence::NativeText);
}

QString entry(QAction *a)
{
    if (a->isSeparator()) return a->text().isEmpty() ? QStringLiteral("────") : QStringLiteral("── %1 ──").arg(label(a->text()));
    QString t = label(a->text());
    if (t.isEmpty()) t = QStringLiteral("(%1)").arg(a->objectName());
    if (a->isCheckable()) t = (a->isChecked() ? QStringLiteral("☑ ") : QStringLiteral("☐ ")) + t;
    if (a->menu()) t += QStringLiteral("  ▸");
    const QString shortcut = shortcutOf(a);
    if (!shortcut.isEmpty()) t += QStringLiteral("    [%1]").arg(shortcut);
    if (!a->isEnabled()) t += QStringLiteral("    (disabled)");
    return t;
}

QString signature(QMenu *menu)
{
    QStringList texts;
    Q_FOREACH (QAction *a, menu->actions()) texts << label(a->text());
    return texts.join(QLatin1Char('|'));
}

#ifdef Q_OS_WIN
BOOL CALLBACK collectDialog(HWND h, LPARAM list)
{
    wchar_t cls[64];
    if (IsWindowVisible(h) && GetClassNameW(h, cls, 64) && !wcscmp(cls, L"#32770")) {
        reinterpret_cast<QSet<quintptr> *>(list)->insert(quintptr(h));
    }
    return TRUE;
}

QSet<quintptr> nativeDialogs()
{
    QSet<quintptr> list;
    EnumThreadWindows(GetCurrentThreadId(), collectDialog, LPARAM(&list));
    return list;
}

/// A native window of Krita's (the file dialog) rendered on its own
QImage printWindow(quintptr handle, QString *title)
{
    HWND h = HWND(handle);
    wchar_t text[256] = {0};
    GetWindowTextW(h, text, 255);
    *title = QString::fromWCharArray(text);
    RECT r;
    GetWindowRect(h, &r);
    const int width = r.right - r.left, height = r.bottom - r.top;
    if (width <= 0 || height <= 0) return QImage();
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bitmap);
    PrintWindow(h, dc, 2 /* PW_RENDERFULLCONTENT */);
    QImage image = QImage(static_cast<uchar *>(bits), width, height, QImage::Format_RGB32).copy();
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return image;
}
#endif

class Tour : public QObject
{
public:
    explicit Tour(const QString &root)
        : QObject(qApp)
        , m_root(QDir(root).absolutePath())
    {
        QDir().mkpath(m_root);
        m_log.setFileName(m_root + QStringLiteral("/tour.log"));
        m_log.open(QIODevice::Append | QIODevice::Text);
        m_manifest.setFileName(m_root + QStringLiteral("/manifest.jsonl"));
        m_manifest.open(QIODevice::Append | QIODevice::Text);
        const QString phases = qEnvironmentVariable("KRITA_DEV_TOUR_PHASES");
        const QString all = QStringLiteral("splash,inventory,window,menus,chrome,panels,tools,canvas,dialogs,layers,pages,brusheditor,effects,welcome,tooltips");
        Q_FOREACH (const QString &p, (phases.isEmpty() ? all : phases).split(QLatin1Char(','))) m_phases.insert(p.trimmed());
        readProgress();

        // effects would show tooltips and menus as fading copies of themselves
        QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);
        QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
        QApplication::setEffectEnabled(Qt::UI_FadeMenu, false);
        QApplication::setEffectEnabled(Qt::UI_AnimateMenu, false);
        QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);

        auto *poll = new QTimer(this);
        poll->setInterval(100);
        connect(poll, &QTimer::timeout, this, [this, poll]() { tick(poll); });
        poll->start();

        // internal errors (a safe assert) show a modal box: answered "Ignore"
        auto *errors = new QTimer(this);
        errors->setInterval(500);
        connect(errors, &QTimer::timeout, this, [this]() { ignoreErrors(); });
        errors->start();
        note(QStringLiteral("tour started, phases: %1").arg(QStringList(m_phases.values()).join(QLatin1Char(','))));
    }

private:
    bool phase(const char *name) const { return m_phases.contains(QLatin1String(name)); }

    void note(const QString &line)
    {
        m_log.write((QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz ")) + line + QLatin1Char('\n')).toUtf8());
        m_log.flush();
    }

    /// Appends to a map file, which earlier runs (resumed after a crash) may have written to
    void appendText(const QString &name, const QString &header, const QString &text)
    {
        if (text.isEmpty()) return;
        QFile f(m_root + QLatin1Char('/') + name);
        const bool fresh = !f.exists();
        if (f.open(QIODevice::Append | QIODevice::Text)) f.write(((fresh ? header : QString()) + text).toUtf8());
    }

    void writeText(const QString &name, const QString &text)
    {
        QFile f(m_root + QLatin1Char('/') + name);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) f.write(text.toUtf8());
    }

    QString save(const QString &section, const QString &name, const QImage &image, const QString &text = QString())
    {
        if (image.isNull() || image.width() < 2 || image.height() < 2) {
            note(QStringLiteral("  nothing to save: %1 (%2x%3)").arg(name).arg(image.width()).arg(image.height()));
            return QString();
        }
        if (!m_count.contains(section)) {
            int last = 0;
            Q_FOREACH (const QString &f, QDir(m_root + QLatin1Char('/') + section).entryList({QStringLiteral("*.png")}, QDir::Files)) last = qMax(last, f.left(3).toInt());
            m_count[section] = last;
        }
        const int n = ++m_count[section];
        const QString rel = section + QLatin1Char('/') + QStringLiteral("%1 %2.png").arg(n, 3, 10, QLatin1Char('0')).arg(fileName(name));
        QDir(m_root).mkpath(section);
        image.save(m_root + QLatin1Char('/') + rel, "PNG", 80);
        QJsonObject o;
        o[QStringLiteral("section")] = section;
        o[QStringLiteral("file")] = rel;
        o[QStringLiteral("title")] = name;
        o[QStringLiteral("text")] = text;
        m_manifest.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
        m_manifest.flush();
        return rel;
    }

    // ---- pictures ----

    QRect around(const QRect &r, int margin = 24) const
    {
        QRect bounds;
        Q_FOREACH (QScreen *s, QGuiApplication::screens()) bounds |= s->geometry();
        return r.adjusted(-margin, -margin, margin, margin) & bounds;
    }

    /// Krita's own windows rendered into a rectangle of the screen
    QImage composite(const QRect &region, bool tips = false)
    {
        const qreal dpr = m_win ? m_win->devicePixelRatioF() : 1.0;
        QImage image(region.size() * dpr, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(m_win ? m_win->palette().color(QPalette::Window) : Qt::black);
        QPainter painter(&image);
        Q_FOREACH (QWidget *w, visibleTops()) {
            if (!tips && w->windowType() == Qt::ToolTip) continue;
            if (!w->geometry().intersects(region)) continue;
            const QPixmap pixmap = (w == m_win && !m_mainShot.isNull()) ? m_mainShot : w->grab();
            painter.drawPixmap(w->geometry().topLeft() - region.topLeft(), pixmap);
        }
        return image;
    }

    QImage windowShot() { return m_win ? composite(m_win->geometry()) : QImage(); }

    // ---- what opens ----

    /// Answers an internal error's box with "Ignore" (its "Abort" ends Krita); true if \p w was one
    bool ignoreError(QWidget *w)
    {
        auto *box = qobject_cast<QMessageBox *>(w);
        if (!box || !box->isVisible() || !box->text().contains(QLatin1String("ASSERT"))) return false;
        QAbstractButton *ignore = box->button(QMessageBox::Ignore);
        if (!ignore) return false;
        note(QStringLiteral("  internal error, ignored: %1").arg(plain(box->text()).left(200)));
        m_errors += QStringLiteral("- during %1: %2\n").arg(m_name, plain(box->text()));
        ignore->click();
        return true;
    }

    void ignoreErrors()
    {
        Q_FOREACH (QWidget *w, QApplication::topLevelWidgets()) ignoreError(w);
    }

    void closeWindow(QWidget *w)
    {
        if (ignoreError(w)) return;
        QPointer<QWidget> guard(w);
        if (auto *menu = qobject_cast<QMenu *>(w)) {
            menu->hide();
        } else if (auto *dialog = qobject_cast<QDialog *>(w)) {
            dialog->reject();
        } else {
            w->close();
        }
        wait(60);
        if (guard && guard->isVisible()) guard->hide();
    }

    /// Runs \p trigger, hands each window it opens (also from a nested event loop) to \p handle and
    /// closes it; returns how many opened
    int capture(const std::function<void()> &trigger, const std::function<void(QWidget *)> &handle, int patience = 2500)
    {
        QSet<QWidget *> before;
        Q_FOREACH (QWidget *w, visibleTops()) before.insert(w);
#ifdef Q_OS_WIN
        QSet<quintptr> nativeBefore = nativeDialogs();
#endif
        int handled = 0;
        bool finished = false;
        bool busy = false;
        QElapsedTimer quiet;
        quiet.start();
        auto step = [&]() {
            if (busy || finished) return;
            busy = true;
            QWidget *opened = nullptr;
            Q_FOREACH (QWidget *w, visibleTops()) {
                if (!before.contains(w) && w->windowType() != Qt::ToolTip) opened = w;
            }
            if (opened) {
                before.insert(opened);
                QPointer<QWidget> guard(opened);
                wait(250);
                if (guard && guard->isVisible()) handle(guard);
                if (guard && guard->isVisible()) closeWindow(guard);
                handled++;
                quiet.restart();
            }
#ifdef Q_OS_WIN
            else if (nativeDialogs().size() > 0 && !(nativeDialogs() - nativeBefore).isEmpty()) {
                const quintptr h = *(nativeDialogs() - nativeBefore).begin();
                nativeBefore.insert(h);
                wait(600);
                QString title;
                const QImage image = printWindow(h, &title);
                save(m_section, m_name + QStringLiteral(" (system dialog ") + title + QLatin1Char(')'), image);
                PostMessageW(HWND(h), WM_COMMAND, IDCANCEL, 0);
                wait(400);
                if (IsWindowVisible(HWND(h))) PostMessageW(HWND(h), WM_CLOSE, 0, 0);
                wait(300);
                handled++;
                quiet.restart();
            }
#endif
            else if (quiet.elapsed() > (handled ? 400 : patience)) {
                finished = true;
            }
            busy = false;
        };
        QTimer poll;
        poll.setInterval(50);
        connect(&poll, &QTimer::timeout, &poll, step);
        poll.start();
        trigger();
        quiet.restart();
        step();
        while (!finished) wait(25);
        poll.stop();
        return handled;
    }

    // ---- menus ----

    /// Saves an open menu and its submenus, listing their items into \p map
    void captureMenu(QMenu *menu, const QString &section, const QString &name, const QRect &context, int depth, QString *map)
    {
        wait(60);
        const QRect shown = menu->geometry().united(context.isValid() ? context : menu->geometry());
        QStringList items;
        Q_FOREACH (QAction *a, menu->actions()) {
            if (a->isVisible()) items << entry(a);
        }
        save(section, name, composite(around(shown)), items.join(QLatin1Char('\n')));
        if (depth > 5) return;
        Q_FOREACH (QAction *a, menu->actions()) {
            if (!a->isVisible()) continue;
            if (map) *map += QString(depth * 2 + 2, QLatin1Char(' ')) + QStringLiteral("- ") + entry(a) + QLatin1Char('\n');
            QMenu *sub = a->menu();
            if (!sub || sub == menu) continue;
            sub->popup(menu->mapToGlobal(menu->actionGeometry(a).topRight()));
            wait(120);
            if (sub->isVisible()) {
                captureMenu(sub, section, name + QStringLiteral(" › ") + label(a->text()), menu->geometry(), depth + 1, map);
            }
            sub->hide();
            wait(30);
        }
    }

    void popupShot(QWidget *win, const QString &section, const QString &name, const QRect &anchor)
    {
        if (auto *menu = qobject_cast<QMenu *>(win)) {
            captureMenu(menu, section, name, anchor, 0, nullptr);
            return;
        }
        QPointer<QWidget> guard(win);
        wait(200);
        if (!guard || !guard->isVisible()) return;
        save(section, name, composite(around(guard->geometry().united(anchor))), formText(guard));
    }

    // ---- forms ----

    QString formText(QWidget *top)
    {
        struct Line {
            int y, x;
            QString text;
        };
        QVector<Line> lines;
        Q_FOREACH (QWidget *w, top->findChildren<QWidget *>()) {
            if (!w->isVisible() || isInner(w, top)) continue;
            QString t;
            if (auto *g = qobject_cast<QGroupBox *>(w)) {
                if (!g->title().isEmpty()) t = QStringLiteral("▌") + (g->isCheckable() ? (g->isChecked() ? QStringLiteral("☑ ") : QStringLiteral("☐ ")) : QString()) + label(g->title());
            } else if (auto *c = qobject_cast<QCheckBox *>(w)) {
                t = (c->isChecked() ? QStringLiteral("☑ ") : QStringLiteral("☐ ")) + label(c->text());
            } else if (auto *r = qobject_cast<QRadioButton *>(w)) {
                t = (r->isChecked() ? QStringLiteral("◉ ") : QStringLiteral("○ ")) + label(r->text());
            } else if (auto *b = qobject_cast<QAbstractButton *>(w)) {
                const QString text = label(b->text());
                t = QStringLiteral("[%1]").arg(text.isEmpty() ? QStringLiteral("icon") : text);
                if (b->isCheckable()) t += b->isChecked() ? QStringLiteral("(on)") : QStringLiteral("(off)");
            } else if (auto *l = qobject_cast<QLabel *>(w)) {
                t = plain(l->text());
            } else if (auto *c = qobject_cast<QComboBox *>(w)) {
                QStringList items;
                for (int i = 0; i < c->count() && i < 40; i++) items << c->itemText(i);
                if (c->count() > 40) items << QStringLiteral("… %1 in all").arg(c->count());
                t = QStringLiteral("▾ %1  {%2}").arg(c->currentText(), items.join(QStringLiteral(" | ")));
            } else if (auto *s = qobject_cast<QAbstractSpinBox *>(w)) {
                t = QStringLiteral("⌗ %1").arg(s->text());
            } else if (auto *e = qobject_cast<QLineEdit *>(w)) {
                t = QStringLiteral("✎ %1").arg(e->text().isEmpty() ? e->placeholderText() : e->text());
            } else if (auto *s = qobject_cast<QSlider *>(w)) {
                t = QStringLiteral("━ %1 (%2–%3)").arg(s->value()).arg(s->minimum()).arg(s->maximum());
            } else if (auto *tabs = qobject_cast<QTabBar *>(w)) {
                QStringList names;
                for (int i = 0; i < tabs->count(); i++) names << (i == tabs->currentIndex() ? QStringLiteral("*%1*").arg(label(tabs->tabText(i))) : label(tabs->tabText(i)));
                t = QStringLiteral("Tabs: ") + names.join(QStringLiteral(" | "));
            } else if (auto *v = qobject_cast<QAbstractItemView *>(w)) {
                QStringList names;
                QAbstractItemModel *m = v->model();
                const int rows = m ? m->rowCount(v->rootIndex()) : 0;
                for (int i = 0; i < rows && i < 30; i++) names << plain(m->index(i, 0, v->rootIndex()).data().toString());
                if (rows > 30) names << QStringLiteral("… %1 in all").arg(rows);
                t = QStringLiteral("List (%1): ").arg(rows) + names.join(QStringLiteral(" | "));
            }
            if (t.trimmed().isEmpty()) continue;
            const QString tip = plain(w->toolTip());
            if (!tip.isEmpty() && !t.contains(tip)) t += QStringLiteral("  ⓘ ") + tip;
            const QPoint p = w->mapTo(top, QPoint(0, 0));
            lines.append({p.y(), p.x(), t});
        }
        std::sort(lines.begin(), lines.end(), [](const Line &a, const Line &b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
        QStringList rows;
        int rowY = -1000;
        QVector<Line> row;
        auto flush = [&]() {
            std::sort(row.begin(), row.end(), [](const Line &a, const Line &b) { return a.x < b.x; });
            QStringList parts;
            Q_FOREACH (const Line &l, row) parts << l.text;
            if (!parts.isEmpty()) rows << parts.join(QStringLiteral("    "));
            row.clear();
        };
        Q_FOREACH (const Line &l, lines) {
            if (l.y - rowY > 10) {
                flush();
                rowY = l.y;
            }
            row.append(l);
        }
        flush();
        return rows.join(QLatin1Char('\n'));
    }

    // ---- sweeps ----

    QList<QPair<QWidget *, QPoint>> probes(QWidget *w, int items)
    {
        QList<QPair<QWidget *, QPoint>> list;
        if (auto *view = qobject_cast<QAbstractItemView *>(w)) {
            QAbstractItemModel *m = view->model();
            const int rows = m ? m->rowCount(view->rootIndex()) : 0;
            for (int i = 0; i < rows && list.size() < items; i++) {
                const QRect r = view->visualRect(m->index(i, 0, view->rootIndex()));
                if (r.isValid() && view->viewport()->rect().intersects(r)) list << qMakePair(view->viewport(), (r & view->viewport()->rect()).center());
            }
            return list;
        }
        list << qMakePair(w, w->visibleRegion().boundingRect().center());
        return list;
    }

    void sweepTooltips(QWidget *root, const QString &folder, QSet<QString> &seen)
    {
        if (!phase("tooltips")) return;
        hideTips();
        m_mainShot = m_win ? m_win->grab() : QPixmap();
        QList<QPointer<QWidget>> widgets;
        widgets << root;
        Q_FOREACH (QWidget *w, root->findChildren<QWidget *>()) widgets << w;
        int shots = 0;
        Q_FOREACH (const QPointer<QWidget> &w, widgets) {
            if (!w || !w->isVisible() || w->visibleRegion().isEmpty() || isViewport(w)) continue;
            Q_FOREACH (const auto &probe, probes(w, 3)) {
                QHelpEvent event(QEvent::ToolTip, probe.second, probe.first->mapToGlobal(probe.second));
                QApplication::sendEvent(probe.first, &event);
                const QList<QWidget *> tips = visibleTips();
                if (tips.isEmpty()) continue;
                const QString text = QToolTip::isVisible() ? QToolTip::text() : QString();
                const QString key = text.isEmpty() ? QStringLiteral("picture:%1").arg(quintptr(w.data())) : text;
                if (seen.contains(key)) {
                    hideTips();
                    continue;
                }
                seen.insert(key);
                QRect region = qobject_cast<QAbstractItemView *>(w) ? QRect(probe.first->mapToGlobal(probe.second) - QPoint(40, 20), QSize(80, 40))
                                                                     : QRect(w->mapToGlobal(w->visibleRegion().boundingRect().topLeft()), w->visibleRegion().boundingRect().size());
                Q_FOREACH (QWidget *tip, tips) region |= tip->geometry();
                const QString what = describe(w);
                const QString shown = text.isEmpty() ? QStringLiteral("(picture tooltip)") : plain(text);
                save(folder, what + QStringLiteral(" — ") + shown.left(50), composite(around(region, 16), true), shown);
                m_tipMap += QStringLiteral("- %1 › **%2** — %3\n").arg(folder.section(QLatin1Char('/'), 1), what, shown);
                hideTips();
                shots++;
            }
        }
        m_mainShot = QPixmap();
        note(QStringLiteral("  tooltips %1: %2").arg(folder).arg(shots));
    }

    void sweepContextMenus(QWidget *root, const QString &section, const QString &prefix, QSet<QString> &seen)
    {
        QList<QPointer<QWidget>> widgets;
        widgets << root;
        Q_FOREACH (QWidget *w, root->findChildren<QWidget *>()) widgets << w;
        QWidget *canvas = canvasWidget();
        Q_FOREACH (const QPointer<QWidget> &w, widgets) {
            if (!w || !w->isVisible() || w->visibleRegion().isEmpty() || isViewport(w) || w == canvas) continue;
            const Qt::ContextMenuPolicy policy = w->contextMenuPolicy();
            if (policy == Qt::NoContextMenu || policy == Qt::PreventContextMenu) continue;
            auto list = probes(w, 1);
            if (auto *view = qobject_cast<QAbstractItemView *>(w.data())) {
                // and off the items
                const QRect vp = view->viewport()->rect();
                list << qMakePair(view->viewport(), QPoint(vp.right() - 4, vp.bottom() - 4));
            }
            Q_FOREACH (const auto &probe, list) {
                QPointer<QWidget> target = probe.first;
                const QPoint pos = probe.second;
                const QRect anchor(target->mapToGlobal(pos) - QPoint(2, 2), QSize(4, 4));
                const QString what = describe(w);
                m_section = section;
                m_name = prefix + what;
                capture(
                    [target, pos]() {
                        if (!target) return;
                        QContextMenuEvent event(QContextMenuEvent::Mouse, pos, target->mapToGlobal(pos));
                        QApplication::sendEvent(target, &event);
                    },
                    [&](QWidget *win) {
                        auto *menu = qobject_cast<QMenu *>(win);
                        const QString sig = menu ? signature(menu) : QString::fromLatin1(win->metaObject()->className());
                        if (seen.contains(sig)) return;
                        seen.insert(sig);
                        note(QStringLiteral("  context menu: %1").arg(prefix + what));
                        popupShot(win, section, prefix + what, anchor);
                    },
                    40);
            }
        }
    }

    void sweepButtons(QWidget *root, const QString &section, const QString &prefix, const std::function<bool(QAbstractButton *)> &clickable)
    {
        QList<QPointer<QAbstractButton>> buttons;
        Q_FOREACH (QAbstractButton *b, root->findChildren<QAbstractButton *>()) buttons << b;
        QSet<QString> seen;
        Q_FOREACH (const QPointer<QAbstractButton> &b, buttons) {
            if (!b || !b->isVisible() || !b->isEnabled() || b->visibleRegion().isEmpty()) continue;
            QPointer<QToolButton> tool = qobject_cast<QToolButton *>(b.data());
            QPointer<QPushButton> push = qobject_cast<QPushButton *>(b.data());
            std::function<void()> trigger;
            if (tool && tool->menu()) {
                trigger = [tool]() { if (tool) tool->showMenu(); };
            } else if (push && push->menu()) {
                trigger = [push]() { if (push) push->showMenu(); };
            } else if (b->inherits("KisPopupButton") || (clickable && clickable(b))) {
                trigger = [b]() { if (b) b->click(); };
            } else {
                continue;
            }
            const QString what = describe(b);
            const bool wasChecked = b->isChecked();
            const QRect anchor = globalRect(b);
            m_section = section;
            m_name = prefix + what;
            const int opened = capture(trigger, [&](QWidget *win) {
                const QString key = what + QLatin1Char(':') + QString::fromLatin1(win->metaObject()->className());
                if (seen.contains(key)) return;
                seen.insert(key);
                note(QStringLiteral("  popup: %1").arg(prefix + what));
                popupShot(win, section, prefix + what, anchor);
            }, 500);
            if (m_studio) m_studio->closeFlyout();
            if (b && !opened && b->isCheckable() && b->isChecked() != wasChecked) b->click();
            wait(40);
        }
    }

    // ---- dialogs and their pages ----

    struct Pager {
        QPointer<QWidget> widget;
        QPointer<QStackedWidget> stack; ///< what a list turns: rows that show the same page are left out
        QList<QPersistentModelIndex> indexes; // item views
        int count() const
        {
            if (auto *t = qobject_cast<QTabWidget *>(widget)) return t->count();
            if (auto *t = qobject_cast<QTabBar *>(widget)) return t->count();
            return indexes.size();
        }
        int current() const
        {
            if (auto *t = qobject_cast<QTabWidget *>(widget)) return t->currentIndex();
            if (auto *t = qobject_cast<QTabBar *>(widget)) return t->currentIndex();
            auto *v = qobject_cast<QAbstractItemView *>(widget);
            return v ? indexes.indexOf(QPersistentModelIndex(v->currentIndex())) : -1;
        }
        void set(int i) const
        {
            if (i < 0 || !widget) return;
            if (auto *t = qobject_cast<QTabWidget *>(widget)) t->setCurrentIndex(i);
            else if (auto *t = qobject_cast<QTabBar *>(widget)) t->setCurrentIndex(i);
            else if (auto *v = qobject_cast<QAbstractItemView *>(widget)) {
                v->setCurrentIndex(indexes[i]);
                if (stack) QMetaObject::invokeMethod(v, "clicked", Q_ARG(QModelIndex, QModelIndex(indexes[i])));
            }
        }
        QString title(int i) const
        {
            if (auto *t = qobject_cast<QTabWidget *>(widget)) return label(t->tabText(i));
            if (auto *t = qobject_cast<QTabBar *>(widget)) return label(t->tabText(i));
            return plain(indexes[i].data().toString());
        }
    };

    static void allIndexes(QAbstractItemModel *m, const QModelIndex &parent, QList<QPersistentModelIndex> &out)
    {
        for (int i = 0; i < m->rowCount(parent); i++) {
            const QModelIndex idx = m->index(i, 0, parent);
            out << idx;
            allIndexes(m, idx, out);
        }
    }

    static int depthIn(QWidget *w, QWidget *top)
    {
        int depth = 0;
        for (QWidget *p = w; p && p != top; p = p->parentWidget()) depth++;
        return depth;
    }

    /// The outermost unexplored control that switches pages in \p top: a KPageDialog's list, a list
    /// turning a stack of pages, or tabs (in that order)
    Pager nextPager(QWidget *top, QSet<QObject *> &explored)
    {
        Pager best;
        int bestRank = INT_MAX;
        auto consider = [&](const Pager &pager, int kind, QWidget *w) {
            const int rank = kind * 1000 + depthIn(w, top);
            if (rank < bestRank) {
                bestRank = rank;
                best = pager;
            }
        };
        Q_FOREACH (QWidget *w, top->findChildren<QWidget *>()) {
            if (!w->isVisible() || explored.contains(w)) continue;
            if (auto *tabs = qobject_cast<QTabWidget *>(w)) {
                if (tabs->count() > 1) {
                    Pager pager;
                    pager.widget = w;
                    consider(pager, 2, w);
                }
            } else if (auto *bar = qobject_cast<QTabBar *>(w)) {
                if (bar->count() > 1 && !qobject_cast<QTabWidget *>(bar->parentWidget())) {
                    Pager pager;
                    pager.widget = w;
                    consider(pager, 2, w);
                }
            } else if (auto *view = qobject_cast<QAbstractItemView *>(w)) {
                QAbstractItemModel *m = view->model();
                if (!m) continue;
                if (QByteArray(m->metaObject()->className()).contains("KPage")) {
                    Pager pager;
                    pager.widget = w;
                    allIndexes(m, QModelIndex(), pager.indexes);
                    if (pager.indexes.size() > 1) consider(pager, 0, w);
                    continue;
                }
                if (bestRank < 1000) continue;
                // a flat list that turns the pages of a stack next to it
                const int rows = m->rowCount(view->rootIndex());
                if (rows < 2 || rows > 80) continue;
                bool nested = false;
                for (int i = 0; i < rows && !nested; i++) nested = m->rowCount(m->index(i, 0, view->rootIndex())) > 0;
                if (qobject_cast<QTreeView *>(w) && nested) continue;
                QList<QStackedWidget *> stacks;
                Q_FOREACH (QStackedWidget *s, top->findChildren<QStackedWidget *>()) {
                    if (s->isVisible() && s->count() > 1) stacks << s;
                }
                if (stacks.isEmpty()) continue;
                QList<int> before;
                Q_FOREACH (QStackedWidget *s, stacks) before << s->currentIndex();
                const QModelIndex was = view->currentIndex();
                const int row = was.isValid() ? was.row() : 0;
                QStackedWidget *turned = nullptr;
                // the next rows, selected and clicked (a header row may turn nothing)
                for (int step = 1; step <= 3 && !turned; step++) {
                    const QModelIndex next = m->index((row + step) % rows, 0, view->rootIndex());
                    view->setCurrentIndex(next);
                    QMetaObject::invokeMethod(view, "clicked", Q_ARG(QModelIndex, next));
                    for (int i = 0; i < stacks.size() && !turned; i++) {
                        if (stacks[i]->currentIndex() != before[i]) turned = stacks[i];
                    }
                }
                const QModelIndex back = was.isValid() ? was : m->index(0, 0, view->rootIndex());
                view->setCurrentIndex(back);
                QMetaObject::invokeMethod(view, "clicked", Q_ARG(QModelIndex, back));
                if (turned) {
                    Pager pager;
                    pager.widget = w;
                    pager.stack = turned;
                    for (int i = 0; i < rows; i++) pager.indexes << m->index(i, 0, view->rootIndex());
                    consider(pager, 1, w);
                }
            }
        }
        return best;
    }

    void leaf(QWidget *top, const QString &section, const QString &name)
    {
        const bool popup = top->windowType() == Qt::Popup || top->windowType() == Qt::Tool || top == m_win;
        const QString form = formText(top);
        save(section, name, popup ? composite(around(top->geometry())) : top->grab().toImage(), form);
        m_formMap += QStringLiteral("\n## %1\n\n```\n%2\n```\n").arg(name, form);
        Q_FOREACH (QScrollArea *area, top->findChildren<QScrollArea *>()) {
            if (!area->isVisible() || !area->widget() || area->viewport()->width() < 120) continue;
            if (area->widget()->height() > area->viewport()->height() + 16) {
                save(section, name + QStringLiteral(" (whole, scrolled)"), area->widget()->grab().toImage());
            }
        }
        if (!m_tipFolder.isEmpty()) sweepTooltips(top, m_tipFolder, m_tipSeen);
    }

    void explore(QWidget *top, const QString &section, const QString &name, QSet<QObject *> &explored, int depth = 0)
    {
        const Pager pager = depth < 5 ? nextPager(top, explored) : Pager();
        if (!pager.widget) {
            leaf(top, section, name);
            return;
        }
        explored.insert(pager.widget);
        const int original = pager.current();
        QSet<int> shown;
        for (int i = 0; i < pager.count() && top->isVisible(); i++) {
            pager.set(i);
            wait(250);
            if (pager.stack) {
                if (shown.contains(pager.stack->currentIndex())) continue;
                shown.insert(pager.stack->currentIndex());
            }
            explore(top, section, name + QStringLiteral(" › ") + pager.title(i), explored, depth + 1);
        }
        pager.set(original);
        wait(50);
    }

    void exploreWindow(QWidget *win, const QString &section, const QString &name)
    {
        if (auto *menu = qobject_cast<QMenu *>(win)) {
            captureMenu(menu, section, name, QRect(), 0, nullptr);
            return;
        }
        QSet<QObject *> explored;
        explore(win, section, name, explored);
    }

    // ---- the parts of the tour ----

    QWidget *canvasWidget() const
    {
        KisView *view = m_win ? m_win->activeView() : nullptr;
        return view && view->canvasBase() ? view->canvasBase()->canvasWidget() : nullptr;
    }

    QAction *action(const char *name) const
    {
        return m_win ? m_win->actionCollection()->action(QLatin1String(name)) : nullptr;
    }

    void trigger(const char *name, int ms = 300)
    {
        if (QAction *a = action(name)) {
            a->trigger();
            wait(ms);
        } else {
            note(QStringLiteral("  no action %1").arg(QLatin1String(name)));
        }
    }

    void tick(QTimer *poll)
    {
        if (!m_splashDone && phase("splash")) {
            Q_FOREACH (QWidget *w, QApplication::topLevelWidgets()) {
                if (w->inherits("KisSplashScreen") && w->isVisible()) {
                    if (!m_splashSeen.isValid()) m_splashSeen.start();
                    if (m_splashSeen.elapsed() > 700) {
                        save(QStringLiteral("00 splash"), QStringLiteral("Splash"), w->grab().toImage());
                        m_splashDone = true;
                    }
                }
            }
        }
        if (!KisPart::exists()) return;
        KisMainWindow *win = KisPart::instance()->currentMainwindow();
        if (!win || !win->isVisible() || !win->activeView()) return;
        if (!m_ready.isValid()) {
            m_ready.start();
            return;
        }
        if (m_ready.elapsed() < 6000) return;
        poll->stop();
        m_win = win;
        m_studio = win->findChild<KisStudioLayout *>();
        if (m_win->isMinimized()) m_win->showNormal();
        m_win->raise();
        wait(1500);
        auto geometry = [this]() {
            const QRect g = m_win->geometry();
            QScreen *screen = m_win->screen();
            const QRect sg = screen ? screen->geometry() : QRect();
            return QStringLiteral("window %1,%2 %3x%4 state %5, screen %6,%7 %8x%9").arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height())
                .arg(int(m_win->windowState())).arg(sg.x()).arg(sg.y()).arg(sg.width()).arg(sg.height());
        };
        note(geometry());
        // pictures are of what is on the screen
        if (QScreen *screen = m_win->screen()) {
            if (!screen->geometry().contains(m_win->geometry())) {
                m_win->showMaximized();
                wait(1500);
                note(QStringLiteral("maximized: ") + geometry());
            }
        }
        run();
    }

    void run()
    {
        QElapsedTimer total;
        total.start();
        if (phase("inventory")) inventory();
        if (phase("window")) windowStates();
        if (phase("menus")) menus();
        if (phase("chrome")) chrome();
        if (phase("panels")) panels();
        if (phase("tools")) tools();
        if (phase("canvas")) canvas();
        if (phase("dialogs")) dialogs();
        if (phase("layers")) layers();
        if (phase("pages")) pages();
        if (phase("brusheditor")) brushEditor();
        if (phase("effects")) effects();
        appendText(QStringLiteral("map-settings-and-dialogs.md"), QStringLiteral("# Dialogs, settings and panels, page by page\n"), m_formMap);
        appendText(QStringLiteral("map-tooltips.md"), QStringLiteral("# Tooltips\n\n"), m_tipMap);
        appendText(QStringLiteral("map-internal-errors.md"), QStringLiteral("# Internal errors Krita showed during the tour (answered Ignore)\n\n"), m_errors);
        m_errors.clear();
        m_formMap.clear();
        m_tipMap.clear();
        if (phase("welcome")) welcome();
        note(QStringLiteral("tour done in %1 s").arg(total.elapsed() / 1000));
        writeText(QStringLiteral("done.txt"), QStringLiteral("done\n"));
    }

    /// Maps every action with its menu path, and the window's widgets; the dialogs phase goes through these
    void inventory()
    {
        note(QStringLiteral("inventory"));
        m_menuOrder.clear();
        m_menuPath.clear();
        std::function<void(QMenu *, const QString &, QSet<QMenu *> &)> walk = [&](QMenu *menu, const QString &path, QSet<QMenu *> &seen) {
            if (seen.contains(menu)) return;
            seen.insert(menu);
            QMetaObject::invokeMethod(menu, "aboutToShow");
            Q_FOREACH (QAction *a, menu->actions()) {
                if (a->isSeparator()) continue;
                const QString here = path + QStringLiteral(" › ") + label(a->text());
                if (a->menu()) {
                    walk(a->menu(), here, seen);
                } else if (!m_menuPath.contains(a)) {
                    m_menuPath.insert(a, path);
                    m_menuOrder << a;
                }
            }
        };
        QSet<QMenu *> seen;
        Q_FOREACH (QAction *top, m_win->menuBar()->actions()) {
            if (top->menu()) walk(top->menu(), label(top->text()), seen);
        }
        QString map = QStringLiteral("# Actions\n\nname | text | shortcut | menu | tooltip\n--- | --- | --- | --- | ---\n");
        QList<QAction *> all = m_win->actionCollection()->actions();
        std::sort(all.begin(), all.end(), [](QAction *a, QAction *b) { return a->objectName() < b->objectName(); });
        Q_FOREACH (QAction *a, all) {
            map += QStringLiteral("%1 | %2 | %3 | %4 | %5\n").arg(a->objectName(), label(a->text()), shortcutOf(a), m_menuPath.value(a), plain(a->toolTip()));
        }
        writeText(QStringLiteral("map-actions.md"), map);

        QString widgets;
        std::function<void(QWidget *, int)> tree = [&](QWidget *w, int depth) {
            if (!w->isVisible()) return;
            const QRect g = globalRect(w);
            widgets += QString(depth * 2, QLatin1Char(' '))
                + QStringLiteral("%1 \"%2\" %3,%4 %5x%6").arg(QString::fromLatin1(w->metaObject()->className()), w->objectName()).arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height());
            if (auto *b = qobject_cast<QAbstractButton *>(w)) widgets += QStringLiteral(" text=\"%1\"").arg(b->text());
            if (!w->toolTip().isEmpty()) widgets += QStringLiteral(" tip=\"%1\"").arg(plain(w->toolTip()).left(80));
            if (w->contextMenuPolicy() != Qt::DefaultContextMenu) widgets += QStringLiteral(" ctx=%1").arg(int(w->contextMenuPolicy()));
            widgets += QLatin1Char('\n');
            Q_FOREACH (QObject *child, w->children()) {
                if (auto *cw = qobject_cast<QWidget *>(child)) {
                    if (!cw->isWindow()) tree(cw, depth + 1);
                }
            }
        };
        Q_FOREACH (QWidget *top, visibleTops()) tree(top, 0);
        widgets += QStringLiteral("\n# docks\n");
        Q_FOREACH (QDockWidget *dock, m_win->dockWidgets()) widgets += QStringLiteral("%1 \"%2\" visible=%3\n").arg(dock->objectName(), dock->windowTitle()).arg(dock->isVisible());
        widgets += QStringLiteral("\n# tools\n");
        Q_FOREACH (KoToolFactoryBase *f, KoToolRegistry::instance()->values()) widgets += QStringLiteral("%1 \"%2\" %3\n").arg(f->id(), f->toolTip(), f->section());
        writeText(QStringLiteral("inventory.txt"), widgets);
    }

    void windowStates()
    {
        note(QStringLiteral("window"));
        const QString section = QStringLiteral("01 window");
        save(section, QStringLiteral("Window"), windowShot());
        trigger("view_show_canvas_only", 900);
        save(section, QStringLiteral("Canvas only"), windowShot());
        trigger("view_show_canvas_only", 900);
        if (QAction *dockers = action("view_toggledockers")) {
            dockers->trigger();
            wait(700);
            save(section, QStringLiteral("Dockers hidden"), windowShot());
            dockers->trigger();
            wait(700);
        }
    }

    void menus()
    {
        note(QStringLiteral("menus"));
        QString map = QStringLiteral("# Menus\n\n");
        Q_FOREACH (QAction *top, m_win->menuBar()->actions()) {
            QMenu *menu = top->menu();
            if (!menu || !top->isVisible()) continue;
            map += QStringLiteral("\n## %1\n\n").arg(label(top->text()));
            menu->popup(m_win->mapToGlobal(QPoint(60, 50)));
            wait(200);
            if (menu->isVisible()) captureMenu(menu, QStringLiteral("02 menus"), label(top->text()), QRect(), 0, &map);
            menu->hide();
            wait(50);
        }
        writeText(QStringLiteral("map-menus.md"), map);
    }

    bool insideDock(QWidget *w) const
    {
        for (QWidget *p = w; p; p = p->parentWidget()) {
            if (qobject_cast<QDockWidget *>(p)) return true;
        }
        return false;
    }

    /// The popups, context menus and tooltips of the window's own controls (outside the panels)
    void chrome()
    {
        note(QStringLiteral("chrome"));
        if (begin(QStringLiteral("chrome")) != Step::Full) return;
        static const QRegularExpression risky(QStringLiteral("close|quit|exit|delete|remove|clear|save|minimi|maximi|restore|new |undo|redo|reset|full ?screen"),
                                              QRegularExpression::CaseInsensitiveOption);
        sweepButtons(m_win, QStringLiteral("04 popups/Window"), QString(), [this](QAbstractButton *b) {
            if (insideDock(b)) return false;
            const QString text = b->toolTip() + QLatin1Char(' ') + b->text() + QLatin1Char(' ') + b->objectName();
            return !risky.match(text).hasMatch();
        });
        QSet<QString> seenMenus;
        sweepContextMenus(m_win, QStringLiteral("03 context menus"), QStringLiteral("Window — "), seenMenus);
        QSet<QString> seenTips;
        sweepTooltips(m_win, QStringLiteral("09 tooltips/Window"), seenTips);
        end(QStringLiteral("chrome"));
    }

    void panels()
    {
        note(QStringLiteral("panels"));
        QList<QDockWidget *> docks = m_win->dockWidgets();
        std::sort(docks.begin(), docks.end(), [](QDockWidget *a, QDockWidget *b) { return a->windowTitle() < b->windowTitle(); });
        QSet<QString> seenMenus;
        Q_FOREACH (QDockWidget *dock, docks) {
            const QString title = fileName(label(dock->windowTitle()).isEmpty() ? dock->objectName() : label(dock->windowTitle()));
            const QString key = QStringLiteral("panel ") + dock->objectName();
            const Step step = begin(key);
            if (step == Step::Skip) continue;
            note(QStringLiteral(" panel %1").arg(title));
            const bool wasVisible = dock->isVisible();
            if (m_studio) m_studio->showPanel(dock);
            else dock->show();
            wait(700);
            if (!dock->isVisible()) {
                note(QStringLiteral("  not shown"));
                continue;
            }
            save(QStringLiteral("05 panels"), title, composite(around(globalRect(dock), 16)), formText(dock));
            Q_FOREACH (QScrollArea *area, dock->findChildren<QScrollArea *>()) {
                if (area->isVisible() && area->widget() && area->widget()->height() > area->viewport()->height() + 16 && area->viewport()->width() > 120) {
                    save(QStringLiteral("05 panels"), title + QStringLiteral(" (whole, scrolled)"), area->widget()->grab().toImage());
                }
            }
            if (step == Step::Full) {
                sweepButtons(dock, QStringLiteral("04 popups/Panels"), title + QStringLiteral(" — "), nullptr);
                sweepContextMenus(dock, QStringLiteral("03 context menus"), QStringLiteral("Panel ") + title + QStringLiteral(" — "), seenMenus);
                QSet<QString> seenTips;
                sweepTooltips(dock, QStringLiteral("09 tooltips/Panels/") + title, seenTips);
            }
            if (!wasVisible) {
                if (m_studio) m_studio->closeFlyout();
                if (dock->isVisible()) dock->hide();
                wait(200);
            }
            end(key);
        }
    }

    void tools()
    {
        note(QStringLiteral("tools"));
        QList<KoToolFactoryBase *> factories = KoToolRegistry::instance()->values();
        std::sort(factories.begin(), factories.end(), [](KoToolFactoryBase *a, KoToolFactoryBase *b) {
            return a->section() != b->section() ? a->section() < b->section() : a->priority() < b->priority();
        });
        QDockWidget *options = m_win->dockWidget(QStringLiteral("sharedtooldocker"));
        QList<KoToolFactoryBase *> later;
        auto visit = [&](KoToolFactoryBase *f) {
            KoToolManager::instance()->switchToolRequested(f->id());
            wait(500);
            if (KoToolManager::instance()->activeToolId() != f->id()) return false;
            const QString key = QStringLiteral("tool ") + f->id();
            const Step step = begin(key);
            if (step == Step::Skip) return true;
            const QString name = fileName(f->toolTip().isEmpty() ? f->id() : f->toolTip());
            note(QStringLiteral(" tool %1").arg(name));
            if (options) {
                if (m_studio) m_studio->showPanel(options);
                else options->show();
                wait(500);
            }
            if (options && options->isVisible()) {
                save(QStringLiteral("06 tool options"), name, composite(around(globalRect(options), 16)), formText(options));
                if (step == Step::Full) {
                    sweepButtons(options, QStringLiteral("04 popups/Tool options"), name + QStringLiteral(" — "), nullptr);
                    QSet<QString> seenTips;
                    sweepTooltips(options, QStringLiteral("09 tooltips/Tool options/") + name, seenTips);
                }
            } else {
                save(QStringLiteral("06 tool options"), name + QStringLiteral(" (window)"), windowShot());
            }
            end(key);
            return true;
        };
        Q_FOREACH (KoToolFactoryBase *f, factories) {
            if (!visit(f)) later << f;
        }
        if (!later.isEmpty()) {
            // the tools for vector shapes, on a vector layer
            trigger("add_new_shape_layer", 600);
            Q_FOREACH (KoToolFactoryBase *f, later) {
                if (!visit(f)) note(QStringLiteral(" tool not available: %1").arg(f->id()));
            }
        }
        if (m_studio) m_studio->closeFlyout();
        KoToolManager::instance()->switchToolRequested(QStringLiteral("KritaShape/KisToolBrush"));
        wait(400);
    }

    void mouse(QWidget *w, const QPoint &pos, QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons buttons)
    {
        QWidget *top = w->window();
        if (!top->windowHandle()) return;
        const QPointF local = w->mapTo(top, pos);
        QWindowSystemInterface::handleMouseEvent(top->windowHandle(), local, QPointF(w->mapToGlobal(pos)), buttons, button, type);
    }

    void canvas()
    {
        note(QStringLiteral("canvas"));
        const QString key = phase("canvasonly") ? QStringLiteral("canvas right clicks") : QStringLiteral("canvas");
        if (begin(key) != Step::Full) return;
        const QString section = QStringLiteral("07 canvas");
        QWidget *canvas = canvasWidget();
        if (!canvas) return;
        const QPoint center = canvas->rect().center();
        auto rightClick = [&](const QString &name) {
            mouse(canvas, center, QEvent::MouseMove, Qt::NoButton, Qt::NoButton);
            wait(150);
            mouse(canvas, center, QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton);
            wait(500);
            save(section, name + QStringLiteral(" (held)"), windowShot());
            // a tool's menu opens on release and blocks in exec(): captured from within
            m_section = section;
            m_name = name;
            const QRect anchor(canvas->mapToGlobal(center) - QPoint(2, 2), QSize(4, 4));
            const int opened = capture([&]() { mouse(canvas, center, QEvent::MouseButtonRelease, Qt::RightButton, Qt::NoButton); },
                                       [&](QWidget *win) { popupShot(win, section, name + QStringLiteral(" (released)"), anchor); }, 700);
            if (!opened) save(section, name + QStringLiteral(" (released)"), windowShot());
            hideTips();
            Q_FOREACH (QWidget *w, visibleTops()) {
                if (w->windowType() == Qt::Popup) w->hide();
            }
            // Escape for what is drawn on the canvas
            if (canvas->window()->windowHandle()) {
                QWindowSystemInterface::handleKeyEvent(canvas->window()->windowHandle(), QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QWindowSystemInterface::handleKeyEvent(canvas->window()->windowHandle(), QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
            }
            wait(400);
        };
        KoToolManager::instance()->switchToolRequested(QStringLiteral("KritaShape/KisToolBrush"));
        wait(400);
        if (phase("canvasonly")) {
            Q_FOREACH (KoToolFactoryBase *f, KoToolRegistry::instance()->values()) {
                KoToolManager::instance()->switchToolRequested(f->id());
                wait(300);
                if (KoToolManager::instance()->activeToolId() != f->id()) continue;
                rightClick(QStringLiteral("Right click, ") + fileName(f->toolTip().isEmpty() ? f->id() : f->toolTip()));
            }
            KoToolManager::instance()->switchToolRequested(QStringLiteral("KritaShape/KisToolBrush"));
            wait(300);
            end(key);
            return;
        }
        rightClick(QStringLiteral("Right click, brush"));
        KoToolManager::instance()->switchToolRequested(QStringLiteral("KisToolSelectRectangular"));
        wait(400);
        rightClick(QStringLiteral("Right click, selection tool"));
        KoToolManager::instance()->switchToolRequested(QStringLiteral("KritaShape/KisToolBrush"));
        wait(400);
        static const char *const popups[] = {"show_brush_editor", "show_brush_presets", "find_anything", "studio_brush_picker", "studio_effects",
                                             "show_snap_options_popup", "show_color_selector", "show_mypaint_shade_selector", "show_minimal_shade_selector",
                                             "show_color_history", "show_common_colors", "show_wg_color_selector", "show_wg_shade_selector",
                                             "show_wg_mypaint_selector", "show_wg_color_history"};
        for (const char *name : popups) {
            QAction *a = action(name);
            if (!a) {
                note(QStringLiteral("  no action %1").arg(QLatin1String(name)));
                continue;
            }
            m_section = section;
            m_name = label(a->text());
            const int opened = capture([a]() { a->trigger(); }, [&](QWidget *win) {
                exploreWindow(win, section, label(a->text()));
            }, 900);
            if (!opened) {
                // drawn inside the window
                save(section, label(a->text()) + QStringLiteral(" (window)"), windowShot());
                if (a->isCheckable()) a->trigger();
                Q_FOREACH (QWidget *w, visibleTops()) {
                    if (w->windowType() == Qt::Popup) w->hide();
                }
                wait(300);
            }
        }
        end(key);
    }

    void readProgress()
    {
        QFile f(m_root + QStringLiteral("/progress.txt"));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        QSet<QString> begun;
        Q_FOREACH (const QByteArray &line, f.readAll().split('\n')) {
            const QString s = QString::fromUtf8(line);
            if (s.startsWith(QLatin1String("begin "))) begun.insert(s.mid(6));
            if (s.startsWith(QLatin1String("end "))) m_finished.insert(s.mid(4));
        }
        m_crashed = begun - m_finished;
    }

    enum class Step { Skip, Light, Full };
    Step begin(const QString &key)
    {
        if (m_finished.contains(key)) return Step::Skip;
        const bool crashed = m_crashed.contains(key);
        if (crashed) note(QStringLiteral("  crashed before, only its picture: %1").arg(key));
        progress(QStringLiteral("begin ") + key);
        return crashed ? Step::Light : Step::Full;
    }
    void end(const QString &key) { progress(QStringLiteral("end ") + key); }

    void progress(const QString &line)
    {
        QFile f(m_root + QStringLiteral("/progress.txt"));
        if (f.open(QIODevice::Append | QIODevice::Text)) f.write((line + QLatin1Char('\n')).toUtf8());
    }

    void dialogs()
    {
        note(QStringLiteral("dialogs"));
        if (m_menuOrder.isEmpty()) inventory();
        const QString section = QStringLiteral("08 dialogs");
        // with a selection, which some dialogs need
        trigger("select_all", 300);
        // held weakly: menus rebuild some of their actions (recent files, themes, windows)
        QList<QPointer<QAction>> list;
        Q_FOREACH (QAction *a, m_menuOrder) list << a;
        Q_FOREACH (QAction *a, m_win->actionCollection()->actions()) {
            if (!m_menuOrder.contains(a)) list << a;
        }
        static const QSet<QString> never = {QStringLiteral("file_quit"), QStringLiteral("file_close"), QStringLiteral("file_close_all"),
                                            QStringLiteral("view_fullscreen"), QStringLiteral("file_reload_file")};
        int index = 0;
        QString skipped;
        Q_FOREACH (const QPointer<QAction> &a, list) {
            if (!a) continue;
            const QString text = label(a->text());
            const QString name = a->objectName();
            const bool dots = text.endsWith(QLatin1String("...")) || text.endsWith(QChar(0x2026));
            const bool extra = name == QLatin1String("file_documentinfo") || name == QLatin1String("help_about_app") || name == QLatin1String("help_about_kde");
            if (!dots && !extra) continue;
            if (never.contains(name) || (name.startsWith(QLatin1String("help_")) && !extra)) {
                skipped += QStringLiteral("- %1 (%2): skipped, leaves Krita\n").arg(text, name);
                continue;
            }
            index++;
            const QString key = name.isEmpty() ? text : name;
            const QString path = m_menuPath.contains(a.data()) ? m_menuPath.value(a.data()) + QStringLiteral(" › ") : QString();
            const QString title = QStringLiteral("%1 %2%3").arg(index, 3, 10, QLatin1Char('0')).arg(path, text);
            if (m_finished.contains(key)) continue;
            if (m_crashed.contains(key)) {
                skipped += QStringLiteral("- %1 (%2): crashed Krita\n").arg(text, name);
                continue;
            }
            if (!a->isEnabled()) {
                skipped += QStringLiteral("- %1 (%2): disabled\n").arg(text, name);
                continue;
            }
            note(QStringLiteral(" dialog %1 (%2)").arg(title, name));
            progress(QStringLiteral("begin ") + key);
            m_section = section;
            m_name = title;
            m_tipFolder = QStringLiteral("09 tooltips/Dialogs/") + fileName(path + text);
            m_tipSeen.clear();
            const int opened = capture([a]() { if (a) a->trigger(); }, [&](QWidget *win) { exploreWindow(win, section, title); }, 2500);
            m_tipFolder.clear();
            if (!opened) skipped += QStringLiteral("- %1 (%2): nothing opened\n").arg(text, name);
            progress(QStringLiteral("end ") + key);
            if (m_studio) m_studio->closeFlyout();
        }
        QFile f(m_root + QStringLiteral("/map-not-captured.md"));
        if (f.open(QIODevice::Append | QIODevice::Text)) f.write((QStringLiteral("\n# Dialog actions not captured\n\n") + skipped).toUtf8());
    }

    /// The properties of every kind of layer and mask
    void layers()
    {
        note(QStringLiteral("layers"));
        if (begin(QStringLiteral("layers")) != Step::Full) return;
        const QString section = QStringLiteral("10 layer properties");
        static const char *const kinds[][2] = {{"add_new_paint_layer", "Paint layer"},
                                               {"add_new_group_layer", "Group layer"},
                                               {"add_new_clone_layer", "Clone layer"},
                                               {"add_new_shape_layer", "Vector layer"},
                                               {"add_new_transparency_mask", "Transparency mask"},
                                               {"add_new_colorize_mask", "Colorize mask"},
                                               {"add_new_transform_mask", "Transform mask"},
                                               {"add_new_selection_mask", "Local selection"}};
        for (const auto &kind : kinds) {
            QAction *add = action(kind[0]);
            if (!add) continue;
            if (QString::fromLatin1(kind[0]).contains(QLatin1String("mask"))) trigger("select_all", 200);
            add->trigger();
            wait(600);
            QAction *properties = action("layer_properties");
            if (!properties) return;
            m_section = section;
            m_name = QString::fromLatin1(kind[1]);
            m_tipFolder = QStringLiteral("09 tooltips/Layer properties/") + m_name;
            m_tipSeen.clear();
            capture([properties]() { properties->trigger(); }, [&](QWidget *win) { exploreWindow(win, section, QString::fromLatin1(kind[1])); }, 1500);
            m_tipFolder.clear();
            // back to a paint layer, for what comes next
            if (QString::fromLatin1(kind[0]).contains(QLatin1String("mask"))) trigger("activatePreviousLayer", 200);
        }
        end(QStringLiteral("layers"));
    }

    /// The pages behind tabs in the panels and in the tool options
    void pages()
    {
        note(QStringLiteral("pages"));
        QList<QDockWidget *> docks = m_win->dockWidgets();
        std::sort(docks.begin(), docks.end(), [](QDockWidget *a, QDockWidget *b) { return a->windowTitle() < b->windowTitle(); });
        Q_FOREACH (QDockWidget *dock, docks) {
            if (dock->objectName() == QLatin1String("sharedtooldocker")) continue;
            const QString key = QStringLiteral("pages ") + dock->objectName();
            if (begin(key) == Step::Skip) continue;
            const QString title = fileName(label(dock->windowTitle()).isEmpty() ? dock->objectName() : label(dock->windowTitle()));
            const bool wasVisible = dock->isVisible();
            if (m_studio) m_studio->showPanel(dock);
            else dock->show();
            wait(600);
            QSet<QObject *> explored;
            if (dock->isVisible() && nextPager(dock, explored).widget) {
                note(QStringLiteral(" pages of %1").arg(title));
                m_tipFolder = QStringLiteral("09 tooltips/Panels/") + title + QStringLiteral(" (pages)");
                m_tipSeen.clear();
                explored.clear();
                explore(dock, QStringLiteral("05 panels"), title, explored);
                m_tipFolder.clear();
            }
            if (!wasVisible) {
                if (m_studio) m_studio->closeFlyout();
                if (dock->isVisible()) dock->hide();
                wait(150);
            }
            end(key);
        }
        QDockWidget *options = m_win->dockWidget(QStringLiteral("sharedtooldocker"));
        if (!options) return;
        Q_FOREACH (KoToolFactoryBase *f, KoToolRegistry::instance()->values()) {
            const QString key = QStringLiteral("pages ") + f->id();
            if (begin(key) == Step::Skip) continue;
            KoToolManager::instance()->switchToolRequested(f->id());
            wait(400);
            if (KoToolManager::instance()->activeToolId() == f->id()) {
                if (m_studio) m_studio->showPanel(options);
                else options->show();
                wait(400);
                QSet<QObject *> explored;
                if (options->isVisible() && nextPager(options, explored).widget) {
                    const QString name = fileName(f->toolTip().isEmpty() ? f->id() : f->toolTip());
                    note(QStringLiteral(" pages of %1").arg(name));
                    m_tipFolder = QStringLiteral("09 tooltips/Tool options/") + name + QStringLiteral(" (pages)");
                    m_tipSeen.clear();
                    explored.clear();
                    explore(options, QStringLiteral("06 tool options"), name, explored);
                    m_tipFolder.clear();
                }
            }
            end(key);
        }
        if (m_studio) m_studio->closeFlyout();
        KoToolManager::instance()->switchToolRequested(QStringLiteral("KritaShape/KisToolBrush"));
        wait(300);
    }

    /// The brush editor with a brush of every engine, page by page
    void brushEditor()
    {
        note(QStringLiteral("brush editor"));
        if (begin(QStringLiteral("brusheditor")) != Step::Full) return;
        const QString section = QStringLiteral("12 brush editor");
        QAction *show = action("show_brush_editor");
        if (!show) return;
        show->trigger();
        wait(800);
        QWidget *editor = nullptr;
        Q_FOREACH (QWidget *top, visibleTops()) {
            Q_FOREACH (QWidget *w, top->findChildren<QWidget *>()) {
                if (!editor && w->inherits("KisPaintOpPresetsEditor") && w->isVisible()) editor = w;
            }
        }
        if (!editor) {
            note(QStringLiteral("  no brush editor"));
            return;
        }
        QWidget *window = editor->window();
        QComboBox *engines = nullptr;
        Q_FOREACH (QComboBox *c, editor->findChildren<QComboBox *>()) {
            if (c->objectName() == QLatin1String("brushEngineComboBox")) engines = c;
        }
        QAbstractItemView *presets = nullptr;
        Q_FOREACH (QAbstractItemView *v, editor->findChildren<QAbstractItemView *>()) {
            if (v->isVisible() && v->inherits("KisResourceItemListView")) presets = v;
        }
        note(QStringLiteral("  engines %1, presets %2").arg(engines ? engines->count() : -1).arg(presets ? 1 : 0));
        // all brushes, not those of one tag
        Q_FOREACH (QComboBox *c, editor->findChildren<QComboBox *>()) {
            if (c == engines) continue;
            const int all = c->findText(QStringLiteral("All"));
            if (all >= 0 && c->count() > 2) {
                c->setCurrentIndex(all);
                QMetaObject::invokeMethod(c, "activated", Q_ARG(int, all));
                wait(300);
            }
        }
        auto shoot = [&](const QString &name) {
            const QRect region = window == m_win ? globalRect(editor) : window->geometry();
            QSet<QObject *> explored;
            if (presets) explored.insert(presets);
            if (engines) explored.insert(engines);
            m_tipFolder = QStringLiteral("09 tooltips/Brush editor/") + name;
            m_tipSeen.clear();
            const Pager pager = nextPager(editor, explored);
            if (!pager.widget) {
                save(section, name, composite(around(region, 8)), formText(editor));
            } else {
                explore(editor, section, name, explored);
            }
            m_tipFolder.clear();
        };
        if (!engines || !presets) {
            shoot(QStringLiteral("Brush editor"));
        } else {
            for (int i = 0; i < engines->count(); i++) {
                const QString engine = engines->itemText(i);
                if (engine == QLatin1String("All")) continue;
                engines->setCurrentIndex(i);
                QMetaObject::invokeMethod(engines, "activated", Q_ARG(int, i));
                wait(500);
                QAbstractItemModel *m = presets->model();
                if (!m || !m->rowCount()) {
                    note(QStringLiteral("  no brush of %1").arg(engine));
                    continue;
                }
                presets->setCurrentIndex(m->index(0, 0));
                wait(800);
                note(QStringLiteral("  engine %1").arg(engine));
                shoot(engine);
            }
        }
        if (window->isVisible()) show->trigger();
        wait(400);
        end(QStringLiteral("brusheditor"));
    }

    QWidget *effectOverlay() const
    {
        Q_FOREACH (QWidget *w, m_win->findChildren<QWidget *>()) {
            if (w->inherits("KisStudioEffectOverlay") && w->isVisible()) return w;
        }
        return nullptr;
    }

    /// The filters: in the studio layout they open over the canvas, not in a dialog
    void effects()
    {
        note(QStringLiteral("effects"));
        if (m_menuOrder.isEmpty()) inventory();
        const QString section = QStringLiteral("13 effects");
        QList<QPointer<QAction>> list;
        Q_FOREACH (QAction *a, m_menuOrder) {
            if (a->objectName().startsWith(QLatin1String("krita_filter_"))) list << a;
        }
        Q_FOREACH (const QPointer<QAction> &a, list) {
            if (!a || !a->isEnabled()) continue;
            const QString key = QStringLiteral("effect ") + a->objectName();
            const Step step = begin(key);
            if (step == Step::Skip) continue;
            const QString title = m_menuPath.value(a.data()) + QStringLiteral(" › ") + label(a->text());
            note(QStringLiteral(" effect %1").arg(title));
            m_section = section;
            m_name = title;
            m_tipFolder = step == Step::Full ? QStringLiteral("09 tooltips/Effects/") + fileName(label(a->text())) : QString();
            m_tipSeen.clear();
            const int opened = capture([a]() { if (a) a->trigger(); }, [&](QWidget *win) { exploreWindow(win, section, title); }, 1200);
            m_tipFolder.clear();
            QWidget *overlay = effectOverlay();
            if (!opened && overlay) {
                wait(1500);
                save(section, title, windowShot(), formText(overlay));
                if (step == Step::Full) {
                    QSet<QString> seenTips;
                    sweepTooltips(overlay, QStringLiteral("09 tooltips/Effects/") + fileName(label(a->text())), seenTips);
                }
                QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(overlay, &press);
                wait(500);
                if (QWidget *still = effectOverlay()) {
                    Q_FOREACH (QToolButton *b, still->findChildren<QToolButton *>()) {
                        if (b->text() == QStringLiteral("✕")) b->click();
                    }
                    wait(400);
                }
            } else if (!opened) {
                note(QStringLiteral("  nothing opened"));
            }
            end(key);
        }
    }

    void welcome()
    {
        note(QStringLiteral("welcome"));
        Q_FOREACH (QPointer<KisDocument> doc, KisPart::instance()->documents()) {
            if (doc) doc->setModified(false);
        }
        trigger("file_close_all", 1500);
        save(QStringLiteral("11 welcome"), QStringLiteral("Welcome page"), windowShot());
        QSet<QString> seenTips;
        sweepTooltips(m_win, QStringLiteral("09 tooltips/Welcome page"), seenTips);
    }

    QString m_root;
    QFile m_log;
    QFile m_manifest;
    QSet<QString> m_phases;
    QHash<QString, int> m_count;
    QPointer<KisMainWindow> m_win;
    QPointer<KisStudioLayout> m_studio;
    QPixmap m_mainShot;
    QElapsedTimer m_ready;
    QElapsedTimer m_splashSeen;
    bool m_splashDone {false};
    QString m_section;
    QString m_name;
    QString m_tipFolder;
    QSet<QString> m_tipSeen;
    QString m_formMap;
    QString m_errors;
    QString m_tipMap;
    QList<QAction *> m_menuOrder;
    QHash<QAction *, QString> m_menuPath;
    QSet<QString> m_finished;
    QSet<QString> m_crashed;
};
} // namespace

void KisDevTour::start()
{
    const QString root = qEnvironmentVariable("KRITA_DEV_TOUR");
    if (root.isEmpty()) return;
    new Tour(root);
}
