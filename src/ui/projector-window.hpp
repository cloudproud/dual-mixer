/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QMetaObject>
#include <QWidget>

class QScreen;

namespace dm {

class Mix;
class DisplayWidget;

/* Output window showing one mix's Program, fullscreen on a display or windowed. */
class ProjectorWindow : public QWidget {
	Q_OBJECT

public:
	explicit ProjectorWindow(Mix *mix);
	~ProjectorWindow() override;

	void setMix(Mix *mix);
	void showFullScreenOn(QScreen *screen);
	void showWindowed(const QByteArray &geometry);

signals:
	void closed();
	/* The user picked another placement from the context menu; empty name = windowed. */
	void screenRequested(const QString &screenName);

protected:
	void closeEvent(QCloseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
	void updateTitle();

	Mix *mix_ = nullptr;
	DisplayWidget *display_ = nullptr;
	QMetaObject::Connection renameConnection_;
};

} // namespace dm
