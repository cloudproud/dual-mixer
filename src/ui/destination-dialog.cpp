/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "destination-dialog.hpp"

#include "properties-form.hpp"

#include "destination.hpp"
#include "manager.hpp"
#include "mix.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
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

OBSData settingsWithDefaults(obs_data_t *defaults, obs_data_t *values)
{
	OBSData result = defaults ? defaults : OBSDataAutoRelease(obs_data_create()).Get();
	if (values)
		obs_data_apply(result, values);
	return result;
}

} // namespace

DestinationDialog::DestinationDialog(Manager *manager, Destination *destination, bool isNew, QWidget *parent)
	: QDialog(parent),
	  manager_(manager),
	  destination_(destination)
{
	const QString typeName = destinationTypeName(destination->type());
	setWindowTitle(isNew ? T("DestinationDialog.NewTitle").arg(typeName)
			     : T("DestinationDialog.EditTitle").arg(destination->name()));
	setMinimumSize(680, 720);

	auto *outer = new QVBoxLayout(this);
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto *content = new QWidget(scroll);
	auto *layout = new QVBoxLayout(content);
	scroll->setWidget(content);
	outer->addWidget(scroll, 1);

	auto *general = new QFormLayout();
	general->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	name_ = new QLineEdit(destination->name(), content);
	general->addRow(T("DestinationDialog.Name"), name_);
	mix_ = new QComboBox(content);
	mix_->addItem(T("Destination.NoMix"), QString());
	for (Mix *mix : manager_->mixes())
		mix_->addItem(mix->name(), mix->id());
	mix_->setCurrentIndex(std::max(0, mix_->findData(destination->mixId())));
	general->addRow(T("DestinationDialog.Mix"), mix_);
	general->addRow(QString(), note(T("DestinationDialog.MixNote"), content));
	layout->addLayout(general);

	if (destination->active() && destination->holdsEncoders()) {
		auto *locked = note(T("DestinationDialog.ActiveLocked"), content);
		locked->setStyleSheet(QStringLiteral("color: #e09030;"));
		layout->addWidget(locked);
		mix_->setEnabled(false);
	}

	switch (destination->type()) {
	case DestinationType::Display:
		layout->addWidget(buildDisplay());
		break;
	case DestinationType::Recording:
		layout->addWidget(buildRecording());
		layout->addWidget(buildEncoding());
		break;
	case DestinationType::Stream:
		layout->addWidget(buildStream());
		layout->addWidget(buildEncoding());
		break;
	}
	layout->addStretch(1);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	outer->addWidget(buttons);

	connect(manager_, &Manager::destinationRemoved, this, [this](const QString &id) {
		if (id == destination_->id())
			reject();
	});
}

QWidget *DestinationDialog::buildDisplay()
{
	auto *display = static_cast<DisplayDestination *>(destination_);
	auto *box = new QGroupBox(T("DestinationDialog.Display"), this);
	auto *form = new QFormLayout(box);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	screen_ = new QComboBox(box);
	screen_->addItem(T("Destination.Display.Windowed"), QString());
	const QList<QScreen *> screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); i++) {
		const QRect g = screens[i]->geometry();
		screen_->addItem(QStringLiteral("%1: %2 (%3×%4)")
					 .arg(i + 1)
					 .arg(screens[i]->name())
					 .arg(g.width())
					 .arg(g.height()),
				 screens[i]->name());
	}
	int index = screen_->findData(display->screenName());
	if (index < 0 && !display->screenName().isEmpty()) {
		screen_->addItem(T("Destination.Display.Missing").arg(display->screenName()), display->screenName());
		index = screen_->count() - 1;
	}
	screen_->setCurrentIndex(std::max(0, index));
	form->addRow(T("DestinationDialog.Screen"), screen_);

	reopen_ = new QCheckBox(T("DestinationDialog.Reopen"), box);
	reopen_->setChecked(display->reopenOnLoad());
	form->addRow(QString(), reopen_);
	form->addRow(T("DestinationDialog.Audio"), note(T("Audio.DisplayNote"), box));
	return box;
}

