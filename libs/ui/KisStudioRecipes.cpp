/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioRecipes.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QSharedPointer>
#include <QTimer>

#include <KConfigGroup>
#include <KSharedConfig>
#include <kactioncollection.h>
#include <kxmlguiclient.h>
#include <kxmlguifactory.h>
#include <klocalizedstring.h>
#include <kundo2command.h>
#include <kundo2stack.h>

#include <KisModernStyle.h>
#include <KisSentenceCase.h>
#include <kis_image.h>

#include "KisDocument.h"
#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisSettingsRegistry.h"
#include "KisView.h"

namespace
{
const QString recipesKey = QStringLiteral("StudioRecipes");
const QString idPrefix = QStringLiteral("skissa_recipe_");
/// A step that comes this long after a command is still that command's (a filter on a large image takes a while)
constexpr qint64 stepWithinMs = 8000;

/// What never goes in a recipe: going through the history itself, and the recipes
bool isLeftOut(const QString &command)
{
    static const QStringList out = {QStringLiteral("edit_undo"), QStringLiteral("edit_redo")};
    return command.isEmpty() || out.contains(command) || command.startsWith(idPrefix);
}

/// A recipe's picture: its initials in a squircle, drawn in the ink of wherever it is shown
class InitialsIcon : public QIconEngine
{
public:
    explicit InitialsIcon(const QString &name)
        : m_initials(initials(name))
    {
    }

    static QString initials(const QString &name)
    {
        const QStringList words = name.split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts);
        if (words.isEmpty()) return QString();
        if (words.size() == 1) return words[0].left(2);
        return QString(words[0].at(0).toUpper()) + words[1].at(0).toLower();
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        QColor ink = qApp->palette().color(QPalette::Text);
        if (mode == QIcon::Disabled) ink.setAlphaF(0.4);
        const qreal side = qMin(rect.width(), rect.height());
        const QRectF box(rect.center().x() - side / 2 + side * 0.1, rect.center().y() - side / 2 + side * 0.1, side * 0.8, side * 0.8);
        painter->setPen(QPen(ink, qMax<qreal>(1.0, side / 16), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(KisModernStyle::squircle(box, box.width() * 0.28));
        QFont font = qApp->font();
        font.setWeight(QFont::DemiBold);
        font.setPixelSize(qMax(6, qRound(box.height() * (m_initials.size() > 1 ? 0.42 : 0.5))));
        painter->setFont(font);
        painter->setPen(ink);
        painter->drawText(box, Qt::AlignCenter, m_initials);
        painter->restore();
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }

    QIconEngine *clone() const override
    {
        auto *copy = new InitialsIcon(QString());
        copy->m_initials = m_initials;
        return copy;
    }

private:
    QString m_initials;
};
} // namespace

struct KisStudioRecipes::Private {
    /// The command each step was made by
    QHash<const KUndo2Command *, QPair<QString, QString>> madeBy; ///< command, and the step's text when it was made
    /// The command run last, waiting for the step it makes
    QString pending;
    qint64 pendingAt {0};
    QHash<const KUndo2Stack *, int> counts;
    QList<QPointer<KisMainWindow>> windows;
    /// A recipe is being run, and what runs it
    bool running {false};
    QSharedPointer<std::function<void()>> runningKeep;

    void noteCommand(const QString &command)
    {
        if (isLeftOut(command) || running) return;
        pending = command;
        pendingAt = QDateTime::currentMSecsSinceEpoch();
    }

    void noteStack(KUndo2Stack *stack)
    {
        const int count = stack->count();
        const int before = counts.value(stack, 0);
        counts[stack] = count;
        // a new step at the end: the command's, if one ran just now
        if (count > before && stack->index() == count && !pending.isEmpty()
            && QDateTime::currentMSecsSinceEpoch() - pendingAt <= stepWithinMs) {
            const KUndo2Command *step = stack->command(count - 1);
            if (step) madeBy.insert(step, {pending, step->text().toString()});
        }
        if (count > before) pending.clear();
    }

