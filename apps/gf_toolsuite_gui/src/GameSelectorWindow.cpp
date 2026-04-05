#include "GameSelectorWindow.hpp"
#include "MainWindow.hpp"

#include "AddGameDialog.hpp"
#include "GameLibrary.hpp"
#include "GameIconProvider.hpp"
#include "GamePlatform.hpp"
#include "PlatformUtils.hpp"
#include "GuiSettings.hpp"
#include "ModProfileManager.hpp"
#include "ModProfilesDialog.hpp"
#include "InstallModDialog.hpp"
#include "InstalledModsDialog.hpp"
#include "RuntimeTargetManager.hpp"
#include "BaselineCaptureService.hpp"
#include "ExportModDialog.hpp"
#include "ModMetadataEditorDialog.hpp"
#include "ModPackageExporter.hpp"
#include "ApplyPreviewDialog.hpp"
#include "ProfileApplyService.hpp"
#include "ProfileResolverService.hpp"
#include "gf/models/scan_result.hpp"
#include "gf/core/log.hpp"
#include "gf/core/features.hpp"
#include "gf/core/zip_writer.hpp"
#include "gf/core/scan.hpp"
#include "gf/core/AstArchive.hpp"
#include "gf_core/version.hpp"
#include "update/UpdateChecker.hpp"
#include "update/UpdateDialog.hpp"
#include "update/UpdaterLauncher.hpp"
#include "update/UpdaterConfig.hpp"
#include "update/VersionBadgeWidget.hpp"

#include <gf/core/AstRootResolver.hpp>

#include <filesystem>
#include <algorithm>
#include <climits>
#include <optional>

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
#include <QTimer>
#include <QFrame>
#include <QCryptographicHash>
#include <QStyledItemDelegate>

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

static QString cleanGameTitle(const GameEntry& g) {
    if (!g.title.isEmpty()) return g.title;
    const QString dn = g.displayName;
    const int i = dn.lastIndexOf(" (");
    return (i > 0) ? dn.left(i) : dn;
}

static QString gameProfileId(const QString& rootPath) {
    return QString::fromLatin1(
        QCryptographicHash::hash(rootPath.toUtf8(), QCryptographicHash::Sha1).toHex());
}

// ── GameLibraryItemDelegate ───────────────────────────────────────────────────

static QColor platformPillColor(const QString& label) {
    const QString l = label.toLower();
    if (l.startsWith("ps") || l == "psp" || l.contains("vita"))
        return QColor(0x00, 0x70, 0xD1);   // PlayStation blue
    if (l.startsWith("xbox"))
        return QColor(0x10, 0x7C, 0x10);   // Xbox green
    if (l == "wii" || l == "wii u")
        return QColor(0x00, 0x9A, 0xC7);   // Wii light blue
    if (l == "gamecube")
        return QColor(0x6A, 0x0D, 0xAD);   // GameCube purple
    return QColor(0x55, 0x55, 0x55);        // Fallback gray
}

class GameLibraryItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        // Only custom-draw in list mode (decoration on left side)
        if (option.decorationPosition != QStyleOptionViewItem::Left) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        painter->save();

        const QRect r = opt.rect;
        const bool selected = opt.state & QStyle::State_Selected;

        // Background
        if (selected) {
            painter->fillRect(r, opt.palette.highlight());
        }

        // Icon — 120×68, vertically centred, 6 px left pad
        static constexpr int kIconW = 120, kIconH = 68, kPad = 6;
        const QRect iconRect(r.left() + kPad,
                             r.top()  + (r.height() - kIconH) / 2,
                             kIconW, kIconH);

        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        if (!icon.isNull()) {
            const QPixmap pix = icon.pixmap(kIconW, kIconH);
            const int xOff = (kIconW - pix.width())  / 2;
            const int yOff = (kIconH - pix.height()) / 2;
            painter->drawPixmap(iconRect.left() + xOff, iconRect.top() + yOff, pix);
        } else {
            painter->fillRect(iconRect, QColor(0x33, 0x33, 0x33));
        }

        // Text layout constants
        static constexpr int kPillH = 18, kPillHPad = 7, kPillGap = 6;
        const int textLeft  = iconRect.right() + 10;
        const int textAvail = r.right() - textLeft - kPad;

        const QColor textColor = selected ? opt.palette.highlightedText().color()
                                          : opt.palette.text().color();
        const QColor dimColor  = selected ? textColor.lighter(140)
                                          : opt.palette.placeholderText().color();

        const QStringList lines = index.data(Qt::DisplayRole).toString().split('\n');
        const QString titleStr    = lines.value(0);
        const QString subtitleStr = lines.value(1);

        // Platform pill — immediately after title text, vertically centred in title row
        const QString platLabel = index.data(Qt::UserRole + 3).toString();
        QFont pillFont = opt.font;
        pillFont.setPointSize(qMax(7, opt.font.pointSize() - 1));
        pillFont.setBold(true);
        const QFontMetrics pillFm(pillFont);
        const int pillW = platLabel.isEmpty() ? 0
                        : (pillFm.horizontalAdvance(platLabel) + kPillHPad * 2);

        // Title takes as much space as needed, leaving room for [gap + pill]
        QFont titleFont = opt.font;
        titleFont.setBold(true);
        const QFontMetrics titleFm(titleFont);
        const int pillSlot  = platLabel.isEmpty() ? 0 : (pillW + kPillGap);
        const int maxTitleW = qMax(0, textAvail - pillSlot);
        const QString elidedTitle = titleFm.elidedText(titleStr, Qt::ElideRight, maxTitleW);
        const int actualTitleW    = qMin(titleFm.horizontalAdvance(elidedTitle), maxTitleW);

        // Title row y-centre: 12 px from top, 22 px tall
        const int titleY = r.top() + 12;
        painter->setFont(titleFont);
        painter->setPen(textColor);
        painter->drawText(QRect(textLeft, titleY, maxTitleW, 22),
                          Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                          elidedTitle);

        // Platform pill — right of title text
        if (!platLabel.isEmpty()) {
            const int pillX = textLeft + actualTitleW + kPillGap;
            // Clamp so it never overruns the row
            const int pillXClamped = qMin(pillX, r.right() - pillW - kPad);
            const QRect pillRect(pillXClamped,
                                 titleY + (22 - kPillH) / 2,
                                 pillW, kPillH);
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            painter->setBrush(platformPillColor(platLabel));
            painter->drawRoundedRect(pillRect, kPillH / 2, kPillH / 2);
            painter->setFont(pillFont);
            painter->setPen(Qt::white);
            painter->drawText(pillRect, Qt::AlignCenter, platLabel);
        }

        // Subtitle (normal weight, dimmer) — full text area width
        if (!subtitleStr.isEmpty()) {
            painter->setFont(opt.font);
            painter->setPen(dimColor);
            const QFontMetrics subFm(opt.font);
            painter->drawText(QRect(textLeft, r.top() + 38, textAvail, 18),
                              Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                              subFm.elidedText(subtitleStr, Qt::ElideRight, textAvail));
        }

        // Focus rect
        if (opt.state & QStyle::State_HasFocus) {
            QStyleOptionFocusRect fo;
            fo.QStyleOption::operator=(opt);
            fo.rect = r;
            QApplication::style()->drawPrimitive(QStyle::PE_FrameFocusRect, &fo, painter);
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        if (opt.decorationPosition != QStyleOptionViewItem::Left)
            return QStyledItemDelegate::sizeHint(opt, idx);
        return QSize(300, 82);
    }
};

