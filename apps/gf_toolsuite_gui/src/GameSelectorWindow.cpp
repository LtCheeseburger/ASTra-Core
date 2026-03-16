#include "GameSelectorWindow.hpp"
#include "MainWindow.hpp"

#include "AddGameDialog.hpp"
#include "GameLibrary.hpp"
#include "GameIconProvider.hpp"
#include "GamePlatform.hpp"
#include "PlatformUtils.hpp"
#include "GuiSettings.hpp"
#include "gf/models/scan_result.hpp"
#include "gf/core/log.hpp"
#include "gf/core/features.hpp"
#include "gf/core/zip_writer.hpp"
#include "gf/core/scan.hpp"
#include "gf/core/AstArchive.hpp"
#include "gf_core/version.hpp"

#include <gf/core/AstRootResolver.hpp>

#include <filesystem>
#include <algorithm>
#include <climits>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QCheckBox>
#include <QDir>
#include <QHBoxLayout>
#include <QComboBox>
#include <QAbstractItemView>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QMenu>
#include <QDesktopServices>
#include <QFileDialog>
#include <QUrl>
#include <QPainter>
#include <QMouseEvent>
#include <QPushButton>
#include <QStatusBar>
#include <QJsonObject>
#include <QDateTime>
#include <nlohmann/json.hpp>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QSysInfo>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QSplitter>
#include <QTextBrowser>
#include <QFileDialog>
#include <QDateTime>
#include <nlohmann/json.hpp>
#include <QInputDialog>
#include <QLineEdit>
#include <QElapsedTimer>
#include <QDirIterator>
#include <QSet>
#include <QVBoxLayout>
#include <QRegularExpression>

namespace gf::gui {


static int extractTitleYear(const QString& title) {
  QRegularExpression re4(R"(\b((?:19|20)\d\d)\b)");
  auto m4 = re4.match(title);
  if (m4.hasMatch()) return m4.captured(1).toInt();

  QRegularExpression re2(R"(\b(\d{2})\b)");
  QRegularExpressionMatchIterator it = re2.globalMatch(title);
  QString last;
  while (it.hasNext()) last = it.next().captured(1);
  if (!last.isEmpty()) {
    const int yy = last.toInt();
    if (yy <= 35) return 2000 + yy;
    return 1900 + yy;
  }
  return 0;
}

static QString detectFranchise(const gf::gui::GameEntry& g) {
  const QString hay = (g.title + " " + g.displayName).toUpper();
  if (hay.contains("NCAA") && hay.contains("FOOTBALL")) return "NCAA Football";
  if (hay.contains("MADDEN")) return "Madden NFL";
  if (hay.contains("NBA LIVE")) return "NBA Live";
  if (hay.contains("NBA")) return "NBA";
  if (hay.contains("NHL")) return "NHL";
  if (hay.contains("FIFA")) return "FIFA";
  if (hay.contains("TIGER WOODS") || hay.contains("PGA")) return "Golf";
  if (hay.contains("FIGHT NIGHT")) return "Fight Night";
  if (hay.contains("NFL STREET")) return "NFL Street";
  return "Other";
}

static QString normalizedPlatformLabel(const QString& platform) {
  const QString p = platform.trimmed().toLower();
  if (p == "ps2") return "PS2";
  if (p == "ps3") return "PS3";
  if (p == "ps4") return "PS4";
  if (p == "psvita") return "PS Vita";
  if (p == "psp") return "PSP";
  if (p == "xbox") return "Xbox";
  if (p == "xbox360" || p == "x360") return "Xbox 360";
  if (p == "wii") return "Wii";
  if (p == "wiiu") return "Wii U";
  if (p == "gamecube") return "GameCube";
  return platform;
}

static QPair<QString, int> detectPrimaryContainerTypeNonRecursive(const QString& root) {
  // If a game doesn't have AST/BGFA archives, surface the primary container family.
  // (We intentionally keep this cheap: non-recursive scan of root only.)
  if (root.isEmpty() || !QDir(root).exists()) return {"Unknown", 0};

  int terfCount = 0;
  int bigCount = 0;

  QDirIterator it(root, QDir::Files);
  while (it.hasNext()) {
    it.next();
    const QString ext = QFileInfo(it.filePath()).suffix().toLower();
    if (ext == "terf") ++terfCount;
    else if (ext == "big") ++bigCount;
  }

  if (terfCount > 0) return {"TERF", terfCount};
  if (bigCount > 0) return {"BIG", bigCount};
  return {"Unknown", 0};
}

struct ContainerSniffCounts {
  int big_count = 0;
  int terf_count = 0;
  int unknown_count = 0;
  int files_examined = 0;
  int folders_scanned = 0;
  QString primary_type = "Unknown";
};


static QStringList findFilesByExtension(const QString& root, const QSet<QString>& extsLower, int maxResults) {
  QStringList out;
  if (root.isEmpty()) return out;
  QDir d(root);
  if (!d.exists()) return out;

  QDirIterator it(root,
                  QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                  QDirIterator::Subdirectories);

  while (it.hasNext() && out.size() < maxResults) {
    const QString p = it.next();
    const QString ext = QFileInfo(p).suffix().toLower();
    if (extsLower.contains(ext)) out.push_back(QDir::toNativeSeparators(p));
  }
  return out;
}

static bool looksLikePlainText(const QByteArray& bytes) {
  if (bytes.isEmpty()) return false;

  // Skip UTF-8 BOM if present.
  int i = 0;
  if (bytes.size() >= 3 &&
      static_cast<unsigned char>(bytes[0]) == 0xEF &&
      static_cast<unsigned char>(bytes[1]) == 0xBB &&
      static_cast<unsigned char>(bytes[2]) == 0xBF) {
    i = 3;
  }

  int printable = 0;
  int total = 0;
  for (; i < bytes.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(bytes[i]);
    // Treat NUL as binary.
    if (c == 0x00) return false;
    ++total;
    if (c == '\n' || c == '\r' || c == '\t') { ++printable; continue; }
    if (c >= 0x20 && c <= 0x7E) { ++printable; continue; }
    // Allow common UTF-8 continuation bytes (heuristic).
    if (c >= 0x80) { ++printable; continue; }
  }
  if (total <= 0) return false;
  const double ratio = static_cast<double>(printable) / static_cast<double>(total);
  return ratio >= 0.90;
}

static bool looksLikeXmlOrMarkup(const QByteArray& bytes) {
  if (bytes.isEmpty()) return false;
  QByteArray b = bytes;
  // Trim leading whitespace
  int i = 0;
  // Skip UTF-8 BOM
  if (b.size() >= 3 &&
      static_cast<unsigned char>(b[0]) == 0xEF &&
      static_cast<unsigned char>(b[1]) == 0xBB &&
      static_cast<unsigned char>(b[2]) == 0xBF) {
    i = 3;
  }
  while (i < b.size()) {
    const char c = b[i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { ++i; continue; }
    break;
  }
  const QByteArray head = b.mid(i, 16);
  return head.startsWith("<?xml") || head.startsWith("<");
}

static QStringList findTextFilesBySniffing(const QString& root,
                                           const QSet<QString>& alreadyIncluded,
                                           int maxResults) {
  QStringList out;
  if (root.isEmpty()) return out;
  QDir d(root);
  if (!d.exists()) return out;

  QDirIterator it(root,
                  QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                  QDirIterator::Subdirectories);

  while (it.hasNext() && out.size() < maxResults) {
    const QString p = it.next();
    const QString native = QDir::toNativeSeparators(p);
    if (alreadyIncluded.contains(native)) continue;

    QFileInfo fi(p);
    // Avoid huge files (most large binaries aren't "plain text").
    if (fi.size() > 512 * 1024) continue;

    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) continue;
    const QByteArray head = f.read(4096);
    if (!looksLikePlainText(head)) continue;

    // If it looks like markup/xml, include it as text candidate anyway.
    out.push_back(native);
  }

  return out;
}


struct SniffedContainer {
  QString type;   // "BIG", "BIG4", "TERF", "Unknown"
  QString path;
};

static QVector<SniffedContainer> findContainersByHeader(const QString& root, int maxFiles, int maxResults) {
  QVector<SniffedContainer> out;
  if (root.isEmpty()) return out;
  QDir d(root);
  if (!d.exists()) return out;

  int filesExamined = 0;
  QDirIterator it(root,
                  QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                  QDirIterator::Subdirectories);

  while (it.hasNext() && filesExamined < maxFiles && out.size() < maxResults) {
    const QString p = it.next();
    const QString ext = QFileInfo(p).suffix().toLower();
    // Skip obviously-large media to keep this fast.
    static const QSet<QString> kSkip = {"png","jpg","jpeg","dds","tga","bmp","webp","mp3","wav","ogg","mp4","mkv","avi","exe","dll","xbe","xex","elf","self","sprx","prx"};
    if (kSkip.contains(ext)) continue;

    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) continue;
    const QByteArray head = f.read(4);
    if (head.size() < 4) continue;
    filesExamined++;

    QString type = "Unknown";
    if (head == "TERF") type = "TERF";
    else if (head == "BIG4") type = "BIG4";
    else if (head == "BIGF" || head == "BIG " || head == "BIG") type = "BIG";

    if (type != "Unknown") {
      out.push_back({type, QDir::toNativeSeparators(p)});
    }
  }
  return out;
}

