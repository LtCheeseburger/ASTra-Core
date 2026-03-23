#include "DeploymentStateStore.hpp"

#include <gf/core/log.hpp>
#include <gf/core/safe_write.hpp>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <filesystem>

namespace gf::gui {

static constexpr int kDeploySchemaVersion = 1;

// static
QString DeploymentStateStore::statePath(const QString& workspacePath) {
    return QDir(workspacePath).filePath("deployment_state.json");
}

// static
std::optional<DeploymentState> DeploymentStateStore::load(const QString& workspacePath,
                                                            QString*       outErr) {
    const QString path = statePath(workspacePath);
    QFile f(path);
    if (!f.exists()) return std::nullopt; // never applied — not an error

    if (!f.open(QIODevice::ReadOnly)) {
        const QString msg = QString("Cannot open deployment state: %1").arg(path);
        if (outErr) *outErr = msg;
        gf::core::logWarn(gf::core::LogCategory::FileIO,
                          "DeploymentStateStore: cannot open", path.toStdString());
        return std::nullopt;
    }

    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (doc.isNull()) {
        const QString msg =
            QString("Deployment state JSON parse error: %1").arg(pe.errorString());
        if (outErr) *outErr = msg;
        gf::core::logWarn(gf::core::LogCategory::FileIO,
                          "DeploymentStateStore: parse error",
                          pe.errorString().toStdString());
        return std::nullopt;
    }

    const QJsonObject root = doc.object();
    const int ver = root.value("schema_version").toInt(0);
    if (ver > kDeploySchemaVersion) {
        gf::core::logWarn(gf::core::LogCategory::General,
                          "DeploymentStateStore: schema version newer than supported",
                          std::to_string(ver));
    }

    DeploymentState state;
    state.profileId = root.value("profile_id").toString();
    state.appliedAt = root.value("applied_at").toString();
    state.status    = deploymentStatusFromString(root.value("status").toString());

    for (const auto& v : root.value("files").toArray()) {
        const QJsonObject fo = v.toObject();
        DeploymentFileEntry entry;
        entry.filename = fo.value("filename").toString();
        entry.size     = static_cast<qint64>(fo.value("size").toDouble());
        entry.destPath = fo.value("dest_path").toString(); // Phase 5A; empty for legacy entries
        state.files.append(entry);
    }

    return state;
}

// static
bool DeploymentStateStore::save(const QString&         workspacePath,
                                 const DeploymentState& state,
                                 QString*               outErr) {
    QJsonArray filesArr;
    for (const DeploymentFileEntry& e : state.files) {
        QJsonObject fo;
        fo["filename"] = e.filename;
        fo["size"]     = static_cast<double>(e.size); // double is safe for all practical sizes
        if (!e.destPath.isEmpty())
            fo["dest_path"] = e.destPath; // Phase 5A: only written when present
        filesArr.append(fo);
    }

    QJsonObject root;
    root["schema_version"] = kDeploySchemaVersion;
    root["profile_id"]     = state.profileId;
    root["applied_at"]     = state.appliedAt;
    root["status"]         = deploymentStatusString(state.status);
    root["files"]          = filesArr;

    const QString json =
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));

    gf::core::SafeWriteOptions opt;
    opt.make_backup = false; // deployment_state.json is cheap to re-derive; no backup needed
    opt.max_bytes   = 4ull * 1024ull * 1024ull; // 4 MiB guard

    const auto res = gf::core::safe_write_text(
        std::filesystem::path(statePath(workspacePath).toStdString()),
        json.toStdString(),
        opt);

    if (!res.ok) {
        if (outErr) *outErr = QString::fromStdString(res.message);
        gf::core::logError(gf::core::LogCategory::FileIO,
                           "DeploymentStateStore: save failed", res.message);
        return false;
    }

    gf::core::logInfo(gf::core::LogCategory::General,
                      "DeploymentStateStore: state saved",
                      (deploymentStatusString(state.status) +
                       " | " + state.profileId).toStdString());
    return true;
}

} // namespace gf::gui
