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

#include "ui/feed-table.hpp"

#include <algorithm>

#include "metrics/feed-registry.hpp"
#include "ui/sparkline.hpp"
#include "ui/status-colors.hpp"

#include <QAction>
#include <QHeaderView>
#include <QMenu>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyledItemDelegate>

#include <obs-module.h>

namespace satellite {

namespace {

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

struct ColumnInfo {
	const char *key;    //< stable, used in saved settings
	const char *header; //< locale key
	/// Representative content, used to judge whether the column fits without waiting
	/// for real rows - so the layout does not jump as feeds come and go.
	const char *sample;
};

const ColumnInfo kColumns[ColumnCount] = {
	{"name", "Satellite.Column.Name", ""},
	{"protocol", "Satellite.Column.Protocol", "OMT"},
	{"direction", "Satellite.Column.Direction", "↓ Receive"},
	{"state", "Satellite.Column.State", "Connecting"},
	{"video", "Satellite.Column.Video", "1920x1080p59.94 UYVY"},
	{"audio", "Satellite.Column.Audio", "48 kHz 2ch"},
	{"bitrate", "Satellite.Column.Bitrate", "~188.8 Mb/s"},
	{"dropped", "Satellite.Column.Dropped", "00000"},
	{"connections", "Satellite.Column.Connections", "00"},
	{"tally", "Satellite.Column.Tally", "PGM"},
	{"history", "Satellite.Column.History", ""},
};

/// Most important first. When the table runs out of room, columns go from the end of this
/// list; the name never goes.
const int kPriority[] = {
	ColumnName,    ColumnState, ColumnBitrate, ColumnTally, ColumnDirection,   ColumnProtocol,
	ColumnHistory, ColumnVideo, ColumnDropped, ColumnAudio, ColumnConnections,
};

/// Below this many logical pixels, or when these columns do not all fit, the table gives up
/// on columns altogether and switches to two-line rows.
constexpr int kCompactBreakpoint = 480;
const int kEssentialColumns[] = {ColumnName, ColumnState, ColumnBitrate};

constexpr int kNameMinimumWidth = 140;
constexpr int kHistoryWidth = 80;
constexpr int kCellPadding = 18;

enum FeedRole {
	RoleFeedId = Qt::UserRole + 1,
	RoleState,
	RoleTally,
	RoleDetail,
};

/// 0 none, 1 preview, 2 program.
int tally_level(const Tally &tally)
{
	return tally.program ? 2 : (tally.preview ? 1 : 0);
}

QString tally_text(int level)
{
	return level == 2 ? QStringLiteral("PGM") : (level == 1 ? QStringLiteral("PVW") : QString());
}

QString direction_text(FeedDirection direction)
{
	return direction == FeedDirection::Send ? QStringLiteral("↑ ") + QObject::tr("Send")
						: QStringLiteral("↓ ") + QObject::tr("Receive");
}

QString bitrate_text(const FeedStats &stats)
{
	// NDI exposes no byte counters, so its bitrate is measured by us from frame sizes.
	// Marking it keeps the number honest next to OMT's exact figure.
	QString bitrate = QStringLiteral("%1 Mb/s").arg(stats.bitrate_mbps, 0, 'f', 1);
	if (stats.bitrate_estimated)
		bitrate.prepend(QStringLiteral("~"));
	return bitrate;
}

/// Paints the name column (state dot, name, and in compact mode a second detail line and
/// the tally badge) and the tally column (a badge rather than bare text).
class FeedDelegate : public QStyledItemDelegate {
public:
	explicit FeedDelegate(FeedTable *table) : QStyledItemDelegate(table), table_(table) {}

	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		if (index.column() != ColumnName && index.column() != ColumnTally) {
			QStyledItemDelegate::paint(painter, option, index);
			return;
		}

		QStyleOptionViewItem opt = option;
		initStyleOption(&opt, index);
		const QString text = opt.text;
		opt.text.clear();