static ContainerSniffCounts sniffContainersByHeader(const QString& root, bool recursive, int maxFiles) {
  ContainerSniffCounts out;
  if (root.isEmpty()) return out;

  auto shouldSkip = [](const QString& path) -> bool {
    const QString lower = QFileInfo(path).suffix().toLower();
    // Skip obvious executables / metadata to avoid wasting the header sniff budget.
    static const QSet<QString> kSkip = {"exe","dll","xbe","xex","elf","self","sprx","prx","bin","ini","cfg","txt","log","xml","json","png","jpg","jpeg","dds","tga","bmp","webp"};
    return kSkip.contains(lower);
  };

  QSet<QString> folders;

  auto scanOneRoot = [&](const QString& scanRoot, bool doRecursive) {
    if (scanRoot.isEmpty()) return;
    QDir d(scanRoot);
    if (!d.exists()) return;

    folders.insert(d.absolutePath());

    QDirIterator it(scanRoot,
                    QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                    doRecursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);

    while (it.hasNext() && out.files_examined < maxFiles) {
      const QString p = it.next();

      if (shouldSkip(p)) continue;

      QFile f(p);
      if (!f.open(QIODevice::ReadOnly)) continue;
      const QByteArray head = f.read(4);
      if (head.size() < 4) continue;

      out.files_examined++;

      if (head == "BIG4" || head == "BIGF" || head == "BIG ") {
        out.big_count++;
      } else if (head == "TERF") {
        out.terf_count++;
      } else {
        out.unknown_count++;
      }

      folders.insert(QFileInfo(p).absoluteDir().absolutePath());
    }
  };

  // Normal scan for this platform (recursive or not).
  scanOneRoot(QDir(root).absolutePath(), recursive);

  // TERF containers typically live in a DATA folder on older titles (e.g., original Xbox).
  // Even when the main scan is non-recursive, we still peek into DATA/ so Format detection isn't "Unknown".
  if (!recursive) {
    const QString dataDir = QDir(root).filePath("DATA");
    const QString dataDirLower = QDir(root).filePath("data");
    if (QDir(dataDir).exists()) {
      scanOneRoot(dataDir, /*doRecursive=*/true);
    } else if (QDir(dataDirLower).exists()) {
      scanOneRoot(dataDirLower, /*doRecursive=*/true);
    }
  }

  out.folders_scanned = folders.size();

  if (out.big_count > 0 || out.terf_count > 0) {
    out.primary_type = (out.big_count >= out.terf_count) ? "BIG" : "TERF";
  } else {
    out.primary_type = "Unknown";
  }
  return out;
}


static QPixmap composeTilePixmap(const GameEntry& e, const QPixmap& base) {
  // Overlay platform badge at TOP-RIGHT of the artwork
  QPixmap out = base;
  QPainter p(&out);
  p.setRenderHint(QPainter::Antialiasing, true);

  // Be robust to older library entries / manual edits.
  const QString plat = e.platform.trimmed().toLower();
  QPixmap badge(platform_badge_resource(plat));
  if (!badge.isNull()) {
    // Platform badges vary wildly in aspect ratio (PlayStation wordmarks are wide).
    // Fit into a consistent corner box so they don't look oversized or misaligned.
    const int pad = qMax(6, out.width() / 50);

    // Platform-specific corner box sizing (tuned for 320x176 tiles).
    const QString pkey = plat;
    double wFrac = 0.18;
    double hFrac = 0.14;

    if (pkey == "ps2" || pkey == "ps3" || pkey == "ps4" || pkey == "psp" || pkey == "psvita") {
      wFrac = 0.18; // wide wordmarks need more width
      hFrac = 0.14;
    } else if (pkey == "xbox" || pkey == "xbox360") {
      wFrac = 0.15;
      hFrac = 0.15;
    } else if (pkey == "wii" || pkey == "wiiu") {
      wFrac = 0.13;
      hFrac = 0.15;
    } else if (pkey == "gamecube") {
      wFrac = 0.14;
      hFrac = 0.15;
    }

    const int maxW = qMax(34, int(out.width() * wFrac));
    const int maxH = qMax(16, int(out.height() * hFrac));
    badge = badge.scaled(maxW, maxH, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    const int x = out.width() - badge.width() - pad;
    const int y = pad;

    // White "chip" behind the badge so it's readable on dark/light boxart.
    const int chipPad = qMax(3, out.width() / 120);
    QRect chipRect(x - chipPad, y - chipPad, badge.width() + chipPad * 2, badge.height() + chipPad * 2);

    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 235));
    p.drawRoundedRect(chipRect, 4, 4);
    p.restore();

    p.drawPixmap(x, y, badge);
  }

  p.end();
  return out;
}

GameSelectorWindow::GameSelectorWindow(QWidget* parent)
  : QMainWindow(parent),
    m_library(new GameLibrary()),
    m_icons(new GameIconProvider(this)) {

  setWindowTitle(QString("ASTra Core - Game Library"));
  setMinimumSize(1100, 650);

  buildUi();
  refresh();
}

