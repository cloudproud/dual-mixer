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

#include <QWidget>

#include <functional>
#include <memory>

class QFormLayout;

namespace dm {

/*
 * Minimal editor for obs_properties_t (encoder and service settings). Mirrors the behaviour of
 * OBS's properties view for the common property types, including "modified" callbacks.
 */
class PropertiesForm : public QWidget {
	Q_OBJECT

public:
	using Loader = std::function<obs_properties_t *()>;

	PropertiesForm(Loader loader, obs_data_t *settings, QWidget *parent = nullptr);
	~PropertiesForm() override;

	obs_data_t *settings() const { return settings_; }

signals:
	void changed();

private:
	void rebuild();
	void scheduleRebuild();
	void addProperties(obs_properties_t *props, QFormLayout *layout);
	void addProperty(obs_property_t *property, QFormLayout *layout);
	void notifyChanged(obs_property_t *property);

	Loader loader_;
	OBSData settings_;
	std::unique_ptr<obs_properties_t, void (*)(obs_properties_t *)> properties_;
	QWidget *content_ = nullptr;
	bool rebuildPending_ = false;
};

} // namespace dm
