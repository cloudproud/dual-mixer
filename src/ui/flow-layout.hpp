/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QLayout>
#include <QList>

namespace dm {

/* Wrapping layout for scene buttons, so panels stay usable at any width. */
class FlowLayout : public QLayout {
public:
	explicit FlowLayout(QWidget *parent = nullptr, int spacing = 4);
	~FlowLayout() override;

	void addItem(QLayoutItem *item) override;
	int count() const override;
	QLayoutItem *itemAt(int index) const override;
	QLayoutItem *takeAt(int index) override;
	Qt::Orientations expandingDirections() const override;
	bool hasHeightForWidth() const override;
	int heightForWidth(int width) const override;
	QSize minimumSize() const override;
	QSize sizeHint() const override;
	void setGeometry(const QRect &rect) override;

	/* Deletes all items and their widgets. */
	void clear();

private:
	int doLayout(const QRect &rect, bool testOnly) const;

	QList<QLayoutItem *> items_;
	int spacing_;
	mutable int lastHeight_ = -1;
};

} // namespace dm
