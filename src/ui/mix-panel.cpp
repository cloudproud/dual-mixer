/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "mix-panel.hpp"

#include "display-widget.hpp"
#include "flow-layout.hpp"
#include "mix-settings-dialog.hpp"
#include "scene-list.hpp"
#include "ui-helpers.hpp"

#include "manager.hpp"
#include "mix.hpp"

#include <obs-frontend-api.h>

#include <QActionGroup>
#include <QCheckBox>
#include <QCursor>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <cstring>

namespace dm {

namespace {

constexpr int kSceneListWidth = 170;
constexpr int kControlsWidth = 160;

/*
 * Keeps a display at the mix's aspect ratio. Grids and scroll areas do not propagate
 * height-for-width, so the height is pinned from the width the layout assigns.
 */
class AspectBox : public QWidget {
public:
	AspectBox(Mix *mix, QWidget *child, QWidget *parent) : QWidget(parent), mix_(mix)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->addWidget(child);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		setMinimumWidth(96);
		setFixedHeight(heightFor(320));
	}

	void updateHeight()
	{
		const int height = heightFor(width());
		if (height == this->height() && minimumHeight() == height)
			return;
		setFixedHeight(height);
		/* Grids cache size hints; make every ancestor recompute its row height. */
		for (QWidget *w = parentWidget(); w; w = w->parentWidget()) {
			w->updateGeometry();
			if (w->isWindow())
				break;
		}
	}

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		if (event->size().width() != event->oldSize().width())
			updateHeight();
	}

private:
	int heightFor(int width) const
	{
		const uint32_t cx = mix_->width();
		const uint32_t cy = mix_->height();
		if (!cx || !cy)
			return width * 9 / 16;
		return std::max(36, int(qint64(width) * cy / cx));
	}

	Mix *mix_;
};

/* Theme-styled icon button (OBS's theme sets the icon from the class). */
QToolButton *iconButton(const char *iconClass, const QString &tooltip, QWidget *parent)
{
	auto *button = new QToolButton(parent);
	button->setProperty("class", iconClass);
	button->setToolTip(tooltip);
	button->setAccessibleName(tooltip);
	return button;
}

QToolButton *textButton(const QString &text, const QString &tooltip, QWidget *parent)
{
	auto *button = new QToolButton(parent);
	button->setText(text);
	button->setToolTip(tooltip);
	return button;
}

QLabel *classLabel(const QString &text, const char *cssClass, QWidget *parent)
{
	auto *label = new QLabel(text, parent);
	label->setProperty("class", cssClass);
	return label;
}

/* Opens a menu below a widget, the way OBS opens its Studio Mode menus. */
void popupBelow(QMenu *menu, QWidget *anchor)
{
	menu->exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
}

/* Duration editor embedded in a menu, like OBS's Quick Transitions menu. */
QSpinBox *addDurationAction(QMenu *menu, int value, bool enabled, const QString &tooltip)
{
	auto *spin = new QSpinBox(menu);
	spin->setRange(50, 20000);
	spin->setSingleStep(50);
	spin->setSuffix(QStringLiteral(" ms"));
	spin->setValue(value);
	spin->setEnabled(enabled);
	spin->setToolTip(tooltip);
	auto *action = new QWidgetAction(menu);
	action->setDefaultWidget(spin);
	menu->addAction(action);
	return spin;
}

OBSSource frontendFade()
{
	for (const OBSSource &transition : frontendTransitions()) {
		if (strcmp(obs_source_get_id(transition), "fade_transition") == 0)
			return transition;
	}
	return nullptr;
}

} // namespace

