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

#include <algorithm>

#include "config/config.hpp"
#include "discovery/discovery-service.hpp"
#include "metrics/feed-registry.hpp"
#include "obs/satellite-output.hpp"
#include "transport/transport.hpp"
#include "ui/import-dialog.hpp"
#include "ui/sparkline.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
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
	ColumnConnections,
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

OutputControls::OutputControls(const QString &title, QWidget *parent) : QWidget(parent)
{
	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(6);

	enabled_ = new QCheckBox(title, this);
	enabled_->setMinimumWidth(70);

	name_ = new QLineEdit(this);
	name_->setPlaceholderText(obs_module_text("Satellite.Output.NamePlaceholder"));

	protocol_ = new QComboBox(this);
	for (Protocol value : kAllProtocols)
		protocol_->addItem(QString::fromUtf8(protocol_display_name(value)),
				   QString::fromUtf8(protocol_id(value)));

	audio_ = new QCheckBox(obs_module_text("Satellite.Output.Audio"), this);

	layout->addWidget(enabled_);
	layout->addWidget(name_, 1);
	layout->addWidget(protocol_);
	layout->addWidget(audio_);

	// The name is meaningless while the sender is off, so grey it out rather than letting
	// someone type into a field that does nothing.
	connect(enabled_, &QCheckBox::toggled, this, [this](bool on) {
		name_->setEnabled(on);
		protocol_->setEnabled(on);
		audio_->setEnabled(on);
		emit changed();
	});

	connect(name_, &QLineEdit::editingFinished, this, &OutputControls::changed);
	connect(protocol_, &QComboBox::currentIndexChanged, this, &OutputControls::changed);
	connect(audio_, &QCheckBox::toggled, this, &OutputControls::changed);
}

void OutputControls::load(const OutputConfig &config)
{
	// Block signals so populating the widgets does not look like the user editing them,
	// which would write the config back and restart the sender on every refresh.
	const QSignalBlocker block_enabled(enabled_);
	const QSignalBlocker block_name(name_);
	const QSignalBlocker block_protocol(protocol_);
	const QSignalBlocker block_audio(audio_);

	enabled_->setChecked(config.enabled);
	name_->setText(QString::fromStdString(config.name));
	audio_->setChecked(config.send_audio);

	const int index = protocol_->findData(QString::fromUtf8(protocol_id(config.protocol)));
	if (index >= 0)
		protocol_->setCurrentIndex(index);

	name_->setEnabled(config.enabled);
	protocol_->setEnabled(config.enabled);
	audio_->setEnabled(config.enabled);
}

void OutputControls::store(OutputConfig &config) const
{
	config.enabled = enabled_->isChecked();
	config.name = name_->text().toStdString();
	config.send_audio = audio_->isChecked();

	Protocol protocol = Protocol::NDI;
	if (protocol_from_id(protocol_->currentData().toString().toUtf8().constData(), protocol))
		config.protocol = protocol;
}

SatelliteDock::SatelliteDock(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("SatelliteDock"));

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(6);

	buildRuntimeStatus(layout);
	buildOutputControls(layout);
	buildAdvancedSettings(layout);
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

	auto *buttons = new QHBoxLayout();
	buttons->setContentsMargins(0, 2, 0, 0);

	// Installing the NDI runtime usually means quitting OBS and coming back. It does not
	// have to: nothing is loaded yet when a runtime is missing, so retrying is safe, and
	// this turns a restart into a click.
	recheck_ = new QPushButton(obs_module_text("Satellite.Dock.Recheck"), group);
	recheck_->setToolTip(obs_module_text("Satellite.Dock.Recheck.Hint"));
	connect(recheck_, &QPushButton::clicked, this, &SatelliteDock::recheckRuntimes);

	auto *import = new QPushButton(obs_module_text("Satellite.Dock.Import"), group);
	import->setToolTip(obs_module_text("Satellite.Dock.Import.Hint"));
	connect(import, &QPushButton::clicked, this, &SatelliteDock::importFromDistroAV);

	buttons->addWidget(recheck_);
	buttons->addWidget(import);
	buttons->addStretch(1);
	group_layout->addLayout(buttons);

	layout->addWidget(group);
}

void SatelliteDock::recheckRuntimes()
{
	// Only ever retried for a backend that is not available. A loaded backend is in use by
	// the discovery thread and by any live source, so reloading it underneath them would be
	// a crash rather than a refresh.
	int loaded = 0;
	for (IBackend *backend : all_backends()) {
		if (backend->available())
			continue;
		if (backend->load())
			++loaded;
	}

	refreshRuntimeStatus();

	if (loaded > 0)
		obs_log(LOG_INFO, "%d protocol(s) became available after a re-check", loaded);
}

void SatelliteDock::importFromDistroAV()
{
	if (!run_distroav_import(this)) {
		QMessageBox::information(this, obs_module_text("Satellite.Import.Title"),
					 obs_module_text("Satellite.Import.NothingFound"));
	}
}

void SatelliteDock::buildOutputControls(QVBoxLayout *layout)
{
	auto *group = new QGroupBox(obs_module_text("Satellite.Dock.Outputs"), this);
	auto *group_layout = new QVBoxLayout(group);
	group_layout->setContentsMargins(8, 6, 8, 6);
	group_layout->setSpacing(4);

	program_ = new OutputControls(obs_module_text("Satellite.Output.Program"), group);
	preview_ = new OutputControls(obs_module_text("Satellite.Output.Preview"), group);

	const Config &config = Config::instance();
	program_->load(config.program);
	preview_->load(config.preview);

	connect(program_, &OutputControls::changed, this, &SatelliteDock::applyOutputSettings);
	connect(preview_, &OutputControls::changed, this, &SatelliteDock::applyOutputSettings);

	auto *note = new QLabel(obs_module_text("Satellite.Output.PreviewNote"), group);
	note->setWordWrap(true);
	QFont note_font = note->font();
	note_font.setPointSizeF(note_font.pointSizeF() * 0.9);
	note->setFont(note_font);
	note->setEnabled(false);

	group_layout->addWidget(program_);
	group_layout->addWidget(preview_);
	group_layout->addWidget(note);

	layout->addWidget(group);
}

