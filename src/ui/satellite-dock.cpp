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
#include "ui/feed-table.hpp"
#include "ui/import-dialog.hpp"
#include "ui/status-colors.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
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

constexpr int kTabFeeds = 0;
constexpr int kTabSenders = 1;
constexpr int kTabSettings = 2;

/// The banner's link to the full runtime details on the Settings tab.
constexpr const char *kSettingsLink = "satellite:settings";

/// Mixer tracks a canvas sender can carry; OBS has six.
constexpr int kAudioTracks = 6;

SatelliteDock *g_dock = nullptr;

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString dot(const QColor &color)
{
	return QStringLiteral("<span style=\"color:%1\">●</span>").arg(color.name());
}

/// A label for secondary text: slightly smaller and dimmed, so the eye goes to the controls.
QLabel *make_note(const QString &content, QWidget *parent)
{
	auto *note = new QLabel(content, parent);
	note->setWordWrap(true);
	QFont font = note->font();
	font.setPointSizeF(font.pointSizeF() * 0.9);
	note->setFont(font);
	note->setEnabled(false);
	return note;
}

QLabel *make_section_title(const QString &content, QWidget *parent)
{
	auto *title = new QLabel(content, parent);
	QFont font = title->font();
	font.setBold(true);
	title->setFont(font);
	return title;
}

/// A scrolling page for a tab, so a short dock never clips the controls on it.
QScrollArea *make_scroll_page(QWidget *content, QWidget *parent)
{
	auto *scroll = new QScrollArea(parent);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setWidget(content);
	return scroll;
}

void fill_protocols(QComboBox *combo)
{
	for (Protocol value : kAllProtocols)
		combo->addItem(QString::fromUtf8(protocol_display_name(value)), QString::fromUtf8(protocol_id(value)));
}

void select_protocol(QComboBox *combo, Protocol protocol)
{
	const int index = combo->findData(QString::fromUtf8(protocol_id(protocol)));
	if (index >= 0)
		combo->setCurrentIndex(index);
}

void read_protocol(const QComboBox *combo, Protocol &protocol)
{
	Protocol value = Protocol::NDI;
	if (protocol_from_id(combo->currentData().toString().toUtf8().constData(), value))
		protocol = value;
}

/// Shows a sender's state as a coloured dot and a word or two. Errors can run long, so the
/// full reason goes in the tooltip.
void show_sender_status(QLabel *label, const SenderStatus &status)
{
	QColor color = label->palette().color(QPalette::Disabled, QPalette::Text);
	QString word = text("Satellite.Sender.Off");

	switch (status.state) {
	case SenderState::Off:
		break;
	case SenderState::Live:
		color = kStatusGoodColor;
		word = text("Satellite.Sender.Live");
		break;
	case SenderState::Waiting:
		color = kStatusWaitingColor;
		word = QString::fromStdString(status.detail);
		break;
	case SenderState::Failed:
		color = kStatusErrorColor;
		word = text("Satellite.Sender.Error");
		break;
	}

	label->setText(dot(color) + QStringLiteral(" ") + word.toHtmlEscaped());
	label->setToolTip(QString::fromStdString(status.detail));
}

QLabel *make_status_label(QWidget *parent)
{
	auto *label = new QLabel(parent);
	label->setTextFormat(Qt::RichText);
	label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	return label;
}

/// "NDI SDK LINUX 14:07:45 Jan 13 2025 6.1.1.0" is the whole truth, but a status chip has
/// room for "6.1.1.0". The full string stays in the tooltip.
QString short_version(const QString &version)
{
	const QStringList words = version.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	for (auto it = words.rbegin(); it != words.rend(); ++it)
		if (!it->isEmpty() && it->at(0).isDigit() && it->contains(QLatin1Char('.')))
			return *it;
	return version;
}

