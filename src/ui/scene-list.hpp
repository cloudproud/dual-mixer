/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QListWidget>

namespace dm {

class Mix;

/*
 * A mix's compact scene list. Mirrors OBS's own Scenes list (same widget type and theme hooks,
 * and it follows OBS's list/grid setting): the selection is the mix's Preview; the scene on the
 * mix's Program is marked in red.
 */
class SceneList : public QListWidget {
	Q_OBJECT

public:
	explicit SceneList(Mix *mix, QWidget *parent = nullptr);

	void refresh();
	void refreshTally();

protected:
	void resizeEvent(QResizeEvent *event) override;

private:
	void applyGridMode();

	Mix *mix_;
	bool gridMode_ = false;
	bool updating_ = false;
};

} // namespace dm
