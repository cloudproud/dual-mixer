/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "destination.hpp"

#include "manager.hpp"
#include "mix.hpp"
#include "ui/projector-window.hpp"

#include <plugin-support.h>

#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QScreen>

#include <algorithm>
#include <cstring>

namespace dm {

namespace {

template<typename Fn> void invokeOnUi(QObject *context, Fn &&fn)
{
	QMetaObject::invokeMethod(context, std::forward<Fn>(fn), Qt::QueuedConnection);
}

QString stopCodeText(int code)
{
	switch (code) {
	case OBS_OUTPUT_SUCCESS:
		return QString();
	case OBS_OUTPUT_BAD_PATH:
		return T("Output.Error.BadPath");
	case OBS_OUTPUT_CONNECT_FAILED:
		return T("Output.Error.ConnectFailed");
	case OBS_OUTPUT_INVALID_STREAM:
		return T("Output.Error.InvalidStream");
	case OBS_OUTPUT_DISCONNECTED:
		return T("Output.Error.Disconnected");
	case OBS_OUTPUT_UNSUPPORTED:
		return T("Output.Error.Unsupported");
	case OBS_OUTPUT_NO_SPACE:
		return T("Output.Error.NoSpace");
	case OBS_OUTPUT_ENCODE_ERROR:
		return T("Output.Error.Encode");
	default:
		return T("Output.Error.Generic");
	}
}

bool listContains(const char *semicolonList, const char *value)
{
	if (!semicolonList || !value)
		return true;
	const QStringList items = QString::fromUtf8(semicolonList).split(';', Qt::SkipEmptyParts);
	return items.contains(QString::fromUtf8(value));
}

bool arrayContains(const char **list, const char *value)
{
	if (!list || !value)
		return true;
	for (size_t i = 0; list[i]; i++) {
		if (strcmp(list[i], value) == 0)
			return true;
	}
	return false;
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Types */

const char *destinationTypeId(DestinationType type)
{
	switch (type) {
	case DestinationType::Display:
		return "display";
	case DestinationType::Recording:
		return "recording";
	case DestinationType::Stream:
		return "stream";
	}
	return "display";
}

std::optional<DestinationType> destinationTypeFromId(const char *id)
{
	for (DestinationType type : {DestinationType::Display, DestinationType::Recording, DestinationType::Stream}) {
		if (id && strcmp(id, destinationTypeId(type)) == 0)
			return type;
	}
	return std::nullopt;
}

QString destinationTypeName(DestinationType type)
{
	switch (type) {
	case DestinationType::Display:
		return T("Destination.Type.Display");
	case DestinationType::Recording:
		return T("Destination.Type.Recording");
	case DestinationType::Stream:
		return T("Destination.Type.Stream");
	}
	return QString();
}

/* ------------------------------------------------------------------------- */
/* Destination */

Destination::Destination(Manager *manager, QString id, DestinationType type)
	: QObject(manager),
	  manager_(manager),
	  id_(std::move(id)),
	  type_(type)
{
}

void Destination::setName(const QString &name)
{
	if (name.isEmpty() || name == name_)
		return;
	name_ = name;
	emit changed();
}

bool Destination::setMixId(const QString &mixId)
{
	if (mixId == mixId_)
		return true;
	if (active() && holdsEncoders())
		return false;
	mixId_ = mixId;
	if (active())
		onMixChanged();
	emit changed();
	return true;
}

QString Destination::stateText() const
{
	switch (state_) {
	case State::Inactive:
		return lastError_.isEmpty() ? T("State.Inactive") : T("State.Failed");
	case State::Starting:
		return T("State.Starting");
	case State::Active:
		return type_ == DestinationType::Display ? T("State.Open") : T("State.Active");
	case State::Reconnecting:
		return T("State.Reconnecting");
	case State::Stopping:
		return T("State.Stopping");
	}
	return QString();
}

void Destination::setState(State state)
{
	if (state == state_)
		return;
	state_ = state;
	if (state == State::Starting || state == State::Active)
		lastError_.clear();
	emit changed();
}

void Destination::setError(const QString &error)
{
	lastError_ = error;
	if (!error.isEmpty())
		obs_log(LOG_WARNING, "Destination '%s': %s", utf8(name_).c_str(), utf8(error).c_str());
	emit changed();
}

Mix *Destination::resolveMix()
{
	return manager_->mix(mixId_);
}

void Destination::save(obs_data_t *data) const
{
	obs_data_set_string(data, "id", utf8(id_).c_str());
	obs_data_set_string(data, "type", destinationTypeId(type_));
	obs_data_set_string(data, "name", utf8(name_).c_str());
	obs_data_set_string(data, "mix", utf8(mixId_).c_str());
}

void Destination::load(obs_data_t *data)
{
	name_ = qstr(obs_data_get_string(data, "name"));
	mixId_ = qstr(obs_data_get_string(data, "mix"));
}

/* ------------------------------------------------------------------------- */
/* DisplayDestination */

DisplayDestination::DisplayDestination(Manager *manager, QString id)
	: Destination(manager, std::move(id), DestinationType::Display)
{
}

DisplayDestination::~DisplayDestination()
{
	delete window_.data();
}

bool DisplayDestination::start()
{
	if (window_) {
		window_->raise();
		return true;
	}

	Mix *mix = resolveMix();
	if (!mix) {
		setError(T("Destination.Error.NoMix"));
		return false;
	}

	window_ = new ProjectorWindow(mix);
	connect(window_, &ProjectorWindow::closed, this, [this]() {
		if (window_ && !window_->isFullScreen())
			geometry_ = window_->saveGeometry();
		window_ = nullptr;
		wasOpen_ = false;
		setState(State::Inactive);
	});
	connect(window_, &ProjectorWindow::screenRequested, this, [this](const QString &name) { setScreenName(name); });

	wasOpen_ = true;
	setState(State::Active);
	place();
	return true;
}

void DisplayDestination::place()
{
	if (!window_)
		return;

	QScreen *target = nullptr;
	for (QScreen *screen : QGuiApplication::screens()) {
		if (!screenName_.isEmpty() && screen->name() == screenName_)
			target = screen;
	}

	if (target) {
		window_->showFullScreenOn(target);
	} else {
		window_->showWindowed(geometry_);
	}
}

void DisplayDestination::stop()
{
	if (window_)
		window_->close();
}

void DisplayDestination::forceStop()
{
	if (!window_)
		return;
	ProjectorWindow *window = window_;
	window_ = nullptr;
	window->disconnect(this);
	delete window;
	setState(State::Inactive);
}

void DisplayDestination::onMixChanged()
{
	Mix *mix = resolveMix();
	if (!mix) {
		forceStop();
		return;
	}
	if (window_)
		window_->setMix(mix);
}

void DisplayDestination::setScreenName(const QString &name)
{
	if (window_ && !window_->isFullScreen())
		geometry_ = window_->saveGeometry();
	screenName_ = name;
	place();
	emit changed();
}

QString DisplayDestination::summary() const
{
	if (screenName_.isEmpty())
		return T("Destination.Display.Windowed");
	for (QScreen *screen : QGuiApplication::screens()) {
		if (screen->name() == screenName_)
			return T("Destination.Display.Fullscreen").arg(screenName_);
	}
	return T("Destination.Display.Missing").arg(screenName_);
}

QString DisplayDestination::audioSummary() const
{
	return T("Audio.VideoOnly");
}

void DisplayDestination::save(obs_data_t *data) const
{
	Destination::save(data);
	QByteArray geometry = geometry_;
	if (window_ && !window_->isFullScreen())
		geometry = window_->saveGeometry();
	obs_data_set_string(data, "screen", utf8(screenName_).c_str());
	obs_data_set_string(data, "geometry", geometry.toBase64().constData());
	obs_data_set_bool(data, "reopen", reopenOnLoad_);
	obs_data_set_bool(data, "open", active());
}

void DisplayDestination::load(obs_data_t *data)
{
	Destination::load(data);
	screenName_ = qstr(obs_data_get_string(data, "screen"));
	geometry_ = QByteArray::fromBase64(obs_data_get_string(data, "geometry"));
	obs_data_set_default_bool(data, "reopen", true);
	reopenOnLoad_ = obs_data_get_bool(data, "reopen");
	wasOpen_ = obs_data_get_bool(data, "open");
}

/* ------------------------------------------------------------------------- */
/* EncodedDestination */

EncodedDestination::EncodedDestination(Manager *manager, QString id, DestinationType type)
	: Destination(manager, std::move(id), type)
{
	const std::vector<QString> video = availableVideoEncoders();
	if (!video.empty()) {
		videoEncoderId_ = video.front();
		videoSettings_ = OBSDataAutoRelease(obs_encoder_defaults(utf8(videoEncoderId_).c_str())).Get();
	}
	if (!videoSettings_)
		videoSettings_ = OBSDataAutoRelease(obs_data_create()).Get();

	const std::vector<QString> audio = availableAudioEncoders();
	if (!audio.empty())
		audioEncoderId_ = audio.front();
}

EncodedDestination::~EncodedDestination()
{
	forceStop();
}

std::vector<QString> EncodedDestination::availableVideoEncoders()
{
	/* Hardware encoders first when present, then everything else that is registered. */
	static const char *preferred[] = {
		"com.apple.videotoolbox.videoencoder.ave.avc",
		"obs_nvenc_h264_tex",
		"jim_nvenc",
		"h264_texture_amf",
		"obs_qsv11_v2",
		"obs_x264",
	};

	std::vector<QString> all;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
			continue;
		if (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
			continue;
		all.push_back(qstr(id));
	}

	std::vector<QString> result;
	for (const char *p : preferred) {
		const QString value = qstr(p);
		if (std::find(all.begin(), all.end(), value) != all.end())
			result.push_back(value);
	}
	for (const QString &value : all) {
		if (std::find(result.begin(), result.end(), value) == result.end())
			result.push_back(value);
	}
	return result;
}

std::vector<QString> EncodedDestination::availableAudioEncoders()
{
	static const char *preferred[] = {"CoreAudio_AAC", "ffmpeg_aac", "ffmpeg_opus"};

	std::vector<QString> all;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (obs_get_encoder_type(id) != OBS_ENCODER_AUDIO)
			continue;
		if (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
			continue;
		all.push_back(qstr(id));
	}

	std::vector<QString> result;
	for (const char *p : preferred) {
		const QString value = qstr(p);
		if (std::find(all.begin(), all.end(), value) != all.end())
			result.push_back(value);
	}
	for (const QString &value : all) {
		if (std::find(result.begin(), result.end(), value) == result.end())
			result.push_back(value);
	}
	return result;
}

void EncodedDestination::setVideoEncoder(const QString &id, obs_data_t *settings)
{
	videoEncoderId_ = id;
	videoSettings_ = OBSDataAutoRelease(obs_data_create()).Get();
	if (settings)
		obs_data_apply(videoSettings_, settings);
	emit changed();
}

void EncodedDestination::setAudioEncoder(const QString &id, int bitrate)
{
	audioEncoderId_ = id;
	audioBitrate_ = bitrate;
	emit changed();
}

void EncodedDestination::setAudioTracks(std::vector<int> tracks)
{
	tracks_ = std::move(tracks);
	if (!multipleAudioTracks() && tracks_.size() > 1)
		tracks_.resize(1);
	emit changed();
}

QString EncodedDestination::audioSummary() const
{
	QStringList parts;
	for (int track : tracks_)
		parts << audioTrackLabel(track);
	return parts.isEmpty() ? T("Audio.None") : parts.join(QStringLiteral(", "));
}

bool EncodedDestination::start()
{
	if (active())
		return true;

	Mix *mix = resolveMix();
	if (!mix) {
		setError(T("Destination.Error.NoMix"));
		return false;
	}
	video_t *video = mix->video();
	if (!video) {
		setError(T("Destination.Error.NoVideo"));
		return false;
	}
	if (tracks_.empty()) {
		setError(T("Destination.Error.NoAudioTrack"));
		return false;
	}
	if (videoEncoderId_.isEmpty() || audioEncoderId_.isEmpty()) {
		setError(T("Destination.Error.NoEncoder"));
		return false;
	}

	const std::string base = utf8(name()) + " (" + PLUGIN_NAME + ")";
	const std::string videoId = utf8(videoEncoderId_);
	const std::string audioId = utf8(audioEncoderId_);

	OBSDataAutoRelease videoSettings = obs_data_create();
	obs_data_apply(videoSettings, videoSettings_);
	OBSDataAutoRelease audioSettings = obs_data_create();
	obs_data_set_int(audioSettings, "bitrate", audioBitrate_);

	QString error;
	output_ = createOutput(videoSettings, audioSettings, error);
	if (!output_) {
		setError(error.isEmpty() ? T("Destination.Error.Output") : error);
		release();
		return false;
	}

	const char *codec = obs_get_encoder_codec(videoId.c_str());
	if (!listContains(obs_get_output_supported_video_codecs(obs_output_get_id(output_)), codec)) {
		setError(T("Destination.Error.Codec").arg(qstr(codec)));
		release();
		return false;
	}

	videoEncoder_ = OBSEncoderAutoRelease(obs_video_encoder_create(videoId.c_str(), (base + " video").c_str(),
								       videoSettings, nullptr))
				.Get();
	if (!videoEncoder_) {
		setError(T("Destination.Error.EncoderCreate").arg(encoderDisplayName(videoId.c_str())));
		release();
		return false;
	}
	obs_encoder_set_video(videoEncoder_, video);
	obs_output_set_video_encoder(output_, videoEncoder_);

	for (size_t i = 0; i < tracks_.size(); i++) {
		const std::string name = base + " audio " + std::to_string(tracks_[i] + 1);
		OBSEncoderAutoRelease encoder = obs_audio_encoder_create(audioId.c_str(), name.c_str(), audioSettings,
									 size_t(tracks_[i]), nullptr);
		if (!encoder) {
			setError(T("Destination.Error.EncoderCreate").arg(encoderDisplayName(audioId.c_str())));
			release();
			return false;
		}
		obs_encoder_set_audio(encoder, obs_get_audio());
		obs_output_set_audio_encoder(output_, encoder, i);
		audioEncoders_.emplace_back(encoder.Get());
	}

	signal_handler_t *handler = obs_output_get_signal_handler(output_);
	startSignal_.Connect(handler, "start", onStart, this);
	stopSignal_.Connect(handler, "stop", onStop, this);
	reconnectSignal_.Connect(handler, "reconnect", onReconnect, this);
	reconnectSuccessSignal_.Connect(handler, "reconnect_success", onReconnectSuccess, this);

	setState(State::Starting);
	if (!obs_output_start(output_)) {
		const char *last = obs_output_get_last_error(output_);
		const QString message = last && *last ? qstr(last) : T("Destination.Error.Start");
		release();
		setState(State::Inactive);
		setError(message);
		return false;
	}

	obs_log(LOG_INFO, "Destination '%s' started from mix '%s' (%s)", utf8(name()).c_str(),
		utf8(mix->name()).c_str(), obs_output_get_id(output_));
	return true;
}

void EncodedDestination::stop()
{
	if (!output_ || state() == State::Stopping)
		return;
	setState(State::Stopping);
	obs_output_stop(output_);
}

void EncodedDestination::forceStop()
{
	if (!output_)
		return;
	startSignal_.Disconnect();
	stopSignal_.Disconnect();
	reconnectSignal_.Disconnect();
	reconnectSuccessSignal_.Disconnect();
	if (obs_output_active(output_))
		obs_output_force_stop(output_);
	release();
	setState(State::Inactive);
}

void EncodedDestination::release()
{
	startSignal_.Disconnect();
	stopSignal_.Disconnect();
	reconnectSignal_.Disconnect();
	reconnectSuccessSignal_.Disconnect();
	/* Releasing the output joins its stop thread, after which the encoders are idle. */
	output_ = nullptr;
	releaseOutput();
	videoEncoder_ = nullptr;
	audioEncoders_.clear();
}

void EncodedDestination::handleStopped(int code, const QString &error)
{
	release();
	setState(State::Inactive);
	if (code != OBS_OUTPUT_SUCCESS)
		setError(error.isEmpty() ? stopCodeText(code) : stopCodeText(code) + QStringLiteral(": ") + error);
	obs_log(LOG_INFO, "Destination '%s' stopped (code %d)", utf8(name()).c_str(), code);
}

void EncodedDestination::onStart(void *data, calldata_t *)
{
	auto *self = static_cast<EncodedDestination *>(data);
	invokeOnUi(self, [self]() { self->setState(State::Active); });
}

void EncodedDestination::onStop(void *data, calldata_t *cd)
{
	auto *self = static_cast<EncodedDestination *>(data);
	const int code = int(calldata_int(cd, "code"));
	obs_output_t *output = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	const QString error = qstr(output ? obs_output_get_last_error(output) : nullptr);
	invokeOnUi(self, [self, code, error]() { self->handleStopped(code, error); });
}

void EncodedDestination::onReconnect(void *data, calldata_t *)
{
	auto *self = static_cast<EncodedDestination *>(data);
	invokeOnUi(self, [self]() { self->setState(State::Reconnecting); });
}

void EncodedDestination::onReconnectSuccess(void *data, calldata_t *)
{
	auto *self = static_cast<EncodedDestination *>(data);
	invokeOnUi(self, [self]() { self->setState(State::Active); });
}

void EncodedDestination::save(obs_data_t *data) const
{
	Destination::save(data);
	obs_data_set_string(data, "video_encoder", utf8(videoEncoderId_).c_str());
	obs_data_set_obj(data, "video_encoder_settings", videoSettings_);
	obs_data_set_string(data, "audio_encoder", utf8(audioEncoderId_).c_str());
	obs_data_set_int(data, "audio_bitrate", audioBitrate_);
	OBSDataArrayAutoRelease tracks = obs_data_array_create();
	for (int track : tracks_) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_int(item, "track", track);
		obs_data_array_push_back(tracks, item);
	}
	obs_data_set_array(data, "audio_tracks", tracks);
}

void EncodedDestination::load(obs_data_t *data)
{
	Destination::load(data);
	const char *videoId = obs_data_get_string(data, "video_encoder");
	if (*videoId) {
		videoEncoderId_ = qstr(videoId);
		OBSDataAutoRelease settings = obs_data_get_obj(data, "video_encoder_settings");
		videoSettings_ = OBSDataAutoRelease(obs_data_create()).Get();
		if (settings)
			obs_data_apply(videoSettings_, settings);
	}
	const char *audioId = obs_data_get_string(data, "audio_encoder");
	if (*audioId)
		audioEncoderId_ = qstr(audioId);
	if (obs_data_has_user_value(data, "audio_bitrate"))
		audioBitrate_ = int(obs_data_get_int(data, "audio_bitrate"));

	OBSDataArrayAutoRelease tracks = obs_data_get_array(data, "audio_tracks");
	if (tracks) {
		tracks_.clear();
		for (size_t i = 0, n = obs_data_array_count(tracks); i < n; i++) {
			OBSDataAutoRelease item = obs_data_array_item(tracks, i);
			const int track = int(obs_data_get_int(item, "track"));
			if (track >= 0 && track < kAudioTracks)
				tracks_.push_back(track);
		}
		if (!multipleAudioTracks() && tracks_.size() > 1)
			tracks_.resize(1);
	}
}

/* ------------------------------------------------------------------------- */
/* RecordingDestination */

RecordingDestination::RecordingDestination(Manager *manager, QString id)
	: EncodedDestination(manager, std::move(id), DestinationType::Recording),
	  filenameFormat_(QStringLiteral("%CCYY-%MM-%DD %hh-%mm-%ss"))
{
	char *path = obs_frontend_get_current_record_output_path();
	directory_ = path && *path ? qstr(path) : QDir::homePath();
	bfree(path);
}

const std::vector<std::pair<QString, QString>> &RecordingDestination::containers()
{
	static const std::vector<std::pair<QString, QString>> list = {
		{QStringLiteral("mkv"), QStringLiteral("Matroska (.mkv)")},
		{QStringLiteral("hybrid_mp4"), QStringLiteral("Hybrid MP4 (.mp4)")},
		{QStringLiteral("mp4"), QStringLiteral("MPEG-4 (.mp4)")},
		{QStringLiteral("mov"), QStringLiteral("QuickTime (.mov)")},
		{QStringLiteral("ts"), QStringLiteral("MPEG-TS (.ts)")},
	};
	return list;
}

QString RecordingDestination::summary() const
{
	if (active() && !lastFile_.isEmpty())
		return lastFile_;
	return QDir::toNativeSeparators(directory_) + QStringLiteral(" (") + container_ + QStringLiteral(")");
}

OBSOutput RecordingDestination::createOutput(obs_data_t *, obs_data_t *, QString &error)
{
	const bool hybrid = container_ == QStringLiteral("hybrid_mp4");
	const QString extension = hybrid ? QStringLiteral("mp4") : container_;
	const char *outputId = hybrid ? "mp4_output" : "ffmpeg_muxer";

	if (!QDir().mkpath(directory_)) {
		error = T("Output.Error.BadPath");
		return nullptr;
	}

	/* The mix name keeps simultaneous recordings from different mixes apart. */
	QString prefix;
	if (Mix *mix = resolveMix())
		prefix = mix->name() + QStringLiteral(" ");
	char *formatted = os_generate_formatted_filename(utf8(extension).c_str(), true, utf8(filenameFormat_).c_str());
	QString file = QDir(directory_).filePath(prefix + qstr(formatted));
	bfree(formatted);

	/* Never overwrite an existing recording. */
	for (int n = 2; QFile::exists(file); n++) {
		QFileInfo info(file);
		file = info.dir().filePath(
			QStringLiteral("%1 (%2).%3").arg(info.completeBaseName()).arg(n).arg(extension));
	}
	lastFile_ = QDir::toNativeSeparators(file);

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "path", utf8(file).c_str());
	OBSOutputAutoRelease output =
		obs_output_create(outputId, (utf8(name()) + " recording").c_str(), settings, nullptr);
	if (!output)
		error = T("Destination.Error.OutputType").arg(qstr(outputId));
	return output.Get();
}

void RecordingDestination::save(obs_data_t *data) const
{
	EncodedDestination::save(data);
	obs_data_set_string(data, "directory", utf8(directory_).c_str());
	obs_data_set_string(data, "filename_format", utf8(filenameFormat_).c_str());
	obs_data_set_string(data, "container", utf8(container_).c_str());
}

void RecordingDestination::load(obs_data_t *data)
{
	EncodedDestination::load(data);
	if (const char *dir = obs_data_get_string(data, "directory"); *dir)
		directory_ = qstr(dir);
	if (const char *format = obs_data_get_string(data, "filename_format"); *format)
		filenameFormat_ = qstr(format);
	if (const char *container = obs_data_get_string(data, "container"); *container)
		container_ = qstr(container);
}

/* ------------------------------------------------------------------------- */
/* StreamDestination */

StreamDestination::StreamDestination(Manager *manager, QString id)
	: EncodedDestination(manager, std::move(id), DestinationType::Stream)
{
	serviceSettings_ = OBSDataAutoRelease(obs_service_defaults(utf8(serviceId_).c_str())).Get();
	if (!serviceSettings_)
		serviceSettings_ = OBSDataAutoRelease(obs_data_create()).Get();
}

std::vector<QString> StreamDestination::availableServices()
{
	std::vector<QString> result;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_service_types(i, &id); i++) {
		if (strcmp(id, "rtmp_custom") == 0)
			result.insert(result.begin(), qstr(id));
		else
			result.push_back(qstr(id));
	}
	return result;
}

