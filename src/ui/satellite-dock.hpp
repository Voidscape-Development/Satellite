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

#include "obs/satellite-output.hpp"

#include <QFrame>
#include <QString>
#include <QWidget>

#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace satellite {

struct CanvasSenderConfig;
struct OutputConfig;
class FeedTable;

/// The card for one of the two main-mix senders: enable, name, protocol, audio, and what it
/// is doing right now.
///
/// The Program and Preview senders are configured here rather than in a source's properties,
/// because they belong to the whole OBS instance rather than to any one scene item.
class OutputControls : public QFrame {
	Q_OBJECT

public:
	OutputControls(const QString &title, QWidget *parent = nullptr);

	void load(const OutputConfig &config);
	void store(OutputConfig &config) const;
	void setStatus(const SenderStatus &status);

signals:
	void changed();

private:
	QCheckBox *enabled_ = nullptr;
	QLineEdit *name_ = nullptr;
	QComboBox *protocol_ = nullptr;
	QCheckBox *audio_ = nullptr;
	QLabel *status_ = nullptr;
};

/// The card for one canvas sender: which canvas, name, protocol, and which mixer track (if
/// any) to carry as its audio.
class CanvasSenderControls : public QFrame {
	Q_OBJECT

public:
	explicit CanvasSenderControls(int id, QWidget *parent = nullptr);

	int id() const { return id_; }

	/// Repopulates the canvas picker, keeping the current choice - shown as missing if it
	/// is not among the canvases that exist right now.
	void setCanvases(const std::vector<CanvasInfo> &canvases);

	void load(const CanvasSenderConfig &config);
	void store(CanvasSenderConfig &config) const;
	void setStatus(const SenderStatus &status);
	void focusName();

signals:
	void changed();
	void removeRequested(int id);

private:
	void selectCanvas(const QString &uuid, const QString &name);
	void updateEnabled();

	const int id_;
	std::vector<CanvasInfo> canvases_;

	QCheckBox *enabled_ = nullptr;
	QComboBox *canvas_ = nullptr;
	QLineEdit *name_ = nullptr;
	QComboBox *protocol_ = nullptr;
	QComboBox *audio_ = nullptr;
	QLabel *status_ = nullptr;
	QToolButton *remove_ = nullptr;
};

/// The Satellite window.
///
/// A glanceable status strip - each protocol's runtime, and how many feeds are moving in
/// each direction - over three tabs: **Feeds**, the live grid that gets most of the space;
/// **Senders**, for Program, Preview and any canvases; and **Settings**, for what is set once
/// and rarely touched.
///
/// It samples FeedRegistry and DiscoveryService snapshots on a timer. It never calls into a
/// backend and never holds a backend lock, so a stalled network cannot freeze the UI.
class SatelliteDock : public QWidget {
	Q_OBJECT

public:
	explicit SatelliteDock(QWidget *parent = nullptr);

private slots:
	void refresh();
	void applyOutputSettings();
	void applyCanvasSettings();
	void addCanvasSender();
	void removeCanvasSender(int id);
	void applyAdvancedSettings();
	void recheckRuntimes();
	void importFromDistroAV();
	void saveFeedColumns();

private:
	void buildStatusStrip(QVBoxLayout *layout);
	QWidget *buildFeedsTab();
	QWidget *buildSendersTab();
	QWidget *buildSettingsTab();
	void rebuildCanvasControls();

	void refreshRuntimeStatus();
	void refreshFeeds();
	void refreshSenders();

	QTabWidget *tabs_ = nullptr;
	QTimer *timer_ = nullptr;

	// Status strip
	QLabel *ndiChip_ = nullptr;
	QLabel *omtChip_ = nullptr;
	QLabel *activity_ = nullptr;
	QFrame *banner_ = nullptr;
	QLabel *bannerText_ = nullptr;
	QPushButton *bannerRecheck_ = nullptr;
	/// The banner's text when it was dismissed: it stays hidden until something changes.
	QString dismissedBanner_;

	// Feeds
	QStackedWidget *feedStack_ = nullptr;
	FeedTable *feeds_ = nullptr;
	QLabel *summary_ = nullptr;

	// Senders
	OutputControls *program_ = nullptr;
	OutputControls *preview_ = nullptr;
	QVBoxLayout *canvasList_ = nullptr;
	std::vector<CanvasSenderControls *> canvasControls_;
	QLabel *canvasHint_ = nullptr;
	QPushButton *addCanvas_ = nullptr;
	/// Last canvas list shown, so the pickers are only rebuilt when it changes.
	std::string canvasSignature_;

	// Settings
	QLabel *ndiDetail_ = nullptr;
	QLabel *omtDetail_ = nullptr;
	QPushButton *settingsRecheck_ = nullptr;
	QLineEdit *ndiGroups_ = nullptr;
	QLineEdit *omtDiscoveryServer_ = nullptr;
	QSpinBox *omtPortStart_ = nullptr;
	QSpinBox *omtPortEnd_ = nullptr;
};

/// Creates the dock, registers it with the frontend, and adds the Tools menu entry that
/// shows and raises it.
void register_satellite_dock();

/// Drops references held by the dock. Called on frontend exit.
void unregister_satellite_dock();

} // namespace satellite
