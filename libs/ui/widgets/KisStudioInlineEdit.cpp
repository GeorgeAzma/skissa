/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioInlineEdit.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>

namespace {

/// The editor itself: Escape drops the typing, and it goes once done either way
class InlineEditor : public QLineEdit
{
public:
    InlineEditor(QWidget *parent, std::function<void(const QString &)> done)
        : QLineEdit(parent)
        , m_done(done)
    {
        connect(this, &QLineEdit::editingFinished, this, [this]() { finish(true); });
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            finish(false);
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

private:
    void finish(bool keep)
    {
        // hiding drops the focus, which finishes the editing a second time
        if (m_finished) return;
        m_finished = true;
        const QString typed = text();
        // what is typed may rebuild the owner
        const QPointer<QWidget> owner = parentWidget();
        hide();
        deleteLater();
        if (keep && m_done) m_done(typed);
        if (owner) owner->update();
    }

    std::function<void(const QString &)> m_done;
    bool m_finished {false};
};

} // namespace

QLineEdit *KisStudioInlineEdit::open(QWidget *owner, const QRect &textRect, const QString &text, Qt::Alignment alignment,
                                     const QFont &font, std::function<void(const QString &typed)> done)
{
    auto *editor = new InlineEditor(owner, done);
    editor->setFrame(false);
    editor->setFont(font);
    editor->setAlignment(alignment | Qt::AlignVCenter);
    editor->setTextMargins(0, 0, 0, 0);
    editor->setContentsMargins(0, 0, 0, 0);
    // the owner's own ground shows through: the text simply becomes editable
    editor->setAttribute(Qt::WA_TranslucentBackground);
    editor->setAutoFillBackground(false);
    editor->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; padding: 0px; margin: 0px; }"));
    QPalette palette = editor->palette();
    palette.setColor(QPalette::Base, Qt::transparent);
    palette.setColor(QPalette::Text, owner->palette().color(QPalette::WindowText));
    editor->setPalette(palette);

    // as wide as the text with room for a few more characters, where the text was
    const QFontMetrics metrics(font);
    const int width = qMax(textRect.width(), metrics.horizontalAdvance(text) + metrics.horizontalAdvance(QStringLiteral("00")));
    QRect area(textRect.topLeft(), QSize(qMin(width, owner->width()), textRect.height()));
    if (alignment & Qt::AlignRight) area.moveRight(textRect.right());
    else if (alignment & Qt::AlignHCenter) area.moveCenter(textRect.center());
    area.moveLeft(qBound(0, area.left(), qMax(0, owner->width() - area.width())));
    editor->setGeometry(area);

    editor->setText(text);
    editor->selectAll();
    editor->show();
    editor->setFocus();
    return editor;
}