MixPanel::MixPanel(Mix *mix, Manager *manager, bool inOwnDock, QWidget *parent)
	: QFrame(parent),
	  mix_(mix),
	  manager_(manager),
	  inOwnDock_(inOwnDock)
{
	setObjectName(QStringLiteral("dualMixerPanel"));
	setFrameShape(QFrame::StyledPanel);
	/* Preferred, not Maximum: a cached size hint must never squeeze the row below its displays. */
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 6, 8, 8);
	layout->setSpacing(6);

	buildHeader();
	buildBody();

	stateTimer_.setInterval(100);
	connect(&stateTimer_, &QTimer::timeout, this, &MixPanel::refreshTransitionState);

	connect(mix_, &Mix::renamed, this, &MixPanel::refreshName);
	connect(mix_, &Mix::previewChanged, this, &MixPanel::refreshTally);
	connect(mix_, &Mix::programChanged, this, &MixPanel::refreshTally);
	connect(mix_, &Mix::scenesChanged, this, &MixPanel::refreshScenes);
	connect(mix_, &Mix::transitionsChanged, this, &MixPanel::refreshTransitions);
	connect(mix_, &Mix::transitionStateChanged, this, &MixPanel::refreshTransitionState);
	connect(mix_, &Mix::cueChanged, this, &MixPanel::refreshCue);
	connect(mix_, &Mix::settingsChanged, this, [this]() {
		refreshNative();
		refreshTransitions();
		refreshCue();
	});
	connect(mix_, &Mix::videoReset, this, [this]() {
		for (QWidget *child : findChildren<QWidget *>()) {
			if (auto *box = dynamic_cast<AspectBox *>(child))
				box->updateHeight();
		}
	});

	connect(manager_, &Manager::destinationAdded, this, &MixPanel::refreshDestinations);
	connect(manager_, &Manager::destinationRemoved, this, &MixPanel::refreshDestinations);
	connect(manager_, &Manager::nativeOutputsChanged, this, &MixPanel::refreshDestinations);
	connect(manager_, &Manager::panelStateChanged, this, [this](const QString &id) {
		if (id == mix_->id())
			applyCompact();
	});
	connect(manager_, &Manager::workspaceChanged, this, &MixPanel::refreshFocus);
	connect(manager_, &Manager::mixOrderChanged, this, &MixPanel::refreshFocus);

	refreshName();
	refreshScenes();
	refreshTransitions();
	refreshDestinations();
	refreshNative();
	refreshFocus();
	refreshCue();
	applyCompact();
}

MixPanel::~MixPanel()
{
	detach();
}

void MixPanel::detach()
{
	stateTimer_.stop();
	if (previewDisplay_)
		previewDisplay_->destroyDisplay();
	if (programDisplay_)
		programDisplay_->destroyDisplay();
	if (mix_)
		disconnect(mix_, nullptr, this, nullptr);
	disconnect(manager_, nullptr, this, nullptr);
}

/* ------------------------------------------------------------------------- */
/* Construction */

