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
#include <mutex>

namespace dm {

class Mix;

/* Native surface rendered by libobs (same approach as OBS's own preview widget). */
class DisplayWidget : public QWidget {
	Q_OBJECT

public:
	/* Called on the graphics thread with the display's pixel size. */
	using Renderer = std::function<void(uint32_t cx, uint32_t cy)>;

	explicit DisplayWidget(QWidget *parent = nullptr);
	~DisplayWidget() override;

	void setRenderer(Renderer renderer);
	void destroyDisplay();

	/* Renders a mix's Program or Preview aspect-fitted into a display. */
	static Renderer programRenderer(Mix *mix);
	static Renderer previewRenderer(Mix *mix);

	QPaintEngine *paintEngine() const override { return nullptr; }

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void moveEvent(QMoveEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void createDisplay();
	void resizeDisplay();
	void watchAncestors();
	void scheduleNativeSync();
	void syncNativeGeometry();
	static void draw(void *param, uint32_t cx, uint32_t cy);

	OBSDisplay display_;
	bool watchingWindow_ = false;
	bool watchingAncestors_ = false;
	bool syncPending_ = false;
	bool destroyed_ = false;
	std::mutex rendererMutex_;
	Renderer renderer_;
};

} // namespace dm