    void watchDocument(KisDocument *document)
    {
        if (!document || document->property("skissaRecipesWatched").toBool()) return;
        document->setProperty("skissaRecipesWatched", true);
        KUndo2Stack *stack = document->undoStack();
        if (!stack) return;
        counts[stack] = stack->count();
        QObject::connect(stack, &KUndo2Stack::indexChanged, KisStudioRecipes::instance(), [this, stack]() { noteStack(stack); });
        QObject::connect(stack, &QObject::destroyed, KisStudioRecipes::instance(), [this, stack]() { counts.remove(stack); });
    }

    void watchAction(QAction *action)
    {
        if (!action || action->property("skissaRecipesWatched").toBool()) return;
        action->setProperty("skissaRecipesWatched", true);
        // connected after the command's own slot, so this runs once its dialog, if any, is closed
        QPointer<QAction> watched = action;
        QObject::connect(action, &QAction::triggered, KisStudioRecipes::instance(), [this, watched]() {
            if (watched) noteCommand(watched->objectName());
        });
    }

    void giveActions(KisMainWindow *window)
    {
        KisKActionCollection *collection = window->actionCollection();
        const QVector<Recipe> all = KisStudioRecipes::recipes();
        QSet<QString> ids;
        for (const Recipe &recipe : all) {
            ids.insert(recipe.id);
            QAction *action = collection->action(recipe.id);
            if (!action) {
                action = new QAction(window);
                action->setObjectName(recipe.id);
                const QString id = recipe.id;
                QObject::connect(action, &QAction::triggered, window, [id]() { KisStudioRecipes::run(id); });
                collection->addAction(recipe.id, action);
            }
            action->setText(recipe.name);
            action->setToolTip(steps(recipe));
            action->setIcon(QIcon(new InitialsIcon(recipe.name)));
        }
        Q_FOREACH (QAction *action, collection->actions()) {
            if (action->objectName().startsWith(idPrefix) && !ids.contains(action->objectName())) {
                collection->removeAction(action);
            }
        }
    }

