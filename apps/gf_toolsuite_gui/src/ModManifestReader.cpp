#include "ModManifestReader.hpp"

#include <gf/core/log.hpp>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace gf::gui {

static constexpr qint64 kMaxManifestBytes = 64LL * 1024LL; // 64 KiB sanity cap

// ── public API ────────────────────────────────────────────────────────────────

// static
std::optional<ModManifest> ModManifestReader::readFromFolder(const QString& modFolder,
                                                              QString*       outErr) {
    auto fail = [&](const QString& msg) -> std::optional<ModManifest> {
        if (outErr) *outErr = msg;
        gf::core::logWarn(gf::core::LogCategory::General,
                          "ModManifestReader: " + msg.toStdString());
        return std::nullopt;
    };

    if (modFolder.isEmpty())
        return fail("Mod folder path is empty.");

    const QDir dir(modFolder);
    if (!dir.exists())
        return fail(QString("Mod folder does not exist: %1").arg(modFolder));

    // ── Read manifest file ────────────────────────────────────────────────────
    const QString manifestPath = dir.filePath(kModManifestFilename);
    QFile f(manifestPath);
    if (!f.exists())
        return fail(QString("No astra_mod.json found in: %1").arg(modFolder));

    if (!f.open(QIODevice::ReadOnly))
        return fail(QString("Cannot open astra_mod.json: %1").arg(f.errorString()));

    const QByteArray raw = f.read(kMaxManifestBytes + 1);
    f.close();

    if (raw.size() > kMaxManifestBytes)
        return fail("astra_mod.json exceeds the 64 KiB size limit.");

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
    if (doc.isNull())
        return fail(QString("JSON parse error in astra_mod.json: %1").arg(pe.errorString()));

    const QJsonObject root = doc.object();

    // ── Schema version ────────────────────────────────────────────────────────
    const int ver = root.value("schema_version").toInt(0);
    if (ver < 1) {
        return fail(QString("Invalid manifest schema_version: %1").arg(ver));
    }
    if (ver > kModManifestSchemaVersion) {
        // Phase 5E: forward-compat — newer schema but the fields we understand are
        // a strict subset, so we can still read the manifest.  Log a warning only.
        gf::core::logWarn(gf::core::LogCategory::General,
                          "ModManifestReader: schema_version is newer than supported — "
                          "some fields may be ignored",
                          "ver=" + std::to_string(ver) +
                          " supported=" + std::to_string(kModManifestSchemaVersion));
    }

    ModManifest m;
    m.schemaVersion    = ver;
    m.manifestRawBytes = raw;
    m.sourcePath       = QDir::cleanPath(QDir(modFolder).absolutePath());

    // ── Required fields ───────────────────────────────────────────────────────
    m.modId = root.value("mod_id").toString().trimmed();
    if (m.modId.isEmpty())
        return fail("Manifest is missing required field: mod_id");

    {
        QString idErr;
        if (!validateModId(m.modId, &idErr))
            return fail(idErr);
    }

    m.name = root.value("name").toString().trimmed();
    if (m.name.isEmpty())
        return fail("Manifest is missing required field: name");

    m.version = root.value("version").toString().trimmed();
    if (m.version.isEmpty())
        return fail("Manifest is missing required field: version");

    // ── Optional fields ───────────────────────────────────────────────────────
    m.author      = root.value("author").toString().trimmed();
    m.description = root.value("description").toString().trimmed();
    m.notes       = root.value("notes").toString().trimmed();

    for (const auto& v : root.value("target_game_ids").toArray()) {
        const QString gid = v.toString().trimmed();
        if (!gid.isEmpty()) m.targetGameIds << gid;
    }
    for (const auto& v : root.value("platforms").toArray()) {
        const QString plat = v.toString().trimmed();
        if (!plat.isEmpty()) m.platforms << plat;
    }

    // Phase 5B: optional package metadata
    m.packageType = root.value("package_type").toString().trimmed();
    m.category    = root.value("category").toString().trimmed();
    for (const auto& v : root.value("tags").toArray()) {
        const QString tag = v.toString().trimmed();
        if (!tag.isEmpty()) m.tags << tag;
    }

    // Phase 5C: optional asset references
    // Phase 5E: validate relative-path safety to reject path traversal in manifests.
    m.icon = root.value("icon").toString().trimmed();
    if (!m.icon.isEmpty() && !isPathSafe(m.icon)) {
        return fail(QString("icon path is unsafe: %1").arg(m.icon));
    }
    for (const auto& v : root.value("previews").toArray()) {
        const QString p = v.toString().trimmed();
        if (p.isEmpty()) continue;
        if (!isPathSafe(p)) {
            return fail(QString("previews contains unsafe path: %1").arg(p));
        }
        m.previews << p;
    }

    // ── Payload files ─────────────────────────────────────────────────────────
    const QDir filesDir(dir.filePath("files"));
    if (!filesDir.exists())
        return fail("Mod folder is missing the required files/ subdirectory.");

    bool collectError = false;
    QString collectErrMsg;
    collectFiles(filesDir.absolutePath(), QString(), m.payloadFiles, &collectErrMsg, collectError);

    if (collectError)
        return fail(collectErrMsg);

    if (m.payloadFiles.isEmpty())
        return fail("Mod files/ directory contains no files.");

    m.payloadFiles.sort();

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModManifestReader: parsed manifest",
                      (m.modId + " v" + m.version + " — " +
                       QString::number(m.payloadFiles.size()) + " files").toStdString());
    return m;
}