/// Frames a group of controls as a card. Drawn from the palette so it follows the theme.
void style_card(QFrame *card)
{
	card->setObjectName(QStringLiteral("SatelliteCard"));
	card->setStyleSheet(QStringLiteral("QFrame#SatelliteCard { border: 1px solid palette(mid); "
					   "border-radius: 4px; }"));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// OutputControls

OutputControls::OutputControls(const QString &title, QWidget *parent) : QFrame(parent)
{
	style_card(this);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 6, 8, 8);
	layout->setSpacing(4);

	// Two lines rather than one: on a single line the name field is what gets squeezed
	// when the dock is narrow, and it is the one field that needs the room.
	auto *top = new QHBoxLayout();
	enabled_ = new QCheckBox(title, this);
	status_ = make_status_label(this);
	top->addWidget(enabled_);
	top->addStretch(1);
	top->addWidget(status_);

	auto *bottom = new QHBoxLayout();
	name_ = new QLineEdit(this);
	name_->setPlaceholderText(text("Satellite.Output.NamePlaceholder"));
	name_->setToolTip(text("Satellite.Filter.Name.Hint"));
	protocol_ = new QComboBox(this);
	fill_protocols(protocol_);
	audio_ = new QCheckBox(text("Satellite.Output.Audio"), this);
	bottom->addWidget(name_, 1);
	bottom->addWidget(protocol_);
	bottom->addWidget(audio_);

	layout->addLayout(top);
	layout->addLayout(bottom);

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
	select_protocol(protocol_, config.protocol);

	name_->setEnabled(config.enabled);
	protocol_->setEnabled(config.enabled);
	audio_->setEnabled(config.enabled);
}

void OutputControls::store(OutputConfig &config) const
{
	config.enabled = enabled_->isChecked();
	config.name = name_->text().toStdString();
	config.send_audio = audio_->isChecked();
	read_protocol(protocol_, config.protocol);
}

void OutputControls::setStatus(const SenderStatus &status)
{
	show_sender_status(status_, status);
}

// ---------------------------------------------------------------------------------------------
// CanvasSenderControls

CanvasSenderControls::CanvasSenderControls(int id, QWidget *parent) : QFrame(parent), id_(id)
{
	style_card(this);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 6, 8, 8);
	layout->setSpacing(4);

	auto *top = new QHBoxLayout();
	enabled_ = new QCheckBox(this);
	enabled_->setToolTip(text("Satellite.Canvas.Enable"));
	canvas_ = new QComboBox(this);
	canvas_->setToolTip(text("Satellite.Canvas.Picker"));
	canvas_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	canvas_->setMinimumContentsLength(8);
	status_ = make_status_label(this);
	remove_ = new QToolButton(this);
	remove_->setText(QStringLiteral("✕"));
	remove_->setAutoRaise(true);
	remove_->setToolTip(text("Satellite.Canvas.Remove"));
	top->addWidget(enabled_);
	top->addWidget(canvas_, 1);
	top->addWidget(status_);
	top->addWidget(remove_);

	auto *bottom = new QHBoxLayout();
	name_ = new QLineEdit(this);
	name_->setPlaceholderText(text("Satellite.Output.NamePlaceholder"));
	name_->setToolTip(text("Satellite.Filter.Name.Hint"));
	protocol_ = new QComboBox(this);
	fill_protocols(protocol_);
	audio_ = new QComboBox(this);
	audio_->setToolTip(text("Satellite.Canvas.Audio.Hint"));
	audio_->addItem(text("Satellite.Canvas.Audio.Off"), 0);
	for (int track = 1; track <= kAudioTracks; ++track)
		audio_->addItem(QString::fromUtf8(obs_module_text("Satellite.Canvas.Audio.Track")).arg(track), track);
	bottom->addWidget(name_, 1);
	bottom->addWidget(protocol_);
	bottom->addWidget(audio_);

	layout->addLayout(top);
	layout->addLayout(bottom);

	connect(enabled_, &QCheckBox::toggled, this, [this]() {
		updateEnabled();
		emit changed();
	});
	connect(canvas_, &QComboBox::currentIndexChanged, this, &CanvasSenderControls::changed);
	connect(name_, &QLineEdit::editingFinished, this, &CanvasSenderControls::changed);
	connect(protocol_, &QComboBox::currentIndexChanged, this, &CanvasSenderControls::changed);
	connect(audio_, &QComboBox::currentIndexChanged, this, &CanvasSenderControls::changed);
	connect(remove_, &QToolButton::clicked, this, [this]() { emit removeRequested(id_); });
}

