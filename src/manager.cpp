/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "manager.hpp"

#include "mix.hpp"

#include <plugin-support.h>

#include <QMetaObject>

#include <algorithm>

namespace dm {

namespace {

Manager *g_instance = nullptr;
constexpr int kSaveDelayMs = 750;

template<typename Fn> void invokeOnUi(QObject *context, Fn &&fn)
{
	QMetaObject::invokeMethod(context, std::forward<Fn>(fn), Qt::QueuedConnection);
}

} // namespace

Manager *Manager::instance()
{
	return g_instance;
}

void Manager::create()
{
	if (!g_instance)
		g_instance = new Manager();
}

void Manager::destroy()
{
	delete g_instance;
	g_instance = nullptr;
}

Manager::Manager()
{
	native_ = new NativeMix(this);
	order_.push_back(native_);
	watchMix(native_);

	saveTimer_.setSingleShot(true);
	saveTimer_.setInterval(kSaveDelayMs);
	connect(&saveTimer_, &QTimer::timeout, this, []() { obs_frontend_save(); });

	obs_frontend_add_event_callback(onFrontendEvent, this);
	obs_frontend_add_save_callback(onSave, this);

	signal_handler_t *handler = obs_get_signal_handler();
	videoResetSignal_.Connect(handler, "video_reset", onVideoReset, this);
	sourceRenameSignal_.Connect(handler, "source_rename", onSourceRename, this);

	globalHotkey_ = obs_hotkey_register_frontend("dual_mixer.transition_cued",
						     utf8(T("Hotkey.TransitionCued")).c_str(), onGlobalHotkey, this);
}

Manager::~Manager()
{
	if (globalHotkey_ != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(globalHotkey_);
	videoResetSignal_.Disconnect();
	sourceRenameSignal_.Disconnect();
	obs_frontend_remove_save_callback(onSave, this);
	obs_frontend_remove_event_callback(onFrontendEvent, this);
	clearCollection();
}

/* ------------------------------------------------------------------------- */
/* Mixes */

Mix *Manager::mix(const QString &id) const
{
	for (Mix *mix : order_) {
		if (mix->id() == id)
			return mix;
	}
	return nullptr;
}

QString Manager::uniqueMixName() const
{
	for (int n = 1;; n++) {
		const QString name = T("Mix.DefaultName").arg(n);
		const bool used = std::any_of(order_.begin(), order_.end(), [&](Mix *m) { return m->name() == name; });
		if (!used)
			return name;
	}
}

void Manager::watchMix(Mix *mix)
{
	connect(mix, &Mix::cueChanged, this, &Manager::cueChanged);
	/* Persist user-visible changes without waiting for OBS's own save triggers. */
	for (auto signal : {&Mix::renamed, &Mix::settingsChanged, &Mix::scenesChanged})
		connect(mix, signal, this, &Manager::requestSave);
}

std::vector<Mix *> Manager::cuedMixes() const
{
	std::vector<Mix *> result;
	for (Mix *mix : order_) {
		if (mix->includedInGlobal() && mix->cued())
			result.push_back(mix);
	}
	return result;
}

int Manager::transitionCued()
{
	/* Start every transition in the same event-loop turn so they begin on the same frame. */
	int started = 0;
	QStringList names;
	for (Mix *mix : cuedMixes()) {
		if (mix->transition()) {
			started++;
			names << mix->name();
		}
	}
	if (started > 0)
		obs_log(LOG_INFO, "Global transition: %s", utf8(names.join(QStringLiteral(", "))).c_str());
	return started;
}

void Manager::onGlobalHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *self = static_cast<Manager *>(data);
	invokeOnUi(self, [self]() { self->transitionCued(); });
}

CanvasMix *Manager::addMix()
{
	auto *mix = new CanvasMix(newId(), uniqueMixName(), nullptr, this);

	/* Start with the first scene on Preview so the mix is immediately usable. */
	const std::vector<OBSSource> scenes = mix->scenes();
	if (!scenes.empty())
		mix->setPreviewScene(scenes.front());

	order_.push_back(mix);
	watchMix(mix);
	emit mixAdded(mix);
	requestSave();
	obs_log(LOG_INFO, "Created mix '%s'", utf8(mix->name()).c_str());
	return mix;
}

void Manager::removeMix(const QString &id)
{
	Mix *target = mix(id);
	if (!target || target->isNative())
		return;

	/* Destinations are not owned by the mix: stop them and leave them unassigned. */
	for (Destination *destination : destinationsFor(id)) {
		destination->forceStop();
		destination->setMixId(QString());
	}

	emit mixRemoved(id);
	order_.erase(std::find(order_.begin(), order_.end(), target));
	panels_.erase(id);
	if (focused_ == id) {
		focused_.clear();
		emit workspaceChanged();
	}
	obs_log(LOG_INFO, "Removed mix '%s'", utf8(target->name()).c_str());
	delete target;
	emit mixOrderChanged();
	requestSave();
}

void Manager::moveMix(const QString &id, int delta)
{
	auto it = std::find_if(order_.begin(), order_.end(), [&](Mix *m) { return m->id() == id; });
	if (it == order_.end())
		return;
	const auto index = std::distance(order_.begin(), it);
	const auto target = index + delta;
	if (target < 0 || target >= std::ptrdiff_t(order_.size()))
		return;
	std::swap(order_[size_t(index)], order_[size_t(target)]);
	emit mixOrderChanged();
	requestSave();
}

/* ------------------------------------------------------------------------- */
/* Destinations */

std::vector<Destination *> Manager::destinationsFor(const QString &mixId) const
{
	std::vector<Destination *> result;
	for (Destination *destination : destinations_) {
		if (destination->mixId() == mixId)
			result.push_back(destination);
	}
	return result;
}

Destination *Manager::destination(const QString &id) const
{
	for (Destination *destination : destinations_) {
		if (destination->id() == id)
			return destination;
	}
	return nullptr;
}

bool Manager::mixVideoInUse(const QString &mixId) const
{
	for (Destination *destination : destinations_) {
		if (destination->mixId() == mixId && destination->holdsVideo() && destination->active())
			return true;
	}
	return false;
}

QString Manager::uniqueDestinationName(DestinationType type) const
{
	for (int n = 1;; n++) {
		const QString name = QStringLiteral("%1 %2").arg(destinationTypeName(type)).arg(n);
		const bool used = std::any_of(destinations_.begin(), destinations_.end(),
					      [&](Destination *d) { return d->name() == name; });
		if (!used)
			return name;
	}
}

Destination *Manager::createDestination(DestinationType type, const QString &id)
{
	switch (type) {
	case DestinationType::Display:
		return new DisplayDestination(this, id);
	case DestinationType::Recording:
		return new RecordingDestination(this, id);
	case DestinationType::Stream:
		return new StreamDestination(this, id);
	case DestinationType::Device:
		return new DeviceDestination(this, id);
	}
	return nullptr;
}

void Manager::watchDestination(Destination *destination)
{
	connect(destination, &Destination::changed, this, [this, destination]() {
		/* State changes alone (start/stop) are not configuration changes. */
		if (!destination->active())
			requestSave();
	});
}

Destination *Manager::addDestination(DestinationType type, const QString &mixId)
{
	Destination *destination = createDestination(type, newId());
	destination->setName(uniqueDestinationName(type));
	destination->setMixId(mixId);
	destinations_.push_back(destination);
	watchDestination(destination);
	emit destinationAdded(destination);
	requestSave();
	return destination;
}

void Manager::removeDestination(const QString &id)
{
	Destination *target = destination(id);
	if (!target)
		return;
	target->forceStop();
	emit destinationRemoved(id);
	destinations_.erase(std::find(destinations_.begin(), destinations_.end(), target));
	delete target;
	requestSave();
}

Manager::NativeOutputs Manager::nativeOutputs() const
{
	NativeOutputs outputs;
	outputs.streaming = obs_frontend_streaming_active();
	outputs.recording = obs_frontend_recording_active();
	outputs.recordingPaused = outputs.recording && obs_frontend_recording_paused();
	outputs.virtualCam = obs_frontend_virtualcam_active();
	outputs.replayBuffer = obs_frontend_replay_buffer_active();
	return outputs;
}

/* ------------------------------------------------------------------------- */
/* Workspace state */

Manager::PanelState Manager::panelState(const QString &mixId) const
{
	auto it = panels_.find(mixId);
	return it == panels_.end() ? PanelState() : it->second;
}

void Manager::setPanelState(const QString &mixId, PanelState state)
{
	PanelState &current = panels_[mixId];
	if (current.collapsed == state.collapsed && current.poppedOut == state.poppedOut)
		return;
	current = state;
	emit panelStateChanged(mixId);
	requestSave();
}

void Manager::setColumns(int columns)
{
	columns = std::clamp(columns, 0, 8);
	if (columns == columns_)
		return;
	columns_ = columns;
	emit workspaceChanged();
	requestSave();
}

void Manager::setFocusedMix(const QString &mixId)
{
	if (mixId == focused_)
		return;
	focused_ = mixId;
	emit workspaceChanged();
	requestSave();
}

void Manager::requestSave()
{
	if (loaded_)
		saveTimer_.start();
}

/* ------------------------------------------------------------------------- */
/* Persistence */

void Manager::save(obs_data_t *data) const
{
	OBSDataArrayAutoRelease order = obs_data_array_create();
	OBSDataArrayAutoRelease mixes = obs_data_array_create();
	for (Mix *mix : order_) {
		OBSDataAutoRelease ref = obs_data_create();
		obs_data_set_string(ref, "id", utf8(mix->id()).c_str());
		obs_data_array_push_back(order, ref);

		if (auto *canvasMix = qobject_cast<CanvasMix *>(mix)) {
			OBSDataAutoRelease item = obs_data_create();
			canvasMix->save(item);
			obs_data_array_push_back(mixes, item);
		}
	}
	obs_data_set_array(data, "order", order);
	obs_data_set_array(data, "mixes", mixes);

	OBSDataArrayAutoRelease destinations = obs_data_array_create();
	for (Destination *destination : destinations_) {
		OBSDataAutoRelease item = obs_data_create();
		destination->save(item);
		obs_data_array_push_back(destinations, item);
	}
	obs_data_set_array(data, "destinations", destinations);

	OBSDataAutoRelease workspace = obs_data_create();
	obs_data_set_int(workspace, "columns", columns_);
	obs_data_set_bool(workspace, "native_in_global", native_->includedInGlobal());
	if (globalHotkey_ != OBS_INVALID_HOTKEY_ID) {
		OBSDataArrayAutoRelease bindings = obs_hotkey_save(globalHotkey_);
		obs_data_set_array(workspace, "global_hotkey", bindings);
	}
	obs_data_set_string(workspace, "focus", utf8(focused_).c_str());
	OBSDataArrayAutoRelease panels = obs_data_array_create();
	for (const auto &[id, state] : panels_) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "id", utf8(id).c_str());
		obs_data_set_bool(item, "collapsed", state.collapsed);
		obs_data_set_bool(item, "popped_out", state.poppedOut);
		obs_data_array_push_back(panels, item);
	}
	obs_data_set_array(workspace, "panels", panels);
	obs_data_set_obj(data, "workspace", workspace);
}

