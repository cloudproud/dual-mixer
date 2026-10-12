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
#include <obs-module.h>

#include <QString>

#include <string>
#include <vector>

namespace dm {

/* Key used for the plugin's object inside the scene collection JSON. */
inline constexpr const char *kSaveKey = "dual-mixer";
/* Identifier of the mix that represents OBS's own main canvas. */
inline constexpr const char *kNativeMixId = "obs-main";
/* Precision of the T-bar, matching the native OBS T-bar. */
inline constexpr int kTBarMax = 1024;
/* Number of OBS audio mixer tracks. */
inline constexpr int kAudioTracks = MAX_AUDIO_MIXES;

inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

inline QString qstr(const char *s)
{
	return QString::fromUtf8(s ? s : "");
}

inline std::string utf8(const QString &s)
{
	return s.toStdString();
}

/* Generates a new random identifier without braces. */
QString newId();

/* Scenes from the current scene collection in the order shown by OBS. Main thread only. */
std::vector<OBSSource> frontendScenes();

/* Resolves a scene by UUID; returns null for removed sources and non-scenes. */
OBSSource sceneByUuid(const char *uuid);

/* Base (canvas) resolution of OBS's main video. */
bool mainVideoInfo(obs_video_info &ovi);

/* Human readable label for an OBS audio mixer track (0-based), including its user name when set. */
QString audioTrackLabel(int track);

/* Display name for a registered encoder id, falling back to the id itself. */
QString encoderDisplayName(const char *id);

/* Computes the aspect-preserving placement of a base-sized frame inside a window. */
void fitInside(uint32_t baseCX, uint32_t baseCY, uint32_t windowCX, uint32_t windowCY, int &x, int &y, int &cx,
	       int &cy);

} // namespace dm
