/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "flow-layout.hpp"

#include <QMetaObject>
#include <QWidget>

#include <algorithm>
#include <utility>
#include <vector>

namespace dm {

FlowLayout::FlowLayout(QWidget *parent, int spacing) : QLayout(parent), spacing_(spacing)
{
	setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
	clear();
}

void FlowLayout::clear()
{
	while (QLayoutItem *item = takeAt(0)) {
		if (QWidget *widget = item->widget())
			widget->deleteLater();
		delete item;
	}
}

void FlowLayout::addItem(QLayoutItem *item)
{
	items_.append(item);
}

int FlowLayout::count() const
{
	return int(items_.size());
}

QLayoutItem *FlowLayout::itemAt(int index) const
{
	return index >= 0 && index < items_.size() ? items_.at(index) : nullptr;
}

QLayoutItem *FlowLayout::takeAt(int index)
{
	return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const
{
	return {};
}

bool FlowLayout::hasHeightForWidth() const
{
	/* Height-for-width propagates poorly through grids and scroll areas; the wrapped height is
	 * reported as the minimum size for the current width instead (see minimumSize()). */
	return false;
}

int FlowLayout::heightForWidth(int width) const
{
	return doLayout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect &rect)
{
	QLayout::setGeometry(rect);
	const int height = doLayout(rect, false);
	if (height != lastHeight_) {
		lastHeight_ = height;
		/* The wrapped height changed with the width: ask the parents for a new layout pass. */
		if (QWidget *parent = parentWidget())
			QMetaObject::invokeMethod(
				parent, [parent]() { parent->updateGeometry(); }, Qt::QueuedConnection);
	}
}

QSize FlowLayout::sizeHint() const
{
	return minimumSize();
}

QSize FlowLayout::minimumSize() const
{
	QSize size;
	for (const QLayoutItem *item : items_)
		size = size.expandedTo(item->minimumSize());
	const QMargins margins = contentsMargins();
	size += QSize(margins.left() + margins.right(), margins.top() + margins.bottom());

	const QWidget *parent = parentWidget();
	const int width = geometry().width() > 0 ? geometry().width() : (parent ? parent->width() : 0);
	if (width > 0)
		size.setHeight(std::max(size.height(), doLayout(QRect(0, 0, width, 0), true)));
	return size;
}

int FlowLayout::doLayout(const QRect &rect, bool testOnly) const
{
	const QMargins margins = contentsMargins();
	const QRect area = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
	int x = area.x();
	int y = area.y();
	int lineHeight = 0;
	/* Items of the current line, vertically centred once the line's height is known. */
	std::vector<std::pair<QLayoutItem *, QPoint>> line;

	auto flush = [&]() {
		if (!testOnly) {
			for (const auto &[item, pos] : line) {
				const QSize hint = item->sizeHint();
				item->setGeometry(
					QRect(QPoint(pos.x(), pos.y() + (lineHeight - hint.height()) / 2), hint));
			}
		}
		line.clear();
	};

	for (QLayoutItem *item : items_) {
		if (item->widget() && item->widget()->isHidden())
			continue;
		const QSize hint = item->sizeHint();
		int nextX = x + hint.width() + spacing_;
		if (nextX - spacing_ > area.right() + 1 && lineHeight > 0) {
			flush();
			x = area.x();
			y += lineHeight + spacing_;
			nextX = x + hint.width() + spacing_;
			lineHeight = 0;
		}
		line.emplace_back(item, QPoint(x, y));
		x = nextX;
		lineHeight = std::max(lineHeight, hint.height());
	}
	flush();
	return y + lineHeight - rect.y() + margins.bottom();
}

} // namespace dm
