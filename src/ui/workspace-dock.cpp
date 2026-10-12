/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "workspace-dock.hpp"

#include "mix-panel.hpp"

#include "manager.hpp"
#include "mix.hpp"

#include <obs-frontend-api.h>

#include <QComboBox>
#include <QDockWidget>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace dm {

namespace {

constexpr int kPanelMinWidth = 900;

std::string dockId(const QString &mixId)
{
	return "dual_mixer_mix_" + utf8(mixId);
}

} // namespace

WorkspaceDock::WorkspaceDock(Manager *manager, QWidget *parent) : QWidget(parent), manager_(manager)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);

	auto *toolbar = new QHBoxLayout();
	auto *addButton = new QPushButton(T("Workspace.AddMix"), this);
	addButton->setProperty("class", "icon-plus");
	addButton->setToolTip(T("Workspace.AddMixTooltip"));
	connect(addButton, &QPushButton::clicked, this, [this]() { manager_->addMix(); });

	columnsCombo_ = new QComboBox(this);
	columnsCombo_->addItem(T("Workspace.ColumnsAuto"), 0);
	for (int n = 1; n <= 4; n++)
		columnsCombo_->addItem(T("Workspace.Columns").arg(n), n);
	columnsCombo_->setToolTip(T("Workspace.ColumnsTooltip"));
	connect(columnsCombo_, &QComboBox::currentIndexChanged, this,
		[this](int index) { manager_->setColumns(columnsCombo_->itemData(index).toInt()); });

	auto *collapseAll = new QPushButton(T("Workspace.CollapseAll"), this);
	connect(collapseAll, &QPushButton::clicked, this, [this]() { setAllCollapsed(true); });
	auto *expandAll = new QPushButton(T("Workspace.ExpandAll"), this);
	connect(expandAll, &QPushButton::clicked, this, [this]() { setAllCollapsed(false); });

	exitFocusButton_ = new QPushButton(T("Workspace.ExitFocus"), this);
	connect(exitFocusButton_, &QPushButton::clicked, this, [this]() { manager_->setFocusedMix(QString()); });

	toolbar->addWidget(addButton);
	toolbar->addWidget(columnsCombo_);
	toolbar->addWidget(collapseAll);
	toolbar->addWidget(expandAll);
	toolbar->addWidget(exitFocusButton_);
	toolbar->addStretch(1);

	/* Global switch: every included mix with a freshly cued Preview goes to Program at once. */
	globalButton_ = new QPushButton(T("Workspace.GlobalButton"), this);
	QFont bold = globalButton_->font();
	bold.setBold(true);
	globalButton_->setFont(bold);
	globalButton_->setMinimumWidth(120);
	connect(globalButton_, &QPushButton::clicked, this, [this]() { manager_->transitionCued(); });
	toolbar->addWidget(globalButton_);
	layout->addLayout(toolbar);

	hintLabel_ = new QLabel(T("Workspace.Hint"), this);
	hintLabel_->setWordWrap(true);
	layout->addWidget(hintLabel_);

	scroll_ = new QScrollArea(this);
	scroll_->setWidgetResizable(true);
	scroll_->setFrameShape(QFrame::NoFrame);
	/* Displays are native surfaces; a native viewport makes the window system clip them to the
	 * visible area while scrolling instead of drawing over neighbouring widgets. */
	scroll_->viewport()->setAttribute(Qt::WA_NativeWindow);
	container_ = new QWidget(scroll_);
	grid_ = new QGridLayout(container_);
	grid_->setContentsMargins(0, 0, 0, 0);
	grid_->setSpacing(6);
	grid_->setAlignment(Qt::AlignTop);
	scroll_->setWidget(container_);
	layout->addWidget(scroll_, 1);

	connect(manager_, &Manager::mixAdded, this, &WorkspaceDock::sync);
	connect(manager_, &Manager::mixRemoved, this, &WorkspaceDock::removePanels);
	connect(manager_, &Manager::mixOrderChanged, this, &WorkspaceDock::sync);
	connect(manager_, &Manager::workspaceChanged, this, &WorkspaceDock::sync);
	connect(manager_, &Manager::collectionLoaded, this, &WorkspaceDock::sync);
	connect(manager_, &Manager::panelStateChanged, this, [this]() { sync(); });
	connect(manager_, &Manager::shuttingDown, this, &WorkspaceDock::releaseDisplays);
	connect(manager_, &Manager::cueChanged, this, &WorkspaceDock::refreshGlobal);
	connect(manager_, &Manager::collectionLoaded, this, &WorkspaceDock::refreshGlobal);
	connect(manager_, &Manager::mixOrderChanged, this, &WorkspaceDock::refreshGlobal);
	connect(manager_, &Manager::transitionSettingsChanged, this, &WorkspaceDock::refreshGlobal);

	/* Panels are created once the scene collection is loaded: the frontend API is not usable
	 * while modules are still loading. */
}

