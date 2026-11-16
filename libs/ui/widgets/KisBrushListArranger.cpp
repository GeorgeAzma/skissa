/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisBrushListArranger.h"

#include <QBoxLayout>
#include <QContextMenuEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
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

#include "KisStudioTools.h"

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

KisBrushListArranger::KisBrushListArranger(KisResourceItemChooser *chooser, QBoxLayout *layout, QObject *parent)
    : QObject(parent)
    , m_chooser(chooser)
{
    m_tags = chooser->findChild<KisTagChooserWidget *>();
    // the user's tags are where brushes are found; "All" and "All untagged" only while there are none
    if (m_tags) m_tags->setHidesAllRows(true);

    QWidget *viewport = chooser->itemView()->viewport();
    viewport->setAcceptDrops(true);
    viewport->installEventFilter(this);
    chooser->itemView()->installEventFilter(this);

    // while adding: the tag's name and Done above the list
    m_header = new QWidget(chooser->parentWidget());
    auto *row = new QHBoxLayout(m_header);
    row->setContentsMargins(KisModernStyle::space2, KisModernStyle::space1, KisModernStyle::space1, KisModernStyle::space1);
    row->setSpacing(KisModernStyle::space2);
    m_headerName = new QLabel(m_header);
    QFont bold = m_headerName->font();
    bold.setWeight(QFont::DemiBold);
    m_headerName->setFont(bold);
    row->addWidget(m_headerName, 1);
    auto *import = new QToolButton(m_header);
    import->setText(i18nc("@action:button", "Import…"));
    import->setAutoRaise(true);
    import->setFocusPolicy(Qt::NoFocus);
    import->setToolTip(i18nc("@info:tooltip", "Brushes, bundles and Photoshop brushes"));
    connect(import, &QToolButton::clicked, this, &KisBrushListArranger::chooseFilesToImport);
    row->addWidget(import);
    auto *done = new QToolButton(m_header);
    done->setText(i18nc("@action:button stop adding brushes to the tag", "Done"));
    done->setAutoRaise(true);
    done->setFocusPolicy(Qt::NoFocus);
    done->setFont(bold);
    connect(done, &QToolButton::clicked, this, &KisBrushListArranger::stopAdding);
    row->addWidget(done);
    m_header->hide();
    layout->insertWidget(0, m_header);

    if (m_tags) {
        m_tags->setAddButton(i18nc("@info:tooltip", "Add brushes"), [this]() {
            if (isAdding()) stopAdding();
            else startAdding();
        });
        // another tag picked while adding: adding is over
        connect(m_tags, &KisTagChooserWidget::sigTagChosen, this, [this](const KisTagSP tag) {
            if (isAdding() && (!tag || tag->url() != m_adding->url())) {
                m_adding.clear();
                m_header->hide();
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
    const KisTagSP tag = userTag();
    if (!tag) return;
    m_adding = tag;
    m_members.clear();
    KisTagResourceModel inTag(ResourceType::PaintOpPresets);
    inTag.setTagsFilter({tag});
    for (int row = 0; row < inTag.rowCount(); row++) {
        m_members.insert(inTag.data(inTag.index(row, 0), Qt::UserRole + KisAbstractResourceModel::Id).toInt());
    }
    m_headerName->setText(i18nc("@title brushes are being added to the tag %1", "Add to %1", tag->name()));
    m_header->show();
    // every brush, the tag's among them
    m_chooser->tagFilterModel()->setTagFilter(KisTagSP());
    m_chooser->itemView()->viewport()->update();
}

void KisBrushListArranger::stopAdding()
{
    if (!isAdding()) return;
    const KisTagSP tag = m_adding;
    m_adding.clear();
    m_header->hide();
    m_chooser->tagFilterModel()->setTagFilter(tag);
    m_chooser->itemView()->viewport()->update();
    KisStudioTools::focusCanvas();
}

void KisBrushListArranger::toggle(const QModelIndex &index)
{
    const int id = index.data(Qt::UserRole + KisAbstractResourceModel::Id).toInt();
    if (!isAdding() || id < 0) return;
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
    if (watched == view && event->type() == QEvent::KeyPress && isAdding()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            stopAdding();
            return true;
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
        return false;
    }
    default:
        return false;
    }
}
