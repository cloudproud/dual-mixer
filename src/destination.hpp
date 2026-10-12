/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "common.hpp"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <optional>
#include <vector>

namespace dm {

class Manager;
class Mix;
class ProjectorWindow;

/* Where a mix's Program goes. The type belongs to the destination, never to the mix. */
enum class DestinationType { Display, Recording, Stream, Device };

/* Every destination type, in the order offered to the user. */
inline constexpr DestinationType kDestinationTypes[] = {DestinationType::Display, DestinationType::Recording,
							DestinationType::Stream, DestinationType::Device};

const char *destinationTypeId(DestinationType type);
std::optional<DestinationType> destinationTypeFromId(const char *id);
QString destinationTypeName(DestinationType type);

/*
 * A plugin-managed output destination fed by exactly one mix (selected by id). A mix can feed
 * any number of destinations. Native OBS outputs are not destinations; see Manager.
 */
class Destination : public QObject {
	Q_OBJECT

public:
	enum class State { Inactive, Starting, Active, Reconnecting, Stopping };

	Destination(Manager *manager, QString id, DestinationType type);

	const QString &id() const { return id_; }
	DestinationType type() const { return type_; }

	QString name() const { return name_; }
	void setName(const QString &name);

	const QString &mixId() const { return mixId_; }
	/* Returns false when the destination cannot be re-routed while active. */
	bool setMixId(const QString &mixId);

	State state() const { return state_; }
	bool active() const { return state_ != State::Inactive; }
	QString stateText() const;
	const QString &lastError() const { return lastError_; }

	/* Starts the destination; on failure lastError() describes why. */
	virtual bool start() = 0;
	virtual void stop() = 0;
	/* Synchronous teardown used when the mix or OBS goes away. */
	virtual void forceStop() = 0;

	/*
	 * True when the destination's output is bound to its mix's video while active: the mix's
	 * canvas must not be recreated and the destination cannot be re-routed until it stops.
	 */
	virtual bool holdsVideo() const = 0;
	virtual QString summary() const = 0;
	virtual QString audioSummary() const = 0;

	virtual void save(obs_data_t *data) const;
	virtual void load(obs_data_t *data);

signals:
	void changed();

protected:
	void setState(State state);
	void setError(const QString &error);
	Mix *resolveMix();
	virtual void onMixChanged() {}

	Manager *manager_;

private:
	QString id_;
	DestinationType type_;
	QString name_;
	QString mixId_;
	State state_ = State::Inactive;
	QString lastError_;
};

/* A projector window on a physical display (fullscreen) or as a free window. Video only. */
class DisplayDestination final : public Destination {
	Q_OBJECT

public:
	DisplayDestination(Manager *manager, QString id);
	~DisplayDestination() override;

	bool start() override;
	void stop() override;
	void forceStop() override;
	bool holdsVideo() const override { return false; }
	QString summary() const override;
	QString audioSummary() const override;

	/* Name of the target screen; empty means windowed. */
	const QString &screenName() const { return screenName_; }
	void setScreenName(const QString &name);
	bool reopenOnLoad() const { return reopenOnLoad_; }
	void setReopenOnLoad(bool enabled) { reopenOnLoad_ = enabled; }
	/* Whether the projector was open when last saved (restored when reopenOnLoad is set). */
	bool wasOpen() const { return wasOpen_; }

	void save(obs_data_t *data) const override;
	void load(obs_data_t *data) override;

protected:
	void onMixChanged() override;

private:
	void place();

	QString screenName_;
	QByteArray geometry_;
	bool reopenOnLoad_ = true;
	bool wasOpen_ = false;
	QPointer<ProjectorWindow> window_;
};

/* A destination backed by an OBS output; follows the output's lifecycle through its signals. */
class OutputDestination : public Destination {
	Q_OBJECT

public:
	OutputDestination(Manager *manager, QString id, DestinationType type);
	~OutputDestination() override;

	void stop() override;
	void forceStop() override;
	bool holdsVideo() const override { return true; }

protected:
	/* Connects to output_ and starts it. On failure releases it and reports startError (or OBS's error). */
	bool startOutput(Mix *mix, const QString &startError);
	/* Drops the output, then whatever the subclass keeps for it (releaseOutput). */
	void release();
	/* Releases resources that must outlive the output (encoders, service). */
	virtual void releaseOutput() {}

	OBSOutput output_;

private:
	void disconnectSignals();
	void handleStopped(int code, const QString &error);
	static void onStart(void *data, calldata_t *cd);
	static void onStop(void *data, calldata_t *cd);
	static void onReconnect(void *data, calldata_t *cd);
	static void onReconnectSuccess(void *data, calldata_t *cd);

	OBSSignal startSignal_;
	OBSSignal stopSignal_;
	OBSSignal reconnectSignal_;
	OBSSignal reconnectSuccessSignal_;
};

/* Shared encoder/audio configuration for destinations that encode (recording, streaming). */
class EncodedDestination : public OutputDestination {
	Q_OBJECT

public:
	EncodedDestination(Manager *manager, QString id, DestinationType type);
	/* Encoders must be released after the output, so stop while they still exist. */
	~EncodedDestination() override;