void CanvasSenderControls::setCanvases(const std::vector<CanvasInfo> &canvases)
{
	canvases_ = canvases;

	const QSignalBlocker block(canvas_);
	const QString uuid = canvas_->currentData(Qt::UserRole).toString();
	const QString name = canvas_->currentData(Qt::UserRole + 1).toString();
	selectCanvas(uuid, name);
}

void CanvasSenderControls::selectCanvas(const QString &uuid, const QString &name)
{
	canvas_->clear();

	int selected = -1;
	int by_name = -1;
	for (const CanvasInfo &canvas : canvases_) {
		const QString canvas_uuid = QString::fromStdString(canvas.uuid);
		const QString canvas_name = QString::fromStdString(canvas.name);

		canvas_->addItem(QStringLiteral("%1  (%2×%3)").arg(canvas_name).arg(canvas.width).arg(canvas.height));
		const int index = canvas_->count() - 1;
		canvas_->setItemData(index, canvas_uuid, Qt::UserRole);
		canvas_->setItemData(index, canvas_name, Qt::UserRole + 1);

		if (!uuid.isEmpty() && canvas_uuid == uuid)
			selected = index;
		if (!name.isEmpty() && canvas_name == name && by_name < 0)
			by_name = index;
	}

	if (selected < 0)
		selected = by_name;

	// A canvas that is not here right now - its plugin is not loaded, or it has not been
	// created yet - stays selected, marked missing, so the setting is not silently lost.
	if (selected < 0 && (!uuid.isEmpty() || !name.isEmpty())) {
		canvas_->addItem(QString::fromUtf8(obs_module_text("Satellite.Canvas.Missing"))
					 .arg(name.isEmpty() ? text("Satellite.Canvas.Unnamed") : name));
		selected = canvas_->count() - 1;
		canvas_->setItemData(selected, uuid, Qt::UserRole);
		canvas_->setItemData(selected, name, Qt::UserRole + 1);
	}

	if (selected >= 0)
		canvas_->setCurrentIndex(selected);
}

void CanvasSenderControls::load(const CanvasSenderConfig &config)
{
	const QSignalBlocker block_enabled(enabled_);
	const QSignalBlocker block_canvas(canvas_);
	const QSignalBlocker block_name(name_);
	const QSignalBlocker block_protocol(protocol_);
	const QSignalBlocker block_audio(audio_);

	enabled_->setChecked(config.enabled);
	selectCanvas(QString::fromStdString(config.canvas_uuid), QString::fromStdString(config.canvas_name));
	name_->setText(QString::fromStdString(config.name));
	select_protocol(protocol_, config.protocol);

	const int track = audio_->findData(config.audio_track);
	audio_->setCurrentIndex(track >= 0 ? track : 0);

	updateEnabled();
}

void CanvasSenderControls::store(CanvasSenderConfig &config) const
{
	config.enabled = enabled_->isChecked();
	config.canvas_uuid = canvas_->currentData(Qt::UserRole).toString().toStdString();
	config.canvas_name = canvas_->currentData(Qt::UserRole + 1).toString().toStdString();
	config.name = name_->text().toStdString();
	config.audio_track = audio_->currentData().toInt();
	read_protocol(protocol_, config.protocol);
}

void CanvasSenderControls::setStatus(const SenderStatus &status)
{
	show_sender_status(status_, status);
}

void CanvasSenderControls::focusName()
{
	name_->setFocus();
	name_->selectAll();
}

void CanvasSenderControls::updateEnabled()
{
	// The canvas stays editable while off: picking it is the first thing anyone does.
	const bool on = enabled_->isChecked();
	name_->setEnabled(on);
	protocol_->setEnabled(on);
	audio_->setEnabled(on);
}

// ---------------------------------------------------------------------------------------------
// SatelliteDock

