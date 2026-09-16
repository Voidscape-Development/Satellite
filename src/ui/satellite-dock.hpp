/*
Satellite - NDI and OMT transport for OBS Studio
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <QWidget>

class QLabel;
class QTimer;
class QTreeWidget;
class QVBoxLayout;

namespace satellite {

/// The Satellite window: runtime status for each protocol, and a live table of every feed
/// in either direction.
///
/// It samples FeedRegistry and DiscoveryService snapshots on a timer. It never calls into a
/// backend and never holds a backend lock, so a stalled network cannot freeze the UI.
class SatelliteDock : public QWidget {
	Q_OBJECT

public:
	explicit SatelliteDock(QWidget *parent = nullptr);

private slots:
	void refresh();

private:
	void buildRuntimeStatus(QVBoxLayout *layout);
	void buildFeedTable(QVBoxLayout *layout);
	void refreshRuntimeStatus();
	void refreshFeedTable();

	QLabel *ndiStatus_ = nullptr;
	QLabel *omtStatus_ = nullptr;
	QLabel *summary_ = nullptr;
	QTreeWidget *feeds_ = nullptr;
	QTimer *timer_ = nullptr;
};

/// Creates the dock, registers it with the frontend, and adds the Tools menu entry that
/// shows and raises it.
void register_satellite_dock();

/// Drops references held by the dock. Called on frontend exit.
void unregister_satellite_dock();

} // namespace satellite
