#include "ModPackageExporter.hpp"

#include <gf/core/log.hpp>
#include <gf/core/safe_write.hpp>

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProgressDialog>
#include <QRegularExpression>

#include <filesystem>

namespace gf::gui {

// Mod-id validation (mirrors ModManifestReader::validateModId)
static bool isValidModId(const QString& id) {
    static const QRegularExpression kSafeId(
        QStringLiteral(R"(^[A-Za-z0-9_\-]{3,64}$)"));
    return kSafeId.match(id).hasMatch();
}

// static
QString ModPackageExporter::layerSubdir(const QString&             destRootPath,
                                         const RuntimeTargetConfig& runtime) {
    if (destRootPath.isEmpty())
        return QStringLiteral("base");

    for (const RuntimeContentRoot& cr : runtime.contentRoots) {
        if (cr.path == destRootPath) {
            switch (cr.kind) {
            case RuntimeContentKind::Update:    return QStringLiteral("update");
            case RuntimeContentKind::Dlc:       return QStringLiteral("dlc");
            case RuntimeContentKind::CustomDlc: return QStringLiteral("custom_dlc");
            default:                            return QStringLiteral("base");
            }
        }
    }

    // destRootPath present but not matched — treat as base with a warning.
    // The caller checks this case and appends a warning.
    return QStringLiteral("base");
}

// static
bool ModPackageExporter::writeManifest(const ModExportSpec& spec,
                                        const QStringList&   payloadFiles,
                                        const QString&       packageDir,
                                        QString*             outErr) {
    QJsonObject root;
    root["schema_version"] = 1; // version 1 — fully compatible with existing installer
    root["mod_id"]         = spec.modId;
    root["name"]           = spec.name;
    root["version"]        = spec.version;
    root["package_type"]   = QStringLiteral("full_replacement");

    if (!spec.author.isEmpty())      root["author"]      = spec.author;
    if (!spec.description.isEmpty()) root["description"] = spec.description;
    if (!spec.notes.isEmpty())       root["notes"]       = spec.notes;
    if (!spec.category.isEmpty())    root["category"]    = spec.category;

    if (!spec.targetGameIds.isEmpty()) {
        QJsonArray arr;
        for (const QString& id : spec.targetGameIds) arr.append(id);
        root["target_game_ids"] = arr;
    }
    if (!spec.platforms.isEmpty()) {
        QJsonArray arr;
        for (const QString& p : spec.platforms) arr.append(p);
        root["platforms"] = arr;
    }
    if (!spec.tags.isEmpty()) {
        QJsonArray arr;
        for (const QString& t : spec.tags) arr.append(t);
        root["tags"] = arr;
    }

    // Phase 5C: icon / preview references (relative paths in the package root)
    if (!spec.iconSourcePath.isEmpty())
        root["icon"] = QStringLiteral("icon.png");

    if (!spec.previewSourcePaths.isEmpty()) {
        QJsonArray arr;
        for (int i = 0; i < spec.previewSourcePaths.size(); ++i) {
            const QString ext =
                QFileInfo(spec.previewSourcePaths[i]).suffix().toLower();
            arr.append(QString("preview_%1.%2").arg(i + 1).arg(ext));
        }
        root["previews"] = arr;
    }

    // payload_files: informational listing; installer scans files/ directly
    {
        QJsonArray arr;
        for (const QString& f : payloadFiles) arr.append(f);
        root["payload_files"] = arr;
    }

    const QString json =
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));

    gf::core::SafeWriteOptions opt;
    opt.make_backup = false;
    opt.max_bytes   = 64ull * 1024ull; // 64 KiB

    const auto res = gf::core::safe_write_text(
        std::filesystem::path(
            QDir(packageDir).filePath("astra_mod.json").toStdString()),
        json.toStdString(),
        opt);

    if (!res.ok) {
        if (outErr) *outErr = QString::fromStdString(res.message);
        return false;
    }
    return true;
}

