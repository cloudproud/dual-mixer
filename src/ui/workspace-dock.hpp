/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

#include <map>

class QComboBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QScrollArea;

namespace dm {

class Manager;
class Mix;
class MixPanel;

/*
 * The "Mixes" dock: every mix as a panel in a grid. Panels can be reordered, collapsed, focused
 * (one large, the rest compact) or popped out into their own OBS dock.
 */
class WorkspaceDock : public QWidget {
	Q_OBJECT

public:
	explicit WorkspaceDock(Manager *manager, QWidget *parent = nullptr);
	~WorkspaceDock() override;

protected:
	void resizeEvent(QResizeEvent *event) override;

private:
	void sync();
	void refreshGlobal();
	void relayout();
	MixPanel *createPanel(Mix *mix, bool inOwnDock);
	void removePanels(const QString &mixId);
	void openOwnDock(Mix *mix);
	void closeOwnDock(const QString &mixId);
	void togglePopOut(const QString &mixId);
	void toggleFocus(const QString &mixId);
	void setAllCollapsed(bool collapsed);
	void releaseDisplays();
	int effectiveColumns() const;

	Manager *manager_;
	QScrollArea *scroll_ = nullptr;
	QWidget *container_ = nullptr;
	QGridLayout *grid_ = nullptr;
	QComboBox *columnsCombo_ = nullptr;
	QPushButton *globalButton_ = nullptr;
	QPushButton *exitFocusButton_ = nullptr;
	QLabel *hintLabel_ = nullptr;
	int lastColumns_ = -1;

	std::map<QString, QPointer<MixPanel>> panels_;
	std::map<QString, QPointer<MixPanel>> docked_;
};

} // namespace dm
