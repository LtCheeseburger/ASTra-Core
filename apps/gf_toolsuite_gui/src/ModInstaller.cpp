#include "ModInstaller.hpp"
#include "ModInstallPlanner.hpp"
#include "ModInstallTransaction.hpp"
#include "ModRegistryStore.hpp"

#include <gf/core/log.hpp>

#include <QCryptographicHash>
#include <QDateTime>

namespace gf::gui {

// static
ModInstallReport ModInstaller::install(const ModManifest& manifest,
                                        const ModProfile&  profile,
                                        ModRegistryStore&  registry) {
    ModInstallReport report;

    // ── Step 1: Plan ──────────────────────────────────────────────────────────
    const InstallPlan plan = ModInstallPlanner::buildPlan(manifest, profile, registry);
    report.plan = plan;

    for (const auto& w : plan.warnings)
        report.warnings << w.message;

    if (!plan.canProceed()) {
        report.success = false;
        report.message = "Installation refused:\n" + plan.hardErrors.join("\n");
        report.errors  = plan.hardErrors;
        gf::core::logWarn(gf::core::LogCategory::General,
                          "ModInstaller: install refused",
                          plan.hardErrors.first().toStdString());
        return report;
    }

    // ── Step 2: Execute transaction ───────────────────────────────────────────
    const InstallResult txResult = ModInstallTransaction::execute(plan);
    if (!txResult.success) {
        report.success = false;
        report.message = "Installation failed: " + txResult.message;
        report.errors  = txResult.errors;
        gf::core::logError(gf::core::LogCategory::FileIO,
                           "ModInstaller: transaction failed",
                           txResult.message.toStdString());
        return report;
    }

    // ── Step 3: Write registry record ─────────────────────────────────────────
    InstalledModRecord rec = buildRecord(plan, txResult, profile);
    QString regErr;
    if (!registry.appendRecord(profile.workspacePath, rec, &regErr)) {
        // Files are installed but the registry write failed.  Log hard and
        // return a partial-success result — do NOT remove the installed files.
        rec.status = "partial";
        report.success = true; // files are present on disk
        report.message =
            QString("Files installed but registry could not be updated.\n"
                    "You may need to re-register this mod manually.\n"
                    "Error: %1").arg(regErr);
        report.warnings << "Registry write failed: " + regErr;
        report.record = rec;
        gf::core::logError(gf::core::LogCategory::FileIO,
                           "ModInstaller: registry write failed after successful install",
                           regErr.toStdString());
        return report;
    }

    report.success = true;
    report.message = QString("'%1 v%2' installed successfully (%3 file(s)).")
        .arg(manifest.name, manifest.version)
        .arg(txResult.installedFiles.size());
    report.record = rec;

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModInstaller: installed",
                      (manifest.modId + " v" + manifest.version +
                       " → profile " + profile.id).toStdString());
    return report;
}

// static
InstalledModRecord ModInstaller::buildRecord(const InstallPlan&   plan,
                                              const InstallResult& txResult,
                                              const ModProfile&    profile) {
    const QByteArray hashBytes = QCryptographicHash::hash(
        plan.manifest.manifestRawBytes, QCryptographicHash::Sha256);

    InstalledModRecord r;
    r.installId      = plan.installId;
    r.profileId      = profile.id;
    r.gameId         = profile.gameId;
    r.modId          = plan.manifest.modId;
    r.modVersion     = plan.manifest.version;
    r.modName        = plan.manifest.name;
    r.sourcePath     = plan.manifest.sourcePath;
    r.installedFiles = txResult.installedFiles;
    r.manifestHash   = QString::fromLatin1(hashBytes.toHex());
    r.enabled        = true;
    r.installedAt    = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    r.status         = "ok";
    for (const auto& w : plan.warnings) r.warnings << w.message;
    return r;
}

} // namespace gf::gui
