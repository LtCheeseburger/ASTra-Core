#include "GameIconProvider.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

namespace gf::gui {

void GameIconProvider::ensureCacheDir() const {
  QDir d(cacheDir());
  if (!d.exists()) {
    d.mkpath(".");
  }
  // Ensure subfolder for custom overrides.
  QDir custom(d.filePath("custom"));
  if (!custom.exists()) custom.mkpath(".");
}

QString GameIconProvider::cachePathForGame(const QString& gameId) const {
  QDir d(cacheDir());
  const QString customDir = d.filePath("custom");
  return QDir(customDir).filePath(gameId + ".png");
}

static QString sha1Hex(const QString& s) {
  return QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1).toHex());
}


static QString normalizeTitle(QString t) {
  t = t.trimmed();
  // Strip common trademark symbols.
  t.replace(QChar(0x00AE), ""); // ®
  t.replace(QChar(0x2122), ""); // ™
  t.replace(QChar(0x2120), ""); // ℠
  // Collapse whitespace.
  t = t.simplified();

  // If title contains a trailing "(...)" (platform/id), drop it for lookup.
  // Examples: "NCAA® Football 14 (PS3, BLUS31159)" -> "NCAA Football 14"
  const int lastOpen = t.lastIndexOf('(');
  const int lastClose = t.endsWith(')') ? t.lastIndexOf(')') : -1;
  if (lastOpen >= 0 && lastClose > lastOpen && (t.length() - lastOpen) < 24) {
    t = t.left(lastOpen).trimmed();
  }
  return t;
}


static QStringList csvSplitLine(const QString& line) {
  QStringList out;
  out.reserve(8);
  QString cur;
  bool inQuotes = false;
  for (int i = 0; i < line.size(); ++i) {
    const QChar c = line[i];
    if (c == '"' ) {
      if (inQuotes && i + 1 < line.size() && line[i + 1] == '"') {
        cur += '"';
        ++i;
      } else {
        inQuotes = !inQuotes;
      }
    } else if (!inQuotes && c == ',') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

static const QHash<QString, QString>& eaSportsTitleAliasMap() {
  // Maps normalized title/alias -> preferred Wikipedia title.
  static QHash<QString, QString> map;
  static bool loaded = false;
  if (loaded) return map;
  loaded = true;

  QFile f(":/data/ea-sports.csv");
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return map;
  }

  QTextStream ts(&f);
  bool first = true;
  while (!ts.atEnd()) {
    const QString line = ts.readLine();
    if (line.trimmed().isEmpty()) continue;
    if (first) { first = false; continue; } // header
    const auto cols = csvSplitLine(line);
    if (cols.size() < 6) continue;

    const QString canonical = normalizeTitle(cols[0]);
    const QString wiki = cols[4].trimmed().isEmpty() ? canonical : cols[4].trimmed();
    const QString aliases = cols.size() >= 6 ? cols[5] : "";

    auto addKey = [&](const QString& k) {
      const QString nk = normalizeTitle(k).toLower();
      if (nk.isEmpty()) return;
      if (!map.contains(nk)) map.insert(nk, wiki);
    };

    addKey(canonical);
    if (!aliases.trimmed().isEmpty()) {
      for (const auto& a : aliases.split(';', Qt::SkipEmptyParts)) addKey(a);
    }
  }
  return map;
}


static const QHash<QString, QString>& titleOverridesMap() {
  static QHash<QString, QString> map;
  static bool loaded = false;
  if (loaded) return map;
  loaded = true;

  const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  const auto path = QDir(base).filePath("title_overrides.json");
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return map;

  const auto doc = QJsonDocument::fromJson(f.readAll());
  if (!doc.isObject()) return map;
  const auto obj = doc.object();
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    const QString k = normalizeTitle(it.key()).toLower();
    const QString v = it.value().toString().trimmed();
    if (!k.isEmpty() && !v.isEmpty()) map.insert(k, v);
  }
  return map;
}

static QString wikipediaQueryFromTitle(const QString& originalTitle) {
  QString t = normalizeTitle(originalTitle);
  if (t.isEmpty()) return t;

  // Try user overrides first.
  const auto& ov = titleOverridesMap();
  const auto key = t.toLower();
  if (ov.contains(key)) return ov.value(key);

  // Then CSV-driven aliasing (more deterministic than raw title strings).
  const auto& map = eaSportsTitleAliasMap();
  if (map.contains(key)) return map.value(key);

  // Heuristic: Wikipedia pages for some series are disambiguated; handle a couple common ones.
  if (t.compare("SSX", Qt::CaseInsensitive) == 0) return "SSX (2000 video game)";

  return t;
}


QString GameIconProvider::resolveQueryTitle(const QString& originalTitle) {
  return wikipediaQueryFromTitle(originalTitle);
}

GameIconProvider::GameIconProvider(QObject* parent)
  : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

QString GameIconProvider::cacheDir() const {
  const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  const auto dir = QDir(base).filePath("cache/game_icons");
  QDir().mkpath(dir);
  return dir;
}

QString GameIconProvider::cachePathForTitle(const QString& title) const {
  const auto key = sha1Hex(title.trimmed().toLower());
  return QDir(cacheDir()).filePath(key + ".png");
}

QPixmap GameIconProvider::getOrRequest(const QString& gameId, const QString& title) {
  const QString resolved = resolveQueryTitle(title);
  const auto path = cachePathForTitle(resolved);
  if (QFileInfo::exists(path)) {
    QPixmap p;
    p.load(path);
    if (!p.isNull()) return p;
  }

  emit iconFetchStarted(gameId);
  // Kick off async request.
  requestWikipediaSummary(gameId, resolved);
  return {};
}

