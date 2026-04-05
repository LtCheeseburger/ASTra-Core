#include "ProfileResolverService.hpp"
#include "ContentRootDiscovery.hpp"
#include "ModRegistryStore.hpp"
#include "ModWorkspaceManager.hpp"

#include <gf/core/log.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace gf::gui {

// Phase 6A: Decompose a mod's installed relPath (e.g. "update/DLC/CFBR/qkl_dlc.AST")
// into the live dest root path and the relative path within that root.
//
// "base/<rest>"       → destRoot = ""              relPath = <rest>
// "update/<rest>"     → destRoot = update root     relPath = <rest>
// "dlc/<rest>"        → destRoot = dlc root        relPath = <rest>
// "custom_dlc/<rest>" → destRoot = custom_dlc root relPath = <rest>
// "<flat>"            → destRoot = ""              relPath = <flat>   (no prefix)
// unknown prefix      → destRoot = ""              relPath = original (safe fallback)
struct LayerDecomposition {
    QString destRoot; // empty = use runtime.astDirPath
    QString relPath;  // relative path within the dest root (may contain subdirs)
};

static LayerDecomposition layerDecompose(const QString&             installedRelPath,
                                          const RuntimeTargetConfig& runtime)
{
    if (!installedRelPath.contains(QLatin1Char('/'))) {
        // Flat filename — no layer prefix, route to base root
        return {QString(), installedRelPath};
    }

    const QString prefix  = installedRelPath.section(QLatin1Char('/'), 0, 0).toLower();
    const QString theRest = installedRelPath.section(QLatin1Char('/'), 1); // strip prefix

    if (prefix == QLatin1String("base"))
        return {QString(), theRest};

    RuntimeContentKind target = RuntimeContentKind::BaseGame;
    if (prefix == QLatin1String("update"))
        target = RuntimeContentKind::Update;
    else if (prefix == QLatin1String("dlc"))
        target = RuntimeContentKind::Dlc;
    else if (prefix == QLatin1String("custom_dlc"))
        target = RuntimeContentKind::CustomDlc;
    else
        return {QString(), installedRelPath}; // unknown prefix → base (safe)

    for (const RuntimeContentRoot& cr : runtime.contentRoots)
        if (cr.kind == target) return {cr.path, theRest};

    return {QString(), installedRelPath}; // unconfigured layer → base fallback
}