void MixPanel::buildHeader()
{
	auto *header = new QHBoxLayout();
	header->setSpacing(6);

	/* Same collapse control as groups in OBS's Sources list (checked = collapsed). */
	collapseCheck_ = new QCheckBox(this);
	collapseCheck_->setProperty("class", "checkbox-icon indicator-expand");
	collapseCheck_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
	collapseCheck_->setToolTip(T("Panel.Collapse"));
	collapseCheck_->setAccessibleName(T("Panel.Collapse"));
#ifdef __APPLE__
	collapseCheck_->setAttribute(Qt::WA_LayoutUsesWidgetRect);
#endif
	connect(collapseCheck_, &QCheckBox::toggled, this, [this](bool collapsed) {
		Manager::PanelState state = manager_->panelState(mix_->id());
		state.collapsed = collapsed;
		manager_->setPanelState(mix_->id(), state);
	});

	nameLabel_ = classLabel(QString(), "text-bold", this);
	transitioningLabel_ = classLabel(T("Panel.Transitioning"), "text-muted", this);
	cueLabel_ = classLabel(T("Panel.Cued"), "text-warning", this);
	liveLabel_ = classLabel(T("Panel.Live"), "text-danger", this);
	summaryLabel_ = classLabel(QString(), "text-muted", this);
	summaryLabel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

	globalCheck_ = new QCheckBox(T("Panel.Global"), this);
	globalCheck_->setToolTip(T("Panel.GlobalTooltip"));
	globalCheck_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	globalCheck_->setMinimumWidth(globalCheck_->sizeHint().width() + 8);
	connect(globalCheck_, &QCheckBox::toggled, this, [this](bool checked) { mix_->setIncludedInGlobal(checked); });

	headerCutButton_ = new QPushButton(T("Panel.Cut"), this);
	connect(headerCutButton_, &QPushButton::clicked, this, [this]() { mix_->cut(); });
	headerTransitionButton_ = new QPushButton(T("Panel.Transition"), this);
	connect(headerTransitionButton_, &QPushButton::clicked, this, [this]() { mix_->transition(); });

	moveUpButton_ = iconButton("icon-up", T("Panel.MoveEarlier"), this);
	connect(moveUpButton_, &QToolButton::clicked, this, [this]() { manager_->moveMix(mix_->id(), -1); });
	moveDownButton_ = iconButton("icon-down", T("Panel.MoveLater"), this);
	connect(moveDownButton_, &QToolButton::clicked, this, [this]() { manager_->moveMix(mix_->id(), 1); });

	focusButton_ = textButton(T("Panel.Focus"), T("Panel.FocusTooltip"), this);
	focusButton_->setCheckable(true);
	connect(focusButton_, &QToolButton::clicked, this, [this]() { emit focusToggled(mix_->id()); });

	popButton_ = textButton(inOwnDock_ ? T("Panel.DockBack") : T("Panel.PopOut"),
				inOwnDock_ ? T("Panel.DockBackTooltip") : T("Panel.PopOutTooltip"), this);
	connect(popButton_, &QToolButton::clicked, this, [this]() { emit popOutToggled(mix_->id()); });

	menuButton_ = iconButton("icon-dots-vert", T("Panel.Menu"), this);
	connect(menuButton_, &QToolButton::clicked, this, [this]() {
		QMenu menu(this);
		if (!mix_->isNative()) {
			menu.addAction(T("Panel.Settings"), this, &MixPanel::openSettings);
		} else {
			auto *native = static_cast<NativeMix *>(mix_);
			menu.addAction(native->studioMode() ? T("Panel.DisableStudioMode")
							    : T("Panel.EnableStudioMode"),
				       this, [native]() { native->setStudioMode(!native->studioMode()); });
		}
		QMenu *add = menu.addMenu(T("Panel.AddDestination"));
		addDestinationActions(add, manager_, mix_->id(), window());
		if (!mix_->isNative()) {
			menu.addSeparator();
			menu.addAction(T("Panel.RemoveMix"), this, &MixPanel::confirmRemove);
		}
		popupBelow(&menu, menuButton_);
	});

	header->addWidget(collapseCheck_);
	header->addWidget(nameLabel_);
	header->addWidget(transitioningLabel_);
	header->addWidget(cueLabel_);
	header->addWidget(liveLabel_);
	header->addSpacing(6);
	header->addWidget(summaryLabel_);
	header->addStretch(1);
	header->addWidget(globalCheck_);
	header->addWidget(headerCutButton_);
	header->addWidget(headerTransitionButton_);
	header->addSpacing(6);
	header->addWidget(moveUpButton_);
	header->addWidget(moveDownButton_);
	header->addWidget(focusButton_);
	header->addWidget(popButton_);
	header->addWidget(menuButton_);
	static_cast<QVBoxLayout *>(layout())->addLayout(header);
}

