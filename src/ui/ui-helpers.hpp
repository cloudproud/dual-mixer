/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "destination.hpp"

#include <QColor>
#include <QString>

#include <functional>
#include <vector>

class QMenu;
class QToolButton;
class QWidget;

namespace dm {

class Manager;

/* Native OBS outputs. They always carry OBS's main mix and are controlled through the frontend. */
struct NativeOutput {
	QString label;
	bool active = false;
	QString state;
	std::function<void()> start;
	std::function<void()> stop;
};

std::vector<NativeOutput> nativeOutputs();

QColor destinationColor(const Destination *destination);
QString destinationTooltip(Manager *manager, const Destination *destination);

/* Compact status button for a plugin destination with a start/stop/edit/remove menu. */
QToolButton *createDestinationChip(Manager *manager, Destination *destination, QWidget *parent);
/* Compact status button for a native OBS output. */
QToolButton *createNativeChip(const NativeOutput &output, QWidget *parent);

/* Adds "Add display / recording / stream" actions that create a destination for the mix. */
void addDestinationActions(QMenu *menu, Manager *manager, const QString &mixId, QWidget *dialogParent);

void editDestination(Manager *manager, Destination *destination, QWidget *parent);
void confirmRemoveDestination(Manager *manager, Destination *destination, QWidget *parent);
/* Starts or stops, asking for confirmation before stopping a live stream or recording. */
void toggleDestination(Destination *destination, QWidget *parent);

} // namespace dm