// static
ModExportResult ModPackageExporter::exportPackage(const ModExportSpec&       spec,
                                                    const ProfileResolvedMap&  resolved,
                                                    const RuntimeTargetConfig& runtime,
                                                    const QString&             parentDir,
                                                    QWidget*                   progressParent) {
    ModExportResult result;

    // ── 1. Validate spec ──────────────────────────────────────────────────────
    if (spec.modId.isEmpty()) {
        result.errors << "Mod ID is required.";
    } else if (!isValidModId(spec.modId)) {
        result.errors << QString(
            "Mod ID '%1' is invalid. Use only letters, digits, underscores, and hyphens (3\u201364 chars).")
            .arg(spec.modId);
    }
    if (spec.name.isEmpty())
        result.errors << "Mod name is required.";
    if (spec.version.isEmpty())
        result.errors << "Version is required.";

    // ── 2. Validate resolved map ──────────────────────────────────────────────
    if (!resolved.isValid()) {
        result.errors << "Profile resolution failed: " +
                         (resolved.errors.isEmpty() ? QString("unknown error") : resolved.errors.first());
    } else if (resolved.files.isEmpty()) {
        result.errors << "Resolved profile contains no files to export.";
    } else {
        // Phase 5E: carry resolver warnings (e.g. unconfigured layer roots) into
        // the export result so the caller can surface them to the user.
        result.warnings << resolved.warnings;
    }

    // ── 3. Validate output parent directory ───────────────────────────────────
    if (parentDir.isEmpty() || !QDir(parentDir).exists())
        result.errors << QString("Output directory does not exist: %1").arg(parentDir);

    if (!result.errors.isEmpty()) {
        result.message = "Export aborted: " + result.errors.first();
        gf::core::logWarn(gf::core::LogCategory::General,
                          "ModPackageExporter: validation failed",
                          result.errors.first().toStdString());
        return result;
    }

    // ── 4. Validate all source files exist ────────────────────────────────────
    for (const ResolvedAstFile& entry : resolved.files) {
        if (!QFile::exists(entry.sourcePath)) {
            result.errors << QString("Source file missing: %1 → %2")
                               .arg(entry.filename, entry.sourcePath);
        }
    }
    if (!result.errors.isEmpty()) {
        result.message = QString("Export aborted: %1 source file(s) are missing.")
                             .arg(result.errors.size());
        return result;
    }

    // ── 4b. Pre-validate icon and preview source paths ────────────────────────
    if (!spec.iconSourcePath.isEmpty() && !QFile::exists(spec.iconSourcePath)) {
        result.errors << QString("Icon image not found: %1").arg(spec.iconSourcePath);
    }
    for (int i = 0; i < spec.previewSourcePaths.size(); ++i) {
        if (!QFile::exists(spec.previewSourcePaths[i])) {
            result.errors << QString("Preview image %1 not found: %2")
                               .arg(i + 1).arg(spec.previewSourcePaths[i]);
        }
    }
    if (!result.errors.isEmpty()) {
        result.message = "Export aborted: " + result.errors.first();
        gf::core::logWarn(gf::core::LogCategory::General,
                          "ModPackageExporter: asset pre-validation failed",
                          result.errors.first().toStdString());
        return result;
    }

    // ── 5. Create package directory ───────────────────────────────────────────
    const QString packageDir = QDir(parentDir).filePath(spec.modId);
    if (QDir(packageDir).exists()) {
        result.errors << QString(
            "Output folder already exists: %1\n"
            "Remove or rename it before exporting.").arg(packageDir);
        result.message = result.errors.first();
        return result;
    }
    if (!QDir().mkpath(packageDir)) {
        result.errors << QString("Cannot create package directory: %1").arg(packageDir);
        result.message = result.errors.first();
        return result;
    }

    // ── 6. Progress dialog ────────────────────────────────────────────────────
    QProgressDialog* progress = nullptr;
    if (progressParent) {
        progress = new QProgressDialog(
            QString("Exporting package \u201c%1\u201d\u2026").arg(spec.name),
            "Cancel", 0, resolved.files.size(), progressParent);
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumDuration(300);
        progress->show();
    }

    // ── 7. Copy files into files/<layer>/ ─────────────────────────────────────
    int         step        = 0;
    bool        cancelled   = false;
    QStringList payloadFiles;

    for (const ResolvedAstFile& entry : resolved.files) {
        if (progress) {
            if (progress->wasCanceled()) { cancelled = true; break; }
            progress->setValue(step);
            progress->setLabelText(
                QString("Exporting %1 (%2 / %3)\u2026")
                    .arg(entry.filename)
                    .arg(step + 1)
                    .arg(resolved.files.size()));
            qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        }

        const QString layer    = layerSubdir(entry.destRootPath, runtime);
        const QString destDir  = QDir(packageDir).filePath("files/" + layer);
        const QString destFile = QDir(destDir).filePath(entry.filename);

        // Warn if destRootPath non-empty but not matched to any content root
        if (!entry.destRootPath.isEmpty() && layer == QLatin1String("base")) {
            bool matched = false;
            for (const auto& cr : runtime.contentRoots)
                if (cr.path == entry.destRootPath) { matched = true; break; }
            if (!matched)
                result.warnings << QString(
                    "File '%1': destination root '%2' not found in runtime config — exporting to base/.")
                    .arg(entry.filename, entry.destRootPath);
        }

        if (!QDir().mkpath(destDir)) {
            if (progress) { progress->setValue(resolved.files.size()); delete progress; }
            result.errors << QString("Cannot create directory: %1").arg(destDir);
            result.message = "Export aborted: " + result.errors.last();
            gf::core::logError(gf::core::LogCategory::FileIO,
                               "ModPackageExporter: mkpath failed", destDir.toStdString());
            // Clean up partial output
            QDir(packageDir).removeRecursively();
            return result;
        }

        if (!QFile::copy(entry.sourcePath, destFile)) {
            if (progress) { progress->setValue(resolved.files.size()); delete progress; }
            result.errors << QString("Failed to copy '%1' to package").arg(entry.filename);
            result.message = "Export aborted: " + result.errors.last();
            gf::core::logError(gf::core::LogCategory::FileIO,
                               "ModPackageExporter: copy failed",
                               entry.filename.toStdString());
            QDir(packageDir).removeRecursively();
            return result;
        }

        payloadFiles << (layer + "/" + entry.filename);
        ++step;
    }

    if (progress) {
        progress->setValue(resolved.files.size());
        delete progress;
    }

    if (cancelled) {
        // Remove partial package on cancel
        QDir(packageDir).removeRecursively();
        result.message = "Export cancelled.";
        return result;
    }

    // ── 8. Copy icon and preview images (non-fatal on failure) ───────────────
    if (!spec.iconSourcePath.isEmpty()) {
        const QString iconDest = QDir(packageDir).filePath("icon.png");
        if (!QFile::copy(spec.iconSourcePath, iconDest)) {
            result.warnings << QString("Failed to copy icon image '%1' into package.")
                                    .arg(QFileInfo(spec.iconSourcePath).fileName());
        }
    }
    for (int i = 0; i < spec.previewSourcePaths.size(); ++i) {
        const QString& src = spec.previewSourcePaths[i];
        const QString   ext = QFileInfo(src).suffix().toLower();
        const QString   dest =
            QDir(packageDir).filePath(QString("preview_%1.%2").arg(i + 1).arg(ext));
        if (!QFile::copy(src, dest)) {
            result.warnings << QString("Failed to copy preview image %1 ('%2') into package.")
                                    .arg(i + 1)
                                    .arg(QFileInfo(src).fileName());
        }
    }

    // ── 9. Write astra_mod.json ───────────────────────────────────────────────
    payloadFiles.sort();
    QString manifestErr;
    if (!writeManifest(spec, payloadFiles, packageDir, &manifestErr)) {
        result.errors << "Failed to write astra_mod.json: " + manifestErr;
        result.message = result.errors.last();
        QDir(packageDir).removeRecursively();
        return result;
    }

    // ── 10. Final result ──────────────────────────────────────────────────────
    result.success    = true;
    result.outputPath = packageDir;
    result.fileCount  = step;
    result.message    = QString("Package \u201c%1\u201d exported: %2 file(s).")
                            .arg(spec.name).arg(step);

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModPackageExporter: export complete",
                      (spec.modId + " v" + spec.version + " \u2014 " +
                       QString::number(step) + " files \u2014 " +
                       packageDir).toStdString());
    return result;
}

} // namespace gf::gui