void StreamDestination::setService(const QString &id, obs_data_t *settings)
{
	serviceId_ = id;
	serviceSettings_ = OBSDataAutoRelease(obs_data_create()).Get();
	if (settings)
		obs_data_apply(serviceSettings_, settings);
	emit changed();
}

QString StreamDestination::summary() const
{
	const QString service = qstr(obs_service_get_display_name(utf8(serviceId_).c_str()));
	const QString server = qstr(obs_data_get_string(serviceSettings_, "server"));
	const QString named = qstr(obs_data_get_string(serviceSettings_, "service"));
	QString target = named.isEmpty() ? server : named;
	return target.isEmpty() ? service : QStringLiteral("%1 — %2").arg(service, target);
}

OBSOutput StreamDestination::createOutput(obs_data_t *videoSettings, obs_data_t *audioSettings, QString &error)
{
	const std::string serviceId = utf8(serviceId_);
	service_ = OBSServiceAutoRelease(obs_service_create(serviceId.c_str(), (utf8(name()) + " service").c_str(),
							    serviceSettings_, nullptr))
			   .Get();
	if (!service_) {
		error = T("Destination.Error.Service").arg(qstr(serviceId.c_str()));
		return nullptr;
	}

	const char *server = obs_service_get_connect_info(service_, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	if (!server || !*server) {
		error = T("Destination.Error.NoServer");
		return nullptr;
	}

	/* Let the service enforce its own encoder constraints (keyframe interval, bitrate caps). */
	obs_service_apply_encoder_settings(service_, videoSettings, audioSettings);

	const char *videoCodec = obs_get_encoder_codec(utf8(videoEncoderId()).c_str());
	if (!arrayContains(obs_service_get_supported_video_codecs(service_), videoCodec)) {
		error = T("Destination.Error.Codec").arg(qstr(videoCodec));
		return nullptr;
	}
	const char *audioCodec = obs_get_encoder_codec(utf8(audioEncoderId()).c_str());
	if (!arrayContains(obs_service_get_supported_audio_codecs(service_), audioCodec)) {
		error = T("Destination.Error.Codec").arg(qstr(audioCodec));
		return nullptr;
	}

	std::string outputType;
	if (const char *preferred = obs_service_get_preferred_output_type(service_))
		outputType = preferred;
	if (outputType.empty()) {
		obs_enum_output_types_with_protocol(obs_service_get_protocol(service_), &outputType,
						    [](void *param, const char *id) {
							    *static_cast<std::string *>(param) = id;
							    return false;
						    });
	}
	if (outputType.empty()) {
		error = T("Destination.Error.Protocol").arg(qstr(obs_service_get_protocol(service_)));
		return nullptr;
	}

	OBSOutputAutoRelease output =
		obs_output_create(outputType.c_str(), (utf8(name()) + " stream").c_str(), nullptr, nullptr);
	if (!output) {
		error = T("Destination.Error.OutputType").arg(qstr(outputType.c_str()));
		return nullptr;
	}
	obs_output_set_service(output, service_);
	obs_output_set_reconnect_settings(output, 25, 2);
	return output.Get();
}

void StreamDestination::save(obs_data_t *data) const
{
	EncodedDestination::save(data);
	obs_data_set_string(data, "service", utf8(serviceId_).c_str());
	obs_data_set_obj(data, "service_settings", serviceSettings_);
}

void StreamDestination::load(obs_data_t *data)
{
	EncodedDestination::load(data);
	if (const char *service = obs_data_get_string(data, "service"); *service) {
		serviceId_ = qstr(service);
		OBSDataAutoRelease settings = obs_data_get_obj(data, "service_settings");
		serviceSettings_ = OBSDataAutoRelease(obs_data_create()).Get();
		if (settings)
			obs_data_apply(serviceSettings_, settings);
	}
}

} // namespace dm