WorkspaceDock::~WorkspaceDock()
{
	releaseDisplays();
}

void WorkspaceDock::releaseDisplays()
{
	for (auto &[id, panel] : panels_) {
		if (panel)
			panel->detach();
	}
	for (auto &[id, panel] : docked_) {
		if (panel)
			panel->detach();
	}
}

MixPanel *WorkspaceDock::createPanel(Mix *mix, bool inOwnDock)
{
	auto *panel = new MixPanel(mix, manager_, inOwnDock, inOwnDock ? nullptr : container_);
	/* Queued: both actions can destroy the panel that emitted them. */
	connect(panel, &MixPanel::focusToggled, this, &WorkspaceDock::toggleFocus, Qt::QueuedConnection);
	connect(panel, &MixPanel::popOutToggled, this, &WorkspaceDock::togglePopOut, Qt::QueuedConnection);
	return panel;
}

void WorkspaceDock::removePanels(const QString &mixId)
{
	auto it = panels_.find(mixId);
	if (it != panels_.end()) {
		if (MixPanel *panel = it->second) {
			grid_->removeWidget(panel);
			panel->detach();
			panel->hide();
			panel->deleteLater();
		}
		panels_.erase(it);
	}
	closeOwnDock(mixId);
	relayout();
}

void WorkspaceDock::openOwnDock(Mix *mix)
{
	if (docked_.count(mix->id()) && docked_[mix->id()])
		return;

	/* Same native-viewport scroll area as the workspace, so displays stay clipped to the dock. */
	auto *scroll = new QScrollArea();
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->viewport()->setAttribute(Qt::WA_NativeWindow);
	MixPanel *panel = createPanel(mix, true);
	scroll->setWidget(panel);

	const std::string id = dockId(mix->id());
	if (!obs_frontend_add_dock_by_id(id.c_str(), utf8(mix->name()).c_str(), scroll)) {
		delete scroll;
		return;
	}
	docked_[mix->id()] = panel;

	auto *dock = qobject_cast<QDockWidget *>(scroll->parentWidget());
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!dock)
		return;
	/* Put the dock back where the user last had it, if OBS's saved layout knows it. */
	const bool restored = main && main->restoreDockWidget(dock);
	dock->show();
	dock->raise();
	if (!restored || dock->width() < 400) {
		dock->setFloating(true);
		QTimer::singleShot(0, dock, [dock]() { dock->resize(1000, 440); });
	}
}

void WorkspaceDock::closeOwnDock(const QString &mixId)
{
	auto it = docked_.find(mixId);
	if (it == docked_.end())
		return;
	if (MixPanel *panel = it->second)
		panel->detach();
	docked_.erase(it);
	/* Deletes the dock and the panel inside it. */
	obs_frontend_remove_dock(dockId(mixId).c_str());
}

void WorkspaceDock::togglePopOut(const QString &mixId)
{
	Manager::PanelState state = manager_->panelState(mixId);
	state.poppedOut = !state.poppedOut;
	if (state.poppedOut && manager_->focusedMix() == mixId)
		manager_->setFocusedMix(QString());
	manager_->setPanelState(mixId, state);
}

void WorkspaceDock::toggleFocus(const QString &mixId)
{
	manager_->setFocusedMix(manager_->focusedMix() == mixId ? QString() : mixId);
}

void WorkspaceDock::setAllCollapsed(bool collapsed)
{
	for (Mix *mix : manager_->mixes()) {
		Manager::PanelState state = manager_->panelState(mix->id());
		state.collapsed = collapsed;
		manager_->setPanelState(mix->id(), state);
	}
}

