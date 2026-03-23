#pragma once
#include "ModCatalogEntry.hpp"
#include <QAbstractTableModel>
#include <QVector>

namespace gf::gui {

// Qt table model presenting a flat list of ModCatalogEntry objects.
//
// Columns: Name | Version | Status | Installed
//
// Qt::UserRole on any column returns the full ModCatalogEntry as a
// QVariant (use qvariant_cast<ModCatalogEntry>).
class InstalledModsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColName        = 0,
        ColVersion     = 1,
        ColStatus      = 2,
        ColInstalledAt = 3,
        ColCount       = 4
    };

    explicit InstalledModsModel(QObject* parent = nullptr);

    // Replace all entries and reset the view.
    void setEntries(const QVector<ModCatalogEntry>& entries);

    // Access a single entry by row index.  Returns a default entry if out of range.
    ModCatalogEntry entryAt(int row) const;

    // QAbstractTableModel interface
    int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int      columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section,
                        Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    QVector<ModCatalogEntry> m_entries;
};

} // namespace gf::gui

Q_DECLARE_METATYPE(gf::gui::ModCatalogEntry)
