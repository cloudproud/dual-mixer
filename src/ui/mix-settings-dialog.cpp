/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "mix-settings-dialog.hpp"

#include "manager.hpp"
#include "mix.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace dm {

namespace {

QLabel *note(const QString &text, QWidget *parent)
{
	auto *label = new QLabel(text, parent);
	label->setWordWrap(true);
	QFont font = label->font();
	font.setPointSizeF(font.pointSizeF() * 0.9);
	label->setFont(font);
	return label;
}

} // namespace

MixSettingsDialog::MixSettingsDialog(Manager *manager, CanvasMix *mix, QWidget *parent)
	: QDialog(parent),
	  manager_(manager),
	  mix_(mix)
{
	setWindowTitle(T("MixSettings.Title").arg(mix->name()));
	setMinimumWidth(520);

	auto *layout = new QVBoxLayout(this);
	auto *tabs = new QTabWidget(this);
	tabs->addTab(buildGeneral(), T("MixSettings.General"));
	tabs->addTab(buildScenes(), T("MixSettings.Scenes"));
	layout->addWidget(tabs);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);

	/* The dialog holds a raw mix pointer; close it if the mix disappears. */
	connect(manager_, &Manager::mixRemoved, this, [this](const QString &id) {
		if (id == mix_->id())
			reject();
	});
}

QWidget *MixSettingsDialog::buildGeneral()
{
	auto *page = new QWidget(this);
	auto *form = new QFormLayout(page);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	name_ = new QLineEdit(mix_->name(), page);
	form->addRow(T("MixSettings.Name"), name_);

	obs_video_info ovi = {};
	mainVideoInfo(ovi);
	followMain_ = new QCheckBox(T("MixSettings.FollowMain").arg(ovi.base_width).arg(ovi.base_height), page);
	followMain_->setChecked(mix_->followsMainResolution());
	form->addRow(T("MixSettings.Resolution"), followMain_);

	auto *size = new QWidget(page);
	auto *sizeLayout = new QHBoxLayout(size);
	sizeLayout->setContentsMargins(0, 0, 0, 0);
	width_ = new QSpinBox(size);
	height_ = new QSpinBox(size);
	for (QSpinBox *spin : {width_, height_}) {
		spin->setRange(32, 8192);
		spin->setSingleStep(2);
	}
	width_->setValue(int(mix_->customWidth()));
	height_->setValue(int(mix_->customHeight()));
	sizeLayout->addWidget(width_);
	sizeLayout->addWidget(new QLabel(QStringLiteral("×"), size));
	sizeLayout->addWidget(height_);
	sizeLayout->addStretch(1);
	form->addRow(QString(), size);
	auto syncSize = [this]() {
		width_->setEnabled(!followMain_->isChecked());
		height_->setEnabled(!followMain_->isChecked());
	};
	connect(followMain_, &QCheckBox::toggled, this, syncSize);
	syncSize();
	form->addRow(QString(), note(T("MixSettings.ResolutionNote"), page));

	swap_ = new QCheckBox(T("MixSettings.Swap"), page);
	swap_->setChecked(mix_->swapAfterTransition());
	doubleClick_ = new QCheckBox(T("MixSettings.DoubleClick"), page);
	doubleClick_->setChecked(mix_->doubleClickTransitions());
	form->addRow(T("MixSettings.Behaviour"), swap_);
	form->addRow(QString(), doubleClick_);

	mixAudio_ = new QCheckBox(T("MixSettings.MixAudio"), page);
	mixAudio_->setChecked(mix_->mixesAudio());
	form->addRow(T("MixSettings.Audio"), mixAudio_);
	form->addRow(QString(), note(T("MixSettings.AudioNote"), page));

	form->addRow(T("MixSettings.Transition"), note(T("MixSettings.TransitionNote"), page));
	form->addRow(T("MixSettings.Hotkeys"), note(T("MixSettings.HotkeysNote").arg(mix_->name()), page));
	return page;
}

QWidget *MixSettingsDialog::buildScenes()
{
	auto *page = new QWidget(this);
	auto *layout = new QVBoxLayout(page);

	allScenes_ = new QRadioButton(T("MixSettings.AllScenes"), page);
	selectedScenes_ = new QRadioButton(T("MixSettings.SelectedScenes"), page);
	(mix_->usesSceneFilter() ? selectedScenes_ : allScenes_)->setChecked(true);
	layout->addWidget(allScenes_);
	layout->addWidget(selectedScenes_);

	scenes_ = new QListWidget(page);
	scenes_->setDragDropMode(QAbstractItemView::InternalMove);

	/* Selected scenes first in their saved order, then every other scene in OBS order. */
	const std::vector<std::string> &filter = mix_->sceneFilter();
	std::vector<OBSSource> all = frontendScenes();
	auto addItem = [&](obs_source_t *scene, bool checked) {
		auto *item = new QListWidgetItem(qstr(obs_source_get_name(scene)), scenes_);
		item->setData(Qt::UserRole, qstr(obs_source_get_uuid(scene)));
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled);
		item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
	};
	for (const std::string &uuid : filter) {
		if (OBSSource scene = sceneByUuid(uuid.c_str()))
			addItem(scene, true);
	}
	for (const OBSSource &scene : all) {
		const std::string uuid = obs_source_get_uuid(scene);
		if (std::find(filter.begin(), filter.end(), uuid) == filter.end())
			addItem(scene, false);
	}
	layout->addWidget(scenes_, 1);
	layout->addWidget(note(T("MixSettings.ScenesNote"), page));

	auto sync = [this]() {
		scenes_->setEnabled(selectedScenes_->isChecked());
	};
	connect(selectedScenes_, &QRadioButton::toggled, this, sync);
	sync();
	return page;
}

void MixSettingsDialog::accept()
{
	const QString name = name_->text().trimmed();
	if (!name.isEmpty())
		mix_->setName(name);

	std::vector<std::string> filter;
	for (int i = 0; i < scenes_->count(); i++) {
		QListWidgetItem *item = scenes_->item(i);
		if (item->checkState() == Qt::Checked)
			filter.push_back(utf8(item->data(Qt::UserRole).toString()));
	}
	mix_->setSceneFilter(selectedScenes_->isChecked(), std::move(filter));
	mix_->setSwapAfterTransition(swap_->isChecked());
	mix_->setDoubleClickTransitions(doubleClick_->isChecked());

	/* Resolution and audio mixing rebuild the mix's video; encoders must not be attached. */
	const bool follow = followMain_->isChecked();
	const auto cx = uint32_t(width_->value());
	const auto cy = uint32_t(height_->value());
	const bool resolutionChanged = follow != mix_->followsMainResolution() ||
				       (!follow && (cx != mix_->customWidth() || cy != mix_->customHeight()));
	const bool audioChanged = mixAudio_->isChecked() != mix_->mixesAudio();
	if ((resolutionChanged || audioChanged) && manager_->mixVideoInUse(mix_->id())) {
		QMessageBox::warning(this, T("MixSettings.Title").arg(mix_->name()), T("MixSettings.VideoBusy"));
	} else {
		if (resolutionChanged)
			mix_->setResolution(follow, cx, cy);
		if (audioChanged)
			mix_->setMixesAudio(mixAudio_->isChecked());
	}

	QDialog::accept();
}

} // namespace dm
