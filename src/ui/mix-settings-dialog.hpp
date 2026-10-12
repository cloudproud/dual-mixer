/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QDialog>

class QCheckBox;
class QLineEdit;
class QListWidget;
class QRadioButton;
class QSpinBox;

namespace dm {

class CanvasMix;
class Manager;

class MixSettingsDialog : public QDialog {
	Q_OBJECT

public:
	MixSettingsDialog(Manager *manager, CanvasMix *mix, QWidget *parent = nullptr);

	void accept() override;

private:
	QWidget *buildGeneral();
	QWidget *buildScenes();

	Manager *manager_;
	CanvasMix *mix_;

	QLineEdit *name_ = nullptr;
	QCheckBox *followMain_ = nullptr;
	QSpinBox *width_ = nullptr;
	QSpinBox *height_ = nullptr;
	QCheckBox *swap_ = nullptr;
	QCheckBox *doubleClick_ = nullptr;
	QCheckBox *mixAudio_ = nullptr;

	QRadioButton *allScenes_ = nullptr;
	QRadioButton *selectedScenes_ = nullptr;
	QListWidget *scenes_ = nullptr;
};

} // namespace dm
