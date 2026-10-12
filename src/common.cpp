/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "common.hpp"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QUuid>

namespace dm {

QString newId()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

std::vector<OBSSource> frontendScenes()
{
	std::vector<OBSSource> result;
	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	result.reserve(list.sources.num);
	for (size_t i = 0; i < list.sources.num; i++)
		result.emplace_back(list.sources.array[i]);
	obs_frontend_source_list_free(&list);
	return result;
}

OBSSource sceneByUuid(const char *uuid)
{
	if (!uuid || !*uuid)
		return nullptr;
	OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid);
	if (!source || !obs_source_is_scene(source) || obs_source_removed(source))
		return nullptr;
	return source.Get();
}

bool mainVideoInfo(obs_video_info &ovi)
{
	return obs_get_video_info(&ovi);
}

QString audioTrackLabel(int track)
{
	QString label = T("Audio.Track").arg(track + 1);
	config_t *profile = obs_frontend_get_profile_config();
	if (!profile)
		return label;

	const std::string key = "Track" + std::to_string(track + 1) + "Name";
	const char *name = config_get_string(profile, "AdvOut", key.c_str());
	if (name && *name)
		label += QStringLiteral(" (%1)").arg(qstr(name));
	return label;
}

QString encoderDisplayName(const char *id)
{
	const char *name = obs_encoder_get_display_name(id);
	return qstr(name && *name ? name : id);
}

void fitInside(uint32_t baseCX, uint32_t baseCY, uint32_t windowCX, uint32_t windowCY, int &x, int &y, int &cx, int &cy)
{
	if (!baseCX || !baseCY || !windowCX || !windowCY) {
		x = y = cx = cy = 0;
		return;
	}

	const double windowAspect = double(windowCX) / double(windowCY);
	const double baseAspect = double(baseCX) / double(baseCY);

	if (windowAspect > baseAspect) {
		cy = int(windowCY);
		cx = int(double(windowCY) * baseAspect);
	} else {
		cx = int(windowCX);
		cy = int(double(windowCX) / baseAspect);
	}

	x = (int(windowCX) - cx) / 2;
	y = (int(windowCY) - cy) / 2;
}

} // namespace dm
