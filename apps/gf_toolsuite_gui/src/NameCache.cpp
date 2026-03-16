
#include "NameCache.hpp"

#include "gf/core/safe_write.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QFileInfo>

#include <filesystem>

namespace gf::gui {

void NameCache::clear() {
  QMutexLocker lk(&m_mu);
  m_map.clear();
  m_kind.clear();
  m_hasApt.clear();
  m_dirty = false;
  m_lastBackupPath.clear();
}

QString NameCache::appDataDir() {
  // Keep cache storage stable across ASTra Core exe/version updates by using a
  // fixed app folder under the generic config location instead of a versioned
  // or executable-derived path.
  QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
  if (base.isEmpty()) {
    base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
  }
  const QString dir = QDir(base).filePath("ASTra Core");
  QDir().mkpath(dir);
  return dir;
}

void NameCache::setGameId(const QString& gameId) {
  m_gameId = gameId;
  const QString dir = appDataDir();
  m_path = QDir(dir).filePath(QString("name_cache_%1.json").arg(gameId));
}

static QString legacyNameCachePath(const QString& gameId) {
  const QString legacyDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (legacyDir.isEmpty()) return {};
  return QDir(legacyDir).filePath(QString("name_cache_%1.json").arg(gameId));
}

QString NameCache::gameId() const { return m_gameId; }

bool NameCache::loadForGame(const QString& gameId) {
  setGameId(gameId);
  QMutexLocker lk(&m_mu);
  m_map.clear();
  m_kind.clear();
  m_hasApt.clear();
  m_dirty = false;
  m_lastBackupPath.clear();
  m_lastBackupPath.clear();

  QFile f(m_path);
  if (!f.exists()) {
    const QString legacyPath = legacyNameCachePath(gameId);
    if (!legacyPath.isEmpty() && QFileInfo::exists(legacyPath)) {
      QDir().mkpath(QFileInfo(m_path).absolutePath());
      QFile::copy(legacyPath, m_path);
      f.setFileName(m_path);
    }
  }
  if (!f.exists()) return true;
  if (!f.open(QIODevice::ReadOnly)) return false;

  const auto doc = QJsonDocument::fromJson(f.readAll());
  if (!doc.isObject()) return false;

  const QJsonObject o = doc.object();
  for (auto it = o.begin(); it != o.end(); ++it) {
    const auto v = it.value();
    if (v.isString()) {
      m_map.insert(it.key(), v.toString());
      continue;
    }
    if (v.isObject()) {
      const QJsonObject mo = v.toObject();
      const QString name = mo.value("n").toString();
      if (!name.isEmpty()) m_map.insert(it.key(), name);
      const QString kind = mo.value("k").toString();
      if (!kind.isEmpty()) m_kind.insert(it.key(), kind);
      if (mo.contains("a")) m_hasApt.insert(it.key(), mo.value("a").toBool());
    }
  }
  return true;
}

QString NameCache::lastBackupPath() const {
  QMutexLocker lk(&m_mu);
  return m_lastBackupPath;
}

bool NameCache::save() {
  QMutexLocker lk(&m_mu);
  if (!m_dirty || m_path.isEmpty()) return true;

  QJsonObject o;
  for (auto it = m_map.begin(); it != m_map.end(); ++it) {
    const QString key = it.key();
    const QString name = it.value();
    const QString kind = m_kind.value(key);
    const bool hasApt = m_hasApt.value(key, false);
    const bool hasMeta = !kind.isEmpty() || m_hasApt.contains(key);
    if (!hasMeta) {
      o.insert(key, name);
      continue;
    }
    QJsonObject mo;
    mo.insert("n", name);
    if (!kind.isEmpty()) mo.insert("k", kind);
    if (m_hasApt.contains(key)) mo.insert("a", hasApt);
    o.insert(key, mo);
  }

  const QString json = QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));

  gf::core::SafeWriteOptions opt;
  opt.make_backup = true;
  // Name cache should always be tiny; keep a strict safety cap.
  opt.max_bytes = 4ull * 1024ull * 1024ull; // 4 MiB

  const auto res = gf::core::safe_write_text(std::filesystem::path(m_path.toStdString()),
                                            json.toStdString(),
                                            opt);
  if (!res.ok) return false;

  m_lastBackupPath.clear();
  if (res.backup_path) {
    m_lastBackupPath = QDir::toNativeSeparators(QString::fromStdString(res.backup_path->string()));
  }

  m_dirty = false;
  return true;
}

QString NameCache::lookup(const QString& key) const {
  QMutexLocker lk(&m_mu);
  return m_map.value(key);
}

void NameCache::put(const QString& key, const QString& friendlyBase) {
  if (key.isEmpty() || friendlyBase.isEmpty()) return;
  QMutexLocker lk(&m_mu);
  const auto existing = m_map.value(key);
  if (existing == friendlyBase) return;
  m_map.insert(key, friendlyBase);
  m_dirty = true;
}

QString NameCache::lookupKind(const QString& key) const {
  if (key.isEmpty()) return {};
  QMutexLocker lk(&m_mu);
  return m_kind.value(key);
}

bool NameCache::lookupHasApt(const QString& key) const {
  if (key.isEmpty()) return false;
  QMutexLocker lk(&m_mu);
  return m_hasApt.value(key, false);
}

void NameCache::putMeta(const QString& key, const QString& friendlyBase, const QString& kind, bool hasApt) {
  if (key.isEmpty()) return;
  QMutexLocker lk(&m_mu);
  bool changed = false;
  if (!friendlyBase.isEmpty()) {
    const auto existing = m_map.value(key);
    if (existing != friendlyBase) {
      m_map.insert(key, friendlyBase);
      changed = true;
    }
  }
  if (!kind.isEmpty()) {
    const auto existingK = m_kind.value(key);
    if (existingK != kind) {
      m_kind.insert(key, kind);
      changed = true;
    }
  }
  // Only set hasApt if it was computed.
  if (!m_hasApt.contains(key) || m_hasApt.value(key) != hasApt) {
    m_hasApt.insert(key, hasApt);
    changed = true;
  }
  if (changed) m_dirty = true;
}

QString NameCache::makeKey(const QString& astPath, quint64 baseOffset, quint64 dataOffset, quint64 size, const QString& type) {
  return QString("%1|%2|%3|%4|%5").arg(astPath).arg(baseOffset).arg(dataOffset).arg(size).arg(type);
}

} // namespace gf::gui