SatelliteDock::SatelliteDock(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("SatelliteDock"));

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);

	buildStatusStrip(layout);

	tabs_ = new QTabWidget(this);
	tabs_->setDocumentMode(true);
	tabs_->addTab(buildFeedsTab(), text("Satellite.Tab.Feeds"));
	tabs_->addTab(buildSendersTab(), text("Satellite.Tab.Senders"));
	tabs_->addTab(buildSettingsTab(), text("Satellite.Tab.Settings"));
	layout->addWidget(tabs_, 1);

	const Config &config = Config::instance();
	tabs_->setCurrentIndex(std::clamp(config.dock_tab, kTabFeeds, kTabSettings));
	connect(tabs_, &QTabWidget::currentChanged, this, [](int index) {
		Config &config = Config::instance();
		config.dock_tab = index;
		config.save();
	});

	timer_ = new QTimer(this);
	timer_->setInterval(kRefreshIntervalMs);
	connect(timer_, &QTimer::timeout, this, &SatelliteDock::refresh);
	timer_->start();

	refresh();
}

void SatelliteDock::buildStatusStrip(QVBoxLayout *layout)
{
	auto *strip = new QHBoxLayout();
	strip->setContentsMargins(4, 0, 4, 0);
	strip->setSpacing(12);

	ndiChip_ = new QLabel(this);
	ndiChip_->setTextFormat(Qt::RichText);
	omtChip_ = new QLabel(this);
	omtChip_->setTextFormat(Qt::RichText);
	activity_ = new QLabel(this);
	activity_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

	strip->addWidget(ndiChip_);
	strip->addWidget(omtChip_);
	strip->addStretch(1);
	strip->addWidget(activity_);
	layout->addLayout(strip);

	// Only there when a protocol cannot be used, because that is the one thing in this
	// window that needs doing something about. Installing a runtime usually means quitting
	// OBS and coming back; it does not have to - nothing is loaded yet when a runtime is
	// missing, so retrying is safe, and the re-check turns a restart into a click.
	banner_ = new QFrame(this);
	style_card(banner_);
	auto *banner_layout = new QHBoxLayout(banner_);
	banner_layout->setContentsMargins(8, 4, 4, 4);

	// Kept to a line per protocol, because a dock is short and this sits above everything
	// else: the full reason is one click away, on the Settings tab.
	bannerText_ = new QLabel(banner_);
	bannerText_->setTextFormat(Qt::RichText);
	bannerText_->setWordWrap(true);
	connect(bannerText_, &QLabel::linkActivated, this, [this](const QString &link) {
		if (link == QLatin1String(kSettingsLink))
			tabs_->setCurrentIndex(kTabSettings);
		else
			QDesktopServices::openUrl(QUrl(link));
	});

	bannerRecheck_ = new QPushButton(text("Satellite.Dock.Recheck"), banner_);
	bannerRecheck_->setToolTip(text("Satellite.Dock.Recheck.Hint"));
	connect(bannerRecheck_, &QPushButton::clicked, this, &SatelliteDock::recheckRuntimes);

	auto *dismiss = new QToolButton(banner_);
	dismiss->setText(QStringLiteral("✕"));
	dismiss->setAutoRaise(true);
	dismiss->setToolTip(text("Satellite.Dock.Dismiss"));
	connect(dismiss, &QToolButton::clicked, this, [this]() {
		dismissedBanner_ = bannerText_->text();
		banner_->setVisible(false);
	});

	banner_layout->addWidget(bannerText_, 1);
	banner_layout->addWidget(bannerRecheck_, 0, Qt::AlignVCenter);
	banner_layout->addWidget(dismiss, 0, Qt::AlignTop);
	banner_->setVisible(false);
	layout->addWidget(banner_);
}

QWidget *SatelliteDock::buildFeedsTab()
{
	auto *page = new QWidget(this);
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(0, 4, 0, 0);
	layout->setSpacing(4);

	feedStack_ = new QStackedWidget(page);

	// An empty grid says nothing; this says what to do next.
	auto *empty = new QLabel(text("Satellite.Feeds.Empty"), feedStack_);
	empty->setWordWrap(true);
	empty->setAlignment(Qt::AlignCenter);
	empty->setEnabled(false);
	empty->setMargin(16);

	feeds_ = new FeedTable(feedStack_);
	feeds_->setHiddenColumnKeys(QString::fromStdString(Config::instance().feed_hidden_columns)
					    .split(QLatin1Char(','), Qt::SkipEmptyParts));
	connect(feeds_, &FeedTable::hiddenColumnsChanged, this, &SatelliteDock::saveFeedColumns);

	feedStack_->addWidget(empty);
	feedStack_->addWidget(feeds_);

	summary_ = make_note(QString(), page);

	layout->addWidget(feedStack_, 1);
	layout->addWidget(summary_);
	return page;
}