// ── private helpers ───────────────────────────────────────────────────────────

// static
void ModManifestReader::collectFiles(const QString& baseDir,
                                     const QString& relPrefix,
                                     QStringList&   out,
                                     QString*       outErr,
                                     bool&          hadError) {
    if (hadError) return;

    const QDir dir(baseDir);
    // Files in this directory
    for (const QFileInfo& fi : dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
        const QString rel = relPrefix.isEmpty()
                            ? fi.fileName()
                            : (relPrefix + "/" + fi.fileName());
        if (!isPathSafe(rel)) {
            if (outErr) *outErr = QString("Unsafe path in files/ directory: %1").arg(rel);
            hadError = true;
            return;
        }
        out << rel;
    }
    // Recurse into subdirectories
    for (const QFileInfo& fi : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString subRel = relPrefix.isEmpty()
                               ? fi.fileName()
                               : (relPrefix + "/" + fi.fileName());
        collectFiles(fi.absoluteFilePath(), subRel, out, outErr, hadError);
        if (hadError) return;
    }
}

// static
bool ModManifestReader::validateModId(const QString& id, QString* outErr) {
    // mod_id must be URL-safe: letters, digits, underscores, hyphens; 3–64 chars.
    static const QRegularExpression kSafeId(QStringLiteral(R"(^[A-Za-z0-9_\-]{3,64}$)"));
    if (!kSafeId.match(id).hasMatch()) {
        if (outErr) {
            *outErr = QString(
                "mod_id '%1' is invalid. "
                "Use only letters, digits, underscores, and hyphens (3–64 chars).").arg(id);
        }
        return false;
    }
    return true;
}

// static
bool ModManifestReader::isPathSafe(const QString& relPath) {
    if (relPath.isEmpty()) return false;
    // Reject absolute paths and upward traversal
    if (relPath.startsWith('/') || relPath.startsWith('\\')) return false;
    if (relPath.contains("..")) return false;
    if (relPath.contains('\0')) return false;
    // Reject Windows reserved device names (CON, PRN, AUX, NUL, COM1–9, LPT1–9)
    static const QRegularExpression kDevName(
        QStringLiteral(R"((?:^|[/\\])(?:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.[^/\\]*)?$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (kDevName.match(relPath).hasMatch()) return false;
    return true;
}

} // namespace gf::gui
