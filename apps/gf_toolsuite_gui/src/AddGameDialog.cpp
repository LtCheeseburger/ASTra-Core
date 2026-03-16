#include "AddGameDialog.hpp"

#include "GameLibrary.hpp"
#include "GameIconProvider.hpp"
#include "GamePlatform.hpp"
#include "gf/core/log.hpp"
#include "gf/platform/ps3/param_sfo.hpp"

#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QMessageBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace gf::gui {

static QString joinPath(const QString& a, const QString& b) { return QDir(a).filePath(b); }
static bool fileExists(const QString& p) { return QFileInfo::exists(p) && QFileInfo(p).isFile(); }

// Stable ID used for persistence/deduping. Must not depend on runtime pointer values.
static QString makeStableId(const QString& platform, const QString& rootPath, const QString& titleId) {
  const auto absRoot = QDir(rootPath).absolutePath();
  const QByteArray data = (platform + "|" + titleId + "|" + absRoot).toUtf8();
  const auto hex = QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex();
  return platform + "_" + QString::fromLatin1(hex.left(12));
}

static QString prettyPlatformName(const QString& platformId) {
  const QString p = platformId.trimmed().toLower();
  if (p == "ps2") return "PS2";
  if (p == "ps3") return "PS3";
  if (p == "ps4") return "PS4";
  if (p == "psvita" || p == "vita") return "PS Vita";
  if (p == "psp") return "PSP";
  if (p == "xbox360" || p == "x360") return "Xbox 360";
  if (p == "xbox" || p == "ogxbox") return "Xbox";
  if (p == "gamecube" || p == "gc") return "GameCube";
  if (p == "wii") return "Wii";
  if (p == "wiiu") return "Wii U";
  return platformId;
}

static bool needsTitlePrompt(const QString& title) {
  const QString t = title.trimmed();
  if (t.isEmpty()) return true;
  if (t.compare("unknown", Qt::CaseInsensitive) == 0) return true;
  return false;
}

static bool promptForGameTitle(QWidget* parent, GameEntry& g) {
  const QString plat = prettyPlatformName(g.platform);
  const QString hint = QString(
    "ASTra Core couldn't determine this game's title automatically (%1).\n\n"
    "Enter a name for the game. ASTra Core will use it to download the correct cover art and for display in the library.")
      .arg(plat);

  bool ok = false;
  const QString def = g.title.trimmed().isEmpty() ? QFileInfo(g.rootPath).fileName() : g.title.trimmed();
  const QString text = QInputDialog::getText(parent, "Game Title", hint, QLineEdit::Normal, def, &ok);
  if (!ok) return false;
  const QString cleaned = text.trimmed();
  if (cleaned.isEmpty()) return false;
  g.title = cleaned;
  return true;
}

static void rebuildDisplayName(GameEntry& g) {
  const QString plat = prettyPlatformName(g.platform);
  if (!g.titleId.isEmpty()) {
    g.displayName = QString("%1 (%2, %3)").arg(g.title, plat, g.titleId);
  } else {
    g.displayName = QString("%1 (%2)").arg(g.title, plat);
  }
}