void SatelliteDock::saveFeedColumns()
{
	Config &config = Config::instance();
	config.feed_hidden_columns = feeds_->hiddenColumnKeys().join(QLatin1Char(',')).toStdString();
	config.save();
}

QWidget *SatelliteDock::buildSendersTab()
{
	auto *content = new QWidget();
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(4, 8, 4, 8);
	layout->setSpacing(6);

	layout->addWidget(make_section_title(text("Satellite.Senders.Main"), content));

	program_ = new OutputControls(text("Satellite.Output.Program"), content);
	preview_ = new OutputControls(text("Satellite.Output.Preview"), content);
	preview_->setToolTip(text("Satellite.Output.PreviewNote"));

	const Config &config = Config::instance();
	program_->load(config.program);
	preview_->load(config.preview);

	connect(program_, &OutputControls::changed, this, &SatelliteDock::applyOutputSettings);
	connect(preview_, &OutputControls::changed, this, &SatelliteDock::applyOutputSettings);

	layout->addWidget(program_);
	layout->addWidget(preview_);

	layout->addSpacing(8);
	layout->addWidget(make_section_title(text("Satellite.Senders.Canvases"), content));

	canvasList_ = new QVBoxLayout();
	canvasList_->setContentsMargins(0, 0, 0, 0);
	canvasList_->setSpacing(6);
	layout->addLayout(canvasList_);

	canvasHint_ = make_note(QString(), content);
	layout->addWidget(canvasHint_);

	addCanvas_ = new QPushButton(text("Satellite.Canvas.Add"), content);
	connect(addCanvas_, &QPushButton::clicked, this, &SatelliteDock::addCanvasSender);
	auto *add_row = new QHBoxLayout();
	add_row->addWidget(addCanvas_);
	add_row->addStretch(1);
	layout->addLayout(add_row);

	layout->addStretch(1);

	rebuildCanvasControls();

	return make_scroll_page(content, this);
}

void SatelliteDock::rebuildCanvasControls()
{
	for (CanvasSenderControls *controls : canvasControls_)
		controls->deleteLater();
	canvasControls_.clear();

	const std::vector<CanvasInfo> canvases = list_canvases();

	for (const CanvasSenderConfig &sender : Config::instance().canvas_senders) {
		auto *controls = new CanvasSenderControls(sender.id, canvasList_->parentWidget());
		controls->setCanvases(canvases);
		controls->load(sender);

		connect(controls, &CanvasSenderControls::changed, this, &SatelliteDock::applyCanvasSettings);
		connect(controls, &CanvasSenderControls::removeRequested, this, &SatelliteDock::removeCanvasSender);

		canvasList_->addWidget(controls);
		canvasControls_.push_back(controls);
	}
}

void SatelliteDock::applyOutputSettings()
{
	Config &config = Config::instance();

	program_->store(config.program);
	preview_->store(config.preview);

	config.save();
	update_frontend_outputs();
	refreshSenders();
}

void SatelliteDock::applyCanvasSettings()
{
	Config &config = Config::instance();

	for (CanvasSenderControls *controls : canvasControls_) {
		for (CanvasSenderConfig &sender : config.canvas_senders)
			if (sender.id == controls->id())
				controls->store(sender);
	}

	config.save();
	update_frontend_outputs();
	refreshSenders();
}

void SatelliteDock::addCanvasSender()
{
	const std::vector<CanvasInfo> canvases = list_canvases();
	if (canvases.empty())
		return;

	Config &config = Config::instance();

	// Default to the first canvas nothing sends yet: adding a second sender for the same
	// canvas is possible (one over NDI, one over OMT, say) but rarely the first intent.
	const CanvasInfo *choice = &canvases.front();
	for (const CanvasInfo &canvas : canvases) {
		const bool used = std::any_of(config.canvas_senders.begin(), config.canvas_senders.end(),
					      [&](const CanvasSenderConfig &sender) {
						      return sender.canvas_uuid == canvas.uuid;
					      });
		if (!used) {
			choice = &canvas;
			break;
		}
	}

	// Added switched off: it goes on the network once the name is what the user wants,
	// not the moment the button is pressed.
	CanvasSenderConfig sender;
	sender.id = config.next_canvas_sender_id();
	sender.enabled = false;
	sender.canvas_uuid = choice->uuid;
	sender.canvas_name = choice->name;
	sender.name = "OBS " + choice->name;
	sender.protocol = config.program.protocol;
	config.canvas_senders.push_back(sender);
	config.save();

	rebuildCanvasControls();
	refreshSenders();

	if (!canvasControls_.empty())
		canvasControls_.back()->focusName();
}

