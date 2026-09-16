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

#include "ui/satellite-dock.hpp"

#include "discovery/discovery-service.hpp"
#include "metrics/feed-registry.hpp"
#include "transport/transport.hpp"
#include "ui/sparkline.hpp"

#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

namespace satellite {

namespace {

/// The Satellite window refreshes on this cadence. Note that this throttles the *display*,
/// not discovery - the discovery thread keeps long-lived finders and is not duty-cycled.
/// See docs/ARCHITECTURE.md section 2.1.
constexpr int kRefreshIntervalMs = 1000;

constexpr const char *kDockId = "satellite_dock";

SatelliteDock *g_dock = nullptr;

enum FeedColumn {
	ColumnName = 0,
	ColumnProtocol,
	ColumnDirection,
	ColumnState,
	ColumnVideo,
	ColumnAudio,
	ColumnBitrate,
	ColumnDropped,
	ColumnTally,
	ColumnHistory,
	ColumnCount,
};

QString tally_text(const Tally &tally)
{
	if (tally.program)
		return QStringLiteral("PGM");
	if (tally.preview)
		return QStringLiteral("PVW");
	return QString();
}

} // namespace

SatelliteDock::SatelliteDock(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("SatelliteDock"));

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(6);

	buildRuntimeStatus(layout);
	buildFeedTable(layout);

	timer_ = new QTimer(this);
	timer_->setInterval(kRefreshIntervalMs);
	connect(timer_, &QTimer::timeout, this, &SatelliteDock::refresh);
	timer_->start();

	refresh();
}

void SatelliteDock::buildRuntimeStatus(QVBoxLayout *layout)
{
	auto *group = new QGroupBox(obs_module_text("Satellite.Dock.Runtimes"), this);
	auto *group_layout = new QVBoxLayout(group);
	group_layout->setContentsMargins(8, 6, 8, 6);
	group_layout->setSpacing(2);

	ndiStatus_ = new QLabel(group);
	ndiStatus_->setTextFormat(Qt::RichText);
	ndiStatus_->setOpenExternalLinks(true);
	ndiStatus_->setWordWrap(true);

	omtStatus_ = new QLabel(group);
	omtStatus_->setTextFormat(Qt::RichText);
	omtStatus_->setOpenExternalLinks(true);
	omtStatus_->setWordWrap(true);

	group_layout->addWidget(ndiStatus_);
	group_layout->addWidget(omtStatus_);

	layout->addWidget(group);
}

void SatelliteDock::buildFeedTable(QVBoxLayout *layout)
{
	feeds_ = new QTreeWidget(this);
	feeds_->setRootIsDecorated(false);
	feeds_->setAlternatingRowColors(true);
	feeds_->setUniformRowHeights(false);
	feeds_->setSelectionMode(QAbstractItemView::SingleSelection);
	feeds_->setColumnCount(ColumnCount);

	QStringList headers;
	headers << obs_module_text("Satellite.Column.Name") << obs_module_text("Satellite.Column.Protocol")
		<< obs_module_text("Satellite.Column.Direction") << obs_module_text("Satellite.Column.State")
		<< obs_module_text("Satellite.Column.Video") << obs_module_text("Satellite.Column.Audio")
		<< obs_module_text("Satellite.Column.Bitrate") << obs_module_text("Satellite.Column.Dropped")
		<< obs_module_text("Satellite.Column.Tally") << obs_module_text("Satellite.Column.History");
	feeds_->setHeaderLabels(headers);
	feeds_->header()->setSectionResizeMode(ColumnName, QHeaderView::Stretch);

	summary_ = new QLabel(this);

	layout->addWidget(feeds_, 1);
	layout->addWidget(summary_);
}

void SatelliteDock::refresh()
{
	refreshRuntimeStatus();
	refreshFeedTable();
}

