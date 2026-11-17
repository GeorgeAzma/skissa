/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisBrushListArranger.h"

#include <QContextMenuEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QToolButton>
#include <QUrl>

#include <klocalizedstring.h>

#include <KisResourceModel.h>
#include <KisModernStyle.h>
#include <KisResourceItemChooser.h>
#include <KisResourceItemListView.h>
#include <KisResourceOrder.h>
#include <KisResourceTypes.h>
#include <KisResourceUserOperations.h>
#include <KisStorageModel.h>
#include <KisTagChooserWidget.h>
#include <KisTagFilterResourceProxyModel.h>
#include <KisTagModel.h>
#include <KisTagResourceModel.h>
#include <KoFileDialog.h>

#include "KisStudioBrushPicker.h"
#include "KisStudioItemCard.h"
#include "KisStudioTools.h"
#include "widgets/KisStudioChips.h"

namespace
{

bool isImportable(const QString &file)
{
    const QString suffix = QFileInfo(file).suffix().toLower();
    return suffix == QLatin1String("kpp") || suffix == QLatin1String("bundle") || suffix == QLatin1String("abr");
}

QStringList importableFiles(const QMimeData *mime)
{
    QStringList files;
    if (!mime || !mime->hasUrls()) return files;
    Q_FOREACH (const QUrl &url, mime->urls()) {
        if (url.isLocalFile() && isImportable(url.toLocalFile())) files << url.toLocalFile();
    }
    return files;
}

} // namespace

KisBrushListArranger::KisBrushListArranger(KisResourceItemChooser *chooser, QObject *parent)
    : QObject(parent)
    , m_chooser(chooser)
{
    m_tags = chooser->findChild<KisTagChooserWidget *>();
    // the user's tags are where brushes are found; "All" and "All untagged" only while there are none
    if (m_tags) m_tags->setHidesAllRows(true);
    // All is the user's set: the brushes they put away are found by search, and picked again while adding
    chooser->tagFilterModel()->setHidesPutAway(true);

    QWidget *viewport = chooser->itemView()->viewport();
    viewport->setAcceptDrops(true);
    viewport->installEventFilter(this);
    chooser->itemView()->installEventFilter(this);

    // while adding: Import beside the +, which ends adding
    m_import = new QToolButton(m_tags ? static_cast<QWidget *>(m_tags) : chooser);
    m_import->setText(i18nc("@action:button", "Import…"));
    m_import->setAutoRaise(true);
    m_import->setFocusPolicy(Qt::NoFocus);
    m_import->setToolTip(i18nc("@info:tooltip", "Brushes, bundles and Photoshop brushes"));
    connect(m_import, &QToolButton::clicked, this, &KisBrushListArranger::chooseFilesToImport);
    m_import->hide();

    // while adding: where the brushes come from, over the list
    m_sources = new KisStudioChips(chooser->itemView());
    m_sources->hide();
    connect(m_sources, &KisStudioChips::clicked, this, &KisBrushListArranger::pickSource);

    if (m_tags) {
        m_tags->setAddButton(i18nc("@info:tooltip", "Add brushes"), [this]() {
            if (isAdding()) stopAdding();
            else startAdding();
        });
        // another tag picked while adding: adding is over
        connect(m_tags, &KisTagChooserWidget::sigTagChosen, this, [this](const KisTagSP tag) {
            if (isAdding() && (!tag || tag->url() != m_adding->url())) {
                m_adding.clear();
                showSources(false);
                m_chooser->tagFilterModel()->setHidesPutAway(true);
                showAdding(false);
                m_chooser->itemView()->viewport()->update();
            }
        });
    }
}

KisTagSP KisBrushListArranger::orderedTag() const
{
    const KisTagSP tag = m_tags ? m_tags->currentlySelectedTag() : KisTagSP();
    return tag && (tag->id() >= 0 || tag->url() == KisAllTagsModel::urlAll()) ? tag : KisTagSP();
}

