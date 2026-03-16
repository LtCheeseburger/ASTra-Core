#include "GameLibrary.hpp"

#include "gf/core/log.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace gf::gui {

static QJsonObject to_json(const GameEntry& e) {
  QJsonObject o;
  o["id"] = e.id;
  o["platform"] = e.platform;
  o["rootPath"] = e.rootPath;

  o["title"] = e.title;
  o["titleId"] = e.titleId;
  o["displayName"] = e.displayName;

  o["iconPath"] = e.iconPath;
  o["boxArtKey"] = e.boxArtKey;

  o["usrdirPath"] = e.usrdirPath;
  o["updateDirPath"] = e.updateDirPath;
  o["image0Path"] = e.image0Path;
  o["sc0Path"] = e.sc0Path;

  {
    QJsonArray a;
    for (const auto& p : e.mainAstPaths) a.append(p);
    o["mainAstPaths"] = a;
  }
  {
    QJsonArray a;
    for (const auto& p : e.patchAstPaths) a.append(p);
    o["patchAstPaths"] = a;
  }

  o["addedAt"] = e.addedAt;
  o["lastOpenedAt"] = e.lastOpenedAt;

  o["astCount"] = e.astCount;
  o["hasAst"] = e.hasAst;
  o["scanRoot"] = e.scanRoot;
  o["lastScanAt"] = e.lastScanAt;
  o["containerType"] = e.containerType;
  o["containerCount"] = e.containerCount;
  o["bigCount"] = e.bigCount;
  o["terfCount"] = e.terfCount;
  o["filesExamined"] = e.filesExamined;
  o["foldersScanned"] = e.foldersScanned;
  // qint64 -> JSON number (double) is fine for ms-scale durations.
  o["scanDurationMs"] = (double)e.scanDurationMs;
  {
    QJsonArray a;
    for (const auto& w : e.scanWarnings) a.append(w);
    o["scanWarnings"] = a;
  }
  o["lastScanDiff"] = e.lastScanDiff;
  o["resolvedQueryTitle"] = e.resolvedQueryTitle;
  return o;
}

static GameEntry from_json(const QJsonObject& o) {
  GameEntry e;
  e.id = o.value("id").toString();
  e.platform = o.value("platform").toString();
  e.rootPath = o.value("rootPath").toString();

  e.title = o.value("title").toString();
  e.titleId = o.value("titleId").toString();
  e.displayName = o.value("displayName").toString();

  e.iconPath = o.value("iconPath").toString();
  e.boxArtKey = o.value("boxArtKey").toString();

  e.usrdirPath = o.value("usrdirPath").toString();
  e.updateDirPath = o.value("updateDirPath").toString();
  e.image0Path = o.value("image0Path").toString();
  e.sc0Path = o.value("sc0Path").toString();

  for (const auto& v : o.value("mainAstPaths").toArray()) e.mainAstPaths.push_back(v.toString());
  for (const auto& v : o.value("patchAstPaths").toArray()) e.patchAstPaths.push_back(v.toString());

  e.addedAt = o.value("addedAt").toString();
  e.lastOpenedAt = o.value("lastOpenedAt").toString();

  e.astCount = o.value("astCount").toInt(0);
  e.hasAst = o.value("hasAst").toBool(false);
  e.scanRoot = o.value("scanRoot").toString();
  e.lastScanAt = o.value("lastScanAt").toString();
  e.containerType = o.value("containerType").toString();
  e.containerCount = o.value("containerCount").toInt(0);
  e.bigCount = o.value("bigCount").toInt(0);
  e.terfCount = o.value("terfCount").toInt(0);
  e.filesExamined = o.value("filesExamined").toInt(0);
  e.foldersScanned = o.value("foldersScanned").toInt(0);
  e.scanDurationMs = (qint64)o.value("scanDurationMs").toDouble(0.0);
  for (const auto& v : o.value("scanWarnings").toArray()) e.scanWarnings.push_back(v.toString());
  e.lastScanDiff = o.value("lastScanDiff").toString();
  e.resolvedQueryTitle = o.value("resolvedQueryTitle").toString();
  return e;
}

GameLibrary::GameLibrary() = default;

QString GameLibrary::libraryPath() {
  // Per-OS appdata path:
  // Windows: %APPDATA%/ASTraCore/
  // macOS: ~/Library/Application Support/ASTraCore/
  // Linux: ~/.config/ASTraCore/
  // Qt maps this via AppDataLocation.
  const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir d(base);
  if (!d.exists()) d.mkpath(".");
  return d.filePath("games.json");
}