QWidget *MixPanel::buildControls()
{
	/* The middle column of OBS's Studio Mode: Transition + ⋮, Quick Transitions, T-bar. */
	auto *controls = new QWidget(body_);
	controls->setFixedWidth(kControlsWidth);
	auto *layout = new QVBoxLayout(controls);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);
	layout->addStretch(1);

	auto *mainRow = new QHBoxLayout();
	mainRow->setSpacing(2);
	transitionButton_ = new QPushButton(T("Panel.Transition"), controls);
	transitionButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	connect(transitionButton_, &QPushButton::clicked, this, [this]() {
		mix_->transition();
		refreshTransitionState();
	});
	auto *configButton = new QPushButton(controls);
	configButton->setProperty("class", "icon-dots-vert");
	configButton->setToolTip(T("Panel.TransitionMenu"));
	configButton->setAccessibleName(T("Panel.TransitionMenu"));
	connect(configButton, &QPushButton::clicked, this, [this, configButton]() {
		QMenu menu(this);
		fillTransitionMenu(&menu);
		popupBelow(&menu, configButton);
	});
	mainRow->addWidget(transitionButton_);
	mainRow->addWidget(configButton);
	layout->addLayout(mainRow);

	/* Quick Transitions: one-click buttons with their own transition and duration. */
	quickSection_ = new QWidget(controls);
	auto *quickLayout = new QVBoxLayout(quickSection_);
	quickLayout->setContentsMargins(0, 0, 0, 0);
	quickLayout->setSpacing(4);
	auto *quickHeader = new QHBoxLayout();
	quickHeader->setSpacing(2);
	auto *quickLabel = new QLabel(T("Panel.QuickTransitions"), quickSection_);
	quickLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	auto *quickAdd = new QPushButton(quickSection_);
	quickAdd->setProperty("class", "icon-plus");
	quickAdd->setToolTip(T("Panel.AddQuickTransition"));
	quickAdd->setAccessibleName(T("Panel.AddQuickTransition"));
	connect(quickAdd, &QPushButton::clicked, this, [this, quickAdd]() {
		QMenu menu(this);
		fillQuickMenu(&menu, -1);
		popupBelow(&menu, quickAdd);
	});
	quickHeader->addWidget(quickLabel);
	quickHeader->addWidget(quickAdd);
	quickLayout->addLayout(quickHeader);
	quickButtons_ = new QWidget(quickSection_);
	auto *buttonsLayout = new QVBoxLayout(quickButtons_);
	buttonsLayout->setContentsMargins(0, 0, 0, 0);
	buttonsLayout->setSpacing(4);
	quickLayout->addWidget(quickButtons_);
	layout->addWidget(quickSection_);

	tbar_ = new QSlider(Qt::Horizontal, controls);
	tbar_->setRange(0, kTBarMax);
	tbar_->setToolTip(T("Panel.TBar"));
	tbar_->setAccessibleName(T("Panel.TBar"));
	/* Picks up the OBS theme's T-bar styling. */
	tbar_->setProperty("class", "slider-tbar");
	connect(tbar_, &QSlider::valueChanged, this, [this](int value) {
		if (tbar_->isSliderDown())
			mix_->setTBar(value);
	});
	connect(tbar_, &QSlider::sliderReleased, this, [this]() {
		mix_->releaseTBar();
		refreshTransitionState();
	});
	layout->addWidget(tbar_);
	layout->addStretch(1);
	return controls;
}

void MixPanel::buildBody()
{
	body_ = new QWidget(this);
	auto *layout = new QVBoxLayout(body_);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(6);

	auto *row = new QHBoxLayout();
	row->setSpacing(8);

	sceneList_ = new SceneList(mix_, body_);
	sceneList_->setFixedWidth(kSceneListWidth);
	sceneList_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
	sceneList_->setMinimumHeight(60);
	sceneList_->setAccessibleName(T("Panel.Scenes"));
	row->addWidget(sceneList_);

	auto addVideo = [&](DisplayWidget *&display, QLabel *&label, DisplayWidget::Renderer renderer) {
		auto *column = new QVBoxLayout();
		column->setSpacing(4);
		/* Same role as OBS's "Preview: …" / "Program: …" Studio Mode labels. */
		label = classLabel(QString(), "text-bold", body_);
		label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		display = new DisplayWidget(body_);
		display->setRenderer(std::move(renderer));
		column->addWidget(label);
		column->addWidget(new AspectBox(mix_, display, body_));
		row->addLayout(column, 1);
	};
	addVideo(previewDisplay_, previewLabel_, DisplayWidget::previewRenderer(mix_));
	row->addWidget(buildControls());
	addVideo(programDisplay_, programLabel_, DisplayWidget::programRenderer(mix_));
	layout->addLayout(row);

	/* Native OBS without Studio Mode has no separate Preview. */
	studioNotice_ = new QFrame(body_);
	studioNotice_->setProperty("class", "frame-notice");
	auto *notice = new QHBoxLayout(studioNotice_);
	auto *noticeLabel = new QLabel(T("Panel.StudioModeOff"), studioNotice_);
	noticeLabel->setWordWrap(true);
	auto *enableStudio = new QPushButton(T("Panel.EnableStudioMode"), studioNotice_);
	connect(enableStudio, &QPushButton::clicked, this, [this]() {
		if (auto *native = qobject_cast<NativeMix *>(mix_))
			native->setStudioMode(true);
	});
	notice->addWidget(noticeLabel, 1);
	notice->addWidget(enableStudio);
	layout->addWidget(studioNotice_);

	outputsWidget_ = new QWidget(body_);
	outputsLayout_ = new FlowLayout(outputsWidget_, 4);
	layout->addWidget(outputsWidget_);

	static_cast<QVBoxLayout *>(this->layout())->addWidget(body_);
}