void GameSelectorWindow::buildUi() {
  auto* central = new QWidget(this);
  setCentralWidget(central);

  auto* outer = new QVBoxLayout(central);
  outer->setContentsMargins(12, 12, 12, 12);
  outer->setSpacing(10);

  auto* toolsMenu = menuBar()->addMenu("Tools");
  auto* actRescanAll = toolsMenu->addAction("Rescan All Games");
  connect(actRescanAll, &QAction::triggered, this, [this]() {
    // Rescan all games and persist updated metadata.
    for (const auto& g : m_library->games()) {
      if (g.id.isEmpty()) continue;
      // Reuse the same rescan logic as the per-game context menu.
      rescanGameEntry(g.id, /*quiet=*/true);
    }
    refresh();
  });

  auto* actOpenOverrides = toolsMenu->addAction("Open Title Overrides File");
  connect(actOpenOverrides, &QAction::triggered, this, [this]() {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const auto path = QDir(base).filePath("title_overrides.json");
    // Ensure file exists (create an empty JSON object on first open).
    if (!QFileInfo::exists(path)) {
      QSaveFile f(path);
      if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(QJsonObject()).toJson(QJsonDocument::Indented));
        f.commit();
      }
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
  });

  toolsMenu->addSeparator();
  auto* actExportLibrary = toolsMenu->addAction("Export Game Library (JSON)...");
  connect(actExportLibrary, &QAction::triggered, this, [this]() { exportLibraryJson(); });
  auto* actImportLibrary = toolsMenu->addAction("Import Game Library (JSON)...");
  connect(actImportLibrary, &QAction::triggered, this, [this]() { importLibraryJson(); });

  // Beta testing: quick access to the same diagnostics bundle export available in the Diagnostics module.
  m_actExportDiagnostics = toolsMenu->addAction("Export Selected Game Diagnostics (zip)...");
  m_actExportDiagnostics->setEnabled(false);
  connect(m_actExportDiagnostics, &QAction::triggered, this, [this]() { exportDiagnosticsBundle(); });

  auto* helpMenu = menuBar()->addMenu("Help");
  auto* actBetaGuide = helpMenu->addAction("Beta Testing Guide");
  connect(actBetaGuide, &QAction::triggered, this, &GameSelectorWindow::onBetaTestingGuide);
  helpMenu->addSeparator();

  auto* about = new QAction("About ASTra Core", this);
  connect(about, &QAction::triggered, this, &GameSelectorWindow::onAbout);
  helpMenu->addAction(about);

  // v0.7.1: view/sort/filter controls for larger libraries.
  auto* controls = new QHBoxLayout();
  controls->setContentsMargins(0, 0, 0, 0);
  controls->setSpacing(8);

  controls->addWidget(new QLabel("View:", central));
  m_viewMode = new QComboBox(central);
  m_viewMode->addItems({"Grid", "List"});
  controls->addWidget(m_viewMode);

  controls->addWidget(new QLabel("Sort:", central));
  m_sortMode = new QComboBox(central);
  m_sortMode->addItems({"Title (A-Z)", "Title Year (Newest)", "Title Year (Oldest)", "Platform", "Recently Added", "Recently Opened"});
  controls->addWidget(m_sortMode);

  controls->addWidget(new QLabel("Platform:", central));
  m_platformFilter = new QComboBox(central);
  m_platformFilter->addItems({"All Platforms", "PS2", "PS3", "PS4", "PS Vita", "PSP", "Xbox", "Xbox 360", "Wii", "Wii U", "GameCube"});
  controls->addWidget(m_platformFilter);

  controls->addWidget(new QLabel("Franchise:", central));
  m_franchiseFilter = new QComboBox(central);
  m_franchiseFilter->addItems({"All Franchises", "NCAA Football", "Madden NFL", "NBA Live", "NBA", "NHL", "FIFA", "Golf", "Fight Night", "NFL Street", "Other"});
  controls->addWidget(m_franchiseFilter);

  m_search = new QLineEdit(central);
  m_search->setPlaceholderText("Search title, platform, year...");
  controls->addWidget(m_search, 1);

  outer->addLayout(controls);

  // v0.7.0 RC cleanup: remove the mostly-unused module shell sidebar and let
  // the game library occupy the full window.
  auto* libraryPane = new QWidget(central);
  auto* libraryLayout = new QVBoxLayout(libraryPane);
  libraryLayout->setContentsMargins(0, 0, 0, 0);
  libraryLayout->setSpacing(10);
  outer->addWidget(libraryPane, /*stretch=*/1);

  m_list = new QListWidget(libraryPane);
  m_list->setViewMode(QListView::IconMode);
  m_list->setMovement(QListView::Static);
  m_list->setResizeMode(QListView::Adjust);
  m_list->setUniformItemSizes(true);
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);
  m_list->setSpacing(20);
  m_list->setIconSize(GameIconProvider::iconSize());
  m_list->setContextMenuPolicy(Qt::CustomContextMenu);
  // v0.5.3: clearer selection highlight
  m_list->setStyleSheet("QListWidget::item:selected{border:2px solid #6f42c1; border-radius:10px;}");


  // Clear selection when clicking empty space (fixes "sticky highlight" UX bug).
  m_list->viewport()->installEventFilter(this);

  connect(m_viewMode, &QComboBox::currentTextChanged, this, [this](const QString&) { refresh(); });
  connect(m_sortMode, &QComboBox::currentTextChanged, this, [this](const QString&) { refresh(); });
  connect(m_platformFilter, &QComboBox::currentTextChanged, this, [this](const QString&) { refresh(); });
  connect(m_franchiseFilter, &QComboBox::currentTextChanged, this, [this](const QString&) { refresh(); });
  connect(m_search, &QLineEdit::textChanged, this, [this](const QString&) { refresh(); });

  // Async cover art downloads.
  connect(m_icons, &GameIconProvider::iconFetchStarted, this, [this](const QString& gameId) {
    for (int i = 0; i < m_list->count(); ++i) {
      auto* it = m_list->item(i);
      if (!it) continue;
      if (it->data(Qt::UserRole).toString() == gameId) {
        it->setData(Qt::UserRole + 1, true);
        // Append a small status line while downloading.
        const QString base = it->data(Qt::UserRole + 2).toString();
        if (!base.isEmpty()) it->setText(base + "\nDownloading cover...");
        break;
      }
    }
  });
  connect(m_icons, &GameIconProvider::iconAvailable, this, [this](const QString& gameId, const QString& path, const QPixmap& pix) {
    // Update list item if it's visible.
    for (int i = 0; i < m_list->count(); ++i) {
      auto* it = m_list->item(i);
      if (!it) continue;
      if (it->data(Qt::UserRole).toString() == gameId) {
        it->setIcon(QIcon(composeTilePixmap(m_library->findById(gameId).value_or(GameEntry{}), pix)));
        it->setData(Qt::UserRole + 1, false);
        const QString base = it->data(Qt::UserRole + 2).toString();
        if (!base.isEmpty()) it->setText(base);
        break;
      }
    }
    // Persist cached icon path.
    QString err;
    m_library->setIconPath(gameId, path, &err);
  });
  connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    auto* item = m_list->itemAt(pos);
    if (!item) return;
    const auto id = item->data(Qt::UserRole).toString();
    auto gOpt = m_library->findById(id);
    if (!gOpt.has_value()) return;
    const auto& g = gOpt.value();

    QMenu menu(this);

    auto* actOpen = menu.addAction("Open Game Files");
    auto* actRescan = menu.addAction("Rescan");
    menu.addSeparator();
    auto* actEditTitle = menu.addAction("Edit Title");
    auto* actCoverUrl = menu.addAction("Change Cover (URL)...");
    auto* actCoverFile = menu.addAction("Change Cover (File)...");
    menu.addSeparator();
    auto* actReveal = menu.addAction(gf::gui::revealMenuLabel());
    auto* actRemove = menu.addAction("Remove Game");

    actOpen->setEnabled(true);
    actRescan->setEnabled(true);
    actEditTitle->setEnabled(true);
    actCoverUrl->setEnabled(true);
    actReveal->setEnabled(!g.rootPath.isEmpty());
    actRemove->setEnabled(true);

    auto* chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    m_list->setCurrentItem(item);

    if (chosen == actOpen) {
      onOpenGameFiles();
    } else if (chosen == actRescan) {
      rescanGameEntry(id, /*quiet=*/false);
      refresh();
    } else if (chosen == actEditTitle) {
      bool ok = false;
      const QString current = g.title.isEmpty() ? g.displayName : g.title;
      const QString t = QInputDialog::getText(this, "Edit Title", "Game title:", QLineEdit::Normal, current, &ok).trimmed();
      if (ok && !t.isEmpty()) {
        // Update title + displayName, and let icon resolver pick a stable query title.
        GameEntry gg = g;
        gg.title = t;
        // Keep platform/id suffix in displayName
        // (match existing behavior by rebuilding a simple suffix)
        QString suffix;
        if (!gg.platform.isEmpty()) suffix += gg.platform.toUpper();
        if (!gg.titleId.isEmpty()) suffix += (suffix.isEmpty() ? "" : ", ") + gg.titleId;
        gg.displayName = suffix.isEmpty() ? t : QString("%1 (%2)").arg(t, suffix);
        QString err;
        m_library->setTitle(id, gg.title, gg.displayName, &err);
        if (!err.isEmpty()) QMessageBox::warning(this, "ASTra Core", err);
        refresh();
      }
    } else if (chosen == actCoverUrl) {
      bool ok = false;
      const QString u = QInputDialog::getText(this, "Cover URL", "Paste an image URL (PNG/JPG/WebP):", QLineEdit::Normal, "", &ok).trimmed();
      if (ok && !u.isEmpty()) {
        const QUrl url(u);
        if (!url.isValid()) {
          QMessageBox::warning(this, "ASTra Core", "Invalid URL.");
        } else {
          m_icons->requestFromUrl(id, url);
          // iconAvailable signal will persist iconPath; refresh updates tile.
        }
      }
    } else if (chosen == actCoverFile) {
      const QString p = QFileDialog::getOpenFileName(this, "Select Cover Image", QString(), "Images (*.png *.jpg *.jpeg *.webp *.bmp)");
      if (!p.isEmpty()) {
        m_icons->requestFromLocalFile(id, p);
        // iconAvailable signal will persist iconPath; refresh updates tile.
      }
    } else if (chosen == actReveal) {
      if (!g.rootPath.isEmpty()) gf::gui::revealInFileManager(g.rootPath);
    } else if (chosen == actRemove) {
      onRemoveGame();
    }
  });

  // Double-click opens the selected game's files.
  connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
    onOpenGameFiles();
  });

  m_emptyState = new QLabel("No games added. Click Add Game to add a game.", libraryPane);
  static_cast<QLabel*>(m_emptyState)->setAlignment(Qt::AlignCenter);

  m_status = new QLabel("", libraryPane);
  m_status->setMinimumHeight(18);
  m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
  // Module sidebar removed in v0.7.0 RC: ASTra Core now opens directly into the
  // game library, and module-specific work happens in the main editor window.

  // Bottom bar (single row)
  auto* bottom = new QHBoxLayout();
  bottom->setContentsMargins(0, 0, 0, 0);

  m_btnExit = new QPushButton("Exit", libraryPane);
  connect(m_btnExit, &QPushButton::clicked, this, &QWidget::close);

  m_btnOpenAstEditor = new QPushButton("Open AST Editor", libraryPane);
  connect(m_btnOpenAstEditor, &QPushButton::clicked, this, &GameSelectorWindow::onOpenAstEditor);

  m_btnAddGame = new QPushButton("Add Game", libraryPane);
  connect(m_btnAddGame, &QPushButton::clicked, this, &GameSelectorWindow::onAddGame);

  m_btnRemoveGame = new QPushButton("Remove Game", libraryPane);
  m_btnRemoveGame->setEnabled(false);
  connect(m_btnRemoveGame, &QPushButton::clicked, this, &GameSelectorWindow::onRemoveGame);

  m_btnOpenGameFiles = new QPushButton("Open Game Files", libraryPane);
  m_btnOpenGameFiles->setEnabled(false);
  connect(m_btnOpenGameFiles, &QPushButton::clicked, this, &GameSelectorWindow::onOpenGameFiles);

  bottom->addWidget(m_btnExit);
  bottom->addWidget(m_btnOpenAstEditor);
  bottom->addStretch(1);

  // Bottom-right cluster: Add, Remove, Open
  bottom->addWidget(m_btnAddGame);
  bottom->addWidget(m_btnRemoveGame);
  bottom->addWidget(m_btnOpenGameFiles);

  libraryLayout->addWidget(m_list, 1);
  libraryLayout->addWidget(m_emptyState, 1);
  libraryLayout->addWidget(m_status);
  libraryLayout->addLayout(bottom);

  statusBar()->showMessage("Ready");
}