void Manager::load(obs_data_t *data)
{
	if (data) {
		OBSDataAutoRelease workspace = obs_data_get_obj(data, "workspace");
		if (workspace) {
			columns_ = int(obs_data_get_int(workspace, "columns"));
			native_->setIncludedFromSave(obs_data_get_bool(workspace, "native_in_global"));
			OBSDataArrayAutoRelease bindings = obs_data_get_array(workspace, "global_hotkey");
			if (bindings && globalHotkey_ != OBS_INVALID_HOTKEY_ID)
				obs_hotkey_load(globalHotkey_, bindings);
			focused_ = qstr(obs_data_get_string(workspace, "focus"));
			OBSDataArrayAutoRelease panels = obs_data_get_array(workspace, "panels");
			for (size_t i = 0, n = obs_data_array_count(panels); i < n; i++) {
				OBSDataAutoRelease item = obs_data_array_item(panels, i);
				PanelState state;
				state.collapsed = obs_data_get_bool(item, "collapsed");
				state.poppedOut = obs_data_get_bool(item, "popped_out");
				panels_[qstr(obs_data_get_string(item, "id"))] = state;
			}
		}

		std::vector<Mix *> loaded;
		OBSDataArrayAutoRelease mixes = obs_data_get_array(data, "mixes");
		for (size_t i = 0, n = obs_data_array_count(mixes); i < n; i++) {
			OBSDataAutoRelease item = obs_data_array_item(mixes, i);
			const QString id = qstr(obs_data_get_string(item, "id"));
			if (id.isEmpty() || id == QString::fromUtf8(kNativeMixId) || mix(id))
				continue;
			auto *canvasMix = new CanvasMix(id, qstr(obs_data_get_string(item, "name")), item, this);
			loaded.push_back(canvasMix);
		}

		/* Rebuild the saved order; anything not mentioned is appended. */
		std::vector<Mix *> ordered;
		OBSDataArrayAutoRelease order = obs_data_get_array(data, "order");
		for (size_t i = 0, n = obs_data_array_count(order); i < n; i++) {
			OBSDataAutoRelease item = obs_data_array_item(order, i);
			const QString id = qstr(obs_data_get_string(item, "id"));
			if (id == native_->id()) {
				ordered.push_back(native_);
				continue;
			}
			auto it = std::find_if(loaded.begin(), loaded.end(), [&](Mix *m) { return m->id() == id; });
			if (it != loaded.end()) {
				ordered.push_back(*it);
				loaded.erase(it);
			}
		}
		if (std::find(ordered.begin(), ordered.end(), native_) == ordered.end())
			ordered.insert(ordered.begin(), native_);
		ordered.insert(ordered.end(), loaded.begin(), loaded.end());
		order_ = ordered;
		for (Mix *mix : order_) {
			if (!mix->isNative())
				watchMix(mix);
		}

		OBSDataArrayAutoRelease destinations = obs_data_get_array(data, "destinations");
		for (size_t i = 0, n = obs_data_array_count(destinations); i < n; i++) {
			OBSDataAutoRelease item = obs_data_array_item(destinations, i);
			const auto type = destinationTypeFromId(obs_data_get_string(item, "type"));
			QString id = qstr(obs_data_get_string(item, "id"));
			if (!type || id.isEmpty() || destination(id))
				continue;
			Destination *created = createDestination(*type, id);
			created->load(item);
			if (!mix(created->mixId()))
				created->setMixId(QString());
			destinations_.push_back(created);
			watchDestination(created);
		}
	}

	if (!focused_.isEmpty() && !mix(focused_))
		focused_.clear();

	native_->refreshFrontendState();
	loaded_ = true;
	obs_log(LOG_INFO, "Loaded %zu mix(es) and %zu destination(s)", order_.size() - 1, destinations_.size());
	emit collectionLoaded();

	/* Physical displays come back the way they were left, like OBS's own projectors. */
	for (Destination *destination : destinations_) {
		auto *display = qobject_cast<DisplayDestination *>(destination);
		if (display && display->reopenOnLoad() && display->wasOpen())
			display->start();
	}
}