		// Background, selection and hover from the style, so it matches the OBS theme.
		const QWidget *widget = opt.widget;
		QStyle *style = widget ? widget->style() : nullptr;
		if (style)
			style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

		const bool selected = (opt.state & QStyle::State_Selected) != 0;
		const QColor foreground = opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
		const int tally = index.data(RoleTally).toInt();

		painter->save();
		painter->setRenderHint(QPainter::Antialiasing, true);

		if (index.column() == ColumnTally) {
			paintBadge(painter, opt.rect, tally, opt.font, Qt::AlignCenter);
			painter->restore();
			return;
		}

		const QFontMetrics metrics(opt.font);
		const int line_height = metrics.height();
		QRect area = opt.rect.adjusted(6, 0, -6, 0);

		const bool compact = table_->compact();
		const int top = compact ? area.top() + (area.height() - (line_height * 2 + 2)) / 2
					: area.top() + (area.height() - line_height) / 2;
		QRect first_line(area.left(), top, area.width(), line_height);

		// State dot
		const int dot = std::max(6, line_height / 2);
		const QColor dot_color =
			feed_state_color(static_cast<FeedState>(index.data(RoleState).toInt()), opt.palette);
		painter->setPen(Qt::NoPen);
		painter->setBrush(dot_color);
		painter->drawEllipse(QRect(first_line.left(), first_line.center().y() - dot / 2 + 1, dot, dot));
		first_line.setLeft(first_line.left() + dot + 6);

		if (compact && tally > 0) {
			const QRect badge =
				paintBadge(painter, first_line, tally, opt.font, Qt::AlignRight | Qt::AlignVCenter);
			first_line.setRight(badge.left() - 6);
		}

		painter->setPen(foreground);
		painter->setFont(opt.font);
		painter->drawText(first_line, Qt::AlignLeft | Qt::AlignVCenter,
				  metrics.elidedText(text, Qt::ElideRight, first_line.width()));

		if (compact) {
			QColor secondary = foreground;
			secondary.setAlphaF(selected ? 0.85 : 0.6);

			QFont detail_font = opt.font;
			detail_font.setPointSizeF(opt.font.pointSizeF() * 0.9);
			const QFontMetrics detail_metrics(detail_font);

			// Indented to line up with the name, under the dot's gutter.
			const int indent = area.left() + dot + 6;
			const QRect second_line(indent, top + line_height + 2, area.right() - indent, line_height);
			painter->setPen(secondary);
			painter->setFont(detail_font);
			painter->drawText(second_line, Qt::AlignLeft | Qt::AlignVCenter,
					  detail_metrics.elidedText(index.data(RoleDetail).toString(), Qt::ElideRight,
								    second_line.width()));
		}

		painter->restore();
	}

	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		QSize size = QStyledItemDelegate::sizeHint(option, index);
		const int line_height = QFontMetrics(option.font).height();
		size.setHeight(std::max(size.height(), table_->compact() ? line_height * 2 + 14 : line_height + 8));
		return size;
	}

private:
	/// Draws a PGM/PVW badge aligned within rect and returns where it went.
	static QRect paintBadge(QPainter *painter, const QRect &rect, int tally, const QFont &base, Qt::Alignment align)
	{
		if (tally <= 0)
			return QRect(rect.right(), rect.top(), 0, rect.height());

		QFont font = base;
		font.setBold(true);
		font.setPointSizeF(base.pointSizeF() * 0.8);
		const QFontMetrics metrics(font);

		const QString text = tally_text(tally);
		const int width = metrics.horizontalAdvance(text) + 10;
		const int height = metrics.height() + 2;

		QRect badge(0, 0, width, height);
		if (align & Qt::AlignRight)
			badge.moveTopRight(QPoint(rect.right(), rect.center().y() - height / 2));
		else
			badge.moveCenter(rect.center());

		painter->setPen(Qt::NoPen);
		painter->setBrush(tally == 2 ? kTallyProgramColor : kTallyPreviewColor);
		painter->drawRoundedRect(badge, 3, 3);
		painter->setPen(Qt::white);
		painter->setFont(font);
		painter->drawText(badge, Qt::AlignCenter, text);
		return badge;
	}

