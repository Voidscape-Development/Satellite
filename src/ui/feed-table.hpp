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

#include <QSet>
#include <QString>
#include <QTreeWidget>

#include <vector>

namespace satellite {

struct FeedSnapshot;

/// The feed grid in the Satellite window.
///
/// A dock can be anything from a sliver beside the preview to half a monitor, so the grid
/// adapts rather than scrolling sideways:
///
/// - **Wide**: a table. Columns the user has turned off (header right-click) never show;
///   of the rest, the least important are hidden first when they stop fitting.
/// - **Narrow**: once even the essential columns do not fit, each feed becomes a two-line
///   row - state, name and tally on top, the details on a second line - with the sparkline
///   kept on the right.
///
/// Rows are keyed by feed id, so a feed keeps its row (and its selection) as others come
/// and go.
class FeedTable : public QTreeWidget {
	Q_OBJECT

public:
	explicit FeedTable(QWidget *parent = nullptr);

	void setFeeds(const std::vector<FeedSnapshot> &feeds);

	bool compact() const { return compact_; }

	/// Columns the user has turned off, as stable keys. Used to save and restore them.
	QStringList hiddenColumnKeys() const;
	void setHiddenColumnKeys(const QStringList &keys);

signals:
	/// The user changed which columns are shown.
	void hiddenColumnsChanged();

protected:
	void resizeEvent(QResizeEvent *event) override;

private:
	void relayout();
	void showHeaderMenu(const QPoint &pos);
	int requiredWidth(int column) const;

	QSet<int> userHidden_;
	QSet<int> autoHidden_;
	bool compact_ = false;
};

} // namespace satellite
