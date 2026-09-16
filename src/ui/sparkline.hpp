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

#include <QVector>
#include <QWidget>

namespace satellite {

/// A small self-painted history plot, drawn from the widget palette so it follows whatever
/// OBS theme is active.
///
/// Deliberately hand-rolled: one row-height plot per feed does not justify pulling a
/// charting library into an OBS plugin.
class Sparkline : public QWidget {
	Q_OBJECT

public:
	explicit Sparkline(QWidget *parent = nullptr);

	void setValues(const QVector<double> &values);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	QVector<double> values_;
};

} // namespace satellite