	FeedTable *table_;
};

} // namespace

FeedTable::FeedTable(QWidget *parent) : QTreeWidget(parent)
{
	setRootIsDecorated(false);
	setAlternatingRowColors(true);
	setUniformRowHeights(false);
	setSelectionMode(QAbstractItemView::SingleSelection);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setTextElideMode(Qt::ElideRight);
	setColumnCount(ColumnCount);
	setItemDelegate(new FeedDelegate(this));

	QStringList headers;
	for (const ColumnInfo &column : kColumns)
		headers << obs_module_text(column.header);
	setHeaderLabels(headers);

	QHeaderView *view = header();
	view->setStretchLastSection(false);
	view->setSectionsMovable(false);
	view->setContextMenuPolicy(Qt::CustomContextMenu);
	view->setToolTip(obs_module_text("Satellite.Feeds.ColumnsHint"));
	connect(view, &QHeaderView::customContextMenuRequested, this, &FeedTable::showHeaderMenu);

	relayout();
}

QStringList FeedTable::hiddenColumnKeys() const
{
	QStringList keys;
	for (int column = 0; column < ColumnCount; ++column)
		if (userHidden_.contains(column))
			keys << QString::fromUtf8(kColumns[column].key);
	return keys;
}

void FeedTable::setHiddenColumnKeys(const QStringList &keys)
{
	userHidden_.clear();
	for (int column = 0; column < ColumnCount; ++column)
		if (column != ColumnName && keys.contains(QString::fromUtf8(kColumns[column].key)))
			userHidden_.insert(column);
	relayout();
}

int FeedTable::requiredWidth(int column) const
{
	if (column == ColumnName)
		return kNameMinimumWidth;
	if (column == ColumnHistory)
		return kHistoryWidth;

	const QFontMetrics metrics(font());
	const QFontMetrics header_metrics(header()->font());
	const int content = metrics.horizontalAdvance(QString::fromUtf8(kColumns[column].sample));
	const int title = header_metrics.horizontalAdvance(QString::fromUtf8(obs_module_text(kColumns[column].header)));
	return std::max(content, title) + kCellPadding;
}

void FeedTable::relayout()
{
	int available = viewport()->width();
	// Leave room for a vertical scrollbar that may be about to appear, so adding a row
	// cannot push a column off and back on again.
	if (!verticalScrollBar()->isVisible())
		available -= style()->pixelMetric(QStyle::PM_ScrollBarExtent);

	int essential = 0;
	for (int column : kEssentialColumns)
		essential += requiredWidth(column);

	const bool compact = available < kCompactBreakpoint || available < essential;
	const bool mode_changed = compact != compact_;
	compact_ = compact;

	autoHidden_.clear();
	if (!compact_) {
		int used = 0;
		for (int column : kPriority) {
			if (userHidden_.contains(column))
				continue;
			const int width = requiredWidth(column);
			if (column != ColumnName && used + width > available)
				autoHidden_.insert(column);
			else
				used += width;
		}
	}

	header()->setVisible(!compact_);

	for (int column = 0; column < ColumnCount; ++column) {
		bool hidden = userHidden_.contains(column) || autoHidden_.contains(column);
		if (compact_)
			hidden = column != ColumnName && column != ColumnHistory;
		setColumnHidden(column, hidden);

		if (column == ColumnName)
			header()->setSectionResizeMode(column, QHeaderView::Stretch);
		else if (column == ColumnHistory)
			header()->setSectionResizeMode(column, QHeaderView::Fixed);
		else
			header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
	}
	setColumnWidth(ColumnHistory, kHistoryWidth);

	if (mode_changed) {
		// Row heights are cached by the view; switching between one and two lines has to
		// make it ask the delegate again.
		for (int row = 0; row < topLevelItemCount(); ++row) {
			if (QWidget *widget = itemWidget(topLevelItem(row), ColumnHistory))
				widget->setContentsMargins(4, compact_ ? 8 : 3, 4, compact_ ? 8 : 3);
		}
		scheduleDelayedItemsLayout();
	}
}