KisTagSP KisBrushListArranger::userTag() const
{
    const KisTagSP tag = m_tags ? m_tags->currentlySelectedTag() : KisTagSP();
    return tag && tag->id() >= 0 ? tag : KisTagSP();
}

int KisBrushListArranger::markOf(const QModelIndex &index) const
{
    if (!isAdding()) return -1;
    return m_members.contains(index.data(Qt::UserRole + KisAbstractResourceModel::Id).toInt()) ? 1 : 0;
}

void KisBrushListArranger::startAdding()
{
    const KisTagSP tag = orderedTag();
    if (!tag) return;
    m_adding = tag;
    m_members.clear();
    if (tag->id() >= 0) {
        KisTagResourceModel inTag(ResourceType::PaintOpPresets);
        inTag.setTagsFilter({tag});
        for (int row = 0; row < inTag.rowCount(); row++) {
            m_members.insert(inTag.data(inTag.index(row, 0), Qt::UserRole + KisAbstractResourceModel::Id).toInt());
        }
    } else {
        // All: every brush not put away
        const QStringList away = KisResourceOrder::putAway(ResourceType::PaintOpPresets);
        KisResourceModel all(ResourceType::PaintOpPresets);
        for (int row = 0; row < all.rowCount(); row++) {
            const QModelIndex index = all.index(row, 0);
            if (!away.contains(index.data(Qt::UserRole + KisAbstractResourceModel::Filename).toString())) {
                m_members.insert(index.data(Qt::UserRole + KisAbstractResourceModel::Id).toInt());
            }
        }
    }
    showAdding(true);
    m_chooser->tagFilterModel()->setHidesPutAway(false);
    showSources(true);
    m_chooser->itemView()->viewport()->update();
    // typing looks for a brush at once
    if (QWidget *search = searchField()) {
        if (QLineEdit *field = search->findChild<QLineEdit *>()) {
            if (!field->isVisible()) {
                if (auto *open = search->findChild<QToolButton *>()) open->click();
            }
            field->activateWindow();
            field->setFocus(Qt::OtherFocusReason);
            // a search looks among every brush
            if (!field->property("skissaSearchesAll").toBool()) {
                field->setProperty("skissaSearchesAll", true);
                connect(field, &QLineEdit::textChanged, this, [this](const QString &text) {
                    if (isAdding() && !text.isEmpty() && !m_sourceTags.isEmpty()) pickSource(m_sourceTags.size() - 1);
                });
            }
        }
    }
}

QWidget *KisBrushListArranger::searchField() const
{
    Q_FOREACH (QWidget *child, m_chooser->findChildren<QWidget *>()) {
        if (child->inherits("KisTagFilterWidget")) return child;
    }
    return nullptr;
}

namespace
{
/// The brush named by a dragged item ("brush:<name>"), or empty
QString draggedBrush(const QMimeData *mime)
{
    const QString key = QString::fromUtf8(mime->data(KisStudioShortcuts::itemMimeType()));
    const QString prefix = QStringLiteral("brush:");
    return key.startsWith(prefix) ? key.mid(prefix.size()) : QString();
}
} // namespace

bool KisBrushListArranger::dropBrush(const QString &name, const QPoint &pos)
{
    const KisTagSP tag = orderedTag();
    if (!tag || isAdding()) return false;
    KisResourceModel all(ResourceType::PaintOpPresets);
    int id = -1;
    QString file;
    for (int row = 0; row < all.rowCount() && id < 0; row++) {
        const QModelIndex index = all.index(row, 0);
        if (index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString() != name) continue;
        id = index.data(Qt::UserRole + KisAbstractResourceModel::Id).toInt();
        file = index.data(Qt::UserRole + KisAbstractResourceModel::Filename).toString();
    }
    if (id < 0) return false;
    if (tag->id() >= 0) {
        KisTagResourceModel inTag(ResourceType::PaintOpPresets);
        inTag.setTagsFilter({tag});
        bool member = false;
        for (int row = 0; row < inTag.rowCount() && !member; row++) {
            member = inTag.data(inTag.index(row, 0), Qt::UserRole + KisAbstractResourceModel::Id).toInt() == id;
        }
        if (!member) KisTagResourceModel(ResourceType::PaintOpPresets).tagResources(tag, {id});
    } else {
        // All: back from being put away
        QStringList away = KisResourceOrder::putAway(ResourceType::PaintOpPresets);
        if (away.removeAll(file)) KisResourceOrder::setPutAway(ResourceType::PaintOpPresets, away);
    }
    moveBrush(file, pos);
    return true;
}