QString GameSelectorWindow::selectedGameId() const {
  const auto items = m_list->selectedItems();
  if (items.isEmpty()) return {};
  return items.first()->data(Qt::UserRole).toString();
}


void GameSelectorWindow::rescanGameEntry(const QString& gameId, bool quiet) {
  QElapsedTimer timer;
  timer.start();
  const auto opt = m_library->findById(gameId);
  if (!opt.has_value()) return;
  auto g = opt.value();

  QVector<QString> main;
  QString scanRoot;

  auto scanNonRecursive = [&](const QString& dirPath) {
    QDir dir(dirPath);
    dir.setFilter(QDir::Files);
    dir.setNameFilters(QStringList() << "*.AST" << "*.ast" << "*.BGFA" << "*.bgfa");
    const auto files = dir.entryInfoList();
    for (const auto& fi : files) main.push_back(fi.absoluteFilePath());
  };

  if (g.platform == "ps3") {
    scanRoot = !g.usrdirPath.isEmpty() ? g.usrdirPath : QDir(g.rootPath).filePath("PS3_GAME/USRDIR");
    if (QDir(scanRoot).exists()) scanNonRecursive(scanRoot);
  } else if (g.platform == "ps4") {
    scanRoot = !g.image0Path.isEmpty() ? g.image0Path : QDir(g.rootPath).filePath("Image0");
    if (QDir(scanRoot).exists()) scanNonRecursive(scanRoot);
  } else if (g.platform == "xbox360") {
    scanRoot = g.rootPath;
    if (QDir(scanRoot).exists()) scanNonRecursive(scanRoot);
  } else if (g.platform == "psvita") {
    // PS Vita dumps typically place archives at the game root (next to eboot.bin)
    scanRoot = g.rootPath;
    if (QDir(scanRoot).exists()) scanNonRecursive(scanRoot);
  } else if (g.platform == "psp") {
    // PSP dumps store most data under PSP_GAME/USRDIR (TERF often under PSP_GAME/USRDIR/data).
    scanRoot = QDir(g.rootPath).filePath("PSP_GAME/USRDIR");
    if (!QDir(scanRoot).exists()) scanRoot = g.rootPath;
    if (QDir(scanRoot).exists()) scanNonRecursive(scanRoot);
  } else if (g.platform == "wiiu") {
    scanRoot = QDir(g.rootPath).filePath("content");
    if (QDir(scanRoot).exists()) {
      QDirIterator it(scanRoot, QStringList() << "*.ast" << "*.AST", QDir::Files, QDirIterator::Subdirectories);
      while (it.hasNext()) main.push_back(it.next());
    }
  } else {
    // Unknown / unsupported platforms: no AST scan.
    scanRoot = g.rootPath;
  }

  // Keep existing patch paths that still exist.
  QVector<QString> patch;
  for (const auto& p : g.patchAstPaths) {
    if (QFileInfo::exists(p)) patch.push_back(p);
  }

const int astCount = main.size() + patch.size();
const bool hasAst = astCount > 0;

// Container type is only relevant when no ASTs were found.
ContainerSniffCounts sniff;
QString containerType;
int containerCount = 0;
int bigCount = 0;
int terfCount = 0;
int filesExamined = 0;
int foldersScanned = 0;

if (hasAst) {
  containerType = "AST";
  containerCount = astCount;
} else {
  // Header-sniff (not extension-based): BIG/TERF/Unknown.
  const bool recursiveSniff = (g.platform == "wiiu" || g.platform == "psp"); // Wii U/PSP live in subfolders; Vita remains root-only elsewhere
  sniff = sniffContainersByHeader(scanRoot, recursiveSniff, /*maxFiles=*/50000);

  bigCount = sniff.big_count;
  terfCount = sniff.terf_count;
  filesExamined = sniff.files_examined;
  foldersScanned = sniff.folders_scanned;

  containerType = sniff.primary_type;
  if (containerType == "BIG") containerCount = bigCount;
  else if (containerType == "TERF") containerCount = terfCount;
  else containerCount = 0;
}

const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
const qint64 scanDurationMs = timer.elapsed();

  // Debug Trace: emit a scan summary block (non-invasive; logging only).
  if (gf::core::AstArchive::debugClassificationTrace()) {
    auto log = gf::core::Log::get();
    if (log) {
      log->info("---- Scan Summary ----");
      log->info("scan_root: {}", scanRoot.toStdString());
      log->info("folders_scanned: {}", foldersScanned);
      log->info("files_examined: {}", filesExamined);
      log->info("ast_count: {}", astCount);
      log->info("big_count: {}", bigCount);
      log->info("terf_count: {}", terfCount);
      log->info("primary_container: {}", containerType.toStdString());
      log->info("----------------------");
    }
  }

  // v0.5.x foundation: build a single ScanResult model (future-friendly)
  // and adapt it to persisted cache fields.
  gf::models::scan_result sr;
  sr.platform = g.platform.toStdString();
  sr.scan_root = scanRoot.toStdString();
  sr.counts.ast = astCount;
  sr.counts.big = bigCount;
  sr.counts.terf = terfCount;
  sr.counts.unknown = 0;
  sr.files_examined = filesExamined;
  sr.folders_scanned = foldersScanned;
  sr.duration_ms = scanDurationMs;
  sr.primary_container = gf::models::container_type_from_string(containerType.toStdString());

  // Soft warnings (never blockers): common selection mistakes + platform expectations.
  {
    namespace fs = std::filesystem;
    const auto selected = fs::path(g.rootPath.toStdString());
    sr.warnings = gf::core::compute_soft_warnings(selected, sr.platform, /*has_root_ast=*/hasAst);
  }

  // Rescan diff: summary vs cached scan (informational, persisted).
  QString lastScanDiff = "No changes";
  {
    QStringList parts;
    const auto deltaPart = [&](const QString& label, int oldV, int newV) {
      if (oldV == newV) return;
      const int d = newV - oldV;
      parts << QString("%1 %2%3").arg(label).arg(d >= 0 ? "+" : "").arg(d);
    };
    deltaPart("AST", g.astCount, (int)sr.counts.ast);
    deltaPart("BIG", g.bigCount, (int)sr.counts.big);
    deltaPart("TERF", g.terfCount, (int)sr.counts.terf);
    if (QString::fromStdString(sr.scan_root) != g.scanRoot) parts << "scan root changed";
    if (QString::fromStdString(sr.platform) != g.platform) parts << "platform changed";
    const QString newPrimary = QString::fromStdString(gf::models::to_string(sr.primary_container));
    if (!g.containerType.isEmpty() && g.containerType.trimmed() != newPrimary) parts << "primary format changed";
    if (!parts.isEmpty()) lastScanDiff = parts.join(", ");
  }
  if (gf::core::AstArchive::debugClassificationTrace()) {
    auto log = gf::core::Log::get();
    if (log) log->info("scan_diff: {}", lastScanDiff.toStdString());
  }


  QString err;
  m_library->updateScanMetadata(gameId,
                                sr,
                                main,
                                patch,
                                containerType,
                                lastScanDiff,
                                now,
                                &err);
  if (!quiet) {
    if (!err.isEmpty()) {
      QMessageBox::warning(this, "ASTra Core", err);
    } else {
      if (hasAst) {
        QMessageBox::information(this, "ASTra Core", QString("Rescan complete. Found %1 AST/BGFA file(s).").arg(astCount));
      } else {
        QMessageBox::information(this, "ASTra Core",
                                 QString("Rescan complete. No AST/BGFA found. Detected format: %1 (%2 file(s)).")
                                   .arg(containerType)
                                   .arg(containerCount));
      }
    }

    // Soft warning hints (non-blocking) for common selection mistakes.
    if (g.platform == "psvita" && !hasAst) {
      QMessageBox::information(
        this,
        "ASTra Core",
        "Note: PSVita dumps typically place AST/BGFA archives at the title root (next to eboot.bin).\n"
        "No AST/BGFA were found at the expected root location."
      );
    }
  }
}


