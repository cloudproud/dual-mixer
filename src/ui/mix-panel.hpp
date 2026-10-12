/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QFrame>
#include <QTimer>

class QCheckBox;
class QLabel;
class QMenu;
class QPushButton;
class QSlider;
class QToolButton;
class QWidget;

namespace dm {

class DisplayWidget;
class FlowLayout;
class Manager;
class Mix;
class SceneList;

/*
 * Operator row for one mix, laid out like OBS's Studio Mode:
 * scene list | Preview | transition controls | Program, with output status underneath.
 */
class MixPanel : public QFrame {
	Q_OBJECT

public:
	MixPanel(Mix *mix, Manager *manager, bool inOwnDock, QWidget *parent = nullptr);
	~MixPanel() override;

	Mix *mix() const { return mix_; }
	/* Never report less than the space the pinned-height displays need. */
	QSize sizeHint() const override { return QFrame::sizeHint().expandedTo(minimumSizeHint()); }
	/* Forces the compact (collapsed) presentation without changing the saved state. */
	void setForcedCompact(bool compact);
	/* Stops rendering and disconnects from the mix; call before the mix is destroyed.
	 * Idempotent: later calls (including the destructor's) are no-ops. */
	void detach();

signals:
	void focusToggled(const QString &mixId);
	void popOutToggled(const QString &mixId);

private:
	void buildHeader();
	void buildBody();
	QWidget *buildControls();
	void fillTransitionMenu(QMenu *menu);
	void fillQuickMenu(QMenu *menu, int index);
	void showQuickMenu(int index, QWidget *anchor, const QPoint &globalPos);
	void applyCompact();
	void refreshName();
	void refreshScenes();
	void refreshTally();
	void refreshTransitions();
	void refreshQuickTransitions();
	void refreshTransitionState();
	void refreshCue();
	void refreshDestinations();
	void refreshNative();
	void refreshFocus();
	void openSettings();
	void confirmRemove();
	bool compact() const;

	Mix *mix_;
	Manager *manager_;
	bool inOwnDock_;
	bool forcedCompact_ = false;
	bool detached_ = false;

	QCheckBox *collapseCheck_ = nullptr;
	QLabel *nameLabel_ = nullptr;
	QLabel *transitioningLabel_ = nullptr;
	QLabel *cueLabel_ = nullptr;
	QLabel *liveLabel_ = nullptr;
	QLabel *summaryLabel_ = nullptr;
	QCheckBox *globalCheck_ = nullptr;
	QPushButton *headerCutButton_ = nullptr;
	QPushButton *headerTransitionButton_ = nullptr;
	QToolButton *moveUpButton_ = nullptr;
	QToolButton *moveDownButton_ = nullptr;
	QToolButton *focusButton_ = nullptr;
	QToolButton *popButton_ = nullptr;
	QToolButton *menuButton_ = nullptr;

	QWidget *body_ = nullptr;
	SceneList *sceneList_ = nullptr;
	DisplayWidget *previewDisplay_ = nullptr;
	DisplayWidget *programDisplay_ = nullptr;
	QLabel *previewLabel_ = nullptr;
	QLabel *programLabel_ = nullptr;
	QWidget *studioNotice_ = nullptr;
	QPushButton *transitionButton_ = nullptr;
	QWidget *quickSection_ = nullptr;
	QWidget *quickButtons_ = nullptr;
	QSlider *tbar_ = nullptr;
	QWidget *outputsWidget_ = nullptr;
	FlowLayout *outputsLayout_ = nullptr;

	QTimer stateTimer_;
};

} // namespace dm