void Manager::clearCollection()
{
	saveTimer_.stop();
	loaded_ = false;

	for (Destination *destination : destinations_)
		destination->forceStop();
	while (!destinations_.empty()) {
		Destination *destination = destinations_.back();
		destinations_.pop_back();
		emit destinationRemoved(destination->id());
		delete destination;
	}

	std::vector<Mix *> mixes = order_;
	for (Mix *mix : mixes) {
		if (mix->isNative())
			continue;
		emit mixRemoved(mix->id());
		order_.erase(std::find(order_.begin(), order_.end(), mix));
		delete mix;
	}

	order_ = {native_};
	native_->setIncludedFromSave(false);
	panels_.clear();
	columns_ = 0;
	focused_.clear();
	emit mixOrderChanged();
	emit workspaceChanged();
}

/* ------------------------------------------------------------------------- */
/* OBS callbacks */

void Manager::handleEvent(int event)
{
	native_->handleFrontendEvent(event);

	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		for (Mix *mix : order_) {
			if (auto *canvasMix = qobject_cast<CanvasMix *>(mix))
				canvasMix->handleScenesChanged();
		}
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_LIST_CHANGED:
		for (Mix *mix : order_) {
			if (auto *canvasMix = qobject_cast<CanvasMix *>(mix))
				canvasMix->handleTransitionsChanged();
		}
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_DURATION_CHANGED:
		/* The global transition changed; mixes following it show the new one. */
		for (Mix *mix : order_) {
			if (!mix->isNative())
				emit mix->transitionsChanged();
		}
		emit transitionSettingsChanged();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
	case OBS_FRONTEND_EVENT_RECORDING_STARTING:
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTING:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPING:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		emit nativeOutputsChanged();
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
		emit shuttingDown();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		clearCollection();
		break;
	default:
		break;
	}
}