namespace
{
/// Lets the list keep room over its items for the chips
struct ScrollAreaMargins : public QAbstractScrollArea {
    using QAbstractScrollArea::setViewportMargins;
};
} // namespace

void KisBrushListArranger::showSources(bool shown)
{
    KisResourceItemListView *view = m_chooser->itemView();
    if (!shown) {
        m_sources->hide();
        static_cast<ScrollAreaMargins *>(static_cast<QAbstractScrollArea *>(view))->setViewportMargins(0, 0, 0, 0);
        m_chooser->tagFilterModel()->setOnly(QStringList(), false);
        return;
    }
    QVector<KisStudioChips::Chip> chips;
    m_sourceTags.clear();
    m_hasRecent = !KisStudioBrushPicker::recentBrushes().isEmpty();
    if (m_hasRecent) {
        chips.append({i18nc("brushes used lately", "Recent"), QString()});
        m_sourceTags.append(KisTagSP());
    }
    // the user's other tags, in their order
    KisTagModel tags(ResourceType::PaintOpPresets);
    QVector<KisTagSP> theirs;
    for (int row = 0; row < tags.rowCount(); row++) {
        const KisTagSP tag = tags.tagForIndex(tags.index(row, 0));
        if (tag && tag->id() >= 0 && tag->active() && tag->url() != m_adding->url()) theirs.append(tag);
    }
    const QStringList order = KisResourceOrder::tags(ResourceType::PaintOpPresets);
    std::stable_sort(theirs.begin(), theirs.end(), [&order](const KisTagSP &a, const KisTagSP &b) {
        const int left = order.indexOf(a->url());
        const int right = order.indexOf(b->url());
        return (left < 0 ? INT_MAX : left) < (right < 0 ? INT_MAX : right);
    });
    for (const KisTagSP &tag : theirs) {
        chips.append({tag->name(), QString()});
        m_sourceTags.append(tag);
    }
    chips.append({KisResourceOrder::allName(), QString()});
    m_sourceTags.append(KisTagSP());
    m_sources->setChips(chips);
    m_sources->show();
    placeSources();
    pickSource(0);
}

void KisBrushListArranger::placeSources()
{
    if (!m_sources->isVisible()) return;
    KisResourceItemListView *view = m_chooser->itemView();
    const int side = KisModernStyle::space1;
    const int width = view->width() - 2 * side;
    const int height = m_sources->heightForWidth(width);
    m_sources->setGeometry(side, side, width, height);
    static_cast<ScrollAreaMargins *>(static_cast<QAbstractScrollArea *>(view))->setViewportMargins(0, height + 2 * side, 0, 0);
}

void KisBrushListArranger::pickSource(int index)
{
    if (index < 0 || index >= m_sourceTags.size()) return;
    m_sources->setCurrent(index);
    KisTagFilterResourceProxyModel *model = m_chooser->tagFilterModel();
    const bool recent = m_hasRecent && index == 0;
    model->setTagFilter(m_sourceTags[index]);
    model->setOnly(recent ? KisStudioBrushPicker::recentBrushes() : QStringList(), recent);
}