void SatelliteDock::applyOutputSettings()
{
	Config &config = Config::instance();

	program_->store(config.program);
	preview_->store(config.preview);

	config.save();
	update_frontend_outputs();
}

void SatelliteDock::buildAdvancedSettings(QVBoxLayout *layout)
{
	auto *group = new QGroupBox(obs_module_text("Satellite.Dock.Advanced"), this);
	group->setCheckable(true);
	group->setChecked(false);

	auto *form = new QFormLayout(group);
	form->setContentsMargins(8, 6, 8, 6);

	const Config &config = Config::instance();

	ndiGroups_ = new QLineEdit(QString::fromStdString(config.ndi_groups), group);
	ndiGroups_->setPlaceholderText(obs_module_text("Satellite.Advanced.NdiGroups.Placeholder"));
	ndiGroups_->setToolTip(obs_module_text("Satellite.Advanced.NdiGroups.Hint"));

	omtDiscoveryServer_ = new QLineEdit(QString::fromStdString(config.omt_discovery_server), group);
	omtDiscoveryServer_->setPlaceholderText(obs_module_text("Satellite.Advanced.OmtServer.Placeholder"));
	omtDiscoveryServer_->setToolTip(obs_module_text("Satellite.Advanced.OmtServer.Hint"));

	omtPortStart_ = new QSpinBox(group);
	omtPortStart_->setRange(1024, 65535);
	omtPortStart_->setValue(config.omt_port_start);

	omtPortEnd_ = new QSpinBox(group);
	omtPortEnd_->setRange(1024, 65535);
	omtPortEnd_->setValue(config.omt_port_end);

	auto *ports = new QHBoxLayout();
	ports->setContentsMargins(0, 0, 0, 0);
	ports->addWidget(omtPortStart_);
	ports->addWidget(new QLabel(QStringLiteral("–"), group));
	ports->addWidget(omtPortEnd_);
	ports->addStretch(1);

	form->addRow(obs_module_text("Satellite.Advanced.NdiGroups"), ndiGroups_);
	form->addRow(obs_module_text("Satellite.Advanced.OmtServer"), omtDiscoveryServer_);
	form->addRow(obs_module_text("Satellite.Advanced.OmtPorts"), ports);

	// editingFinished rather than textChanged: applying on every keystroke would rebuild the
	// NDI finder once per character.
	connect(ndiGroups_, &QLineEdit::editingFinished, this, &SatelliteDock::applyAdvancedSettings);
	connect(omtDiscoveryServer_, &QLineEdit::editingFinished, this, &SatelliteDock::applyAdvancedSettings);
	connect(omtPortStart_, &QSpinBox::editingFinished, this, &SatelliteDock::applyAdvancedSettings);
	connect(omtPortEnd_, &QSpinBox::editingFinished, this, &SatelliteDock::applyAdvancedSettings);

	layout->addWidget(group);
}

void SatelliteDock::applyAdvancedSettings()
{
	Config &config = Config::instance();

	config.ndi_groups = ndiGroups_->text().toStdString();
	config.omt_discovery_server = omtDiscoveryServer_->text().toStdString();
	config.omt_port_start = omtPortStart_->value();
	config.omt_port_end = omtPortEnd_->value();

	config.save();

	// Backends decide for themselves what can be applied live and what has to wait for a
	// safe moment on another thread.
	for (IBackend *backend : all_backends())
		backend->settings_changed();
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
		<< obs_module_text("Satellite.Column.Connections") << obs_module_text("Satellite.Column.Tally")
		<< obs_module_text("Satellite.Column.History");
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
	bool any_unavailable = false;

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

	if (recheck_)
		recheck_->setEnabled(any_unavailable);
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
		item->setText(ColumnConnections,
			      feed.stats.connections >= 0 ? QString::number(feed.stats.connections) : QString());
		item->setText(ColumnTally, tally_text(feed.tally));

		// Everything else worth knowing goes in the row's tooltip. A dock is narrow, and a
		// column each for fps, codec time and peak bitrate would make the table unreadable
		// for numbers most people look at once.
		QStringList detail;
		detail << tr("Protocol: %1").arg(QString::fromUtf8(protocol_display_name(feed.protocol)));
		if (feed.stats.fps > 0.0)
			detail << tr("Rate: %1 fps").arg(feed.stats.fps, 0, 'f', 1);
		detail << tr("Frames: %1 total, %2 dropped").arg(feed.stats.frames).arg(feed.stats.frames_dropped);
		if (feed.stats.codec_ms >= 0.0)
			detail << tr("Codec: %1 ms/frame").arg(feed.stats.codec_ms, 0, 'f', 1);
		detail << (feed.stats.bitrate_estimated ? tr("Bitrate is measured locally, the protocol reports no "
							     "byte counters")
							: tr("Bitrate is reported exactly by the protocol"));

		if (!feed.bitrate_history.empty()) {
			const double peak = *std::max_element(feed.bitrate_history.begin(), feed.bitrate_history.end());
			detail << tr("Peak: %1 Mb/s").arg(peak, 0, 'f', 1);
		}

		const QString tooltip = detail.join(QStringLiteral("\n"));
		for (int column = 0; column < ColumnCount; ++column)
			item->setToolTip(column, tooltip);

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
