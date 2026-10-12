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

#include <QObject>
#include <QString>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dm {

/*
 * A mix is an independent Preview/Program pair with its own scene selection and transition
 * playback. What a mix produces (its Program) is separate from where it goes: destinations
 * reference a mix by id and never the other way around.
 *
 * Transition definitions come from OBS's own transition list. The transition selected in OBS
 * is the global transition; a mix may override it with any other OBS transition.
 */
class Mix : public QObject {
	Q_OBJECT

public:
	explicit Mix(QString id, QObject *parent = nullptr) : QObject(parent), id_(std::move(id)) {}

	const QString &id() const { return id_; }
	virtual QString name() const = 0;
	/* True for the mix that represents OBS's own main canvas. */
	virtual bool isNative() const = 0;

	/* Program video for encoders. Null if the mix has no video. */
	virtual video_t *video() const = 0;
	/* Base resolution of the Program. Safe to call from the graphics thread. */
	virtual uint32_t width() const = 0;
	virtual uint32_t height() const = 0;
	/* Graphics thread: draw Program / Preview in mix coordinates (0,0)-(width,height). */
	virtual void renderProgram() = 0;
	virtual void renderPreview() = 0;
	/* True when this mix currently has no separate Preview (native OBS without Studio Mode). */
	virtual bool previewFollowsProgram() const { return false; }

	virtual std::vector<OBSSource> scenes() const = 0;
	virtual OBSSource previewScene() const = 0;
	virtual OBSSource programScene() const = 0;
	virtual void setPreviewScene(obs_source_t *scene) = 0;

	/* OBS transition used for this mix's next transition (the global one unless overridden). */
	virtual OBSSource effectiveTransition() const = 0;
	/* OBS transition this mix uses instead of the global one; null = follow global. */
	virtual OBSSource transitionOverride() const = 0;
	virtual void setTransitionOverride(obs_source_t *transition) = 0;
	/* Duration of the effective transition in milliseconds. */
	virtual int transitionDuration() const = 0;
	virtual void setTransitionDuration(int ms) = 0;

	virtual bool transitioning() const = 0;
	/* Takes Preview to Program with the effective transition. */
	virtual bool transition() = 0;
	/* Instant Preview to Program switch; unsupported mixes return false. */
	virtual bool cut() = 0;
	virtual bool supportsCut() const = 0;

	/* Manual transition control, position in 0..kTBarMax. */
	virtual void setTBar(int position) = 0;
	virtual void releaseTBar() = 0;
	virtual int tbarPosition() const = 0;

	/*
	 * Cued: a Preview was prepared since the last transition and differs from Program. The
	 * global switch takes every cued mix that is included in it to Program at the same time.
	 */
	virtual bool cued() const = 0;
	bool includedInGlobal() const { return includedInGlobal_; }
	void setIncludedInGlobal(bool included);

	/* Scene selection helpers shared by all mixes. */
	void previewRelative(int delta);

signals:
	void renamed();
	void previewChanged();
	void programChanged();
	void scenesChanged();
	void transitionsChanged();
	void transitionStateChanged();
	void cueChanged();
	/* The video_t returned by video() changed (resolution or flags changed). */
	void videoReset();
	void settingsChanged();

protected:
	void initIncludedInGlobal(bool included) { includedInGlobal_ = included; }

private:
	QString id_;
	bool includedInGlobal_ = true;
};

/* OBS's transition list, in the order shown by OBS. Main thread only. */
std::vector<OBSSource> frontendTransitions();

/*
 * OBS's own main mix. Everything is delegated to the native frontend so that native recording,
 * streaming, Studio Mode and external control (e.g. obs-websocket) keep their exact behaviour.
 * Its transition is always OBS's selected transition, which is also the global transition.
 */
class NativeMix final : public Mix {
	Q_OBJECT

public:
	explicit NativeMix(QObject *parent = nullptr);

	QString name() const override;
	bool isNative() const override { return true; }

	video_t *video() const override;
	uint32_t width() const override { return width_; }
	uint32_t height() const override { return height_; }
	void renderProgram() override;
	void renderPreview() override;
	bool previewFollowsProgram() const override { return !studioMode_; }