void FeedTable::resizeEvent(QResizeEvent *event)
{
	QTreeWidget::resizeEvent(event);
	relayout();
}

void FeedTable::showHeaderMenu(const QPoint &pos)
{
	QMenu menu(this);

	QAction *title = menu.addAction(obs_module_text("Satellite.Feeds.ShowColumns"));
	title->setEnabled(false);

	for (int column = 0; column < ColumnCount; ++column) {
		QString label = QString::fromUtf8(obs_module_text(kColumns[column].header));
		if (autoHidden_.contains(column))
			label += QStringLiteral(" — ") + obs_module_text("Satellite.Feeds.NoRoom");

		QAction *action = menu.addAction(label);
		action->setCheckable(true);
		action->setChecked(!userHidden_.contains(column));
		// The name is what identifies a row; a table without it is no use to anyone.
		action->setEnabled(column != ColumnName);

		connect(action, &QAction::toggled, this, [this, column](bool on) {
			if (on)
				userHidden_.remove(column);
			else
				userHidden_.insert(column);
			relayout();
			emit hiddenColumnsChanged();
		});
	}

	menu.addSeparator();
	QAction *reset = menu.addAction(obs_module_text("Satellite.Feeds.ResetColumns"));
	reset->setEnabled(!userHidden_.isEmpty());
	connect(reset, &QAction::triggered, this, [this]() {
		userHidden_.clear();
		relayout();
		emit hiddenColumnsChanged();
	});

	menu.exec(header()->mapToGlobal(pos));
}

