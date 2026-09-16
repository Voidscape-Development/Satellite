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

#include "ui/import-dialog.hpp"

#include "config/config.hpp"
#include "obs/satellite-output.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <obs-module.h>
#include <plugin-support.h>

namespace satellite {

namespace {

QString kind_label(ImportItem::Kind kind)
{
	switch (kind) {
	case ImportItem::Kind::Source:
		return QObject::tr("Source");
	case ImportItem::Kind::Filter:
		return QObject::tr("Filter");
	case ImportItem::Kind::ProgramOutput:
	case ImportItem::Kind::PreviewOutput:
		return QObject::tr("Output");
	}
	return {};
}

} // namespace

ImportDialog::ImportDialog(std::vector<ImportItem> items, QWidget *parent) : QDialog(parent), items_(std::move(items))
{
	setWindowTitle(obs_module_text("Satellite.Import.Title"));
	resize(640, 420);

	auto *layout = new QVBoxLayout(this);

	auto *intro = new QLabel(obs_module_text("Satellite.Import.Intro"), this);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	tree_ = new QTreeWidget(this);
	tree_->setRootIsDecorated(false);
	tree_->setAlternatingRowColors(true);
	tree_->setColumnCount(3);
	tree_->setHeaderLabels({obs_module_text("Satellite.Import.Column.Item"),
				obs_module_text("Satellite.Import.Column.Kind"),
				obs_module_text("Satellite.Import.Column.Becomes")});
	tree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
	buildRows(tree_);
	layout->addWidget(tree_, 1);

	removeOriginals_ = new QCheckBox(obs_module_text("Satellite.Import.RemoveOriginals"), this);
	removeOriginals_->setChecked(false);
	removeOriginals_->setToolTip(obs_module_text("Satellite.Import.RemoveOriginals.Hint"));
	layout->addWidget(removeOriginals_);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	buttons->button(QDialogButtonBox::Ok)->setText(obs_module_text("Satellite.Import.Convert"));
	connect(buttons, &QDialogButtonBox::accepted, this, [this] {
		collectSelection();
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);
}

void ImportDialog::buildRows(QTreeWidget *tree)
{
	for (size_t index = 0; index < items_.size(); ++index) {
		const ImportItem &item = items_[index];

		auto *row = new QTreeWidgetItem(tree);
		row->setText(0, QString::fromStdString(item.label));
		row->setText(1, kind_label(item.kind));
		row->setText(2, QString::fromStdString(item.detail));
		row->setCheckState(0, item.selected ? Qt::Checked : Qt::Unchecked);
		row->setData(0, Qt::UserRole, static_cast<qulonglong>(index));

		if (!item.caveat.empty()) {
			// Flag anything Satellite cannot reproduce exactly, rather than converting it
			// quietly and letting the difference turn up on air.
			const QString caveat = QString::fromStdString(item.caveat);
			row->setToolTip(0, caveat);
			row->setToolTip(2, caveat);
			row->setText(2, row->text(2) + QStringLiteral("  ⚠"));
		}
	}
}

void ImportDialog::collectSelection()
{
	for (int row = 0; row < tree_->topLevelItemCount(); ++row) {
		QTreeWidgetItem *widget_item = tree_->topLevelItem(row);
		const size_t index = widget_item->data(0, Qt::UserRole).toULongLong();
		if (index < items_.size())
			items_[index].selected = widget_item->checkState(0) == Qt::Checked;
	}
}

bool ImportDialog::removeOriginals() const
{
	return removeOriginals_->isChecked();
}

bool run_distroav_import(QWidget *parent)
{
	std::vector<ImportItem> items = scan_for_distroav();
	if (items.empty())
		return false;

	ImportDialog dialog(std::move(items), parent);
	if (dialog.exec() != QDialog::Accepted)
		return true;

	const int converted = apply_distroav_import(dialog.items(), dialog.removeOriginals());

	// Output settings may have changed, so bring the frontend senders in line with them.
	update_frontend_outputs();

	QMessageBox::information(parent, obs_module_text("Satellite.Import.Title"),
				 QString(obs_module_text("Satellite.Import.Done")).arg(converted));
	return true;
}

void maybe_offer_distroav_import(QWidget *parent)
{
	Config &config = Config::instance();
	if (config.distroav_import_offered)
		return;

	std::vector<ImportItem> items = scan_for_distroav();
	if (items.empty()) {
		// Nothing to convert. Don't record the offer - a DistroAV setup may well be in a
		// scene collection that simply is not loaded yet.
		return;
	}

	// Recorded before the dialog runs, so declining it is also an answer and the user is
	// not asked again on every launch.
	config.distroav_import_offered = true;
	config.save();

	const auto choice = QMessageBox::question(parent, obs_module_text("Satellite.Import.Title"),
						  QString(obs_module_text("Satellite.Import.Offer")).arg(items.size()),
						  QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
	if (choice != QMessageBox::Yes) {
		obs_log(LOG_INFO, "DistroAV import declined; it stays available from the Satellite window");
		return;
	}

	ImportDialog dialog(std::move(items), parent);
	if (dialog.exec() != QDialog::Accepted)
		return;

	const int converted = apply_distroav_import(dialog.items(), dialog.removeOriginals());
	update_frontend_outputs();

	QMessageBox::information(parent, obs_module_text("Satellite.Import.Title"),
				 QString(obs_module_text("Satellite.Import.Done")).arg(converted));
}

} // namespace satellite