void KisBrushListArranger::stopAdding()
{
    if (!isAdding()) return;
    const KisTagSP tag = m_adding;
    m_adding.clear();
    showAdding(false);
    showSources(false);
    // what was typed to find brushes to add
    if (QWidget *search = searchField()) QMetaObject::invokeMethod(search, "clear");
    m_chooser->tagFilterModel()->setHidesPutAway(true);
    m_chooser->tagFilterModel()->setTagFilter(tag);
    m_chooser->itemView()->viewport()->update();
    KisStudioTools::focusCanvas();
}

void KisBrushListArranger::showAdding(bool adding)
{
    if (m_tags) {
        m_tags->setAdding(adding, adding ? i18nc("@info:tooltip stop adding brushes", "Done") : i18nc("@info:tooltip", "Add brushes"), m_import);
    }
}

void KisBrushListArranger::toggle(const QModelIndex &index)
{
    const int id = index.data(Qt::UserRole + KisAbstractResourceModel::Id).toInt();
    if (!isAdding() || id < 0) return;
    if (m_adding->id() < 0) {
        // All: put away, or back
        const QString file = index.data(Qt::UserRole + KisAbstractResourceModel::Filename).toString();
        QStringList away = KisResourceOrder::putAway(ResourceType::PaintOpPresets);
        if (m_members.contains(id)) {
            if (!away.contains(file)) away << file;
            m_members.remove(id);
        } else {
            away.removeAll(file);
            m_members.insert(id);
        }
        KisResourceOrder::setPutAway(ResourceType::PaintOpPresets, away);
        m_chooser->itemView()->viewport()->update();
        return;
    }
    KisTagResourceModel model(ResourceType::PaintOpPresets);
    if (m_members.contains(id)) {
        if (model.untagResources(m_adding, {id})) m_members.remove(id);
    } else {
        if (model.tagResources(m_adding, {id})) m_members.insert(id);
    }
    m_chooser->itemView()->viewport()->update();
}

void KisBrushListArranger::importFiles(const QStringList &files)
{
    const KisTagSP tag = userTag();
    Q_FOREACH (const QString &file, files) {
        const QString suffix = QFileInfo(file).suffix().toLower();
        if (suffix == QLatin1String("kpp")) {
            // a brush: into the tag shown
            KoResourceSP resource = KisResourceUserOperations::importResourceFileWithUserInput(m_chooser, QString(), ResourceType::PaintOpPresets, file);
            if (resource && tag) KisTagResourceModel(ResourceType::PaintOpPresets).tagResources(tag, {resource->resourceId()});
            if (resource) m_chooser->setCurrentResource(resource);
        } else {
            // a bundle or a Photoshop brush set: its brushes come with their own tags
            KisStorageModel::instance()->importStorage(file, KisStorageModel::None);
        }
    }
}

void KisBrushListArranger::chooseFilesToImport()
{
    KoFileDialog dialog(m_chooser, KoFileDialog::OpenFiles, QStringLiteral("SkissaImportBrushes"));
    dialog.setCaption(i18nc("@title:window", "Import brushes"));
    dialog.setMimeTypeFilters({QStringLiteral("application/x-krita-paintoppreset"), QStringLiteral("application/x-krita-bundle"),
                               QStringLiteral("image/x-adobe-brushlibrary")});
    const QStringList files = dialog.filenames();
    if (!files.isEmpty()) importFiles(files);
}

void KisBrushListArranger::showMenu(const QPoint &globalPos)
{
    QMenu menu(m_chooser);
    if (isAdding()) {
        menu.addAction(i18nc("@action stop adding brushes to the tag", "Done"), this, &KisBrushListArranger::stopAdding);
    } else if (const KisTagSP tag = userTag()) {
        menu.addAction(i18nc("@action %1 is a tag", "Add brushes to %1", tag->name()), this, &KisBrushListArranger::startAdding);
    } else if (orderedTag()) {
        menu.addAction(i18nc("@action", "Choose brushes"), this, &KisBrushListArranger::startAdding);
    }
    menu.addAction(i18nc("@action", "Import brushes…"), this, &KisBrushListArranger::chooseFilesToImport);
    menu.exec(globalPos);
}