/* ------------------------------------------------------------------------- */
/* Menus */

void MixPanel::fillTransitionMenu(QMenu *menu)
{
	const OBSSource effective = mix_->effectiveTransition();
	const OBSSource override = mix_->transitionOverride();

	auto *group = new QActionGroup(menu);
	if (!mix_->isNative()) {
		/* "Global" follows whatever transition is selected in OBS. */
		OBSSourceAutoRelease global = obs_frontend_get_current_transition();
		QAction *action = menu->addAction(
			T("Panel.GlobalTransition").arg(qstr(global ? obs_source_get_name(global) : nullptr)), this,
			[this]() { mix_->setTransitionOverride(nullptr); });
		action->setCheckable(true);
		action->setChecked(!override);
		group->addAction(action);
		menu->addSeparator();
	}
	for (const OBSSource &transition : frontendTransitions()) {
		OBSWeakSource weak = OBSGetWeakRef(transition);
		QAction *action = menu->addAction(qstr(obs_source_get_name(transition)), this, [this, weak]() {
			if (OBSSource source = OBSGetStrongRef(weak))
				mix_->setTransitionOverride(source);
		});
		action->setCheckable(true);
		action->setChecked(mix_->isNative() ? transition.Get() == effective.Get()
						    : transition.Get() == override.Get());
		group->addAction(action);
	}

	menu->addSeparator();
	const bool fixed = effective && obs_transition_fixed(effective);
	const bool ownsDuration = mix_->isNative() || override;
	QSpinBox *spin = addDurationAction(menu, mix_->transitionDuration(), ownsDuration && !fixed,
					   fixed          ? T("Panel.DurationFixed")
					   : ownsDuration ? T("Panel.Duration")
							  : T("Panel.DurationGlobal"));
	connect(spin, &QSpinBox::valueChanged, this, [this](int value) { mix_->setTransitionDuration(value); });

	QAction *properties = menu->addAction(T("Panel.TransitionProperties"), this, [this]() {
		/* Edits OBS's transition; every mix's playback copy follows the change. */
		OBSSource transition = mix_->effectiveTransition();
		if (transition && obs_source_configurable(transition))
			obs_frontend_open_source_properties(transition);
	});
	properties->setEnabled(effective && obs_source_configurable(effective));

	if (auto *canvasMix = qobject_cast<CanvasMix *>(mix_)) {
		menu->addSeparator();
		QAction *swap = menu->addAction(T("MixSettings.Swap"), this, [canvasMix](bool checked) {
			canvasMix->setSwapAfterTransition(checked);
		});
		swap->setCheckable(true);
		swap->setChecked(canvasMix->swapAfterTransition());
		QAction *doubleClick = menu->addAction(T("MixSettings.DoubleClick"), this, [canvasMix](bool checked) {
			canvasMix->setDoubleClickTransitions(checked);
		});
		doubleClick->setCheckable(true);
		doubleClick->setChecked(canvasMix->doubleClickTransitions());
	}
}

