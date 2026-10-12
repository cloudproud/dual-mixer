/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "ui-helpers.hpp"

#include "destination-dialog.hpp"
#include "manager.hpp"
#include "mix.hpp"

#include <obs-frontend-api.h>

#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QMessageBox>
#include <QPointer>
#include <QToolButton>

namespace dm {

namespace {

/* A drawn status dot (not a text glyph), at the display's pixel density. */
QIcon statusIcon(const QColor &color, qreal dpr)
{
	const int size = 10;
	QPixmap pixmap(QSize(size, size) * dpr);
	pixmap.setDevicePixelRatio(dpr);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(color);
	painter.drawEllipse(QRectF(1, 1, size - 2, size - 2));
	return QIcon(pixmap);
}

/* Theme-styled status button; the OBS theme supplies border, background and states. */
QToolButton *chipButton(QWidget *parent)
{
	auto *chip = new QToolButton(parent);
	chip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	chip->setIconSize(QSize(10, 10));
	chip->setPopupMode(QToolButton::InstantPopup);
	chip->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0px; }"));
	return chip;
}

} // namespace

std::vector<NativeOutput> nativeOutputs()
{
	const Manager::NativeOutputs state = Manager::instance()->nativeOutputs();
	std::vector<NativeOutput> result;

	NativeOutput stream;
	stream.label = T("Native.Stream");
	stream.active = state.streaming;
	stream.start = []() {
		obs_frontend_streaming_start();
	};
	stream.stop = []() {
		obs_frontend_streaming_stop();
	};
	result.push_back(stream);

	NativeOutput recording;
	recording.label = T("Native.Recording");
	recording.active = state.recording;
	recording.state = state.recordingPaused ? T("State.Paused") : QString();
	recording.start = []() {
		obs_frontend_recording_start();
	};
	recording.stop = []() {
		obs_frontend_recording_stop();
	};
	result.push_back(recording);

	NativeOutput virtualCam;
	virtualCam.label = T("Native.VirtualCamera");
	virtualCam.active = state.virtualCam;
	virtualCam.start = []() {
		obs_frontend_start_virtualcam();
	};
	virtualCam.stop = []() {
		obs_frontend_stop_virtualcam();
	};
	result.push_back(virtualCam);

	NativeOutput replay;
	replay.label = T("Native.ReplayBuffer");
	replay.active = state.replayBuffer;
	replay.start = []() {
		obs_frontend_replay_buffer_start();
	};
	replay.stop = []() {
		obs_frontend_replay_buffer_stop();
	};
	result.push_back(replay);

	for (NativeOutput &output : result) {
		if (output.state.isEmpty())
			output.state = output.active ? T("State.Active") : T("State.Inactive");
	}
	return result;
}

QColor destinationColor(const Destination *destination)
{
	switch (destination->state()) {
	case Destination::State::Inactive:
		return destination->lastError().isEmpty() ? QColor(0x8a, 0x8a, 0x8a) : QColor(0xe0, 0x90, 0x30);
	case Destination::State::Starting:
	case Destination::State::Stopping:
		return QColor(0xe0, 0xc0, 0x40);
	case Destination::State::Reconnecting:
		return QColor(0xe0, 0x90, 0x30);
	case Destination::State::Active:
		/* Red = carrying the mix out (OBS's multiview Program colour); blue = projector open. */
		return destination->type() == DestinationType::Display ? QColor(0x40, 0xa0, 0xe0)
								       : QColor(0xd0, 0x00, 0x00);
	}
	return QColor();
}

QString destinationTooltip(Manager *manager, const Destination *destination)
{
	const Mix *mix = manager->mix(destination->mixId());
	QString text = QStringLiteral("<b>%1</b> — %2<br>")
			       .arg(destination->name().toHtmlEscaped(), destinationTypeName(destination->type()));
	text += T("Destination.Tooltip.Mix").arg(mix ? mix->name().toHtmlEscaped() : T("Destination.NoMix")) +
		QStringLiteral("<br>");
	text += T("Destination.Tooltip.Target").arg(destination->summary().toHtmlEscaped()) + QStringLiteral("<br>");
	text += T("Destination.Tooltip.Audio").arg(destination->audioSummary().toHtmlEscaped()) +
		QStringLiteral("<br>");
	text += T("Destination.Tooltip.State").arg(destination->stateText());
	if (!destination->lastError().isEmpty())
		text += QStringLiteral("<br><span style='color:#e09030'>%1</span>")
				.arg(destination->lastError().toHtmlEscaped());
	text += QStringLiteral("<br><i>%1</i>").arg(T("Destination.Tooltip.PluginManaged"));
	return text;
}

