#include "InstallModDialog.hpp"
#include "ModManifestReader.hpp"
#include "ModInstallPlanner.hpp"
#include "ModInstaller.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace gf::gui {

InstallModDialog::InstallModDialog(const ModProfile& profile,
                                   QWidget*          parent)
    : QDialog(parent)
    , m_profile(profile)
{
    setWindowTitle(QString("Install Mod \u2014 %1").arg(profile.name));
    setMinimumSize(580, 460);
    setAttribute(Qt::WA_DeleteOnClose);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(8);

    // Header
    auto* hdr = new QLabel(
        QString("<b>Installing into profile:</b> %1").arg(profile.name.toHtmlEscaped()),
        this);
    outer->addWidget(hdr);

    // Folder picker row
    auto* pathRow = new QHBoxLayout();
    pathRow->setSpacing(6);
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText("Select a mod folder containing astra_mod.json…");
    m_pathEdit->setReadOnly(true);
    m_btnBrowse = new QPushButton("Browse\u2026", this);
    m_btnBrowse->setFixedWidth(80);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(m_btnBrowse);
    outer->addLayout(pathRow);

    // Validation / plan summary
    m_summary = new QTextBrowser(this);
    m_summary->setReadOnly(true);
    m_summary->setOpenLinks(false);
    m_summary->setHtml(
        "<i>No mod selected. Click Browse to choose a local mod folder.</i>");
    outer->addWidget(m_summary, 1);

    // Bottom button row
    auto* btns = new QHBoxLayout();
    btns->addStretch(1);
    m_btnInstall = new QPushButton("Install", this);
    m_btnInstall->setEnabled(false);
    m_btnInstall->setDefault(true);
    m_btnClose = new QPushButton("Close", this);
    btns->addWidget(m_btnInstall);
    btns->addWidget(m_btnClose);
    outer->addLayout(btns);

    connect(m_btnBrowse,  &QPushButton::clicked, this, &InstallModDialog::onBrowse);
    connect(m_btnInstall, &QPushButton::clicked, this, &InstallModDialog::onInstall);
    connect(m_btnClose,   &QPushButton::clicked, this, &QDialog::close);
}

void InstallModDialog::onBrowse() {
    const QString folder = QFileDialog::getExistingDirectory(
        this,
        "Select Mod Folder",
        QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (folder.isEmpty()) return;

    m_pathEdit->setText(folder);
    validateFolder(folder);
}

void InstallModDialog::validateFolder(const QString& folder) {
    m_manifest.reset();
    m_plan.reset();
    m_btnInstall->setEnabled(false);

    if (folder.isEmpty()) {
        m_summary->setHtml(
            "<i>No mod selected. Click Browse to choose a local mod folder.</i>");
        return;
    }

    m_summary->setHtml("<i>Reading manifest…</i>");

    // Read manifest
    QString manifestErr;
    m_manifest = ModManifestReader::readFromFolder(folder, &manifestErr);
    if (!m_manifest.has_value()) {
        m_summary->setHtml(QString(
            "<p style='color:#c0392b;'><b>Manifest error:</b></p>"
            "<p>%1</p>").arg(manifestErr.toHtmlEscaped()));
        return;
    }

    // Build plan
    const InstallPlan plan = ModInstallPlanner::buildPlan(
        *m_manifest, m_profile, m_registry);
    m_plan = plan;
    showPlanSummary(plan);
    m_btnInstall->setEnabled(plan.canProceed());
}

void InstallModDialog::showPlanSummary(const InstallPlan& plan) {
    const ModManifest& mf = plan.manifest;
    QString html;

    // Icon thumbnail (float right so text wraps naturally)
    if (!mf.icon.isEmpty()) {
        const QString iconAbs = QDir(mf.sourcePath).filePath(mf.icon);
        if (QFileInfo::exists(iconAbs)) {
            html += QString("<img src='%1' width='64' height='64' "
                            "style='float:right; margin-left:10px; margin-bottom:4px; "
                            "border-radius:4px;'/>")
                        .arg(QUrl::fromLocalFile(iconAbs).toString());
        }
    }

    // Mod header
    html += QString("<p><b>%1</b> v%2</p>")
                .arg(mf.name.toHtmlEscaped(), mf.version.toHtmlEscaped());
    if (!mf.author.isEmpty())
        html += QString("<p style='color:gray;'>Author: %1</p>")
                    .arg(mf.author.toHtmlEscaped());
    if (!mf.description.isEmpty())
        html += QString("<p>%1</p>").arg(mf.description.toHtmlEscaped());

    html += QString("<p>Files to install: <b>%1</b></p>").arg(plan.ops.size());
    html += "<hr/>";

    // Hard errors — installation blocked
    if (!plan.hardErrors.isEmpty()) {
        html += "<p style='color:#c0392b;'><b>Cannot install:</b></p><ul>";
        for (const auto& e : plan.hardErrors)
            html += QString("<li style='color:#c0392b;'>%1</li>")
                        .arg(e.toHtmlEscaped());
        html += "</ul>";
    }

    // Warnings — non-blocking
    if (!plan.warnings.isEmpty()) {
        html += "<p style='color:#b87300;'><b>Warnings:</b></p><ul>";
        for (const auto& w : plan.warnings)
            html += QString("<li style='color:#b87300;'>%1</li>")
                        .arg(w.message.toHtmlEscaped());
        html += "</ul>";
    }

    // File list (capped at 20 for readability)
    if (plan.canProceed() && !plan.ops.isEmpty()) {
        html += "<p><b>Files:</b></p><ul>";
        const int shown = qMin(plan.ops.size(), 20);
        for (int i = 0; i < shown; ++i)
            html += QString("<li>%1</li>").arg(plan.ops[i].relPath.toHtmlEscaped());
        if (plan.ops.size() > 20)
            html += QString("<li><i>… and %1 more</i></li>")
                        .arg(plan.ops.size() - 20);
        html += "</ul>";
    }

    m_summary->setHtml(html);
}

void InstallModDialog::onInstall() {
    if (!m_manifest.has_value() || !m_plan.has_value()) return;
    if (!m_plan->canProceed()) return;

    m_btnInstall->setEnabled(false);
    m_btnBrowse->setEnabled(false);

    const ModInstallReport report =
        ModInstaller::install(*m_manifest, m_profile, m_registry);

    showResult(report.success, report.message, report.warnings);

    if (report.success) {
        m_btnClose->setText("Done");
    } else {
        // Re-enable for a retry after a transient failure
        m_btnBrowse->setEnabled(true);
        m_btnInstall->setEnabled(m_plan->canProceed());
    }
}

void InstallModDialog::showResult(bool success,
                                   const QString& message,
                                   const QStringList& warnings) {
    QString html;
    if (success) {
        html += "<p style='color:#27ae60;'><b>Installed successfully!</b></p>";
        html += QString("<p>%1</p>").arg(message.toHtmlEscaped());
    } else {
        html += "<p style='color:#c0392b;'><b>Installation failed.</b></p>";
        html += QString("<p>%1</p>").arg(message.toHtmlEscaped());
    }
    if (!warnings.isEmpty()) {
        html += "<p style='color:#b87300;'><b>Warnings:</b></p><ul>";
        for (const auto& w : warnings)
            html += QString("<li style='color:#b87300;'>%1</li>")
                        .arg(w.toHtmlEscaped());
        html += "</ul>";
    }
    m_summary->setHtml(html);
}

} // namespace gf::gui