void MixPanel::fillQuickMenu(QMenu *menu, int index)
{
	/* index < 0: the "+" menu that adds a quick transition; otherwise the menu of an existing one.
	 * Same structure as OBS: Remove, Fade to Black, transitions, then the duration. */
	auto *canvasMix = qobject_cast<CanvasMix *>(mix_);
	if (!canvasMix)
		return;
	const bool adding = index < 0;
	const auto &quick = canvasMix->quickTransitions();
	if (!adding && size_t(index) >= quick.size())
		return;
	const size_t at = adding ? 0 : size_t(index);

	if (!adding) {
		menu->addAction(T("Panel.RemoveQuickTransition"), this, [canvasMix, at]() {
			/* Deferred: removing rebuilds the buttons, including the one that opened this menu. */
			QMetaObject::invokeMethod(
				canvasMix, [canvasMix, at]() { canvasMix->removeQuickTransition(at); },
				Qt::QueuedConnection);
		});
		menu->addSeparator();
	}

	const int duration = adding ? 300 : quick[at].durationMs;
	auto *spin = new QSpinBox(menu);
	spin->setRange(50, 20000);
	spin->setSingleStep(50);
	spin->setSuffix(QStringLiteral(" ms"));
	spin->setValue(duration);
	if (!adding) {
		connect(spin, &QSpinBox::valueChanged, this,
			[canvasMix, at](int value) { canvasMix->setQuickTransitionDuration(at, value); });
	}

	auto addChoice = [&](const QString &label, OBSSource source, bool toBlack, bool checked) {
		OBSWeakSource weak = OBSGetWeakRef(source);
		QAction *action = menu->addAction(label, this, [canvasMix, weak, toBlack, adding, at, spin]() {
			OBSSource transition = OBSGetStrongRef(weak);
			if (!transition)
				return;
			if (adding)
				canvasMix->addQuickTransition(transition, spin->value(), toBlack);
			else
				canvasMix->changeQuickTransition(at, transition, toBlack);
		});
		if (!adding) {
			action->setCheckable(true);
			action->setChecked(checked);
		}
		action->setEnabled(source != nullptr);
	};

	const OBSSource current = adding ? OBSSource() : canvasMix->quickTransitionSource(at);
	const bool currentToBlack = !adding && quick[at].fadeToBlack;
	addChoice(T("Transition.FadeToBlack"), frontendFade(), true, currentToBlack);
	for (const OBSSource &transition : frontendTransitions())
		addChoice(qstr(obs_source_get_name(transition)), transition, false,
			  !currentToBlack && transition.Get() == current.Get());

	menu->addSeparator();
	auto *durationAction = new QWidgetAction(menu);
	durationAction->setDefaultWidget(spin);
	menu->addAction(durationAction);
}

void MixPanel::showQuickMenu(int index, QWidget *anchor, const QPoint &globalPos)
{
	QMenu menu(anchor);
	fillQuickMenu(&menu, index);
	menu.exec(globalPos);
}

/* ------------------------------------------------------------------------- */
/* State */

bool MixPanel::compact() const
{
	return forcedCompact_ || manager_->panelState(mix_->id()).collapsed;
}

void MixPanel::setForcedCompact(bool forced)
{
	if (forcedCompact_ == forced)
		return;
	forcedCompact_ = forced;
	applyCompact();
}

void MixPanel::applyCompact()
{
	const bool isCompact = compact();
	body_->setVisible(!isCompact);
	{
		const QSignalBlocker blocker(collapseCheck_);
		collapseCheck_->setChecked(isCompact);
	}
	collapseCheck_->setEnabled(!forcedCompact_);
	summaryLabel_->setVisible(isCompact);
	headerCutButton_->setVisible(isCompact && mix_->supportsCut());
	headerTransitionButton_->setVisible(isCompact);
	refreshTransitionState();
}

void MixPanel::refreshFocus()
{
	focusButton_->setChecked(manager_->focusedMix() == mix_->id());
	focusButton_->setVisible(!inOwnDock_);

	const std::vector<Mix *> &order = manager_->mixes();
	const bool first = !order.empty() && order.front() == mix_;
	const bool last = !order.empty() && order.back() == mix_;
	moveUpButton_->setVisible(!inOwnDock_);
	moveDownButton_->setVisible(!inOwnDock_);
	moveUpButton_->setEnabled(!first);
	moveDownButton_->setEnabled(!last);
}