void GameIconProvider::requestFromUrl(const QString& gameId, const QUrl& url) {
  if (!url.isValid()) return;
  emit iconFetchStarted(gameId);

  // Store per-game custom covers separately so changing titles doesn't invalidate them.
  const auto customDir = QDir(cacheDir()).filePath("custom");
  QDir().mkpath(customDir);
  const auto outPath = QDir(customDir).filePath(gameId + ".png");

  QNetworkRequest req(url);
  req.setHeader(QNetworkRequest::UserAgentHeader, "ASTra/0.3 (Qt)");
  auto* reply = m_net->get(req);
  connect(reply, &QNetworkReply::finished, this, [this, reply, gameId, outPath]() {
    const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto bytes = reply->readAll();
    reply->deleteLater();

    if (code < 200 || code >= 300) return;
    QImage img;
    img.loadFromData(bytes);
    if (img.isNull()) return;

    const auto pix = scaleAndCropToTile(img);
    if (pix.isNull()) return;
    pix.save(outPath, "PNG");
    emit iconAvailable(gameId, outPath, pix);
  });
}

void GameIconProvider::requestFromLocalFile(const QString& gameId, const QString& filePath) {
  if (gameId.isEmpty() || filePath.isEmpty()) return;
  ensureCacheDir();
  QImage img(filePath);
  if (img.isNull()) return;
  // Match the same processing as URL downloads: center-crop to 16:9-ish and scale for UI.
  // We keep this conservative (no fancy filters) to avoid pulling in extra dependencies.
  const QSize target(600, 338);
  img = img.convertToFormat(QImage::Format_ARGB32);

  // Center-crop to target aspect.
  const double want = double(target.width()) / double(target.height());
  const double have = double(img.width()) / double(img.height());
  QRect crop(0, 0, img.width(), img.height());
  if (have > want) {
    const int newW = int(img.height() * want);
    crop.setX((img.width() - newW) / 2);
    crop.setWidth(newW);
  } else if (have < want) {
    const int newH = int(img.width() / want);
    crop.setY((img.height() - newH) / 2);
    crop.setHeight(newH);
  }
  img = img.copy(crop);
  img = img.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

  const QString outPath = cachePathForGame(gameId);
  img.save(outPath, "PNG");
  emit iconAvailable(gameId, outPath, QPixmap::fromImage(img));
}

void GameIconProvider::requestWikipediaSummary(const QString& gameId, const QString& title) {
  // Wikipedia REST summary expects spaces as underscores.
  QString page = title;
  if (page.isEmpty()) return;
  page.replace(' ', '_');
  const auto enc = QString::fromLatin1(QUrl::toPercentEncoding(page));
  const QUrl url(QString("https://en.wikipedia.org/api/rest_v1/page/summary/%1").arg(enc));

  QNetworkRequest req(url);
  req.setHeader(QNetworkRequest::UserAgentHeader, "ASTra/0.3 (Qt)");

  auto* reply = m_net->get(req);
  connect(reply, &QNetworkReply::finished, this, [this, reply, gameId, title]() {
    const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto bytes = reply->readAll();
    reply->deleteLater();

    if (code < 200 || code >= 300) return;

    const auto doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject()) return;
    const auto obj = doc.object();
    const auto thumb = obj.value("thumbnail");
    if (!thumb.isObject()) return;
    const auto tObj = thumb.toObject();
    const auto src = tObj.value("source").toString();
    if (src.isEmpty()) return;

    requestImage(gameId, title, QUrl(src));
  });
}

void GameIconProvider::requestImage(const QString& gameId, const QString& title, const QUrl& imageUrl) {
  if (!imageUrl.isValid()) return;

  QNetworkRequest req(imageUrl);
  req.setHeader(QNetworkRequest::UserAgentHeader, "ASTra/0.3 (Qt)");
  auto* reply = m_net->get(req);

  connect(reply, &QNetworkReply::finished, this, [this, reply, gameId, title]() {
    const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto bytes = reply->readAll();
    reply->deleteLater();

    if (code < 200 || code >= 300) return;

    QImage img;
    if (!img.loadFromData(bytes)) return;

    const auto pix = scaleAndCropToTile(img);
    if (pix.isNull()) return;

    const auto path = cachePathForTitle(title);
    pix.save(path, "PNG");
    emit iconAvailable(gameId, path, pix);
  });
}

QPixmap GameIconProvider::scaleAndCropToTile(const QImage& img) {
  // NOTE: despite the historical name, we now do "contain" (letterbox) instead of crop,
  // so the entire cover is visible while keeping a consistent 320x176 tile size.
  if (img.isNull()) return {};
  const auto target = iconSize();

  // Scale to fit entirely within the tile (no cropping).
  const QImage scaled = img.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);

  // Letterbox onto a fixed-size canvas (transparent; the tile view background shows through).
  QImage canvas(target, QImage::Format_ARGB32_Premultiplied);
  canvas.fill(Qt::transparent);

  QPainter p(&canvas);
  p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  const int x = (target.width() - scaled.width()) / 2;
  const int y = (target.height() - scaled.height()) / 2;
  p.drawImage(x, y, scaled);
  p.end();

  return QPixmap::fromImage(canvas);
}

} // namespace gf::gui