// static
ProfileResolvedMap ProfileResolverService::resolve(const ModProfile&          profile,
                                                     const RuntimeTargetConfig& runtime,
                                                     QString*                   outErr) {
    ProfileResolvedMap result;

    if (!profile.isValid()) {
        const QString e = "ProfileResolverService: invalid profile (id/gameId/name/workspacePath empty)";
        result.errors << e;
        if (outErr) *outErr = e;
        return result;
    }

    const WorkspaceLayout layout = WorkspaceLayout::from(profile.workspacePath);

    // Conflict-detection maps.
    // Key = (destRoot + "|" + relPath).toUpper() — distinguishes same filename
    // in different roots while still allowing one mod to override the baseline.
    QMap<QString, int>     keyToIdx;   // composite key → result.files index
    QMap<QString, QString> providedBy; // composite key → "baseline" | modName

    auto makeKey = [](const QString& destRoot, const QString& relPath) -> QString {
        // Use empty-or-path as root distinguisher
        return (destRoot.isEmpty() ? QStringLiteral("__base__") : destRoot)
               + QLatin1Char('|')
               + relPath.toUpper();
    };

    // ── 1. Seed from base overlay (overlay/ast/) ───────────────────────────────
    // Phase 6A: scan *.ast / *.AST — no qkl_* prefix restriction
    const QDir baselineDir(layout.overlayAstDir());
    if (baselineDir.exists()) {
        const QStringList baseFiles = discoverBaseAstFiles(baselineDir);
        for (const QString& f : baseFiles) {
            const QString key = makeKey(QString(), f);
            result.files.append({f, baselineDir.filePath(f), QStringLiteral("baseline"), {}});
            keyToIdx.insert(key, result.files.size() - 1);
            providedBy.insert(key, QStringLiteral("baseline"));
        }
    }

    // ── 1b. Seed additional roots from roots_manifest.json ────────────────────
    // Phase 6A: recursively scan each root overlay directory so that files
    // captured from subfolders (DLC/...) are included with their relative paths.
    const QString manifestPath = layout.rootsManifestPath();
    if (QFile::exists(manifestPath)) {
        QFile mf(manifestPath);
        if (mf.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(mf.readAll());
            const QJsonArray roots = doc.object().value("roots").toArray();
            for (const auto& v : roots) {
                const QJsonObject rm   = v.toObject();
                const int         idx  = rm.value("index").toInt(-1);
                const QString     live = rm.value("live_path").toString();
                if (idx < 0 || live.isEmpty()) continue;

                const QDir rootDir(layout.overlayRootDir(idx));
                if (!rootDir.exists()) continue;

                // Phase 6A: recursive discovery preserves nested relPaths
                QVector<QPair<QString,QString>> rootFiles;
                discoverAstFilesRecursive(rootDir, QString(), rootFiles);

                for (const auto& [relPath, absPath] : rootFiles) {
                    const QString key = makeKey(live, relPath);
                    if (keyToIdx.contains(key)) {
                        // Update root overrides base root for the same relPath
                        result.files[keyToIdx.value(key)].sourcePath   = absPath;
                        result.files[keyToIdx.value(key)].destRootPath = live;
                    } else {
                        result.files.append({relPath, absPath, QStringLiteral("baseline"), live});
                        keyToIdx.insert(key, result.files.size() - 1);
                        providedBy.insert(key, QStringLiteral("baseline"));
                    }
                }
            }
        } else {
            gf::core::logWarn(gf::core::LogCategory::General,
                              "ProfileResolverService: cannot open roots_manifest.json",
                              manifestPath.toStdString());
        }
    }

    // ── 2. Load installed mod registry ────────────────────────────────────────
    ModRegistryStore           store;
    QVector<InstalledModRecord> records;
    QString regErr;
    if (!store.load(profile.workspacePath, records, &regErr)) {
        gf::core::logWarn(gf::core::LogCategory::General,
                          "ProfileResolverService: registry load failed (treating as empty)",
                          regErr.toStdString());
    }

    // ── 3. Overlay enabled mod files ──────────────────────────────────────────
    // Tracks (modName|layerPrefix) pairs we've already warned about to avoid
    // repeating the same warning for every file in the same unconfigured layer.
    QSet<QString> warnedLayerRoots;

    for (const InstalledModRecord& rec : records) {
        if (!rec.enabled) continue;

        const QString slotFilesDir =
            QDir(layout.modsInstalledDir()).filePath(
                rec.modId + "-" + rec.installId + "/files");

        for (const QString& installedRelPath : rec.installedFiles) {
            // Phase 6A: decompose into dest root + relPath within that root
            const LayerDecomposition decomp = layerDecompose(installedRelPath, runtime);
            const QString& modRelPath = decomp.relPath;

            // Phase 6A: filter by extension on the final filename component
            const QString plainName = QFileInfo(modRelPath).fileName().toUpper();
            if (!plainName.endsWith(QStringLiteral(".AST")) &&
                !plainName.endsWith(QStringLiteral(".AST.EDAT")))
                continue;

            const QString absPath = QDir(slotFilesDir).filePath(installedRelPath);
            const QString key     = makeKey(decomp.destRoot, modRelPath);
            const QString prevPro = providedBy.value(key);

            // Phase 5E/6A: warn once per (mod, layer) when a non-base layer is
            // requested but no matching runtime content root is configured.
            if (decomp.destRoot.isEmpty() && installedRelPath.contains(QLatin1Char('/'))) {
                const QString prefix = installedRelPath.section(QLatin1Char('/'), 0, 0).toLower();
                if (prefix == QLatin1String("update") ||
                    prefix == QLatin1String("dlc")    ||
                    prefix == QLatin1String("custom_dlc")) {
                    const QString warnKey = rec.modName + QLatin1Char('|') + prefix;
                    if (!warnedLayerRoots.contains(warnKey)) {
                        warnedLayerRoots.insert(warnKey);
                        result.warnings << QString(
                            "Mod \"%1\": files in layer \"%2/\" will fall back to the base "
                            "root because no matching runtime content root is configured.")
                            .arg(rec.modName, prefix);
                    }
                }
            }

            if (prevPro.isEmpty()) {
                // New file not in baseline — add it
                result.files.append({modRelPath, absPath, rec.modName, decomp.destRoot});
                keyToIdx.insert(key, result.files.size() - 1);
                providedBy.insert(key, rec.modName);

            } else if (prevPro == QLatin1String("baseline")) {
                // Mod overrides baseline — update source
                result.files[keyToIdx.value(key)].sourcePath = absPath;
                result.files[keyToIdx.value(key)].providedBy  = rec.modName;
                if (!decomp.destRoot.isEmpty())
                    result.files[keyToIdx.value(key)].destRootPath = decomp.destRoot;
                providedBy.insert(key, rec.modName);

            } else {
                // Two enabled mods provide the same file — hard conflict
                result.errors << QString(
                    "Conflict: \"%1\" is provided by both \"%2\" and \"%3\". "
                    "Disable one of these mods before applying the profile.")
                    .arg(modRelPath, prevPro, rec.modName);
            }
        }
    }

    if (!result.errors.isEmpty()) {
        result.files.clear();
        if (outErr) *outErr = result.errors.first();
        gf::core::logWarn(
            gf::core::LogCategory::General,
            "ProfileResolverService: resolution failed with conflicts",
            QString::number(result.errors.size()).toStdString());
        return result;
    }

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ProfileResolverService: resolved",
                      (profile.name + " \u2014 " +
                       QString::number(result.files.size()) + " files").toStdString());
    return result;
}

} // namespace gf::gui
