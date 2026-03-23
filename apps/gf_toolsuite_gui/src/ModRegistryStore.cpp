#include "ModRegistryStore.hpp"

#include <gf/core/log.hpp>
#include <gf/core/safe_write.hpp>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <filesystem>

namespace gf::gui {

static constexpr int kRegistrySchemaVersion = 1;

// ── JSON helpers ──────────────────────────────────────────────────────────────

static InstalledModRecord recordFromJson(const QJsonObject& o) {
    InstalledModRecord r;
    r.installId    = o.value("install_id").toString();
    r.profileId    = o.value("profile_id").toString();
    r.gameId       = o.value("game_id").toString();
    r.modId        = o.value("mod_id").toString();
    r.modVersion   = o.value("mod_version").toString();
    r.modName      = o.value("mod_name").toString();
    r.sourcePath   = o.value("source_path").toString();
    r.manifestHash = o.value("manifest_hash").toString();
    r.enabled      = o.value("enabled").toBool(true);
    r.installedAt  = o.value("installed_at").toString();
    r.status       = o.value("status").toString("ok");
    for (const auto& v : o.value("installed_files").toArray())
        r.installedFiles << v.toString();
    for (const auto& v : o.value("warnings").toArray())
        r.warnings << v.toString();
    return r;
}

static QJsonObject recordToJson(const InstalledModRecord& r) {
    QJsonObject o;
    o["install_id"]    = r.installId;
    o["profile_id"]    = r.profileId;
    o["game_id"]       = r.gameId;
    o["mod_id"]        = r.modId;
    o["mod_version"]   = r.modVersion;
    o["mod_name"]      = r.modName;
    o["source_path"]   = r.sourcePath;
    o["manifest_hash"] = r.manifestHash;
    o["enabled"]       = r.enabled;
    o["installed_at"]  = r.installedAt;
    o["status"]        = r.status;

    QJsonArray files;
    for (const auto& f : r.installedFiles) files << f;
    o["installed_files"] = files;

    QJsonArray warns;
    for (const auto& w : r.warnings) warns << w;
    o["warnings"] = warns;

    return o;
}

// ── ModRegistryStore ──────────────────────────────────────────────────────────

// static
QString ModRegistryStore::registryPath(const QString& workspacePath) {
    return QDir(workspacePath).filePath("mods/installed_registry.json");
}

bool ModRegistryStore::load(const QString&               workspacePath,
                             QVector<InstalledModRecord>& outRecords,
                             QString*                     outErr) const {
    outRecords.clear();

    const QString path = registryPath(workspacePath);
    QFile f(path);
    if (!f.exists()) return true; // no mods installed yet — not an error

    if (!f.open(QIODevice::ReadOnly)) {
        if (outErr) *outErr = QString("Cannot open registry: %1").arg(path);
        gf::core::logError(gf::core::LogCategory::FileIO,
                           "ModRegistryStore: failed to open registry",
                           path.toStdString());
        return false;
    }

    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (doc.isNull()) {
        const QString msg =
            QString("Registry JSON parse error: %1").arg(pe.errorString());
        if (outErr) *outErr = msg;
        gf::core::logError(gf::core::LogCategory::FileIO,
                           "ModRegistryStore: registry parse error",
                           pe.errorString().toStdString());
        return false;
    }

    const QJsonObject root = doc.object();
    const int ver = root.value("schema_version").toInt(0);
    if (ver > kRegistrySchemaVersion) {
        gf::core::logWarn(gf::core::LogCategory::General,
                          "ModRegistryStore: registry schema version newer than supported",
                          std::to_string(ver));
    }

    for (const auto& v : root.value("records").toArray())
        outRecords << recordFromJson(v.toObject());

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModRegistryStore: registry loaded",
                      std::to_string(outRecords.size()) + " records");
    return true;
}

bool ModRegistryStore::save(const QString&                     workspacePath,
                             const QVector<InstalledModRecord>& records,
                             QString*                           outErr) const {
    // Ensure the mods/ directory exists
    const QString modsDir = QDir(workspacePath).filePath("mods");
    if (!QDir().mkpath(modsDir)) {
        if (outErr)
            *outErr = QString("Cannot create mods directory: %1").arg(modsDir);
        return false;
    }

    QJsonArray arr;
    for (const auto& r : records) arr << recordToJson(r);

    QJsonObject root;
    root["schema_version"] = kRegistrySchemaVersion;
    root["records"]        = arr;

    const QString json =
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));

    gf::core::SafeWriteOptions opt;
    opt.make_backup = true;
    opt.max_bytes   = 4ull * 1024ull * 1024ull; // 4 MiB

    const auto res = gf::core::safe_write_text(
        std::filesystem::path(registryPath(workspacePath).toStdString()),
        json.toStdString(),
        opt);

    if (!res.ok) {
        if (outErr) *outErr = QString::fromStdString(res.message);
        gf::core::logError(gf::core::LogCategory::FileIO,
                           "ModRegistryStore: failed to save registry",
                           res.message);
        return false;
    }

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModRegistryStore: registry saved",
                      std::to_string(records.size()) + " records");
    return true;
}

bool ModRegistryStore::appendRecord(const QString&            workspacePath,
                                     const InstalledModRecord& record,
                                     QString*                  outErr) const {
    QVector<InstalledModRecord> existing;
    QString loadErr;
    if (!load(workspacePath, existing, &loadErr)) {
        // Registry is corrupt/unreadable.  Start fresh rather than blocking the install.
        gf::core::logWarn(gf::core::LogCategory::General,
                          "ModRegistryStore: corrupt registry, starting fresh",
                          loadErr.toStdString());
    }
    existing.append(record);
    return save(workspacePath, existing, outErr);
}

} // namespace gf::gui
