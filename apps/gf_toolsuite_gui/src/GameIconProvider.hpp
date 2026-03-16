#pragma once

#include <QObject>
#include <QPixmap>
#include <QSize>
#include <QString>

class QNetworkAccessManager;

namespace gf::gui {

// Downloads and caches 320x176 game cover images based on a game title.
// Default implementation uses Wikipedia REST summary thumbnails (no API key).
class GameIconProvider final : public QObject {
  Q_OBJECT
public:
  explicit GameIconProvider(QObject* parent = nullptr);

  // Returns a cached icon if available; otherwise returns null pixmap and starts an async download.
  // When complete, emits iconAvailable.
  // Uses CSV + overrides to turn arbitrary titles into a stable lookup key.
  static QString resolveQueryTitle(const QString& originalTitle);

  QPixmap getOrRequest(const QString& gameId, const QString& title);

  // Forces a cover image from a user-provided URL (downloads + caches + emits iconAvailable).
  void requestFromUrl(const QString& gameId, const QUrl& url);

  // Sets a cover image from a user-selected local file (caches + emits iconAvailable).
  void requestFromLocalFile(const QString& gameId, const QString& filePath);

  static QSize iconSize() { return QSize(320, 176); }

signals:
  void iconFetchStarted(const QString& gameId);
  void iconAvailable(const QString& gameId, const QString& absolutePath, const QPixmap& pixmap);

private:
  QString cacheDir() const;
  QString cachePathForTitle(const QString& title) const;
  QString cachePathForGame(const QString& gameId) const;
  void ensureCacheDir() const;

  void requestWikipediaSummary(const QString& gameId, const QString& title);
  void requestImage(const QString& gameId, const QString& title, const QUrl& imageUrl);

  static QPixmap scaleAndCropToTile(const QImage& img);

  QNetworkAccessManager* m_net = nullptr;
};

} // namespace gf::gui
