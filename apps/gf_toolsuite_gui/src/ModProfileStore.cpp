#include "ModProfileStore.hpp"
#include "NameCache.hpp"

#include "gf/core/log.hpp"
#include "gf/core/safe_write.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <filesystem>

namespace gf::gui {

static constexpr int kIndexSchemaVersion   = 1;
static constexpr int kProfileSchemaVersion = 1;

// Base directory under the shared ASTra appdata folder
static QString modProfilesBaseDir() {
    return QDir(NameCache::appDataDir()).filePath("mod_profiles");
}

QString ModProfileStore::indexPath() {
    return QDir(modProfilesBaseDir()).filePath("index.json");
}

QString ModProfileStore::defaultWorkspaceRoot() {
    return QDir(modProfilesBaseDir()).filePath("workspaces");
}

QString ModProfileStore::workspacePathFor(const QString& gameId, const ModProfileId& id) {
    // Both gameId and id are hash/UUID strings — safe to use as directory components.
    // Truncate gameId to 40 chars as an extra safety measure.
    const QString safeGame = gameId.left(40);
    return QDir(defaultWorkspaceRoot()).filePath(safeGame + "/" + id);
}

// ─── JSON helpers ────────────────────────────────────────────────────────────

static ModProfile profileFromJson(const QJsonObject& o) {
    ModProfile p;
    p.id            = o.value("id").toString();
    p.gameId        = o.value("game_id").toString();
    p.name          = o.value("name").toString();
    p.description   = o.value("description").toString();
    p.workspacePath = o.value("workspace_path").toString();
    p.createdAt     = o.value("created_at").toString();
    p.updatedAt     = o.value("updated_at").toString();
    // Phase 4A: baseline fields (optional — default values for pre-4A profiles)
    p.isBaseline    = o.value("is_baseline").toBool(false);
    p.baselineType  = static_cast<BaselineType>(
        o.value("baseline_type").toInt(static_cast<int>(BaselineType::Custom)));
    return p;
}

static QJsonObject profileToJson(const ModProfile& p) {
    QJsonObject o;
    o["id"]             = p.id;
    o["game_id"]        = p.gameId;
    o["name"]           = p.name;
    o["description"]    = p.description;
    o["workspace_path"] = p.workspacePath;
    o["created_at"]     = p.createdAt;
    o["updated_at"]     = p.updatedAt;
    // Phase 4A: baseline fields
    o["is_baseline"]    = p.isBaseline;
    o["baseline_type"]  = static_cast<int>(p.baselineType);
    return o;
}

// ─── Index I/O ───────────────────────────────────────────────────────────────

bool ModProfileStore::loadIndex(QVector<ModProfile>&         outProfiles,
                                QHash<QString, ModProfileId>& outActiveByGame,
                                QString*                      outErr) const {
    outProfiles.clear();
    outActiveByGame.clear();

    const QString path = indexPath();
    QFile f(path);
    if (!f.exists()) return true; // first run — empty index is fine

    if (!f.open(QIODevice::ReadOnly)) {
        const QString msg = QString("Cannot open mod profiles index: %1").arg(path);
        if (outErr) *outErr = msg;
        gf::core::logError(gf::core::LogCategory::FileIO,
                           "ModProfileStore: failed to open index",
                           path.toStdString());
        return false;
    }

    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (doc.isNull()) {
        const QString msg = QString("JSON parse error in mod profiles index: %1").arg(pe.errorString());
        if (outErr) *outErr = msg;
        gf::core::logError(gf::core::LogCategory::FileIO,
                           "ModProfileStore: index JSON parse error",
                           pe.errorString().toStdString());
        return false;
    }

    const QJsonObject root = doc.object();

    // Forward-compatibility: warn on newer schemas, but still attempt to read.
    const int ver = root.value("schema_version").toInt(0);
    if (ver > kIndexSchemaVersion) {
        gf::core::logWarn(gf::core::LogCategory::FileIO,
                          "ModProfileStore: index schema version newer than supported",
                          std::to_string(ver));
    }

    for (const auto& v : root.value("profiles").toArray()) {
        const ModProfile p = profileFromJson(v.toObject());
        if (p.isValid()) outProfiles.append(p);
    }

    const QJsonObject activeObj = root.value("active_profile_per_game").toObject();
    for (auto it = activeObj.begin(); it != activeObj.end(); ++it) {
        const QString pid = it.value().toString();
        if (!it.key().isEmpty() && !pid.isEmpty())
            outActiveByGame.insert(it.key(), pid);
    }

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModProfileStore: index loaded",
                      std::to_string(outProfiles.size()) + " profiles");
    return true;
}