void MixPanel::refreshName()
{
	nameLabel_->setText(mix_->name());
	nameLabel_->setToolTip(mix_->isNative() ? T("Mix.NativeTooltip") : QString());
	if (inOwnDock_) {
		for (QWidget *w = parentWidget(); w; w = w->parentWidget()) {
			if (auto *dock = qobject_cast<QDockWidget *>(w)) {
				dock->setWindowTitle(mix_->name());
				break;
			}
		}
	}
	refreshTally();
}

void MixPanel::refreshNative()
{
	const bool noPreview = mix_->previewFollowsProgram();
	studioNotice_->setVisible(noPreview);
	tbar_->setVisible(!noPreview);
	refreshTally();
	refreshTransitionState();
}

void MixPanel::refreshScenes()
{
	sceneList_->refresh();
	refreshTally();
}

void MixPanel::refreshTally()
{
	sceneList_->refreshTally();

	const OBSSource preview = mix_->previewScene();
	const OBSSource program = mix_->programScene();
	const auto *canvasMix = qobject_cast<CanvasMix *>(mix_);
	const QString programName = program                                    ? qstr(obs_source_get_name(program))
				    : (canvasMix && canvasMix->fadedToBlack()) ? T("Panel.Black")
									       : T("Panel.NoScene");
	const QString previewName = preview ? qstr(obs_source_get_name(preview)) : T("Panel.NoScene");

	programLabel_->setText(T("Panel.ProgramNamed").arg(programName));
	previewLabel_->setText(mix_->previewFollowsProgram() ? T("Panel.PreviewIsProgram")
							     : T("Panel.PreviewNamed").arg(previewName));
	summaryLabel_->setText(mix_->previewFollowsProgram() ? T("Panel.ProgramNamed").arg(programName)
							     : T("Panel.Summary").arg(programName, previewName));
}

void MixPanel::refreshTransitions()
{
	const OBSSource effective = mix_->effectiveTransition();
	const bool isCut = !effective || strcmp(obs_source_get_id(effective), "cut_transition") == 0;
	const QString name = qstr(effective ? obs_source_get_name(effective) : nullptr);
	transitionButton_->setToolTip(T("Panel.TransitionButtonTooltip").arg(name));
	headerTransitionButton_->setToolTip(T("Panel.TransitionButtonTooltip").arg(name));
	/* Like OBS, manual transitions work for everything except a cut. */
	tbar_->setEnabled(!isCut);
	refreshQuickTransitions();
}

void MixPanel::refreshQuickTransitions()
{
	auto *canvasMix = qobject_cast<CanvasMix *>(mix_);
	/* OBS Main's Quick Transitions are OBS's own and live in the main window. */
	quickSection_->setVisible(canvasMix != nullptr);
	if (!canvasMix)
		return;

	QLayout *layout = quickButtons_->layout();
	while (QLayoutItem *item = layout->takeAt(0)) {
		if (QWidget *widget = item->widget())
			widget->deleteLater();
		delete item;
	}

	const auto &quick = canvasMix->quickTransitions();
	for (size_t i = 0; i < quick.size(); i++) {
		OBSSource source = canvasMix->quickTransitionSource(i);
		QString label = canvasMix->quickTransitionLabel(i);
		if (!source)
			label = T("Panel.QuickMissing").arg(qstr(quick[i].name.c_str()));

		auto *button = new QPushButton(label, quickButtons_);
		button->setEnabled(source != nullptr);
		if (quick[i].fadeToBlack) {
			/* Fade to Black stays visibly engaged while Program is held on black. */
			button->setCheckable(true);
			button->setChecked(canvasMix->fadedToBlack());
		}
		button->setToolTip(T("Panel.QuickTooltip"));
		button->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(button, &QPushButton::clicked, this, [this, canvasMix, i]() {
			canvasMix->quickTransition(i);
			refreshQuickTransitions();
			refreshTransitionState();
		});
		connect(button, &QWidget::customContextMenuRequested, this, [this, i, button](const QPoint &pos) {
			showQuickMenu(int(i), button, button->mapToGlobal(pos));
		});
		layout->addWidget(button);
	}
}

