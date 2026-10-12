/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "scene-list.hpp"

#include "common.hpp"
#include "mix.hpp"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyledItemDelegate>

#include <cmath>

namespace dm {

namespace {

constexpr int kUuidRole = Qt::UserRole;
constexpr int kProgramRole = Qt::UserRole + 1;
/* Same grid metrics as OBS's Scenes list. */
constexpr int kGridItemWidth = 150;
constexpr int kGridItemHeight = 24;

/* OBS's multiview Program colour. */
const QColor kProgramColor(0xd0, 0x00, 0x00);

/* Marks the Program scene with a 1px outline, leaving the theme's selection (= Preview) intact. */
class TallyDelegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		QStyledItemDelegate::paint(painter, option, index);
		if (!index.data(kProgramRole).toBool())
			return;
		painter->save();
		painter->setPen(QPen(kProgramColor, 1));
		painter->setBrush(Qt::NoBrush);
		painter->drawRect(QRectF(option.rect).adjusted(0.5, 0.5, -0.5, -0.5));
		painter->restore();
	}
};

} // namespace

SceneList::SceneList(Mix *mix, QWidget *parent) : QListWidget(parent), mix_(mix)
{
	setItemDelegate(new TallyDelegate(this));
	setSelectionMode(QAbstractItemView::SingleSelection);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setUniformItemSizes(true);

	connect(this, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
		if (updating_ || !item)
			return;
		OBSSource scene = sceneByUuid(utf8(item->data(kUuidRole).toString()).c_str());
		if (scene)
			mix_->setPreviewScene(scene);
	});
	connect(this, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
		auto *canvasMix = qobject_cast<CanvasMix *>(mix_);
		if (canvasMix && !canvasMix->doubleClickTransitions())
			return;
		OBSSource scene = sceneByUuid(utf8(item->data(kUuidRole).toString()).c_str());
		if (!scene)
			return;
		mix_->setPreviewScene(scene);
		mix_->transition();
	});
}

void SceneList::refresh()
{
	updating_ = true;
	clear();
	for (const OBSSource &scene : mix_->scenes()) {
		auto *item = new QListWidgetItem(qstr(obs_source_get_name(scene)), this);
		item->setData(kUuidRole, qstr(obs_source_get_uuid(scene)));
	}
	updating_ = false;
	applyGridMode();
	refreshTally();
}

void SceneList::refreshTally()
{
	const OBSSource preview = mix_->previewScene();
	const OBSSource program = mix_->programScene();
	const QString previewUuid = preview ? qstr(obs_source_get_uuid(preview)) : QString();
	const QString programUuid = program ? qstr(obs_source_get_uuid(program)) : QString();

	updating_ = true;
	QListWidgetItem *current = nullptr;
	for (int i = 0; i < count(); i++) {
		QListWidgetItem *it = item(i);
		const QString uuid = it->data(kUuidRole).toString();
		it->setData(kProgramRole, uuid == programUuid);
		if (uuid == previewUuid)
			current = it;
	}
	setCurrentItem(current);
	if (!current)
		clearSelection();
	updating_ = false;
	viewport()->update();
}

void SceneList::applyGridMode()
{
	config_t *config = obs_frontend_get_user_config();
	const bool grid = config && config_get_bool(config, "BasicWindow", "gridMode");
	if (grid != gridMode_) {
		gridMode_ = grid;
		if (parentWidget())
			parentWidget()->setProperty("class", grid ? "list-grid" : "");
		if (grid) {
			setResizeMode(QListView::Adjust);
			setViewMode(QListView::IconMode);
			setMovement(QListView::Static);
			setStyleSheet(QStringLiteral("*{padding: 0; margin: 0;}"));
		} else {
			setViewMode(QListView::ListMode);
			setResizeMode(QListView::Fixed);
			setStyleSheet(QString());
		}
	}
	QResizeEvent event(size(), size());
	resizeEvent(&event);
}

void SceneList::resizeEvent(QResizeEvent *event)
{
	/* Same sizing rules as OBS's SceneTree. */
	if (gridMode_ && count() > 0) {
		const int scroll = verticalScrollBar()->isVisible() ? verticalScrollBar()->sizeHint().width() : 0;
		const int width = std::max(1, contentsRect().width() - scroll - 1);
		const int columns = std::max(1, int(std::ceil(float(width) / kGridItemWidth)));
		const QSize size(width / columns, kGridItemHeight);
		setGridSize(size);
		for (int i = 0; i < count(); i++)
			item(i)->setSizeHint(size);
	} else if (!gridMode_) {
		setGridSize(QSize());
		for (int i = 0; i < count(); i++)
			item(i)->setData(Qt::SizeHintRole, QVariant());
	}
	QListWidget::resizeEvent(event);
}

} // namespace dm