void SatelliteDock::refreshRuntimeStatus()
{
	for (IBackend *backend : all_backends()) {
		QLabel *label = backend->protocol() == Protocol::NDI ? ndiStatus_ : omtStatus_;
		if (!label)
			continue;

		const QString name = QString::fromUtf8(protocol_display_name(backend->protocol()));

		if (backend->available()) {
			const QString version = QString::fromStdString(backend->runtime_version());
			label->setText(
				QStringLiteral("<b>%1</b>: %2").arg(name, version.isEmpty() ? tr("ready") : version));
			continue;
		}

		QString text =
			QStringLiteral("<b>%1</b>: %2")
				.arg(name, QString::fromStdString(backend->unavailable_reason()).toHtmlEscaped());

		// NDI is proprietary and cannot be bundled, so when it is missing the only useful
		// thing we can offer is where to get it. OMT ships with Satellite and has no URL.
		const QString url = QString::fromStdString(backend->install_url());
		if (!url.isEmpty())
			text += QStringLiteral(" <a href=\"%1\">%2</a>").arg(url, tr("Install"));

		label->setText(text);
	}
}

void SatelliteDock::refreshFeedTable()
{
	const std::vector<FeedSnapshot> feeds = FeedRegistry::instance().snapshot();

	while (feeds_->topLevelItemCount() > static_cast<int>(feeds.size()))
		delete feeds_->takeTopLevelItem(feeds_->topLevelItemCount() - 1);

	for (size_t index = 0; index < feeds.size(); ++index) {
		const FeedSnapshot &feed = feeds[index];

		QTreeWidgetItem *item = nullptr;
		if (static_cast<int>(index) < feeds_->topLevelItemCount()) {
			item = feeds_->topLevelItem(static_cast<int>(index));
		} else {
			item = new QTreeWidgetItem(feeds_);
			feeds_->setItemWidget(item, ColumnHistory, new Sparkline(feeds_));
		}

		item->setText(ColumnName, QString::fromStdString(feed.name));
		item->setText(ColumnProtocol, QString::fromUtf8(protocol_display_name(feed.protocol)));
		item->setText(ColumnDirection, feed.direction == FeedDirection::Send ? tr("Send") : tr("Receive"));
		item->setText(ColumnState, QString::fromUtf8(feed_state_display_name(feed.state)));
		item->setText(ColumnVideo, QString::fromStdString(feed.video_format));
		item->setText(ColumnAudio, QString::fromStdString(feed.audio_format));

		// NDI exposes no byte counters, so its bitrate is measured by us from frame sizes.
		// Marking it keeps the number honest next to OMT's exact figure.
		QString bitrate = QStringLiteral("%1 Mb/s").arg(feed.stats.bitrate_mbps, 0, 'f', 1);
		if (feed.stats.bitrate_estimated)
			bitrate.prepend(QStringLiteral("~"));
		item->setText(ColumnBitrate, bitrate);

		item->setText(ColumnDropped, QString::number(feed.stats.frames_dropped));
		item->setText(ColumnTally, tally_text(feed.tally));

		if (auto *sparkline = qobject_cast<Sparkline *>(feeds_->itemWidget(item, ColumnHistory))) {
			QVector<double> values;
			values.reserve(static_cast<int>(feed.bitrate_history.size()));
			for (double value : feed.bitrate_history)
				values.append(value);
			sparkline->setValues(values);
		}
	}

	const size_t discovered = DiscoveryService::instance().sources().size();
	summary_->setText(tr("%n feed(s) active", "", static_cast<int>(feeds.size())) + QStringLiteral(" — ") +
			  tr("%n source(s) on the network", "", static_cast<int>(discovered)));
}

namespace {

void on_tools_menu_clicked(void *)
{
	if (!g_dock)
		return;

	// The dock lives inside a QDockWidget created by the frontend, so raising it means
	// raising that parent, not the content widget.
	if (QWidget *container = g_dock->parentWidget()) {
		container->setVisible(true);
		container->raise();
	} else {
		g_dock->setVisible(true);
		g_dock->raise();
	}
}

} // namespace

void register_satellite_dock()
{
	if (g_dock)
		return;

	g_dock = new SatelliteDock();

	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("Satellite.Dock.Title"), g_dock)) {
		obs_log(LOG_WARNING, "could not register the Satellite dock");
		delete g_dock;
		g_dock = nullptr;
		return;
	}

	obs_frontend_add_tools_menu_item(obs_module_text("Satellite.Menu"), on_tools_menu_clicked, nullptr);
}

void unregister_satellite_dock()
{
	// The frontend owns the dock widget once it has been registered, so this only drops our
	// reference to it.
	g_dock = nullptr;
}

} // namespace satellite