bool ModProfileStore::saveIndex(const QVector<ModProfile>&         profiles,
                                const QHash<QString, ModProfileId>& activeByGame,
                                QString*                            outErr) const {
    // Ensure directory exists before writing
    const QString dir = modProfilesBaseDir();
    if (!QDir().mkpath(dir)) {
        if (outErr) *outErr = QString("Cannot create mod profiles directory: %1").arg(dir);
        return false;
    }

    QJsonArray arr;
    for (const auto& p : profiles) arr.append(profileToJson(p));

    QJsonObject activeObj;
    for (auto it = activeByGame.constBegin(); it != activeByGame.constEnd(); ++it)
        activeObj.insert(it.key(), it.value());

    QJsonObject root;
    root["schema_version"]          = kIndexSchemaVersion;
    root["profiles"]                = arr;
    root["active_profile_per_game"] = activeObj;

    const QString json =
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));

    gf::core::SafeWriteOptions opt;
    opt.make_backup = true;
    opt.max_bytes   = 4ull * 1024ull * 1024ull; // 4 MiB sanity cap

    const auto res = gf::core::safe_write_text(
        std::filesystem::path(indexPath().toStdString()),
        json.toStdString(),
        opt);

    if (!res.ok) {
        if (outErr) *outErr = QString::fromStdString(res.message);
        gf::core::logError(gf::core::LogCategory::FileIO,
                           "ModProfileStore: failed to save index",
                           res.message);
        return false;
    }

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModProfileStore: index saved",
                      std::to_string(profiles.size()) + " profiles");
    return true;
}

// ─── Per-workspace profile.json ──────────────────────────────────────────────

bool ModProfileStore::writeProfileJson(const ModProfile& profile, QString* outErr) const {
    if (!QDir().mkpath(profile.workspacePath)) {
        if (outErr)
            *outErr = QString("Cannot create workspace directory: %1").arg(profile.workspacePath);
        return false;
    }

    const QString path = QDir(profile.workspacePath).filePath("profile.json");

    QJsonObject o = profileToJson(profile);
    o["schema_version"] = kProfileSchemaVersion;

    const QString json =
        QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));

    gf::core::SafeWriteOptions opt;
    opt.make_backup = false; // profile.json is low-stakes; index.json is the authority
    opt.max_bytes   = 64ull * 1024ull; // 64 KiB sanity cap

    const auto res = gf::core::safe_write_text(
        std::filesystem::path(path.toStdString()),
        json.toStdString(),
        opt);

    if (!res.ok) {
        if (outErr) *outErr = QString::fromStdString(res.message);
        gf::core::logError(gf::core::LogCategory::FileIO,
                           "ModProfileStore: failed to write profile.json",
                           res.message);
        return false;
    }
    return true;
}

std::optional<ModProfile> ModProfileStore::readProfileJson(const QString& workspacePath,
                                                            QString*       outErr) const {
    const QString path = QDir(workspacePath).filePath("profile.json");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (outErr) *outErr = QString("Cannot read profile.json: %1").arg(path);
        return std::nullopt;
    }

    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (doc.isNull()) {
        if (outErr) *outErr = pe.errorString();
        return std::nullopt;
    }

    const ModProfile p = profileFromJson(doc.object());
    if (!p.isValid()) {
        if (outErr) *outErr = "profile.json data is invalid or incomplete";
        return std::nullopt;
    }
    return p;
}

} // namespace gf::gui