    /// The recipe's steps, one a line, for its tooltip
    static QString steps(const Recipe &recipe)
    {
        QStringList names;
        for (const QString &command : recipe.actions) names << KisStudioRecipes::commandName(command);
        return names.join(QLatin1Char('\n'));
    }
};

KisStudioRecipes::KisStudioRecipes()
    : d(new Private)
{
    connect(KisSettingsRegistry::instance(), &KisSettingsRegistry::valueChanged, this, [this](const QString &id) {
        if (id != recipesKey) return;
        for (const QPointer<KisMainWindow> &window : d->windows) {
            if (window) d->giveActions(window);
        }
        Q_EMIT changed();
    });
}

KisStudioRecipes *KisStudioRecipes::instance()
{
    static KisStudioRecipes *recipes = new KisStudioRecipes();
    return recipes;
}

void KisStudioRecipes::watch(KisMainWindow *window)
{
    Private *d = instance()->d;
    d->windows << window;
    Q_FOREACH (QAction *action, window->actionCollection()->actions()) d->watchAction(action);
    connect(window->actionCollection(), &KisKActionCollection::inserted, instance(), [d](QAction *action) { d->watchAction(action); });
    // the actions of the window's other parts (layers, filters, selections…) come with its views
    auto watchClients = [d, window]() {
        if (!window || !window->guiFactory()) return;
        Q_FOREACH (KisKXMLGUIClient *client, window->guiFactory()->clients()) {
            if (!client || !client->actionCollection()) continue;
            Q_FOREACH (QAction *action, client->actionCollection()->actions()) d->watchAction(action);
        }
    };
    QPointer<KisMainWindow> guarded = window;
    connect(KisPart::instance(), &KisPart::sigViewAdded, instance(), [watchClients, guarded]() {
        if (guarded) QTimer::singleShot(0, guarded.data(), watchClients);
    });
    QTimer::singleShot(0, window, watchClients);
    Q_FOREACH (QPointer<KisDocument> document, KisPart::instance()->documents()) d->watchDocument(document);
    connect(KisPart::instance(), &KisPart::sigDocumentAdded, instance(), [d](KisDocument *document) { d->watchDocument(document); });
    d->giveActions(window);
}

QString KisStudioRecipes::commandOf(const KUndo2Command *step)
{
    if (!step) return QString();
    const auto it = instance()->d->madeBy.constFind(step);
    // the same address may be a newer step since: the text tells
    if (it == instance()->d->madeBy.constEnd() || it->second != step->text().toString()) return QString();
    return it->first;
}

QString KisStudioRecipes::commandName(const QString &command)
{
    for (const QPointer<KisMainWindow> &window : instance()->d->windows) {
        if (!window || !window->guiFactory()) continue;
        Q_FOREACH (KisKXMLGUIClient *client, window->guiFactory()->clients()) {
            if (!client || !client->actionCollection()) continue;
            if (QAction *action = client->actionCollection()->action(command)) {
                return KisSentenceCase::of(action->text().remove(QLatin1Char('&')).remove(QStringLiteral("...")).remove(QStringLiteral("…")));
            }
        }
    }
    return command;
}

QVector<KisStudioRecipes::Recipe> KisStudioRecipes::recipes()
{
    QVector<Recipe> all;
    const QStringList lines = KSharedConfig::openConfig()->group(QString()).readEntry(recipesKey, QStringList());
    for (const QString &line : lines) {
        const QStringList parts = line.split(QLatin1Char('\t'));
        if (parts.size() < 3) continue;
        all.append({parts[0], parts[1], parts.mid(2)});
    }
    return all;
}

namespace
{
void store(const QVector<KisStudioRecipes::Recipe> &all)
{
    QStringList lines;
    for (const KisStudioRecipes::Recipe &recipe : all) {
        lines << (QStringList{recipe.id, recipe.name} + recipe.actions).join(QLatin1Char('\t'));
    }
    KisSettingsRegistry::writeList(recipesKey, lines);
}
} // namespace

QString KisStudioRecipes::add(const QStringList &commands)
{
    QStringList names;
    for (const QString &command : commands) {
        const QString name = commandName(command);
        if (!names.contains(name)) names << name;
    }
    QString name = names.mid(0, 2).join(QStringLiteral(", "));
    if (names.size() > 2) name += QStringLiteral("…");
    QVector<Recipe> all = recipes();
    const QString id = idPrefix + QString::number(QRandomGenerator::global()->generate(), 16);
    all.append({id, name, commands});
    store(all);
    return id;
}

void KisStudioRecipes::rename(const QString &id, const QString &name)
{
    QVector<Recipe> all = recipes();
    for (Recipe &recipe : all) {
        if (recipe.id == id && !name.trimmed().isEmpty()) recipe.name = name.trimmed().remove(QLatin1Char('\t'));
    }
    store(all);
}

void KisStudioRecipes::remove(const QString &id)
{
    QVector<Recipe> all = recipes();
    all.erase(std::remove_if(all.begin(), all.end(), [&id](const Recipe &recipe) { return recipe.id == id; }), all.end());
    store(all);
}

void KisStudioRecipes::run(const QString &id)
{
    Private *d = instance()->d;
    if (d->running) return;
    QStringList commands;
    for (const Recipe &recipe : recipes()) {
        if (recipe.id == id) commands = recipe.actions;
    }
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    if (commands.isEmpty() || !window || !window->activeView()) return;
    d->running = true;
    // one command after another, each once the image is done with the last
    auto left = QSharedPointer<QStringList>::create(commands);
    auto next = QSharedPointer<std::function<void()>>::create();
    QPointer<KisMainWindow> guarded = window;
    QWeakPointer<std::function<void()>> again = next;
    *next = [d, left, guarded, again]() {
        KisView *view = guarded ? guarded->activeView() : nullptr;
        KisImageSP image;
        if (view) image = view->image();
        if (!image || left->isEmpty()) {
            d->running = false;
            // let go once this has returned
            QTimer::singleShot(0, KisStudioRecipes::instance(), [d]() { d->runningKeep.reset(); });
            return;
        }
        if (!image->isIdle()) {
            if (const auto self = again.toStrongRef()) QTimer::singleShot(30, guarded.data(), *self);
            return;
        }
        const QString command = left->takeFirst();
        QAction *action = nullptr;
        Q_FOREACH (KisKXMLGUIClient *client, guarded->guiFactory()->clients()) {
            if (!action && client && client->actionCollection()) action = client->actionCollection()->action(command);
        }
        if (action && action->isEnabled()) action->trigger();
        if (const auto self = again.toStrongRef()) QTimer::singleShot(30, guarded.data(), *self);
    };
    // kept while it runs
    d->runningKeep = next;
    QTimer::singleShot(0, window, *next);
}