void SatelliteDock::removeCanvasSender(int id)
{
	Config &config = Config::instance();

	auto it = std::find_if(config.canvas_senders.begin(), config.canvas_senders.end(),
			       [id](const CanvasSenderConfig &sender) { return sender.id == id; });
	if (it == config.canvas_senders.end())
		return;

	// Removing a sender that is on the network takes it off someone else's screen, so that
	// one is confirmed. A sender that is off goes straight away.
	if (it->enabled) {
		const auto answer =
			QMessageBox::question(this, text("Satellite.Canvas.Remove"),
					      QString::fromUtf8(obs_module_text("Satellite.Canvas.RemoveConfirm"))
						      .arg(QString::fromStdString(it->name).toHtmlEscaped()));
		if (answer != QMessageBox::Yes)
			return;

		it = std::find_if(config.canvas_senders.begin(), config.canvas_senders.end(),
				  [id](const CanvasSenderConfig &sender) { return sender.id == id; });
		if (it == config.canvas_senders.end())
			return;
	}

	config.canvas_senders.erase(it);
	config.save();
	update_frontend_outputs();

	rebuildCanvasControls();
	refreshSenders();
}

QWidget *SatelliteDock::buildSettingsTab()
{
	auto *content = new QWidget();
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(4, 8, 4, 8);
	layout->setSpacing(8);

	const Config &config = Config::instance();

	// Runtimes: the full story behind the status strip's chips.
	auto *runtimes = new QGroupBox(text("Satellite.Dock.Runtimes"), content);
	auto *runtimes_layout = new QVBoxLayout(runtimes);
	runtimes_layout->setSpacing(4);

	ndiDetail_ = new QLabel(runtimes);
	ndiDetail_->setTextFormat(Qt::RichText);
	ndiDetail_->setOpenExternalLinks(true);
	ndiDetail_->setWordWrap(true);
	ndiDetail_->setTextInteractionFlags(Qt::TextBrowserInteraction);

	omtDetail_ = new QLabel(runtimes);
	omtDetail_->setTextFormat(Qt::RichText);
	omtDetail_->setOpenExternalLinks(true);
	omtDetail_->setWordWrap(true);
	omtDetail_->setTextInteractionFlags(Qt::TextBrowserInteraction);

	settingsRecheck_ = new QPushButton(text("Satellite.Dock.Recheck"), runtimes);
	settingsRecheck_->setToolTip(text("Satellite.Dock.Recheck.Hint"));
	connect(settingsRecheck_, &QPushButton::clicked, this, &SatelliteDock::recheckRuntimes);

	auto *recheck_row = new QHBoxLayout();
	recheck_row->addWidget(settingsRecheck_);
	recheck_row->addStretch(1);

	runtimes_layout->addWidget(ndiDetail_);
	runtimes_layout->addWidget(omtDetail_);
	runtimes_layout->addLayout(recheck_row);
	layout->addWidget(runtimes);

	// Network
	auto *network = new QGroupBox(text("Satellite.Settings.Network"), content);
	auto *form = new QFormLayout(network);
	// Labels above fields: side by side, a narrow dock squeezes the fields until their
	// placeholders are unreadable, and the labels are long enough to do it.
	form->setRowWrapPolicy(QFormLayout::WrapAllRows);
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	ndiGroups_ = new QLineEdit(QString::fromStdString(config.ndi_groups), network);
	ndiGroups_->setPlaceholderText(text("Satellite.Advanced.NdiGroups.Placeholder"));
	ndiGroups_->setToolTip(text("Satellite.Advanced.NdiGroups.Hint"));

	omtDiscoveryServer_ = new QLineEdit(QString::fromStdString(config.omt_discovery_server), network);
	omtDiscoveryServer_->setPlaceholderText(text("Satellite.Advanced.OmtServer.Placeholder"));
	omtDiscoveryServer_->setToolTip(text("Satellite.Advanced.OmtServer.Hint"));

	omtPortStart_ = new QSpinBox(network);
	omtPortStart_->setRange(1024, 65535);
	omtPortStart_->setValue(config.omt_port_start);

	omtPortEnd_ = new QSpinBox(network);
	omtPortEnd_->setRange(1024, 65535);
	omtPortEnd_->setValue(config.omt_port_end);

	auto *ports = new QHBoxLayout();
	ports->setContentsMargins(0, 0, 0, 0);
	ports->addWidget(omtPortStart_);
	ports->addWidget(new QLabel(QStringLiteral("–"), network));
	ports->addWidget(omtPortEnd_);
	ports->addStretch(1);

	form->addRow(text("Satellite.Advanced.NdiGroups"), ndiGroups_);
	form->addRow(text("Satellite.Advanced.OmtServer"), omtDiscoveryServer_);
	form->addRow(text("Satellite.Advanced.OmtPorts"), ports);

	// editingFinished rather than textChanged: applying on every keystroke would rebuild the
	// NDI finder once per character.
	connect(ndiGroups_, &QLineEdit::editingFinished, this, &SatelliteDock::applyAdvancedSettings);
	connect(omtDiscoveryServer_, &QLineEdit::editingFinished, this, &SatelliteDock::applyAdvancedSettings);
	connect(omtPortStart_, &QSpinBox::editingFinished, this, &SatelliteDock::applyAdvancedSettings);
	connect(omtPortEnd_, &QSpinBox::editingFinished, this, &SatelliteDock::applyAdvancedSettings);

	layout->addWidget(network);

	// DistroAV
	auto *migrate = new QGroupBox(text("Satellite.Settings.DistroAV"), content);
	auto *migrate_layout = new QVBoxLayout(migrate);
	migrate_layout->addWidget(make_note(text("Satellite.Dock.Import.Hint"), migrate));

	auto *import = new QPushButton(text("Satellite.Dock.Import"), migrate);
	connect(import, &QPushButton::clicked, this, &SatelliteDock::importFromDistroAV);
	auto *import_row = new QHBoxLayout();
	import_row->addWidget(import);
	import_row->addStretch(1);
	migrate_layout->addLayout(import_row);

	layout->addWidget(migrate);
	layout->addStretch(1);

	return make_scroll_page(content, this);
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

void SatelliteDock::refresh()
{
	refreshRuntimeStatus();
	refreshFeeds();
	refreshSenders();
}

void SatelliteDock::refreshRuntimeStatus()
{
	QStringList problems;

	for (IBackend *backend : all_backends()) {
		const bool is_ndi = backend->protocol() == Protocol::NDI;
		QLabel *chip = is_ndi ? ndiChip_ : omtChip_;
		QLabel *detail = is_ndi ? ndiDetail_ : omtDetail_;

		const QString name = QString::fromUtf8(protocol_display_name(backend->protocol()));

		if (backend->available()) {
			const QString version = QString::fromStdString(backend->runtime_version());
			const QString shown = version.isEmpty() ? text("Satellite.Runtime.Ready")
								: short_version(version);

			chip->setText(QStringLiteral("%1 %2 <small>%3</small>")
					      .arg(dot(kStatusGoodColor), name, shown.toHtmlEscaped()));
			chip->setToolTip(QStringLiteral("%1: %2").arg(name, version.isEmpty() ? shown : version));
			detail->setText(QStringLiteral("%1 <b>%2</b>: %3")
						.arg(dot(kStatusGoodColor), name,
						     (version.isEmpty() ? shown : version).toHtmlEscaped()));
			continue;
		}

		const QString reason = QString::fromStdString(backend->unavailable_reason()).toHtmlEscaped();

		// NDI is proprietary and cannot be bundled, so when it is missing the only useful
		// thing we can offer is where to get it. OMT ships with Satellite and has no URL.
		QString link;
		const QString url = QString::fromStdString(backend->install_url());
		if (!url.isEmpty())
			link = QStringLiteral(" <a href=\"%1\">%2</a>").arg(url, text("Satellite.Runtime.Install"));

		chip->setText(QStringLiteral("%1 %2").arg(dot(kStatusErrorColor), name));
		chip->setToolTip(QStringLiteral("%1: %2").arg(name, reason));
		detail->setText(QStringLiteral("%1 <b>%2</b>: %3%4").arg(dot(kStatusErrorColor), name, reason, link));
		problems << QString::fromUtf8(obs_module_text("Satellite.Runtime.Unavailable"))
					    .arg(QStringLiteral("<b>%1</b>").arg(name)) +
				    link +
				    QStringLiteral(" <a href=\"%1\">%2</a>")
					    .arg(QLatin1String(kSettingsLink), text("Satellite.Runtime.Details"));
	}

	const bool any_unavailable = !problems.isEmpty();
	bannerRecheck_->setEnabled(any_unavailable);
	settingsRecheck_->setEnabled(any_unavailable);

	const QString banner = problems.join(QStringLiteral("<br>"));
	if (bannerText_->text() != banner)
		bannerText_->setText(banner);
	// Shown again whenever the problem changes, even after a dismissal.
	banner_->setVisible(any_unavailable && banner != dismissedBanner_);
}

void SatelliteDock::refreshFeeds()
{
	const std::vector<FeedSnapshot> feeds = FeedRegistry::instance().snapshot();

	feeds_->setFeeds(feeds);
	feedStack_->setCurrentIndex(feeds.empty() ? 0 : 1);

	int sending = 0;
	int receiving = 0;
	for (const FeedSnapshot &feed : feeds)
		(feed.direction == FeedDirection::Send ? sending : receiving)++;

	activity_->setVisible(!feeds.empty());
	activity_->setText(QStringLiteral("↑%1  ↓%2").arg(sending).arg(receiving));
	activity_->setToolTip(
		QString::fromUtf8(obs_module_text("Satellite.Dock.Activity")).arg(sending).arg(receiving));

	tabs_->setTabText(kTabFeeds,
			  feeds.empty() ? text("Satellite.Tab.Feeds")
					: QStringLiteral("%1 (%2)").arg(text("Satellite.Tab.Feeds")).arg(feeds.size()));

	const size_t discovered = DiscoveryService::instance().sources().size();
	summary_->setText(tr("%n feed(s) active", "", static_cast<int>(feeds.size())) + QStringLiteral(" · ") +
			  tr("%n source(s) on the network", "", static_cast<int>(discovered)));
}

void SatelliteDock::refreshSenders()
{
	int live = 0;
	auto show = [&live](auto *controls, const SenderStatus &status) {
		controls->setStatus(status);
		if (status.state == SenderState::Live)
			++live;
	};

	show(program_, program_sender_status());
	show(preview_, preview_sender_status());
	for (CanvasSenderControls *controls : canvasControls_)
		show(controls, canvas_sender_status(controls->id()));

	tabs_->setTabText(kTabSenders,
			  live == 0 ? text("Satellite.Tab.Senders")
				    : QStringLiteral("%1 (%2)").arg(text("Satellite.Tab.Senders")).arg(live));

	// Canvases come and go with other plugins, so the pickers follow along.
	const std::vector<CanvasInfo> canvases = list_canvases();
	std::string signature;
	for (const CanvasInfo &canvas : canvases)
		signature += canvas.uuid + '\n' + canvas.name + '\n' + std::to_string(canvas.width) + 'x' +
			     std::to_string(canvas.height) + '\n';

	if (signature != canvasSignature_ || canvasHint_->text().isEmpty()) {
		canvasSignature_ = signature;
		for (CanvasSenderControls *controls : canvasControls_)
			controls->setCanvases(canvases);

		if (!canvases_supported())
			canvasHint_->setText(text("Satellite.Canvas.Unsupported"));
		else if (canvases.empty())
			canvasHint_->setText(text("Satellite.Canvas.None"));
		else
			canvasHint_->setText(text("Satellite.Canvas.Hint"));

		addCanvas_->setEnabled(canvases_supported() && !canvases.empty());
	}
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
