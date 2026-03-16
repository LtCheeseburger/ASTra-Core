#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

namespace gf::gui {

// Per-game project state stored under AppData (safe; does not touch game folders).
// v0.6.20: minimal support for Recent files + future Pinned files.
class ProjectState final {
public:
  ProjectState() = default;

  static ProjectState loadForRoot(const QString& rootPath, QString* outErr = nullptr);

  // Persist current state.
  bool save(QString* outErr = nullptr) const;

  // Adds a recent entry (deduped). `score` reserved for future sorting.
  void addRecent(const QString& path, int score = 0);

  const QStringList& recentFiles() const { return m_recentFiles; }
  const QStringList& pinnedFiles() const { return m_pinnedFiles; }

  static QString stableProjectIdForRoot(const QString& rootPath);
  static QString stateFilePathForRoot(const QString& rootPath);

private:
  QString m_rootPath;
  QString m_projectId;
  QStringList m_recentFiles;
  QStringList m_pinnedFiles;
};

} // namespace gf::gui
