#include "BaselineCaptureService.hpp"
#include "ModProfileManager.hpp"
#include "ModWorkspaceManager.hpp"

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

#include <filesystem>

namespace gf::gui {

// static
BaselineCaptureResult BaselineCaptureService::capture(const QString&     gameId,
                                                       const QString&     astDirPath,
                                                       const QString&     profileName,
                                                       BaselineType       type,
                                                       ModProfileManager& mgr,
                                                       QWidget*           progressParent,
                                                       const QString&     gameDisplayName) {
    BaselineCaptureResult result;

    if (gameId.isEmpty() || astDirPath.isEmpty() || profileName.isEmpty()) {
        result.message = "capture(): gameId, astDirPath, and profileName must not be empty.";
        gf::core::logWarn(gf::core::LogCategory::General,
                          "BaselineCaptureService: " + result.message.toStdString());
        return result;
    }

    // ── 1. Discover qkl_*.AST files in the live AST directory ─────────────────
    const QDir astDir(astDirPath);
    if (!astDir.exists()) {
        result.message = QString("AST directory does not exist: %1").arg(astDirPath);
        return result;
    }

    const QStringList astFiles = astDir.entryList(
        {"qkl_*.AST", "qkl_*.ast"}, QDir::Files, QDir::Name);

    if (astFiles.isEmpty()) {
        result.message = QString("No qkl_*.AST files found in: %1").arg(astDirPath);
        return result;
    }

    // ── 2. Create a new profile ────────────────────────────────────────────────
    ModProfile newProfile;
    QString createErr;
    if (!mgr.createProfile(gameId, profileName,
                           QString("Baseline capture from: %1").arg(astDirPath),
                           &newProfile, &createErr,
                           gameDisplayName)) {
        result.message = "Failed to create baseline profile: " + createErr;
        return result;
    }

    result.profileId = newProfile.id;

    // ── 3. Copy AST files into <workspace>/overlay/ast/ ───────────────────────
    const WorkspaceLayout layout = WorkspaceLayout::from(newProfile.workspacePath);
    const QString destDir = layout.overlayAstDir();

    if (!QDir().mkpath(destDir)) {
        result.message =
            QString("Cannot create overlay/ast directory: %1").arg(destDir);
        // Profile was created — return its ID so the caller knows what happened
        result.warnings << "Profile created but overlay/ast/ directory could not be created.";
        return result;
    }

    // Progress dialog (optional)
    QProgressDialog* progress = nullptr;
    if (progressParent) {
        progress = new QProgressDialog(
            QString("Capturing baseline: %1…").arg(profileName),
            "Cancel", 0, astFiles.size(), progressParent);
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumDuration(500); // only show if it takes > 500 ms
        progress->show();
    }

    int copied   = 0;
    int failed   = 0;
    bool cancelled = false;

    for (int i = 0; i < astFiles.size(); ++i) {
        if (progress) {
            if (progress->wasCanceled()) { cancelled = true; break; }
            progress->setValue(i);
            progress->setLabelText(
                QString("Copying %1 (%2 / %3)…")
                    .arg(astFiles[i]).arg(i + 1).arg(astFiles.size()));
            // Let the event loop process the progress update
            qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        }

        const QString srcPath  = astDir.filePath(astFiles[i]);
        const QString dstPath  = QDir(destDir).filePath(astFiles[i]);

        // Never overwrite — this is a fresh workspace, so this shouldn't happen,
        // but guard just in case.
        if (QFile::exists(dstPath)) {
            result.warnings << QString("Skipped (already exists): %1").arg(astFiles[i]);
            continue;
        }

        if (!QFile::copy(srcPath, dstPath)) {
            ++failed;
            result.warnings << QString("Failed to copy: %1").arg(astFiles[i]);
            gf::core::logWarn(gf::core::LogCategory::FileIO,
                              "BaselineCaptureService: copy failed",
                              astFiles[i].toStdString());
        } else {
            ++copied;
        }
    }

    if (progress) {
        progress->setValue(astFiles.size());
        delete progress;
    }

    result.filesCopied = copied;

    if (cancelled) {
        result.warnings << QString("Capture cancelled after %1 file(s). "
                                   "Profile workspace is partial.").arg(copied);
    }

    // ── 4. Mark the profile as baseline ───────────────────────────────────────
    QString markErr;
    if (!mgr.markAsBaseline(newProfile.id, true, type, &markErr)) {
        result.warnings << "Could not mark profile as baseline: " + markErr;
        // Non-fatal — the profile and files exist, just the flag is missing
    }

    // ── 5. Set as active profile ───────────────────────────────────────────────
    QString activeErr;
    if (!mgr.setActiveProfile(gameId, newProfile.id, &activeErr)) {
        result.warnings << "Could not set baseline as active profile: " + activeErr;
    }

    result.success = (copied > 0 || astFiles.isEmpty());
    result.message = result.success
        ? QString("Baseline '%1' captured: %2 file(s) copied.")
              .arg(profileName).arg(copied)
        : QString("Baseline capture failed: no files were copied.");

    if (failed > 0) {
        result.message += QString(" %1 file(s) could not be copied.").arg(failed);
    }

    gf::core::logInfo(gf::core::LogCategory::General,
                      "BaselineCaptureService: capture complete",
                      (profileName + " — " + QString::number(copied) + " files").toStdString());
    return result;
}

// ── Phase 5A: multi-root capture ──────────────────────────────────────────────

// Helper: copies all qkl_*.AST files from srcDir into destDir.
// Returns number copied; appends failure messages to warnings.
static int copyAstDirectory(const QDir&      srcDir,
                             const QString&   destDir,
                             QStringList&     warnings,
                             QProgressDialog* progress,
                             int              progressOffset,
                             int              /*progressTotal*/,
                             const QString&   label) {
    if (!srcDir.exists()) {
        warnings << QString("Source directory does not exist: %1").arg(srcDir.path());
        return 0;
    }

    if (!QDir().mkpath(destDir)) {
        warnings << QString("Cannot create capture directory: %1").arg(destDir);
        return 0;
    }

    const QStringList files = srcDir.entryList(
        {"qkl_*.AST", "qkl_*.ast"}, QDir::Files, QDir::Name);

    int copied = 0;
    for (int i = 0; i < files.size(); ++i) {
        if (progress) {
            if (progress->wasCanceled()) break;
            progress->setValue(progressOffset + i);
            progress->setLabelText(
                QString("Copying %1 (%2): %3…")
                    .arg(label).arg(i + 1).arg(files[i]));
            qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        }

        const QString src = srcDir.filePath(files[i]);
        const QString dst = QDir(destDir).filePath(files[i]);

        if (QFile::exists(dst)) continue; // fresh workspace, skip silently

        if (!QFile::copy(src, dst)) {
            warnings << QString("Failed to copy \"%1\" from %2").arg(files[i], srcDir.path());
            gf::core::logWarn(gf::core::LogCategory::FileIO,
                              "BaselineCaptureService: copy failed",
                              files[i].toStdString());
        } else {
            ++copied;
        }
    }

    if (progress && !progress->wasCanceled())
        progress->setValue(progressOffset + files.size());

    return copied;
}

// static
BaselineCaptureResult BaselineCaptureService::captureFromRuntime(
    const QString&             gameId,
    const RuntimeTargetConfig& runtime,
    const QString&             profileName,
    BaselineType               type,
    ModProfileManager&         mgr,
    QWidget*                   progressParent,
    const QString&             gameDisplayName)
{
    // Single-root: delegate to original capture().
    if (runtime.contentRoots.isEmpty()) {
        return capture(gameId, runtime.astDirPath, profileName, type, mgr,
                       progressParent, gameDisplayName);
    }

    // ── Multi-root path ───────────────────────────────────────────────────────
    BaselineCaptureResult result;

    if (gameId.isEmpty() || runtime.astDirPath.isEmpty() || profileName.isEmpty()) {
        result.message = "captureFromRuntime(): gameId, astDirPath, and profileName must not be empty.";
        return result;
    }

    // ── Pre-flight: verify base root before creating a profile ────────────────
    // Fail clearly now so we don't leave an empty orphan profile in the index.
    const QDir baseDir(runtime.astDirPath);
    if (!baseDir.exists()) {
        result.message = QString(
            "Base AST directory does not exist: %1").arg(runtime.astDirPath);
        gf::core::logWarn(gf::core::LogCategory::General,
                          "BaselineCaptureService: base root missing",
                          runtime.astDirPath.toStdString());
        return result;
    }
    const QStringList baseFiles =
        baseDir.entryList({"qkl_*.AST", "qkl_*.ast"}, QDir::Files);
    if (baseFiles.isEmpty()) {
        result.message = QString(
            "No qkl_*.AST files found in base AST directory: %1").arg(runtime.astDirPath);
        gf::core::logWarn(gf::core::LogCategory::General,
                          "BaselineCaptureService: base root empty",
                          runtime.astDirPath.toStdString());
        return result;
    }

    // Pre-check optional roots and warn now (before progress dialog opens).
    for (const RuntimeContentRoot& cr : runtime.contentRoots) {
        const QDir rd(cr.path);
        const QString label = cr.displayName.isEmpty()
            ? runtimeContentKindLabel(cr.kind) : cr.displayName;
        if (!rd.exists()) {
            // Will also warn during copy — just note it here for the preflight log.
            gf::core::logWarn(gf::core::LogCategory::General,
                              "BaselineCaptureService: optional root does not exist",
                              cr.path.toStdString());
        } else if (rd.entryList({"qkl_*.AST", "qkl_*.ast"}, QDir::Files).isEmpty()) {
            result.warnings << QString(
                "Content root \"%1\" is configured but contains no qkl_*.AST files: %2")
                .arg(label, cr.path);
        }
    }

    // Count total files across all roots for the progress dialog range.
    int totalFiles = static_cast<int>(baseFiles.size());
    for (const RuntimeContentRoot& cr : runtime.contentRoots) {
        const QDir rd(cr.path);
        if (rd.exists())
            totalFiles += rd.entryList({"qkl_*.AST", "qkl_*.ast"}, QDir::Files).size();
    }

    // ── Create profile ────────────────────────────────────────────────────────
    ModProfile newProfile;
    QString createErr;
    if (!mgr.createProfile(gameId, profileName,
                           QString("Multi-root baseline capture from: %1").arg(runtime.astDirPath),
                           &newProfile, &createErr,
                           gameDisplayName)) {
        result.message = "Failed to create baseline profile: " + createErr;
        return result;
    }
    result.profileId = newProfile.id;

    const WorkspaceLayout layout = WorkspaceLayout::from(newProfile.workspacePath);

    // ── Progress dialog ───────────────────────────────────────────────────────
    QProgressDialog* progress = nullptr;
    if (progressParent) {
        progress = new QProgressDialog(
            QString("Capturing baseline: %1…").arg(profileName),
            "Cancel", 0, totalFiles, progressParent);
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumDuration(500);
        progress->show();
    }

    int totalCopied = 0;
    int offset      = 0;

    // ── Copy base game → overlay/ast/ ─────────────────────────────────────────
    // baseFiles was already computed in the pre-flight check above.
    totalCopied += copyAstDirectory(
        baseDir,
        layout.overlayAstDir(),
        result.warnings,
        progress, offset,
        static_cast<int>(baseFiles.size()),
        "Base Game");
    offset += static_cast<int>(baseFiles.size());

    // ── Copy each content root → overlay/roots/<idx>/ ─────────────────────────
    QJsonArray manifestRoots;
    for (int idx = 0; idx < runtime.contentRoots.size(); ++idx) {
        const RuntimeContentRoot& cr = runtime.contentRoots[idx];
        if (progress && progress->wasCanceled()) break;

        const QDir crDir(cr.path);
        const int crFileCount = crDir.exists()
            ? crDir.entryList({"qkl_*.AST", "qkl_*.ast"}, QDir::Files).size()
            : 0;

        const QString label = cr.displayName.isEmpty()
            ? runtimeContentKindLabel(cr.kind)
            : cr.displayName;

        totalCopied += copyAstDirectory(
            crDir,
            layout.overlayRootDir(idx),
            result.warnings,
            progress, offset, crFileCount,
            label);
        offset += crFileCount;

        // Build manifest entry for this root.
        QJsonObject rm;
        rm["index"]     = idx;
        rm["kind"]      = runtimeContentKindString(cr.kind);
        rm["live_path"] = cr.path;
        rm["label"]     = label;
        manifestRoots.append(rm);
    }

    if (progress) {
        progress->setValue(totalFiles);
        delete progress;
    }

    // ── Write roots_manifest.json ─────────────────────────────────────────────
    if (!manifestRoots.isEmpty()) {
        const QString rootsDir = layout.overlayRootsDir();
        if (QDir().mkpath(rootsDir)) {
            QJsonObject doc;
            doc["schema_version"] = 1;
            doc["roots"]          = manifestRoots;
            const QString json =
                QString::fromUtf8(QJsonDocument(doc).toJson(QJsonDocument::Indented));

            gf::core::SafeWriteOptions opt;
            opt.make_backup = false;
            opt.max_bytes   = 64ull * 1024ull; // 64 KiB — manifest is tiny
            const auto res = gf::core::safe_write_text(
                std::filesystem::path(layout.rootsManifestPath().toStdString()),
                json.toStdString(),
                opt);
            if (!res.ok) {
                result.warnings << "Could not write roots_manifest.json: " +
                                   QString::fromStdString(res.message);
            }
        } else {
            result.warnings << "Could not create overlay/roots/ directory.";
        }
    }

    // ── Mark and set active ───────────────────────────────────────────────────
    QString markErr;
    if (!mgr.markAsBaseline(newProfile.id, true, type, &markErr))
        result.warnings << "Could not mark profile as baseline: " + markErr;

    QString activeErr;
    if (!mgr.setActiveProfile(gameId, newProfile.id, &activeErr))
        result.warnings << "Could not set baseline as active profile: " + activeErr;

    result.filesCopied = totalCopied;
    result.success     = (totalCopied > 0);
    result.message     = result.success
        ? QString("Multi-root baseline '%1' captured: %2 file(s) copied.")
              .arg(profileName).arg(totalCopied)
        : QString("Multi-root baseline capture failed: no files were copied.");

    gf::core::logInfo(gf::core::LogCategory::General,
                      "BaselineCaptureService: multi-root capture complete",
                      (profileName + " \u2014 " +
                       QString::number(totalCopied) + " files, " +
                       QString::number(runtime.contentRoots.size()) + " extra roots").toStdString());
    return result;
}

} // namespace gf::gui
