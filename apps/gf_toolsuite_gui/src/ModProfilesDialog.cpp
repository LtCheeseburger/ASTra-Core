#include "ModProfilesDialog.hpp"
#include "ModProfileManager.hpp"
#include "ShellHelper.hpp"

#include <QDialogButtonBox>
#include <QDir>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace gf::gui {

ModProfilesDialog::ModProfilesDialog(ModProfileManager* mgr,
                                     const QString&     gameId,
                                     const QString&     gameDisplayName,
                                     QWidget*           parent)
    : QDialog(parent)
    , m_mgr(mgr)
    , m_gameId(gameId)
    , m_gameDisplayName(gameDisplayName)
{
    setWindowTitle(QString("Mod Profiles \u2014 %1").arg(gameDisplayName));
    setMinimumSize(560, 380);
    resize(720, 480);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 8);
    mainLayout->setSpacing(8);

    // ── Splitter: list left, details right ──────────────────────────────────
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setHandleWidth(4);

    // ── Left: profile list ───────────────────────────────────────────────────
    auto* listGroup  = new QGroupBox("Profiles", splitter);
    auto* listLayout = new QVBoxLayout(listGroup);
    listLayout->setContentsMargins(6, 8, 6, 6);

    m_list = new QListWidget(listGroup);
    m_list->setAlternatingRowColors(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    // Phase 5D: enable right-click context menu
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    listLayout->addWidget(m_list);
    splitter->addWidget(listGroup);

    // ── Right: details pane ──────────────────────────────────────────────────
    auto* detailGroup  = new QGroupBox("Details", splitter);
    auto* detailLayout = new QVBoxLayout(detailGroup);
    detailLayout->setContentsMargins(6, 8, 6, 6);
    detailLayout->setSpacing(6);

    // Phase 5D: QTextBrowser instead of QLabel — scrollable, selectable, better HTML
    m_detailsPane = new QTextBrowser(detailGroup);
    m_detailsPane->setOpenLinks(false);
    m_detailsPane->setReadOnly(true);
    m_detailsPane->setHtml("<p style='color:gray;'>(No profile selected)</p>");
    detailLayout->addWidget(m_detailsPane, 1);

    // Phase 5D: Open Location / Copy Path action buttons in the details group
    auto* detailBtnRow = new QHBoxLayout();
    detailBtnRow->setSpacing(6);
    m_btnOpenLocation = new QPushButton("Open Location", detailGroup);
    m_btnOpenLocation->setToolTip("Open the workspace folder in the system file manager");
    m_btnOpenLocation->setEnabled(false);
    m_btnCopyPath = new QPushButton("Copy Path", detailGroup);
    m_btnCopyPath->setToolTip("Copy the workspace folder path to the clipboard");
    m_btnCopyPath->setEnabled(false);
    detailBtnRow->addWidget(m_btnOpenLocation);
    detailBtnRow->addWidget(m_btnCopyPath);
    detailBtnRow->addStretch(1);
    detailLayout->addLayout(detailBtnRow);

    splitter->addWidget(detailGroup);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    mainLayout->addWidget(splitter, 1);

    // ── Action buttons ───────────────────────────────────────────────────────
    auto* actRow = new QHBoxLayout;
    actRow->setSpacing(6);

    m_btnCreate    = new QPushButton("New\u2026",    this);
    m_btnRename    = new QPushButton("Rename\u2026", this);
    m_btnDelete    = new QPushButton("Delete\u2026", this);
    actRow->addWidget(m_btnCreate);
    actRow->addWidget(m_btnRename);
    actRow->addWidget(m_btnDelete);
    actRow->addStretch();

    m_btnSetActive  = new QPushButton("Set Active",  this);
    m_btnDeactivate = new QPushButton("Deactivate",  this);
    m_btnSetActive->setToolTip("Mark this profile as the active profile for the current game");
    m_btnDeactivate->setToolTip("Clear the active profile selection for the current game");
    actRow->addWidget(m_btnSetActive);
    actRow->addWidget(m_btnDeactivate);

    mainLayout->addLayout(actRow);

    // ── Close button ─────────────────────────────────────────────────────────
    auto* closeBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(closeBox, &QDialogButtonBox::rejected, this, &QDialog::accept);
    mainLayout->addWidget(closeBox);

    // ── Connections ──────────────────────────────────────────────────────────
    connect(m_list, &QListWidget::currentRowChanged,
            this, &ModProfilesDialog::onSelectionChanged);
    connect(m_list, &QListWidget::customContextMenuRequested,
            this, &ModProfilesDialog::onContextMenu);

    connect(m_btnCreate,       &QPushButton::clicked, this, &ModProfilesDialog::onCreate);
    connect(m_btnRename,       &QPushButton::clicked, this, &ModProfilesDialog::onRename);
    connect(m_btnDelete,       &QPushButton::clicked, this, &ModProfilesDialog::onDelete);
    connect(m_btnSetActive,    &QPushButton::clicked, this, &ModProfilesDialog::onSetActive);
    connect(m_btnDeactivate,   &QPushButton::clicked, this, &ModProfilesDialog::onDeactivate);
    connect(m_btnOpenLocation, &QPushButton::clicked, this, &ModProfilesDialog::onOpenLocation);
    connect(m_btnCopyPath,     &QPushButton::clicked, this, &ModProfilesDialog::onCopyPath);

    // Refresh when the manager's data changes
    connect(m_mgr, &ModProfileManager::profilesChanged,
            this, [this](const QString& gid) {
                if (gid == m_gameId) rebuildList();
            });
    connect(m_mgr, &ModProfileManager::activeProfileChanged,
            this, [this](const QString& gid, const QString&) {
                if (gid == m_gameId) rebuildList();
            });

    rebuildList();
}

