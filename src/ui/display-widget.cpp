/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "display-widget.hpp"

#include "common.hpp"
#include "mix.hpp"

#include <graphics/vec4.h>

#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QWindow>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

namespace dm {

namespace {

constexpr uint32_t kBackgroundColor = 0x1a1a1a;

bool toGsWindow(QWindow *window, gs_window &gswindow)
{
#ifdef _WIN32
	gswindow.hwnd = reinterpret_cast<HWND>(window->winId());
	return true;
#elif defined(__APPLE__)
	gswindow.view = reinterpret_cast<id>(window->winId());
	return true;
#else
	switch (obs_get_nix_platform()) {
	case OBS_NIX_PLATFORM_X11_EGL:
		gswindow.id = window->winId();
		gswindow.display = obs_get_nix_platform_display();
		return true;
#ifdef ENABLE_WAYLAND
	case OBS_NIX_PLATFORM_WAYLAND:
		gswindow.display = reinterpret_cast<void *>(window->winId());
		return gswindow.display != nullptr;
#endif
	default:
		return false;
	}
#endif
}

template<typename Fn> void renderFitted(uint32_t baseCX, uint32_t baseCY, uint32_t cx, uint32_t cy, Fn &&render)
{
	int x, y, w, h;
	fitInside(baseCX, baseCY, cx, cy, x, y, w, h);
	if (w <= 0 || h <= 0)
		return;

	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, float(baseCX), 0.0f, float(baseCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, w, h);

	/* Black frame for the mix area so letterboxing outside it stays distinguishable. */
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	vec4 black;
	vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(color, &black);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, baseCX, baseCY);

	render();

	gs_projection_pop();
	gs_viewport_pop();
}

} // namespace

DisplayWidget::DisplayWidget(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setMinimumSize(64, 36);
}

DisplayWidget::~DisplayWidget()
{
	destroyDisplay();
}

void DisplayWidget::setRenderer(Renderer renderer)
{
	std::lock_guard lock(rendererMutex_);
	renderer_ = std::move(renderer);
}

void DisplayWidget::destroyDisplay()
{
	destroyed_ = true;
	/* obs_display_destroy waits for an in-flight draw callback to finish. */
	display_ = nullptr;
	setRenderer(nullptr);
}

DisplayWidget::Renderer DisplayWidget::programRenderer(Mix *mix)
{
	return [mix](uint32_t cx, uint32_t cy) {
		renderFitted(mix->width(), mix->height(), cx, cy, [mix]() { mix->renderProgram(); });
	};
}

DisplayWidget::Renderer DisplayWidget::previewRenderer(Mix *mix)
{
	return [mix](uint32_t cx, uint32_t cy) {
		renderFitted(mix->width(), mix->height(), cx, cy, [mix]() { mix->renderPreview(); });
	};
}

void DisplayWidget::draw(void *param, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<DisplayWidget *>(param);
	std::lock_guard lock(self->rendererMutex_);
	if (self->renderer_)
		self->renderer_(cx, cy);
}

void DisplayWidget::createDisplay()
{
	if (display_ || destroyed_)
		return;
	QWindow *window = windowHandle();
	if (!window || !window->isExposed())
		return;

	const QSize size = this->size() * devicePixelRatioF();
	gs_init_data info = {};
	info.cx = uint32_t(size.width());
	info.cy = uint32_t(size.height());
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	if (!toGsWindow(window, info.window))
		return;

	display_ = obs_display_create(&info, kBackgroundColor);
	if (display_)
		obs_display_add_draw_callback(display_, draw, this);
}

void DisplayWidget::resizeDisplay()
{
	createDisplay();
	if (display_ && isVisible()) {
		const QSize size = this->size() * devicePixelRatioF();
		obs_display_resize(display_, uint32_t(size.width()), uint32_t(size.height()));
	}
}

void DisplayWidget::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);

	QWindow *window = windowHandle();
	if (window && !watchingWindow_) {
		watchingWindow_ = true;
		window->installEventFilter(this);
		connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
			if (visible)
				resizeDisplay();
		});
		connect(window, &QWindow::screenChanged, this, [this](QScreen *) {
			if (display_)
				obs_display_update_color_space(display_);
			resizeDisplay();
		});
	}
	if (display_)
		obs_display_set_enabled(display_, true);
	watchAncestors();
	resizeDisplay();
	scheduleNativeSync();
}

void DisplayWidget::watchAncestors()
{
	if (watchingAncestors_)
		return;
	watchingAncestors_ = true;
	/* Qt does not always move a native child when one of its non-native ancestors is moved by a
	 * layout (e.g. a panel re-placed in the grid), leaving the surface drawn at a stale spot. */
	QWidget *nativeParent = nativeParentWidget();
	for (QWidget *w = parentWidget(); w && w != nativeParent; w = w->parentWidget())
		w->installEventFilter(this);
}

void DisplayWidget::scheduleNativeSync()
{
	if (syncPending_)
		return;
	syncPending_ = true;
	QTimer::singleShot(0, this, [this]() {
		syncPending_ = false;
		syncNativeGeometry();
	});
}

void DisplayWidget::syncNativeGeometry()
{
	QWindow *window = windowHandle();
	QWidget *nativeParent = nativeParentWidget();
	if (!window || !nativeParent || !isVisible())
		return;
	const QRect target(mapTo(nativeParent, QPoint(0, 0)), size());
	/* QWindow's cached geometry can already match while the native surface was never moved, so
	 * nudge it to force the window system to apply the frame. */
	if (window->geometry() == target)
		window->setGeometry(target.adjusted(0, 0, 1, 0));
	window->setGeometry(target);
	/* Repaint what the surface used to cover. */
	nativeParent->update();
}

void DisplayWidget::hideEvent(QHideEvent *event)
{
	/* Collapsed panels cost nothing on the GPU. */
	if (display_)
		obs_display_set_enabled(display_, false);
	QWidget::hideEvent(event);
}

bool DisplayWidget::eventFilter(QObject *watched, QEvent *event)
{
	if (watched != windowHandle()) {
		switch (event->type()) {
		case QEvent::Move:
		case QEvent::Resize:
		case QEvent::Show:
		case QEvent::LayoutRequest:
			scheduleNativeSync();
			break;
		default:
			break;
		}
		return QWidget::eventFilter(watched, event);
	}

	if (event->type() == QEvent::PlatformSurface) {
		auto *surfaceEvent = static_cast<QPlatformSurfaceEvent *>(event);
		if (surfaceEvent->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
			display_ = nullptr;
	} else if (event->type() == QEvent::Expose) {
		resizeDisplay();
	}
	return QWidget::eventFilter(watched, event);
}

void DisplayWidget::paintEvent(QPaintEvent *event)
{
	createDisplay();
	QWidget::paintEvent(event);
}

void DisplayWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	scheduleNativeSync();
	resizeDisplay();
}

void DisplayWidget::moveEvent(QMoveEvent *event)
{
	QWidget::moveEvent(event);
	scheduleNativeSync();
	if (display_)
		obs_display_update_color_space(display_);
}

} // namespace dm