void WorkspaceDock::sync()
{
	const QSignalBlocker blocker(columnsCombo_);
	columnsCombo_->setCurrentIndex(std::max(0, columnsCombo_->findData(manager_->columns())));

	for (Mix *mix : manager_->mixes()) {
		const bool poppedOut = manager_->panelState(mix->id()).poppedOut;
		if (poppedOut) {
			auto it = panels_.find(mix->id());
			if (it != panels_.end()) {
				if (MixPanel *panel = it->second) {
					grid_->removeWidget(panel);
					panel->detach();
					panel->hide();
					panel->deleteLater();
				}
				panels_.erase(it);
			}
			openOwnDock(mix);
		} else {
			closeOwnDock(mix->id());
			if (!panels_.count(mix->id()) || !panels_[mix->id()])
				panels_[mix->id()] = createPanel(mix, false);
		}
	}
	relayout();
}

void WorkspaceDock::refreshGlobal()
{
	const std::vector<Mix *> cued = manager_->cuedMixes();
	QStringList names;
	for (Mix *mix : cued)
		names << mix->name().toHtmlEscaped();
	globalButton_->setEnabled(!cued.empty());

	OBSSourceAutoRelease transition = obs_frontend_get_current_transition();
	const QString transitionName = qstr(transition ? obs_source_get_name(transition) : nullptr).toHtmlEscaped();
	const QString cuedText = names.isEmpty() ? T("Workspace.NothingCued")
						 : T("Workspace.Cued").arg(names.join(QStringLiteral(", ")));
	globalButton_->setToolTip(QStringLiteral("%1<br><br>%2<br>%3")
					  .arg(T("Workspace.GlobalTooltip"), cuedText,
					       T("Workspace.GlobalTransition")
						       .arg(transitionName)
						       .arg(obs_frontend_get_transition_duration())));
}

int WorkspaceDock::effectiveColumns() const
{
	if (manager_->columns() > 0)
		return manager_->columns();
	return std::max(1, scroll_->viewport()->width() / kPanelMinWidth);
}

void WorkspaceDock::relayout()
{
	while (grid_->count() > 0)
		grid_->takeAt(0);
	for (int c = 0; c < grid_->columnCount(); c++)
		grid_->setColumnStretch(c, 0);
	for (int r = 0; r < grid_->rowCount(); r++)
		grid_->setRowStretch(r, 0);

	std::vector<MixPanel *> visible;
	for (Mix *mix : manager_->mixes()) {
		auto it = panels_.find(mix->id());
		if (it != panels_.end() && it->second)
			visible.push_back(it->second);
	}

	const QString focused = manager_->focusedMix();
	const bool focusActive =
		std::any_of(visible.begin(), visible.end(), [&](MixPanel *p) { return p->mix()->id() == focused; });
	exitFocusButton_->setVisible(focusActive);
	hintLabel_->setVisible(manager_->mixes().size() <= 1);

	const int columns = focusActive ? 1 : std::max(1, std::min(effectiveColumns(), int(visible.size())));
	lastColumns_ = columns;
	int row = 0;
	int column = 0;

	if (focusActive) {
		/* Focus: the chosen mix full width on top, every other mix as a compact strip below. */
		for (MixPanel *panel : visible) {
			if (panel->mix()->id() == focused) {
				panel->setForcedCompact(false);
				grid_->addWidget(panel, row++, 0);
			}
		}
		for (MixPanel *panel : visible) {
			if (panel->mix()->id() != focused) {
				panel->setForcedCompact(true);
				grid_->addWidget(panel, row++, 0);
			}
		}
	} else {
		for (MixPanel *panel : visible) {
			panel->setForcedCompact(false);
			grid_->addWidget(panel, row, column);
			if (++column >= columns) {
				column = 0;
				row++;
			}
		}
	}

	for (int c = 0; c < columns; c++)
		grid_->setColumnStretch(c, 1);
	grid_->setRowStretch(row + 1, 1);
	for (MixPanel *panel : visible)
		panel->show();
}

void WorkspaceDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (manager_->columns() == 0 && manager_->focusedMix().isEmpty() && effectiveColumns() != lastColumns_)
		relayout();
}

} // namespace dm