void MixPanel::refreshTransitionState()
{
	const bool transitioning = mix_->transitioning();
	if (transitioning && !stateTimer_.isActive())
		stateTimer_.start();
	else if (!transitioning && stateTimer_.isActive())
		stateTimer_.stop();

	if (!tbar_->isSliderDown()) {
		const QSignalBlocker blocker(tbar_);
		tbar_->setValue(mix_->tbarPosition());
	}

	const bool canAct = !transitioning && !mix_->previewFollowsProgram();
	transitionButton_->setEnabled(canAct);
	quickButtons_->setEnabled(!transitioning);
	headerTransitionButton_->setEnabled(canAct);
	headerCutButton_->setEnabled(canAct);
	refreshCue();
}

void MixPanel::refreshCue()
{
	{
		const QSignalBlocker blocker(globalCheck_);
		globalCheck_->setChecked(mix_->includedInGlobal());
	}

	const bool transitioning = mix_->transitioning();
	transitioningLabel_->setVisible(transitioning);
	const bool cued = !transitioning && mix_->cued();
	cueLabel_->setVisible(cued);
	cueLabel_->setText(mix_->includedInGlobal() ? T("Panel.Cued") : T("Panel.CuedExcluded"));
	cueLabel_->setToolTip(mix_->includedInGlobal() ? T("Panel.CuedTooltip") : T("Panel.CuedExcludedTooltip"));

	int live = 0;
	for (Destination *destination : manager_->destinationsFor(mix_->id())) {
		if (destination->active())
			live++;
	}
	if (mix_->isNative()) {
		for (const NativeOutput &output : nativeOutputs()) {
			if (output.active)
				live++;
		}
	}
	liveLabel_->setVisible(live > 0);
	liveLabel_->setToolTip(T("Panel.LiveTooltip").arg(live));
}

void MixPanel::refreshDestinations()
{
	outputsLayout_->clear();

	if (mix_->isNative()) {
		outputsLayout_->addWidget(classLabel(T("Panel.NativeOutputs"), "text-muted", outputsWidget_));
		for (const NativeOutput &output : nativeOutputs())
			outputsLayout_->addWidget(createNativeChip(output, outputsWidget_));
	}

	outputsLayout_->addWidget(classLabel(T("Panel.Outputs"), "text-muted", outputsWidget_));
	for (Destination *destination : manager_->destinationsFor(mix_->id())) {
		outputsLayout_->addWidget(createDestinationChip(manager_, destination, outputsWidget_));
		connect(destination, &Destination::changed, this, &MixPanel::refreshCue, Qt::UniqueConnection);
	}

	auto *add = iconButton("icon-plus", T("Panel.AddDestination"), outputsWidget_);
	connect(add, &QToolButton::clicked, this, [this, add]() {
		QMenu menu(this);
		addDestinationActions(&menu, manager_, mix_->id(), window());
		popupBelow(&menu, add);
	});
	outputsLayout_->addWidget(add);

	outputsWidget_->updateGeometry();
	refreshCue();
}

/* ------------------------------------------------------------------------- */
/* Actions */

void MixPanel::openSettings()
{
	auto *canvasMix = qobject_cast<CanvasMix *>(mix_);
	if (!canvasMix)
		return;
	MixSettingsDialog dialog(manager_, canvasMix, window());
	dialog.exec();
}

void MixPanel::confirmRemove()
{
	int active = 0;
	for (Destination *destination : manager_->destinationsFor(mix_->id())) {
		if (destination->active())
			active++;
	}
	QString message = T("Panel.RemoveConfirm").arg(mix_->name());
	if (active > 0)
		message += QStringLiteral("\n\n") + T("Panel.RemoveActive").arg(active);
	if (QMessageBox::question(window(), T("Panel.RemoveMix"), message) != QMessageBox::Yes)
		return;

	const QString id = mix_->id();
	Manager *manager = manager_;
	/* Removing the mix destroys this panel; defer until the menu has closed. */
	QMetaObject::invokeMethod(manager, [manager, id]() { manager->removeMix(id); }, Qt::QueuedConnection);
}

} // namespace dm
