#pragma once
#include <QString>
#include <QVector>
#include <optional>

#include "gf/models/scan_result.hpp"

namespace gf::gui {

struct GameEntry {
  QString id;           // uuid / stable hash
  QString platform;     // "ps3" | "xbox360" | future
  QString rootPath;

  QString title;
  QString titleId;      // PS3 only (BLUS/BLES/etc) or empty
  QString displayName;

  // Icon sources:
  // - PS3: absolute path to PS3_GAME/ICON0.PNG (from user's dump)
  // - Others: leave empty and use boxArtKey->resource mapping
  QString iconPath;

  // Bundled art key (fallback, and for Xbox/others)
  QString boxArtKey;

  // Platform content roots (not all are used for every platform)
  QString usrdirPath;    // PS3: PS3_GAME/USRDIR
  QString updateDirPath; // Optional: PS3 update/patch dir
  QString image0Path;    // PS4: Image0
  QString sc0Path;       // PS4: Sc0 (param.sfo, icon0.png)
  QVector<QString> mainAstPaths;
  QVector<QString> patchAstPaths;

  QString addedAt;
  QString lastOpenedAt;

  // Cached metadata (for instant startup + no rescans unless requested)
  int astCount = 0;          // total ASTs (main + patch) at last scan
  bool hasAst = false;       // convenience flag
  QString scanRoot;          // resolved folder used for scanning (USRDIR/Image0/content/etc)
  QString lastScanAt;        // UTC ISO string

  // Primary container format detected at scan time.
  // - "AST" when .ast/.bgfa files were found.
  // - Otherwise: "TERF", "BIG", or "Unknown".
  QString containerType;
  int containerCount = 0;    // number of container files matching containerType

  // Extra cached scan metadata (v0.4.6+)
  // Stored to avoid rescans on startup and enable future diffs.
  int bigCount = 0;
  int terfCount = 0;
  int filesExamined = 0;
  int foldersScanned = 0;
  qint64 scanDurationMs = 0;

  // Soft scan warnings (non-blocking). Persisted for UI tooltips.
  QVector<QString> scanWarnings;

  // Summary of changes detected on the last rescan (non-blocking, informational).
  QString lastScanDiff;

  // Stable cover lookup key (resolved via CSV/overrides; persisted so icons don't "flip")
  QString resolvedQueryTitle;

};

class GameLibrary {
public:
  GameLibrary();

  bool load(QString* outErr);
  bool save(QString* outErr) const;

  const QVector<GameEntry>& games() const { return m_games; }

  std::optional<GameEntry> findById(const QString& id) const;

  bool addOrReplace(const GameEntry& e, QString* outErr);
  bool removeById(const QString& id, QString* outErr);
  bool touchLastOpened(const QString& id, QString* outErr);

  // Update iconPath for an existing entry (used by online icon cache).
  bool setIconPath(const QString& id, const QString& iconPath, QString* outErr);
  bool setResolvedQueryTitle(const QString& id, const QString& resolvedTitle, QString* outErr);
  bool setTitle(const QString& id, const QString& title, const QString& displayName, QString* outErr);
  bool updateScanMetadata(const QString& id, int astCount, bool hasAst, const QString& scanRoot, const QString& lastScanAt,
                          const QVector<QString>& mainAstPaths, const QVector<QString>& patchAstPaths,
                          const QString& containerType, int containerCount,
                          int bigCount, int terfCount,
                          int filesExamined, int foldersScanned,
                          qint64 scanDurationMs,
                          QString* outErr);

  // v0.5.x foundation: single scan result model (future-friendly).
  // This overload is a thin adapter over the existing cached fields.
  bool updateScanMetadata(const QString& id,
                          const gf::models::scan_result& r,
                          const QVector<QString>& mainAstPaths,
                          const QVector<QString>& patchAstPaths,
                          const QString& containerType,
                          const QString& lastScanDiff,
                          const QString& lastScanAt,
                          QString* outErr);

  static QString libraryPath();
  static QString defaultBoxArtForKey(const QString& key);

private:
  QVector<GameEntry> m_games;
};

} // namespace gf::gui
