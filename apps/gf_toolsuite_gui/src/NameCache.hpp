
#pragma once
#include <QString>
#include <QHash>
#include <QMutex>
#include <QDateTime>

namespace gf::gui {

// Simple persistent name cache (per game) to avoid re-deriving friendly names.
// Key is stable within a given AST file: astPath|baseOffset|dataOffset|size|type
class NameCache {
public:
  void clear();
  bool loadForGame(const QString& gameId);
  bool save();

  // If the last save created a backup, this returns the backup path (native separators).
  QString lastBackupPath() const;

  void setGameId(const QString& gameId);
  QString gameId() const;

  QString lookup(const QString& key) const;
  void put(const QString& key, const QString& friendlyBase);

  // Optional metadata used for smarter bucketing (Textures/UI/Models) and UI detection.
  // kind is one of: "Textures", "UI", "Models", "ASTs".
  QString lookupKind(const QString& key) const;
  bool lookupHasApt(const QString& key) const;
  void putMeta(const QString& key, const QString& friendlyBase, const QString& kind, bool hasApt);

  // Helper to build a stable cache key.
  static QString makeKey(const QString& astPath,
                         quint64 baseOffset,
                         quint64 dataOffset,
                         quint64 size,
                         const QString& type);

  // App data folder used by ASTra.
  static QString appDataDir();

private:
  QString m_gameId;
  QString m_path;
  mutable QMutex m_mu;
  QHash<QString, QString> m_map;
  QHash<QString, QString> m_kind;
  QHash<QString, bool> m_hasApt;
  bool m_dirty = false;

  // Last backup file written by save() (if any).
  QString m_lastBackupPath;
};

} // namespace gf::gui