void Manager::onFrontendEvent(enum obs_frontend_event event, void *data)
{
	static_cast<Manager *>(data)->handleEvent(int(event));
}

void Manager::onSave(obs_data_t *data, bool saving, void *param)
{
	auto *self = static_cast<Manager *>(param);
	if (saving) {
		OBSDataAutoRelease obj = obs_data_create();
		self->save(obj);
		obs_data_set_obj(data, kSaveKey, obj);
		return;
	}

	self->clearCollection();
	OBSDataAutoRelease obj = obs_data_get_obj(data, kSaveKey);
	self->load(obj);
}

void Manager::onVideoReset(void *data, calldata_t *)
{
	auto *self = static_cast<Manager *>(data);
	invokeOnUi(self, [self]() {
		self->native_->refreshVideoInfo();
		for (Mix *mix : self->order_) {
			if (auto *canvasMix = qobject_cast<CanvasMix *>(mix))
				canvasMix->handleVideoReset();
		}
	});
}

void Manager::onSourceRename(void *data, calldata_t *cd)
{
	obs_source_t *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source || !obs_source_is_scene(source))
		return;
	auto *self = static_cast<Manager *>(data);
	invokeOnUi(self, [self]() {
		for (Mix *mix : self->order_)
			emit mix->scenesChanged();
	});
}

} // namespace dm