int KisBrushListArranger::dropRowAt(const QPoint &pos) const
{
    KisResourceItemListView *view = m_chooser->itemView();
    QAbstractItemModel *model = view->model();
    const int count = model ? model->rowCount() : 0;
    if (!count) return 0;
    const bool across = view->flow() == QListView::LeftToRight;
    const QModelIndex at = view->indexAt(pos);
    if (at.isValid()) {
        const QRect cell = view->visualRect(at);
        const bool after = across ? pos.x() > cell.center().x() : pos.y() > cell.center().y();
        return at.row() + (after ? 1 : 0);
    }
    // past the last one
    const QRect last = view->visualRect(model->index(count - 1, 0));
    return pos.y() > last.bottom() || (pos.y() >= last.top() && pos.x() > last.right()) ? count : 0;
}

void KisBrushListArranger::showDropLine(const QPoint &pos)
{
    KisResourceItemListView *view = m_chooser->itemView();
    QAbstractItemModel *model = view->model();
    const int count = model ? model->rowCount() : 0;
    if (!count) return;
    if (!m_dropLine) {
        m_dropLine = new QWidget(view->viewport());
        m_dropLine->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_dropLine->setAutoFillBackground(true);
    }
    QPalette palette = m_dropLine->palette();
    palette.setColor(QPalette::Window, view->palette().color(QPalette::Text));
    m_dropLine->setPalette(palette);
    const int row = dropRowAt(pos);
    const bool across = view->flow() == QListView::LeftToRight;
    const QRect cell = view->visualRect(model->index(qMin(row, count - 1), 0));
    if (across) {
        const int x = row < count ? cell.left() : cell.right() + 1;
        m_dropLine->setGeometry(x - 1, cell.top() + 2, 2, cell.height() - 4);
    } else {
        const int y = row < count ? cell.top() : cell.bottom() + 1;
        m_dropLine->setGeometry(cell.left() + 4, y - 1, cell.width() - 8, 2);
    }
    m_dropLine->show();
    m_dropLine->raise();
}

void KisBrushListArranger::hideDropLine()
{
    if (m_dropLine) m_dropLine->hide();
}

void KisBrushListArranger::moveBrush(const QString &moved, const QPoint &pos)
{
    const KisTagSP tag = orderedTag();
    QAbstractItemModel *model = m_chooser->itemView()->model();
    if (!tag || !model || moved.isEmpty()) return;
    // the tag's brushes as shown
    QStringList shown;
    for (int row = 0; row < model->rowCount(); row++) {
        shown << model->index(row, 0).data(Qt::UserRole + KisAbstractResourceModel::Filename).toString();
    }
    const int to = dropRowAt(pos);
    const QString before = to < shown.size() ? shown[to] : QString();
    if (before == moved) return;
    // the order kept so far, then what shows that it doesn't hold: brushes a search hides keep their places
    QStringList order = KisResourceOrder::inTag(ResourceType::PaintOpPresets, tag->url());
    Q_FOREACH (const QString &file, shown) {
        if (!order.contains(file)) order << file;
    }
    order.removeAll(moved);
    if (!before.isEmpty()) {
        order.insert(qMax(0, order.indexOf(before)), moved);
    } else {
        // after the last one shown
        int after = -1;
        Q_FOREACH (const QString &file, shown) {
            if (file != moved) after = qMax(after, order.indexOf(file));
        }
        order.insert(after + 1, moved);
    }
    KisResourceOrder::setInTag(ResourceType::PaintOpPresets, tag->url(), order);
}