void GameSelectorWindow::updateCandidatePickers() {
  const bool hasSel = !m_activeGameId.isEmpty();
  if (!hasSel || !m_library) {
    auto clearList = [](QListWidget* w) { if (w) w->clear(); };
    clearList(m_astCandidates);
    clearList(m_rsfCandidates);
    clearList(m_texCandidates);
    clearList(m_aptCandidates);
    clearList(m_dbCandidates);
    clearList(m_textCandidates);
    auto setStatus = [](QLabel* l, const QString& t) { if (l) l->setText(t); };
    setStatus(m_astCandidatesStatus, "Select a game to populate candidates.");
    setStatus(m_rsfCandidatesStatus, "Select a game to populate candidates.");
    setStatus(m_texCandidatesStatus, "Select a game to populate candidates.");
    setStatus(m_aptCandidatesStatus, "Select a game to populate candidates.");
    setStatus(m_dbCandidatesStatus, "Select a game to populate candidates.");
    setStatus(m_textCandidatesStatus, "Select a game to populate candidates.");
    return;
  }

  const auto opt = m_library->findById(m_activeGameId);
  if (!opt.has_value()) return;
  const auto g = opt.value();

  const QString scanRoot = !g.scanRoot.isEmpty() ? g.scanRoot : g.rootPath;

  // AST candidates: use cached paths first, fallback to extension scan.
  if (m_astCandidates) {
    m_astCandidates->clear();
    QStringList asts;
    for (const auto& p : g.mainAstPaths) asts.push_back(QDir::toNativeSeparators(p));
    for (const auto& p : g.patchAstPaths) asts.push_back(QDir::toNativeSeparators(p));

    if (asts.isEmpty()) {
      asts = findFilesByExtension(scanRoot, QSet<QString>{"ast","bgfa"}, /*maxResults=*/200);
    }

    for (const auto& p : asts) m_astCandidates->addItem(p);
    if (m_astCandidatesStatus) {
      m_astCandidatesStatus->setText(QString("Found %1 AST/BGFA candidate(s).").arg(m_astCandidates->count()));
    }
  }

  // RSF candidates: scan for *.rsf and common config-like siblings.
  if (m_rsfCandidates) {
    m_rsfCandidates->clear();
    const auto rsf = findFilesByExtension(scanRoot, QSet<QString>{"rsf","bin","cfg","conf","ini"}, /*maxResults=*/200);
    for (const auto& p : rsf) m_rsfCandidates->addItem(p);

    if (m_rsfCandidatesStatus) {
      m_rsfCandidatesStatus->setText(QString("Heuristic scan in %1: %2 candidate(s).")
                                         .arg(QDir::toNativeSeparators(scanRoot))
                                         .arg(m_rsfCandidates->count()));
    }
  }

  // Texture/container candidates: header-sniff for BIG/BIG4/TERF.
  if (m_texCandidates) {
    m_texCandidates->clear();
    const auto c = findContainersByHeader(scanRoot, /*maxFiles=*/800, /*maxResults=*/250);
    for (const auto& e : c) {
      m_texCandidates->addItem(QString("[%1] %2").arg(e.type, e.path));
    }
    if (m_texCandidatesStatus) {
      m_texCandidatesStatus->setText(QString("Header-sniff: %1 container candidate(s) (BIG/BIG4/TERF).").arg(m_texCandidates->count()));
    }
  }

  // APT candidates: conservative extension scan.
  if (m_aptCandidates) {
    m_aptCandidates->clear();
    const auto apt = findFilesByExtension(scanRoot, QSet<QString>{"apt","apx","apt2"}, /*maxResults=*/200);
    for (const auto& p : apt) m_aptCandidates->addItem(p);
    if (m_aptCandidatesStatus) {
      m_aptCandidatesStatus->setText(QString("Found %1 candidate(s) by extension (apt/apx/apt2).").arg(m_aptCandidates->count()));
    }
  }

  // DB candidates: conservative extension scan.
  if (m_dbCandidates) {
    m_dbCandidates->clear();
    const auto db = findFilesByExtension(scanRoot, QSet<QString>{"db","fdb","sqlite","sqlite3"}, /*maxResults=*/200);
    for (const auto& p : db) m_dbCandidates->addItem(p);
    if (m_dbCandidatesStatus) {
      m_dbCandidatesStatus->setText(QString("Found %1 candidate(s) by extension (db/fdb/sqlite).").arg(m_dbCandidates->count()));
    }
  }

  // Text candidates: common plain text / xml / config formats.
  if (m_textCandidates) {
    m_textCandidates->clear();

    const QSet<QString> exts = QSet<QString>{
        "txt","xml","cfg","conf","ini","json","yaml","yml","lua","js","css","html","htm"
    };

    const auto byExt = findFilesByExtension(scanRoot, exts, /*maxResults=*/250);

    QSet<QString> included;
    included.reserve(byExt.size() * 2);
    for (const auto& p : byExt) {
      m_textCandidates->addItem(p);
      included.insert(p);
    }

    const int remaining = std::max(0, 250 - m_textCandidates->count());
    const auto bySniff = remaining > 0 ? findTextFilesBySniffing(scanRoot, included, remaining) : QStringList{};
    for (const auto& p : bySniff) m_textCandidates->addItem(p);

    if (m_textCandidatesStatus) {
      if (!bySniff.isEmpty()) {
        m_textCandidatesStatus->setText(
            QString("Found %1 candidate(s) by extension + %2 by content sniffing (text/xml/config).")
                .arg(byExt.size())
                .arg(bySniff.size()));
      } else {
        m_textCandidatesStatus->setText(
            QString("Found %1 candidate(s) by extension (text/xml/config).").arg(byExt.size()));
      }
    }
  }
}