QWidget *DestinationDialog::buildRecording()
{
	auto *recording = static_cast<RecordingDestination *>(destination_);
	auto *box = new QGroupBox(T("DestinationDialog.Recording"), this);
	auto *form = new QFormLayout(box);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	auto *row = new QWidget(box);
	auto *h = new QHBoxLayout(row);
	h->setContentsMargins(0, 0, 0, 0);
	directory_ = new QLineEdit(recording->directory(), row);
	auto *browse = new QPushButton(T("Browse"), row);
	connect(browse, &QPushButton::clicked, this, [this]() {
		const QString dir =
			QFileDialog::getExistingDirectory(this, T("DestinationDialog.Directory"), directory_->text());
		if (!dir.isEmpty())
			directory_->setText(dir);
	});
	h->addWidget(directory_, 1);
	h->addWidget(browse);
	form->addRow(T("DestinationDialog.Directory"), row);

	filenameFormat_ = new QLineEdit(recording->filenameFormat(), box);
	form->addRow(T("DestinationDialog.Filename"), filenameFormat_);
	form->addRow(QString(), note(T("DestinationDialog.FilenameNote"), box));

	container_ = new QComboBox(box);
	for (const auto &[id, label] : RecordingDestination::containers())
		container_->addItem(label, id);
	container_->setCurrentIndex(std::max(0, container_->findData(recording->container())));
	form->addRow(T("DestinationDialog.Container"), container_);

	box->setEnabled(!destination_->active());
	return box;
}

QWidget *DestinationDialog::buildStream()
{
	auto *stream = static_cast<StreamDestination *>(destination_);
	auto *box = new QGroupBox(T("DestinationDialog.Stream"), this);
	auto *layout = new QVBoxLayout(box);
	auto *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	service_ = new QComboBox(box);
	for (const QString &id : StreamDestination::availableServices())
		service_->addItem(qstr(obs_service_get_display_name(utf8(id).c_str())), id);
	service_->setCurrentIndex(std::max(0, service_->findData(stream->serviceId())));
	form->addRow(T("DestinationDialog.Service"), service_);
	layout->addLayout(form);

	serviceHost_ = new QVBoxLayout();
	layout->addLayout(serviceHost_);
	layout->addWidget(note(T("DestinationDialog.StreamNote"), box));

	const std::string id = utf8(stream->serviceId());
	serviceSettings_ =
		settingsWithDefaults(OBSDataAutoRelease(obs_service_defaults(id.c_str())), stream->serviceSettings());
	rebuildServiceForm();
	connect(service_, &QComboBox::currentIndexChanged, this, [this, stream]() {
		const std::string selected = utf8(service_->currentData().toString());
		serviceSettings_ = settingsWithDefaults(
			OBSDataAutoRelease(obs_service_defaults(selected.c_str())),
			selected == utf8(stream->serviceId()) ? stream->serviceSettings() : nullptr);
		rebuildServiceForm();
	});

	box->setEnabled(!destination_->active());
	return box;
}

void DestinationDialog::rebuildServiceForm()
{
	delete serviceForm_;
	const std::string id = utf8(service_->currentData().toString());
	serviceForm_ = new PropertiesForm([id]() { return obs_get_service_properties(id.c_str()); }, serviceSettings_,
					  serviceHost_->parentWidget());
	serviceHost_->addWidget(serviceForm_);
}