bool KisBrushListArranger::eventFilter(QObject *watched, QEvent *event)
{
    KisResourceItemListView *view = m_chooser->itemView();
    if (watched == view && event->type() == QEvent::Resize) {
        placeSources();
        return false;
    }
    if (watched == view && event->type() == QEvent::KeyPress && isAdding()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            stopAdding();
            return true;
        }
        // typing looks for a brush
        const QString text = key->text();
        if (!text.isEmpty() && text.at(0).isPrint() && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
            // the search's magnifier opens its field
            QWidget *search = searchField();
            QLineEdit *field = search ? search->findChild<QLineEdit *>() : nullptr;
            if (field) {
                if (!field->isVisible()) {
                    if (auto *open = search->findChild<QToolButton *>()) open->click();
                }
                field->setFocus(Qt::OtherFocusReason);
                field->insert(text);
                return true;
            }
        }
        return false;
    }
    if (watched != view->viewport()) return false;

    switch (event->type()) {
    case QEvent::ContextMenu: {
        auto *menu = static_cast<QContextMenuEvent *>(event);
        // on a brush, its tags; elsewhere, adding and importing
        if (!isAdding() && view->indexAt(menu->pos()).isValid()) return false;
        showMenu(menu->globalPos());
        return true;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        m_pressedRow = mouse->button() == Qt::LeftButton ? view->indexAt(mouse->pos()).row() : -1;
        m_pressPos = mouse->pos();
        m_reordering = false;
        // adding: a click puts the brush in the tag or takes it out; it isn't picked
        return isAdding() && mouse->button() == Qt::LeftButton;
    }
    case QEvent::MouseMove: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (!(mouse->buttons() & Qt::LeftButton)) return false;
        if (isAdding()) return true;
        // a brush of the user's tag dragged within the list goes elsewhere in it; out of the list, it is carried off
        // (to the tools, to pin it) as the list's own drag
        if (m_pressedRow < 0 || !orderedTag()) return false;
        const bool inside = view->viewport()->rect().contains(mouse->pos());
        if (!m_reordering && inside && QLineF(mouse->pos(), m_pressPos).length() > KisModernStyle::dragDistance(mouse)) {
            m_reordering = true;
        }
        if (m_reordering && !inside) {
            m_reordering = false;
            hideDropLine();
            return false;
        }
        if (m_reordering) {
            showDropLine(mouse->pos());
            return true;
        }
        return false;
    }
    case QEvent::MouseButtonRelease: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        const int pressed = m_pressedRow;
        m_pressedRow = -1;
        if (m_reordering) {
            m_reordering = false;
            hideDropLine();
            QAbstractItemModel *model = view->model();
            if (model && pressed >= 0 && pressed < model->rowCount()) {
                moveBrush(model->index(pressed, 0).data(Qt::UserRole + KisAbstractResourceModel::Filename).toString(), mouse->pos());
            }
            return true;
        }
        if (!isAdding()) return false;
        const QModelIndex index = view->indexAt(mouse->pos());
        if (mouse->button() == Qt::LeftButton && index.isValid() && index.row() == pressed) toggle(index);
        return mouse->button() == Qt::LeftButton;
    }
    case QEvent::DragEnter:
    case QEvent::DragMove: {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        if (!importableFiles(drag->mimeData()).isEmpty()) {
            drag->acceptProposedAction();
            return true;
        }
        // a brush from elsewhere goes into the tag shown, where the line is
        if (!draggedBrush(drag->mimeData()).isEmpty() && orderedTag() && !isAdding()) {
            drag->acceptProposedAction();
            showDropLine(drag->pos());
            return true;
        }
        return false;
    }
    case QEvent::DragLeave:
        hideDropLine();
        return false;
    case QEvent::Drop: {
        auto *drop = static_cast<QDropEvent *>(event);
        hideDropLine();
        const QStringList files = importableFiles(drop->mimeData());
        if (!files.isEmpty()) {
            drop->acceptProposedAction();
            importFiles(files);
            return true;
        }
        const QString brush = draggedBrush(drop->mimeData());
        if (!brush.isEmpty() && dropBrush(brush, drop->pos())) {
            drop->acceptProposedAction();
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}