void GameSelectorWindow::exportDiagnosticsBundle() {
  const auto id = selectedGameId();
  if (id.isEmpty()) return;

  // Ensure on-disk library is up to date so the diagnostics bundle can include it.
  if (m_library) {
    QString saveErr;
    (void)m_library->save(&saveErr);
  }

  auto gOpt = m_library->findById(id);
  if (!gOpt.has_value()) return;
  const auto& g = gOpt.value();

  const QString suggested = QString("astra_diagnostics_%1.zip").arg(id);
  const QString outPath = QFileDialog::getSaveFileName(this, "Export Diagnostics", suggested, "Zip Files (*.zip)");
  if (outPath.isEmpty()) return;

  // Build a minimal scan_result from cached metadata (no rescan).
  gf::models::scan_result r;
  r.platform = g.platform.toStdString();
  r.scan_root = g.scanRoot.toStdString();
  r.counts.ast = g.astCount;
  r.counts.big = g.bigCount;
  r.counts.terf = g.terfCount;
  r.counts.unknown = 0;
  r.files_examined = g.filesExamined;
  r.folders_scanned = g.foldersScanned;
  r.duration_ms = g.scanDurationMs;

  const auto ct = g.containerType.trimmed().toUpper();
  if (ct == "BIG" || ct == "BIG4") r.primary_container = gf::models::container_type::big;
  else if (ct == "TERF") r.primary_container = gf::models::container_type::terf;
  else r.primary_container = gf::models::container_type::unknown;

  for (const auto& w : g.scanWarnings) r.warnings.push_back(w.toStdString());

  const auto j = gf::models::to_json(r);
  const auto jsonText = QString::fromStdString(j.dump(2));

  QString readme;
  readme += "ASTra Core Diagnostics Bundle\n";
  readme += "------------------------\n";
  readme += QString("Version: %1\n").arg(QString::fromUtf8(gf::core::kVersionString));
  readme += QString("Game ID: %1\n").arg(id);
  readme += QString("Title: %1\n").arg(g.displayName);
  readme += QString("Platform: %1\n").arg(g.platform);
  readme += QString("Root: %1\n").arg(g.rootPath);
  readme += QString("Scan Root: %1\n").arg(g.scanRoot);
  readme += QString("Last Scanned: %1\n").arg(g.lastScanAt);
  if (!g.lastScanDiff.isEmpty()) readme += QString("Last Scan Diff: %1\n").arg(g.lastScanDiff);
  readme += "\nContents:\n";
  readme += " - diagnostics/scan_result.json\n";
  readme += " - diagnostics/README.txt\n";
  readme += " - diagnostics/library_games.json\n";
  readme += " - diagnostics/settings.json\n";
  readme += " - diagnostics/environment.json\n";
  readme += " - diagnostics/astra_gui.log (best-effort)\n";

  gf::core::ZipWriter zip;
  zip.add_text("diagnostics/scan_result.json", jsonText.toStdString());
  zip.add_text("diagnostics/README.txt", readme.toStdString());

  // Include the full game library so issues can be reproduced more easily.
  // (Best-effort; diagnostics should still export even if this fails.)
  {
    const QString libPath = GameLibrary::libraryPath();
    QFile f(libPath);
    if (f.open(QIODevice::ReadOnly)) {
      const QByteArray bytes = f.readAll();
      std::vector<std::uint8_t> data;
      data.reserve(static_cast<size_t>(bytes.size()));
      data.insert(data.end(), bytes.begin(), bytes.end());
      zip.add_file("diagnostics/library_games.json", std::move(data));
    }
  }

  // Include GUI settings (best-effort). On Windows this comes from the registry, so we export a JSON snapshot.
  {
    QJsonObject root;
    root["org"] = kSettingsOrg;
    root["app"] = kSettingsApp;
    QSettings s(kSettingsOrg, kSettingsApp);
    const auto keys = s.allKeys();
    QJsonObject kv;
    for (const auto& k : keys) {
      const QVariant v = s.value(k);
      // Keep it simple: serialize common scalars, fallback to string.
      if (v.typeId() == QMetaType::Bool) kv[k] = v.toBool();
      else if (v.canConvert<double>()) kv[k] = v.toDouble();
      else kv[k] = v.toString();
    }
    root["values"] = kv;
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    zip.add_text("diagnostics/settings.json", bytes.toStdString());
  }

  // Environment + build snapshot
  {
    QJsonObject env;
    env["version"] = QString::fromUtf8(gf::core::kVersionString);
    env["qt_version"] = QString::fromUtf8(qVersion());
    env["os"] = QSysInfo::prettyProductName();
    env["kernel"] = QSysInfo::kernelType() + " " + QSysInfo::kernelVersion();
    env["cpu_arch"] = QSysInfo::currentCpuArchitecture();
    env["app_dir"] = QCoreApplication::applicationDirPath();
    env["cwd"] = QDir::currentPath();

    auto addVar = [&](const char* name) {
      const QByteArray v = qgetenv(name);
      if (!v.isEmpty()) env[name] = QString::fromUtf8(v);
    };
    addVar("ASTRA_ENABLE_BETA");
    addVar("ASTRA_XPR2_UNTILE_MODE");
    addVar("ASTRA_LOG_LEVEL");

    const QByteArray bytes = QJsonDocument(env).toJson(QJsonDocument::Indented);
    zip.add_text("diagnostics/environment.json", bytes.toStdString());
  }

  // Best-effort: include the current GUI log file if present.
  {
    const QStringList candidates = {
      QDir(QDir::currentPath()).filePath("astra_gui.log"),
      QDir(QCoreApplication::applicationDirPath()).filePath("astra_gui.log"),
      QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("astra_gui.log"),
    };
    for (const auto& p : candidates) {
      if (!QFileInfo::exists(p)) continue;
      QFile f(p);
      if (!f.open(QIODevice::ReadOnly)) continue;
      const QByteArray bytes = f.readAll();
      std::vector<std::uint8_t> data;
      data.reserve(static_cast<size_t>(bytes.size()));
      data.insert(data.end(), bytes.begin(), bytes.end());
      zip.add_file("diagnostics/astra_gui.log", std::move(data));
      break;
    }
  }

  std::string err;
  if (!zip.write_to_file(outPath.toStdString(), &err)) {
    QMessageBox::warning(this, "Export Diagnostics", QString("Failed to write zip:\n%1").arg(QString::fromStdString(err)));
    return;
  }

  statusBar()->showMessage("Diagnostics exported.", 3500);
}

void GameSelectorWindow::exportLibraryJson() {
  if (!m_library) return;

  // Ensure the on-disk library reflects current memory state.
  QString err;
  if (!m_library->save(&err)) {
    QMessageBox::warning(this, "Export Library", QString("Could not save current library: %1").arg(err));
    return;
  }

  const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
  const QString suggested = QString("astra_library_%1.json").arg(ts);
  const QString defaultDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  const QString outPath = QFileDialog::getSaveFileName(this,
                                                      "Export Game Library",
                                                      QDir(defaultDir).filePath(suggested),
                                                      "JSON (*.json)");
  if (outPath.isEmpty()) return;

  const QString srcPath = GameLibrary::libraryPath();
  QFile src(srcPath);
  if (!src.open(QIODevice::ReadOnly)) {
    QMessageBox::warning(this, "Export Library", QString("Could not read library file: %1").arg(srcPath));
    return;
  }
  const QByteArray bytes = src.readAll();

  QSaveFile out(outPath);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    QMessageBox::warning(this, "Export Library", QString("Could not write: %1").arg(outPath));
    return;
  }
  out.write(bytes);
  if (!out.commit()) {
    QMessageBox::warning(this, "Export Library", QString("Could not commit: %1").arg(outPath));
    return;
  }

  statusBar()->showMessage("Game library exported.", 3500);
}

void GameSelectorWindow::importLibraryJson() {
  if (!m_library) return;

  const QString defaultDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  const QString inPath = QFileDialog::getOpenFileName(this,
                                                     "Import Game Library",
                                                     defaultDir,
                                                     "JSON (*.json)");
  if (inPath.isEmpty()) return;

  QFile in(inPath);
  if (!in.open(QIODevice::ReadOnly)) {
    QMessageBox::warning(this, "Import Library", QString("Could not read: %1").arg(inPath));
    return;
  }
  const QByteArray bytes = in.readAll();

  // Validate JSON structure before touching the live library file.
  QJsonParseError pe{};
  const auto doc = QJsonDocument::fromJson(bytes, &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
    QMessageBox::warning(this, "Import Library", QString("Parse error: %1").arg(pe.errorString()));
    return;
  }
  const auto root = doc.object();
  if (!root.contains("games") || !root.value("games").isArray()) {
    QMessageBox::warning(this, "Import Library", "Invalid library JSON: missing 'games' array.");
    return;
  }

  const QString dstPath = GameLibrary::libraryPath();
  const QFileInfo dstInfo(dstPath);

  // Best-effort backup of the existing library for safety.
  if (dstInfo.exists()) {
    const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    const QString backupPath = QDir(dstInfo.absolutePath()).filePath(QString("games.backup_%1.json").arg(ts));
    QFile::copy(dstPath, backupPath);
  }

  QSaveFile out(dstPath);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    QMessageBox::warning(this, "Import Library", QString("Could not write library file: %1").arg(dstPath));
    return;
  }
  out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  if (!out.commit()) {
    QMessageBox::warning(this, "Import Library", QString("Could not commit library file: %1").arg(dstPath));
    return;
  }

  // Reload into memory and refresh UI.
  QString err;
  if (!m_library->load(&err)) {
    QMessageBox::warning(this, "Import Library", QString("Imported, but failed to load: %1").arg(err));
  }
  m_activeGameId.clear();
  refresh();
  statusBar()->showMessage("Game library imported.", 3500);
}



bool GameSelectorWindow::eventFilter(QObject* obj, QEvent* event) {
  if (obj == m_list->viewport()) {
    if (event->type() == QEvent::MouseButtonPress) {
      auto* me = static_cast<QMouseEvent*>(event);
      if (!m_list->itemAt(me->pos())) {
        m_list->clearSelection();
        m_list->setCurrentItem(nullptr);
      }
    }
  }
  return QMainWindow::eventFilter(obj, event);
}