	std::vector<OBSSource> scenes() const override;
	OBSSource previewScene() const override;
	OBSSource programScene() const override;
	void setPreviewScene(obs_source_t *scene) override;

	OBSSource effectiveTransition() const override;
	OBSSource transitionOverride() const override { return nullptr; }
	/* Changes OBS's selected (= global) transition. */
	void setTransitionOverride(obs_source_t *transition) override;
	int transitionDuration() const override;
	void setTransitionDuration(int ms) override;

	bool transitioning() const override;
	bool transition() override;
	bool cut() override { return false; }
	bool supportsCut() const override { return false; }

	void setTBar(int position) override;
	void releaseTBar() override;
	int tbarPosition() const override;

	bool cued() const override;

	bool studioMode() const { return studioMode_; }
	void setStudioMode(bool enabled);

	/* Re-reads Studio Mode and Preview from the frontend; only valid once OBS's UI exists. */
	void refreshFrontendState();
	/* Called by the manager for every frontend event. */
	void handleFrontendEvent(int event);
	void refreshVideoInfo();
	void setIncludedFromSave(bool included) { initIncludedInGlobal(included); }

private:
	void refreshPreviewCache();
	void setPending(bool pending);

	std::atomic<uint32_t> width_{0};
	std::atomic<uint32_t> height_{0};
	std::atomic<bool> studioMode_{false};
	bool pending_ = false;

	mutable std::mutex previewMutex_;
	OBSWeakSource preview_;
};

class CanvasMix final : public Mix {
	Q_OBJECT

public:
	/* Creates a mix with defaults, or restores it from saved data when data is set. */
	CanvasMix(QString id, QString name, obs_data_t *data, QObject *parent = nullptr);
	~CanvasMix() override;

	QString name() const override { return name_; }
	void setName(const QString &name);
	bool isNative() const override { return false; }

	video_t *video() const override;
	uint32_t width() const override { return width_; }
	uint32_t height() const override { return height_; }
	void renderProgram() override;
	void renderPreview() override;

	std::vector<OBSSource> scenes() const override;
	OBSSource previewScene() const override;
	OBSSource programScene() const override;
	void setPreviewScene(obs_source_t *scene) override;

	OBSSource effectiveTransition() const override;
	OBSSource transitionOverride() const override;
	void setTransitionOverride(obs_source_t *transition) override;
	int transitionDuration() const override;
	void setTransitionDuration(int ms) override;

	bool transitioning() const override;
	bool transition() override;
	bool cut() override;
	bool supportsCut() const override { return true; }

	void setTBar(int position) override;
	void releaseTBar() override;
	int tbarPosition() const override { return tbarPosition_; }

	bool cued() const override;

	/* One-click transitions, like OBS's Quick Transitions: an OBS transition and a duration, or
	 * Fade to Black, which fades Program to black and back with the given fade. */
	struct QuickTransition {
		int id = 0;
		std::string uuid;
		std::string name;
		int durationMs = 300;
		bool fadeToBlack = false;
		obs_hotkey_id hotkey = OBS_INVALID_HOTKEY_ID;
	};
	const std::vector<QuickTransition> &quickTransitions() const { return quickTransitions_; }
	/* Resolves the OBS transition of a quick transition; null if it no longer exists. */
	OBSSource quickTransitionSource(size_t index) const;
	/* Button text in OBS's format, e.g. "Fade (300ms)" or "Fade to Black (300ms)". */
	QString quickTransitionLabel(size_t index) const;
	void addQuickTransition(obs_source_t *transition, int durationMs, bool fadeToBlack);
	void changeQuickTransition(size_t index, obs_source_t *transition, bool fadeToBlack);
	void setQuickTransitionDuration(size_t index, int durationMs);
	void removeQuickTransition(size_t index);
	/* Runs the quick transition without changing the mix's selected transition. */
	bool quickTransition(size_t index);
	/* True while Fade to Black holds Program on black. */
	bool fadedToBlack() const;