// ── Private helpers ───────────────────────────────────────────────────────────

void ModProfilesDialog::rebuildList() {
    // Preserve current selection by id
    const QString prevId = [this]() -> QString {
        const auto opt = selectedProfile();
        return opt ? opt->id : QString{};
    }();

    m_list->blockSignals(true);
    m_list->clear();

    const QVector<ModProfile> profiles = m_mgr->profilesForGame(m_gameId);
    const auto                activeId = m_mgr->activeProfileId(m_gameId);

    for (const auto& p : profiles) {
        const bool isActive = activeId && *activeId == p.id;
        QString    label    = p.name;
        if (isActive)   label += "  \u2605"; // filled star
        if (p.isBaseline) label += "  \u25b6"; // small right-pointing indicator

        auto* item = new QListWidgetItem(label, m_list);
        item->setData(Qt::UserRole, p.id);

        if (isActive) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
    }

    m_list->blockSignals(false);

    // Restore selection
    bool restored = false;
    if (!prevId.isEmpty()) {
        for (int i = 0; i < m_list->count(); ++i) {
            if (m_list->item(i)->data(Qt::UserRole).toString() == prevId) {
                m_list->setCurrentRow(i);
                restored = true;
                break;
            }
        }
    }
    if (!restored && m_list->count() > 0)
        m_list->setCurrentRow(0);

    onSelectionChanged();
}

void ModProfilesDialog::onSelectionChanged() {
    const auto opt = selectedProfile();
    if (opt) {
        const auto activeId = m_mgr->activeProfileId(m_gameId);
        const bool isActive = activeId && *activeId == opt->id;

        const QString activeBadge =
            isActive
            ? QStringLiteral(" <span style='background:#3a9;color:white;"
                             "border-radius:3px;padding:1px 5px;"
                             "font-size:11px;'>Active</span>")
            : QString();

        const QString baselineBadge =
            opt->isBaseline
            ? QString(" <span style='background:#7580a0;color:white;"
                      "border-radius:3px;padding:1px 5px;"
                      "font-size:11px;'>%1</span>")
                  .arg(baselineTypeLabel(opt->baselineType).toHtmlEscaped())
            : QString();

        const QString nativePath =
            QDir::toNativeSeparators(opt->workspacePath).toHtmlEscaped();

        QString html;
        html += QString("<p style='margin:0 0 4px 0;'>"
                        "<b>%1</b>%2%3</p>")
                    .arg(opt->name.toHtmlEscaped(), activeBadge, baselineBadge);

        html += QString("<p style='color:#888;margin:0 0 6px 0;font-size:11px;'>"
                        "Created: %1 &nbsp;\u2502&nbsp; Updated: %2</p>")
                    .arg(opt->createdAt.toHtmlEscaped(),
                         opt->updatedAt.toHtmlEscaped());

        if (!opt->description.isEmpty()) {
            html += QString("<p style='margin:0 0 6px 0;'>%1</p>")
                        .arg(opt->description.toHtmlEscaped());
        }

        html += QStringLiteral("<hr style='margin:4px 0;'/>");

        html += QString("<p style='margin:0 0 2px 0;'><b>Workspace</b></p>"
                        "<p style='font-family:monospace;font-size:11px;"
                        "color:#555;word-break:break-all;margin:0;'>%1</p>")
                    .arg(nativePath);

        m_detailsPane->setHtml(html);
    } else {
        m_detailsPane->setHtml(
            "<p style='color:gray;'>(No profile selected)</p>");
    }
    updateButtons();
}

void ModProfilesDialog::updateButtons() {
    const auto opt    = selectedProfile();
    const bool hasSel = opt.has_value();

    const bool isActive = hasSel && [&] {
        const auto aid = m_mgr->activeProfileId(m_gameId);
        return aid && *aid == opt->id;
    }();

    m_btnRename->setEnabled(hasSel);
    // Prevent deleting the active profile — user must deactivate first.
    m_btnDelete->setEnabled(hasSel && !isActive);
    m_btnSetActive->setEnabled(hasSel && !isActive);
    m_btnDeactivate->setEnabled(isActive);

    // Phase 5D: Open/Copy only make sense when there's a selection
    m_btnOpenLocation->setEnabled(hasSel);
    m_btnCopyPath->setEnabled(hasSel);
}