// Prompt the user for optional patch/update locations for a game entry.
// For now we store only an update directory path; later we can expand to multi-patch ASTs.
static void maybePromptForPatchAndUpdate(QWidget* parent, GameEntry& g) {
  // Ask for an optional update directory (can be skipped).
  const auto msg = QString("Optional: Select an update/patch directory for this game.\n\n"
                           "If you have a title update folder or a patch ASTs folder, select it now.\n"
                           "You can skip this and set it later.");

  auto r = QMessageBox::question(parent, "Update/Patch Directory", msg,
                                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (r != QMessageBox::Yes) return;

  const auto dir = QFileDialog::getExistingDirectory(parent, "Select Update/Patch Directory", g.rootPath);
  if (dir.isEmpty()) return;
  g.updateDirPath = QDir::toNativeSeparators(dir);
}

static bool dirExists(const QString& p) { return QFileInfo::exists(p) && QFileInfo(p).isDir(); }

static QString findIcon0(const QString& ps3GameDir) {
  const QString p1 = joinPath(ps3GameDir, "ICON0.PNG");
  if (fileExists(p1)) return p1;
  const QString p2 = joinPath(ps3GameDir, "ICON0.png");
  if (fileExists(p2)) return p2;

  QDir d(ps3GameDir);
  const auto files = d.entryInfoList(QDir::Files);
  for (const auto& fi : files) {
    if (fi.fileName().startsWith("icon0.", Qt::CaseInsensitive))
      return fi.absoluteFilePath();
  }
  return {};
}

// Xbox 360: title often appears as ASCII bytes separated by 0x00 bytes
static QString extractTitleFromNxeart(const QString& nxeartPath) {
  QFile f(nxeartPath);
  if (!f.open(QIODevice::ReadOnly)) return {};
  const auto bytes = f.readAll();
  if (bytes.size() < 64) return {};

  auto isPrintable = [](unsigned char c) {
    return (c >= 0x20 && c <= 0x7E) || c == 0xAE; // include ®
  };

  auto score = [](const QString& s) {
    int sc = s.size();
    if (s.contains("NCAA", Qt::CaseInsensitive)) sc += 60;
    if (s.contains("Football", Qt::CaseInsensitive)) sc += 45;
    if (s.contains("Madden", Qt::CaseInsensitive)) sc += 45;
    if (s.contains("NFL", Qt::CaseInsensitive)) sc += 20;
    return sc;
  };

  QString best, cur;
  for (int i = 0; i + 1 < bytes.size(); ++i) {
    const unsigned char b0 = (unsigned char)bytes[i];
    const unsigned char b1 = (unsigned char)bytes[i + 1];

    if (b1 == 0x00 && isPrintable(b0)) {
      cur.append(QChar(b0));
      i += 1;
    } else {
      if (cur.size() >= 6 && score(cur) > score(best)) best = cur;
      cur.clear();
    }
  }
  if (cur.size() >= 6 && score(cur) > score(best)) best = cur;

  // normalize whitespace
  best = best.trimmed();
  best.replace(QRegularExpression("\\s+"), " ");
  return best;
}

static QString normalizeForMatch(QString t) {
  t.remove(QChar(0x00AE)); // ®
  t = t.toUpper();
  // Convert non-alnum to spaces, collapse
  t.replace(QRegularExpression("[^A-Z0-9]+"), " ");
  t = t.trimmed();
  t.replace(QRegularExpression("\\s+"), " ");
  return t;
}

static QString xboxIconKeyFromTitle(const QString& title) {
  const auto t = normalizeForMatch(title);

  auto year2 = [&](const QString& s) -> QString {
    // Find last 2-digit token (08..25) or 4-digit 20xx
    const auto parts = s.split(' ', Qt::SkipEmptyParts);
    for (int i = parts.size() - 1; i >= 0; --i) {
      const auto p = parts[i];
      if (p.size() == 4 && p.startsWith("20")) return p.mid(2, 2);
      if (p.size() == 2 && p.toInt() >= 0) return p;
      if (p.size() == 2 && p == "08") return "08";
    }
    return {};
  };

  const auto yy = year2(t);

  if (t.contains("NCAA") && t.contains("FOOTBALL")) {
    // Try to locate a year:
    // - "NCAA Football 2014" or "NCAA Football 14" -> NCAAFB14
    // - "NCAA Football 2008" or "NCAA Football 08" -> NCAAFB08
    {
            // Use raw string literal so backslashes are preserved for the regex engine.
            QRegularExpression re4(R"(\b(19|20)\d\d\b)");
      auto m4 = re4.match(t);
      if (m4.hasMatch()) {
        const QString year = m4.captured(0);
        const int yy = year.right(2).toInt();
        return QString("NCAAFB%1").arg(yy, 2, 10, QChar('0'));
      }
    }
    // 2-digit fallback (very common in 360 title strings)
    // Prefer the last 2-digit number in the string (e.g. "... 14", "... 08")
        QRegularExpression re2(R"(\b(\d{2})\b)");
    QRegularExpressionMatchIterator it = re2.globalMatch(t);
    QString last;
    while (it.hasNext()) {
      last = it.next().captured(1);
    }
    if (!last.isEmpty()) {
      const int yy = last.toInt();
      return QString("NCAAFB%1").arg(yy, 2, 10, QChar('0'));
    }
  }

  if (t.contains("MADDEN") && t.contains("NFL")) {
    if (!yy.isEmpty()) return QString("Madden%1").arg(yy); // Madden11
    if (t.contains("11")) return "Madden11";
    return "UNKNOWN";
  }

  if (t.contains("HEAD") && t.contains("COACH")) return "NFLHC09";

  return "UNKNOWN";
}

AddGameDialog::AddGameDialog(GameLibrary* lib, QWidget* parent)
  : QDialog(parent), m_lib(lib) {

  setWindowTitle("Add Game");
  setModal(true);

  auto* outer = new QVBoxLayout(this);

  m_hint = new QLabel(
    "Select a game folder. ASTra Core will auto-detect the platform:\n"
    "• PS2: SYSTEM.CNF\n"
    "• PS3: PS3_GAME/PARAM.SFO\n"
    "• PS4: Sc0/param.sfo\n"
    "• PSVita: sce_sys/param.sfo\n"
    "• PSP: PSP_GAME/PARAM.SFO\n"
    "• Xbox 360: *.xex (default.xex)\n"
    "• OG Xbox: *.xbe\n"
    "• GameCube: opening.bnr\n"
    "• Wii: *.rso\n"
    "• WiiU: code/*.rpx\n", this);
  m_hint->setWordWrap(true);

  auto* row = new QHBoxLayout();
  m_rootPath = new QLineEdit(this);
  m_rootPath->setPlaceholderText("Select game root folder...");
  m_browse = new QPushButton("Browse...", this);
  connect(m_browse, &QPushButton::clicked, this, &AddGameDialog::onBrowse);

  row->addWidget(m_rootPath, 1);
  row->addWidget(m_browse);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &AddGameDialog::onAccept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  outer->addWidget(m_hint);
  outer->addLayout(row);
  outer->addWidget(buttons);

  resize(640, 180);
}

QString AddGameDialog::pickRootFolder() {
  return QFileDialog::getExistingDirectory(this, "Select Game Folder");
}

void AddGameDialog::onBrowse() {
  const auto dir = pickRootFolder();
  if (!dir.isEmpty()) m_rootPath->setText(dir);
}

bool AddGameDialog::detectPs3Game(const QString& root, GameEntry* outGame, QString* outErr) const {
  const auto ps3GameDir = joinPath(root, "PS3_GAME");
  const auto sfoPath = joinPath(ps3GameDir, "PARAM.SFO");

  if (!dirExists(ps3GameDir) || !fileExists(sfoPath)) {
    if (outErr) *outErr = "Not a PS3 folder (missing PS3_GAME/PARAM.SFO).";
    return false;
  }

  std::string err;
  const auto sfoOpt = gf::platform::ps3::read_param_sfo(sfoPath.toStdString(), &err);
  if (!sfoOpt) {
    if (outErr) *outErr = err.empty() ? "Failed to read PARAM.SFO." : QString::fromStdString(err);
    return false;
  }

  const auto& sfo = *sfoOpt;
  auto getStr = [&](const char* key) -> QString {
    auto it = sfo.strings.find(key);
    if (it == sfo.strings.end()) return {};
    return QString::fromStdString(it->second);
  };

  const auto title = getStr("TITLE");
  const auto titleId = getStr("TITLE_ID");

  GameEntry g;
  g.platform = "ps3";
  g.rootPath = root;
  g.title = title;
  g.titleId = titleId;
  rebuildDisplayName(g);

  g.iconPath = findIcon0(ps3GameDir);

  // fallback for missing ICON0 (use key icon)
  g.boxArtKey = xboxIconKeyFromTitle(title);

  const auto usrdir = joinPath(ps3GameDir, "USRDIR");
  g.usrdirPath = usrdir;
  if (dirExists(usrdir)) {
    QDir d(usrdir);
    const auto files = d.entryInfoList(QStringList() << "*.ast" << "*.AST", QDir::Files);
    for (const auto& fi : files) g.mainAstPaths.push_back(fi.absoluteFilePath());
  }

  *outGame = g;
  return true;
}

bool AddGameDialog::detectPs4Game(const QString& root, GameEntry* outGame, QString* outErr) const {
  // Typical extracted PS4 folder layout:
  //   <root>/Image0  (game data)
  //   <root>/Sc0     (param.sfo, icon0.png)
  const QDir d(root);
  const QString image0 = d.filePath("Image0");
  const QString sc0 = d.filePath("Sc0");
  const QString sfoPath = QDir(sc0).filePath("param.sfo");
  if (!QDir(image0).exists() || !QDir(sc0).exists() || !QFileInfo::exists(sfoPath)) {
    return false;
  }

  const auto sfoOpt = gf::platform::ps3::read_param_sfo(sfoPath.toStdString());
  if (!sfoOpt) {
    if (outErr) *outErr = "Failed to parse PS4 param.sfo";
    return false;
  }
  const auto tiOpt = gf::platform::ps3::extract_title_and_id(*sfoOpt);
  if (!tiOpt) {
    if (outErr) *outErr = "PS4 param.sfo parsed, but failed to extract title/title_id.";
    return false;
  }

  GameEntry g;
  g.platform = "ps4";
  g.rootPath = root;
  g.image0Path = image0;
  g.sc0Path = sc0;

  // PS4 uses TITLE and TITLE_ID (CUSAxxxxx)
  g.title = QString::fromStdString(tiOpt->title);
  g.titleId = QString::fromStdString(tiOpt->title_id);
  if (g.title.isEmpty()) g.title = "Unknown";
  // If titleId is empty, still allow add.

  // icon is typically Sc0/icon0.png
  const QString iconPng = QDir(sc0).filePath("icon0.png");
  if (QFileInfo::exists(iconPng)) g.iconPath = iconPng;
  g.displayName = QString("%1 (PS4%2)")
                      .arg(g.title)
                      .arg(g.titleId.isEmpty() ? QString() : QString(", %1").arg(g.titleId));
  // We'll rebuild displayName again at the end of the add flow if the user overrides the title.

  // Index ASTs without parsing: scan Image0 for *.ast (case-insensitive)
  const QDir imageDir(image0);
  const QStringList asts = imageDir.entryList({"*.ast", "*.AST"}, QDir::Files, QDir::Name);
  for (const auto& f : asts) g.mainAstPaths.push_back(imageDir.filePath(f));

  g.id = makeStableId(g.platform, g.rootPath, g.titleId);
  *outGame = g;
  return true;
}

bool AddGameDialog::detectPsVitaGame(const QString& root, GameEntry* outGame, QString* outErr) const {
  // PSVita VPK/ux0 dump layout typically contains sce_sys/param.sfo
  const QString param = QDir(root).filePath("sce_sys/param.sfo");
  if (!QFileInfo::exists(param)) {
    if (outErr) *outErr = "Not a PSVita title root (missing sce_sys/param.sfo).";
    return false;
  }

  GameEntry g;
  // PSVita: generate a stable, deterministic ID so rescans don't duplicate entries.
  const QString pseudoTitleId = QFileInfo(root).fileName();
  g.id = makeStableId("psvita", root, pseudoTitleId);
  g.platform = "psvita";
  g.rootPath = root;
  g.title = QFileInfo(root).fileName();
  g.titleId = "";
  // Scan for AST/BGFA archives in the title root (cheap + matches common dumps).
  {
    QDir dir(root);
    dir.setFilter(QDir::Files);
    dir.setNameFilters(QStringList() << "*.AST" << "*.ast" << "*.BGFA" << "*.bgfa");
    const auto files = dir.entryInfoList();
    for (const auto& fi : files) {
      g.mainAstPaths.push_back(fi.absoluteFilePath());
    }
  }

  g.astCount = g.mainAstPaths.size();
  g.hasAst = (g.astCount > 0);
  g.scanRoot = root;
  g.lastScanAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

  if (g.hasAst) {
    g.containerType = "AST";
    g.containerCount = g.astCount;
  } else {
    // Fallback: detect a primary container family.
    QDir dir(root);
    dir.setFilter(QDir::Files);
    const auto files = dir.entryInfoList();
    int bigCount = 0;
    int terfCount = 0;
    for (const auto& fi : files) {
      const QString ext = fi.suffix().toLower();
      if (ext == "big") bigCount++;
      if (ext == "terf") terfCount++;
    }
    if (terfCount > 0) {
      g.containerType = "TERF";
      g.containerCount = terfCount;
    } else if (bigCount > 0) {
      g.containerType = "BIG";
      g.containerCount = bigCount;
    } else {
      g.containerType = "Unknown";
      g.containerCount = 0;
    }
  }
  // g.iconPath is optional (custom cover image); if empty, we use built-in art / URL art.

  *outGame = g;
  return true;
}

bool AddGameDialog::detectPspGame(const QString& root, GameEntry* outGame, QString* outErr) const {
  if (!outGame) return false;

  GameEntry g;
  g.platform = "psp";
  g.rootPath = root;
  // We'll compute a stable ID after we extract the title ID from PARAM.SFO.
  g.titleId = "";

  const QDir rootDir(root);
  const QString pspGame = rootDir.filePath("PSP_GAME");
  const QString paramSfo = QDir(pspGame).filePath("PARAM.SFO");

  // PSP uses the same PARAM.SFO container format as PS3.
  std::string errStd;
  const auto sfoOpt = gf::platform::ps3::read_param_sfo(paramSfo.toStdString(), &errStd);
  if (!sfoOpt.has_value()) {
    if (outErr) {
      *outErr = QString("Failed to read PSP PARAM.SFO: %1").arg(QString::fromStdString(errStd));
    }
    return false;
  }

  const auto metaOpt = gf::platform::ps3::extract_title_and_id(*sfoOpt);
  if (metaOpt.has_value()) {
    g.title = QString::fromStdString(metaOpt->title).trimmed();
    g.titleId = QString::fromStdString(metaOpt->title_id).trimmed();
  }

  // Stable identity: platform + install root + title id (when available).
  g.id = makeStableId(g.platform, root, g.titleId);

  if (g.title.isEmpty()) g.title = QFileInfo(root).fileName();

  // Prefer ICON0.PNG if present.
  const QString icon0 = QDir(pspGame).filePath("ICON0.PNG");
  if (QFileInfo::exists(icon0)) {
    g.iconPath = icon0;
  } else {
    // No built-in PSP art yet; fall back to a neutral placeholder.
    g.boxArtKey = "UNKNOWN";
  }

  const QString usrdir = QDir(pspGame).filePath("USRDIR");
  if (QFileInfo(usrdir).isDir()) {
    g.scanRoot = usrdir;
  } else {
    g.scanRoot = root;
  }

  // PSP commonly stores TERF containers under PSP_GAME/USRDIR/data.
  // We still scan from USRDIR so VIVs in sibling folders are detected too.

  *outGame = g;
  return true;
}

bool AddGameDialog::detectXbox360Game(const QString& root, GameEntry* outGame, QString* outErr) const {
  const auto xex = joinPath(root, "default.xex");
  const auto nxeart = joinPath(root, "nxeart");

  if (!fileExists(xex)) {
    if (outErr) *outErr = "Not an Xbox 360 folder (missing default.xex).";
    return false;
  }

  const auto title = fileExists(nxeart) ? extractTitleFromNxeart(nxeart) : QString();
  // Some rips may have a missing/stripped nxeart title string. We'll still add the game
  // and prompt the user for a friendly name in the main add flow.

  GameEntry g;
  g.platform = "xbox360";
  g.rootPath = root;
  g.title = title.isEmpty() ? QString("Unknown") : title;
  g.titleId = "";
  rebuildDisplayName(g);

  // Xbox 360 games commonly store ASTs at the game root (alongside default.xex).
  {
    QDir dir(root);
    dir.setFilter(QDir::Files);
    dir.setNameFilters(QStringList() << "*.AST" << "*.ast");
    const auto files = dir.entryInfoList();
    for (const auto& fi : files) g.mainAstPaths.push_back(fi.absoluteFilePath());
  }

  g.iconPath.clear();
  g.boxArtKey = xboxIconKeyFromTitle(title);

  *outGame = g;
  return true;
}

void AddGameDialog::onAccept() {
  QString root = m_rootPath->text().trimmed();
  if (root.isEmpty()) {
    QMessageBox::warning(this, "ASTra Core", "Please select a folder.");
    return;
  }
  if (!dirExists(root)) {
    QMessageBox::warning(this, "ASTra Core", "That folder does not exist.");
    return;
  }

  // ---- Soft guardrails / heuristics (non-blocking) ----
  auto offerAutoFix = [&](const QString& title, const QString& body, const QString& suggestedRoot) -> bool {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(title);
    box.setText(body);
    box.setInformativeText(QString("Suggested root:\n%1").arg(QDir::toNativeSeparators(suggestedRoot)));
    auto* yes = box.addButton("Use suggested", QMessageBox::YesRole);
    auto* no  = box.addButton("Keep selected", QMessageBox::NoRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == yes) {
      root = QDir(suggestedRoot).absolutePath();
      return true;
    }
    if (box.clickedButton() == no) {
      return true;
    }
    return false;
  };

  // PS3: user sometimes selects PS3_GAME/USRDIR instead of the game root.
  {
    const QDir d(root);
    if (d.dirName().compare("USRDIR", Qt::CaseInsensitive) == 0) {
      const QString parent = d.absoluteFilePath("..");
      const QString grand = d.absoluteFilePath("../..");
      if (QDir(parent).dirName().compare("PS3_GAME", Qt::CaseInsensitive) == 0 &&
          QFileInfo(QDir(parent).filePath("PARAM.SFO")).exists()) {
        if (!offerAutoFix(
              "Folder Selection Hint",
              "It looks like you selected PS3_GAME/USRDIR.\n\n"
              "ASTra Core expects the *game root* folder (the folder containing PS3_GAME).",
              grand)) {
          return;
        }
      }
    }
  }

  // PSP: user sometimes selects PSP_GAME or PSP_GAME/USRDIR instead of the PSP root.
  {
    const QDir d(root);
    const QString name = d.dirName();
    if (name.compare("USRDIR", Qt::CaseInsensitive) == 0) {
      const QString parent = d.absoluteFilePath("..");
      const QString grand = d.absoluteFilePath("../..");
      if (QDir(parent).dirName().compare("PSP_GAME", Qt::CaseInsensitive) == 0 &&
          QFileInfo(QDir(parent).filePath("PARAM.SFO")).exists()) {
        if (!offerAutoFix(
              "Folder Selection Hint",
              "It looks like you selected PSP_GAME/USRDIR.\n\n"
              "ASTra Core expects the *PSP root* folder (the folder containing PSP_GAME).",
              grand)) {
          return;
        }
      }
    } else if (name.compare("PSP_GAME", Qt::CaseInsensitive) == 0) {
      if (QFileInfo(d.filePath("PARAM.SFO")).exists()) {
        const QString parent = d.absoluteFilePath("..");
        if (!offerAutoFix(
              "Folder Selection Hint",
              "It looks like you selected the PSP_GAME folder.\n\n"
              "ASTra Core expects the *PSP root* folder (the folder containing PSP_GAME).",
              parent)) {
          return;
        }
      }
    }
  }

  const auto det = detect_platform_from_root(root);

  // Guardrail: if the folder doesn't look like any supported game root, warn once.
  if (det.platformId == platform_id::kUnknown) {
    const auto ret = QMessageBox::warning(
      this,
      "Folder Selection Warning",
      "This folder does not look like a recognized game root.\n\n"
      "Expected examples:\n"
      "• PS3: <root>/PS3_GAME/PARAM.SFO\n"
      "• PSP: <root>/PSP_GAME/PARAM.SFO\n"
      "• PS4: <root>/Sc0/param.sfo\n"
      "• PSVita: <root>/sce_sys/param.sfo\n"
      "• Xbox 360: *.xex\n"
      "\nYou can still add it for now, but scanning/detection may be incomplete.",
      QMessageBox::Ok | QMessageBox::Cancel,
      QMessageBox::Ok);
    if (ret != QMessageBox::Ok) return;
  }

  GameEntry g;
  QString err;
  // For now ASTra Core only opens BGFA/AST based games, but we persist all platforms
  // so the library can grow later without changing the save format again.
  if (det.platformId == platform_id::kPS3) {
    if (!detectPs3Game(root, &g, &err)) {
      QMessageBox::warning(this, "ASTra Core", err.isEmpty() ? "Failed to detect PS3 game." : err);
      return;
    }
  } else if (det.platformId == platform_id::kPS4) {
    if (!detectPs4Game(root, &g, &err)) {
      QMessageBox::warning(this, "ASTra Core", err.isEmpty() ? "Failed to detect PS4 game." : err);
      return;
    }
  } else if (det.platformId == platform_id::kPSVita) {
    if (!detectPsVitaGame(root, &g, &err)) {
      QMessageBox::warning(this, "ASTra Core", err.isEmpty() ? "Failed to detect PSVita game." : err);
      return;
    }
  } else if (det.platformId == platform_id::kPSP) {
    if (!detectPspGame(root, &g, &err)) {
      QMessageBox::warning(this, "ASTra Core", err.isEmpty() ? "Failed to detect PSP game." : err);
      return;
    }
  } else if (det.platformId == platform_id::kXbox360) {
    if (!detectXbox360Game(root, &g, &err)) {
      QMessageBox::warning(this, "ASTra Core", err.isEmpty() ? "Failed to detect Xbox 360 game." : err);
      return;
    }
  } else {
    // Persist entry for unsupported platforms (framework only).
    g.platform = det.platformId;
    g.rootPath = QDir::toNativeSeparators(root);
    g.titleId = det.titleId;
    g.title = det.displayTitle.isEmpty() ? QFileInfo(root).fileName() : det.displayTitle;
    rebuildDisplayName(g);
    g.iconPath = det.iconHintPath; // may be empty; library view can fetch online art.
    g.boxArtKey = "UNKNOWN";
    g.id = makeStableId(g.platform, g.rootPath, g.titleId);
  }

  // Soft warning: Vita ASTs are typically at the title root.
  if (g.platform == "psvita" && g.astCount == 0 && g.mainAstPaths.isEmpty()) {
    QMessageBox::information(
      this,
      "PSVita Hint",
      "No AST/BGFA archives were found at the PSVita title root.\n\n"
      "If you selected a subfolder, please select the title root folder (the folder containing sce_sys/param.sfo)."
    );
  }

  // If we couldn't confidently determine a proper title, ask the user once.
  // The provided title is used for display AND for online cover art fetching.
  if (needsTitlePrompt(g.title)) {
    if (!promptForGameTitle(this, g)) {
      // User cancelled or provided an empty title.
      return;
    }
  }

  // Ensure displayName is consistent after any overrides.
  rebuildDisplayName(g);

  // Recompute simple art key fallback (used only if online art isn't available).
  if (g.boxArtKey.isEmpty() || g.boxArtKey == "UNKNOWN") {
    g.boxArtKey = xboxIconKeyFromTitle(g.title);
  }

  // Ask once per game add flow for optional patch/update content.
  maybePromptForPatchAndUpdate(this, g);


  // Cache scan metadata for instant startup (no rescans unless requested).
  g.astCount = g.mainAstPaths.size() + g.patchAstPaths.size();
  g.hasAst = (g.astCount > 0);
  if (g.lastScanAt.isEmpty()) {
    g.lastScanAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  }
  if (g.scanRoot.isEmpty()) {
    if (g.platform == "ps3") g.scanRoot = g.usrdirPath;
    else if (g.platform == "ps4") g.scanRoot = g.image0Path;
    else if (g.platform == "wiiu") g.scanRoot = QDir(g.rootPath).filePath("content");
    else g.scanRoot = g.rootPath;
  }


  QString saveErr;
  if (!m_lib->addOrReplace(g, &saveErr)) {
    QMessageBox::warning(this, "ASTra Core", saveErr.isEmpty() ? "Failed to save game library." : saveErr);
    return;
  }

  m_result = g;
  m_accepted = true;
  accept();
}

} // namespace gf::gui
