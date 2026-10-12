/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "mix.hpp"

#include <plugin-support.h>

#include <obs-frontend-api.h>
#include <graphics/matrix4.h>

#include <QMetaObject>

#include <algorithm>
#include <cstring>

namespace dm {

namespace {

constexpr int kDefaultDurationMs = 300;
constexpr int kTBarClamp = kTBarMax * 2 / 100;

/* Draws a scene aspect-fitted into a (cx, cy) area, matching OBS_TRANSITION_SCALE_ASPECT. */
void renderSceneFitted(obs_source_t *scene, uint32_t cx, uint32_t cy)
{
	const uint32_t sceneCX = obs_source_get_width(scene);
	const uint32_t sceneCY = obs_source_get_height(scene);
	if (!sceneCX || !sceneCY || !cx || !cy)
		return;

	int x, y, w, h;
	fitInside(sceneCX, sceneCY, cx, cy, x, y, w, h);

	gs_matrix_push();
	gs_matrix_translate3f(float(x), float(y), 0.0f);
	gs_matrix_scale3f(float(w) / float(sceneCX), float(h) / float(sceneCY), 1.0f);
	obs_source_video_render(scene);
	gs_matrix_pop();
}

template<typename Fn> void invokeOnUi(QObject *context, Fn &&fn)
{
	QMetaObject::invokeMethod(context, std::forward<Fn>(fn), Qt::QueuedConnection);
}

void saveHotkey(obs_data_t *data, const char *key, obs_hotkey_id id)
{
	if (id == OBS_INVALID_HOTKEY_ID)
		return;
	OBSDataArrayAutoRelease bindings = obs_hotkey_save(id);
	obs_data_set_array(data, key, bindings);
}

void loadHotkey(obs_data_t *data, const char *key, obs_hotkey_id id)
{
	if (!data || id == OBS_INVALID_HOTKEY_ID)
		return;
	OBSDataArrayAutoRelease bindings = obs_data_get_array(data, key);
	if (bindings)
		obs_hotkey_load(id, bindings);
}

bool isLiveTransition(obs_source_t *source)
{
	return source && !obs_source_removed(source) && obs_source_get_type(source) == OBS_SOURCE_TYPE_TRANSITION;
}

} // namespace

std::vector<OBSSource> frontendTransitions()
{
	std::vector<OBSSource> result;
	obs_frontend_source_list list = {};
	obs_frontend_get_transitions(&list);
	for (size_t i = 0; i < list.sources.num; i++)
		result.emplace_back(list.sources.array[i]);
	obs_frontend_source_list_free(&list);
	return result;
}

/* ------------------------------------------------------------------------- */
/* Mix */

void Mix::setIncludedInGlobal(bool included)
{
	if (included == includedInGlobal_)
		return;
	includedInGlobal_ = included;
	emit cueChanged();
	emit settingsChanged();
}

void Mix::previewRelative(int delta)
{
	const std::vector<OBSSource> list = scenes();
	if (list.empty())
		return;

	const OBSSource current = previewScene();
	auto it = std::find_if(list.begin(), list.end(), [&](const OBSSource &s) { return s.Get() == current.Get(); });
	const int count = int(list.size());
	int index = it == list.end() ? (delta > 0 ? -1 : 0) : int(it - list.begin());
	index = ((index + delta) % count + count) % count;
	setPreviewScene(list[size_t(index)]);
}

/* ------------------------------------------------------------------------- */
/* NativeMix */

NativeMix::NativeMix(QObject *parent) : Mix(QString::fromUtf8(kNativeMixId), parent)
{
	/* The native mix only joins the global switch when the user opts in. */
	initIncludedInGlobal(false);
	/* Created during module load: the frontend UI is not built yet, so only query libobs here. */
	refreshVideoInfo();
}

QString NativeMix::name() const
{
	return T("Mix.NativeName");
}

video_t *NativeMix::video() const
{
	return obs_get_video();
}

void NativeMix::refreshVideoInfo()
{
	obs_video_info ovi;
	if (mainVideoInfo(ovi)) {
		width_ = ovi.base_width;
		height_ = ovi.base_height;
	}
}

void NativeMix::refreshFrontendState()
{
	studioMode_ = obs_frontend_preview_program_mode_active();
	/* Loading or toggling Studio Mode sets a Preview, but that is not the user cueing a switch. */
	pending_ = false;
	refreshPreviewCache();
	emit settingsChanged();
	emit previewChanged();
	emit cueChanged();
}

void NativeMix::renderProgram()
{
	obs_render_main_texture();
}

void NativeMix::renderPreview()
{
	if (!studioMode_) {
		obs_render_main_texture();
		return;
	}

	OBSSource scene;
	{
		std::lock_guard lock(previewMutex_);
		scene = OBSGetStrongRef(preview_);
	}
	if (scene)
		renderSceneFitted(scene, width_, height_);
}

std::vector<OBSSource> NativeMix::scenes() const
{
	return frontendScenes();
}

OBSSource NativeMix::previewScene() const
{
	OBSSourceAutoRelease scene = studioMode_ ? obs_frontend_get_current_preview_scene()
						 : obs_frontend_get_current_scene();
	return scene.Get();
}

OBSSource NativeMix::programScene() const
{
	OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
	return scene.Get();
}

void NativeMix::setPreviewScene(obs_source_t *scene)
{
	if (!scene)
		return;
	if (studioMode_)
		obs_frontend_set_current_preview_scene(scene);
	else
		obs_frontend_set_current_scene(scene);
}

OBSSource NativeMix::effectiveTransition() const
{
	OBSSourceAutoRelease current = obs_frontend_get_current_transition();
	return current.Get();
}

void NativeMix::setTransitionOverride(obs_source_t *transition)
{
	if (transition)
		obs_frontend_set_current_transition(transition);
}

int NativeMix::transitionDuration() const
{
	return obs_frontend_get_transition_duration();
}

void NativeMix::setTransitionDuration(int ms)
{
	obs_frontend_set_transition_duration(ms);
}

bool NativeMix::transitioning() const
{
	OBSSourceAutoRelease transition = obs_get_output_source(0);
	if (!transition || obs_source_get_type(transition) != OBS_SOURCE_TYPE_TRANSITION)
		return false;
	const float t = obs_transition_get_time(transition);
	return t > 0.0f && t < 1.0f;
}

bool NativeMix::transition()
{
	if (!studioMode_)
		return false;
	obs_frontend_preview_program_trigger_transition();
	setPending(false);
	return true;
}

void NativeMix::setTBar(int position)
{
	if (obs_frontend_preview_program_mode_active())
		obs_frontend_set_tbar_position(std::clamp(position, 0, kTBarMax));
}

void NativeMix::releaseTBar()
{
	if (obs_frontend_preview_program_mode_active())
		obs_frontend_release_tbar();
}

int NativeMix::tbarPosition() const
{
	/* OBS only creates its T-bar widget in Studio Mode; querying it otherwise is unsafe. */
	return obs_frontend_preview_program_mode_active() ? obs_frontend_get_tbar_position() : 0;
}

bool NativeMix::cued() const
{
	if (!studioMode_ || !pending_)
		return false;
	const OBSSource preview = previewScene();
	return preview && preview.Get() != programScene().Get();
}

void NativeMix::setPending(bool pending)
{
	if (pending == pending_)
		return;
	pending_ = pending;
	emit cueChanged();
}

void NativeMix::setStudioMode(bool enabled)
{
	obs_frontend_set_preview_program_mode(enabled);
}

void NativeMix::refreshPreviewCache()
{
	OBSSource scene = previewScene();
	std::lock_guard lock(previewMutex_);
	preview_ = OBSGetWeakRef(scene);
}

void NativeMix::handleFrontendEvent(int event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		refreshPreviewCache();
		emit programChanged();
		if (!studioMode_)
			emit previewChanged();
		emit cueChanged();
		emit transitionStateChanged();
		break;
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED: {
		refreshPreviewCache();
		/* Swap-scenes mode changes Preview right before TRANSITION_STOPPED, which clears it again. */
		const OBSSource preview = previewScene();
		setPending(preview && preview.Get() != programScene().Get());
		emit previewChanged();
		emit cueChanged();
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		refreshFrontendState();
		emit scenesChanged();
		emit programChanged();
		emit transitionsChanged();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_DURATION_CHANGED:
		emit transitionsChanged();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_STOPPED:
		setPending(false);
		emit transitionStateChanged();
		break;
	case OBS_FRONTEND_EVENT_TBAR_VALUE_CHANGED:
		emit transitionStateChanged();
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		refreshFrontendState();
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* CanvasMix */

CanvasMix::CanvasMix(QString id, QString name, obs_data_t *data, QObject *parent)
	: Mix(std::move(id), parent),
	  name_(std::move(name))
{
	OBSSourceAutoRelease cut =
		obs_source_create_private("cut_transition", utf8(T("Transition.Cut")).c_str(), nullptr);
	cut_ = cut.Get();
	if (cut_)
		cutStopSignal_.Connect(obs_source_get_signal_handler(cut_), "transition_stop", onTransitionStop, this);

	if (data) {
		load(data);
		return;
	}

	createCanvas();
	activate(cut_);
	registerHotkeys();
	addDefaultQuickTransitions();
}

CanvasMix::~CanvasMix()
{
	unregisterHotkeys();
	cutStopSignal_.Disconnect();
	for (auto &[uuid, copy] : copies_) {
		copy->stopSignal.Disconnect();
		copy->originUpdateSignal.Disconnect();
	}

	OBSSource preview;
	{
		std::lock_guard lock(sceneMutex_);
		preview = OBSGetStrongRef(preview_);
		preview_ = nullptr;
		program_ = nullptr;
	}
	if (preview)
		obs_source_dec_showing(preview);

	OBSCanvas canvas;
	{
		std::lock_guard lock(canvasMutex_);
		canvas = std::move(canvas_);
	}
	if (canvas) {
		obs_canvas_set_channel(canvas, 0, nullptr);
		obs_canvas_remove(canvas);
	}

	obs_transition_clear(cut_);
	for (auto &[uuid, copy] : copies_)
		obs_transition_clear(copy->source);
	copies_.clear();
	active_ = nullptr;
}

void CanvasMix::setName(const QString &name)
{
	if (name.isEmpty() || name == name_)
		return;
	name_ = name;
	{
		std::lock_guard lock(canvasMutex_);
		if (canvas_)
			obs_canvas_set_name(canvas_, utf8(name_).c_str());
	}
	updateHotkeyDescriptions();
	emit renamed();
}

video_t *CanvasMix::video() const
{
	std::lock_guard lock(canvasMutex_);
	return canvas_ ? obs_canvas_get_video(canvas_) : nullptr;
}

void CanvasMix::createCanvas()
{
	obs_video_info ovi;
	if (!mainVideoInfo(ovi)) {
		obs_log(LOG_WARNING, "Mix '%s': OBS video is not initialised; canvas not created", utf8(name_).c_str());
		return;
	}

	const uint32_t cx = followMain_ ? ovi.base_width : customWidth_;
	const uint32_t cy = followMain_ ? ovi.base_height : customHeight_;
	ovi.base_width = ovi.output_width = cx;
	ovi.base_height = ovi.output_height = cy;

	/* ACTIVATE: sources on this Program behave as "on air" (media restarts, tally).
	 * EPHEMERAL + private: the plugin owns persistence; OBS must not save or list this canvas. */
	uint32_t flags = ACTIVATE | EPHEMERAL;
	if (mixAudio_)
		flags |= MIX_AUDIO;

	OBSCanvasAutoRelease created = obs_canvas_create_private(utf8(name_).c_str(), &ovi, flags);
	if (!created) {
		obs_log(LOG_ERROR, "Mix '%s': failed to create canvas %ux%u", utf8(name_).c_str(), cx, cy);
		return;
	}

	width_ = cx;
	height_ = cy;
	applyTransitionSize(cut_);
	for (auto &[uuid, copy] : copies_)
		applyTransitionSize(copy->source);

	/* Attach the Program transition to the new canvas before detaching it from the old one so
	 * the scenes on Program never see a deactivate/activate cycle. */
	if (active_)
		obs_canvas_set_channel(created, 0, active_);

	OBSCanvas previous;
	{
		std::lock_guard lock(canvasMutex_);
		previous = std::move(canvas_);
		canvas_ = created.Get();
	}
	if (previous) {
		obs_canvas_set_channel(previous, 0, nullptr);
		obs_canvas_remove(previous);
	}

	obs_log(LOG_INFO, "Mix '%s': canvas %ux%u%s", utf8(name_).c_str(), cx, cy, mixAudio_ ? " (mixes audio)" : "");
	emit videoReset();
}

void CanvasMix::applyTransitionSize(obs_source_t *transition) const
{
	if (!transition)
		return;
	obs_transition_set_size(transition, width_, height_);
	obs_transition_set_scale_type(transition, OBS_TRANSITION_SCALE_ASPECT);
	obs_transition_set_alignment(transition, OBS_ALIGN_CENTER);
}

void CanvasMix::renderProgram()
{
	std::lock_guard lock(canvasMutex_);
	if (canvas_)
		obs_render_canvas_texture(canvas_);
}

void CanvasMix::renderPreview()
{
	OBSSource scene;
	{
		std::lock_guard lock(sceneMutex_);
		scene = OBSGetStrongRef(preview_);
	}
	if (scene)
		renderSceneFitted(scene, width_, height_);
}

std::vector<OBSSource> CanvasMix::scenes() const
{
	if (!useSceneFilter_)
		return frontendScenes();

	std::vector<OBSSource> result;
	for (const std::string &uuid : sceneFilter_) {
		OBSSource scene = sceneByUuid(uuid.c_str());
		if (scene)
			result.push_back(scene);
	}
	return result;
}

OBSSource CanvasMix::previewScene() const
{
	std::lock_guard lock(sceneMutex_);
	return OBSGetStrongRef(preview_);
}

OBSSource CanvasMix::programScene() const
{
	std::lock_guard lock(sceneMutex_);
	return OBSGetStrongRef(program_);
}

void CanvasMix::setPreviewScene(obs_source_t *scene)
{
	setPreviewInternal(scene, true);
}

void CanvasMix::setPreviewInternal(obs_source_t *scene, bool userCue)
{
	OBSSource previous;
	bool changed = false;
	{
		std::lock_guard lock(sceneMutex_);
		previous = OBSGetStrongRef(preview_);
		if (previous.Get() != scene) {
			preview_ = OBSGetWeakRef(scene);
			changed = true;
		}
		if (userCue)
			pending_ = scene && scene != OBSGetStrongRef(program_).Get();
	}

	if (changed) {
		/* Like OBS's own Studio Mode preview: the scene is shown, but not "active" until on Program. */
		if (scene)
			obs_source_inc_showing(scene);
		if (previous)
			obs_source_dec_showing(previous);
		emit previewChanged();
	}
	emit cueChanged();
}

void CanvasMix::setProgramInternal(obs_source_t *scene)
{
	{
		std::lock_guard lock(sceneMutex_);
		program_ = OBSGetWeakRef(scene);
	}
	emit programChanged();
}

bool CanvasMix::cued() const
{
	std::lock_guard lock(sceneMutex_);
	if (!pending_)
		return false;
	const OBSSource preview = OBSGetStrongRef(preview_);
	return preview && preview.Get() != OBSGetStrongRef(program_).Get();
}

/* ------------------------------------------------------------------------- */
/* Transitions */

OBSSource CanvasMix::resolveTransition(const std::string &uuid, const std::string &name)
{
	if (uuid.empty())
		return nullptr;
	OBSSourceAutoRelease byUuid = obs_get_source_by_uuid(uuid.c_str());
	if (isLiveTransition(byUuid))
		return byUuid.Get();
	/* OBS recreated the transition (e.g. collection re-import): fall back to its name. */
	for (const OBSSource &transition : frontendTransitions()) {
		if (name == obs_source_get_name(transition))
			return transition;
	}
	return nullptr;
}

OBSSource CanvasMix::transitionOverride() const
{
	return resolveTransition(overrideUuid_, overrideName_);
}

OBSSource CanvasMix::quickTransitionSource(size_t index) const
{
	if (index >= quickTransitions_.size())
		return nullptr;
	return resolveTransition(quickTransitions_[index].uuid, quickTransitions_[index].name);
}

QString CanvasMix::quickTransitionLabel(size_t index) const
{
	if (index >= quickTransitions_.size())
		return QString();
	const QuickTransition &quick = quickTransitions_[index];
	OBSSource source = quickTransitionSource(index);
	QString label = quick.fadeToBlack ? T("Transition.FadeToBlack") : qstr(quick.name.c_str());
	if (source && !obs_transition_fixed(source))
		label += QStringLiteral(" (%1ms)").arg(quick.durationMs);
	return label;
}

void CanvasMix::addQuickTransition(obs_source_t *transition, int durationMs, bool fadeToBlack)
{
	if (!transition)
		return;
	QuickTransition quick;
	quick.id = nextQuickId_++;
	quick.uuid = obs_source_get_uuid(transition);
	quick.name = obs_source_get_name(transition);
	quick.durationMs = std::max(durationMs, 50);
	quick.fadeToBlack = fadeToBlack;
	quickTransitions_.push_back(std::move(quick));
	registerQuickHotkey(quickTransitions_.back());
	emit transitionsChanged();
	emit settingsChanged();
}

void CanvasMix::changeQuickTransition(size_t index, obs_source_t *transition, bool fadeToBlack)
{
	if (index >= quickTransitions_.size() || !transition)
		return;
	QuickTransition &quick = quickTransitions_[index];
	quick.uuid = obs_source_get_uuid(transition);
	quick.name = obs_source_get_name(transition);
	quick.fadeToBlack = fadeToBlack;
	updateHotkeyDescriptions();
	emit transitionsChanged();
	emit settingsChanged();
}

void CanvasMix::setQuickTransitionDuration(size_t index, int durationMs)
{
	if (index >= quickTransitions_.size())
		return;
	quickTransitions_[index].durationMs = std::max(durationMs, 50);
	updateHotkeyDescriptions();
	emit transitionsChanged();
	emit settingsChanged();
}

void CanvasMix::removeQuickTransition(size_t index)
{
	if (index >= quickTransitions_.size())
		return;
	if (quickTransitions_[index].hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(quickTransitions_[index].hotkey);
	quickTransitions_.erase(quickTransitions_.begin() + std::ptrdiff_t(index));
	emit transitionsChanged();
	emit settingsChanged();
}

bool CanvasMix::quickTransition(size_t index)
{
	OBSSource origin = quickTransitionSource(index);
	if (!origin)
		return false;
	const QuickTransition &quick = quickTransitions_[index];
	if (quick.fadeToBlack)
		return toggleFadeToBlack(playbackFor(origin), quick.durationMs);
	return startTransition(playbackFor(origin), quick.durationMs);
}

bool CanvasMix::fadedToBlack() const
{
	return OBSGetStrongRef(blackFrom_) != nullptr;
}

bool CanvasMix::toggleFadeToBlack(obs_source_t *playback, int durationMs)
{
	if (!playback || transitioning())
		return false;

	OBSSource restore = OBSGetStrongRef(blackFrom_);
	activate(playback);
	if (!restore) {
		OBSSource current = programScene();
		if (!current ||
		    !obs_transition_start(playback, OBS_TRANSITION_MODE_AUTO, uint32_t(durationMs), nullptr))
			return false;
		blackFrom_ = OBSGetWeakRef(current);
		setProgramInternal(nullptr);
	} else {
		if (!obs_transition_start(playback, OBS_TRANSITION_MODE_AUTO, uint32_t(durationMs), restore))
			return false;
		blackFrom_ = nullptr;
		setProgramInternal(restore);
	}
	emit cueChanged();
	emit transitionStateChanged();
	return true;
}

void CanvasMix::addDefaultQuickTransitions()
{
	/* OBS's own defaults: Cut, Fade and Fade to Black. */
	OBSSource cut, fade;
	for (const OBSSource &transition : frontendTransitions()) {
		const char *type = obs_source_get_id(transition);
		if (!cut && strcmp(type, "cut_transition") == 0)
			cut = transition;
		else if (!fade && strcmp(type, "fade_transition") == 0)
			fade = transition;
	}
	if (cut)
		addQuickTransition(cut, kDefaultDurationMs, false);
	if (fade) {
		addQuickTransition(fade, kDefaultDurationMs, false);
		addQuickTransition(fade, kDefaultDurationMs, true);
	}
}

void CanvasMix::setTransitionOverride(obs_source_t *transition)
{
	if (transition) {
		overrideUuid_ = obs_source_get_uuid(transition);
		overrideName_ = obs_source_get_name(transition);
	} else {
		overrideUuid_.clear();
		overrideName_.clear();
	}
	emit transitionsChanged();
	emit settingsChanged();
}

OBSSource CanvasMix::effectiveTransition() const
{
	if (OBSSource override = transitionOverride())
		return override;
	OBSSourceAutoRelease current = obs_frontend_get_current_transition();
	return current.Get();
}

int CanvasMix::transitionDuration() const
{
	return transitionOverride() ? overrideDurationMs_ : obs_frontend_get_transition_duration();
}

void CanvasMix::setTransitionDuration(int ms)
{
	/* The global duration belongs to OBS; a mix only owns the duration of its override. */
	if (!transitionOverride())
		return;
	overrideDurationMs_ = std::max(ms, 50);
	emit settingsChanged();
}

obs_source_t *CanvasMix::playbackFor(obs_source_t *origin)
{
	if (!origin || strcmp(obs_source_get_id(origin), "cut_transition") == 0)
		return cut_;

	const std::string uuid = obs_source_get_uuid(origin);
	auto it = copies_.find(uuid);
	if (it != copies_.end()) {
		if (it->second->dirty && it->second->source != active_)
			syncCopy(*it->second);
		return it->second->source;
	}

	/* A private copy per mix: OBS transitions track their own progress and cannot be shared
	 * between canvases. Created from scratch rather than duplicated (stingers need this). */
	OBSDataAutoRelease settings = obs_source_get_settings(origin);
	OBSSourceAutoRelease source =
		obs_source_create_private(obs_source_get_id(origin), obs_source_get_name(origin), settings);
	if (!source)
		return cut_;

	auto copy = std::make_unique<Copy>();
	copy->origin = OBSGetWeakRef(origin);
	copy->source = source.Get();
	applyTransitionSize(copy->source);
	copy->stopSignal.Connect(obs_source_get_signal_handler(copy->source), "transition_stop", onTransitionStop,
				 this);
	copy->originUpdateSignal.Connect(obs_source_get_signal_handler(origin), "update", onOriginUpdate, this);
	obs_source_t *playback = copy->source;
	copies_.emplace(uuid, std::move(copy));
	return playback;
}

void CanvasMix::syncCopy(Copy &copy)
{
	OBSSource origin = OBSGetStrongRef(copy.origin);
	copy.dirty = false;
	if (!origin)
		return;
	OBSDataAutoRelease settings = obs_source_get_settings(origin);
	obs_source_reset_settings(copy.source, settings);
}

void CanvasMix::onOriginUpdate(void *data, calldata_t *cd)
{
	auto *mix = static_cast<CanvasMix *>(data);
	obs_source_t *origin = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!origin)
		return;
	const std::string uuid = obs_source_get_uuid(origin);
	invokeOnUi(mix, [mix, uuid]() {
		auto it = mix->copies_.find(uuid);
		if (it == mix->copies_.end())
			return;
		/* Apply right away unless this copy is mid-transition; then on its next use. */
		it->second->dirty = true;
		if (it->second->source != mix->active_ || !mix->transitioning())
			mix->syncCopy(*it->second);
	});
}

void CanvasMix::handleTransitionsChanged()
{
	for (auto it = copies_.begin(); it != copies_.end();) {
		OBSSource origin = OBSGetStrongRef(it->second->origin);
		if ((!origin || obs_source_removed(origin)) && it->second->source != active_) {
			it->second->stopSignal.Disconnect();
			it->second->originUpdateSignal.Disconnect();
			obs_transition_clear(it->second->source);
			it = copies_.erase(it);
		} else {
			++it;
		}
	}
	emit transitionsChanged();
}

void CanvasMix::activate(obs_source_t *playback)
{
	if (!playback || playback == active_)
		return;

	OBSCanvas canvas;
	{
		std::lock_guard lock(canvasMutex_);
		canvas = canvas_;
	}

	if (active_) {
		obs_transition_swap_begin(playback, active_);
		if (canvas)
			obs_canvas_set_channel(canvas, 0, playback);
		obs_transition_swap_end(playback, active_);
	} else {
		obs_transition_set(playback, programScene());
		if (canvas)
			obs_canvas_set_channel(canvas, 0, playback);
	}
	active_ = playback;
}

bool CanvasMix::transitioning() const
{
	if (tbarActive_)
		return true;
	if (!active_)
		return false;
	const float t = obs_transition_get_time(active_);
	return t > 0.0f && t < 1.0f;
}

bool CanvasMix::transition()
{
	return startTransition(playbackFor(effectiveTransition()), transitionDuration());
}

bool CanvasMix::cut()
{
	return startTransition(cut_, 0);
}

bool CanvasMix::startTransition(obs_source_t *playback, int durationMs)
{
	if (!playback || transitioning())
		return false;

	OBSSource target = previewScene();
	if (!target)
		return false;
	OBSSource previous = programScene();

	activate(playback);
	if (!obs_transition_start(playback, OBS_TRANSITION_MODE_AUTO, uint32_t(std::max(durationMs, 0)), target))
		return false;

	blackFrom_ = nullptr;
	setProgramInternal(target);
	{
		std::lock_guard lock(sceneMutex_);
		pending_ = false;
	}
	if (swapAfterTransition_ && previous)
		setPreviewInternal(previous, false);
	emit cueChanged();
	emit transitionStateChanged();
	return true;
}

void CanvasMix::setTBar(int position)
{
	position = std::clamp(position, 0, kTBarMax);

	if (!tbarActive_) {
		obs_source_t *playback = playbackFor(effectiveTransition());
		if (position <= 0 || transitioning() || !playback || playback == cut_)
			return;
		OBSSource target = previewScene();
		if (!target)
			return;
		activate(playback);
		obs_transition_set_manual_torque(playback, 8.0f, 0.05f);
		if (!obs_transition_start(playback, OBS_TRANSITION_MODE_MANUAL, 0, target))
			return;
		tbarActive_ = true;
	}

	tbarPosition_ = position;
	obs_transition_set_manual_time(active_, float(position) / float(kTBarMax));
	if (position >= kTBarMax)
		finishManualTransition();
	emit transitionStateChanged();
}

void CanvasMix::releaseTBar()
{
	if (!tbarActive_)
		return;
	if (tbarPosition_ >= kTBarMax - kTBarClamp) {
		obs_transition_set_manual_time(active_, 1.0f);
		finishManualTransition();
	} else if (tbarPosition_ <= kTBarClamp) {
		obs_transition_set_manual_time(active_, 0.0f);
		tbarActive_ = false;
		tbarPosition_ = 0;
	}
	emit transitionStateChanged();
}

void CanvasMix::finishManualTransition()
{
	tbarActive_ = false;
	tbarPosition_ = 0;
	OBSSource target = previewScene();
	OBSSource previous = programScene();
	blackFrom_ = nullptr;
	setProgramInternal(target);
	{
		std::lock_guard lock(sceneMutex_);
		pending_ = false;
	}
	if (swapAfterTransition_ && previous)
		setPreviewInternal(previous, false);
	emit cueChanged();
}

void CanvasMix::onTransitionStop(void *data, calldata_t *)
{
	auto *mix = static_cast<CanvasMix *>(data);
	invokeOnUi(mix, [mix]() { emit mix->transitionStateChanged(); });
}

/* ------------------------------------------------------------------------- */
/* Settings */

void CanvasMix::setResolution(bool followMain, uint32_t cx, uint32_t cy)
{
	followMain_ = followMain;
	if (cx >= 2 && cy >= 2) {
		customWidth_ = cx & ~1u;
		customHeight_ = cy & ~1u;
	}
	createCanvas();
	emit settingsChanged();
}

void CanvasMix::setMixesAudio(bool enabled)
{
	if (mixAudio_ == enabled)
		return;
	mixAudio_ = enabled;
	createCanvas();
	emit settingsChanged();
}

void CanvasMix::setSwapAfterTransition(bool enabled)
{
	swapAfterTransition_ = enabled;
	emit settingsChanged();
}

void CanvasMix::setDoubleClickTransitions(bool enabled)
{
	doubleClickTransitions_ = enabled;
	emit settingsChanged();
}

void CanvasMix::setSceneFilter(bool enabled, std::vector<std::string> uuids)
{
	useSceneFilter_ = enabled;
	sceneFilter_ = std::move(uuids);
	emit scenesChanged();
}

void CanvasMix::handleVideoReset()
{
	createCanvas();
}

void CanvasMix::handleScenesChanged()
{
	OBSSource preview = previewScene();
	if (preview && obs_source_removed(preview))
		setPreviewInternal(nullptr, true);

	OBSSource program = programScene();
	if (program && obs_source_removed(program)) {
		if (active_)
			obs_transition_set(active_, nullptr);
		setProgramInternal(nullptr);
	}
	emit scenesChanged();
}

/* ------------------------------------------------------------------------- */
/* Hotkeys */

namespace {

std::string hotkeyName(const QString &mixId, const char *action)
{
	return "dual_mixer." + utf8(mixId) + "." + action;
}

template<typename Action>
obs_hotkey_id registerFrontendHotkey(const std::string &name, const QString &description, CanvasMix *mix)
{
	auto callback = [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		if (!pressed)
			return;
		auto *m = static_cast<CanvasMix *>(data);
		invokeOnUi(m, [m]() { Action{}(m); });
	};
	return obs_hotkey_register_frontend(name.c_str(), utf8(description).c_str(), callback, mix);
}

struct TransitionAction {
	void operator()(CanvasMix *m) const { m->transition(); }
};
struct CutAction {
	void operator()(CanvasMix *m) const { m->cut(); }
};
struct NextAction {
	void operator()(CanvasMix *m) const { m->previewRelative(1); }
};
struct PrevAction {
	void operator()(CanvasMix *m) const { m->previewRelative(-1); }
};

} // namespace

void CanvasMix::registerHotkeys()
{
	transitionHotkey_ = registerFrontendHotkey<TransitionAction>(hotkeyName(id(), "transition"),
								     T("Hotkey.Transition").arg(name_), this);
	cutHotkey_ = registerFrontendHotkey<CutAction>(hotkeyName(id(), "cut"), T("Hotkey.Cut").arg(name_), this);
	nextHotkey_ = registerFrontendHotkey<NextAction>(hotkeyName(id(), "preview_next"),
							 T("Hotkey.PreviewNext").arg(name_), this);
	prevHotkey_ = registerFrontendHotkey<PrevAction>(hotkeyName(id(), "preview_prev"),
							 T("Hotkey.PreviewPrevious").arg(name_), this);
}

void CanvasMix::registerQuickHotkey(QuickTransition &quick)
{
	auto callback = [](void *data, obs_hotkey_id hotkey, obs_hotkey_t *, bool pressed) {
		if (!pressed)
			return;
		auto *mix = static_cast<CanvasMix *>(data);
		invokeOnUi(mix, [mix, hotkey]() {
			for (size_t i = 0; i < mix->quickTransitions_.size(); i++) {
				if (mix->quickTransitions_[i].hotkey == hotkey) {
					mix->quickTransition(i);
					return;
				}
			}
		});
	};
	const std::string name = hotkeyName(id(), "quick_transition.") + std::to_string(quick.id);
	quick.hotkey = obs_hotkey_register_frontend(name.c_str(), "", callback, this);
	updateHotkeyDescriptions();
}

void CanvasMix::unregisterHotkeys()
{
	for (obs_hotkey_id *id : {&transitionHotkey_, &cutHotkey_, &nextHotkey_, &prevHotkey_}) {
		if (*id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(*id);
		*id = OBS_INVALID_HOTKEY_ID;
	}
	for (QuickTransition &quick : quickTransitions_) {
		if (quick.hotkey != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(quick.hotkey);
		quick.hotkey = OBS_INVALID_HOTKEY_ID;
	}
}

void CanvasMix::updateHotkeyDescriptions()
{
	auto set = [](obs_hotkey_id id, const QString &description) {
		if (id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_set_description(id, utf8(description).c_str());
	};
	set(transitionHotkey_, T("Hotkey.Transition").arg(name_));
	set(cutHotkey_, T("Hotkey.Cut").arg(name_));
	set(nextHotkey_, T("Hotkey.PreviewNext").arg(name_));
	set(prevHotkey_, T("Hotkey.PreviewPrevious").arg(name_));
	for (size_t i = 0; i < quickTransitions_.size(); i++)
		set(quickTransitions_[i].hotkey, T("Hotkey.QuickTransition").arg(name_, quickTransitionLabel(i)));
}

/* ------------------------------------------------------------------------- */
/* Persistence */

void CanvasMix::save(obs_data_t *data) const
{
	obs_data_set_string(data, "id", utf8(id()).c_str());
	obs_data_set_string(data, "name", utf8(name_).c_str());
	obs_data_set_bool(data, "follow_main_resolution", followMain_);
	obs_data_set_int(data, "width", customWidth_);
	obs_data_set_int(data, "height", customHeight_);
	obs_data_set_bool(data, "mix_audio", mixAudio_);
	obs_data_set_bool(data, "swap_after_transition", swapAfterTransition_);
	obs_data_set_bool(data, "double_click_transition", doubleClickTransitions_);
	obs_data_set_bool(data, "include_in_global", includedInGlobal());
	obs_data_set_bool(data, "use_scene_filter", useSceneFilter_);

	OBSDataArrayAutoRelease filter = obs_data_array_create();
	for (const std::string &uuid : sceneFilter_) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "uuid", uuid.c_str());
		obs_data_array_push_back(filter, item);
	}
	obs_data_set_array(data, "scene_filter", filter);

	OBSSource preview = previewScene();
	OBSSource program = programScene();
	obs_data_set_string(data, "preview", preview ? obs_source_get_uuid(preview) : "");
	obs_data_set_string(data, "program", program ? obs_source_get_uuid(program) : "");

	obs_data_set_string(data, "transition_override", overrideUuid_.c_str());
	obs_data_set_string(data, "transition_override_name", overrideName_.c_str());
	obs_data_set_int(data, "transition_override_duration", overrideDurationMs_);

	OBSDataArrayAutoRelease quick = obs_data_array_create();
	for (const QuickTransition &q : quickTransitions_) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_int(item, "id", q.id);
		obs_data_set_string(item, "uuid", q.uuid.c_str());
		obs_data_set_string(item, "name", q.name.c_str());
		obs_data_set_int(item, "duration", q.durationMs);
		obs_data_set_bool(item, "fade_to_black", q.fadeToBlack);
		saveHotkey(item, "hotkey", q.hotkey);
		obs_data_array_push_back(quick, item);
	}
	obs_data_set_array(data, "quick_transitions", quick);

	OBSDataAutoRelease hotkeys = obs_data_create();
	saveHotkey(hotkeys, "transition", transitionHotkey_);
	saveHotkey(hotkeys, "cut", cutHotkey_);
	saveHotkey(hotkeys, "preview_next", nextHotkey_);
	saveHotkey(hotkeys, "preview_prev", prevHotkey_);
	obs_data_set_obj(data, "hotkeys", hotkeys);
}

void CanvasMix::load(obs_data_t *data)
{
	obs_data_set_default_bool(data, "follow_main_resolution", true);
	obs_data_set_default_bool(data, "double_click_transition", true);
	obs_data_set_default_bool(data, "include_in_global", true);
	obs_data_set_default_int(data, "transition_override_duration", kDefaultDurationMs);

	followMain_ = obs_data_get_bool(data, "follow_main_resolution");
	customWidth_ = std::max<uint32_t>(2, uint32_t(obs_data_get_int(data, "width")));
	customHeight_ = std::max<uint32_t>(2, uint32_t(obs_data_get_int(data, "height")));
	mixAudio_ = obs_data_get_bool(data, "mix_audio");
	swapAfterTransition_ = obs_data_get_bool(data, "swap_after_transition");
	doubleClickTransitions_ = obs_data_get_bool(data, "double_click_transition");
	initIncludedInGlobal(obs_data_get_bool(data, "include_in_global"));
	useSceneFilter_ = obs_data_get_bool(data, "use_scene_filter");

	OBSDataArrayAutoRelease filter = obs_data_get_array(data, "scene_filter");
	for (size_t i = 0, n = obs_data_array_count(filter); i < n; i++) {
		OBSDataAutoRelease item = obs_data_array_item(filter, i);
		sceneFilter_.emplace_back(obs_data_get_string(item, "uuid"));
	}

	overrideUuid_ = obs_data_get_string(data, "transition_override");
	overrideName_ = obs_data_get_string(data, "transition_override_name");
	overrideDurationMs_ = std::max(50, int(obs_data_get_int(data, "transition_override_duration")));

	createCanvas();
	setProgramInternal(sceneByUuid(obs_data_get_string(data, "program")));
	activate(cut_);
	/* A restored Preview is not a new cue: the global switch waits for a fresh selection. */
	setPreviewInternal(sceneByUuid(obs_data_get_string(data, "preview")), false);

	registerHotkeys();

	/* Quick transitions: a mix saved without any gets OBS's defaults. */
	OBSDataArrayAutoRelease quick = obs_data_get_array(data, "quick_transitions");
	if (!quick)
		addDefaultQuickTransitions();
	for (size_t i = 0, n = obs_data_array_count(quick); i < n; i++) {
		OBSDataAutoRelease item = obs_data_array_item(quick, i);
		QuickTransition q;
		/* Ids only need to be unique per session: bindings are restored from the entry itself. */
		q.id = nextQuickId_++;
		q.uuid = obs_data_get_string(item, "uuid");
		q.name = obs_data_get_string(item, "name");
		q.durationMs = std::max(50, int(obs_data_get_int(item, "duration")));
		q.fadeToBlack = obs_data_get_bool(item, "fade_to_black");
		quickTransitions_.push_back(std::move(q));
		registerQuickHotkey(quickTransitions_.back());
		loadHotkey(item, "hotkey", quickTransitions_.back().hotkey);
	}

	OBSDataAutoRelease hotkeys = obs_data_get_obj(data, "hotkeys");
	loadHotkey(hotkeys, "transition", transitionHotkey_);
	loadHotkey(hotkeys, "cut", cutHotkey_);
	loadHotkey(hotkeys, "preview_next", nextHotkey_);
	loadHotkey(hotkeys, "preview_prev", prevHotkey_);
}

} // namespace dm