bool GameLibrary::load(QString* outErr) {
  m_games.clear();

  const QString path = libraryPath();
  QFile f(path);
  if (!f.exists()) return true; // first run OK

  if (!f.open(QIODevice::ReadOnly)) {
    if (outErr) *outErr = QString("Could not open library file: %1").arg(path);
    gf::core::Log::get()->warn("GameLibrary: failed to open {}", path.toStdString());
    return false;
  }

  const QByteArray bytes = f.readAll();
  QJsonParseError pe{};
  const auto doc = QJsonDocument::fromJson(bytes, &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
    if (outErr) *outErr = QString("Game library parse error: %1").arg(pe.errorString());
    gf::core::Log::get()->warn("GameLibrary: parse error {}", pe.errorString().toStdString());
    return false;
  }

  const auto root = doc.object();
  const auto arr = root.value("games").toArray();
  for (const auto& v : arr) {
    if (!v.isObject()) continue;
    auto e = from_json(v.toObject());
    if (e.id.isEmpty()) e.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_games.push_back(e);
  }

  return true;
}

bool GameLibrary::save(QString* outErr) const {
  const QString path = libraryPath();

  QJsonArray arr;
  for (const auto& e : m_games) arr.append(to_json(e));

  QJsonObject root;
  root["version"] = 1;
  root["games"] = arr;

  const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);

  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if (outErr) *outErr = QString("Could not write library file: %1").arg(path);
    gf::core::Log::get()->warn("GameLibrary: failed to write {}", path.toStdString());
    return false;
  }

  f.write(bytes);
  if (!f.commit()) {
    if (outErr) *outErr = QString("Could not commit library file: %1").arg(path);
    gf::core::Log::get()->warn("GameLibrary: commit failed {}", path.toStdString());
    return false;
  }
  return true;
}

bool GameLibrary::addOrReplace(const GameEntry& in, QString* outErr) {
  GameEntry e = in;
  if (e.id.isEmpty()) e.id = QUuid::createUuid().toString(QUuid::WithoutBraces);

  const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  if (e.addedAt.isEmpty()) e.addedAt = now;
  if (e.lastOpenedAt.isEmpty()) e.lastOpenedAt = "";

  for (auto& g : m_games) {
    if (g.id == e.id) {
      g = e;
      return save(outErr);
    }
  }

  m_games.push_back(e);
  return save(outErr);
}


std::optional<GameEntry> GameLibrary::findById(const QString& id) const {
  for (const auto& g : m_games) {
    if (g.id == id) return g;
  }
  return std::nullopt;
}

bool GameLibrary::removeById(const QString& id, QString* outErr) {
  for (int i = 0; i < m_games.size(); ++i) {
    if (m_games[i].id == id) {
      m_games.removeAt(i);
      return save(outErr);
    }
  }
  if (outErr) *outErr = "Game not found.";
  return false;
}

bool GameLibrary::touchLastOpened(const QString& id, QString* outErr) {
  const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  for (auto& g : m_games) {
    if (g.id == id) {
      g.lastOpenedAt = now;
      return save(outErr);
    }
  }
  if (outErr) *outErr = "Game not found.";
  return false;
}

bool GameLibrary::setIconPath(const QString& id, const QString& iconPath, QString* outErr) {
  for (auto& g : m_games) {
    if (g.id == id) {
      g.iconPath = iconPath;
      return save(outErr);
    }
  }
  if (outErr) *outErr = "Game not found.";
  return false;
}


bool GameLibrary::setResolvedQueryTitle(const QString& id, const QString& resolvedTitle, QString* outErr) {
  for (auto& g : m_games) {
    if (g.id == id) {
      g.resolvedQueryTitle = resolvedTitle;
      return save(outErr);
    }
  }
  if (outErr) *outErr = "Game not found.";
  return false;
}

bool GameLibrary::setTitle(const QString& id, const QString& title, const QString& displayName, QString* outErr) {
  for (auto& g : m_games) {
    if (g.id == id) {
      g.title = title;
      g.displayName = displayName;
      // Clearing resolvedQueryTitle forces a fresh resolve next time.
      g.resolvedQueryTitle.clear();
      return save(outErr);
    }
  }
  if (outErr) *outErr = "Game not found.";
  return false;
}