void FeedTable::setFeeds(const std::vector<FeedSnapshot> &feeds)
{
	// Drop rows whose feed has gone, then update or append the rest in registry order.
	for (int row = topLevelItemCount() - 1; row >= 0; --row) {
		const quint64 id = topLevelItem(row)->data(ColumnName, RoleFeedId).toULongLong();
		const bool present = std::any_of(feeds.begin(), feeds.end(),
						 [id](const FeedSnapshot &feed) { return feed.id == id; });
		if (!present)
			delete takeTopLevelItem(row);
	}

	for (size_t index = 0; index < feeds.size(); ++index) {
		const FeedSnapshot &feed = feeds[index];
		const int row = static_cast<int>(index);

		QTreeWidgetItem *item = nullptr;
		for (int existing = row; existing < topLevelItemCount(); ++existing) {
			if (topLevelItem(existing)->data(ColumnName, RoleFeedId).toULongLong() == feed.id) {
				item = topLevelItem(existing);
				if (existing != row) {
					// Moving an item destroys its item widget, so recreate the sparkline.
					takeTopLevelItem(existing);
					insertTopLevelItem(row, item);
					auto *sparkline = new Sparkline(this);
					sparkline->setContentsMargins(4, compact_ ? 8 : 3, 4, compact_ ? 8 : 3);
					setItemWidget(item, ColumnHistory, sparkline);
				}
				break;
			}
		}

		if (!item) {
			item = new QTreeWidgetItem();
			item->setData(ColumnName, RoleFeedId, QVariant::fromValue<quint64>(feed.id));
			insertTopLevelItem(row, item);
			auto *sparkline = new Sparkline(this);
			sparkline->setContentsMargins(4, compact_ ? 8 : 3, 4, compact_ ? 8 : 3);
			setItemWidget(item, ColumnHistory, sparkline);
		}

		const QString protocol = QString::fromUtf8(protocol_display_name(feed.protocol));
		const QString video = QString::fromStdString(feed.video_format);
		const QString bitrate = bitrate_text(feed.stats);
		const int tally = tally_level(feed.tally);

		item->setText(ColumnName, QString::fromStdString(feed.name));
		item->setData(ColumnName, RoleState, static_cast<int>(feed.state));
		item->setData(ColumnName, RoleTally, tally);
		item->setData(ColumnTally, RoleTally, tally);
		item->setText(ColumnProtocol, protocol);
		item->setText(ColumnDirection, direction_text(feed.direction));
		item->setText(ColumnState, QString::fromUtf8(feed_state_display_name(feed.state)));
		item->setText(ColumnVideo, video);
		item->setText(ColumnAudio, QString::fromStdString(feed.audio_format));
		item->setText(ColumnBitrate, bitrate);
		item->setText(ColumnDropped, QString::number(feed.stats.frames_dropped));
		item->setText(ColumnConnections,
			      feed.stats.connections >= 0 ? QString::number(feed.stats.connections) : QString());

		const Qt::Alignment numeric = Qt::AlignRight | Qt::AlignVCenter;
		item->setTextAlignment(ColumnBitrate, numeric);
		item->setTextAlignment(ColumnDropped, numeric);
		item->setTextAlignment(ColumnConnections, numeric);

		// The second line of a compact row: everything a glance needs, in the order the
		// wide table's columns would give it.
		QStringList detail;
		detail << protocol + QStringLiteral(" ") + direction_text(feed.direction).left(1);
		if (feed.state != FeedState::Connected)
			detail << QString::fromUtf8(feed_state_display_name(feed.state));
		if (!video.isEmpty())
			detail << video.section(QLatin1Char(' '), 0, 0);
		detail << bitrate;
		if (feed.stats.frames_dropped > 0)
			detail << tr("%1 dropped").arg(feed.stats.frames_dropped);
		item->setData(ColumnName, RoleDetail, detail.join(QStringLiteral(" · ")));

		// Everything else worth knowing goes in the row's tooltip. A column each for fps,
		// codec time and peak bitrate would make the table unreadable for numbers most
		// people look at once.
		QStringList tooltip;
		tooltip << QStringLiteral("<b>%1</b>").arg(QString::fromStdString(feed.name).toHtmlEscaped());
		tooltip << tr("%1, %2").arg(protocol, direction_text(feed.direction));
		tooltip << tr("State: %1").arg(QString::fromUtf8(feed_state_display_name(feed.state)));
		if (!video.isEmpty())
			tooltip << tr("Video: %1").arg(video.toHtmlEscaped());
		if (!feed.audio_format.empty())
			tooltip << tr("Audio: %1").arg(QString::fromStdString(feed.audio_format).toHtmlEscaped());
		if (feed.stats.fps > 0.0)
			tooltip << tr("Rate: %1 fps").arg(feed.stats.fps, 0, 'f', 1);
		tooltip << tr("Frames: %1 total, %2 dropped").arg(feed.stats.frames).arg(feed.stats.frames_dropped);
		if (feed.stats.connections >= 0)
			tooltip << tr("Connections: %1").arg(feed.stats.connections);
		if (feed.stats.codec_ms >= 0.0)
			tooltip << tr("Codec: %1 ms/frame").arg(feed.stats.codec_ms, 0, 'f', 1);
		tooltip << tr("Bitrate: %1").arg(bitrate);
		if (!feed.bitrate_history.empty()) {
			const double peak = *std::max_element(feed.bitrate_history.begin(), feed.bitrate_history.end());
			tooltip << tr("Peak: %1 Mb/s").arg(peak, 0, 'f', 1);
		}
		tooltip << QStringLiteral("<i>%1</i>")
				   .arg(feed.stats.bitrate_estimated
						? tr("Bitrate is measured locally; the protocol "
						     "reports no byte counters")
						: tr("Bitrate is reported exactly by the protocol"));

		const QString tooltip_text = tooltip.join(QStringLiteral("<br>"));
		for (int column = 0; column < ColumnCount; ++column)
			item->setToolTip(column, tooltip_text);

		if (auto *sparkline = qobject_cast<Sparkline *>(itemWidget(item, ColumnHistory))) {
			QVector<double> values;
			values.reserve(static_cast<int>(feed.bitrate_history.size()));
			for (double value : feed.bitrate_history)
				values.append(value);
			sparkline->setValues(values);
		}
	}
}

} // namespace satellite