QWidget *DestinationDialog::buildEncoding()
{
	auto *encoded = static_cast<EncodedDestination *>(destination_);
	auto *box = new QGroupBox(T("DestinationDialog.Encoding"), this);
	auto *layout = new QVBoxLayout(box);
	auto *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	videoEncoder_ = new QComboBox(box);
	for (const QString &id : EncodedDestination::availableVideoEncoders())
		videoEncoder_->addItem(encoderDisplayName(utf8(id).c_str()), id);
	videoEncoder_->setCurrentIndex(std::max(0, videoEncoder_->findData(encoded->videoEncoderId())));
	form->addRow(T("DestinationDialog.VideoEncoder"), videoEncoder_);
	layout->addLayout(form);

	videoHost_ = new QVBoxLayout();
	layout->addLayout(videoHost_);
	const std::string currentId = utf8(videoEncoder_->currentData().toString());
	videoSettings_ = settingsWithDefaults(
		OBSDataAutoRelease(obs_encoder_defaults(currentId.c_str())),
		currentId == utf8(encoded->videoEncoderId()) ? encoded->videoEncoderSettings() : nullptr);
	rebuildVideoForm();
	connect(videoEncoder_, &QComboBox::currentIndexChanged, this, [this, encoded]() {
		const std::string selected = utf8(videoEncoder_->currentData().toString());
		videoSettings_ = settingsWithDefaults(
			OBSDataAutoRelease(obs_encoder_defaults(selected.c_str())),
			selected == utf8(encoded->videoEncoderId()) ? encoded->videoEncoderSettings() : nullptr);
		rebuildVideoForm();
	});

	auto *audioForm = new QFormLayout();
	audioForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	audioEncoder_ = new QComboBox(box);
	for (const QString &id : EncodedDestination::availableAudioEncoders())
		audioEncoder_->addItem(encoderDisplayName(utf8(id).c_str()), id);
	audioEncoder_->setCurrentIndex(std::max(0, audioEncoder_->findData(encoded->audioEncoderId())));
	audioForm->addRow(T("DestinationDialog.AudioEncoder"), audioEncoder_);
	audioBitrate_ = new QSpinBox(box);
	audioBitrate_->setRange(32, 1024);
	audioBitrate_->setSingleStep(32);
	audioBitrate_->setSuffix(QStringLiteral(" kbps"));
	audioBitrate_->setValue(encoded->audioBitrate());
	audioForm->addRow(T("DestinationDialog.AudioBitrate"), audioBitrate_);

	/* Explicit audio selection: which OBS mixer tracks this destination carries. */
	auto *tracksWidget = new QWidget(box);
	auto *tracksLayout = new QVBoxLayout(tracksWidget);
	tracksLayout->setContentsMargins(0, 0, 0, 0);
	const bool multiple = encoded->multipleAudioTracks();
	auto *group = new QButtonGroup(tracksWidget);
	group->setExclusive(!multiple);
	const std::vector<int> &selected = encoded->audioTracks();
	for (int track = 0; track < kAudioTracks; track++) {
		QAbstractButton *button = multiple ? static_cast<QAbstractButton *>(new QCheckBox(tracksWidget))
						   : new QRadioButton(tracksWidget);
		button->setText(audioTrackLabel(track));
		button->setChecked(std::find(selected.begin(), selected.end(), track) != selected.end());
		group->addButton(button, track);
		tracksLayout->addWidget(button);
		tracks_.push_back(button);
	}
	audioForm->addRow(multiple ? T("DestinationDialog.AudioTracks") : T("DestinationDialog.AudioTrack"),
			  tracksWidget);
	audioForm->addRow(QString(), note(T("Outputs.AudioInfo"), box));
	layout->addLayout(audioForm);

	box->setEnabled(!destination_->active());
	return box;
}

void DestinationDialog::rebuildVideoForm()
{
	delete videoForm_;
	const std::string id = utf8(videoEncoder_->currentData().toString());
	videoForm_ = new PropertiesForm([id]() { return obs_get_encoder_properties(id.c_str()); }, videoSettings_,
					videoHost_->parentWidget());
	videoHost_->addWidget(videoForm_);
}

void DestinationDialog::accept()
{
	const QString name = name_->text().trimmed();
	if (name.isEmpty()) {
		QMessageBox::warning(this, windowTitle(), T("DestinationDialog.NameRequired"));
		return;
	}

	std::vector<int> tracks;
	for (size_t i = 0; i < tracks_.size(); i++) {
		if (tracks_[i]->isChecked())
			tracks.push_back(int(i));
	}
	if (!tracks_.empty() && tracks.empty()) {
		QMessageBox::warning(this, windowTitle(), T("Destination.Error.NoAudioTrack"));
		return;
	}

	destination_->setName(name);
	if (!destination_->setMixId(mix_->currentData().toString()))
		QMessageBox::information(this, windowTitle(), T("Outputs.RouteLocked"));

	switch (destination_->type()) {
	case DestinationType::Display: {
		auto *display = static_cast<DisplayDestination *>(destination_);
		display->setReopenOnLoad(reopen_->isChecked());
		const QString screen = screen_->currentData().toString();
		if (screen != display->screenName())
			display->setScreenName(screen);
		break;
	}
	case DestinationType::Recording: {
		if (destination_->active())
			break;
		auto *recording = static_cast<RecordingDestination *>(destination_);
		recording->setDirectory(directory_->text().trimmed());
		recording->setFilenameFormat(filenameFormat_->text().trimmed().isEmpty()
						     ? QStringLiteral("%CCYY-%MM-%DD %hh-%mm-%ss")
						     : filenameFormat_->text().trimmed());
		recording->setContainer(container_->currentData().toString());
		break;
	}
	case DestinationType::Stream: {
		if (destination_->active())
			break;
		static_cast<StreamDestination *>(destination_)
			->setService(service_->currentData().toString(), serviceSettings_);
		break;
	}
	}

	if (auto *encoded = qobject_cast<EncodedDestination *>(destination_); encoded && !encoded->active()) {
		encoded->setVideoEncoder(videoEncoder_->currentData().toString(), videoSettings_);
		encoded->setAudioEncoder(audioEncoder_->currentData().toString(), audioBitrate_->value());
		encoded->setAudioTracks(std::move(tracks));
	}

	QDialog::accept();
}

} // namespace dm
