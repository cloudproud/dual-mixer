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
#include "destination.hpp"

#include <obs-frontend-api.h>

#include <QObject>
#include <QString>
#include <QTimer>

#include <map>
#include <vector>

namespace dm {

class Mix;
class NativeMix;
class CanvasMix;

/*
 * Owns all mixes (the native OBS main mix plus any number of plugin mixes) and all plugin-managed
 * destinations, persists them inside the scene collection, and relays OBS frontend events.
 */
class Manager final : public QObject {
	Q_OBJECT

public:
	struct PanelState {
		bool collapsed = false;
		bool poppedOut = false;
	};

	struct NativeOutputs {
		bool streaming = false;
		bool recording = false;
		bool recordingPaused = false;
		bool virtualCam = false;
		bool replayBuffer = false;
	};

	static Manager *instance();
	static void create();
	static void destroy();

	/* Mixes in user-defined order. The native mix is always present. */
	const std::vector<Mix *> &mixes() const { return order_; }
	Mix *mix(const QString &id) const;
	NativeMix *nativeMix() const { return native_; }
	CanvasMix *addMix();
	void removeMix(const QString &id);
	void moveMix(const QString &id, int delta);

	const std::vector<Destination *> &destinations() const { return destinations_; }
	std::vector<Destination *> destinationsFor(const QString &mixId) const;
	Destination *destination(const QString &id) const;
	Destination *addDestination(DestinationType type, const QString &mixId);
	void removeDestination(const QString &id);
	/* True if any active destination of the mix has an output bound to its video. */
	bool mixVideoInUse(const QString &mixId) const;

	NativeOutputs nativeOutputs() const;

	PanelState panelState(const QString &mixId) const;
	void setPanelState(const QString &mixId, PanelState state);
	/* 0 = automatic. */
	int columns() const { return columns_; }
	void setColumns(int columns);
	const QString &focusedMix() const { return focused_; }
	void setFocusedMix(const QString &mixId);

	/* Mixes that the global switch would take to Program right now, in workspace order. */
	std::vector<Mix *> cuedMixes() const;
	/* Global switch: transitions every cued mix that is included, at the same time. */
	int transitionCued();

	/* Schedules a save of the scene collection (debounced). */
	void requestSave();

signals:
	void mixAdded(dm::Mix *mix);
	void mixRemoved(const QString &id);
	void mixOrderChanged();
	void destinationAdded(dm::Destination *destination);
	void destinationRemoved(const QString &id);
	void nativeOutputsChanged();
	void workspaceChanged();
	void panelStateChanged(const QString &mixId);
	/* The set of cued mixes may have changed. */
	void cueChanged();
	/* OBS's selected transition or its duration (the global transition) changed. */
	void transitionSettingsChanged();
	void collectionLoaded();
	/* OBS is shutting down; release displays before the graphics subsystem goes away. */
	void shuttingDown();

private:
	Manager();
	~Manager() override;

	void handleEvent(int event);
	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
	void clearCollection();
	void watchMix(Mix *mix);
	void watchDestination(Destination *destination);
	QString uniqueMixName() const;
	QString uniqueDestinationName(DestinationType type) const;
	Destination *createDestination(DestinationType type, const QString &id);

	static void onFrontendEvent(enum obs_frontend_event event, void *data);
	static void onSave(obs_data_t *data, bool saving, void *param);
	static void onVideoReset(void *data, calldata_t *cd);
	static void onSourceRename(void *data, calldata_t *cd);
	static void onGlobalHotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);

	NativeMix *native_ = nullptr;
	std::vector<Mix *> order_;
	std::vector<Destination *> destinations_;
	std::map<QString, PanelState> panels_;
	int columns_ = 0;
	QString focused_;
	bool loaded_ = false;
	QTimer saveTimer_;
	obs_hotkey_id globalHotkey_ = OBS_INVALID_HOTKEY_ID;

	OBSSignal videoResetSignal_;
	OBSSignal sourceRenameSignal_;
};

} // namespace dm
