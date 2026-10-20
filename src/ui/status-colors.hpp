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

#include "metrics/feed-registry.hpp"

#include <QColor>
#include <QPalette>

namespace satellite {

/// The few colours the Satellite window does not take from the theme. They carry meaning -
/// healthy, waiting, broken, on air - and are chosen to read on both OBS's light and dark
/// themes. Everything else follows the palette.
inline const QColor kStatusGoodColor(0x3f, 0xb9, 0x50);
inline const QColor kStatusWaitingColor(0xd2, 0x99, 0x22);
inline const QColor kStatusErrorColor(0xf8, 0x51, 0x49);

/// Tally badges, in the broadcast convention: red is on air, green is cued.
inline const QColor kTallyProgramColor(0xd9, 0x30, 0x30);
inline const QColor kTallyPreviewColor(0x2e, 0x9e, 0x4f);

inline QColor feed_state_color(FeedState state, const QPalette &palette)
{
	switch (state) {
	case FeedState::Connected:
		return kStatusGoodColor;
	case FeedState::Connecting:
		return kStatusWaitingColor;
	case FeedState::Error:
		return kStatusErrorColor;
	case FeedState::Idle:
		break;
	}
	return palette.color(QPalette::Disabled, QPalette::Text);
}

} // namespace satellite
