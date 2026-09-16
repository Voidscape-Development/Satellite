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

#include "ui/sparkline.hpp"

#include <algorithm>

#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

namespace satellite {

Sparkline::Sparkline(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_TransparentForMouseEvents);
}

void Sparkline::setValues(const QVector<double> &values)
{
	if (values_ == values)
		return;

	values_ = values;
	update();
}

QSize Sparkline::sizeHint() const
{
	return QSize(120, 18);
}

QSize Sparkline::minimumSizeHint() const
{
	return QSize(40, 12);
}

void Sparkline::paintEvent(QPaintEvent *)
{
	if (values_.size() < 2)
		return;

	const double maximum = *std::max_element(values_.begin(), values_.end());
	if (maximum <= 0.0)
		return;

	const QRectF area = rect().adjusted(1, 1, -1, -1);
	if (area.width() <= 0.0 || area.height() <= 0.0)
		return;

	const double step = area.width() / static_cast<double>(values_.size() - 1);

	QPainterPath line;
	for (int index = 0; index < values_.size(); ++index) {
		const double normalized = std::clamp(values_[index] / maximum, 0.0, 1.0);
		const QPointF point(area.left() + step * index, area.bottom() - normalized * area.height());

		if (index == 0)
			line.moveTo(point);
		else
			line.lineTo(point);
	}

	// Follow the theme rather than hard-coding colours: OBS ships light and dark themes and
	// users ship more.
	QColor stroke = palette().color(QPalette::Highlight);
	QColor fill = stroke;
	fill.setAlpha(60);

	QPainterPath area_path = line;
	area_path.lineTo(area.right(), area.bottom());
	area_path.lineTo(area.left(), area.bottom());
	area_path.closeSubpath();

	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.fillPath(area_path, fill);
	painter.setPen(QPen(stroke, 1.2));
	painter.drawPath(line);
}

} // namespace satellite