std::optional<ModProfile> ModProfilesDialog::selectedProfile() const {
    const QListWidgetItem* item = m_list->currentItem();
    if (!item) return std::nullopt;
    return m_mgr->findById(item->data(Qt::UserRole).toString());
}

// ── Slot implementations ──────────────────────────────────────────────────────

void ModProfilesDialog::onCreate() {
    bool ok = false;
    const QString name = QInputDialog::getText(
        this,
        "New Mod Profile",
        "Profile name:",
        QLineEdit::Normal,
        {},
        &ok).trimmed();

    if (!ok || name.isEmpty()) return;

    ModProfile created;
    QString    err;
    // Phase 5D: pass game display name so the workspace uses the readable naming.
    if (!m_mgr->createProfile(m_gameId, name, {}, &created, &err,
                               m_gameDisplayName)) {
        QMessageBox::warning(this, "Create Profile", err);
        return;
    }
    // rebuildList() is called via profilesChanged signal
}

void ModProfilesDialog::onRename() {
    const auto opt = selectedProfile();
    if (!opt) return;

    bool ok = false;
    const QString newName = QInputDialog::getText(
        this,
        "Rename Profile",
        "New name:",
        QLineEdit::Normal,
        opt->name,
        &ok).trimmed();

    if (!ok || newName.isEmpty()) return;

    QString err;
    if (!m_mgr->renameProfile(opt->id, newName, &err)) {
        QMessageBox::warning(this, "Rename Profile", err);
    }
}

void ModProfilesDialog::onDelete() {
    const auto opt = selectedProfile();
    if (!opt) return;

    // Step 1: confirm deletion
    const auto confirm = QMessageBox::question(
        this,
        "Delete Profile",
        QString("Delete profile \"%1\"?\n\n"
                "The workspace files on disk will not be touched.\n"
                "This cannot be undone.")
            .arg(opt->name),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if (confirm != QMessageBox::Yes) return;

    // Step 2: optionally delete workspace files
    const auto wsConfirm = QMessageBox::question(
        this,
        "Delete Workspace Files",
        QString("Also permanently delete the workspace directory?\n\n"
                "%1\n\n"
                "Warning: all files in the workspace will be lost.")
            .arg(QDir::toNativeSeparators(opt->workspacePath)),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    const bool deleteFiles = (wsConfirm == QMessageBox::Yes);

    QString err;
    if (!m_mgr->deleteProfile(opt->id, deleteFiles, &err)) {
        QMessageBox::warning(this, "Delete Profile", err);
    }
}

void ModProfilesDialog::onSetActive() {
    const auto opt = selectedProfile();
    if (!opt) return;

    QString err;
    if (!m_mgr->setActiveProfile(m_gameId, opt->id, &err)) {
        QMessageBox::warning(this, "Set Active Profile", err);
    }
}

void ModProfilesDialog::onDeactivate() {
    const auto opt = selectedProfile();
    if (!opt) return;
    m_mgr->clearActiveProfile(m_gameId);
}

void ModProfilesDialog::onOpenLocation() {
    const auto opt = selectedProfile();
    if (!opt) return;
    ShellHelper::openFolder(opt->workspacePath);
}

void ModProfilesDialog::onCopyPath() {
    const auto opt = selectedProfile();
    if (!opt) return;
    ShellHelper::copyToClipboard(QDir::toNativeSeparators(opt->workspacePath));
}

void ModProfilesDialog::onContextMenu(const QPoint& pos) {
    const QListWidgetItem* item = m_list->itemAt(pos);
    if (!item) return;

    // Ensure the right-clicked item is also the current selection so that the
    // action handlers (onRename, onDelete, etc.) operate on the correct profile.
    m_list->setCurrentItem(const_cast<QListWidgetItem*>(item));

    const auto opt = m_mgr->findById(item->data(Qt::UserRole).toString());
    if (!opt) return;

    const auto activeId = m_mgr->activeProfileId(m_gameId);
    const bool isActive = activeId && *activeId == opt->id;

    QMenu menu(this);

    QAction* actSetActive   = menu.addAction("Set Active");
    actSetActive->setEnabled(!isActive);

    QAction* actDeactivate  = menu.addAction("Deactivate");
    actDeactivate->setEnabled(isActive);

    menu.addSeparator();

    QAction* actRename = menu.addAction("Rename\u2026");

    menu.addSeparator();

    QAction* actOpenLocation = menu.addAction("Open Profile Location");
    QAction* actCopyPath     = menu.addAction("Copy Workspace Path");

    menu.addSeparator();

    QAction* actDelete = menu.addAction("Delete\u2026");
    actDelete->setEnabled(!isActive);

    QAction* chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if      (chosen == actSetActive)    onSetActive();
    else if (chosen == actDeactivate)   onDeactivate();
    else if (chosen == actRename)       onRename();
    else if (chosen == actDelete)       onDelete();
    else if (chosen == actOpenLocation) ShellHelper::openFolder(opt->workspacePath);
    else if (chosen == actCopyPath)
        ShellHelper::copyToClipboard(QDir::toNativeSeparators(opt->workspacePath));
}

} // namespace gf::gui