void toggleDestination(Destination *destination, QWidget *parent)
{
	if (!destination->active()) {
		if (!destination->start() && !destination->lastError().isEmpty())
			QMessageBox::warning(parent, T("Destination.StartFailed"), destination->lastError());
		return;
	}

	if (destination->type() != DestinationType::Display) {
		const auto answer = QMessageBox::question(parent, T("Destination.StopTitle"),
							  T("Destination.StopConfirm").arg(destination->name()));
		if (answer != QMessageBox::Yes)
			return;
	}
	destination->stop();
}

QToolButton *createDestinationChip(Manager *manager, Destination *destination, QWidget *parent)
{
	auto *chip = chipButton(parent);

	auto refresh = [chip, manager, destination]() {
		chip->setIcon(statusIcon(destinationColor(destination), chip->devicePixelRatioF()));
		chip->setText(destination->name());
		chip->setToolTip(destinationTooltip(manager, destination));
	};
	refresh();
	QObject::connect(destination, &Destination::changed, chip, refresh);

	auto *menu = new QMenu(chip);
	QObject::connect(menu, &QMenu::aboutToShow, chip, [menu, manager, destination, chip]() {
		menu->clear();
		QAction *title = menu->addAction(QStringLiteral("%1 — %2").arg(destinationTypeName(destination->type()),
									       destination->summary()));
		title->setEnabled(false);
		menu->addSeparator();
		const QString toggle = destination->active() ? T("Destination.Stop") : T("Destination.Start");
		menu->addAction(toggle, chip,
				[destination, chip]() { toggleDestination(destination, chip->window()); });
		QPointer<QToolButton> guard(chip);
		menu->addAction(T("Destination.Edit"), chip,
				[manager, destination, guard]() { editDestination(manager, destination, guard); });
		menu->addAction(T("Destination.Remove"), chip, [manager, destination, guard]() {
			confirmRemoveDestination(manager, destination, guard);
		});
	});
	chip->setMenu(menu);
	return chip;
}

QToolButton *createNativeChip(const NativeOutput &output, QWidget *parent)
{
	auto *chip = chipButton(parent);
	const QColor color = output.active ? QColor(0xd0, 0x00, 0x00) : QColor(0x8a, 0x8a, 0x8a);
	chip->setIcon(statusIcon(color, chip->devicePixelRatioF()));
	chip->setText(output.label);
	chip->setToolTip(
		QStringLiteral("<b>%1</b> — %2<br><i>%3</i>").arg(output.label, output.state, T("Native.Tooltip")));

	auto *menu = new QMenu(chip);
	const NativeOutput copy = output;
	menu->addAction(output.active ? T("Native.Stop").arg(output.label) : T("Native.Start").arg(output.label), chip,
			[copy]() {
				if (copy.active)
					copy.stop();
				else
					copy.start();
			});
	chip->setMenu(menu);
	return chip;
}

void addDestinationActions(QMenu *menu, Manager *manager, const QString &mixId, QWidget *dialogParent)
{
	QPointer<QWidget> parent(dialogParent);
	for (DestinationType type : {DestinationType::Display, DestinationType::Recording, DestinationType::Stream}) {
		menu->addAction(T("Destination.AddType").arg(destinationTypeName(type)), menu,
				[manager, mixId, type, parent]() {
					Destination *destination = manager->addDestination(type, mixId);
					DestinationDialog dialog(manager, destination, true, parent);
					if (dialog.exec() != QDialog::Accepted)
						manager->removeDestination(destination->id());
				});
	}
}

void editDestination(Manager *manager, Destination *destination, QWidget *parent)
{
	DestinationDialog dialog(manager, destination, false, parent);
	dialog.exec();
}

void confirmRemoveDestination(Manager *manager, Destination *destination, QWidget *parent)
{
	QString message = T("Destination.RemoveConfirm").arg(destination->name());
	if (destination->active())
		message += QStringLiteral("\n\n") + T("Destination.RemoveActive");
	if (QMessageBox::question(parent, T("Destination.Remove"), message) == QMessageBox::Yes)
		manager->removeDestination(destination->id());
}

} // namespace dm
