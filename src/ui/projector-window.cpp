/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "projector-window.hpp"

#include "common.hpp"
#include "display-widget.hpp"
#include "mix.hpp"

#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QScreen>
#include <QVBoxLayout>
#include <QWindow>

namespace dm {

ProjectorWindow::ProjectorWindow(Mix *mix) : QWidget(nullptr, Qt::Window)
{
	setAttribute(Qt::WA_DeleteOnClose);
	setMinimumSize(160, 90);
	setStyleSheet(QStringLiteral("background-color: black;"));

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	display_ = new DisplayWidget(this);
	layout->addWidget(display_);

	setMix(mix);
}

ProjectorWindow::~ProjectorWindow()
{
	display_->destroyDisplay();
}

void ProjectorWindow::setMix(Mix *mix)
{
	disconnect(renameConnection_);
	mix_ = mix;
	display_->setRenderer(mix ? DisplayWidget::programRenderer(mix) : DisplayWidget::Renderer());
	if (mix)
		renameConnection_ = connect(mix, &Mix::renamed, this, &ProjectorWindow::updateTitle);
	updateTitle();
}

void ProjectorWindow::updateTitle()
{
	setWindowTitle(T("Projector.Title").arg(mix_ ? mix_->name() : QString()));
}

void ProjectorWindow::showFullScreenOn(QScreen *screen)
{
	if (isFullScreen())
		showNormal();
	setGeometry(screen->geometry());
	if (windowHandle())
		windowHandle()->setScreen(screen);
	showFullScreen();
	raise();
}

void ProjectorWindow::showWindowed(const QByteArray &geometry)
{
	if (isFullScreen())
		showNormal();
	if (geometry.isEmpty() || !restoreGeometry(geometry))
		resize(640, 360);
	show();
	raise();
}

void ProjectorWindow::closeEvent(QCloseEvent *event)
{
	emit closed();
	QWidget::closeEvent(event);
}

void ProjectorWindow::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Escape) {
		close();
		return;
	}
	QWidget::keyPressEvent(event);
}

void ProjectorWindow::mouseDoubleClickEvent(QMouseEvent *)
{
	if (isFullScreen()) {
		emit screenRequested(QString());
		return;
	}
	if (QScreen *current = screen())
		emit screenRequested(current->name());
}

void ProjectorWindow::contextMenuEvent(QContextMenuEvent *event)
{
	QMenu menu(this);
	QMenu *fullscreen = menu.addMenu(T("Projector.Fullscreen"));
	const QList<QScreen *> screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); i++) {
		QScreen *screen = screens[i];
		const QRect g = screen->geometry();
		const QString label =
			QStringLiteral("%1: %2 (%3x%4)").arg(i + 1).arg(screen->name()).arg(g.width()).arg(g.height());
		const QString name = screen->name();
		fullscreen->addAction(label, this, [this, name]() { emit screenRequested(name); });
	}
	menu.addAction(T("Projector.Windowed"), this, [this]() { emit screenRequested(QString()); });
	menu.addSeparator();
	menu.addAction(T("Projector.Close"), this, &QWidget::close);
	menu.exec(event->globalPos());
}

} // namespace dm
