#include "InstalledModsModel.hpp"

#include <QColor>

namespace gf::gui {

InstalledModsModel::InstalledModsModel(QObject* parent)
    : QAbstractTableModel(parent)
{}

void InstalledModsModel::setEntries(const QVector<ModCatalogEntry>& entries) {
    beginResetModel();
    m_entries = entries;
    endResetModel();
}

ModCatalogEntry InstalledModsModel::entryAt(int row) const {
    if (row < 0 || row >= m_entries.size()) return {};
    return m_entries[row];
}

int InstalledModsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return m_entries.size();
}

int InstalledModsModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return ColCount;
}

QVariant InstalledModsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};

    const ModCatalogEntry& entry  = m_entries[index.row()];
    const InstalledModRecord& rec = entry.record;

    if (role == Qt::UserRole)
        return QVariant::fromValue(entry);

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColName:        return rec.modName.isEmpty() ? rec.modId : rec.modName;
        case ColVersion:     return rec.modVersion;
        case ColStatus:      return entry.statusLabel();
        case ColInstalledAt: return rec.installedAt.left(10); // date portion only
        default: break;
        }
    }

    if (role == Qt::ForegroundRole && index.column() == ColStatus) {
        switch (entry.status) {
        case ModEntryStatus::Ok:       return QColor(0x27, 0xAE, 0x60); // green
        case ModEntryStatus::Disabled: return QColor(0x7F, 0x8C, 0x8D); // gray
        case ModEntryStatus::Partial:  return QColor(0xE6, 0x7E, 0x22); // orange
        case ModEntryStatus::Invalid:  return QColor(0xC0, 0x39, 0x2B); // red
        }
    }

    if (role == Qt::ToolTipRole) {
        QString tip = QString("%1 v%2").arg(rec.modName, rec.modVersion);
        tip += QString("\nStatus: %1").arg(entry.statusLabel());
        if (entry.missingFileCount > 0)
            tip += QString("\nMissing files: %1").arg(entry.missingFileCount);
        if (!entry.installRootExists)
            tip += "\nInstall directory not found on disk.";
        return tip;
    }

    return {};
}

QVariant InstalledModsModel::headerData(int section,
                                         Qt::Orientation orientation,
                                         int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ColName:        return "Name";
    case ColVersion:     return "Version";
    case ColStatus:      return "Status";
    case ColInstalledAt: return "Installed";
    default:             return {};
    }
}

} // namespace gf::gui
