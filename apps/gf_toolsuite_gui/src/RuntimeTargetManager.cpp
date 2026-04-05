#include "RuntimeTargetManager.hpp"
#include "NameCache.hpp"

#include <gf/core/log.hpp>
#include <gf/core/safe_write.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <filesystem>

namespace gf::gui {

static constexpr int kSchemaVersion = 1;

// ── private path helpers ──────────────────────────────────────────────────────

// static
QString RuntimeTargetManager::configDir() {
    return QDir(NameCache::appDataDir()).filePath("runtime_configs");
}

// static
QString RuntimeTargetManager::configPath(const QString& gameId) {
    return QDir(configDir()).filePath(gameId + ".json");
}

// ── public API ────────────────────────────────────────────────────────────────

// static
bool RuntimeTargetManager::hasConfig(const QString& gameId) {
    if (gameId.isEmpty()) return false;
    return QFile::exists(configPath(gameId));
}

// static
std::optional<RuntimeTargetConfig> RuntimeTargetManager::load(const QString& gameId,
                                                               QString*       outErr) {
    auto fail = [&](const QString& msg) -> std::optional<RuntimeTargetConfig> {
        if (outErr) *outErr = msg;
        return std::nullopt;
    };

    if (gameId.isEmpty())
        return fail("gameId is empty.");

    const QString path = configPath(gameId);
    QFile f(path);
    if (!f.exists())
        return fail(QString("No runtime config found for game: %1").arg(gameId));
    if (!f.open(QIODevice::ReadOnly))
        return fail(QString("Cannot open runtime config: %1").arg(f.errorString()));

    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (doc.isNull())
        return fail(QString("JSON parse error in runtime config: %1").arg(pe.errorString()));

    const QJsonObject root = doc.object();
    const int ver = root.value("schema_version").toInt(0);
    if (ver > kSchemaVersion) {
        gf::core::logWarn(gf::core::LogCategory::General,
                          "RuntimeTargetManager: config schema version newer than supported",
                          std::to_string(ver));
    }

    RuntimeTargetConfig cfg;
    cfg.gameId       = root.value("game_id").toString();
    cfg.platform     = RuntimePlatform::RPCS3; // only supported value
    cfg.rpcs3ExePath = root.value("rpcs3_exe_path").toString();
    cfg.astDirPath   = root.value("ast_dir_path").toString();
    cfg.configuredAt = root.value("configured_at").toString();

    if (cfg.gameId.isEmpty() || cfg.rpcs3ExePath.isEmpty() || cfg.astDirPath.isEmpty())
        return fail("Runtime config is incomplete.");

    // Phase 5A: deserialize optional content_roots array (absent = legacy single-root)
    const QJsonArray rootsArr = root.value("content_roots").toArray();
    for (const auto& v : rootsArr) {
        const QJsonObject ro = v.toObject();
        RuntimeContentRoot cr;
        cr.kind        = runtimeContentKindFromString(ro.value("kind").toString());
        cr.path        = ro.value("path").toString();
        cr.displayName = ro.value("display_name").toString();
        if (!cr.path.isEmpty())
            cfg.contentRoots.append(cr);
    }

    return cfg;
}

// static
bool RuntimeTargetManager::save(const RuntimeTargetConfig& config, QString* outErr) {
    auto fail = [&](const QString& msg) {
        if (outErr) *outErr = msg;
        gf::core::logError(gf::core::LogCategory::FileIO,
                           "RuntimeTargetManager::save: " + msg.toStdString());
        return false;
    };

    if (config.gameId.isEmpty())
        return fail("Cannot save runtime config: gameId is empty.");

    const QString dir = configDir();
    if (!QDir().mkpath(dir))
        return fail(QString("Cannot create runtime_configs directory: %1").arg(dir));

    // Phase 5A: serialize content_roots (only written when non-empty for backward compat)
    QJsonArray rootsArr;
    for (const RuntimeContentRoot& cr : config.contentRoots) {
        QJsonObject ro;
        ro["kind"]         = runtimeContentKindString(cr.kind);
        ro["path"]         = cr.path;
        ro["display_name"] = cr.displayName;
        rootsArr.append(ro);
    }

    QJsonObject root;
    root["schema_version"]  = kSchemaVersion;
    root["game_id"]         = config.gameId;
    root["platform"]        = static_cast<int>(config.platform);
    root["rpcs3_exe_path"]  = config.rpcs3ExePath;
    root["ast_dir_path"]    = config.astDirPath;
    root["configured_at"]   = config.configuredAt;
    if (!rootsArr.isEmpty())
        root["content_roots"] = rootsArr;

    const QString json =
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));

    gf::core::SafeWriteOptions opt;
    opt.make_backup = false; // not critical — user can reconfigure
    opt.max_bytes   = 16ull * 1024ull; // 16 KiB sanity cap

    const auto res = gf::core::safe_write_text(
        std::filesystem::path(configPath(config.gameId).toStdString()),
        json.toStdString(),
        opt);

    if (!res.ok)
        return fail(QString::fromStdString(res.message));

    gf::core::logInfo(gf::core::LogCategory::General,
                      "RuntimeTargetManager: config saved",
                      config.gameId.toStdString());
    return true;
}

// static
bool RuntimeTargetManager::validate(const RuntimeTargetConfig& config,
                                     QStringList*               outErrors) {
    QStringList errors;

    // rpcs3 executable
    if (config.rpcs3ExePath.isEmpty()) {
        errors << "RPCS3 executable path is empty.";
    } else {
        const QFileInfo fi(config.rpcs3ExePath);
        if (!fi.exists())
            errors << QString("RPCS3 executable not found: %1").arg(config.rpcs3ExePath);
        else if (fi.isDir())
            errors << QString("RPCS3 path points to a directory, not an executable: %1")
                       .arg(config.rpcs3ExePath);
    }

    // Phase 6A: base content root (flat *.ast / *.AST files — no qkl_ prefix required)
    if (config.astDirPath.isEmpty()) {
        errors << "Base content root path is empty.";
    } else {
        const QDir baseDir(config.astDirPath);
        if (!baseDir.exists()) {
            errors << QString("Base content root does not exist: %1").arg(config.astDirPath);
        } else {
            const QStringList astFiles = baseDir.entryList(
                {"*.ast", "*.AST"}, QDir::Files);
            if (astFiles.size() < kMinAstFiles) {
                errors << QString(
                    "Base content root contains only %1 *.ast file(s) "
                    "(expected at least %2). Is this the correct directory?")
                    .arg(astFiles.size()).arg(kMinAstFiles);
            }
        }
    }

    // Phase 6A: validate optional content roots (update root only in new configs).
    // Old configs may still have Dlc/CustomDlc entries — validate existence but
    // do not enforce uniqueness or presence, since new configs only use Update.
    for (const RuntimeContentRoot& cr : config.contentRoots) {
        const QString label = cr.displayName.isEmpty()
            ? runtimeContentKindLabel(cr.kind)
            : cr.displayName;
        if (cr.path.isEmpty()) {
            errors << QString("Content root \"%1\" has an empty path.").arg(label);
            continue;
        }
        if (!QDir(cr.path).exists()) {
            errors << QString("Content root \"%1\" path does not exist: %2")
                       .arg(label, cr.path);
        }
    }

    if (outErrors) *outErrors = errors;
    return errors.isEmpty();
}

} // namespace gf::gui