	/* Resolution: follow OBS's base canvas or use a custom size. */
	bool followsMainResolution() const { return followMain_; }
	uint32_t customWidth() const { return customWidth_; }
	uint32_t customHeight() const { return customHeight_; }
	/* Recreates the canvas; callers must ensure no encoder is attached to this mix. */
	void setResolution(bool followMain, uint32_t cx, uint32_t cy);

	/* Whether audio of this mix's Program sources is mixed into OBS's global audio output. */
	bool mixesAudio() const { return mixAudio_; }
	/* Recreates the canvas; callers must ensure no encoder is attached to this mix. */
	void setMixesAudio(bool enabled);

	bool swapAfterTransition() const { return swapAfterTransition_; }
	void setSwapAfterTransition(bool enabled);
	bool doubleClickTransitions() const { return doubleClickTransitions_; }
	void setDoubleClickTransitions(bool enabled);

	/* Optional ordered subset of OBS scenes offered by this mix (UUIDs). */
	bool usesSceneFilter() const { return useSceneFilter_; }
	const std::vector<std::string> &sceneFilter() const { return sceneFilter_; }
	void setSceneFilter(bool enabled, std::vector<std::string> uuids);

	void save(obs_data_t *data) const;

	/* Main OBS video was reset; rebuild the canvas with the new frame rate/format. */
	void handleVideoReset();
	/* Scene list changed; drop references to removed scenes. */
	void handleScenesChanged();
	/* OBS's transition list changed; drop copies of removed transitions. */
	void handleTransitionsChanged();

private:
	/* This mix's private playback copy of an OBS transition, kept in sync with its settings. */
	struct Copy {
		OBSWeakSource origin;
		OBSSource source;
		OBSSignal stopSignal;
		OBSSignal originUpdateSignal;
		bool dirty = false;
	};

	void load(obs_data_t *data);
	void createCanvas();
	static OBSSource resolveTransition(const std::string &uuid, const std::string &name);
	obs_source_t *playbackFor(obs_source_t *origin);
	void syncCopy(Copy &copy);
	bool startTransition(obs_source_t *playback, int durationMs);
	bool toggleFadeToBlack(obs_source_t *playback, int durationMs);
	void registerQuickHotkey(QuickTransition &quick);
	void addDefaultQuickTransitions();
	void activate(obs_source_t *playback);
	void applyTransitionSize(obs_source_t *transition) const;
	void setPreviewInternal(obs_source_t *scene, bool userCue);
	void setProgramInternal(obs_source_t *scene);
	void finishManualTransition();
	void registerHotkeys();
	void unregisterHotkeys();
	void updateHotkeyDescriptions();
	static void onTransitionStop(void *data, calldata_t *cd);
	static void onOriginUpdate(void *data, calldata_t *cd);

	QString name_;

	mutable std::mutex canvasMutex_;
	OBSCanvas canvas_;
	std::atomic<uint32_t> width_{0};
	std::atomic<uint32_t> height_{0};
	bool followMain_ = true;
	uint32_t customWidth_ = 1920;
	uint32_t customHeight_ = 1080;
	bool mixAudio_ = false;

	/* Built-in Cut used by the Cut button and when OBS's Cut is the effective transition. */
	OBSSource cut_;
	OBSSignal cutStopSignal_;
	std::map<std::string, std::unique_ptr<Copy>> copies_;
	/* Transition currently attached to the canvas. */
	OBSSource active_;

	std::string overrideUuid_;
	std::string overrideName_;
	int overrideDurationMs_ = 300;
	bool swapAfterTransition_ = false;
	bool doubleClickTransitions_ = true;
	std::vector<QuickTransition> quickTransitions_;
	int nextQuickId_ = 1;
	/* Program scene to return to when Fade to Black is active. */
	OBSWeakSource blackFrom_;

	bool tbarActive_ = false;
	int tbarPosition_ = 0;

	bool useSceneFilter_ = false;
	std::vector<std::string> sceneFilter_;

	mutable std::mutex sceneMutex_;
	OBSWeakSource preview_;
	OBSWeakSource program_;
	bool pending_ = false;

	obs_hotkey_id transitionHotkey_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id cutHotkey_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id nextHotkey_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id prevHotkey_ = OBS_INVALID_HOTKEY_ID;
};

} // namespace dm