	bool start() override;
	QString audioSummary() const override;

	const QString &videoEncoderId() const { return videoEncoderId_; }
	obs_data_t *videoEncoderSettings() const { return videoSettings_; }
	void setVideoEncoder(const QString &id, obs_data_t *settings);

	const QString &audioEncoderId() const { return audioEncoderId_; }
	int audioBitrate() const { return audioBitrate_; }
	void setAudioEncoder(const QString &id, int bitrate);

	/* OBS audio mixer tracks (0-based) sent to this destination, in order. */
	const std::vector<int> &audioTracks() const { return tracks_; }
	void setAudioTracks(std::vector<int> tracks);
	virtual bool multipleAudioTracks() const = 0;

	void save(obs_data_t *data) const override;
	void load(obs_data_t *data) override;

	/* Registered video/audio encoder ids usable by destinations, best default first. */
	static std::vector<QString> availableVideoEncoders();
	static std::vector<QString> availableAudioEncoders();

protected:
	/* Creates the output (and anything it depends on). Encoder settings may be adjusted. */
	virtual OBSOutput createOutput(obs_data_t *videoSettings, obs_data_t *audioSettings, QString &error) = 0;
	void releaseOutput() override;

private:
	QString videoEncoderId_;
	OBSData videoSettings_;
	QString audioEncoderId_;
	int audioBitrate_ = 160;
	std::vector<int> tracks_{0};

	OBSEncoder videoEncoder_;
	std::vector<OBSEncoder> audioEncoders_;
};

class RecordingDestination final : public EncodedDestination {
	Q_OBJECT

public:
	RecordingDestination(Manager *manager, QString id);

	QString summary() const override;
	bool multipleAudioTracks() const override { return true; }

	const QString &directory() const { return directory_; }
	void setDirectory(const QString &directory) { directory_ = directory; }
	const QString &filenameFormat() const { return filenameFormat_; }
	void setFilenameFormat(const QString &format) { filenameFormat_ = format; }
	/* One of: mkv, hybrid_mp4, mp4, mov, ts. */
	const QString &container() const { return container_; }
	void setContainer(const QString &container) { container_ = container; }
	const QString &lastFile() const { return lastFile_; }

	static const std::vector<std::pair<QString, QString>> &containers();

	void save(obs_data_t *data) const override;
	void load(obs_data_t *data) override;

protected:
	OBSOutput createOutput(obs_data_t *videoSettings, obs_data_t *audioSettings, QString &error) override;

private:
	QString directory_;
	QString filenameFormat_;
	QString container_ = QStringLiteral("mkv");
	QString lastFile_;
};

class StreamDestination final : public EncodedDestination {
	Q_OBJECT

public:
	StreamDestination(Manager *manager, QString id);
	/* The output does not reference-count its service, so stop it while the service is alive. */
	~StreamDestination() override { forceStop(); }

	QString summary() const override;
	bool multipleAudioTracks() const override { return false; }

	const QString &serviceId() const { return serviceId_; }
	obs_data_t *serviceSettings() const { return serviceSettings_; }
	void setService(const QString &id, obs_data_t *settings);

	/* Registered streaming service ids (custom server first). */
	static std::vector<QString> availableServices();

	void save(obs_data_t *data) const override;
	void load(obs_data_t *data) override;

protected:
	OBSOutput createOutput(obs_data_t *videoSettings, obs_data_t *audioSettings, QString &error) override;
	void releaseOutput() override;

private:
	QString serviceId_ = QStringLiteral("rtmp_custom");
	OBSData serviceSettings_;
	OBSService service_;
};

/*
 * A Blackmagic Design device (DeckLink, UltraStudio, Intensity) driven by OBS's DeckLink output:
 * uncompressed video scaled to the device mode, plus one OBS audio track as 48 kHz stereo.
 */
class DeviceDestination final : public OutputDestination {
	Q_OBJECT

public:
	DeviceDestination(Manager *manager, QString id);

	bool start() override;
	QString summary() const override;
	QString audioSummary() const override;

	/* Settings of OBS's DeckLink output: device_hash, mode_id, keyer, force_sdr, plus display names. */
	obs_data_t *outputSettings() const { return settings_; }
	void setOutputSettings(obs_data_t *settings);
	/* OBS audio mixer track (0-based) sent to the device. */
	int audioTrack() const { return track_; }
	void setAudioTrack(int track);

	/* False when OBS's DeckLink output is not loaded (Blackmagic Desktop Video not installed). */
	static bool available();
	/* OBS's DeckLink output properties without the options of OBS's own DeckLink Output tool. */
	static obs_properties_t *properties();
	static obs_data_t *defaults();

	void save(obs_data_t *data) const override;
	void load(obs_data_t *data) override;

private:
	OBSData settings_;
	int track_ = 0;
};

} // namespace dm
