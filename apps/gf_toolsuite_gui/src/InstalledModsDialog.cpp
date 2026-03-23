#include "InstalledModsDialog.hpp"
#include "InstalledModsModel.hpp"
#include "ModCatalogService.hpp"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>
#include <optional>

namespace gf::gui {

InstalledModsDialog::InstalledModsDialog(const ModProfile& profile,
                                         const QString&    gameDisplayName,
                                         QWidget*          parent)
    : QDialog(parent)
    , m_profile(profile)
    , m_gameDisplayName(gameDisplayName)
{
    setWindowTitle(QString("Installed Mods \u2014 %1").arg(profile.name));
    setMinimumSize(760, 480);
    setAttribute(Qt::WA_DeleteOnClose);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(6);

    // Context header
    m_contextLabel = new QLabel(this);
    m_contextLabel->setStyleSheet("font-weight: bold;");
    m_contextLabel->setText(
        QString("Game: %1  \u2502  Profile: %2")
            .arg(gameDisplayName.toHtmlEscaped(), profile.name.toHtmlEscaped()));
    outer->addWidget(m_contextLabel);

    m_countLabel = new QLabel(this);
    m_countLabel->setStyleSheet("color: palette(mid);");
    outer->addWidget(m_countLabel);

    // Splitter: table (left) + details (right)
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    // ── Table ────────────────────────────────────────────────────────────────
    m_model = new InstalledModsModel(this);

    m_table = new QTableView(splitter);
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(false);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(
        InstalledModsModel::ColName, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(
        InstalledModsModel::ColVersion, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(
        InstalledModsModel::ColStatus, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(
        InstalledModsModel::ColInstalledAt, QHeaderView::ResizeToContents);
    splitter->addWidget(m_table);

    // ── Details pane ─────────────────────────────────────────────────────────
    m_details = new QTextBrowser(splitter);
    m_details->setReadOnly(true);
    m_details->setOpenLinks(false);
    m_details->setMinimumWidth(220);
    clearDetails();
    splitter->addWidget(m_details);

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    outer->addWidget(splitter, 1);

    // ── Button row ────────────────────────────────────────────────────────────
    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(6);

    m_btnEnable = new QPushButton("Enable", this);
    m_btnEnable->setEnabled(false);
    m_btnEnable->setToolTip("Enable the selected mod in this profile");

    m_btnDisable = new QPushButton("Disable", this);
    m_btnDisable->setEnabled(false);
    m_btnDisable->setToolTip("Disable the selected mod without removing its files");

    m_btnRefresh = new QPushButton("Refresh", this);
    m_btnRefresh->setToolTip("Re-read the registry and re-check installed files");

    m_btnClose = new QPushButton("Close", this);

    btnRow->addWidget(m_btnEnable);
    btnRow->addWidget(m_btnDisable);
    btnRow->addWidget(m_btnRefresh);
    btnRow->addStretch(1);
    btnRow->addWidget(m_btnClose);
    outer->addLayout(btnRow);

    // ── Connections ───────────────────────────────────────────────────────────
    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex&, const QModelIndex&) {
                onSelectionChanged();
            });

    connect(m_btnEnable,  &QPushButton::clicked, this, &InstalledModsDialog::onEnable);
    connect(m_btnDisable, &QPushButton::clicked, this, &InstalledModsDialog::onDisable);
    connect(m_btnRefresh, &QPushButton::clicked, this, &InstalledModsDialog::onRefresh);
    connect(m_btnClose,   &QPushButton::clicked, this, &QDialog::close);

    populate();
}

// ── private ───────────────────────────────────────────────────────────────────

void InstalledModsDialog::populate() {
    QString err;
    const auto entries = ModCatalogService::load(m_profile.workspacePath, &err);

    m_model->setEntries(entries);

    if (!err.isEmpty()) {
        m_countLabel->setText(QString("Error reading registry: %1").arg(err));
    } else if (entries.isEmpty()) {
        m_countLabel->setText("No mods installed in this profile.");
    } else {
        m_countLabel->setText(
            QString("%1 mod(s) installed.").arg(entries.size()));
    }

    clearDetails();
    updateButtons();
}

void InstalledModsDialog::onSelectionChanged() {
    updateButtons();

    const auto e = selectedEntry();
    if (!e.has_value()) { clearDetails(); return; }
    showEntryDetails(*e);
}

void InstalledModsDialog::onEnable() {
    const auto e = selectedEntry();
    if (!e.has_value()) return;

    QString err;
    if (!ModCatalogService::setEnabled(
            m_profile.workspacePath, e->record.installId, true, &err)) {
        QMessageBox::warning(this, "Enable Mod",
            QString("Could not enable '%1':\n\n%2")
                .arg(e->record.modName, err));
        return;
    }
    populate();
    // Re-select the same row by installId after reload
    const QString id = e->record.installId;
    for (int r = 0; r < m_model->rowCount(); ++r) {
        if (m_model->entryAt(r).record.installId == id) {
            m_table->selectRow(r);
            break;
        }
    }
}

void InstalledModsDialog::onDisable() {
    const auto e = selectedEntry();
    if (!e.has_value()) return;

    QString err;
    if (!ModCatalogService::setEnabled(
            m_profile.workspacePath, e->record.installId, false, &err)) {
        QMessageBox::warning(this, "Disable Mod",
            QString("Could not disable '%1':\n\n%2")
                .arg(e->record.modName, err));
        return;
    }
    populate();
    const QString id = e->record.installId;
    for (int r = 0; r < m_model->rowCount(); ++r) {
        if (m_model->entryAt(r).record.installId == id) {
            m_table->selectRow(r);
            break;
        }
    }
}

void InstalledModsDialog::onRefresh() {
    const auto e = selectedEntry();
    const QString prevId = e.has_value() ? e->record.installId : QString();

    populate();

    if (!prevId.isEmpty()) {
        for (int r = 0; r < m_model->rowCount(); ++r) {
            if (m_model->entryAt(r).record.installId == prevId) {
                m_table->selectRow(r);
                break;
            }
        }
    }
}

void InstalledModsDialog::updateButtons() {
    const auto e = selectedEntry();
    if (!e.has_value()) {
        m_btnEnable->setEnabled(false);
        m_btnDisable->setEnabled(false);
        return;
    }

    const bool isEnabled  = e->record.enabled;
    const bool isInvalid  = (e->status == ModEntryStatus::Invalid);

    // Enable button: only if currently disabled and not invalid (no files to enable)
    m_btnEnable->setEnabled(!isEnabled && !isInvalid);

    // Disable button: only if currently enabled (or partial)
    m_btnDisable->setEnabled(isEnabled || e->status == ModEntryStatus::Partial);
}

void InstalledModsDialog::showEntryDetails(const ModCatalogEntry& entry) {
    const InstalledModRecord& r = entry.record;

    QString html;

    // Icon thumbnail — look for icon.png in the mod's source folder
    if (!r.sourcePath.isEmpty()) {
        const QString iconPath = QDir(r.sourcePath).filePath("icon.png");
        if (QFileInfo::exists(iconPath)) {
            html += QString("<img src='%1' width='64' height='64' "
                            "style='float:right; margin-left:10px; margin-bottom:4px; "
                            "border-radius:4px;'/>")
                        .arg(QUrl::fromLocalFile(iconPath).toString());
        }
    }

    // Name + status badge
    const QString statusColor = [&]() -> QString {
        switch (entry.status) {
        case ModEntryStatus::Ok:       return "#27ae60";
        case ModEntryStatus::Disabled: return "#7f8c8d";
        case ModEntryStatus::Partial:  return "#e67e22";
        case ModEntryStatus::Invalid:  return "#c0392b";
        }
        return "#555555";
    }();

    html += QString(
        "<p><b>%1</b>&nbsp;"
        "<span style='background:%2; color:white; border-radius:4px; "
        "padding:1px 6px; font-size:11px;'>%3</span></p>")
        .arg(r.modName.toHtmlEscaped(), statusColor, entry.statusLabel());

    // Core metadata table
    html += "<table style='margin-top:4px;'>";
    auto row = [&](const QString& label, const QString& value) {
        if (!value.isEmpty())
            html += QString("<tr><td style='color:gray; padding-right:8px;'>%1</td>"
                            "<td>%2</td></tr>")
                        .arg(label.toHtmlEscaped(), value.toHtmlEscaped());
    };
    row("mod_id",    r.modId);
    row("Version",   r.modVersion);
    row("Installed", r.installedAt.left(10));
    row("Files",     QString::number(r.installedFiles.size()) +
                         (entry.missingFileCount > 0
                          ? QString(" (%1 missing)").arg(entry.missingFileCount)
                          : QString()));
    row("Source",    r.sourcePath);
    html += "</table>";
    html += "<hr/>";

    // Warnings
    if (!r.warnings.isEmpty()) {
        html += "<p style='color:#e67e22;'><b>Warnings:</b></p><ul>";
        for (const auto& w : r.warnings)
            html += QString("<li>%1</li>").arg(w.toHtmlEscaped());
        html += "</ul>";
    }

    // Disk state issues
    if (!entry.installRootExists) {
        html += "<p style='color:#c0392b;'>Install directory not found on disk.</p>";
    } else if (entry.missingFileCount > 0) {
        html += QString("<p style='color:#e67e22;'>%1 registered file(s) missing from disk.</p>")
                    .arg(entry.missingFileCount);
    }

    // Advanced: install ID + hash (collapsed visually but present)
    html += QString("<p style='color:gray; font-size:10px;'>install_id: %1</p>")
                .arg(r.installId.toHtmlEscaped());
    if (!r.manifestHash.isEmpty())
        html += QString("<p style='color:gray; font-size:10px;'>manifest SHA-256: %1</p>")
                    .arg(r.manifestHash.left(16).toHtmlEscaped() + "…");

    m_details->setHtml(html);
}

void InstalledModsDialog::clearDetails() {
    m_details->setHtml(
        "<p style='color:gray;'><i>Select a mod to see details.</i></p>");
}

std::optional<ModCatalogEntry> InstalledModsDialog::selectedEntry() const {
    const QModelIndex idx = m_table->currentIndex();
    if (!idx.isValid()) return std::nullopt;
    const ModCatalogEntry e = m_model->entryAt(idx.row());
    if (e.record.installId.isEmpty()) return std::nullopt;
    return e;
}

} // namespace gf::gui