// ── SidePanelProfileDelegate ──────────────────────────────────────────────────
// Compact profile row for the in-library side panel.
// UserRole+1 (bool): isActive

static QString spProfileIconPath(const QString& workspacePath) {
    return QDir(workspacePath).filePath("profile_icon.png");
}

class SidePanelProfileDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return {200, 50};
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.icon = QIcon();
        opt.text.clear();
        const QWidget* w = opt.widget;
        QStyle* style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, w);
        painter->save();

        const QRect r = option.rect;
        constexpr int kIcon   = 32;
        constexpr int kPad    = 8;
        constexpr int kGap    = 8;

        const int iconX = r.left() + kPad;
        const int iconY = r.top() + (r.height() - kIcon) / 2;
        const QRect iconRect(iconX, iconY, kIcon, kIcon);

        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        if (!icon.isNull()) {
            icon.paint(painter, iconRect, Qt::AlignCenter);
        } else {
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(140, 152, 178));
            painter->drawRoundedRect(iconRect, 5, 5);
            const QString name   = index.data(Qt::DisplayRole).toString();
            const QString letter = name.isEmpty() ? QStringLiteral("?")
                                                  : QString(name.at(0).toUpper());
            QFont lf = option.font;
            lf.setBold(true);
            lf.setPixelSize(14);
            painter->setFont(lf);
            painter->setPen(QColor(255, 255, 255, 210));
            painter->drawText(iconRect, Qt::AlignCenter, letter);
        }

        const int textX = iconX + kIcon + kGap;
        const int textW = r.right() - textX - kPad;
        const bool isActive = index.data(Qt::UserRole + 1).toBool();

        QFont nf = option.font;
        if (isActive) nf.setBold(true);
        painter->setFont(nf);
        const QPalette::ColorRole role =
            (option.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text;
        painter->setPen(option.palette.color(role));

        const QFontMetrics fm(nf);
        const QRect nameRect(textX, r.top() + 5, textW, fm.height() + 2);
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          fm.elidedText(index.data(Qt::DisplayRole).toString(),
                                        Qt::ElideRight, textW));

        if (isActive) {
            QFont bf = option.font;
            bf.setPixelSize(9);
            painter->setFont(bf);
            const QFontMetrics bfm(bf);
            const QString lbl = "Active";
            const int bw = bfm.horizontalAdvance(lbl) + 8;
            constexpr int bh = 13;
            const int bx = textX, by = nameRect.bottom() + 2;
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(0x3a, 0xaa, 0x88));
            painter->drawRoundedRect(bx, by, bw, bh, 3, 3);
            painter->setPen(Qt::white);
            painter->drawText(QRect(bx, by, bw, bh), Qt::AlignCenter, lbl);
        }
        painter->restore();
    }
};

// ─────────────────────────────────────────────────────────────────────────────

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


GameSelectorWindow::GameSelectorWindow(QWidget* parent)
  : QMainWindow(parent),
    m_library(new GameLibrary()),
    m_icons(new GameIconProvider(this)) {

  setWindowTitle(QString("ASTra Core - Game Library"));
  setMinimumSize(1100, 650);

  m_profileManager = new ModProfileManager(this);
  {
      QString err;
      if (!m_profileManager->load(&err)) {
          gf::core::logWarn(gf::core::LogCategory::General,
                            "GameSelectorWindow: failed to load mod profiles", err.toStdString());
      }
  }

  buildUi();
  refresh();

  QTimer::singleShot(4000, this, &GameSelectorWindow::onStartupUpdateCheck);
}

