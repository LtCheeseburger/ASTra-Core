#include "ProfileResolverService.hpp"
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

// Phase 5B: map a layer prefix in a mod relPath to the live dest root directory.
// "base/" and no prefix → "" (use runtime.astDirPath, legacy-compat).
// "update/" / "dlc/" / "custom_dlc/" → first matching RuntimeContentRoot.path.
static QString destRootFromLayerPrefix(const QString&             relPath,
                                        const RuntimeTargetConfig& runtime) {
    if (!relPath.contains('/'))
        return {}; // flat layout, no prefix → base root

    const QString prefix = relPath.section('/', 0, 0).toLower();
    if (prefix == QLatin1String("base"))
        return {}; // explicit base prefix → base root

    RuntimeContentKind target = RuntimeContentKind::BaseGame;
    if (prefix == QLatin1String("update"))
        target = RuntimeContentKind::Update;
    else if (prefix == QLatin1String("dlc"))
        target = RuntimeContentKind::Dlc;
    else if (prefix == QLatin1String("custom_dlc"))
        target = RuntimeContentKind::CustomDlc;
    else
        return {}; // unknown prefix → base root (safe fallback)

    for (const RuntimeContentRoot& cr : runtime.contentRoots)
        if (cr.kind == target) return cr.path;

    return {}; // layer not configured → base root fallback
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

    // Internal lookup tables (upper-case filename → index into result.files).
    // Used for conflict detection and for updating entries in-place when a mod
    // overrides a baseline file.
    QMap<QString, int>     keyToIdx;   // uppercase filename → result.files index
    QMap<QString, QString> providedBy; // uppercase filename → "baseline" | modName

    // ── 1. Seed from baseline AST directory ───────────────────────────────────
    // Base game files go to runtime.astDirPath (destRootPath empty = legacy compat).
    const QDir baselineDir(layout.overlayAstDir());
    if (baselineDir.exists()) {
        const QStringList baseFiles = baselineDir.entryList(
            {"qkl_*.AST", "qkl_*.ast"}, QDir::Files, QDir::Name);
        for (const QString& f : baseFiles) {
            const QString key = f.toUpper();
            // destRootPath left empty → ProfileApplyService uses runtime.astDirPath (legacy compat)
            result.files.append({f, baselineDir.filePath(f), QStringLiteral("baseline"), {}});
            keyToIdx.insert(key, result.files.size() - 1);
            providedBy.insert(key, QStringLiteral("baseline"));
        }
    }
    // An empty baseline is valid — the profile may consist of mods only.

    // ── 1b. Phase 5A: seed additional roots from roots_manifest.json ──────────
    // When captureFromRuntime() captured multiple roots, each root's files are
    // stored in overlay/roots/<index>/ and its live destination is in the manifest.
    // Higher-priority roots (update > base) are listed later in the manifest and
    // overwrite earlier entries, preserving their destRootPath.
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

                const QStringList rootFiles = rootDir.entryList(
                    {"qkl_*.AST", "qkl_*.ast"}, QDir::Files, QDir::Name);
                for (const QString& f : rootFiles) {
                    const QString key = f.toUpper();
                    if (keyToIdx.contains(key)) {
                        // Overwrite source and destination from this higher-priority root.
                        result.files[keyToIdx.value(key)].sourcePath   = rootDir.filePath(f);
                        result.files[keyToIdx.value(key)].destRootPath = live;
                        // providedBy stays "baseline" (still original content, just different root)
                    } else {
                        result.files.append({f, rootDir.filePath(f), QStringLiteral("baseline"), live});
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
        // Non-fatal: missing registry means no mods installed yet.
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

        // The install slot layout: <workspace>/mods/installed/<modId>-<installId>/files/
        const QString slotFilesDir =
            QDir(layout.modsInstalledDir()).filePath(
                rec.modId + "-" + rec.installId + "/files");

        for (const QString& relPath : rec.installedFiles) {
            const QString fname      = QFileInfo(relPath).fileName();
            const QString fnameUpper = fname.toUpper();

            // Only process AST files — other payload types are not resolved here.
            if (!fnameUpper.endsWith(".AST")) continue;

            const QString absPath = QDir(slotFilesDir).filePath(relPath);
            const QString prevPro = providedBy.value(fnameUpper);

            // Phase 5B: if the relPath has a known layer prefix (base/, update/, dlc/,
            // custom_dlc/), resolve it to the matching live root directory.
            const QString modDestRoot = destRootFromLayerPrefix(relPath, runtime);

            // Phase 5E: warn once per (mod, layer) when a non-base layer is requested
            // but no matching runtime content root is configured.
            if (modDestRoot.isEmpty() && relPath.contains('/')) {
                const QString prefix = relPath.section('/', 0, 0).toLower();
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
                // File not in baseline and not yet provided by any mod — add it.
                result.files.append({fname, absPath, rec.modName, modDestRoot});
                keyToIdx.insert(fnameUpper, result.files.size() - 1);
                providedBy.insert(fnameUpper, rec.modName);

            } else if (prevPro == QLatin1String("baseline")) {
                // Mod overrides a baseline entry — update source in-place.
                result.files[keyToIdx.value(fnameUpper)].sourcePath = absPath;
                result.files[keyToIdx.value(fnameUpper)].providedBy  = rec.modName;
                // If the mod carries an explicit layer prefix, respect it (it may differ
                // from the seeded baseline destRootPath — e.g. a base-game file being
                // redirected to an update root by a layered package).
                // If no layer prefix (flat or "base/"), preserve the baseline's destRootPath.
                if (!modDestRoot.isEmpty())
                    result.files[keyToIdx.value(fnameUpper)].destRootPath = modDestRoot;
                providedBy.insert(fnameUpper, rec.modName);

            } else {
                // Two enabled mods both supply the same filename — hard conflict.
                result.errors << QString(
                    "Conflict: \"%1\" is provided by both \"%2\" and \"%3\". "
                    "Disable one of these mods before applying the profile.")
                    .arg(fname, prevPro, rec.modName);
            }
        }
    }

    // If any conflicts were detected, clear files so callers cannot accidentally
    // proceed with a partial map.
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
