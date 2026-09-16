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

#include "obs/distroav-import.hpp"

#include <QDialog>
#include <vector>

class QCheckBox;
class QTreeWidget;

namespace satellite {

/// Shows exactly what converting a DistroAV setup would produce, and converts nothing until
/// the user says so.
///
/// Every row is individually checkable, and the originals are left alone unless the user
/// explicitly asks for them to be removed - so an import that turns out to be wrong costs
/// nothing but a few extra sources.
class ImportDialog : public QDialog {
	Q_OBJECT

public:
	ImportDialog(std::vector<ImportItem> items, QWidget *parent = nullptr);

	/// Items with their selection state as the user left them.
	const std::vector<ImportItem> &items() const { return items_; }
	bool removeOriginals() const;

private:
	void buildRows(QTreeWidget *tree);
	void collectSelection();

	std::vector<ImportItem> items_;
	QTreeWidget *tree_ = nullptr;
	QCheckBox *removeOriginals_ = nullptr;
};

/// Runs the import flow: scan, show the dialog, convert what was approved.
///
/// Returns false when there was nothing to import, so the caller can stay quiet.
bool run_distroav_import(QWidget *parent);

/// Offers the import once, the first time a DistroAV setup is seen, and records that it has
/// been offered so the user is not asked again.
void maybe_offer_distroav_import(QWidget *parent);

} // namespace satellite
