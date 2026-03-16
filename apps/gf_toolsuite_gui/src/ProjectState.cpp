#include "ProjectState.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace gf::gui {

static QString appDataDir() {
  // e.g. %APPDATA%/ASTra
  const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir d(base);
  if (!d.exists()) d.mkpath(".");
  return d.absolutePath();
}

QString ProjectState::stableProjectIdForRoot(const QString& rootPath) {
  const QByteArray b = QFileInfo(rootPath).absoluteFilePath().toUtf8();
  const QByteArray h = QCryptographicHash::hash(b, QCryptographicHash::Sha1).toHex();
  return QString::fromUtf8(h);
}

QString ProjectState::stateFilePathForRoot(const QString& rootPath) {
  const QString id = stableProjectIdForRoot(rootPath);
  QDir d(appDataDir());
  if (!d.exists("projects")) d.mkpath("projects");
  return d.filePath(QString("projects/%1.json").arg(id));
}

ProjectState ProjectState::loadForRoot(const QString& rootPath, QString* outErr) {
  ProjectState ps;
  ps.m_rootPath = QFileInfo(rootPath).absoluteFilePath();
  ps.m_projectId = stableProjectIdForRoot(ps.m_rootPath);

  const QString filePath = stateFilePathForRoot(ps.m_rootPath);
  QFile f(filePath);
  if (!f.exists()) return ps;
  if (!f.open(QIODevice::ReadOnly)) {
    if (outErr) *outErr = QString("Failed to open project state: %1").arg(filePath);
    return ps;
  }

  const QByteArray bytes = f.readAll();
  f.close();

  QJsonParseError pe{};
  const auto doc = QJsonDocument::fromJson(bytes, &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
    if (outErr) *outErr = QString("Invalid project state JSON: %1").arg(filePath);
    return ps;
  }

  const QJsonObject o = doc.object();
  const auto recent = o.value("recentFiles").toArray();
  for (const auto& v : recent) {
    if (v.isString()) ps.m_recentFiles.push_back(v.toString());
  }
  const auto pinned = o.value("pinnedFiles").toArray();
  for (const auto& v : pinned) {
    if (v.isString()) ps.m_pinnedFiles.push_back(v.toString());
  }
  return ps;
}

bool ProjectState::save(QString* outErr) const {
  if (m_rootPath.isEmpty()) return true;

  QJsonObject o;
  o.insert("rootPath", m_rootPath);
  o.insert("projectId", m_projectId);

  QJsonArray recent;
  for (const auto& s : m_recentFiles) recent.append(s);
  o.insert("recentFiles", recent);

  QJsonArray pinned;
  for (const auto& s : m_pinnedFiles) pinned.append(s);
  o.insert("pinnedFiles", pinned);

  const QJsonDocument doc(o);

  const QString filePath = stateFilePathForRoot(m_rootPath);
  QFile f(filePath);
  QDir().mkpath(QFileInfo(filePath).absolutePath());
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if (outErr) *outErr = QString("Failed to write project state: %1").arg(filePath);
    return false;
  }
  f.write(doc.toJson(QJsonDocument::Indented));
  f.close();
  return true;
}

void ProjectState::addRecent(const QString& path, int) {
  const QString p = QFileInfo(path).absoluteFilePath();
  m_recentFiles.removeAll(p);
  m_recentFiles.push_front(p);
  while (m_recentFiles.size() > 20) m_recentFiles.pop_back();
}

} // namespace gf::gui
