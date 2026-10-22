/*
  SPDX-FileCopyrightText: 2006 Gábor Lehel <illissius@gmail.com>

  SPDX-License-Identifier: LGPL-2.0-or-later
*/
#include "NodeToolTip.h"
#include "kis_node_model.h"

#include <QImage>
#include <QModelIndex>
#include <QTextDocument>
#include <QUrl>
#include <klocalizedstring.h>

#include <kis_base_node.h>
#include <kis_layer_properties_icons.h>

NodeToolTip::NodeToolTip(QWidget *widget)
    : KoItemToolTip(widget)
{
}

NodeToolTip::~NodeToolTip()
{
}

QTextDocument *NodeToolTip::createDocument(const QModelIndex &index)
{
    // the layer's thumbnail, larger
    QTextDocument *doc = new QTextDocument(this);
    const int size = 250 * devicePixelRatioF();
    QImage thumb = index.data(int(KisNodeModel::BeginThumbnailRole) + size).value<QImage>();
    thumb.setDevicePixelRatio(devicePixelRatioF());
    doc->addResource(QTextDocument::ImageResource, QUrl("data:thumbnail"), thumb);
    doc->setHtml(QStringLiteral("<html><body><img src=\"data:thumbnail\"></body></html>"));
    doc->setDocumentMargin(6);
    doc->setUseDesignMetrics(true);
    doc->setTextWidth(thumb.width() / devicePixelRatioF() + 2 * doc->documentMargin());
    return doc;
}