void GameSelectorWindow::refresh() {
  QString err;
  if (!m_library->load(&err)) {
    gf::core::Log::get()->warn("GameLibrary load failed: {}", err.toStdString());
  }

  const QString selectedIdBefore = !selectedGameId().isEmpty() ? selectedGameId() : m_activeGameId;
  m_list->clear();

  QVector<GameEntry> games = m_library->games();

  const QString platformFilter = m_platformFilter ? m_platformFilter->currentText().trimmed() : QString();
  const QString franchiseFilter = m_franchiseFilter ? m_franchiseFilter->currentText().trimmed() : QString();
  const QString needle = m_search ? m_search->text().trimmed().toLower() : QString();

  if (!platformFilter.isEmpty() && platformFilter != "All Platforms") {
    QVector<GameEntry> filtered;
    for (const auto& g : games) {
      if (QString::compare(normalizedPlatformLabel(g.platform), platformFilter, Qt::CaseInsensitive) == 0) {
        filtered.push_back(g);
      }
    }
    games = filtered;
  }

  if (!franchiseFilter.isEmpty() && franchiseFilter != "All Franchises") {
    QVector<GameEntry> filtered;
    for (const auto& g : games) {
      if (QString::compare(detectFranchise(g), franchiseFilter, Qt::CaseInsensitive) == 0) {
        filtered.push_back(g);
      }
    }
    games = filtered;
  }

  if (!needle.isEmpty()) {
    QVector<GameEntry> filtered;
    for (const auto& g : games) {
      const int year = extractTitleYear(g.title.isEmpty() ? g.displayName : g.title);
      const QString hay = QString("%1 %2 %3 %4 %5")
                             .arg(g.title, g.displayName, normalizedPlatformLabel(g.platform), detectFranchise(g), year > 0 ? QString::number(year) : QString())
                             .toLower();
      if (hay.contains(needle)) filtered.push_back(g);
    }
    games = filtered;
  }

  const QString sortMode = m_sortMode ? m_sortMode->currentText() : QStringLiteral("Title (A-Z)");
  std::sort(games.begin(), games.end(), [&](const GameEntry& a, const GameEntry& b) {
    const QString aTitle = (a.title.isEmpty() ? a.displayName : a.title).trimmed();
    const QString bTitle = (b.title.isEmpty() ? b.displayName : b.title).trimmed();
    if (sortMode == "Title Year (Newest)") {
      const int ay = extractTitleYear(aTitle);
      const int by = extractTitleYear(bTitle);
      if (ay != by) return ay > by;
      return QString::localeAwareCompare(aTitle, bTitle) < 0;
    }
    if (sortMode == "Title Year (Oldest)") {
      const int ay = extractTitleYear(aTitle);
      const int by = extractTitleYear(bTitle);
      const int aSort = (ay == 0 ? INT_MAX : ay);
      const int bSort = (by == 0 ? INT_MAX : by);
      if (aSort != bSort) return aSort < bSort;
      return QString::localeAwareCompare(aTitle, bTitle) < 0;
    }
    if (sortMode == "Platform") {
      const QString ap = normalizedPlatformLabel(a.platform);
      const QString bp = normalizedPlatformLabel(b.platform);
      const int cmp = QString::localeAwareCompare(ap, bp);
      if (cmp != 0) return cmp < 0;
      const int fcmp = QString::localeAwareCompare(detectFranchise(a), detectFranchise(b));
      if (fcmp != 0) return fcmp < 0;
      return QString::localeAwareCompare(aTitle, bTitle) < 0;
    }
    if (sortMode == "Recently Added") return a.addedAt > b.addedAt;
    if (sortMode == "Recently Opened") return a.lastOpenedAt > b.lastOpenedAt;
    return QString::localeAwareCompare(aTitle, bTitle) < 0;
  });

  const bool listView = m_viewMode && m_viewMode->currentText() == "List";
  m_list->setViewMode(listView ? QListView::ListMode : QListView::IconMode);
  m_list->setMovement(QListView::Static);
  m_list->setWrapping(!listView);
  m_list->setUniformItemSizes(!listView);
  m_list->setWordWrap(true);
  m_list->setIconSize(listView ? QSize(72, 72) : GameIconProvider::iconSize());

  const bool hasGames = !games.isEmpty();
  m_list->setVisible(hasGames);
  m_emptyState->setVisible(!hasGames);

  for (const auto& g : games) {
    bool downloading = false;
    QPixmap pix;
    if (!g.iconPath.isEmpty() && QFileInfo::exists(g.iconPath)) pix.load(g.iconPath);
    if (pix.isNull()) {
      const QString query = g.title.isEmpty() ? g.displayName : g.title;
      const auto got = m_icons->getOrRequest(g.id, query);
      if (!got.isNull()) pix = got; else downloading = true;
    }
    if (pix.isNull()) {
      const auto art = GameLibrary::defaultBoxArtForKey(g.boxArtKey);
      pix.load(art);
    }
    if (!pix.isNull()) {
      pix = pix.scaled(m_list->iconSize(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
      const int x = qMax(0, (pix.width() - m_list->iconSize().width()) / 2);
      const int y = qMax(0, (pix.height() - m_list->iconSize().height()) / 2);
      pix = pix.copy(x, y, m_list->iconSize().width(), m_list->iconSize().height());
      pix = composeTilePixmap(g, pix);
    }

    auto* item = new QListWidgetItem();
    const int astCount = (g.astCount > 0 || g.hasAst) ? g.astCount : (g.mainAstPaths.size() + g.patchAstPaths.size());
    QString secondLine;
    if (astCount > 0 || g.hasAst) {
      secondLine = QString("ASTs: %1").arg(astCount);
    } else {
      const QString ct = g.containerType.isEmpty() ? QString("Unknown") : g.containerType;
      const QString label = (ct == "BIG" || ct == "TERF") ? ct : QString("Unknown");
      secondLine = (g.containerCount > 0)
          ? QString("Format: %1 (%2)").arg(label).arg(g.containerCount)
          : QString("Format: %1").arg(label);
    }

    const int titleYear = extractTitleYear(g.title.isEmpty() ? g.displayName : g.title);
    QString thirdLine = QString("%1 | %2").arg(normalizedPlatformLabel(g.platform), detectFranchise(g));
    if (titleYear > 0) thirdLine += QString(" | %1").arg(titleYear);

    const QString baseLabel = listView
        ? QString("%1\n%2\n%3").arg(g.displayName, secondLine, thirdLine)
        : QString("%1\n%2").arg(g.displayName, secondLine);
    item->setData(Qt::UserRole + 2, baseLabel);
    item->setData(Qt::UserRole + 1, downloading);
    item->setText(downloading ? (baseLabel + "\nDownloading cover...") : baseLabel);

    QString tip;
    tip += QString("Platform: %1\n").arg(normalizedPlatformLabel(g.platform));
    tip += QString("Franchise: %1\n").arg(detectFranchise(g));
    if (titleYear > 0) tip += QString("Year: %1\n").arg(titleYear);
    if (!g.scanRoot.isEmpty()) tip += QString("Scan root: %1\n").arg(g.scanRoot);
    if (!g.lastScanAt.isEmpty()) tip += QString("Last scanned: %1\n").arg(g.lastScanAt);
    if (g.scanDurationMs > 0) tip += QString("Scan duration: %1 ms\n").arg(g.scanDurationMs);
    if (astCount > 0 || g.hasAst) {
      tip += QString("AST/BGFA: %1\n").arg(astCount);
    } else {
      const QString ct = g.containerType.isEmpty() ? QString("Unknown") : g.containerType;
      tip += QString("Primary format: %1\n").arg(ct);
    }
    if (g.foldersScanned > 0 || g.filesExamined > 0) {
      tip += QString("Folders scanned: %1\n").arg(g.foldersScanned);
      tip += QString("Files examined: %1\n").arg(g.filesExamined);
    }
    if (!g.scanWarnings.isEmpty()) {
      tip += "Warnings:\n";
      for (const auto& w : g.scanWarnings) tip += QString("• %1\n").arg(w);
    }
    item->setToolTip(tip.trimmed());
    item->setData(Qt::UserRole, g.id);
    if (!pix.isNull()) item->setIcon(QIcon(pix));
    m_list->addItem(item);
    if (!selectedIdBefore.isEmpty() && g.id == selectedIdBefore) m_list->setCurrentItem(item);
  }

  onSelectionChanged();
}

void GameSelectorWindow::onSelectionChanged() {
  const auto id = selectedGameId();
  const bool hasSel = !id.isEmpty();
  m_btnOpenGameFiles->setEnabled(hasSel);
  m_btnRemoveGame->setEnabled(hasSel);

  if (m_actExportDiagnostics) {
    m_actExportDiagnostics->setEnabled(hasSel);
  }

  if (!m_status) return;
  if (!hasSel) {
    m_status->setText("");
    return;
  }
  const auto opt = m_library->findById(id);
  if (!opt.has_value()) {
    m_status->setText("");
    return;
  }
  const auto& g = opt.value();
  const int astCount = (g.astCount > 0 || g.hasAst) ? g.astCount : (g.mainAstPaths.size() + g.patchAstPaths.size());
  QString s = QString("%1  |  Platform: %2").arg(g.title.isEmpty() ? g.displayName : g.title, g.platform);
  if (astCount > 0 || g.hasAst) {
    s += QString("  |  ASTs: %1").arg(astCount);
  } else {
    const QString ct = g.containerType.isEmpty() ? "Unknown" : g.containerType;
    s += QString("  |  Format: %1").arg(ct);
  }
  if (!g.lastScanAt.isEmpty()) s += QString("  |  Last scan: %1").arg(g.lastScanAt);
  if (!g.scanRoot.isEmpty()) s += QString("  |  Scan root: %1").arg(g.scanRoot);

  // v0.5.2: update active game context for module shells / diagnostics.
  m_activeGameId = id;

  if (m_btnExportDiagnostics) {
    m_btnExportDiagnostics->setEnabled(hasSel);
  }
  if (m_moduleInfo) {
    if (!hasSel) {
      m_moduleInfo->setText("Select a game to see cached scan info and export a diagnostic bundle.");
    } else {
      QString html;
      html += "<b>Selected game</b><br/>";
      html += QString("Name: %1<br/>").arg(g.displayName.toHtmlEscaped());
      html += QString("Platform: %1<br/>").arg(g.platform.toHtmlEscaped());
      if (!g.scanRoot.isEmpty()) html += QString("Scan root: %1<br/>").arg(g.scanRoot.toHtmlEscaped());
      if (!g.lastScanAt.isEmpty()) html += QString("Last scanned: %1<br/>").arg(g.lastScanAt.toHtmlEscaped());
      if (!g.lastScanDiff.isEmpty()) html += QString("Last scan diff: %1<br/>").arg(g.lastScanDiff.toHtmlEscaped());
      html += "<br/><b>Containers</b><br/>";
      html += QString("AST: %1<br/>").arg(astCount);
      html += QString("BIG: %1<br/>").arg(g.bigCount);
      html += QString("TERF: %1<br/>").arg(g.terfCount);
      if (!g.containerType.isEmpty()) html += QString("Primary: %1 (%2)<br/>").arg(g.containerType.toHtmlEscaped()).arg(g.containerCount);
      if (g.scanDurationMs > 0) html += QString("Scan duration: %1 ms<br/>").arg(g.scanDurationMs);
      html += QString("Folders scanned: %1<br/>").arg(g.foldersScanned);
      html += QString("Files examined: %1<br/>").arg(g.filesExamined);

      if (!g.scanWarnings.isEmpty()) {
        html += "<br/><b>Warnings</b><ul>";
        for (const auto& w : g.scanWarnings) {
          html += QString("<li>%1</li>").arg(w.toHtmlEscaped());
        }
        html += "</ul>";
      }
      m_moduleInfo->setHtml(html);
    }
  }

  m_status->setText(s);

  updateCandidatePickers();
}


void GameSelectorWindow::onAbout() {
  QMessageBox box(this);
  box.setWindowTitle("About ASTra Core");
  const QString ver = QString::fromUtf8(gf::core::kVersionString);
  QString text;
  text += QString("ASTra Core %1\n\n").arg(ver);
  text += "Stable EA Sports archive and asset editor focused on core workflows.\n\n";
  text += "Core functionality:\n";
  text += "• Game library / game registration\n";
  text += "• AST browsing, search, extraction, replace/import, and rebuild/save\n";
  text += "• Texture preview/export/import\n";
  text += "• Text/XML/config editing\n";
  text += "• Hex/raw inspection\n";
  text += "• Minimal RSF structured table editing only\n\n";
  text += "Intentionally removed from ASTra Core:\n";
  text += "• Old RSF viewport / scene / geometry preview stack\n";
  text += "• APT viewer/editor workflows\n";
  box.setText(text);
  QPixmap logo(":/branding/astra_logo.png");
  if (!logo.isNull()) {
    box.setIconPixmap(logo.scaled(200, 200, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  }
  box.exec();
}

void GameSelectorWindow::onBetaTestingGuide() {
  const QString ver = QString::fromUtf8(gf::core::kVersionString);
  QString text;
  text += QString("ASTra Core %1 — Testing Guide\n\n").arg(ver);
  text += "What to verify:\n";
  text += "1) Add your game root via Add Game.\n";
  text += "2) Open AST Editor and verify:\n";
  text += "   • File tree populates\n";
  text += "   • Hex view loads\n";
  text += "   • Text preview works on text-like entries\n";
  text += "   • Texture preview works on DDS/P3R/XPR2 where supported\n";
  text += "   • RSF tab loads structured table data for supported RSFs\n";
  text += "3) Test core import/export flows:\n";
  text += "   • Entry extraction works\n";
  text += "   • P3R exports as .dds\n";
  text += "   • Replacing P3R with DDS still rebuilds correctly\n";
  text += "4) If something breaks, export a Diagnostics zip from Tools.\n\n";
  text += "When reporting an issue, include:\n";
  text += "• Game/platform\n";
  text += "• What you clicked (steps)\n";
  text += "• Expected vs actual\n";
  text += "• The diagnostics zip\n\n";

  QMessageBox box(this);
  box.setWindowTitle("ASTra Core Testing Guide");
  box.setText(text);
  box.setIcon(QMessageBox::Information);
  auto* copyBtn = box.addButton("Copy Template", QMessageBox::ActionRole);
  box.addButton(QMessageBox::Ok);
  box.exec();
  if (box.clickedButton() == copyBtn) {
    QString tpl;
    tpl += "[ASTra Core] Issue Report\n";
    tpl += QString("Version: %1\n").arg(ver);
    tpl += "Platform/Game: \n";
    tpl += "Steps to reproduce:\n1) \n2) \n3) \n";
    tpl += "Expected: \n";
    tpl += "Actual: \n";
    tpl += "Notes: (attach diagnostics zip)\n";
    QGuiApplication::clipboard()->setText(tpl);
  }
}

void GameSelectorWindow::onAddGame() {
  AddGameDialog dlg(m_library, this);
  if (dlg.exec() == QDialog::Accepted) {
    refresh();
  }
}

void GameSelectorWindow::onRemoveGame() {
  const auto id = selectedGameId();
  if (id.isEmpty()) return;

  QString name;
  for (const auto& g : m_library->games()) {
    if (g.id == id) { name = g.displayName; break; }
  }
  if (name.isEmpty()) name = "this game";

  const auto ret = QMessageBox::question(
    this,
    "Remove Game",
    QString("Remove %1 from the library?\n\nThis does NOT delete any game files on disk.").arg(name),
    QMessageBox::Yes | QMessageBox::No,
    QMessageBox::No
  );
  if (ret != QMessageBox::Yes) return;

  QString err;
  if (!m_library->removeById(id, &err)) {
    QMessageBox::warning(this, "ASTra Core", err.isEmpty() ? "Failed to remove game." : err);
    return;
  }
  refresh();
}

void GameSelectorWindow::onOpenGameFiles() {
  const auto id = selectedGameId();
  if (id.isEmpty()) return;
  auto gOpt = m_library->findById(id);
  if (!gOpt.has_value()) return;

  const auto& g = gOpt.value();

  // Guard: only AST/BGFA titles are supported right now.
  // (We still allow other platforms to exist in the library for future format support.)
  const int astCount = (g.astCount > 0 || g.hasAst) ? g.astCount : (g.mainAstPaths.size() + g.patchAstPaths.size());
  if (astCount <= 0) {
    QMessageBox::information(
      this,
      "ASTra Core",
      "Game is not .ast/.bgfa format (or no ASTs were found).\n\n"
      "Support for other formats (.big / .ps3 / .terf / etc.) has not been added yet."
    );
    return;
  }

  auto* mw = new MainWindow();
  mw->setMode(MainWindow::Mode::Game);
  // Base content dir depends on platform:
  // - PS3: USRDIR
  // - PS4: Image0
  // - Xbox 360: rootPath
  // Resolve a single best AST root (Qt-free core resolver).
  gf::core::AstRootResolveInput in{};
  in.gameRoot = std::filesystem::path(g.rootPath.toStdString());

  // Map GUI platform string to core enum.
  const QString plat = g.platform.trimmed().toLower();
  if (plat == "ps3") in.platform = gf::core::Platform::PS3;
  else if (plat == "xbox360" || plat == "xbox" || plat == "x360") in.platform = gf::core::Platform::Xbox360;
  else if (plat == "ps4") in.platform = gf::core::Platform::PS4;
  else if (plat == "wiiu") in.platform = gf::core::Platform::WiiU;
  else in.platform = gf::core::Platform::Unknown;

  const auto rr = gf::core::AstRootResolver::resolveBest(in);
  const QString baseDir = QString::fromStdString(rr.astRoot.string());

  mw->openGame(g.displayName, g.rootPath, baseDir, g.updateDirPath);
  mw->showMaximized();

  emit openGameRequested(id);
}


void GameSelectorWindow::onOpenAstEditor() {
  // Standalone editor (no game selected)
  auto* mw = new MainWindow();
  mw->setMode(MainWindow::Mode::Standalone);
  mw->showMaximized();
  emit openAstEditorRequested();
}


} // namespace gf::gui


void gf::gui::GameSelectorWindow::onItemDoubleClicked(QListWidgetItem*) {
  onOpenGameFiles();
}