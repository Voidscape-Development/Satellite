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

#include "transport/transport.hpp"

#include <string>
#include <vector>

namespace satellite {

/// Translates DistroAV's ndi_bw_mode onto Satellite's shared Quality.
///
/// The numbers are DistroAV's PROP_BW_* constants, read from its ndi-source.cpp rather than
/// guessed. Exposed so the mapping can be tested without a running OBS, because a silent
/// change here would quietly downgrade every imported source.
Quality distroav_quality_from_bandwidth(int bandwidth);

/// One thing a DistroAV setup contains that Satellite can reproduce.
struct ImportItem {
	enum class Kind {
		/// An "ndi_source" input, wherever it appears.
		Source,
		/// An "ndi_filter" or "ndi_audiofilter" attached to some source.
		Filter,
		/// DistroAV's main output settings, from OBS's global config.
		ProgramOutput,
		/// DistroAV's preview output settings.
		PreviewOutput,
	};

	Kind kind = Kind::Source;

	/// What the user sees in the dialog.
	std::string label;
	/// The one-line explanation of what converting it will do.
	std::string detail;
	/// Set when Satellite cannot reproduce something faithfully, e.g. an audio-only filter.
	std::string caveat;

	/// The DistroAV source's name, or the parent source's name for a filter.
	std::string owner;
	/// The filter's own name, for Kind::Filter.
	std::string filter_name;
	/// The DistroAV source id, so apply() knows exactly what it is looking at.
	std::string source_id;

	/// Satellite equivalent, already translated from the DistroAV settings.
	std::string network_name;
	std::string quality_id;
	bool audio = true;

	bool selected = true;
};

/// Looks for a DistroAV setup in the loaded scene collection and OBS's config.
///
/// Read-only: it changes nothing, so it is safe to call whenever, including just to decide
/// whether offering the import is worth it.
std::vector<ImportItem> scan_for_distroav();

/// Creates Satellite equivalents for the selected items.
///
/// Non-destructive by default: the DistroAV sources and filters are left exactly where they
/// are, and the Satellite versions are added alongside, so nothing is lost if the conversion
/// is not what the user wanted. Passing remove_originals removes the DistroAV items that were
/// successfully converted, and only those.
///
/// Returns how many items were converted.
int apply_distroav_import(const std::vector<ImportItem> &items, bool remove_originals);

} // namespace satellite