// ── Update check ──────────────────────────────────────────────────────────────

void GameSelectorWindow::triggerUpdateCheck(bool silent) {
    auto* checker = new gf::gui::update::UpdateChecker(
        QStringLiteral(ASTRA_GITHUB_OWNER),
        QStringLiteral(ASTRA_GITHUB_REPO),
        this);

    // Channel from QSettings (matches MainWindow's setting).
    {
        QSettings s(kSettingsOrg, kSettingsApp);
        const int v = s.value(QStringLiteral("update/channel"), 0).toInt();
        checker->setChannel(v == 1 ? gf::gui::update::UpdateChannel::Beta
                          : v == 2 ? gf::gui::update::UpdateChannel::Nightly
                          :          gf::gui::update::UpdateChannel::Stable);
    }

    if (m_versionBadge)
        m_versionBadge->setStatusChecking();

    // Always update the badge.
    connect(checker, &gf::gui::update::UpdateChecker::updateAvailable,
            this, [this](const gf::gui::update::ReleaseInfo& info) {
        if (m_versionBadge) m_versionBadge->setStatusUpdateAvailable(info);
    });
    connect(checker, &gf::gui::update::UpdateChecker::upToDate,
            this, [this]() {
        if (m_versionBadge) m_versionBadge->setStatusLatest();
    });
    connect(checker, &gf::gui::update::UpdateChecker::localAhead,
            this, [this](const gf::gui::update::ReleaseInfo& latestRelease) {
        if (m_versionBadge) m_versionBadge->setStatusPreReleaseBuild(latestRelease);
    });
    connect(checker, &gf::gui::update::UpdateChecker::checkFailed,
            this, [this](const QString& reason) {
        if (m_versionBadge) m_versionBadge->setStatusError(reason);
    });

    if (!silent) {
        connect(checker, &gf::gui::update::UpdateChecker::updateAvailable,
                this, [this, checker](const gf::gui::update::ReleaseInfo& info) {
            checker->deleteLater();

            auto* dlg = new gf::gui::update::UpdateDialog(info, this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);

            connect(dlg, &gf::gui::update::UpdateDialog::updateRequested,
                    this, [this](const gf::gui::update::ReleaseInfo& releaseInfo) {
                auto* launcher = new gf::gui::update::UpdaterLauncher(this, this);
                connect(launcher, &gf::gui::update::UpdaterLauncher::updateReadyToInstall,
                        this, []() { QApplication::quit(); });
                connect(launcher, &gf::gui::update::UpdaterLauncher::downloadFailed,
                        this, [this, launcher](const QString& reason) {
                    QMessageBox::critical(this, tr("Update Failed"),
                        tr("The update could not be downloaded or applied:\n\n%1").arg(reason));
                    launcher->deleteLater();
                });
                launcher->startUpdate(releaseInfo);
            });

            dlg->exec();
        });

        connect(checker, &gf::gui::update::UpdateChecker::upToDate,
                this, [this, checker]() {
            checker->deleteLater();
            auto* dlg = new gf::gui::update::UpToDateDialog(this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->exec();
        });

        connect(checker, &gf::gui::update::UpdateChecker::localAhead,
                checker, &QObject::deleteLater);

        connect(checker, &gf::gui::update::UpdateChecker::checkFailed,
                this, [this, checker](const QString& errorMessage) {
            checker->deleteLater();
            QMessageBox::warning(this, tr("Update Check Failed"),
                tr("Could not check for updates:\n\n%1").arg(errorMessage));
        });
    } else {
        connect(checker, &gf::gui::update::UpdateChecker::updateAvailable,
                checker, &QObject::deleteLater);
        connect(checker, &gf::gui::update::UpdateChecker::upToDate,
                checker, &QObject::deleteLater);
        connect(checker, &gf::gui::update::UpdateChecker::localAhead,
                checker, &QObject::deleteLater);
        connect(checker, &gf::gui::update::UpdateChecker::checkFailed,
                checker, &QObject::deleteLater);
    }

    checker->checkForUpdates();
}

void GameSelectorWindow::onStartupUpdateCheck() {
    triggerUpdateCheck(/*silent=*/true);
}

void GameSelectorWindow::onCheckForUpdates() {
    triggerUpdateCheck(/*silent=*/false);
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

  toolsMenu->addSeparator();
  m_actCreateBaseline = toolsMenu->addAction("Create Baseline\u2026");
  m_actCreateBaseline->setEnabled(false);
  m_actCreateBaseline->setToolTip("Capture the selected game\u2019s current live files into a new baseline profile");
  connect(m_actCreateBaseline, &QAction::triggered, this, &GameSelectorWindow::onCreateBaseline);

  m_actEditMetadata = toolsMenu->addAction("Edit Package Metadata\u2026");
  m_actEditMetadata->setEnabled(false);
  m_actEditMetadata->setToolTip("Edit the metadata of an existing ASTra mod package folder");
  connect(m_actEditMetadata, &QAction::triggered, this, &GameSelectorWindow::onEditPackageMetadata);

  auto* helpMenu = menuBar()->addMenu("Help");
  auto* actBetaGuide = helpMenu->addAction("Beta Testing Guide");
  connect(actBetaGuide, &QAction::triggered, this, &GameSelectorWindow::onBetaTestingGuide);
  helpMenu->addSeparator();

  auto* about = new QAction("About ASTra Core", this);
  connect(about, &QAction::triggered, this, &GameSelectorWindow::onAbout);
  helpMenu->addAction(about);

  helpMenu->addSeparator();
  auto* actCheckUpdates = helpMenu->addAction("Check for Updates\u2026");
  connect(actCheckUpdates, &QAction::triggered,
          this, &GameSelectorWindow::onCheckForUpdates);

  // Version badge — created here; inserted into controls row below.
  m_versionBadge = new gf::gui::update::VersionBadgeWidget(
      ASTRA_CURRENT_VERSION_QSTRING, this);
  connect(m_versionBadge, &gf::gui::update::VersionBadgeWidget::clicked,
          this, &GameSelectorWindow::onCheckForUpdates);

  // v0.7.1: view/sort/filter controls for larger libraries.
  auto* controls = new QHBoxLayout();
  controls->setContentsMargins(0, 0, 0, 0);
  controls->setSpacing(8);

  controls->addWidget(new QLabel("View:", central));
  m_viewMode = new QComboBox(central);
  m_viewMode->addItems({"Grid", "List"});
  {
      QSettings s(kSettingsOrg, kSettingsApp);
      m_viewMode->setCurrentText(s.value("library/view_mode", "List").toString());
  }
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

  // Version badge — right-aligned in the controls row so it's fully visible.
  auto* badgeSep = new QFrame(central);
  badgeSep->setFrameShape(QFrame::VLine);
  badgeSep->setFrameShadow(QFrame::Sunken);
  controls->addWidget(badgeSep);
  controls->addWidget(m_versionBadge);

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
  m_list->setItemDelegate(new GameLibraryItemDelegate(m_list));


  // Drive the selected-game details/profile panel on every selection change.
  connect(m_list, &QListWidget::itemSelectionChanged,
          this, &GameSelectorWindow::onSelectionChanged);

  // Clear selection when clicking empty space (fixes "sticky highlight" UX bug).
  m_list->viewport()->installEventFilter(this);

  connect(m_viewMode, &QComboBox::currentTextChanged, this, [this](const QString& v) {
      QSettings s(kSettingsOrg, kSettingsApp);
      s.setValue("library/view_mode", v);
      refresh();
  });
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
        const QSize sz = m_list->iconSize();
        const QPixmap scaled = pix.scaled(sz, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        it->setIcon(QIcon(scaled));
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

  // ── Side panel (slides in when a game is selected) ────────────────────────
  m_sidePanel = new QFrame(libraryPane);
  m_sidePanel->setFrameShape(QFrame::NoFrame);
  m_sidePanel->setStyleSheet(
      "QFrame { background: palette(base); border-left: 1px solid palette(mid); }"
      "QLabel { border: none; background: transparent; }");
  m_sidePanel->setFixedWidth(280);
  m_sidePanel->setVisible(false);

  auto* spLayout = new QVBoxLayout(m_sidePanel);
  spLayout->setContentsMargins(12, 12, 12, 12);
  spLayout->setSpacing(6);

  m_spGameLabel = new QLabel("", m_sidePanel);
  m_spGameLabel->setStyleSheet("font-weight: bold; font-size: 14px;");
  m_spGameLabel->setWordWrap(true);
  spLayout->addWidget(m_spGameLabel);

  m_spMetaLabel = new QLabel("", m_sidePanel);
  m_spMetaLabel->setStyleSheet("color: palette(mid); font-size: 11px;");
  spLayout->addWidget(m_spMetaLabel);

  {
      auto* sep = new QFrame(m_sidePanel);
      sep->setFrameShape(QFrame::HLine);
      sep->setFrameShadow(QFrame::Sunken);
      spLayout->addWidget(sep);
  }

  auto* phRow = new QHBoxLayout;
  auto* phLabel = new QLabel("PROFILES", m_sidePanel);
  phLabel->setStyleSheet("font-weight: bold; font-size: 10px; color: palette(mid);");
  m_spProfilesBtn = new QPushButton("Manage\u2026", m_sidePanel);
  m_spProfilesBtn->setFlat(true);
  m_spProfilesBtn->setFixedHeight(20);
  m_spProfilesBtn->setStyleSheet("font-size: 11px;");
  m_spProfilesBtn->setToolTip("Open full profile manager (create, rename, delete, clone\u2026)");
  connect(m_spProfilesBtn, &QPushButton::clicked, this, &GameSelectorWindow::onManageProfiles);
  phRow->addWidget(phLabel, 1);
  phRow->addWidget(m_spProfilesBtn);
  spLayout->addLayout(phRow);

  m_spProfileList = new QListWidget(m_sidePanel);
  m_spProfileList->setItemDelegate(new SidePanelProfileDelegate(m_spProfileList));
  m_spProfileList->setIconSize(QSize(32, 32));
  m_spProfileList->setFixedHeight(160);
  m_spProfileList->setStyleSheet(
      "QListWidget { border: 1px solid palette(mid); border-radius: 4px; }"
      "QListWidget::item:selected { border: 2px solid #6f42c1; border-radius: 4px; }");
  connect(m_spProfileList, &QListWidget::itemDoubleClicked,
          this, &GameSelectorWindow::onSidePanelActivateProfile);
  spLayout->addWidget(m_spProfileList);

  {
      auto* sep = new QFrame(m_sidePanel);
      sep->setFrameShape(QFrame::HLine);
      sep->setFrameShadow(QFrame::Sunken);
      spLayout->addWidget(sep);
  }

  m_spApplyBtn = new QPushButton("Apply Profile", m_sidePanel);
  m_spApplyBtn->setEnabled(false);
  m_spApplyBtn->setToolTip("Copy resolved profile files to the live game directory");
  connect(m_spApplyBtn, &QPushButton::clicked, this, &GameSelectorWindow::onApplyProfile);
  spLayout->addWidget(m_spApplyBtn);

  auto* modRow = new QHBoxLayout;
  m_spInstallBtn = new QPushButton("Install Mod\u2026", m_sidePanel);
  m_spInstallBtn->setEnabled(false);
  m_spInstallBtn->setToolTip("Install a local mod package into the active profile");
  connect(m_spInstallBtn, &QPushButton::clicked, this, &GameSelectorWindow::onInstallMod);
  m_spInstalledBtn = new QPushButton("Installed\u2026", m_sidePanel);
  m_spInstalledBtn->setEnabled(false);
  m_spInstalledBtn->setToolTip("Browse mods installed in the active profile");
  connect(m_spInstalledBtn, &QPushButton::clicked, this, &GameSelectorWindow::onShowInstalledMods);
  modRow->addWidget(m_spInstallBtn);
  modRow->addWidget(m_spInstalledBtn);
  spLayout->addLayout(modRow);

  m_spExportBtn = new QPushButton("Export Package\u2026", m_sidePanel);
  m_spExportBtn->setEnabled(false);
  m_spExportBtn->setToolTip("Export the active profile as a portable ASTra mod package");
  connect(m_spExportBtn, &QPushButton::clicked, this, &GameSelectorWindow::onExportPackage);
  spLayout->addWidget(m_spExportBtn);

  spLayout->addStretch(1);

  // ── Body row: game list (left) + side panel (right) ───────────────────────
  auto* bodyRow = new QHBoxLayout;
  bodyRow->setContentsMargins(0, 0, 0, 0);
  bodyRow->setSpacing(0);

  auto* listPane = new QWidget(libraryPane);
  auto* listPaneLayout = new QVBoxLayout(listPane);
  listPaneLayout->setContentsMargins(0, 0, 0, 0);
  listPaneLayout->setSpacing(0);
  listPaneLayout->addWidget(m_list, 1);
  listPaneLayout->addWidget(m_emptyState, 1);

  bodyRow->addWidget(listPane, 1);
  bodyRow->addWidget(m_sidePanel, 0);

  connect(m_profileManager, &ModProfileManager::activeProfileChanged,
          this, [this](const QString&, const QString&) { refreshSidePanel(); });
  connect(m_profileManager, &ModProfileManager::profilesChanged,
          this, [this](const QString&) { refreshSidePanel(); });

  libraryLayout->addLayout(bodyRow, 1);
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
  m_list->setIconSize(listView ? QSize(120, 68) : GameIconProvider::iconSize());

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
      if (listView) {
        // List mode: keep full image visible (no crop)
        pix = pix.scaled(m_list->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
      } else {
        // Grid mode: fill tile exactly
        pix = pix.scaled(m_list->iconSize(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const int x = qMax(0, (pix.width() - m_list->iconSize().width()) / 2);
        const int y = qMax(0, (pix.height() - m_list->iconSize().height()) / 2);
        pix = pix.copy(x, y, m_list->iconSize().width(), m_list->iconSize().height());
      }
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

    const QString cleanTitle = cleanGameTitle(g);
    const int     titleYear  = extractTitleYear(cleanTitle);
    const QString baseLabel  = QString("%1\n%2").arg(cleanTitle, secondLine);
    item->setData(Qt::UserRole + 2, baseLabel);
    item->setData(Qt::UserRole + 1, downloading);
    item->setData(Qt::UserRole + 3, normalizedPlatformLabel(g.platform));
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

  refreshSidePanel();

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

void GameSelectorWindow::refreshSidePanel() {
    const QString id  = selectedGameId();
    const bool hasSel = !id.isEmpty();

    m_sidePanel->setVisible(hasSel);
    if (m_actCreateBaseline) m_actCreateBaseline->setEnabled(hasSel);
    if (m_actEditMetadata)   m_actEditMetadata->setEnabled(hasSel);

    if (!hasSel) return;

    const auto gOpt = m_library->findById(id);
    if (!gOpt.has_value()) return;
    const auto& g = gOpt.value();

    m_spGameLabel->setText(cleanGameTitle(g));

    const int astCount = (g.astCount > 0 || g.hasAst)
        ? g.astCount
        : (g.mainAstPaths.size() + g.patchAstPaths.size());
    QString meta = normalizedPlatformLabel(g.platform);
    if (astCount > 0) meta += QString("  \u00b7  ASTs: %1").arg(astCount);
    m_spMetaLabel->setText(meta);

    const QString pgid = gameProfileId(g.rootPath);
    {
        QString err;
        m_profileManager->load(&err);
    }
    const auto profiles = m_profileManager->profilesForGame(pgid);
    const auto activeId = m_profileManager->activeProfileId(pgid);

    m_spProfileList->clear();
    for (const auto& p : profiles) {
        auto* item = new QListWidgetItem(p.name);
        const QString iconPath = spProfileIconPath(p.workspacePath);
        if (QFile::exists(iconPath))
            item->setIcon(QIcon(iconPath));
        item->setData(Qt::UserRole,     p.id);
        item->setData(Qt::UserRole + 1, activeId.has_value() && *activeId == p.id);
        m_spProfileList->addItem(item);
    }
    if (profiles.isEmpty()) {
        auto* ph = new QListWidgetItem("No profiles \u2014 click Manage\u2026 to create one");
        ph->setFlags(Qt::NoItemFlags);
        ph->setForeground(QColor(150, 150, 150));
        m_spProfileList->addItem(ph);
    }

    const bool hasActive = activeId.has_value();
    m_spApplyBtn->setEnabled(hasActive);
    m_spInstallBtn->setEnabled(hasActive);
    m_spInstalledBtn->setEnabled(hasActive);
    m_spExportBtn->setEnabled(hasActive);

    if (hasActive) {
        const auto ap = m_profileManager->activeProfile(pgid);
        if (ap) {
            m_spApplyBtn->setToolTip(
                QString("Apply profile \u201c%1\u201d to the live game directory").arg(ap->name));
            m_spInstallBtn->setToolTip(
                QString("Install a mod into profile: %1").arg(ap->name));
            m_spInstalledBtn->setToolTip(
                QString("Browse mods installed in profile: %1").arg(ap->name));
            m_spExportBtn->setToolTip(
                QString("Export profile \u201c%1\u201d as an ASTra mod package").arg(ap->name));
        }
    } else {
        const QString tip = "Activate a profile to enable this action";
        m_spApplyBtn->setToolTip(tip);
        m_spInstallBtn->setToolTip(tip);
        m_spInstalledBtn->setToolTip(tip);
        m_spExportBtn->setToolTip(tip);
    }
}

void GameSelectorWindow::onSidePanelActivateProfile(QListWidgetItem* item) {
    if (!item) return;
    const QString profileId = item->data(Qt::UserRole).toString();
    if (profileId.isEmpty()) return;

    const QString gameId = selectedGameId();
    if (gameId.isEmpty()) return;
    const auto gOpt = m_library->findById(gameId);
    if (!gOpt.has_value()) return;

    const QString pgid = gameProfileId(gOpt.value().rootPath);
    QString err;
    if (!m_profileManager->setActiveProfile(pgid, profileId, &err)) {
        QMessageBox::warning(this, "Activate Profile", err);
        return;
    }
    refreshSidePanel();
}

void GameSelectorWindow::onManageProfiles() {
    const QString gameId = selectedGameId();
    if (gameId.isEmpty()) return;

    const auto opt = m_library->findById(gameId);
    if (!opt.has_value()) return;

    const auto& g = opt.value();
    const QString profileGameId = gameProfileId(g.rootPath);
    const QString displayName   = cleanGameTitle(g);

    // Reload profiles so the dialog reflects any changes made since startup.
    {
        QString err;
        m_profileManager->load(&err);
    }

    // Phase 7: resolve the source content path so NewProfileDialog can pre-fill
    // the game copy field.  Prefer the runtime-configured AST dir; fall back to
    // the game entry's USRDIR / scanRoot (covers games without runtime config).
    QString sourceContentPath;
    {
        const auto rtCfg = RuntimeTargetManager::load(profileGameId);
        if (rtCfg && !rtCfg->astDirPath.isEmpty()) {
            sourceContentPath = rtCfg->astDirPath;
        } else if (!g.usrdirPath.isEmpty()) {
            sourceContentPath = g.usrdirPath;
        } else if (!g.scanRoot.isEmpty()) {
            sourceContentPath = g.scanRoot;
        }
    }

    auto* dlg = new ModProfilesDialog(m_profileManager, profileGameId, displayName,
                                      sourceContentPath, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void GameSelectorWindow::onInstallMod() {
    const QString gameId = selectedGameId();
    if (gameId.isEmpty()) return;

    const auto gOpt = m_library->findById(gameId);
    if (!gOpt.has_value()) return;

    const QString pgid = gameProfileId(gOpt.value().rootPath);

    // Reload so we have the freshest profile state
    {
        QString err;
        m_profileManager->load(&err);
    }

    const auto active = m_profileManager->activeProfile(pgid);
    if (!active.has_value()) {
        QMessageBox::information(
            this,
            "Install Mod",
            "Please activate a mod profile for this game before installing mods.\n\n"
            "Use \u201cManage Profiles\u2026\u201d to create or activate a profile.");
        return;
    }

    auto* dlg = new InstallModDialog(*active, this);
    dlg->exec(); // modal — profile panel updates after close via onSelectionChanged
    onSelectionChanged(); // refresh panel in case the active profile changed
}

void GameSelectorWindow::onShowInstalledMods() {
    const QString gameId = selectedGameId();
    if (gameId.isEmpty()) return;

    const auto gOpt = m_library->findById(gameId);
    if (!gOpt.has_value()) return;

    const QString pgid        = gameProfileId(gOpt.value().rootPath);
    const QString displayName = cleanGameTitle(gOpt.value());

    {
        QString err;
        m_profileManager->load(&err);
    }

    const auto active = m_profileManager->activeProfile(pgid);
    if (!active.has_value()) {
        QMessageBox::information(
            this,
            "Installed Mods",
            "Please activate a mod profile for this game first.\n\n"
            "Use \u201cManage Profiles\u2026\u201d to create or activate a profile.");
        return;
    }

    auto* dlg = new InstalledModsDialog(*active, displayName, this);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void GameSelectorWindow::onCreateBaseline() {
    const QString gameId = selectedGameId();
    if (gameId.isEmpty()) return;

    const auto gOpt = m_library->findById(gameId);
    if (!gOpt.has_value()) return;

    const auto& g              = gOpt.value();
    const QString pgid         = gameProfileId(g.rootPath);
    const QString displayName  = cleanGameTitle(g);

    // Resolve AST directory from the game entry directly — no runtime config needed.
    QString astDir;
    if (!g.usrdirPath.isEmpty())    astDir = g.usrdirPath;
    else if (!g.scanRoot.isEmpty()) astDir = g.scanRoot;
    else                             astDir = g.rootPath;

    if (astDir.isEmpty() || !QDir(astDir).exists()) {
        QMessageBox::warning(
            this, "Create Baseline",
            QString("Could not locate the game\u2019s content directory.\n\n"
                    "Expected path: %1\n\n"
                    "Verify the game was added correctly.")
                .arg(QDir::toNativeSeparators(astDir)));
        return;
    }

    bool nameOk = false;
    const QString profileName = QInputDialog::getText(
        this,
        "Create Baseline",
        "Enter a name for the new baseline profile:",
        QLineEdit::Normal,
        QStringLiteral("Vanilla"),
        &nameOk).trimmed();

    if (!nameOk || profileName.isEmpty()) return;

    const BaselineCaptureResult result = BaselineCaptureService::capture(
        pgid,
        astDir,
        profileName,
        BaselineType::Vanilla,
        *m_profileManager,
        this,
        displayName);

    if (!result.warnings.isEmpty()) {
        QMessageBox::warning(
            this,
            "Create Baseline \u2014 Warnings",
            result.warnings.join('\n'));
    }

    if (result.success) {
        QMessageBox::information(
            this,
            "Baseline Created",
            QString("Baseline \u201c%1\u201d created successfully.\n%2")
                .arg(profileName, result.message));
    } else {
        QMessageBox::critical(
            this,
            "Baseline Creation Failed",
            result.message);
    }

    onSelectionChanged();
}

// ── Phase 4B/4D helpers ───────────────────────────────────────────────────────

// Build a RuntimeTargetConfig from a GameEntry when no persisted config exists.
// Uses the same path-resolution order as onCreateBaseline() / onAddGame().
// contentRoots is left empty → single-root mode (safe for the common case).
static RuntimeTargetConfig deriveRuntimeConfig(const QString&   pgid,
                                                const GameEntry& g)
{
    RuntimeTargetConfig cfg;
    cfg.gameId = pgid;
    if (!g.usrdirPath.isEmpty())    cfg.astDirPath = g.usrdirPath;
    else if (!g.scanRoot.isEmpty()) cfg.astDirPath = g.scanRoot;
    else                             cfg.astDirPath = g.rootPath;
    return cfg;
}

// Load the persisted config; if absent, derive one from the game entry.
// Users who set up a runtime config (multi-root / DLC) get their full config.
// Everyone else gets single-root mode derived from the game entry.
static RuntimeTargetConfig resolveRuntimeConfig(const QString&   pgid,
                                                 const GameEntry& g)
{
    const auto persisted = RuntimeTargetManager::load(pgid);
    return persisted.value_or(deriveRuntimeConfig(pgid, g));
}

// ── Phase 5B: mod package export ─────────────────────────────────────────────

void GameSelectorWindow::onExportPackage() {
    const QString gameId = selectedGameId();
    if (gameId.isEmpty()) return;

    const auto gOpt = m_library->findById(gameId);
    if (!gOpt.has_value()) return;

    const QString pgid        = gameProfileId(gOpt.value().rootPath);
    const QString displayName = cleanGameTitle(gOpt.value());

    // Load runtime config (derived from game entry if no persisted config)
    const RuntimeTargetConfig runtime = resolveRuntimeConfig(pgid, gOpt.value());

    // Load active profile
    {
        QString e;
        m_profileManager->load(&e);
    }
    const auto active = m_profileManager->activeProfile(pgid);
    if (!active.has_value()) {
        QMessageBox::information(this, "Export Package",
            "Please activate a mod profile for this game before exporting.\n\n"
            "Use \u201cManage Profiles\u2026\u201d to create or activate a profile.");
        return;
    }

    // Resolve profile
    QString resolveErr;
    const ProfileResolvedMap resolved =
        ProfileResolverService::resolve(*active, runtime, &resolveErr);
    if (!resolved.isValid()) {
        QMessageBox::critical(this, "Export Package — Resolution Failed",
            QString("Profile resolution failed:\n%1")
                .arg(resolved.errors.isEmpty() ? resolveErr : resolved.errors.join('\n')));
        return;
    }
    if (resolved.files.isEmpty()) {
        QMessageBox::warning(this, "Export Package",
            "The active profile has no files to export.\n\n"
            "Capture a baseline or install mods first.");
        return;
    }

    // Show export dialog
    auto* dlg = new ExportModDialog(pgid, active->name, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose, false); // we need spec() after exec
    if (dlg->exec() != QDialog::Accepted) {
        delete dlg;
        return;
    }

    const ModExportSpec spec = dlg->spec();
    const QString       dir  = dlg->outputDir();
    delete dlg;

    // Export
    const ModExportResult exportResult =
        ModPackageExporter::exportPackage(spec, resolved, runtime, dir, this);

    if (!exportResult.warnings.isEmpty()) {
        QMessageBox::warning(this, "Export Package — Warnings",
            exportResult.warnings.join('\n'));
    }

    if (exportResult.success) {
        QMessageBox::information(this, "Export Package",
            QString("%1\n\nPackage folder:\n%2")
                .arg(exportResult.message,
                     QDir::toNativeSeparators(exportResult.outputPath)));
    } else {
        QMessageBox::critical(this, "Export Package Failed",
            exportResult.errors.isEmpty()
                ? exportResult.message
                : exportResult.errors.join('\n'));
    }
}

// ── Phase 5C: edit existing package metadata ──────────────────────────────────

void GameSelectorWindow::onEditPackageMetadata() {
    const QString folder = QFileDialog::getExistingDirectory(
        this,
        "Select Mod Package Folder",
        QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (folder.isEmpty()) return;

    // Quick sanity-check before opening the editor
    const QString manifestPath = QDir(folder).filePath("astra_mod.json");
    if (!QFileInfo::exists(manifestPath)) {
        QMessageBox::warning(this, "Edit Package Metadata",
            QString("No astra_mod.json found in:\n%1\n\n"
                    "Please select a valid ASTra mod package folder.")
                .arg(QDir::toNativeSeparators(folder)));
        return;
    }

    auto* dlg = new ModMetadataEditorDialog(folder, this);
    dlg->exec();
}

// Phase 6C: Resolve the active profile and show an ApplyPreviewDialog.
// Returns false if preconditions fail (dialog shown) or user cancelled.
// Returns true if resolution succeeded and user confirmed.
static bool doApplyPreview(const QString&      gameId,
                            GameLibrary*        library,
                            ModProfileManager*  profileManager,
                            QWidget*            parent)
{
    const auto gOpt = library->findById(gameId);
    if (!gOpt.has_value()) return false;

    const QString pgid = gameProfileId(gOpt.value().rootPath);

    {
        QString e;
        profileManager->load(&e);
    }

    const auto active = profileManager->activeProfile(pgid);
    if (!active.has_value()) return true; // let doApply handle this case

    const RuntimeTargetConfig runtime = resolveRuntimeConfig(pgid, gOpt.value());

    const ProfileResolvedMap resolved =
        ProfileResolverService::resolve(*active, runtime);

    if (!resolved.isValid()) {
        QMessageBox::critical(
            parent, "Apply Profile \u2014 Conflict",
            "Profile resolution failed:\n\n" + resolved.errors.join('\n'));
        return false;
    }

    ApplyPreviewDialog preview(
        resolved, runtime, active->name, parent);
    return preview.exec() == QDialog::Accepted;
}

// Shared apply setup used by onApplyProfile().
// Returns false if preconditions are not met (error dialogs already shown).
static bool doApply(const QString&      gameId,
                    GameLibrary*        library,
                    ModProfileManager*  profileManager,
                    QWidget*            parent,
                    ProfileApplyResult& outResult) {
    const auto gOpt = library->findById(gameId);
    if (!gOpt.has_value()) return false;

    const QString pgid = gameProfileId(gOpt.value().rootPath);

    {
        QString e;
        profileManager->load(&e);
    }

    const auto active = profileManager->activeProfile(pgid);
    if (!active.has_value()) {
        QMessageBox::information(
            parent, "Apply Profile",
            "Please activate a mod profile for this game before applying.\n\n"
            "Use \u201cManage Profiles\u2026\u201d to create or activate a profile.");
        return false;
    }

    const RuntimeTargetConfig runtime = resolveRuntimeConfig(pgid, gOpt.value());

    outResult = ProfileApplyService::apply(*active, runtime, /*backup=*/true, parent);
    return true;
}

void GameSelectorWindow::onApplyProfile() {
    const QString gameId = selectedGameId();
    if (gameId.isEmpty()) return;

    // Phase 6C: show apply preview before writing any files
    if (!doApplyPreview(gameId, m_library, m_profileManager, this)) return;

    ProfileApplyResult result;
    if (!doApply(gameId, m_library, m_profileManager, this, result)) return;

    if (!result.warnings.isEmpty()) {
        QMessageBox::warning(this, "Apply Profile — Warnings", result.warnings.join('\n'));
    }

    if (result.success) {
        QString msg = result.message;
        if (result.backupCreated)
            msg += "\n\nA baseline backup was created in the profile workspace "
                   "(snapshots/baseline_backup/).";
        QMessageBox::information(this, "Apply Profile", msg);
    } else {
        QMessageBox::critical(this, "Apply Profile Failed",
                              result.errors.isEmpty() ? result.message : result.errors.join('\n'));
    }

    onSelectionChanged();
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
  if (dlg.exec() != QDialog::Accepted) return;

  // Auto-create a Vanilla baseline for the newly added game (best-effort).
  const GameEntry newGame = dlg.result();
  if (!newGame.id.isEmpty()) {
    QString astDir;
    if (!newGame.usrdirPath.isEmpty())    astDir = newGame.usrdirPath;
    else if (!newGame.scanRoot.isEmpty()) astDir = newGame.scanRoot;
    else                                  astDir = newGame.rootPath;

    if (!astDir.isEmpty() && QDir(astDir).exists()) {
      const QString pgid        = gameProfileId(newGame.rootPath);
      const QString displayName = cleanGameTitle(newGame);
      BaselineCaptureService::capture(pgid, astDir, "Vanilla",
                                      BaselineType::Vanilla,
                                      *m_profileManager, this, displayName);
      // Ignore result — baseline is best-effort; user can create one manually
      // via Tools > Create Baseline if this fails (e.g. no ASTs found yet).
    }
  }

  refresh();
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