bool GameLibrary::updateScanMetadata(const QString& id,
                                     int astCount,
                                     bool hasAst,
                                     const QString& scanRoot,
                                     const QString& lastScanAt,
                                     const QVector<QString>& mainAstPaths,
                                     const QVector<QString>& patchAstPaths,
                                     const QString& containerType,
                                     int containerCount,
                                     int bigCount,
                                     int terfCount,
                                     int filesExamined,
                                     int foldersScanned,
                                     qint64 scanDurationMs,
                                     QString* outErr) {
  for (auto& g : m_games) {
    if (g.id == id) {
      g.astCount = astCount;
      g.hasAst = hasAst;
      g.scanRoot = scanRoot;
      g.lastScanAt = lastScanAt;
      g.mainAstPaths = mainAstPaths;
      g.patchAstPaths = patchAstPaths;
      g.containerType = containerType;
      g.containerCount = containerCount;
      g.bigCount = bigCount;
      g.terfCount = terfCount;
      g.filesExamined = filesExamined;
      g.foldersScanned = foldersScanned;
      g.scanDurationMs = scanDurationMs;
      return save(outErr);
    }
  }
  if (outErr) *outErr = "Game not found.";
  return false;
}


bool GameLibrary::updateScanMetadata(const QString& id,
                                     const gf::models::scan_result& r,
                                     const QVector<QString>& mainAstPaths,
                                     const QVector<QString>& patchAstPaths,
                                     const QString& containerType,
                                     const QString& lastScanDiff,
                                     const QString& lastScanAt,
                                     QString* outErr) {

  // Persist soft warnings + last diff (non-blocking) for UI tooltips/diagnostics.
  for (auto& g : m_games) {
    if (g.id == id) {
      g.scanWarnings.clear();
      for (const auto& w : r.warnings) g.scanWarnings.push_back(QString::fromStdString(w));
      g.lastScanDiff = lastScanDiff;
      break;
    }
  }

  const QString scanRoot = QString::fromStdString(r.scan_root);
  const int astCount = r.counts.ast;
  const bool hasAst = astCount > 0;

  const int bigCount = r.counts.big;
  const int terfCount = r.counts.terf;

  const QString ct = containerType.isEmpty()
                       ? QString::fromStdString(gf::models::to_string(r.primary_container))
                       : containerType;

  const int containerCount = (ct.trimmed().toUpper() == "BIG" || ct.trimmed().toUpper() == "BIG4") ? bigCount
                         : (ct.trimmed().toUpper() == "TERF") ? terfCount
                         : (ct.trimmed().toUpper() == "AST") ? astCount
                         : 0;

  return updateScanMetadata(id,
                            astCount,
                            hasAst,
                            scanRoot,
                            lastScanAt,
                            mainAstPaths,
                            patchAstPaths,
                            ct,
                            containerCount,
                            bigCount,
                            terfCount,
                            (int)r.files_examined,
                            (int)r.folders_scanned,
                            (qint64)r.duration_ms,
                            outErr);
}


QString GameLibrary::defaultBoxArtForKey(const QString& key) {
  const QString file = QString("%1.png").arg(key);

  // 1) Preferred: runtime folder next to the executable: <app>/game_icons/<key>.png
  const auto appDir = QCoreApplication::applicationDirPath();
  const QString nextToExe = QDir(appDir).filePath(QString("game_icons/%1").arg(file));
  if (QFileInfo::exists(nextToExe)) return nextToExe;

  // 2) Known repo layouts (dev): apps/gf_toolsuite_gui/resources/game_icons/<key>.png
  //    We try a few anchors:
  //      - current working dir
  //      - appDir (when launched from build tree)
  //      - parents of appDir (when in out-win/apps/gf_toolsuite_gui)
  const auto tryRepoPath = [&](const QString& base) -> QString {
    const QString p = QDir(base).filePath(QString("apps/gf_toolsuite_gui/resources/game_icons/%1").arg(file));
    if (QFileInfo::exists(p)) return p;
    const QString p2 = QDir(base).filePath(QString("resources/game_icons/%1").arg(file)); // repo-root convenience
    if (QFileInfo::exists(p2)) return p2;
    return {};
  };

  const QString cwd = QDir::currentPath();
  if (const auto p = tryRepoPath(cwd); !p.isEmpty()) return p;
  if (const auto p = tryRepoPath(appDir); !p.isEmpty()) return p;

  // Walk up a few parents from appDir to find repo-root resources/game_icons
  QDir walk(appDir);
  for (int i = 0; i < 6; ++i) {
    if (!walk.cdUp()) break;
    if (const auto p = tryRepoPath(walk.absolutePath()); !p.isEmpty()) return p;
  }

  // 3) Qt resource fallback (if you later embed them)
  const QString qrc = QString(":/game_icons/%1").arg(file);
  if (QFileInfo::exists(qrc)) return qrc;

  return QString();
}

} // namespace gf::gui
