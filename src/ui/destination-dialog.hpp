/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs.hpp>

#include <QDialog>

#include <vector>

class QAbstractButton;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QVBoxLayout;

namespace dm {

class Destination;
class Manager;
class PropertiesForm;

class DestinationDialog : public QDialog {
	Q_OBJECT

public:
	DestinationDialog(Manager *manager, Destination *destination, bool isNew, QWidget *parent = nullptr);

	void accept() override;

private:
	QWidget *buildDisplay();
	QWidget *buildRecording();
	QWidget *buildStream();
	QWidget *buildDevice();
	QWidget *buildEncoding();
	/* Adds one button per OBS audio track to tracks_. */
	QWidget *buildTrackPicker(const std::vector<int> &selected, bool multiple, QWidget *parent);
	void rebuildVideoForm();
	void rebuildServiceForm();

	Manager *manager_;
	Destination *destination_;

	QLineEdit *name_ = nullptr;
	QComboBox *mix_ = nullptr;

	/* Display */
	QComboBox *screen_ = nullptr;
	QCheckBox *reopen_ = nullptr;

	/* Recording */
	QLineEdit *directory_ = nullptr;
	QLineEdit *filenameFormat_ = nullptr;
	QComboBox *container_ = nullptr;

	/* Stream */
	QComboBox *service_ = nullptr;
	QVBoxLayout *serviceHost_ = nullptr;
	PropertiesForm *serviceForm_ = nullptr;
	OBSData serviceSettings_;

	/* Device */
	OBSData deviceSettings_;

	/* Encoding */
	QComboBox *videoEncoder_ = nullptr;
	QVBoxLayout *videoHost_ = nullptr;
	PropertiesForm *videoForm_ = nullptr;
	OBSData videoSettings_;
	QComboBox *audioEncoder_ = nullptr;
	QSpinBox *audioBitrate_ = nullptr;
	std::vector<QAbstractButton *> tracks_;
};

} // namespace dm
