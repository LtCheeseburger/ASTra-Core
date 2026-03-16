#include "GamePlatform.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace gf::gui {

static QString joinPath(const QString& a, const QString& b) { return QDir(a).filePath(b); }
static bool fileExists(const QString& p) { return QFileInfo::exists(p) && QFileInfo(p).isFile(); }
static bool dirExists(const QString& p) { return QFileInfo::exists(p) && QFileInfo(p).isDir(); }

static QString readTextFilePrefix(const QString& p, int maxBytes = 4096) {
  QFile f(p);
  if (!f.open(QIODevice::ReadOnly)) return {};
  return QString::fromUtf8(f.read(maxBytes));
}

static QString extractPs2TitleIdFromSystemCnf(const QString& systemCnfPath) {
  const auto text = readTextFilePrefix(systemCnfPath);
  if (text.isEmpty()) return {};

  // Common line: BOOT2 = cdrom0:\SLUS_212.22;1
  QRegularExpression re(R"((S[CL]US[_-]?\d{3}\.\d{2}))", QRegularExpression::CaseInsensitiveOption);
  auto m = re.match(text);
  if (!m.hasMatch()) return {};
  QString id = m.captured(1).toUpper();
  // Normalize to SLUS123.45 style (some show SLUS_123.45)
  id.remove('_');
  id.remove('-');
  return id;
}

static QString findFirstFileByExtRecursive(const QString& root, const QString& extLower) {
  QDir d(root);
  const auto entries = d.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
  for (const auto& e : entries) {
    if (e.isDir()) {
      const auto hit = findFirstFileByExtRecursive(e.absoluteFilePath(), extLower);
      if (!hit.isEmpty()) return hit;
    } else {
      if (e.suffix().toLower() == extLower) return e.absoluteFilePath();
    }
  }
  return {};
}

PlatformDetectResult detect_platform_from_root(const QString& rootPath) {
  PlatformDetectResult r;
  r.platformId = platform_id::kUnknown;

  // PS3: PS3_GAME/PARAM.SFO
  {
    const auto ps3GameDir = joinPath(rootPath, "PS3_GAME");
    const auto sfo = joinPath(ps3GameDir, "PARAM.SFO");
    if (dirExists(ps3GameDir) && fileExists(sfo)) {
      r.platformId = platform_id::kPS3;
      r.primaryMetaPath = sfo;
      // Prefer ICON0.PNG if present
      const auto icon0a = joinPath(ps3GameDir, "ICON0.PNG");
      const auto icon0b = joinPath(ps3GameDir, "ICON0.png");
      if (fileExists(icon0a)) r.iconHintPath = icon0a;
      else if (fileExists(icon0b)) r.iconHintPath = icon0b;
      return r;
    }
  }

  // PS4: Sc0/param.sfo (and icon0.png)
  {
    const auto sc0 = joinPath(rootPath, "Sc0");
    const auto sfo = joinPath(sc0, "param.sfo");
    if (dirExists(sc0) && fileExists(sfo)) {
      r.platformId = platform_id::kPS4;
      r.primaryMetaPath = sfo;
      const auto icon0 = joinPath(sc0, "icon0.png");
      if (fileExists(icon0)) r.iconHintPath = icon0;
      return r;
    }
  }

  // PSVita: sce_sys/param.sfo
  {
    const auto sceSys = joinPath(rootPath, "sce_sys");
    const auto sfo = joinPath(sceSys, "param.sfo");
    if (dirExists(sceSys) && fileExists(sfo)) {
      r.platformId = platform_id::kPSVita;
      r.primaryMetaPath = sfo;
      const auto icon0 = joinPath(sceSys, "icon0.png");
      if (fileExists(icon0)) r.iconHintPath = icon0;
      return r;
    }
  }

  // PSP: PSP_GAME/PARAM.SFO
  {
    const auto pspGame = joinPath(rootPath, "PSP_GAME");
    const auto sfo = joinPath(pspGame, "PARAM.SFO");
    if (dirExists(pspGame) && fileExists(sfo)) {
      r.platformId = platform_id::kPSP;
      r.primaryMetaPath = sfo;
      const auto icon0 = joinPath(pspGame, "ICON0.PNG");
      if (fileExists(icon0)) r.iconHintPath = icon0;
      return r;
    }
  }

  // PS2: SYSTEM.CNF
  {
    const auto systemCnf = joinPath(rootPath, "SYSTEM.CNF");
    if (fileExists(systemCnf)) {
      r.platformId = platform_id::kPS2;
      r.primaryMetaPath = systemCnf;
      r.titleId = extractPs2TitleIdFromSystemCnf(systemCnf);
      return r;
    }
  }

  // WiiU: code/*.rpx (e.g. code/madden.rpx)
  {
    const auto codeDir = joinPath(rootPath, "code");
    if (dirExists(codeDir)) {
      const auto rpx = findFirstFileByExtRecursive(codeDir, "rpx");
      if (!rpx.isEmpty()) {
        r.platformId = platform_id::kWiiU;
        r.primaryMetaPath = rpx;
        return r;
      }
    }
  }

  // Wii: .rso
  {
    const auto rso = findFirstFileByExtRecursive(rootPath, "rso");
    if (!rso.isEmpty()) {
      r.platformId = platform_id::kWii;
      r.primaryMetaPath = rso;
      return r;
    }
  }

  // GameCube: opening.bnr
  {
    const auto opening = joinPath(rootPath, "opening.bnr");
    if (fileExists(opening)) {
      r.platformId = platform_id::kGameCube;
      r.primaryMetaPath = opening;
      return r;
    }
  }

  // Xbox 360: *.xex (commonly default.xex)
  {
    const auto xex = findFirstFileByExtRecursive(rootPath, "xex");
    if (!xex.isEmpty()) {
      r.platformId = platform_id::kXbox360;
      r.primaryMetaPath = xex;
      return r;
    }
  }

  // OG Xbox: *.xbe
  {
    const auto xbe = findFirstFileByExtRecursive(rootPath, "xbe");
    if (!xbe.isEmpty()) {
      r.platformId = platform_id::kXbox;
      r.primaryMetaPath = xbe;
      return r;
    }
  }

  return r;
}

QString platform_badge_resource(const QString& platformId) {
  const auto p = platformId.trimmed().toLower();
  if (p == platform_id::kPS2) return ":/platform/ps2.png";
  if (p == platform_id::kPS3) return ":/platform/ps3.png";
  if (p == platform_id::kPS4) return ":/platform/ps4.png";
  if (p == platform_id::kPSVita) return ":/platform/psvita.png";
  if (p == platform_id::kPSP) return ":/platform/psp.png";
  if (p == platform_id::kXbox360) return ":/platform/xbox360.png";
  if (p == platform_id::kXbox) return ":/platform/xbox.png";
  if (p == platform_id::kGameCube) return ":/platform/gamecube.png";
  if (p == platform_id::kWii) return ":/platform/wii.png";
  if (p == platform_id::kWiiU) return ":/platform/wiiu.png";
  return ":/platform/ps3.png";
}

} // namespace gf::gui
