#include "MainWindow.hpp"
#include "CreateRsfAstDialog.hpp"
#include "update/UpdateChecker.hpp"
#include "update/UpdateDialog.hpp"
#include "update/UpdaterLauncher.hpp"
#include "update/UpdaterConfig.hpp"

#include <gf/apt/apt_xml.hpp>
#include <gf/apt/apt_reader.hpp>
#include <gf/apt/apt_renderer.hpp>
#include <gf/apt/apt_action_inspector.hpp>
#include "AstIndexer.hpp"
#include "PlatformUtils.hpp"
#include "gf/core/log.hpp"
#include "gf/core/AstArchive.hpp"
#include "gf/core/AstContainerEditor.hpp"
#include "gf/core/safe_write.hpp"

#include <optional>
#include <filesystem>
#include <unordered_set>

#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QAbstractButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QAbstractItemView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QStatusBar>
#include <QSplitter>
#include <QProgressBar>
#include <QToolBar>
#include <QDockWidget>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QImage>
#include <QPixmap>
#include <QFileInfo>
#include <QScrollArea>
#include <QShortcut>
#include <QWheelEvent>
#include <QEvent>
#include <QFontDatabase>
#include <QDir>

#include <tuple>
#include <QWidget>
#include <QFrame>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QMenu>
#include <QAction>
#include <QMenuBar>
#include <QMessageBox>
#include <QInputDialog>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QFileDialog>
#include <QStandardPaths>
#include <QFile>
#include <QDesktopServices>
#include <QUrl>
#include <QHash>
#include <QSettings>
#include <QApplication>
#include <QClipboard>
#include <QStyle>
#include <QMap>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QStackedWidget>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QCheckBox>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsLineItem>
#include <QPen>
#include <QBrush>
#include <QPainter>
#include <QScopeGuard>
#include <QSignalBlocker>

#include "GuiSettings.hpp"
#include "apt_editor/AptPreviewScene.hpp"
#include "apt_editor/AptSelectionManager.hpp"


namespace {

QString rsfNormalizeTextureKey(QString value) {
  value = value.trimmed().toLower();
  value.replace('\\', '/');
  if (value.startsWith('"') && value.endsWith('"') && value.size() >= 2) value = value.mid(1, value.size() - 2);
  return value;
}

QStringList rsfBuildTextureNeedles(const std::string& textureName, const std::string& textureFilename) {
  QStringList out;
  auto addKey = [&](const QString& raw) {
    const QString k = rsfNormalizeTextureKey(raw);
    if (k.isEmpty()) return;
    if (!out.contains(k)) out << k;
    const QFileInfo fi(k);
    if (!fi.fileName().isEmpty() && !out.contains(fi.fileName().toLower())) out << fi.fileName().toLower();
    if (!fi.completeBaseName().isEmpty() && !out.contains(fi.completeBaseName().toLower())) out << fi.completeBaseName().toLower();
  };
  addKey(QString::fromStdString(textureName));
  addKey(QString::fromStdString(textureFilename));
  return out;
}

bool rsfCandidateMatchesTexture(const QStringList& needles, const QString& displayName, const QString& path) {
  const QString displayNorm = rsfNormalizeTextureKey(displayName);
  const QString pathNorm = rsfNormalizeTextureKey(path);
  const QFileInfo displayInfo(displayNorm);
  const QFileInfo pathInfo(pathNorm);
  const QString displayFile = displayInfo.fileName().toLower();
  const QString displayStem = displayInfo.completeBaseName().toLower();
  const QString pathFile = pathInfo.fileName().toLower();
  const QString pathStem = pathInfo.completeBaseName().toLower();
  for (const auto& needle : needles) {
    if (needle.isEmpty()) continue;
    if (displayNorm == needle || pathNorm == needle || displayFile == needle || pathFile == needle || displayStem == needle || pathStem == needle) return true;
    if (displayNorm.contains(needle) || pathNorm.contains(needle) || (!displayFile.isEmpty() && displayFile.contains(needle)) || (!pathFile.isEmpty() && pathFile.contains(needle))) return true;
  }
  return false;
}

} // namespace

// APT tree item node-type roles (stored in kAptRoleNodeType / +11)
static constexpr int kAptNodePlain     = 0; // group/root/fallback
static constexpr int kAptNodeSummary   = 1;
static constexpr int kAptNodeImport    = 2;
static constexpr int kAptNodeExport    = 3;
static constexpr int kAptNodeFrame     = 4;
static constexpr int kAptNodeCharacter = 5;
static constexpr int kAptNodeSlice     = 6;
static constexpr int kAptNodePlacement = 7;

static constexpr int kAptRoleNodeType = Qt::UserRole + 10;
static constexpr int kAptRoleNodeIndex = Qt::UserRole + 11;
static constexpr int kAptRoleOwnerKind = Qt::UserRole + 12; // 0=root movie, 1=character sprite/movie
static constexpr int kAptRoleOwnerIndex = Qt::UserRole + 13;
static constexpr int kAptRolePlacementIndex = Qt::UserRole + 14;


#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <fstream>

#include <cctype>
#include <limits>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <functional>

#include <zlib.h>

#include <gf/models/rsf.hpp>

#include <gf/textures/dds_decode.hpp>
#include <gf/textures/dds_validate.hpp>
#include <gf/textures/ea_dds_rebuild.hpp>
#include <gf/textures/xpr2_rebuild.hpp>
#include <gf/textures/texture_replace.hpp>
#include "rsf_editor/RsfPreviewWidget.hpp"


namespace {

class AptPreviewView final : public QGraphicsView {
  Q_OBJECT
 public:
  explicit AptPreviewView(QWidget* parent = nullptr)
      : QGraphicsView(parent) {
    setTransformationAnchor(AnchorUnderMouse);
    setResizeAnchor(AnchorViewCenter);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setMouseTracking(true);
  }

  // Fit content to view (called after initial load and by the Fit toolbar button).
  void fitContent() {
    if (scene() && !scene()->items().isEmpty())
      fitInView(scene()->itemsBoundingRect().adjusted(-20, -20, 20, 20), Qt::KeepAspectRatio);
    else if (scene())
      fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
    m_fitActive = true;
  }

  void zoomTo100() {
    resetTransform();
    m_fitActive = false;
  }

  void zoomBy(double factor) {
    const double current = transform().m11(); // uniform scale
    const double next = current * factor;
    if (next < 0.05 || next > 50.0) return;
    scale(factor, factor);
    m_fitActive = false;
  }

 protected:
  void resizeEvent(QResizeEvent* event) override {
    QGraphicsView::resizeEvent(event);
    if (m_fitActive && scene())
      fitContent();
  }

  void wheelEvent(QWheelEvent* event) override {
    if (event->modifiers() & Qt::ControlModifier) {
      const double factor = (event->angleDelta().y() > 0) ? 1.18 : 1.0 / 1.18;
      zoomBy(factor);
      event->accept();
    } else {
      QGraphicsView::wheelEvent(event);
    }
  }

  void mouseDoubleClickEvent(QMouseEvent* event) override {
    // Center the view on the topmost item under the cursor.
    const QList<QGraphicsItem*> its = items(event->pos());
    if (!its.isEmpty()) {
      centerOn(its.first());
      m_fitActive = false;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
  }

 private:
  bool m_fitActive = true; // true = re-fit on resize
};

static const char* ddsFormatName(gf::textures::DdsFormat f) {
  using gf::textures::DdsFormat;
  switch (f) {
    case DdsFormat::DXT1: return "DXT1";
    case DdsFormat::DXT3: return "DXT3";
    case DdsFormat::DXT5: return "DXT5";
    case DdsFormat::ATI1: return "ATI1/BC4";
    case DdsFormat::ATI2: return "ATI2/BC5";
    case DdsFormat::RGBA32: return "RGBA32";
    default: return "Unknown";
  }
}

static QString textureInfoPanelText(const QString& containerType, const QString& displayName, const gf::textures::DdsInfo& info, int shownMip) {
  return QString("%1\n%2\nResolution: %3x%4\nFormat: %5\nMipmaps: %6\nShowing Mip: %7")
      .arg(containerType.isEmpty() ? QString("Texture") : containerType,
           displayName.isEmpty() ? QString("Unnamed") : displayName)
      .arg(info.width)
      .arg(info.height)
      .arg(QString::fromLatin1(ddsFormatName(info.format)))
      .arg(info.mipCount)
      .arg(shownMip);
}


}

namespace gf::gui {

static QString aptPlacementValueText(const gf::apt::AptPlacement& pl);
static QString aptPlacementDetailText(const gf::apt::AptPlacement& pl, const QString& ownerLabel, int frameIndex, int placementIndex);
static QString aptPlacementTreeLabel(const gf::apt::AptPlacement& pl);


static QString aptPlacementValueText(const gf::apt::AptPlacement& pl) {
  return QString("depth %1").arg(pl.depth);
}

// Primary label for a placement node in the APT tree.
// Format (Ion-debugger style): "mcTitle (C42)" when an instance name is present,
// "C42  D3" (charId + depth) otherwise.
static QString aptPlacementTreeLabel(const gf::apt::AptPlacement& pl) {
  if (!pl.instance_name.empty())
    return QString("%1  (C%2)")
        .arg(QString::fromStdString(pl.instance_name))
        .arg(pl.character);
  return QString("C%1  D%2").arg(pl.character).arg(pl.depth);
}

static QString aptPlacementDetailText(const gf::apt::AptPlacement& pl, const QString& ownerLabel, int frameIndex, int placementIndex) {
  return QString("Type: Placement\nOwner: %1\nFrame: %2\nPlacement Index: %3\nDepth: %4\nCharacter: %5\nInstance Name: %6\n"
                 "X: %7\nY: %8\nScale X (a): %9\nRotate Skew 0 (b): %10\nRotate Skew 1 (c): %11\nScale Y (d): %12\nOffset: 0x%13")
      .arg(ownerLabel)
      .arg(frameIndex)
      .arg(placementIndex)
      .arg(pl.depth)
      .arg(pl.character)
      .arg(pl.instance_name.empty() ? QString("(empty)") : QString::fromStdString(pl.instance_name))
      .arg(pl.transform.x, 0, 'f', 3)
      .arg(pl.transform.y, 0, 'f', 3)
      .arg(pl.transform.scale_x, 0, 'f', 3)
      .arg(pl.transform.rotate_skew_0, 0, 'f', 3)
      .arg(pl.transform.rotate_skew_1, 0, 'f', 3)
      .arg(pl.transform.scale_y, 0, 'f', 3)
      .arg(QString::number(qulonglong(pl.offset), 16).toUpper());
}


// ---- Export / UI helpers ----------------------------------------------------
static QString normalizeExt(QString ext) {
  ext = ext.trimmed();
  if (ext.startsWith('.')) ext = ext.mid(1);
  return ext;
}

static QString defaultExtForTypeUpper(const QString& typeUpper) {
  const QString t = typeUpper.trimmed().toUpper();
  if (t == "XML") return "xml";
  if (t == "JSON") return "json";
  if (t == "INI" || t == "CFG" || t == "CONF") return "ini";
  if (t == "YAML" || t == "YML") return "yml";
  if (t == "TXT" || t == "TEXT") return "txt";
  if (t == "DDS") return "dds";
  if (t == "PNG") return "png";
  if (t == "RSF" || t == "RSG") return "rsf";
  if (t == "APT") return "apt";
  if (t == "CONST") return "const";
  if (t == "TX2D") return "tx2d";
  if (t == "XPR2" || t == "XPR") return "xpr";
  if (t.startsWith("P3R")) return "dds";
  if (t == "AST") return "ast";
  return "bin";
}

static QString saveFilterForExt(const QString& extNoDot) {
  const QString ext = normalizeExt(extNoDot);
  if (ext.isEmpty() || ext == "*") return "All Files (*.*)";
  // Keep this simple and consistent.
  return QString("%1 Files (*.%2);;All Files (*.*)")
      .arg(ext.toUpper())
      .arg(ext);
}


static QString sanitizeExportName(QString name) {
  name = name.trimmed();
  if (name.isEmpty()) return "unnamed";
  static const QString bad = QStringLiteral(R"(\/:*?"<>|)");
  for (const QChar ch : bad) name.replace(ch, '_');
  name.replace('\n', '_');
  name.replace('\r', '_');
  return name;
}

static QString visibleBaseNameForItem(QTreeWidgetItem* item, std::uint32_t fallbackIndex = 0) {
  if (!item) return QString("entry_%1").arg(fallbackIndex);
  QString baseName = item->text(0).trimmed();
  const int paren = baseName.indexOf('(');
  if (paren >= 0) baseName = baseName.left(paren).trimmed();
  if (baseName.isEmpty()) baseName = QString("entry_%1").arg(fallbackIndex);
  QFileInfo fi(baseName);
  if (!fi.completeBaseName().isEmpty()) baseName = fi.completeBaseName();
  return sanitizeExportName(baseName);
}

static QString humanSizeString(quint64 bytes) {
  static const char* units[] = {"B", "KB", "MB", "GB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 3) {
    value /= 1024.0;
    ++unit;
  }
  if (unit == 0) return QString("%1 %2").arg(bytes).arg(units[unit]);
  return QString::number(value, 'f', value >= 100.0 ? 0 : (value >= 10.0 ? 1 : 2)) + " " + units[unit];
}

static QString buildTreeItemTooltip(const QString& displayName,
                                    const QString& type,
                                    const QString& info,
                                    const QString& sourcePath,
                                    bool embedded,
                                    quint64 absOffset = 0,
                                    quint64 maxReadable = 0,
                                    const QString& extra = QString()) {
  QStringList lines;
  lines << (displayName.isEmpty() ? QString("Unnamed") : displayName);
  if (!type.isEmpty()) lines << QString("Type: %1").arg(type);
  if (!info.isEmpty()) lines << QString("Info: %1").arg(info);
  if (embedded) {
    lines << QString("Embedded Entry: Yes");
    lines << QString("Offset: 0x%1").arg(absOffset, 0, 16);
    if (maxReadable > 0) lines << QString("Size: %1 (%2 bytes)").arg(humanSizeString(maxReadable)).arg(maxReadable);
  }
  if (!extra.isEmpty()) lines << extra;
  if (!sourcePath.isEmpty()) lines << QString("Source: %1").arg(QDir::toNativeSeparators(sourcePath));
  return lines.join('\n');
}

static QString bytesToEntryName(const std::vector<std::uint8_t>& v, std::uint32_t fallbackIndex) {
  size_t n = 0;
  while (n < v.size() && v[n] != 0) ++n;
  QString name = QString::fromUtf8(reinterpret_cast<const char*>(v.data()), static_cast<int>(n)).trimmed();
  if (name.isEmpty()) name = QString("entry_%1").arg(fallbackIndex);
  return name;
}

static QString detectTypeUpperFromBytes(std::span<const std::uint8_t> bytes) {
  if (bytes.size() >= 4 && bytes[0] == 'B' && bytes[1] == 'G' && bytes[2] == 'F' && bytes[3] == 'A') return "AST";
  if (bytes.size() >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ') return "DDS";
  if (bytes.size() >= 4 && bytes[0] == 'X' && bytes[1] == 'P' && bytes[2] == 'R' && bytes[3] == '2') return "XPR2";
  if (bytes.size() >= 3 && bytes[0] == 'R' && bytes[1] == 'S' && bytes[2] == 'F') return "RSF";
  if (bytes.size() >= 3 && (bytes[0] == 'P' || bytes[0] == 'p') && bytes[1] == '3' && bytes[2] == 'R') return "P3R";
  // "Apt constant file\x1A..." — EA APT CONST companion
  if (bytes.size() >= 8 && bytes[0] == 'A' && bytes[1] == 'p' && bytes[2] == 't' && bytes[3] == ' ' &&
      bytes[4] == 'c' && bytes[5] == 'o' && bytes[6] == 'n' && bytes[7] == 's') return "CONST";
  if (bytes.size() >= 5 && bytes[0] == '<' && bytes[1] == '?' && bytes[2] == 'x' && bytes[3] == 'm' && bytes[4] == 'l') return "XML";
  if (!bytes.empty() && bytes[0] == '<') return "XML";
  return {};
}

static QString uniqueOutputPath(const QString& outDir, const QString& baseName, const QString& extNoDot) {
  QDir dir(outDir);
  const QString ext = normalizeExt(extNoDot);
  const QString stem = sanitizeExportName(baseName);
  QString candidate = dir.filePath(ext.isEmpty() ? stem : (stem + "." + ext));
  if (!QFileInfo::exists(candidate)) return candidate;
  for (int i = 2; i < 10000; ++i) {
    candidate = dir.filePath(ext.isEmpty() ? QString("%1_%2").arg(stem).arg(i)
                                           : QString("%1_%2.%3").arg(stem).arg(i).arg(ext));
    if (!QFileInfo::exists(candidate)) return candidate;
  }
  return candidate;
}

static QString rsfInfoNameFromBytes(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 40) return {};
  auto be32 = [&](std::size_t off) -> std::uint32_t {
    if (off + 4 > bytes.size()) return 0;
    return (std::uint32_t(bytes[off]) << 24) |
           (std::uint32_t(bytes[off + 1]) << 16) |
           (std::uint32_t(bytes[off + 2]) << 8) |
           std::uint32_t(bytes[off + 3]);
  };
  if (!(bytes[0] == 'R' && bytes[1] == 'S' && bytes[2] == 'F')) return {};
  if (!(bytes[16] == 'I' && bytes[17] == 'N' && bytes[18] == 'F' && bytes[19] == 'O')) return {};
  const std::uint32_t nameLen = be32(32);
  if (nameLen == 0 || 36 + nameLen > bytes.size()) return {};
  return QString::fromLatin1(reinterpret_cast<const char*>(bytes.data() + 36), int(nameLen));
}

static int rsfGeomCountFromBytes(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 0x90) return 0;
  for (std::size_t off = 16; off + 32 <= bytes.size() && off < 512; ) {
    const bool isGeom = bytes[off] == 'G' && bytes[off + 1] == 'E' && bytes[off + 2] == 'O' && bytes[off + 3] == 'M';
    const std::uint32_t total = (std::uint32_t(bytes[off + 8]) << 24) |
                                (std::uint32_t(bytes[off + 9]) << 16) |
                                (std::uint32_t(bytes[off + 10]) << 8) |
                                std::uint32_t(bytes[off + 11]);
    if (isGeom) {
      return int((std::uint32_t(bytes[off + 16]) << 24) |
                 (std::uint32_t(bytes[off + 17]) << 16) |
                 (std::uint32_t(bytes[off + 18]) << 8) |
                 std::uint32_t(bytes[off + 19]));
    }
    if (total == 0 || off + total > bytes.size()) break;
    off += total;
  }
  return 0;
}

static bool isXmlLikeBytes(const QByteArray& bytes) {
  const QByteArray t = bytes.trimmed();
  return t.startsWith("<?xml") || t.startsWith("<");
}

static QString ensureHasExtension(QString path, const QString& extNoDot) {
  const QString ext = normalizeExt(extNoDot);
  if (path.isEmpty() || ext.isEmpty()) return path;
  QFileInfo fi(path);
  if (!fi.suffix().isEmpty()) return path;
  return path + "." + ext;
}

// Forward declarations for helpers used by status bar updates.
// (Definitions live further down in this file.)
static QString formatBytesHuman(qulonglong v);

// ------------------------------
// Crash-safety helpers
// ------------------------------
// QTreeWidgetItem is NOT a QObject, so capturing raw pointers into background-worker
// completion lambdas can easily become a use-after-free when the tree is rebuilt,
// when nodes are deleted, or when the app is closing.
//
// We avoid dereferencing stale pointers by re-finding the item by a stable cache key
// (path + embedded offset tuple) at completion time.
static QString makeTreeCacheKey(QTreeWidgetItem* it) {
  if (!it) return {};
  const QString path = it->data(0, Qt::UserRole).toString();
  if (path.isEmpty()) return {};
  const bool isEmbedded = it->data(0, Qt::UserRole + 3).toBool();
  if (!isEmbedded) return path;
  const qulonglong baseOffset = it->data(0, Qt::UserRole + 1).toULongLong();
  const qulonglong maxReadable = it->data(0, Qt::UserRole + 2).toULongLong();
  return QStringLiteral("%1@%2@%3").arg(path).arg(baseOffset).arg(maxReadable);
}

static QTreeWidgetItem* findTreeItemByCacheKey(QTreeWidget* tree, const QString& cacheKey) {
  if (!tree || cacheKey.isEmpty()) return nullptr;
  QVector<QTreeWidgetItem*> stack;
  stack.reserve(256);
  for (int i = 0; i < tree->topLevelItemCount(); ++i) {
    if (auto* t = tree->topLevelItem(i)) stack.push_back(t);
  }
  while (!stack.isEmpty()) {
    QTreeWidgetItem* it = stack.back();
    stack.pop_back();
    if (makeTreeCacheKey(it) == cacheKey) return it;
    for (int c = 0; c < it->childCount(); ++c) {
      if (auto* ch = it->child(c)) stack.push_back(ch);
    }
  }
  return nullptr;
}

using gf::gui::kSettingsOrg;
using gf::gui::kSettingsApp;
static constexpr const char* kSettingDevMode = "ui/dev_mode";
static constexpr const char* kSettingEditingEnabled = "ui/editing_enabled";

bool MainWindow::devModeEnabled() const {
  return m_devMode;
}

void MainWindow::setDevModeEnabled(bool enabled) {
  if (m_devMode == enabled) return;
  m_devMode = enabled;
  if (m_actDevMode) m_actDevMode->setChecked(enabled);
  QSettings s(kSettingsOrg, kSettingsApp);
  s.setValue(kSettingDevMode, enabled);
  updateStatusBar();
}

bool MainWindow::editingEnabled() const {
  return true;
}

void MainWindow::setEditingEnabled(bool enabled) {
  Q_UNUSED(enabled);
  m_editingEnabled = true;
  if (m_actEnableEditing) {
    m_actEnableEditing->setChecked(true);
    m_actEnableEditing->setVisible(false);
    m_actEnableEditing->setEnabled(false);
  }
  if (m_editModeLabel) m_editModeLabel->setText("Editing: ON");

  QSettings s(kSettingsOrg, kSettingsApp);
  s.setValue(kSettingEditingEnabled, true);

  // Editing is always on in the editor build.
  statusBar()->showMessage(enabled ? "Editing enabled (unsafe mode)" : "Read-only mode", 4000);
  if (m_rsfEditAction) {
    Q_UNUSED(enabled);
    m_rsfEditAction->setEnabled(false);
    m_rsfEditAction->setChecked(true);
    m_rsfEditAction->setVisible(false);
  }
  if (!enabled) setRsfDirty(false);

  updateDocumentActions();
  updateStatusBar();
}

void MainWindow::updateStatusSelection(QTreeWidgetItem* current) {
  m_statusContainerPath.clear();
  m_statusEntryName.clear();
  m_statusEntryType.clear();
  m_statusEntrySize = 0;
  m_statusEntryFlags = 0;

  if (!current) {
    updateStatusBar();
    return;
  }

  const QString path = current->data(0, Qt::UserRole).toString();
  if (!path.isEmpty()) {
    m_statusContainerPath = path;
    m_statusEntryName = current->text(0);
    m_statusEntryType = current->text(1);

    const QVariant sz = current->data(0, Qt::UserRole + 2);
    if (sz.isValid()) m_statusEntrySize = sz.toULongLong();

    const QVariant fl = current->data(0, Qt::UserRole + 4);
    if (fl.isValid()) m_statusEntryFlags = fl.toULongLong();
  } else {
    // Folder/bucket nodes: show only label.
    m_statusEntryName = current->text(0);
  }

  updateStatusBar();
}

void MainWindow::updateStatusBar() {
  if (!m_statusDocLabel || !m_statusEntryLabel || !m_statusMetaLabel || !m_statusDirtyLabel) return;

  // Prefer selection container path, fall back to document path.
  const QString docPath = !m_statusContainerPath.isEmpty() ? m_statusContainerPath : m_doc.path;

  const QFontMetrics fm(statusBar()->font());
  const int docMaxPx = 220;
  const int entryMaxPx = 260;

  if (!docPath.isEmpty()) {
    const QString fn = QFileInfo(docPath).fileName();
    m_statusDocLabel->setText(QString("AST: %1").arg(fm.elidedText(fn, Qt::ElideMiddle, docMaxPx)));
    m_statusDocLabel->setToolTip(QDir::toNativeSeparators(docPath));
  } else {
    m_statusDocLabel->setText("AST: (none)");
    m_statusDocLabel->setToolTip("No container loaded");
  }

  if (!m_statusEntryName.isEmpty()) {
    m_statusEntryLabel->setText(QString("Entry: %1").arg(fm.elidedText(m_statusEntryName, Qt::ElideMiddle, entryMaxPx)));
    m_statusEntryLabel->setToolTip(m_statusEntryName);
  } else {
    m_statusEntryLabel->setText("Entry: (none)");
    m_statusEntryLabel->setToolTip(QString());
  }

  QString meta;
  if (!m_statusEntryType.isEmpty()) {
    meta += QString("Type: %1").arg(m_statusEntryType);
  }
  if (m_statusEntrySize > 0) {
    if (!meta.isEmpty()) meta += "  |  ";
    meta += QString("Size: %1").arg(formatBytesHuman(m_statusEntrySize));
  }
  if (devModeEnabled() && m_statusEntryFlags != 0) {
    if (!meta.isEmpty()) meta += "  |  ";
    meta += QString("Flags: 0x%1").arg(QString::number(m_statusEntryFlags, 16).toUpper());
  }

  m_statusMetaLabel->setText(meta);
  m_statusMetaLabel->setVisible(!meta.isEmpty());

  m_statusDirtyLabel->setText(m_doc.dirty ? "Dirty" : "");
  m_statusDirtyLabel->setToolTip(m_doc.dirty ? "Unsaved changes" : "");
  m_statusDirtyLabel->setVisible(m_doc.dirty);
}

void MainWindow::showErrorDialog(const QString& title,
                                 const QString& message,
                                 const QString& details,
                                 bool noChangesSaved) {
  // Always log UI-visible errors so crashes/support sessions have a trail.
  {
    const std::string logMsg = (title + ": " + message).toStdString();
    const std::string logDetail = details.toStdString();
    gf::core::logError(gf::core::LogCategory::UI, logMsg,
                       logDetail.empty() ? std::string_view{} : std::string_view{logDetail});
  }

  QMessageBox box(this);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(title);

  QString text = message;
  if (noChangesSaved) {
    if (!text.endsWith('\n')) text += '\n';
    text += "\nNo changes were saved.";
  }
  box.setText(text);
  if (!details.isEmpty()) box.setDetailedText(details);
  box.exec();
}

void MainWindow::showInfoDialog(const QString& title, const QString& message) {
  QMessageBox::information(this, title, message);
}

void MainWindow::toastOk(const QString& message) {
  if (statusBar()) statusBar()->showMessage(message, 3000);
}

static QString cacheIdFromSeed(const QString& seed) {
  const QByteArray h = QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha1).toHex();
  return QString::fromLatin1(h);
}

// Stable key for caching metadata about embedded entries.
// Deterministic across runs for the same container file.
static QString cacheKeyForEmbedded(quint64 absOffset, std::uint64_t compressedSize) {
  // Include both absolute file offset and compressed size (offset alone can collide on some malformed indexes).
  return QString("emb:%1:%2").arg(absOffset).arg(static_cast<qulonglong>(compressedSize));
}


static QString fileNameOnly(const QString& p) {
  return QFileInfo(p).fileName();
}

static QString formatBytes(qulonglong v) {
  return QString("%1 bytes").arg(v);
}

static QString formatBytesHuman(qulonglong v) {
  const double dv = static_cast<double>(v);
  if (v >= 1024ull * 1024ull * 1024ull) return QString::number(dv / (1024.0 * 1024.0 * 1024.0), 'f', 2) + " GB";
  if (v >= 1024ull * 1024ull) return QString::number(dv / (1024.0 * 1024.0), 'f', 2) + " MB";
  if (v >= 1024ull) return QString::number(dv / 1024.0, 'f', 1) + " KB";
  return QString("%1 B").arg(v);
}

static QString ddsFormatToString(gf::textures::DdsFormat f) {
  using gf::textures::DdsFormat;
  switch (f) {
    case DdsFormat::DXT1: return "DXT1";
    case DdsFormat::DXT3: return "DXT3";
    case DdsFormat::DXT5: return "DXT5";
    case DdsFormat::ATI1: return "ATI1";
    case DdsFormat::ATI2: return "ATI2";
    case DdsFormat::RGBA32: return "RGBA32";
default: return "Unknown";
  }
}

static QString ddsStatusToString(gf::textures::DdsValidationStatus status) {
  using gf::textures::DdsValidationStatus;
  switch (status) {
    case DdsValidationStatus::Valid: return "valid";
    case DdsValidationStatus::UnsupportedValid: return "unsupported-but-valid";
    case DdsValidationStatus::Invalid: return "invalid";
    default: return "unknown";
  }
}

static QString summarizeDdsValidation(const gf::textures::DdsValidationResult& r) {
  return QString("status=%1 | format=%2 | %3x%4 | mips=%5 | payload=%6/%7 bytes")
      .arg(ddsStatusToString(r.status), QString::fromStdString(r.formatName).isEmpty() ? QString("Unknown") : QString::fromStdString(r.formatName))
      .arg(r.width).arg(r.height).arg(r.mipCount)
      .arg(qulonglong(r.payloadSize)).arg(qulonglong(r.expectedPayloadSizeMin));
}

static QString ddsValidationDetailsText(const gf::textures::DdsValidationResult& r) {
  QStringList lines;
  lines << QString("Status: %1").arg(ddsStatusToString(r.status));
  lines << QString("Kind: %1").arg(QString::fromLatin1(gf::textures::dds_kind_name(r.kind)));
  lines << QString("Format: %1").arg(QString::fromStdString(r.formatName).isEmpty() ? QString("Unknown") : QString::fromStdString(r.formatName));
  lines << QString("Dimensions: %1 x %2").arg(r.width).arg(r.height);
  lines << QString("Mip count: %1").arg(r.mipCount);
  lines << QString("Header size: %1").arg(r.headerSize);
  lines << QString("Pixel format size: %1").arg(r.pixelFormatSize);
  lines << QString("Wrapped header offset: %1").arg(qulonglong(r.dataOffset));
  lines << QString("Payload: actual %1 bytes, expected %2 bytes")
              .arg(qulonglong(r.payloadSize)).arg(qulonglong(r.expectedPayloadSizeMin));
  lines << QString("FourCC: 0x%1").arg(r.fourcc, 8, 16, QChar('0'));
  if (r.dx10HeaderPresent) lines << QString("DX10 format: %1").arg(r.dx10Format);
  if (r.issues.empty()) {
    lines << "Issues: none";
  } else {
    lines << "Issues:";
    for (const auto& issue : r.issues) {
      lines << QString("- [%1] %2 (%3)")
                  .arg(QString::fromLatin1(gf::textures::dds_issue_severity_name(issue.severity)),
                       QString::fromStdString(issue.message),
                       QString::fromStdString(issue.code));
    }
  }
  return lines.join("\n");
}



static QString p3rConversionDetailsText(const gf::textures::PreparedDdsExport& prep) {
  QStringList lines;
  lines << QString("Source recognized as P3R");
  lines << QString("Source first bytes: %1").arg(QString::fromStdString(prep.p3r.sourceMagic));
  lines << QString("DDS magic at offset 0: %1").arg(prep.p3r.ddsMagicAtZero ? "yes" : "no");
  lines << QString("DDS magic at non-zero offset: %1").arg(
      prep.p3r.ddsMagicWrapped
          ? QString("yes (%1)").arg(qulonglong(prep.p3r.ddsMagicOffset))
          : QString("no"));
  lines << QString("P3R magic at offset 0: %1").arg(prep.p3r.p3rMagicAtZero ? "yes" : "no");
  lines << QString("P3R magic at non-zero offset: %1").arg(
      prep.p3r.p3rMagicWrapped
          ? QString("yes (%1)").arg(qulonglong(prep.p3r.p3rMagicOffset))
          : QString("no"));
  lines << QString("DDS-like payload: %1").arg(prep.p3r.payloadLooksDdsLike ? "yes" : "no");
  lines << QString("BCn-like payload: %1").arg(prep.p3r.payloadLooksBcLike ? "yes" : "no");
  lines << QString("Parsed header: %1").arg(prep.p3r.parsedHeader ? "yes" : "no");
  if (prep.p3r.parsedHeader) {
    lines << QString("Parsed signature: %1").arg(QString::fromStdString(prep.p3r.parsedSignature));
    lines << QString("Parsed format: %1").arg(QString::fromStdString(prep.p3r.parsedFormatName));
    lines << QString("Parsed header size: %1").arg(prep.p3r.parsedHeaderSize);
    lines << QString("Parsed data offset: %1").arg(qulonglong(prep.p3r.parsedDataOffset));
    lines << QString("Parsed payload size: %1").arg(qulonglong(prep.p3r.parsedPayloadSize));
    if (!prep.p3r.parseIssues.empty()) {
      lines << "Parse issues:";
      for (const auto& issue : prep.p3r.parseIssues) {
        lines << QString(" - %1").arg(QString::fromStdString(issue));
      }
    }
  }

  if (!prep.p3r.successStage.empty()) {
    lines << QString("Succeeded stage: %1").arg(QString::fromStdString(prep.p3r.successStage));
  }

  if (!prep.error.empty()) {
    lines << QString("Overall error: %1").arg(QString::fromStdString(prep.error));
  }

  if (!prep.p3r.attempts.empty()) {
    lines << "Stages:";
    for (const auto& attempt : prep.p3r.attempts) {
      QString line = QString("- [%1] %2")
                         .arg(attempt.success ? "ok" : "fail",
                              QString::fromStdString(attempt.stage));
      if (attempt.offset != 0) {
        line += QString(" @ offset %1").arg(qulonglong(attempt.offset));
      }
      if (!attempt.message.empty()) {
        line += QString(": %1").arg(QString::fromStdString(attempt.message));
      }
      lines << line;

      if (!attempt.validation.summary.empty() || !attempt.validation.issues.empty()) {
        lines << QString("  %1")
                     .arg(ddsValidationDetailsText(attempt.validation).replace("\n", "\n  "));
      }
    }
  }

  return lines.join("\n");
}
static bool validateDdsForWrite(QWidget* parent,
                                const QString& title,
                                const QString& entryName,
                                std::span<const std::uint8_t> bytes,
                                QString* detailsOut = nullptr) {
  const auto result = gf::textures::inspect_dds(bytes);
  if (detailsOut) *detailsOut = ddsValidationDetailsText(result);
  if (auto lg = gf::core::Log::get(); lg) {
    lg->info("DDSValidation write_check: entry='{}' {}", entryName.toStdString(), summarizeDdsValidation(result).toStdString());
    for (const auto& issue : result.issues) {
      lg->info("DDSValidation issue: entry='{}' severity={} code={} message={}",
               entryName.toStdString(),
               gf::textures::dds_issue_severity_name(issue.severity),
               issue.code,
               issue.message);
    }
  }
  if (result.status == gf::textures::DdsValidationStatus::Invalid) {
    QMessageBox::critical(parent, title,
                          QString("Refusing to write an invalid DDS for '%1'.\n\n%2")
                              .arg(entryName, ddsValidationDetailsText(result)));
    return false;
  }
  return true;
}

static std::vector<std::uint8_t> p3rToDds(std::span<const std::uint8_t> in) {
  std::vector<std::uint8_t> out(in.begin(), in.end());
  if (out.size() >= 4) {
    out[0] = 'D';
    out[1] = 'D';
    out[2] = 'S';
    out[3] = ' '; // "DDS "
  }
  return out;
}

static std::vector<std::uint8_t> ddsToP3r(std::span<const std::uint8_t> in, std::uint8_t p3rVerByte = 0x02) {
  std::vector<std::uint8_t> out(in.begin(), in.end());
  if (out.size() >= 4 && out[0] == 'D' && out[1] == 'D' && out[2] == 'S' && out[3] == ' ') {
    // EASE's DDStoP3R hardcodes data[0]=112 (0x70, lowercase 'p').
    // The PS3 EA game loader only accepts this lowercase variant.
    // Uppercase 'P' (0x50) causes the texture manager to reject the header,
    // leaving 0x7FFFFFFF as the RSX texture offset -> fatal RSX crash.
    out[0] = 0x70; // 'p' lowercase
    out[1] = '3';
    out[2] = 'R';
    out[3] = p3rVerByte;
  }
  return out;
}

// Many EA textures (including some P3R payloads) store only an EA header + blocks
// without a standard DDS header. For those, rebuild a proper DDS blob first.
static std::vector<std::uint8_t> maybe_rebuild_ea_dds(std::span<const std::uint8_t> bytes, std::uint32_t astFlags = 0) {
  try {
    // If it already looks like a DDS header, just return it.
    if (bytes.size() >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ') {
      return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    }
    gf::textures::EaDdsInfo info{};
    if (auto rebuilt = gf::textures::rebuild_ea_dds(bytes, astFlags, &info)) {
      if (rebuilt->size() >= 4 && (*rebuilt)[0] == 'D' && (*rebuilt)[1] == 'D' && (*rebuilt)[2] == 'S' && (*rebuilt)[3] == ' ') {
        return *rebuilt;
      }
    }
  } catch (const std::exception& ex) {
    gf::core::logWarn(gf::core::LogCategory::DdsConversion,
                      "maybe_rebuild_ea_dds threw exception", ex.what());
  } catch (...) {
    gf::core::logWarn(gf::core::LogCategory::DdsConversion,
                      "maybe_rebuild_ea_dds threw unknown exception");
  }
  return {};
}

static std::optional<gf::textures::DdsInfo> tryParseTextureDdsInfo(std::span<const std::uint8_t> bytes, std::uint32_t astFlags = 0) {
  try {
    if (bytes.size() >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ') {
      return gf::textures::parse_dds_info(bytes);
    }

    if (bytes.size() >= 3 &&
        ((bytes[0] == 'p' && bytes[1] == '3' && bytes[2] == 'R') ||
         (bytes[0] == 'P' && bytes[1] == '3' && bytes[2] == 'R'))) {
      auto rebuilt = p3rToDds(bytes);
      if (auto info = gf::textures::parse_dds_info(std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size())); info.has_value()) {
        return info;
      }
    }

    auto rebuilt = maybe_rebuild_ea_dds(bytes, astFlags);
    if (!rebuilt.empty()) {
      return gf::textures::parse_dds_info(std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size()));
    }
  } catch (const std::exception& ex) {
    gf::core::logWarn(gf::core::LogCategory::DdsConversion,
                      "tryParseTextureDdsInfo threw exception", ex.what());
  } catch (...) {
    gf::core::logWarn(gf::core::LogCategory::DdsConversion,
                      "tryParseTextureDdsInfo threw unknown exception");
  }
  return std::nullopt;
}


static bool looks_like_zlib_cmf_flg(std::uint8_t cmf, std::uint8_t flg);
static std::vector<std::uint8_t> zlib_inflate_unknown_size(std::span<const std::uint8_t> in);
// Forward declaration — full definition lives near buildPreviewContextForItem.
// Returns true when `item` is a leaf inside a nested embedded sub-AST, meaning
// its UserRole+6 entry-index is scoped to the inner sub-AST and must NOT be
// used with the outer on-disk AstContainerEditor (m_liveAstEditor).
static bool itemIsNestedSubEntry(const QTreeWidgetItem* item);
// Forward declaration — full definition lives near the preview helpers below.
static bool looksZlibPreviewBytes(const QByteArray& b);
// Forward declaration — reads `size` bytes from `path` starting at `offset`.
static std::vector<std::uint8_t> read_file_range(const QString& path, std::uint64_t offset, std::uint64_t size);

struct ResolvedTexturePayload {
  std::vector<std::uint8_t> bytes;          // effective payload for texture work
  std::vector<std::uint8_t> rawBytes;       // authoritative stored/current-entry bytes
  std::vector<std::uint8_t> inflatedBytes;  // only populated when the entry itself is actually zlib-backed
  QString source;
  QString rawSource;
  QString inflatedSource;
  std::size_t rawSize = 0;
  bool usedPreview = false;
  bool usedPending = false;
  bool usedInflate = false;
  bool entryLooksCompressed = false;
};

static QString hexSignaturePrefix(std::span<const std::uint8_t> bytes, std::size_t count = 8) {
  const std::size_t n = std::min<std::size_t>(bytes.size(), count);
  QStringList parts;
  parts.reserve(static_cast<int>(n));
  for (std::size_t i = 0; i < n; ++i) {
    parts << QString("%1").arg(bytes[i], 2, 16, QChar('0')).toUpper();
  }
  return parts.join(' ');
}

static ResolvedTexturePayload resolveTexturePayloadForEditor(QTreeWidgetItem* item,
                                                             const QString& type,
                                                             gf::core::AstContainerEditor& editor,
                                                             std::uint32_t entryIndex) {
  ResolvedTexturePayload out;

  auto copyByteArray = [](const QByteArray& in) {
    std::vector<std::uint8_t> out;
    if (!in.isEmpty()) {
      out.resize(static_cast<std::size_t>(in.size()));
      std::memcpy(out.data(), in.constData(), static_cast<std::size_t>(in.size()));
    }
    return out;
  };

  // Pending replacement bytes always win regardless of nesting.
  if (item) {
    const QVariant pendingVar = item->data(0, Qt::UserRole + 30);
    if (pendingVar.isValid()) {
      const QByteArray pending = pendingVar.toByteArray();
      if (!pending.isEmpty()) {
        out.rawBytes = copyByteArray(pending);
        out.rawSource = QStringLiteral("tree.pendingBytes");
        out.rawSize = out.rawBytes.size();
        out.usedPending = true;
      }
    }
  }

  // For nested sub-entries the supplied `editor` is the OUTER on-disk container
  // and `entryIndex` is relative to the INNER sub-AST directory.  Using the editor
  // here would silently return the wrong outer entry (e.g. a tiny XML stub).
  // Instead, read directly from the file using the absolute payload offset that
  // was stored in UserRole+1 when the inner-AST children were created.
  const bool isNested = itemIsNestedSubEntry(item);

  if (out.rawBytes.empty() && !isNested) {
    if (auto storedOpt = editor.getEntryStoredBytes(entryIndex); storedOpt.has_value() && !storedOpt->empty()) {
      out.rawBytes = *storedOpt;
      out.rawSource = QStringLiteral("editor.getEntryStoredBytes");
      out.rawSize = out.rawBytes.size();
    } else if (auto storedOpt2 = editor.getEntryStoredBytes(entryIndex); storedOpt2.has_value()) {
      out.rawSource = QStringLiteral("editor.getEntryStoredBytes");
      out.rawSize = storedOpt2->size();
    }
  }

  // Direct file-range read: used for nested sub-entries and as a fallback when
  // the editor returned nothing useful.  UserRole+1 is the absolute byte offset
  // of the payload in the on-disk file; UserRole+2 is the compressed size.
  if (out.rawBytes.empty() && item) {
    const QString filePath = item->data(0, Qt::UserRole).toString();
    const quint64 absOff   = item->data(0, Qt::UserRole + 1).toULongLong();
    const quint64 compSz   = item->data(0, Qt::UserRole + 2).toULongLong();
    if (!filePath.isEmpty() && compSz > 0) {
      auto raw = read_file_range(filePath,
                                 static_cast<std::uint64_t>(absOff),
                                 static_cast<std::uint64_t>(compSz));
      if (!raw.empty()) {
        out.rawBytes = std::move(raw);
        out.rawSource = isNested
            ? QStringLiteral("nested.file.range")
            : QStringLiteral("fallback.file.range");
        out.rawSize = out.rawBytes.size();
      }
    }
  }

  out.bytes = out.rawBytes;
  out.source = out.rawSource;

  const bool isZlibType = (type.compare("ZLIB", Qt::CaseInsensitive) == 0);
  const bool looksZlib = (out.rawBytes.size() >= 2 && looks_like_zlib_cmf_flg(out.rawBytes[0], out.rawBytes[1]));
  out.entryLooksCompressed = isZlibType || looksZlib;

  if (out.entryLooksCompressed) {
    // Only use the editor inflate path when not nested (same scoping rule).
    std::string inflateErr;
    if (!isNested) {
      if (auto inflatedOpt = editor.getEntryInflatedBytes(entryIndex, &inflateErr); inflatedOpt.has_value() && !inflatedOpt->empty()) {
        out.inflatedBytes = *inflatedOpt;
        out.inflatedSource = QStringLiteral("editor.getEntryInflatedBytes");
        out.bytes = out.inflatedBytes;
        out.source = out.inflatedSource;
        out.usedInflate = true;
      }
    }
    if (!out.usedInflate && !out.rawBytes.empty() && !isZlibType && looksZlib) {
      try {
        out.inflatedBytes = zlib_inflate_unknown_size(std::span<const std::uint8_t>(out.rawBytes.data(), out.rawBytes.size()));
        out.inflatedSource = isNested
            ? QStringLiteral("nested.file.range+zlib.inflate")
            : QStringLiteral("editor.getEntryStoredBytes+zlib.inflate");
        out.bytes = out.inflatedBytes;
        out.source = out.inflatedSource;
        out.usedInflate = true;
      } catch (...) {
      }
    }
  }

  if (out.bytes.empty() && !out.rawBytes.empty()) {
    out.bytes = out.rawBytes;
    out.source = out.rawSource;
  }

  return out;
}


static bool textureTypePrefersStoredBytes(const QString& typeUpper) {
  return typeUpper == "DDS" || typeUpper == "XPR" || typeUpper == "XPR2" || typeUpper.startsWith("P3R");
}

static QString summarizeResolvedTexturePayload(const ResolvedTexturePayload& payload) {
  return QString("source=%1 | raw=%2 | effective=%3 | inflated=%4 | pending=%5 | inflatedUsed=%6 | sig=[%7]")
      .arg(payload.source.isEmpty() ? QStringLiteral("<none>") : payload.source)
      .arg(qulonglong(payload.rawSize))
      .arg(qulonglong(payload.bytes.size()))
      .arg(qulonglong(payload.inflatedBytes.size()))
      .arg(payload.usedPending ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(payload.usedInflate ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(hexSignaturePrefix(std::span<const std::uint8_t>(payload.bytes.data(), payload.bytes.size())));
}

static std::optional<std::vector<std::uint8_t>> buildDdsForTextureExport(const QString& entryTypeUpper,
                                                                         const ResolvedTexturePayload& payload,
                                                                         std::uint32_t astFlags,
                                                                         QString* detailsOut = nullptr) {
  QStringList details;
  details << QString("Entry type: %1").arg(entryTypeUpper);
  details << QString("Resolved payload: %1").arg(summarizeResolvedTexturePayload(payload));

  if (payload.bytes.empty()) {
    details << QStringLiteral("Resolved payload is empty.");
    if (detailsOut) *detailsOut = details.join('\n');
    return std::nullopt;
  }

  if (entryTypeUpper == "DDS") {
    std::vector<std::uint8_t> out = payload.bytes;
    if (detailsOut) *detailsOut = details.join('\n');
    return out;
  }

  if (entryTypeUpper.startsWith("P3R")) {
    std::span<const std::uint8_t> texBytes(payload.bytes.data(), payload.bytes.size());

    auto swapped = p3rToDds(texBytes);
    if (swapped.size() >= 4 && std::memcmp(swapped.data(), "DDS ", 4) == 0) {
      const auto validation = gf::textures::inspect_dds(std::span<const std::uint8_t>(swapped.data(), swapped.size()));
      if (validation.is_valid()) {
        details << QStringLiteral("Conversion stage: direct legacy P3R magic/header swap");
        if (detailsOut) *detailsOut = details.join('\n');
        return swapped;
      }
    }

    if (auto rebuilt = maybe_rebuild_ea_dds(texBytes, astFlags); !rebuilt.empty()) {
      details << QStringLiteral("Conversion stage: fallback EA rebuild");
      if (detailsOut) *detailsOut = details.join('\n');
      return rebuilt;
    }

    const auto prep = gf::textures::prepare_texture_dds_for_export(texBytes, true, astFlags);
    if (prep.ok()) {
      details << QStringLiteral("Conversion stage: prepare_texture_dds_for_export");
      details << p3rConversionDetailsText(prep);
      if (detailsOut) *detailsOut = details.join('\n');
      return prep.ddsBytes;
    }

    details << p3rConversionDetailsText(prep);
    if (auto rebuilt = maybe_rebuild_ea_dds(texBytes, astFlags); !rebuilt.empty()) {
      details << QStringLiteral("Conversion stage: late fallback EA rebuild");
      if (detailsOut) *detailsOut = details.join('\n');
      return rebuilt;
    }

    if (detailsOut) *detailsOut = details.join('\n');
    return std::nullopt;
  }

  if (entryTypeUpper == "XPR2" || entryTypeUpper == "XPR") {
    std::string xprErr;
    auto dds = gf::textures::rebuild_xpr2_dds_first(
        std::span<const std::uint8_t>(payload.bytes.data(), payload.bytes.size()),
        &xprErr, /*all_mips=*/true);
    if (dds.has_value() && dds->size() >= 4 && std::memcmp(dds->data(), "DDS ", 4) == 0) {
      details << QStringLiteral("Conversion stage: rebuild_xpr2_dds_first(all_mips=true)");
      if (detailsOut) *detailsOut = details.join('\n');
      return dds;
    }
    details << QString("XPR2 decode failed: %1").arg(QString::fromStdString(xprErr));
    if (detailsOut) *detailsOut = details.join('\n');
    return std::nullopt;
  }

  auto rebuilt = maybe_rebuild_ea_dds(std::span<const std::uint8_t>(payload.bytes.data(), payload.bytes.size()), astFlags);
  if (!rebuilt.empty()) {
    details << QStringLiteral("Conversion stage: generic EA rebuild");
    if (detailsOut) *detailsOut = details.join('\n');
    return rebuilt;
  }

  if (detailsOut) *detailsOut = details.join('\n');
  return std::nullopt;
}

static QString validateImportedDdsForEntry(const QString& entryTypeUpper,
                                           const gf::textures::DdsValidationResult& validation,
                                           const std::optional<gf::textures::DdsInfo>& currentInfo,
                                           const std::optional<gf::textures::DdsInfo>& importedInfo) {
  QStringList issues;
  if (validation.status != gf::textures::DdsValidationStatus::Valid) {
    issues << QString("DDS header/status is not fully supported: %1").arg(ddsStatusToString(validation.status));
  }
  if (validation.dx10HeaderPresent) {
    issues << QString("DX10 DDS headers are not accepted for %1 replacement.").arg(entryTypeUpper);
  }
  if (!importedInfo.has_value()) {
    issues << QString("The replacement DDS could not be parsed into a supported texture description.");
  }
  if (currentInfo.has_value() && importedInfo.has_value()) {
    if (currentInfo->width != importedInfo->width) issues << QString("Width mismatch: current %1 vs import %2").arg(currentInfo->width).arg(importedInfo->width);
    if (currentInfo->height != importedInfo->height) issues << QString("Height mismatch: current %1 vs import %2").arg(currentInfo->height).arg(importedInfo->height);
    if (currentInfo->mipCount != importedInfo->mipCount) issues << QString("Mip count mismatch: current %1 vs import %2").arg(currentInfo->mipCount).arg(importedInfo->mipCount);
    if (currentInfo->format != importedInfo->format) {
      issues << QString("Format mismatch: current %1 vs import %2")
                    .arg(ddsFormatToString(currentInfo->format), ddsFormatToString(importedInfo->format));
    }
  }
  return issues.join("\n");
}

static QString ddsInfoSummary(const gf::textures::DdsInfo& info) {
  return QString("%1x%2 (%3, %4 mip%5)")
      .arg(info.width)
      .arg(info.height)
      .arg(ddsFormatToString(info.format))
      .arg(info.mipCount)
      .arg(info.mipCount == 1 ? "" : "s");
}

static bool looks_like_zlib_cmf_flg(std::uint8_t cmf, std::uint8_t flg) {
  const int cm = (cmf & 0x0F);
  const int cinfo = (cmf >> 4);
  const bool fdict = ((flg & 0x20) != 0);
  if (cm != 8) return false;
  if (cinfo > 7) return false;
  if (fdict) return false;
  const int v = (static_cast<int>(cmf) << 8) | static_cast<int>(flg);
  return (v % 31) == 0;
}

static std::vector<std::uint8_t> zlib_inflate_unknown_size(std::span<const std::uint8_t> in) {
  z_stream zs{};
  zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
  zs.avail_in = static_cast<uInt>(std::min<std::size_t>(in.size(), static_cast<std::size_t>(std::numeric_limits<uInt>::max())));

  int rc = ::inflateInit2(&zs, 15);
  if (rc != Z_OK) throw std::runtime_error("inflateInit2 failed");

  std::vector<std::uint8_t> out;
  out.reserve(in.size() * 2 + 1024);
  std::uint8_t tmp[64 * 1024];
  while (true) {
    zs.next_out = reinterpret_cast<Bytef*>(tmp);
    zs.avail_out = static_cast<uInt>(sizeof(tmp));
    rc = ::inflate(&zs, Z_NO_FLUSH);
    const std::size_t produced = sizeof(tmp) - zs.avail_out;
    if (produced) out.insert(out.end(), tmp, tmp + produced);
    if (rc == Z_STREAM_END) break;
    if (rc != Z_OK) { ::inflateEnd(&zs); throw std::runtime_error("inflate failed"); }
  }
  ::inflateEnd(&zs);
  return out;
}

static std::vector<std::uint8_t> read_file_range(const QString& path, std::uint64_t offset, std::uint64_t size) {
  std::ifstream f(path.toStdString(), std::ios::binary);
  if (!f) return {};
  f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  std::vector<std::uint8_t> buf;
  buf.resize(static_cast<std::size_t>(size));
  f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
  const auto got = f.gcount();
  if (got <= 0) return {};
  buf.resize(static_cast<std::size_t>(got));
  return buf;
}

static std::vector<std::uint8_t> read_payload_maybe_inflate(const QString& path, std::uint64_t absOffset, std::uint64_t compSize) {
  auto stored = read_file_range(path, absOffset, compSize);
  if (stored.size() >= 2 && looks_like_zlib_cmf_flg(stored[0], stored[1])) {
    try {
      return zlib_inflate_unknown_size(std::span<const std::uint8_t>(stored.data(), stored.size()));
    } catch (...) {
      return stored;
    }
  }
  return stored;
}

static std::string trim_ext_and_path(std::string s) {
  // normalize separators
  for (auto& ch : s) {
    if (ch == '\\') ch = '/';
  }
  const auto slash = s.find_last_of('/');
  if (slash != std::string::npos) s = s.substr(slash + 1);
  const auto dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  return s;
}

static std::vector<std::string> extract_ident_strings(std::span<const std::uint8_t> bytes,
                                                      std::size_t minLen = 4,
                                                      std::size_t maxLen = 48) {
  std::vector<std::string> out;
  out.reserve(64);
  std::string cur;
  auto flush = [&]() {
    if (cur.size() >= minLen && cur.size() <= maxLen) out.push_back(cur);
    cur.clear();
  };
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const unsigned char c = bytes[i];
    const bool ok = (std::isalnum(c) != 0) || c == '_' || c == '/' || c == '\\' || c == '.';
    if (ok) {
      if (cur.size() < maxLen + 8) cur.push_back(static_cast<char>(c));
    } else {
      flush();
    }
  }
  flush();
  return out;
}

static std::string pick_best_apt_name(std::span<const std::uint8_t> payload) {
  // Heuristic: prefer exported symbol-ish names found in string pools.
  const auto strs = extract_ident_strings(payload, 4, 32);
  std::unordered_map<std::string, int> score;
  score.reserve(strs.size());

  auto isGood = [](const std::string& s) {
    if (s.size() < 4 || s.size() > 32) return false;
    if (!(std::isalpha(static_cast<unsigned char>(s[0])))) return false;
    // avoid obvious junk
    if (s == "constantpool" || s == "movieclip" || s == "exports" || s == "frames") return false;
    return true;
  };

  for (const auto& s : strs) {
    std::string t = trim_ext_and_path(s);
    if (!isGood(t)) continue;
    int add = 1;
    if (t.find("UI") != std::string::npos) add += 2;
    if (t.find("Score") != std::string::npos || t.find("score") != std::string::npos) add += 2;
    if (std::isupper(static_cast<unsigned char>(t[0]))) add += 1;
    score[t] += add;
  }

  std::string best;
  int bestScore = 0;
  for (const auto& kv : score) {
    if (kv.second > bestScore) {
      bestScore = kv.second;
      best = kv.first;
    }
  }
  return best;
}

struct FriendlyOverrides {
  std::unordered_map<std::uint32_t, std::string> byEntryIndex;
};

static FriendlyOverrides build_friendly_overrides_v2(const gf::core::AstArchive::Index& idx,
                                                     const QString& parentPath,
                                                     std::uint64_t baseOffset) {
  FriendlyOverrides ov;

  // ---- RSF: texture filename/name list
  std::vector<std::string> rsfTextureNames;
  rsfTextureNames.reserve(256);

  // ---- APT: per-entry label
  for (const auto& e : idx.entries) {
    if (e.type_hint != "RSF") continue;
    const std::uint64_t abs = baseOffset + e.data_offset;
    const auto payload = read_payload_maybe_inflate(parentPath, abs, e.compressed_size);
    if (payload.empty()) continue;
    const auto docOpt = gf::models::rsf::parse(std::span<const std::uint8_t>(payload.data(), payload.size()));
    if (!docOpt) continue;
    for (const auto& t : docOpt->textures) {
      std::string s = !t.filename.empty() ? t.filename : t.name;
      s = trim_ext_and_path(s);
      if (!s.empty()) rsfTextureNames.push_back(s);
    }
  }

  // Map RSF texture list -> unnamed DDS entries in appearance order.
  if (!rsfTextureNames.empty()) {
    std::size_t nameIt = 0;
    for (const auto& e : idx.entries) {
      if (nameIt >= rsfTextureNames.size()) break;
      const bool isDds = (e.type_hint == "DDS") || (e.ext_hint == ".dds");
      if (!isDds) continue;
      const bool alreadyNamed = (!e.display_name.empty() || !e.name.empty());
      if (alreadyNamed) continue;
      ov.byEntryIndex[e.index] = rsfTextureNames[nameIt++];
    }
  }

  // APT naming: use embedded string pools to pick best export-ish identifier.
  for (const auto& e : idx.entries) {
    const bool isApt = (e.type_hint == "APT" || e.type_hint == "CONST") || (e.ext_hint == ".apt" || e.ext_hint == ".const");
    if (!isApt) continue;
    const bool alreadyNamed = (!e.display_name.empty() || !e.name.empty());
    if (alreadyNamed) continue;
    const std::uint64_t abs = baseOffset + e.data_offset;
    const auto payload = read_payload_maybe_inflate(parentPath, abs, e.compressed_size);
    if (payload.empty()) continue;
    const std::string best = pick_best_apt_name(std::span<const std::uint8_t>(payload.data(), payload.size()));
    if (!best.empty()) ov.byEntryIndex[e.index] = best;
  }

  return ov;
}

static bool isLikelyQklAstPath(const QString& path) {
  const QString base = QFileInfo(path).fileName().toLower();
  return base.startsWith("qkl_") && base.endsWith(".ast");
}

static bool isQklStyleFriendlyBase(const QString& base) {
  const QString trimmed = base.trimmed().toLower();
  return trimmed.startsWith("qkl_");
}

static bool isBinaryRsfBytes(const QByteArray& bytes) {
  return bytes.size() >= 4 && bytes.mid(0, 4) == QByteArray("RSF\0", 4);
}

static QIcon loadFileTypeIcon(const QString& typeKey, bool folder = false) {
  const QString normalized = folder ? QStringLiteral("folder") : typeKey.trimmed().toLower();
  static QMap<QString, QIcon> cache;
  const QString cacheKey = (folder ? QStringLiteral("folder") : QStringLiteral("file:")) + normalized;
  auto it = cache.find(cacheKey);
  if (it != cache.end()) return it.value();

  const QString appDir = QCoreApplication::applicationDirPath();
  const QStringList bases = {
      appDir + QStringLiteral("/game_icons/filetypes"),
      appDir + QStringLiteral("/game_icons"),
      QDir::currentPath() + QStringLiteral("/game_icons/filetypes"),
      QDir::currentPath() + QStringLiteral("/game_icons")
  };
  const QStringList exts = {QStringLiteral("png"), QStringLiteral("svg"), QStringLiteral("ico"), QStringLiteral("webp"), QStringLiteral("jpg"), QStringLiteral("jpeg")};

  QIcon icon;
  for (const QString& base : bases) {
    for (const QString& ext : exts) {
      const QString candidate = base + QLatin1Char('/') + normalized + QLatin1Char('.') + ext;
      if (QFileInfo::exists(candidate)) {
        icon = QIcon(candidate);
        if (!icon.isNull()) break;
      }
      const QString upperCandidate = base + QLatin1Char('/') + normalized.toUpper() + QLatin1Char('.') + ext;
      if (QFileInfo::exists(upperCandidate)) {
        icon = QIcon(upperCandidate);
        if (!icon.isNull()) break;
      }
    }
    if (!icon.isNull()) break;
  }

  if (icon.isNull()) {
    QStyle* st = QApplication::style();
    if (st) {
      if (folder) icon = st->standardIcon(QStyle::SP_DirIcon);
      else if (normalized == QStringLiteral("ast")) icon = st->standardIcon(QStyle::SP_DriveHDIcon);
      else if (normalized == QStringLiteral("rsf") || normalized == QStringLiteral("xml")) icon = st->standardIcon(QStyle::SP_FileDialogDetailedView);
      else if (normalized == QStringLiteral("p3r") || normalized == QStringLiteral("dds") || normalized == QStringLiteral("xpr") || normalized == QStringLiteral("xpr2")) icon = st->standardIcon(QStyle::SP_FileIcon);
      else if (normalized == QStringLiteral("zlib")) icon = st->standardIcon(QStyle::SP_DialogSaveButton);
      else icon = st->standardIcon(QStyle::SP_FileIcon);
    }
  }

  cache.insert(cacheKey, icon);
  return icon;
}

static void applyItemIcon(QTreeWidgetItem* item, const QString& /*typeKey*/, bool /*folder*/ = false) {
  if (!item) return;
  // Performance mode default: skip per-item icons in very large AST trees.
  item->setIcon(0, QIcon());
}

static QTreeWidgetItem* ensureGroup(QTreeWidgetItem* parent, const QString& name) {
  if (!parent) return nullptr;
  for (int i = 0; i < parent->childCount(); ++i) {
    auto* c = parent->child(i);
    if (c && c->text(0) == name && c->data(0, Qt::UserRole + 50).toBool()) { applyItemIcon(c, "FOLDER", true); return c; }
  }
  auto* g = new QTreeWidgetItem(QStringList() << name << "" << "");
  g->setFlags(g->flags() & ~Qt::ItemIsSelectable);
  g->setData(0, Qt::UserRole + 50, true); // group marker
  applyItemIcon(g, "FOLDER", true);
  parent->addChild(g);
  return g;
}

static QString deriveUiGroupFromExportBase(QString base) {
  base = base.trimmed();

  // Strip "(File_XXXXX...)" suffix if still present.
  base = base.section('(', 0, 0).trimmed();

  // Strip file extension only if it looks like a real extension at the end.
  if (base.endsWith(".ast", Qt::CaseInsensitive)) base.chop(4);
  else if (base.endsWith(".const", Qt::CaseInsensitive)) base.chop(5);
  else if (base.endsWith(".apt", Qt::CaseInsensitive)) base.chop(4);

  const QString lower = base.toLower();

  // Conservative domain grouping. Expand later as we learn more namespaces.
  if (lower.startsWith("ncaa.art.dynamo.") || lower.startsWith("packages.ncaa.art.dynamo."))
    return "Dynamo UI";
  if (lower.startsWith("packages.ncaa.art."))
    return "UI Packages";
  if (lower.startsWith("ncaa.art."))
    return "UI (ncaa.art)";

  return "UI (Other)";
}

static QString classifyEmbeddedIndexKind(const gf::core::AstArchive::Index& idx) {
  std::size_t tex = 0, ui = 0, audio = 0, models = 0, other = 0;
  for (const auto& e : idx.entries) {
    const std::string t = e.type_hint;
    if (t == "DDS" || t == "P3R" || t == "XPR" || t == "XPR2") ++tex;
    else if (t == "APT" || t == "CONST" || t == "XML") ++ui;
    else if (t == "OGG" || t == "RIFF") ++audio;
    else if (t == "RSF" || t == "RSG" || t == "STRM") ++models;
    else ++other;
  }
  const std::size_t maxv = std::max({tex, ui, audio, models, other});
  if (maxv == 0) return "Misc";
  if (maxv == tex) return "Textures";
  if (maxv == ui) return "UI";
  if (maxv == audio) return "Audio";
  if (maxv == models) return "Models";
  return "Misc";



}


static QString trimTextureFriendlyBase(QString b) {
  const int dot = b.lastIndexOf('.');
  if (dot > 0) b = b.left(dot);
  auto trimSuffix = [&](const QString& sfx) {
    if (b.endsWith(sfx, Qt::CaseInsensitive)) b.chop(sfx.size());
  };
  trimSuffix("_COL");
  trimSuffix("_VECTOR");
  trimSuffix("_NORM");
  trimSuffix("_TRAN");
  trimSuffix("_ALPHA");
  trimSuffix("_MASK");
  return b.trimmed();
}

static std::tuple<QString, QString, bool> deriveFriendlyAstContainerMeta(const gf::core::AstArchive::Index& idx) {
  QStringList rsfBases;
  QStringList texBases;
  QStringList aptBases;
  bool hasApt = false;

  for (const auto& ee : idx.entries) {
    const QString t = ee.type_hint.empty() ? QString() : QString::fromStdString(ee.type_hint).toUpper();
    const QString n = !ee.display_name.empty() ? QString::fromStdString(ee.display_name)
                      : (!ee.name.empty() ? QString::fromStdString(ee.name) : QString());

    if (t == "APT" || t == "CONST" || n.endsWith(".apt", Qt::CaseInsensitive) || n.endsWith(".const", Qt::CaseInsensitive)) {
      hasApt = true;
      QString b = n;
      if (b.endsWith(".apt", Qt::CaseInsensitive)) b.chop(4);
      if (b.endsWith(".const", Qt::CaseInsensitive)) b.chop(6);
      if (!b.isEmpty() && !b.startsWith("File_", Qt::CaseInsensitive)) aptBases << b;
    }

    if (t == "RSF" || n.endsWith(".rsf", Qt::CaseInsensitive)) {
      QString b = n;
      if (b.endsWith(".rsf", Qt::CaseInsensitive)) b.chop(4);
      if (!b.isEmpty()) rsfBases << b;
      continue;
    }

    if (t == "P3R" || t == "DDS" || t == "XPR" || t == "XPR2" ||
        n.endsWith(".p3r", Qt::CaseInsensitive) || n.endsWith(".dds", Qt::CaseInsensitive)) {
      const QString b = trimTextureFriendlyBase(n);
      if (!b.isEmpty()) texBases << b;
    }
  }

  QString chosen;
  auto mostFrequent = [](const QStringList& vals) -> QString {
    QHash<QString, int> freq;
    for (const auto& v : vals) freq[v] += 1;
    QString best;
    int bestN = -1;
    for (auto it = freq.begin(); it != freq.end(); ++it) {
      if (it.value() > bestN) {
        bestN = it.value();
        best = it.key();
      }
    }
    return best;
  };

  if (!aptBases.isEmpty()) chosen = mostFrequent(aptBases);
  if (chosen.isEmpty() && !rsfBases.isEmpty()) chosen = mostFrequent(rsfBases);
  if (chosen.isEmpty() && !texBases.isEmpty()) {
    chosen = texBases.first();
    for (const auto& b : texBases) {
      if (b.size() < chosen.size()) chosen = b;
    }
  }

  return {chosen, classifyEmbeddedIndexKind(idx), hasApt};
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(m_doc.makeWindowTitle("ASTra Core", "AST Editor"));
  setWindowIcon(QIcon(":/icons/astra.png"));

  // Persisted UI toggles
  {
    QSettings s(kSettingsOrg, kSettingsApp);
    m_devMode = s.value(kSettingDevMode, false).toBool();
    m_editingEnabled = true;
  }

  buildUi();

  // Tool is always dark-mode (no toggle). We don't force a palette here because
  // you may already be applying a global dark style in main().
}


void MainWindow::closeEvent(QCloseEvent* e) {
  // v0.6.4: unify dirty-close prompt with other editors.
  if (!DocumentLifecycle::maybePromptDiscard(this, m_doc.dirty)) {
    e->ignore();
    return;
  }

  // Persist friendly-name cache so derived AST names survive reopen even if
  // they were learned during this session and no explicit save path was hit.
  m_nameCache.save();
  e->accept();
}

void MainWindow::resizeEvent(QResizeEvent* e) {
  QMainWindow::resizeEvent(e);
  if (m_textureOriginal.isNull() || !m_imageView) return;

  // Only refit if we are on the Texture tab.
  if (m_viewTabs && m_viewTabs->currentIndex() != 2) return;

  if (m_textureFitToView) applyTextureZoom();
}

void MainWindow::applyTextureZoom() {
  if (!m_imageView || m_textureOriginal.isNull()) return;

  if (m_textureFitToView) {
    const QSize bounds = (m_imageScroll ? m_imageScroll->viewport()->size() : m_imageView->size()) - QSize(12, 12);
    if (bounds.width() > 32 && bounds.height() > 32) {
      m_imageView->setPixmap(m_textureOriginal.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation));
      m_imageView->resize(m_imageView->pixmap(Qt::ReturnByValue).size());
    } else {
      m_imageView->setPixmap(m_textureOriginal);
      m_imageView->resize(m_textureOriginal.size());
    }
    return;
  }

  const QSize scaledSize = m_textureOriginal.size() * m_textureZoom;
  if (scaledSize.width() > 0 && scaledSize.height() > 0) {
    QPixmap scaled = m_textureOriginal.scaled(scaledSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_imageView->setPixmap(scaled);
    m_imageView->resize(scaled.size());
  } else {
    m_imageView->setPixmap(m_textureOriginal);
    m_imageView->resize(m_textureOriginal.size());
  }
}

void MainWindow::resetTextureZoomToFit() {
  m_textureFitToView = true;
  m_textureZoom = 1.0;
  applyTextureZoom();
}

void MainWindow::populateTextureMipSelector(int mipCount) {
  if (!m_textureMipSelector) return;
  const bool old = m_textureMipSelector->blockSignals(true);
  m_textureMipSelector->clear();
  m_currentTextureMipCount = std::max(0, mipCount);
  for (int i = 0; i < m_currentTextureMipCount; ++i) {
    m_textureMipSelector->addItem(QString("Mip %1").arg(i), i);
  }
  const bool enabled = (m_currentTextureMipCount > 1);
  m_textureMipSelector->setEnabled(enabled);
  m_textureMipSelector->setVisible(m_currentTextureMipCount > 0);
  if (m_currentTextureMipCount > 0) {
    m_textureMipSelector->setCurrentIndex(std::clamp(m_currentTextureMipShown, 0, m_currentTextureMipCount - 1));
  }
  m_textureMipSelector->blockSignals(old);
}

bool MainWindow::renderCurrentTextureMip(int mipIndex) {
  if (!m_imageView || m_currentTextureBytes.isEmpty()) return false;
  if (m_currentTextureSelectionVersion == 0 || m_currentTextureSelectionVersion != m_previewContext.selectionVersion) return false;
  const auto bytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(m_currentTextureBytes.constData()), static_cast<std::size_t>(m_currentTextureBytes.size()));
  auto info = gf::textures::parse_dds_info(bytes);
  if (!info.has_value()) return false;
  if (mipIndex < 0) mipIndex = 0;
  if (mipIndex >= static_cast<int>(info->mipCount)) mipIndex = static_cast<int>(info->mipCount) - 1;
  if (mipIndex < 0) mipIndex = 0;
  auto img = gf::textures::decode_dds_mip_rgba(bytes, static_cast<std::uint32_t>(mipIndex));
  if (!img.has_value()) return false;

  const auto& im = img.value();
  QImage qimg(im.rgba.data(), static_cast<int>(im.width), static_cast<int>(im.height), QImage::Format_RGBA8888);
  m_textureOriginal = QPixmap::fromImage(qimg.copy());
  m_imageView->setPixmap(m_textureOriginal);
  m_currentTextureMipShown = mipIndex;
  resetTextureZoomToFit();
  if (m_textureInfo) {
    m_textureInfo->setText(textureInfoPanelText(m_currentTextureType, m_currentTextureName, *info, mipIndex));
    m_textureInfo->setVisible(true);
  }
  populateTextureMipSelector(static_cast<int>(info->mipCount));
  return true;
}

void MainWindow::clearCurrentTextureState() {
  m_currentTextureBytes.clear();
  m_currentTextureType.clear();
  m_currentTextureName.clear();
  m_currentTextureSelectionVersion = 0;
  m_currentTextureMipCount = 0;
  m_currentTextureMipShown = 0;
  if (m_textureMipSelector) {
    const bool old = m_textureMipSelector->blockSignals(true);
    m_textureMipSelector->clear();
    m_textureMipSelector->setEnabled(false);
    m_textureMipSelector->setVisible(false);
    m_textureMipSelector->blockSignals(old);
  }
}

void MainWindow::stepTextureZoom(int direction) {
  if (m_textureOriginal.isNull()) return;
  m_textureFitToView = false;
  const double factor = (direction > 0) ? 1.2 : (1.0 / 1.2);
  m_textureZoom *= factor;
  if (m_textureZoom < 0.05) m_textureZoom = 0.05;
  if (m_textureZoom > 32.0) m_textureZoom = 32.0;
  applyTextureZoom();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
  if ((obj == m_imageView || (m_imageScroll && obj == m_imageScroll->viewport())) && event) {
    if (event->type() == QEvent::Wheel) {
      auto* we = static_cast<QWheelEvent*>(event);
      if (we->modifiers() & Qt::ControlModifier) {
        stepTextureZoom(we->angleDelta().y() >= 0 ? +1 : -1);
        we->accept();
        return true;
      }
    }
  }
  return QMainWindow::eventFilter(obj, event);
}

void MainWindow::setRsfDirty(bool dirty) {
  m_rsfDirty = dirty;
  if (m_rsfApplyAction) m_rsfApplyAction->setEnabled(m_rsfEditMode && dirty && m_rsfCurrentDoc.has_value());
  if (m_statusDirtyLabel && m_viewTabs && m_viewTabs->currentWidget() == m_rsfTab) m_statusDirtyLabel->setText(dirty ? "Dirty" : "");
}

void MainWindow::refreshRsfMaterialsTable() {
  if (!m_rsfMaterialsTable) return;
  const int prevRow = std::max(0, m_rsfMaterialsTable->currentRow());
  m_rsfUpdatingUi = true;
  m_rsfMaterialsTable->setRowCount(0);
  if (m_rsfCurrentDoc) {
    for (int i = 0; i < int(m_rsfCurrentDoc->materials.size()); ++i) {
      const auto& m = m_rsfCurrentDoc->materials[std::size_t(i)];
      m_rsfMaterialsTable->insertRow(i);
      auto* num = new QTableWidgetItem(QString::number(i));
      num->setData(Qt::UserRole, i);
      num->setFlags(num->flags() & ~Qt::ItemIsEditable);
      m_rsfMaterialsTable->setItem(i, 0, num);
      m_rsfMaterialsTable->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(m.name)));
      m_rsfMaterialsTable->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(m.sub_name)));
    }
    if (!m_rsfCurrentDoc->materials.empty()) m_rsfMaterialsTable->selectRow(std::min(prevRow, int(m_rsfCurrentDoc->materials.size()) - 1));
  }
  m_rsfUpdatingUi = false;
}

void MainWindow::refreshRsfParamsTable(int materialIndex) {
  if (!m_rsfParamsTable) return;
  m_rsfUpdatingUi = true;
  m_rsfParamsTable->setRowCount(0);
  if (m_rsfCurrentDoc && materialIndex >= 0 && materialIndex < int(m_rsfCurrentDoc->materials.size())) {
    const auto& m = m_rsfCurrentDoc->materials[std::size_t(materialIndex)];
    int prow = 0;
    for (const auto& p : m.params) {
      QStringList vals;
      QString varType = QString::fromStdString(p.var_type);
      QString paramName = p.names.empty() ? QString() : QString::fromStdString(p.names.front());
      std::visit([&](auto&& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, gf::models::rsf::param_tex>) vals << QString::number(val.texture_index);
        else if constexpr (std::is_same_v<T, gf::models::rsf::param_vec>) for (float f : val.values) vals << QString::number(f);
        else if constexpr (std::is_same_v<T, gf::models::rsf::param_bool>) vals << (val.value ? "1" : "0");
        else if constexpr (std::is_same_v<T, gf::models::rsf::param_i32>) vals << QString::number(val.value);
        else if constexpr (std::is_same_v<T, gf::models::rsf::param_f32>) vals << QString::number(val.value);
      }, p.value);
      m_rsfParamsTable->insertRow(prow);
      auto* idx = new QTableWidgetItem(QString::number(prow)); idx->setFlags(idx->flags() & ~Qt::ItemIsEditable);
      auto* type = new QTableWidgetItem(varType); type->setFlags(type->flags() & ~Qt::ItemIsEditable);
      m_rsfParamsTable->setItem(prow, 0, idx);
      m_rsfParamsTable->setItem(prow, 1, type);
      m_rsfParamsTable->setItem(prow, 2, new QTableWidgetItem(paramName));
      for (int i = 0; i < 4; ++i) m_rsfParamsTable->setItem(prow, 3 + i, new QTableWidgetItem(i < vals.size() ? vals[i] : QString()));
      ++prow;
    }
  }
  m_rsfUpdatingUi = false;
}

void MainWindow::pullRsfUiIntoDocument() {
  if (!m_rsfCurrentDoc || !m_rsfMaterialsTable || !m_rsfParamsTable) return;
  for (int i = 0; i < m_rsfMaterialsTable->rowCount() && i < int(m_rsfCurrentDoc->materials.size()); ++i) {
    auto& m = m_rsfCurrentDoc->materials[std::size_t(i)];
    if (auto* it = m_rsfMaterialsTable->item(i, 1)) m.name = it->text().toStdString();
    if (auto* it = m_rsfMaterialsTable->item(i, 2)) m.sub_name = it->text().toStdString();
  }
  const int row = m_rsfMaterialsTable->currentRow();
  if (row < 0 || row >= int(m_rsfCurrentDoc->materials.size())) return;
  auto& mat = m_rsfCurrentDoc->materials[std::size_t(row)];
  for (int prow = 0; prow < m_rsfParamsTable->rowCount() && prow < int(mat.params.size()); ++prow) {
    auto& p = mat.params[std::size_t(prow)];
    if (auto* it = m_rsfParamsTable->item(prow, 2)) {
      if (p.names.empty()) p.names.push_back(it->text().toStdString()); else p.names[0] = it->text().toStdString();
    }
    auto cell = [&](int col) -> QString { auto* it = m_rsfParamsTable->item(prow, col); return it ? it->text().trimmed() : QString(); };
    if (std::holds_alternative<gf::models::rsf::param_tex>(p.value)) { bool ok=false; auto v=cell(3).toUInt(&ok); if (ok) std::get<gf::models::rsf::param_tex>(p.value).texture_index=v; }
    else if (std::holds_alternative<gf::models::rsf::param_vec>(p.value)) { auto& vals=std::get<gf::models::rsf::param_vec>(p.value).values; for (int i=0;i<int(vals.size()) && i<4;++i){ bool ok=false; float f=cell(3+i).toFloat(&ok); if(ok) vals[std::size_t(i)]=f; } }
    else if (std::holds_alternative<gf::models::rsf::param_bool>(p.value)) { const auto v=cell(3).toLower(); std::get<gf::models::rsf::param_bool>(p.value).value=(v=="1"||v=="true"||v=="yes"); }
    else if (std::holds_alternative<gf::models::rsf::param_i32>(p.value)) { bool ok=false; int v=cell(3).toInt(&ok); if(ok) std::get<gf::models::rsf::param_i32>(p.value).value=v; }
    else if (std::holds_alternative<gf::models::rsf::param_f32>(p.value)) { bool ok=false; float v=cell(3).toFloat(&ok); if(ok) std::get<gf::models::rsf::param_f32>(p.value).value=v; }
  }
}



std::optional<QByteArray> MainWindow::tryResolveRsfTextureBytes(const std::string& textureName, const std::string& textureFilename) const {
  if (!m_tree) return std::nullopt;
  const QStringList needles = rsfBuildTextureNeedles(textureName, textureFilename);
  if (needles.isEmpty()) return std::nullopt;

  QList<QTreeWidgetItem*> queue;
  for (int i = 0; i < m_tree->topLevelItemCount(); ++i) queue.push_back(m_tree->topLevelItem(i));
  while (!queue.isEmpty()) {
    auto* item = queue.takeFirst();
    if (!item) continue;
    for (int i = 0; i < item->childCount(); ++i) queue.push_back(item->child(i));

    const QString displayName = item->text(0);
    const QString type = item->text(1).toUpper();
    const QString path = item->data(0, Qt::UserRole).toString();
    const bool looksTexture = (type == "DDS" || type == "XPR" || type == "XPR2" || type == "P3R") ||
                              displayName.endsWith(".dds", Qt::CaseInsensitive) ||
                              displayName.endsWith(".xpr", Qt::CaseInsensitive) ||
                              displayName.endsWith(".xpr2", Qt::CaseInsensitive) ||
                              path.endsWith(".dds", Qt::CaseInsensitive) ||
                              path.endsWith(".xpr", Qt::CaseInsensitive) ||
                              path.endsWith(".xpr2", Qt::CaseInsensitive);
    if (!looksTexture) continue;
    if (!rsfCandidateMatchesTexture(needles, displayName, path)) continue;

    const QVariant previewVar = item->data(0, Qt::UserRole + 31);
    if (previewVar.canConvert<QByteArray>()) {
      const QByteArray preview = previewVar.toByteArray();
      if (!preview.isEmpty()) return preview;
    }
    const QVariant pendingVar = item->data(0, Qt::UserRole + 30);
    if (pendingVar.canConvert<QByteArray>()) {
      const QByteArray pending = pendingVar.toByteArray();
      if (!pending.isEmpty()) return pending;
    }

    const bool isEmbedded = item->data(0, Qt::UserRole + 3).toBool();
    const bool isNested = itemIsNestedSubEntry(item);

    // Only use the live outer editor when this item is a direct outer-AST entry.
    // Nested sub-entries carry an entryIndex scoped to their inner sub-AST; feeding
    // that index to the outer editor returns the wrong payload.
    if (isEmbedded && !isNested && m_liveAstEditor && m_liveAstPath == path) {
      const qulonglong entryIndexQ = item->data(0, Qt::UserRole + 6).toULongLong();
      const std::uint32_t entryIndex = static_cast<std::uint32_t>(entryIndexQ);
      std::string liveErr;
      if (auto fullOpt = m_liveAstEditor->getEntryInflatedBytes(entryIndex, &liveErr); fullOpt.has_value() && !fullOpt->empty()) {
        return QByteArray(reinterpret_cast<const char*>(fullOpt->data()), static_cast<int>(fullOpt->size()));
      }
      if (auto storedOpt = m_liveAstEditor->getEntryStoredBytes(entryIndex); storedOpt.has_value() && !storedOpt->empty()) {
        return QByteArray(reinterpret_cast<const char*>(storedOpt->data()), static_cast<int>(storedOpt->size()));
      }
    }

    // For nested sub-entries (or when the live editor wasn't useful): read directly
    // from the file using the absolute payload offset stored in UserRole+1.
    if (isEmbedded && !path.isEmpty()) {
      const quint64 absOff  = item->data(0, Qt::UserRole + 1).toULongLong();
      const quint64 compSz  = item->data(0, Qt::UserRole + 2).toULongLong();
      if (compSz > 0) {
        auto raw = read_file_range(path, static_cast<std::uint64_t>(absOff),
                                   static_cast<std::uint64_t>(compSz));
        if (!raw.empty()) {
          QByteArray bytes(reinterpret_cast<const char*>(raw.data()), static_cast<int>(raw.size()));
          // If the stored bytes are zlib-compressed, inflate them before returning.
          if (looksZlibPreviewBytes(bytes)) {
            std::vector<std::uint8_t> zIn(static_cast<std::size_t>(bytes.size()));
            std::memcpy(zIn.data(), bytes.constData(), static_cast<std::size_t>(bytes.size()));
            const auto inflated = gf::core::AstArchive::inflateZlibPreview(zIn, 8u * 1024u * 1024u);
            if (!inflated.empty()) {
              return QByteArray(reinterpret_cast<const char*>(inflated.data()),
                                static_cast<int>(inflated.size()));
            }
          }
          return bytes;
        }
      }
    }

    if (!path.isEmpty() && !isEmbedded) {
      QFile f(path);
      if (f.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = f.readAll();
        if (!bytes.isEmpty()) return bytes;
      }
    }
  }
  return std::nullopt;
}

void MainWindow::refreshRsfPreview() {
  if (!m_rsfPreviewWidget) return;
  if (!m_rsfCurrentDoc || m_rsfOriginalBytes.isEmpty()) {
    m_rsfPreviewDoc.reset();
    m_rsfPreviewWidget->clear();
    return;
  }

  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(m_rsfOriginalBytes.size()));
  std::memcpy(bytes.data(), m_rsfOriginalBytes.constData(), static_cast<std::size_t>(m_rsfOriginalBytes.size()));
  auto doc = gf::models::rsf::build_preview_document(*m_rsfCurrentDoc);
  doc.geometry = gf::models::rsf::decode_geom_candidates(*m_rsfCurrentDoc, std::span<const std::uint8_t>(bytes.data(), bytes.size()));
  m_rsfPreviewDoc = doc;
  m_rsfPreviewWidget->setDocument(doc);

  QStringList textureStatus;
  for (const auto& obj : doc.objects) {
    if (!obj.material.texture_index) continue;
    const int texIndex = static_cast<int>(*obj.material.texture_index);
    const QString requestedName = QString::fromStdString(obj.material.texture_name).trimmed();
    const QString requestedFile = QString::fromStdString(obj.material.texture_filename).trimmed();
    const auto resolved = tryResolveRsfTextureBytes(obj.material.texture_name, obj.material.texture_filename);
    if (!resolved.has_value()) {
      QStringList diag;
      diag << QString("Texture #%1 unresolved").arg(texIndex);
      if (!requestedName.isEmpty()) diag << QString("name=%1").arg(requestedName);
      if (!requestedFile.isEmpty()) diag << QString("file=%1").arg(requestedFile);
      diag << "lookup=session-tree/current-AST/neighbors/filesystem";
      textureStatus << diag.join(" | ");
      continue;
    }
    QByteArray texBytes = *resolved;
    std::optional<gf::textures::ImageRGBA> img;
    if (texBytes.startsWith("DDS ")) {
      std::vector<std::uint8_t> raw(static_cast<std::size_t>(texBytes.size()));
      std::memcpy(raw.data(), texBytes.constData(), raw.size());
      img = gf::textures::decode_dds_mip_rgba(raw, 0);
    } else if (texBytes.size() >= 4 && std::memcmp(texBytes.constData(), "XPR2", 4) == 0) {
      std::vector<std::uint8_t> raw(static_cast<std::size_t>(texBytes.size()));
      std::memcpy(raw.data(), texBytes.constData(), raw.size());
      auto dds = gf::textures::rebuild_xpr2_dds_first(raw, nullptr, true);
      if (dds) img = gf::textures::decode_dds_mip_rgba(*dds, 0);
    }
    if (img) {
      QImage qimg(img->rgba.data(), static_cast<int>(img->width), static_cast<int>(img->height), QImage::Format_RGBA8888);
      m_rsfPreviewWidget->setTexturePixmap(texIndex, QPixmap::fromImage(qimg.copy()));
    } else {
      QStringList diag;
      diag << QString("Texture #%1 decode failed").arg(texIndex);
      if (!requestedName.isEmpty()) diag << QString("name=%1").arg(requestedName);
      if (!requestedFile.isEmpty()) diag << QString("file=%1").arg(requestedFile);
      diag << QString("header=%1").arg(QString::fromLatin1(texBytes.left(4).toHex()));
      textureStatus << diag.join(" | ");
    }
  }
  if (!textureStatus.isEmpty()) m_rsfPreviewWidget->setTextureStatus(textureStatus.join("\n"));
}

void MainWindow::onRsfPreviewSelectionChanged(int materialIndex) {
  if (!m_rsfMaterialsTable || m_rsfUpdatingUi) return;
  if (materialIndex >= 0 && materialIndex < m_rsfMaterialsTable->rowCount()) {
    m_rsfMaterialsTable->selectRow(materialIndex);
    refreshRsfParamsTable(materialIndex);
  }
}

void MainWindow::onRsfPreviewTransformEdited(int materialIndex, const gf::models::rsf::preview_transform& transform, bool interactive) {
  if (!m_rsfCurrentDoc || materialIndex < 0 || materialIndex >= static_cast<int>(m_rsfCurrentDoc->materials.size())) return;
  auto& mat = m_rsfCurrentDoc->materials[static_cast<std::size_t>(materialIndex)];
  auto setNamedScalar = [&](std::initializer_list<const char*> needles, float value) {
    for (auto& p : mat.params) {
      const std::string name = p.names.empty() ? p.var_type : p.names.front();
      std::string lower = name; std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
      bool hit = false; for (auto* n : needles) if (lower.find(n) != std::string::npos) { hit = true; break; }
      if (!hit) continue;
      if (std::holds_alternative<gf::models::rsf::param_f32>(p.value)) std::get<gf::models::rsf::param_f32>(p.value).value = value;
      else if (std::holds_alternative<gf::models::rsf::param_i32>(p.value)) std::get<gf::models::rsf::param_i32>(p.value).value = static_cast<int>(value);
    }
  };
  setNamedScalar({"offsetx","translatex","positionx","posx","centerx","pivotx","locx"}, transform.x);
  setNamedScalar({"offsety","translatey","positiony","posy","centery","pivoty","locy"}, transform.y);
  setNamedScalar({"rot","angle","yaw","heading"}, transform.rotation_deg);
  setNamedScalar({"scalex","sizex","width","stretchx"}, transform.scale_x);
  setNamedScalar({"scaley","sizey","height","stretchy"}, transform.scale_y);
  refreshRsfParamsTable(materialIndex);
  setRsfDirty(true);
}

bool MainWindow::applyRsfChanges() {
  if (!m_rsfCurrentDoc || m_rsfOriginalBytes.isEmpty()) return false;
  gf::core::logBreadcrumb(gf::core::LogCategory::FileIO,
                          "Apply RSF changes: " + m_rsfSourcePath.toStdString());
  pullRsfUiIntoDocument();
  std::vector<std::uint8_t> original(static_cast<std::size_t>(m_rsfOriginalBytes.size()));
  std::memcpy(original.data(), m_rsfOriginalBytes.constData(), static_cast<std::size_t>(m_rsfOriginalBytes.size()));
  const auto rebuilt = gf::models::rsf::rebuild(*m_rsfCurrentDoc, std::span<const std::uint8_t>(original.data(), original.size()));
  if (rebuilt.empty() || !gf::models::rsf::parse(std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size())).has_value()) {
    gf::core::logError(gf::core::LogCategory::FileIO,
                       "Apply RSF failed: rebuild produced invalid RSF", m_rsfSourcePath.toStdString());
    showErrorDialog("Apply RSF", "Failed to rebuild a valid RSF.");
    return false;
  }
  if (m_rsfSourceEmbedded) {
    std::string err;
    auto ed = gf::core::AstContainerEditor::load(m_rsfSourcePath.toStdString(), &err);
    if (!ed.has_value()) { showErrorDialog("Apply RSF", "Failed to load container.", QString::fromStdString(err), true); return false; }
    if (!ed->replaceEntryBytes(m_rsfSourceEntryIndex, std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size()), gf::core::AstContainerEditor::ReplaceMode::PreserveZlibIfPresent, &err) || !ed->writeInPlace(&err, true)) {
      showErrorDialog("Apply RSF", "Failed to write updated RSF.", QString::fromStdString(err), true); return false;
    }
  } else {
    gf::core::SafeWriteOptions opt; opt.make_backup = true;
    const auto r = gf::core::safe_write_bytes(m_rsfSourcePath.toStdString(), std::span<const std::byte>(reinterpret_cast<const std::byte*>(rebuilt.data()), rebuilt.size()), opt);
    if (!r.ok) { showErrorDialog("Apply RSF", "Failed to write RSF file.", QString::fromStdString(r.message), true); return false; }
  }
  m_rsfOriginalBytes = QByteArray(reinterpret_cast<const char*>(rebuilt.data()), int(rebuilt.size()));
  setRsfDirty(false);
  statusBar()->showMessage("Applied RSF changes (backup created)", 3500);
  if (m_tree && m_tree->currentItem()) showViewerForItem(m_tree->currentItem());
  return true;
}

void MainWindow::setMode(Mode m) {
  m_mode = m;
  updateWindowTitle();
}

void MainWindow::openStandaloneAst(const QString& astPath) {
  gf::core::logInfo(gf::core::LogCategory::AstParsing,
                    "Opening standalone AST", astPath.toStdString());
  setMode(Mode::Standalone);
  m_cacheId = cacheIdFromSeed(astPath);
  m_nameCache.loadForGame(m_cacheId);
  m_doc.path = astPath;
  m_liveAstEditor.reset();
  m_liveAstPath.clear();
  m_lastReplaceUndo = LastReplaceUndo{};
  if (m_actUndoLastReplace) m_actUndoLastReplace->setEnabled(false);
  setDirty(false);
  ++m_treeToken;
  m_tree->clear();

  // Standalone view: show single AST root (no parsing yet).
  auto* root = new QTreeWidgetItem(m_tree, QStringList() << QFileInfo(astPath).fileName() << "");
  root->setData(0, Qt::UserRole, astPath);
  root->setToolTip(0, buildTreeItemTooltip(QFileInfo(astPath).fileName(), "AST", "Root Container", astPath, false));
  root->setToolTip(1, QDir::toNativeSeparators(astPath));
  root->setToolTip(2, QDir::toNativeSeparators(astPath));
  root->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
  root->addChild(new QTreeWidgetItem(QStringList() << "Expand to parse..." << ""));

  updateStatusSelection(root);
  showViewerForItem(root);
  updateDocumentActions();
}

void MainWindow::openGame(const QString& displayName,
                          const QString& rootPath,
                          const QString& baseContentDir,
                          const QString& updateContentDir) {
  gf::core::logInfo(gf::core::LogCategory::General,
                    "Opening game library",
                    (displayName + " | root=" + rootPath).toStdString());
  setMode(Mode::Game);
  m_lastGameDisplayName = displayName;
  m_lastGameRootPath = rootPath;
  m_lastGameBaseContentDir = baseContentDir;
  m_lastGameUpdateContentDir = updateContentDir;

  m_cacheId = cacheIdFromSeed(rootPath);
  m_nameCache.loadForGame(m_cacheId);
  m_doc.path.clear();
  m_liveAstEditor.reset();
  m_liveAstPath.clear();
  m_lastReplaceUndo = LastReplaceUndo{};
  if (m_actUndoLastReplace) m_actUndoLastReplace->setEnabled(false);
  setDirty(false);
  updateStatusSelection(nullptr);
  startIndexing(displayName, rootPath, baseContentDir, updateContentDir);
  updateDocumentActions();
}


void MainWindow::onShowCoreHelp() {
  QString text;
  text += "ASTra Core v1.0.0\n\n";
  text += "Core functionality focuses on stable archive and asset workflows instead of experimental viewers.\n\n";
  text += "• Game library and registration\n";
  text += "• AST browsing, searching, extraction, and rebuild/save workflows\n";
  text += "• Generic file import/replace\n";
  text += "• Texture preview, export, and import\n";
  text += "• Text/XML/config editing\n";
  text += "• Hex/raw inspection\n";
  text += "• Minimal RSF structured table editing\n\n";
  text += "Intentionally not included in ASTra Core: old RSF scene/model viewers, APT workflows, and DAT workflows.";
  QMessageBox::information(this, "About ASTra Core", text);
}

void MainWindow::onExportLogs() {
  const QString logFile = QString::fromStdString(gf::core::Log::logFilePath());
  if (logFile.isEmpty()) {
    showInfoDialog("Export Logs",
                   "No log file path is available (logger may not be initialised).");
    return;
  }

  const QFileInfo logInfo(logFile);
  const QString dest = QFileDialog::getExistingDirectory(
      this, "Export Logs \u2014 Choose Destination Folder",
      QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
  if (dest.isEmpty()) return;

  // Flush so the exported copy captures the most recent entries.
  if (auto lg = gf::core::Log::get()) lg->flush();

  // Collect astra.log + all rotated files (astra.log.1 … astra.log.5).
  QDir srcDir = logInfo.absoluteDir();
  const QString base = logInfo.fileName();
  const QStringList candidates = srcDir.entryList(
      QStringList{base, base + ".*"}, QDir::Files, QDir::Name);

  if (candidates.isEmpty()) {
    showInfoDialog("Export Logs",
                   QString("No log files found in:\n%1")
                       .arg(QDir::toNativeSeparators(srcDir.absolutePath())));
    return;
  }

  int copied = 0;
  QStringList failed;
  for (const QString& name : candidates) {
    const QString src = srcDir.absoluteFilePath(name);
    const QString dst = QDir(dest).absoluteFilePath(name);
    if (QFile::exists(dst)) QFile::remove(dst);
    if (QFile::copy(src, dst)) ++copied;
    else failed << name;
  }

  if (failed.isEmpty()) {
    gf::core::logInfo(gf::core::LogCategory::General,
                      "Log export succeeded",
                      std::to_string(copied) + " file(s) -> " + dest.toStdString());
    toastOk(QString("Exported %1 log file(s) to %2").arg(copied).arg(dest));
    showInfoDialog("Export Logs",
                   QString("Exported %1 log file(s) to:\n%2\n\nAttach these files to GitHub issues or Discord support requests.")
                       .arg(copied)
                       .arg(QDir::toNativeSeparators(dest)));
  } else {
    showErrorDialog("Export Logs",
                    QString("Exported %1 file(s). Failed to copy: %2")
                        .arg(copied).arg(failed.join(", ")));
  }
}

void MainWindow::onCheckForUpdates() {
  // Disable the menu action while a check is already in progress to prevent
  // spawning multiple simultaneous requests.
  if (m_actCheckForUpdates)
    m_actCheckForUpdates->setEnabled(false);

  auto* checker = new gf::gui::update::UpdateChecker(
      QStringLiteral(ASTRA_GITHUB_OWNER),
      QStringLiteral(ASTRA_GITHUB_REPO),
      this);

  connect(checker, &gf::gui::update::UpdateChecker::updateAvailable,
          this, [this, checker](const gf::gui::update::ReleaseInfo& info) {
      checker->deleteLater();
      if (m_actCheckForUpdates) m_actCheckForUpdates->setEnabled(true);

      auto* dlg = new gf::gui::update::UpdateDialog(info, this);
      dlg->setAttribute(Qt::WA_DeleteOnClose);

      connect(dlg, &gf::gui::update::UpdateDialog::updateRequested,
              this, [this](const gf::gui::update::ReleaseInfo& releaseInfo) {
          auto* launcher = new gf::gui::update::UpdaterLauncher(this, this);

          connect(launcher, &gf::gui::update::UpdaterLauncher::updateReadyToInstall,
                  this, [this]() {
              gf::core::Log::get()->info("[Updater] Update launched – closing ASTra");
              QApplication::quit();
          });

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
      if (m_actCheckForUpdates) m_actCheckForUpdates->setEnabled(true);

      auto* dlg = new gf::gui::update::UpToDateDialog(this);
      dlg->setAttribute(Qt::WA_DeleteOnClose);
      dlg->exec();
  });

  connect(checker, &gf::gui::update::UpdateChecker::checkFailed,
          this, [this, checker](const QString& errorMessage) {
      checker->deleteLater();
      if (m_actCheckForUpdates) m_actCheckForUpdates->setEnabled(true);

      QMessageBox::warning(this, tr("Update Check Failed"),
          tr("Could not check for updates:\n\n%1").arg(errorMessage));
  });

  checker->checkForUpdates();
}

void MainWindow::buildUi() {
  // ---- Menu bar (v0.6.1 foundation) ----
  auto* fileMenu = menuBar()->addMenu("File");
  m_actOpen = fileMenu->addAction("Open…");
  m_actSave = fileMenu->addAction("Save");
  m_actSaveAs = fileMenu->addAction("Save As…");
  m_actRevert = fileMenu->addAction("Revert");
  m_actRestoreBackup = fileMenu->addAction("Restore Latest Backup…");
  fileMenu->addSeparator();
  fileMenu->addAction("Close", this, &QWidget::close);

  connect(m_actOpen, &QAction::triggered, this, &MainWindow::onOpenFile);
  connect(m_actSave, &QAction::triggered, this, &MainWindow::onSave);
  connect(m_actSaveAs, &QAction::triggered, this, &MainWindow::onSaveAs);
  connect(m_actRevert, &QAction::triggered, this, &MainWindow::onRevert);
  connect(m_actRestoreBackup, &QAction::triggered, this, &MainWindow::onRestoreLatestBackup);

  // ---- Tools menu (v0.6.9.1) ----
  auto* toolsMenu = menuBar()->addMenu("Tools");
  m_actDevMode = toolsMenu->addAction("Developer Mode");
  m_actDevMode->setCheckable(true);
  m_actDevMode->setChecked(devModeEnabled());
  connect(m_actDevMode, &QAction::toggled, this, &MainWindow::setDevModeEnabled);

  m_actUndoLastReplace = toolsMenu->addAction("Undo Last Replace");
  m_actUndoLastReplace->setEnabled(false);
  connect(m_actUndoLastReplace, &QAction::triggered, this, &MainWindow::onUndoLastReplace);
  toolsMenu->addSeparator();
  toolsMenu->addAction("Refresh Archive View", this, &MainWindow::refreshCurrentArchiveView);

  toolsMenu->addSeparator();
  {
      auto* actCreateRsfAst = toolsMenu->addAction("Create RSF-Based AST…");
      connect(actCreateRsfAst, &QAction::triggered, this, [this]() {
          auto* dlg = new gf::gui::CreateRsfAstDialog(this);
          dlg->setAttribute(Qt::WA_DeleteOnClose);
          dlg->exec();
      });
  }

  auto* helpMenu = menuBar()->addMenu("Help");
  m_actCoreHelp = helpMenu->addAction("ASTra Core Help");
  connect(m_actCoreHelp, &QAction::triggered, this, &MainWindow::onShowCoreHelp);

  auto* actExportLogs = helpMenu->addAction("Export Logs\u2026");
  connect(actExportLogs, &QAction::triggered, this, &MainWindow::onExportLogs);

  helpMenu->addSeparator();
  m_actCheckForUpdates = helpMenu->addAction("Check for Updates\u2026");
  connect(m_actCheckForUpdates, &QAction::triggered,
          this, &MainWindow::onCheckForUpdates);

  // Editing is always enabled in ASTra editor mode.
  m_actEnableEditing = nullptr;

  // Enabled/disabled by updateDocumentActions().
  m_actSave->setEnabled(false);
  m_actRevert->setEnabled(false);

  // ---- Toolbar (GridironForge vibe) ----
  auto* tb = addToolBar("Main");
  tb->setMovable(false);
  tb->setFloatable(false);

  // Mirror menu actions in toolbar for fast access.
  tb->addAction(m_actOpen);
  tb->addAction(m_actSave);
  tb->addAction(m_actSaveAs);
  tb->addAction(m_actRevert);

  // ---- Status bar ----
  auto* sb = new QStatusBar(this);
  setStatusBar(sb);
  statusBar()->showMessage("Ready");

  // Persistent context (v0.6.20.9)
  m_statusDocLabel = new QLabel(this);
  m_statusDocLabel->setText("AST: (none)");
  m_statusDocLabel->setToolTip("No container loaded");
  statusBar()->addPermanentWidget(m_statusDocLabel);

  auto* sep1 = new QLabel(" | ", this);
  statusBar()->addPermanentWidget(sep1);

  m_statusEntryLabel = new QLabel(this);
  m_statusEntryLabel->setText("Entry: (none)");
  statusBar()->addPermanentWidget(m_statusEntryLabel);

  auto* sep2 = new QLabel(" | ", this);
  statusBar()->addPermanentWidget(sep2);

  m_statusMetaLabel = new QLabel(this);
  m_statusMetaLabel->setText("");
  m_statusMetaLabel->setVisible(false);
  statusBar()->addPermanentWidget(m_statusMetaLabel);

  auto* sep3 = new QLabel(" | ", this);
  statusBar()->addPermanentWidget(sep3);

  m_statusDirtyLabel = new QLabel(this);
  m_statusDirtyLabel->setText("");
  m_statusDirtyLabel->setVisible(false);
  statusBar()->addPermanentWidget(m_statusDirtyLabel);

  auto* sep4 = new QLabel(" | ", this);
  statusBar()->addPermanentWidget(sep4);

  m_editModeLabel = new QLabel(this);
  m_editModeLabel->setText("Editing: ON");
  m_editModeLabel->setToolTip("Read-only by default. Enable Editing to allow replace/save actions.");
  statusBar()->addPermanentWidget(m_editModeLabel);

  // Progress indicator (top-right)
  m_parseProgress = new QProgressBar(this);
  m_parseProgress->setTextVisible(false);
  m_parseProgress->setFixedWidth(140);
  m_parseProgress->setVisible(false);
  statusBar()->addPermanentWidget(m_parseProgress);

  updateStatusBar();

  // ---- Left dock: File Tree + search ----
  m_treeDock = new QDockWidget("File Tree", this);
  m_treeDock->setObjectName("dock_file_tree");
  m_treeDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
  m_treeDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

  auto* dockHost = new QWidget(this);
  auto* dockLayout = new QVBoxLayout(dockHost);
  dockLayout->setContentsMargins(8, 8, 8, 8);
  dockLayout->setSpacing(6);

  m_header = new QLabel("Standalone mode. Use File > Open (not implemented yet).", dockHost);
  m_header->setWordWrap(true);

  m_search = new QLineEdit(dockHost);
  m_search->setPlaceholderText("Search (AST name, folder, path)...");
  connect(m_search, &QLineEdit::textChanged, this, &MainWindow::onSearchChanged);

  m_tree = new QTreeWidget(dockHost);
  m_tree->setHeaderLabels(QStringList() << "Item" << "Type" << "Info");
  m_tree->setUniformRowHeights(true);
  m_tree->setAnimated(false);
  m_tree->setExpandsOnDoubleClick(false);
  m_tree->setIconSize(QSize(0, 0));
  m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
  m_tree->setMouseTracking(true);
  m_tree->setToolTipDuration(15000);
  if (m_tree->header()) {
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setMinimumSectionSize(48);
    m_tree->setColumnWidth(0, 520);
    m_tree->setColumnWidth(1, 84);
    m_tree->setColumnWidth(2, 160);
  }

  connect(m_tree, &QTreeWidget::itemExpanded, this, &MainWindow::onItemExpanded);
  connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &MainWindow::onItemDoubleClicked);
  connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &MainWindow::onTreeContextMenu);
  connect(m_tree, &QTreeWidget::currentItemChanged, this, &MainWindow::onCurrentItemChanged);

  dockLayout->addWidget(m_header);
  dockLayout->addWidget(m_search);
  dockLayout->addWidget(m_tree, 1);
  dockHost->setLayout(dockLayout);

  m_treeDock->setWidget(dockHost);
  m_treeDock->setMinimumWidth(620);
  addDockWidget(Qt::LeftDockWidgetArea, m_treeDock);
  resizeDocks({m_treeDock}, {680}, Qt::Horizontal);

  // ---- Central viewer host (no tabs; auto "tool" placeholder) ----
  m_viewerHost = new QWidget(this);
  auto* vlayout = new QVBoxLayout(m_viewerHost);
  vlayout->setContentsMargins(12, 12, 12, 12);
  vlayout->setSpacing(8);

  m_viewerLabel = new QPlainTextEdit(m_viewerHost);
  m_viewerLabel->setReadOnly(true);
  m_viewerLabel->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  m_viewerLabel->setPlainText("Select a file in the tree to view it.");
  m_viewerLabel->setMaximumHeight(160);

  m_viewTabs = new QTabWidget(m_viewerHost);

  m_hexView = new QPlainTextEdit(m_viewerHost);
  m_hexView->setReadOnly(true);
  m_hexView->setLineWrapMode(QPlainTextEdit::NoWrap);
  QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  m_hexView->setFont(mono);

  // Text editor tab (basic, in-pane editor)
  auto* textTab = new QWidget(m_viewerHost);
  auto* textLayout = new QVBoxLayout(textTab);
  textLayout->setContentsMargins(0, 0, 0, 0);
  textLayout->setSpacing(6);

  m_textToolbar = new QToolBar(textTab);
  m_textToolbar->setIconSize(QSize(16, 16));
  m_textReloadAction = m_textToolbar->addAction("Reload");
  m_textOpenExternalAction = m_textToolbar->addAction("Open...");
  m_textExportAction = m_textToolbar->addAction("Export...");
  m_textEditAction = m_textToolbar->addAction("Edit");
  m_textEditAction->setCheckable(true);
  m_textEditAction->setToolTip("Enable editing for embedded text entries");
  m_textApplyAction = m_textToolbar->addAction("Apply");
  m_textApplyAction->setEnabled(false);
  m_textToolbar->addSeparator();
  m_textFindAction = m_textToolbar->addAction("Find...");
  m_textFindAction->setShortcut(QKeySequence::Find);
  m_textFindNextAction = m_textToolbar->addAction("Next");
  m_textFindNextAction->setShortcut(QKeySequence::FindNext);
m_textReplaceAction = m_textToolbar->addAction("Replace...");
m_textReplaceAction->setShortcut(QKeySequence::Replace);
m_textGotoLineAction = m_textToolbar->addAction("Go To...");
m_textGotoLineAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
  m_textToolbar->addSeparator();
  m_textWrapAction = m_textToolbar->addAction("Wrap");
  m_textWrapAction->setCheckable(true);
  m_textWrapAction->setChecked(false);

  m_textView = new QPlainTextEdit(textTab);
  m_textView->setReadOnly(false);
  m_textView->setLineWrapMode(QPlainTextEdit::NoWrap);
  m_textView->setFont(mono);
// Ctrl+S should apply changes when in edit mode.
m_textSaveShortcutAction = new QAction(this);
m_textSaveShortcutAction->setShortcut(QKeySequence::Save);
textTab->addAction(m_textSaveShortcutAction);

  textLayout->addWidget(m_textToolbar, 0);
  textLayout->addWidget(m_textView, 1);
  textTab->setLayout(textLayout);

  m_textureTab = new QWidget(m_viewerHost);
  auto* texLayout = new QVBoxLayout(m_textureTab);
  texLayout->setContentsMargins(0,0,0,0);
  texLayout->setSpacing(6);

  auto* texInfoRow = new QHBoxLayout();
  texInfoRow->setContentsMargins(6, 4, 6, 0);
  texInfoRow->setSpacing(8);

  m_textureInfo = new QLabel(m_textureTab);
  m_textureInfo->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  m_textureInfo->setText("");
  m_textureInfo->setVisible(false);
  m_textureInfo->setWordWrap(true);

  auto* mipLabel = new QLabel("Mip:", m_textureTab);
  m_textureMipSelector = new QComboBox(m_textureTab);
  m_textureMipSelector->setMinimumWidth(110);
  m_textureMipSelector->setEnabled(false);
  m_textureMipSelector->setVisible(false);
  auto* inspectDdsButton = new QPushButton("Inspect DDS", m_textureTab);
  inspectDdsButton->setToolTip("Inspect the current DDS bytes and show validation diagnostics.");

  texInfoRow->addWidget(m_textureInfo, 1);
  texInfoRow->addWidget(inspectDdsButton, 0);
  texInfoRow->addWidget(mipLabel, 0);
  texInfoRow->addWidget(m_textureMipSelector, 0);

  m_imageView = new QLabel(m_textureTab);
  m_imageView->setAlignment(Qt::AlignCenter);
  m_imageView->setMinimumSize(200, 200);

  m_imageScroll = new QScrollArea(m_textureTab);
  m_imageScroll->setWidgetResizable(false);
  m_imageScroll->setAlignment(Qt::AlignCenter);
  m_imageScroll->setFrameShape(QFrame::NoFrame);
  m_imageScroll->setBackgroundRole(QPalette::Dark);
  m_imageScroll->setWidget(m_imageView);

  m_imageView->installEventFilter(this);
  m_imageScroll->viewport()->installEventFilter(this);

  auto* zoomInShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Plus), m_textureTab);
  connect(zoomInShortcut, &QShortcut::activated, this, [this]() { stepTextureZoom(+1); });
  auto* zoomInShortcutEq = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal), m_textureTab);
  connect(zoomInShortcutEq, &QShortcut::activated, this, [this]() { stepTextureZoom(+1); });
  auto* zoomOutShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus), m_textureTab);
  connect(zoomOutShortcut, &QShortcut::activated, this, [this]() { stepTextureZoom(-1); });
  auto* zoomResetShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_0), m_textureTab);
  connect(zoomResetShortcut, &QShortcut::activated, this, [this]() { resetTextureZoomToFit(); });

  if (m_textureMipSelector) {
    connect(m_textureMipSelector, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
      if (idx >= 0) renderCurrentTextureMip(idx);
    });
  }
  connect(inspectDdsButton, &QPushButton::clicked, this, [this]() {
    if (m_currentTextureBytes.isEmpty()) {
      showInfoDialog("Inspect DDS", "No DDS is currently loaded in the texture view.");
      return;
    }
    const auto bytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(m_currentTextureBytes.constData()), static_cast<std::size_t>(m_currentTextureBytes.size()));
    const auto result = gf::textures::inspect_dds(bytes);
    showInfoDialog("Inspect DDS",
                   QString("%1\n\n%2")
                       .arg(m_currentTextureName.isEmpty() ? QString("Current texture") : m_currentTextureName,
                            ddsValidationDetailsText(result)));
  });

  texLayout->addLayout(texInfoRow, 0);
  texLayout->addWidget(m_imageScroll, 1);

  m_aptTab = new QWidget(m_viewerHost);
  auto* aptLayout = new QVBoxLayout(m_aptTab);
  aptLayout->setContentsMargins(0,0,0,0);
  aptLayout->setSpacing(0);

  // Toolbar
  m_aptToolbar = new QToolBar(m_aptTab);
  m_aptToolbar->setIconSize(QSize(16, 16));

  // Frame navigation
  m_aptPrevFrameAction = m_aptToolbar->addAction(QChar(0x25C4)); // ◄
  m_aptPrevFrameAction->setToolTip("Previous root frame");
  m_aptPrevFrameAction->setEnabled(false);
  connect(m_aptPrevFrameAction, &QAction::triggered, this, [this]() {
    if (m_aptCurrentFrameIndex > 0) setAptFrameIndex(m_aptCurrentFrameIndex - 1);
  });

  m_aptFrameSpin = new QSpinBox();
  m_aptFrameSpin->setRange(0, 0);
  m_aptFrameSpin->setValue(0);
  m_aptFrameSpin->setMinimumWidth(52);
  m_aptFrameSpin->setEnabled(false);
  m_aptFrameSpin->setToolTip("Current root movie frame index (0-based)");
  m_aptToolbar->addWidget(m_aptFrameSpin);
  connect(m_aptFrameSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
    if (!m_aptUpdatingUi) setAptFrameIndex(v);
  });

  m_aptFrameCountLabel = new QLabel(" / 0 ");
  m_aptFrameCountLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
  m_aptToolbar->addWidget(m_aptFrameCountLabel);

  m_aptNextFrameAction = m_aptToolbar->addAction(QChar(0x25BA)); // ►
  m_aptNextFrameAction->setToolTip("Next root frame");
  m_aptNextFrameAction->setEnabled(false);
  connect(m_aptNextFrameAction, &QAction::triggered, this, [this]() {
    if (m_currentAptFile &&
        m_aptCurrentFrameIndex + 1 < static_cast<int>(m_currentAptFile->frames.size()))
      setAptFrameIndex(m_aptCurrentFrameIndex + 1);
  });

  m_aptToolbar->addSeparator();

  m_aptApplyAction = m_aptToolbar->addAction("Apply");
  m_aptApplyAction->setEnabled(false);
  m_aptApplyAction->setToolTip("Commit edits to the in-memory APT model");
  connect(m_aptApplyAction, &QAction::triggered, this, &MainWindow::onAptApply);

  m_aptSaveAction = m_aptToolbar->addAction("Save APT");
  m_aptSaveAction->setEnabled(false);
  m_aptSaveAction->setToolTip("Write patched APT bytes back to the source (standalone file or AST container entry)");
  connect(m_aptSaveAction, &QAction::triggered, this, &MainWindow::onAptSave);

  m_aptExportAction = m_aptToolbar->addAction("Export APT...");
  m_aptExportAction->setEnabled(false);
  m_aptExportAction->setToolTip("Save the current APT binary (with edits applied) to any file");
  connect(m_aptExportAction, &QAction::triggered, this, &MainWindow::onAptExport);

  m_aptToolbar->addSeparator();

  m_aptBringForwardAction = m_aptToolbar->addAction("Bring Forward");
  m_aptBringForwardAction->setToolTip("Increase placement order/depth in the current frame");
  connect(m_aptBringForwardAction, &QAction::triggered, this, [this]() {
    if (m_aptPreviewScene && m_aptPreviewScene->bringSelectionForward()) {
      refreshAptPlacementTreeLabels();
      m_aptDirty = true;
      setDirty(true);
      refreshAptPreview();
    }
  });

  m_aptSendBackwardAction = m_aptToolbar->addAction("Send Backward");
  m_aptSendBackwardAction->setToolTip("Decrease placement order/depth in the current frame");
  connect(m_aptSendBackwardAction, &QAction::triggered, this, [this]() {
    if (m_aptPreviewScene && m_aptPreviewScene->sendSelectionBackward()) {
      refreshAptPlacementTreeLabels();
      m_aptDirty = true;
      setDirty(true);
      refreshAptPreview();
    }
  });

  m_aptAddPlacementAction = m_aptToolbar->addAction("Add Placement");
  connect(m_aptAddPlacementAction, &QAction::triggered, this, [this]() {
    if (m_aptPreviewScene && m_aptPreviewScene->addPlacement()) {
      refreshAptPlacementTreeLabels();
      m_aptDirty = true;
      setDirty(true);
      refreshAptPreview();
    }
  });

  m_aptDuplicatePlacementAction = m_aptToolbar->addAction("Duplicate");
  connect(m_aptDuplicatePlacementAction, &QAction::triggered, this, [this]() {
    if (m_aptPreviewScene && m_aptPreviewScene->duplicateSelection()) {
      refreshAptPlacementTreeLabels();
      m_aptDirty = true;
      setDirty(true);
      refreshAptPreview();
    }
  });

  m_aptRemovePlacementAction = m_aptToolbar->addAction("Remove");
  connect(m_aptRemovePlacementAction, &QAction::triggered, this, [this]() {
    if (m_aptPreviewScene && m_aptPreviewScene->removeSelection()) {
      refreshAptPlacementTreeLabels();
      m_aptDirty = true;
      setDirty(true);
      refreshAptPreview();
    }
  });

  m_aptToolbar->addSeparator();

  m_aptDebugAction = m_aptToolbar->addAction("Debug Overlay");
  m_aptDebugAction->setCheckable(true);
  m_aptDebugAction->setChecked(false);
  m_aptDebugAction->setToolTip("Show placement origins, bounding boxes, and character IDs for all nested sprites");
  connect(m_aptDebugAction, &QAction::toggled, this, [this](bool) { refreshAptPreview(); });

  m_aptToolbar->addSeparator();

  m_aptZoomFitAction = m_aptToolbar->addAction("Fit");
  m_aptZoomFitAction->setToolTip("Fit preview content to view (F)");
  m_aptZoomFitAction->setShortcut(QKeySequence(Qt::Key_F));
  connect(m_aptZoomFitAction, &QAction::triggered, this, [this]() {
    if (auto* v = qobject_cast<AptPreviewView*>(m_aptPreviewView)) v->fitContent();
  });

  m_aptZoom100Action = m_aptToolbar->addAction("100%");
  m_aptZoom100Action->setToolTip("Reset to 1:1 zoom (Ctrl+0)");
  m_aptZoom100Action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
  connect(m_aptZoom100Action, &QAction::triggered, this, [this]() {
    if (auto* v = qobject_cast<AptPreviewView*>(m_aptPreviewView)) v->zoomTo100();
  });

  m_aptZoomInAction = m_aptToolbar->addAction("+");
  m_aptZoomInAction->setToolTip("Zoom in (Ctrl++)");
  m_aptZoomInAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal));
  connect(m_aptZoomInAction, &QAction::triggered, this, [this]() {
    if (auto* v = qobject_cast<AptPreviewView*>(m_aptPreviewView)) v->zoomBy(1.25);
  });

  m_aptZoomOutAction = m_aptToolbar->addAction("\u2212"); // minus sign
  m_aptZoomOutAction->setToolTip("Zoom out (Ctrl+-)");
  m_aptZoomOutAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
  connect(m_aptZoomOutAction, &QAction::triggered, this, [this]() {
    if (auto* v = qobject_cast<AptPreviewView*>(m_aptPreviewView)) v->zoomBy(1.0 / 1.25);
  });

  m_aptToolbar->addSeparator();

  {
    auto* lbl = new QLabel(" Render: ");
    lbl->setStyleSheet("color: #aaa; font-size: 11px;");
    m_aptToolbar->addWidget(lbl);
  }
  m_aptRenderModeCombo = new QComboBox;
  m_aptRenderModeCombo->addItem("Mixed",    static_cast<int>(AptRenderMode::Mixed));
  m_aptRenderModeCombo->addItem("Boxes",    static_cast<int>(AptRenderMode::Boxes));
  m_aptRenderModeCombo->addItem("Geometry", static_cast<int>(AptRenderMode::Geometry));
  m_aptRenderModeCombo->setToolTip(
      "Mixed: box outlines + DAT geometry\n"
      "Boxes: outlines only (no DAT triangles)\n"
      "Geometry: DAT triangles only (hides box for Image nodes)");
  m_aptToolbar->addWidget(m_aptRenderModeCombo);
  connect(m_aptRenderModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
          this, [this](int) {
    m_aptRenderMode = static_cast<AptRenderMode>(
        m_aptRenderModeCombo->currentData().toInt());
    refreshAptPreview();
  });

  aptLayout->addWidget(m_aptToolbar, 0);

  auto* aptSplit = new QSplitter(Qt::Horizontal, m_aptTab);

  m_aptTree = new QTreeWidget(aptSplit);
  m_aptTree->setHeaderLabels(QStringList() << "APT Structure" << "Value");
  m_aptTree->setUniformRowHeights(true);
  m_aptTree->setRootIsDecorated(true);
  m_aptTree->setAlternatingRowColors(false);
  if (m_aptTree->header()) {
    m_aptTree->header()->setStretchLastSection(false);
    m_aptTree->setColumnWidth(0, 260);
    m_aptTree->setColumnWidth(1, 140);
  }

  // Right panel: preview + stacked property editor
  m_aptRightPane = new QWidget(aptSplit);
  auto* aptRightLayout = new QVBoxLayout(m_aptRightPane);
  aptRightLayout->setContentsMargins(0, 0, 0, 0);
  aptRightLayout->setSpacing(6);

  m_aptSelectionManager = new gf::gui::apt_editor::AptSelectionManager(m_aptRightPane);
  m_aptPreviewScene = new gf::gui::apt_editor::AptPreviewScene(m_aptRightPane);
  m_aptPreviewScene->setSelectionManager(m_aptSelectionManager);
  m_aptPreviewScene->onPlacementSelected = [this](int placementIndex) { onAptSceneSelectionChanged(placementIndex); };
  m_aptPreviewScene->onPlacementEdited = [this](int placementIndex, bool interactive) { onAptScenePlacementEdited(placementIndex, interactive); };
  m_aptPreviewView = new AptPreviewView(m_aptRightPane);
  m_aptPreviewView->setScene(m_aptPreviewScene);
  m_aptPreviewView->setRenderHint(QPainter::Antialiasing, true);
  // DragMode is ScrollHandDrag (set in AptPreviewView constructor for pan support).
  m_aptPreviewView->setAlignment(Qt::AlignCenter);
  m_aptPreviewView->setMinimumHeight(260);
  m_aptPreviewView->setFrameShape(QFrame::StyledPanel);
  m_aptPreviewView->setBackgroundBrush(QBrush(QColor(24, 24, 28)));
  aptRightLayout->addWidget(m_aptPreviewView, 3);

  // Display-list status bar: always visible below the preview.
  m_aptDlStatusLabel = new QLabel(m_aptRightPane);
  m_aptDlStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_aptDlStatusLabel->setFont(mono);
  m_aptDlStatusLabel->setText("(no APT loaded)");
  aptRightLayout->addWidget(m_aptDlStatusLabel, 0);

  m_aptPropStack = new QStackedWidget(m_aptRightPane);
  aptRightLayout->addWidget(m_aptPropStack, 2);

  // Page 0 — plain text fallback
  m_aptDetails = new QPlainTextEdit(m_aptPropStack);
  m_aptDetails->setReadOnly(true);
  m_aptDetails->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  m_aptDetails->setFont(mono);
  m_aptDetails->setPlaceholderText("Select an APT item to view details.");
  m_aptPropStack->addWidget(m_aptDetails); // index 0

  // Helper: create a scroll-wrapped QFormLayout page and add it to the stack.
  auto makePage = [&]() -> std::pair<QWidget*, QFormLayout*> {
    auto* scroll = new QScrollArea(m_aptPropStack);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget();
    auto* form  = new QFormLayout(inner);
    form->setContentsMargins(8, 8, 8, 8);
    form->setSpacing(6);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    scroll->setWidget(inner);
    m_aptPropStack->addWidget(scroll);
    return {scroll, form};
  };

  auto makeReadLabel = [](QWidget* parent) -> QLabel* {
    auto* l = new QLabel(parent);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return l;
  };

  // Page 1 — Summary
  {
    auto [page, form] = makePage();
    m_aptSummaryPage        = page;
    m_aptSumWidthSpin       = new QSpinBox();
    m_aptSumHeightSpin      = new QSpinBox();
    m_aptSumWidthSpin->setRange(0, 65535);
    m_aptSumHeightSpin->setRange(0, 65535);
    m_aptSumFrameCountLabel  = makeReadLabel(page);
    m_aptSumCharCountLabel   = makeReadLabel(page);
    m_aptSumImportCountLabel = makeReadLabel(page);
    m_aptSumExportCountLabel = makeReadLabel(page);
    m_aptSumOffsetLabel      = makeReadLabel(page);
    form->addRow("Screen Width",  m_aptSumWidthSpin);
    form->addRow("Screen Height", m_aptSumHeightSpin);
    form->addRow("Frames",        m_aptSumFrameCountLabel);
    form->addRow("Characters",    m_aptSumCharCountLabel);
    form->addRow("Imports",       m_aptSumImportCountLabel);
    form->addRow("Exports",       m_aptSumExportCountLabel);
    form->addRow("Movie Offset",  m_aptSumOffsetLabel);
  }

  // Page 2 — Import editor
  {
    auto [page, form]     = makePage();
    m_aptImportPage       = page;
    m_aptImportMovieEdit  = new QLineEdit();
    m_aptImportNameEdit   = new QLineEdit();
    m_aptImportCharLabel  = makeReadLabel(page);
    m_aptImportOffsetLabel = makeReadLabel(page);
    form->addRow("Movie File",   m_aptImportMovieEdit);
    form->addRow("Symbol Name",  m_aptImportNameEdit);
    form->addRow("Character ID", m_aptImportCharLabel);
    form->addRow("Offset",       m_aptImportOffsetLabel);
  }

  // Page 3 — Export editor
  {
    auto [page, form]      = makePage();
    m_aptExportPage        = page;
    m_aptExportNameEdit    = new QLineEdit();
    m_aptExportCharLabel   = makeReadLabel(page);
    m_aptExportOffsetLabel = makeReadLabel(page);
    form->addRow("Symbol Name",  m_aptExportNameEdit);
    form->addRow("Character ID", m_aptExportCharLabel);
    form->addRow("Offset",       m_aptExportOffsetLabel);
  }

  // Page 4 — Character info (read-only)
  {
    auto [page, form]         = makePage();
    m_aptCharPage             = page;
    m_aptCharTypeLabel        = makeReadLabel(page);
    m_aptCharSigLabel         = makeReadLabel(page);
    m_aptCharOffsetLabel      = makeReadLabel(page);
    m_aptCharFrameCountLabel  = makeReadLabel(page);
    m_aptCharBoundsLabel      = makeReadLabel(page);
    m_aptCharImportLabel      = makeReadLabel(page);
    form->addRow("Type",         m_aptCharTypeLabel);
    form->addRow("Signature",    m_aptCharSigLabel);
    form->addRow("Offset",       m_aptCharOffsetLabel);
    form->addRow("Frames",       m_aptCharFrameCountLabel);
    form->addRow("Bounds",       m_aptCharBoundsLabel);
    form->addRow("Import",       m_aptCharImportLabel);
    // Scaffold breakdown — shown only for frameless Sprite/Movie containers.
    m_aptScaffoldDump = new QPlainTextEdit(page);
    m_aptScaffoldDump->setReadOnly(true);
    m_aptScaffoldDump->setFont(mono);
    m_aptScaffoldDump->setMinimumHeight(60);
    m_aptScaffoldDump->setMaximumHeight(200);
    m_aptScaffoldDump->setPlaceholderText("(scaffold breakdown for runtime-only containers)");
    m_aptScaffoldDump->setVisible(false);
    form->addRow("Scaffold", m_aptScaffoldDump);
  }

  // Page 5 — Frame info (read-only)
  {
    auto [page, form]          = makePage();
    m_aptFramePage             = page;
    m_aptFrameItemCountLabel   = makeReadLabel(page);
    m_aptFrameItemsOffsetLabel = makeReadLabel(page);
    form->addRow("Item Count",    m_aptFrameItemCountLabel);
    form->addRow("Items Offset",  m_aptFrameItemsOffsetLabel);

    // Cumulative display-list dump for this frame (populated by syncAptPropEditorFromItem).
    m_aptFrameDlDump = new QPlainTextEdit();
    m_aptFrameDlDump->setReadOnly(true);
    m_aptFrameDlDump->setFont(mono);
    m_aptFrameDlDump->setMinimumHeight(100);
    m_aptFrameDlDump->setMaximumHeight(240);
    m_aptFrameDlDump->setPlaceholderText("(cumulative display list will appear here)");
    form->addRow("Display List", m_aptFrameDlDump);
  }

  // Page 6 — Placement editor
  {
    auto [page, form]           = makePage();
    m_aptPlacementPage          = page;
    m_aptPlacementDepthSpin     = new QSpinBox();
    m_aptPlacementCharSpin      = new QSpinBox();
    m_aptPlacementNameEdit      = new QLineEdit();
    m_aptPlacementXSpin         = new QDoubleSpinBox();
    m_aptPlacementYSpin         = new QDoubleSpinBox();
    m_aptPlacementScaleXSpin    = new QDoubleSpinBox();
    m_aptPlacementScaleYSpin    = new QDoubleSpinBox();
    m_aptPlacementRotSkew0Spin  = new QDoubleSpinBox();
    m_aptPlacementRotSkew1Spin  = new QDoubleSpinBox();
    m_aptPlacementOffsetLabel   = makeReadLabel(page);

    m_aptPlacementDepthSpin->setRange(-32768, 32767);
    m_aptPlacementCharSpin->setRange(0, 65535);

    for (QDoubleSpinBox* spin : {m_aptPlacementXSpin, m_aptPlacementYSpin,
                                  m_aptPlacementScaleXSpin, m_aptPlacementScaleYSpin,
                                  m_aptPlacementRotSkew0Spin, m_aptPlacementRotSkew1Spin}) {
      spin->setRange(-100000.0, 100000.0);
      spin->setDecimals(6);
      spin->setSingleStep(0.01);
    }
    m_aptPlacementXSpin->setDecimals(3); m_aptPlacementXSpin->setSingleStep(1.0);
    m_aptPlacementYSpin->setDecimals(3); m_aptPlacementYSpin->setSingleStep(1.0);
    m_aptPlacementScaleXSpin->setDecimals(4); m_aptPlacementScaleXSpin->setSingleStep(0.1);
    m_aptPlacementScaleYSpin->setDecimals(4); m_aptPlacementScaleYSpin->setSingleStep(0.1);

    form->addRow("Depth",           m_aptPlacementDepthSpin);
    form->addRow("Character ID",    m_aptPlacementCharSpin);
    form->addRow("Instance Name",   m_aptPlacementNameEdit);
    form->addRow("X (tx)",          m_aptPlacementXSpin);
    form->addRow("Y (ty)",          m_aptPlacementYSpin);
    form->addRow("Scale X (a)",     m_aptPlacementScaleXSpin);
    form->addRow("Rot/Skew b",      m_aptPlacementRotSkew0Spin);
    form->addRow("Rot/Skew c",      m_aptPlacementRotSkew1Spin);
    form->addRow("Scale Y (d)",     m_aptPlacementScaleYSpin);
    form->addRow("Offset",          m_aptPlacementOffsetLabel);
  }

  auto markAptEditorDirty = [this]() {
    if (m_aptUpdatingUi) return;
    if (m_aptApplyAction) m_aptApplyAction->setEnabled(true);
  };
  connect(m_aptSumWidthSpin,  qOverload<int>(&QSpinBox::valueChanged), this, [markAptEditorDirty](int) { markAptEditorDirty(); });
  connect(m_aptSumHeightSpin, qOverload<int>(&QSpinBox::valueChanged), this, [markAptEditorDirty](int) { markAptEditorDirty(); });
  connect(m_aptImportMovieEdit, &QLineEdit::textEdited, this, [markAptEditorDirty](const QString&) { markAptEditorDirty(); });
  connect(m_aptImportNameEdit,  &QLineEdit::textEdited, this, [markAptEditorDirty](const QString&) { markAptEditorDirty(); });
  connect(m_aptExportNameEdit,  &QLineEdit::textEdited, this, [markAptEditorDirty](const QString&) { markAptEditorDirty(); });
  connect(m_aptPlacementNameEdit, &QLineEdit::textEdited, this, [markAptEditorDirty](const QString&) { markAptEditorDirty(); });
  connect(m_aptPlacementDepthSpin, qOverload<int>(&QSpinBox::valueChanged), this, [markAptEditorDirty](int) { markAptEditorDirty(); });
  connect(m_aptPlacementCharSpin, qOverload<int>(&QSpinBox::valueChanged), this, [markAptEditorDirty](int) { markAptEditorDirty(); });
  connect(m_aptPlacementXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [markAptEditorDirty](double) { markAptEditorDirty(); });
  connect(m_aptPlacementYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [markAptEditorDirty](double) { markAptEditorDirty(); });
  connect(m_aptPlacementScaleXSpin,   qOverload<double>(&QDoubleSpinBox::valueChanged), this, [markAptEditorDirty](double) { markAptEditorDirty(); });
  connect(m_aptPlacementScaleYSpin,   qOverload<double>(&QDoubleSpinBox::valueChanged), this, [markAptEditorDirty](double) { markAptEditorDirty(); });
  connect(m_aptPlacementRotSkew0Spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [markAptEditorDirty](double) { markAptEditorDirty(); });
  connect(m_aptPlacementRotSkew1Spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [markAptEditorDirty](double) { markAptEditorDirty(); });

  aptSplit->setStretchFactor(0, 3);
  aptSplit->setStretchFactor(1, 4);

  connect(m_aptTree, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
            if (m_suppressSelectionChange) return;
            syncAptPropEditorFromItem(current);
            refreshAptPreview();
          });

  aptLayout->addWidget(aptSplit, 1);

  m_rsfTab = new QWidget(m_viewerHost);
  auto* rsfLayout = new QVBoxLayout(m_rsfTab);
  rsfLayout->setContentsMargins(0,0,0,0);
  rsfLayout->setSpacing(6);

  m_rsfToolbar = new QToolBar(m_rsfTab);
  m_rsfToolbar->setIconSize(QSize(16, 16));
  m_rsfEditAction = m_rsfToolbar->addAction("Edit");
  m_rsfEditAction->setCheckable(true);
  m_rsfEditAction->setChecked(true);
  m_rsfEditAction->setEnabled(false);
  m_rsfEditAction->setVisible(false);
  m_rsfApplyAction = m_rsfToolbar->addAction("Apply");
  m_rsfApplyAction->setEnabled(false);
  rsfLayout->addWidget(m_rsfToolbar, 0);

  auto* rsfTop = new QWidget(m_rsfTab);
  auto* rsfTopLayout = new QGridLayout(rsfTop);
  rsfTopLayout->setContentsMargins(0,0,0,0);
  rsfTopLayout->setHorizontalSpacing(8);
  rsfTopLayout->setVerticalSpacing(4);

  auto makeValueLabel = [rsfTop]() -> QLabel* {
    auto* l = new QLabel(rsfTop);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return l;
  };

  rsfTopLayout->addWidget(new QLabel("Name", rsfTop), 0, 0);
  m_rsfNameValue = makeValueLabel();
  rsfTopLayout->addWidget(m_rsfNameValue, 0, 1, 1, 5);

  rsfTopLayout->addWidget(new QLabel("Model Count", rsfTop), 1, 0);
  m_rsfModelCountValue = makeValueLabel();
  rsfTopLayout->addWidget(m_rsfModelCountValue, 1, 1);
  rsfTopLayout->addWidget(new QLabel("Material Count", rsfTop), 1, 2);
  m_rsfMaterialCountValue = makeValueLabel();
  rsfTopLayout->addWidget(m_rsfMaterialCountValue, 1, 3);
  rsfTopLayout->addWidget(new QLabel("Texture Count", rsfTop), 1, 4);
  m_rsfTextureCountValue = makeValueLabel();
  rsfTopLayout->addWidget(m_rsfTextureCountValue, 1, 5);

  m_rsfMaterialsTable = new QTableWidget(m_rsfTab);
  m_rsfMaterialsTable->setColumnCount(3);
  m_rsfMaterialsTable->setHorizontalHeaderLabels(QStringList() << "#" << "Name" << "SubName");
  m_rsfMaterialsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_rsfMaterialsTable->setSelectionMode(QAbstractItemView::SingleSelection);
  m_rsfMaterialsTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed);
  m_rsfMaterialsTable->verticalHeader()->setVisible(false);
  m_rsfMaterialsTable->horizontalHeader()->setStretchLastSection(true);
  m_rsfMaterialsTable->setMinimumHeight(220);

  m_rsfParamsTable = new QTableWidget(m_rsfTab);
  m_rsfParamsTable->setColumnCount(7);
  m_rsfParamsTable->setHorizontalHeaderLabels(QStringList() << "#" << "VarType" << "Name" << "DATA0" << "DATA1" << "DATA2" << "DATA3");
  m_rsfParamsTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed);
  m_rsfParamsTable->verticalHeader()->setVisible(false);
  m_rsfParamsTable->horizontalHeader()->setStretchLastSection(true);
  m_rsfParamsTable->setMinimumHeight(180);


  auto* rsfLeft = new QWidget(m_rsfTab);
  auto* rsfLeftLayout = new QVBoxLayout(rsfLeft);
  rsfLeftLayout->setContentsMargins(0,0,0,0);
  rsfLeftLayout->setSpacing(6);
  rsfLeftLayout->addWidget(m_rsfMaterialsTable, 1);
  rsfLeftLayout->addWidget(m_rsfParamsTable, 1);

  m_rsfPreviewWidget = nullptr;

  rsfLayout->addWidget(rsfTop, 0);
  rsfLayout->addWidget(rsfLeft, 1);

  connect(m_rsfMaterialsTable, &QTableWidget::itemSelectionChanged, this, [this]() {
    if (!m_rsfMaterialsTable) return;
    const int row = m_rsfMaterialsTable->currentRow();
    refreshRsfParamsTable(row);
    if (m_rsfPreviewWidget) m_rsfPreviewWidget->setSelectionByMaterialIndex(row);
  });
  if (m_rsfPreviewWidget) {
    connect(m_rsfPreviewWidget, &gf::gui::rsf_editor::RsfPreviewWidget::materialSelectionChanged, this, &MainWindow::onRsfPreviewSelectionChanged);
    connect(m_rsfPreviewWidget, &gf::gui::rsf_editor::RsfPreviewWidget::transformEdited, this, &MainWindow::onRsfPreviewTransformEdited);
  }
  connect(m_rsfMaterialsTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem*) {
    if (m_rsfUpdatingUi || !m_rsfEditMode) return;
    pullRsfUiIntoDocument();
    setRsfDirty(true);
  });
  connect(m_rsfParamsTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem*) {
    if (m_rsfUpdatingUi || !m_rsfEditMode) return;
    pullRsfUiIntoDocument();
    setRsfDirty(true);
  });
  connect(m_rsfEditAction, &QAction::toggled, this, [this](bool on) {
    m_rsfEditMode = on;
    if (m_rsfMaterialsTable) {
      m_rsfMaterialsTable->setEditTriggers(on ? (QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed) : QAbstractItemView::NoEditTriggers);
    }
    if (m_rsfParamsTable) {
      m_rsfParamsTable->setEditTriggers(on ? (QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed) : QAbstractItemView::NoEditTriggers);
    }
    if (m_rsfApplyAction) m_rsfApplyAction->setEnabled(on && m_rsfDirty && m_rsfCurrentDoc.has_value());
  });
  connect(m_rsfApplyAction, &QAction::triggered, this, [this]() {
    applyRsfChanges();
  });

  // --- DAT tab ---
  m_datTab = new QWidget(m_viewerHost);
  auto* datOuterLayout = new QVBoxLayout(m_datTab);
  datOuterLayout->setContentsMargins(6, 6, 6, 6);
  datOuterLayout->setSpacing(4);

  // File-level summary (images count, file length, etc.)
  m_datSummaryLabel = new QLabel(m_datTab);
  m_datSummaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  datOuterLayout->addWidget(m_datSummaryLabel, 0);

  // Horizontal splitter: left = image list, right = geometry preview
  auto* datSplit = new QSplitter(Qt::Horizontal, m_datTab);

  // --- Left: image table ---
  m_datImagesTable = new QTableWidget(datSplit);
  m_datImagesTable->setColumnCount(7);
  m_datImagesTable->setHorizontalHeaderLabels(QStringList()
      << "#" << "CharId" << "RGBA" << "OffsetX" << "OffsetY" << "Triangles" << "FileOff");
  m_datImagesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_datImagesTable->setSelectionMode(QAbstractItemView::SingleSelection);
  m_datImagesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_datImagesTable->verticalHeader()->setVisible(false);
  m_datImagesTable->horizontalHeader()->setStretchLastSection(true);
  m_datImagesTable->setMinimumWidth(220);

  // --- Right: per-entry info + geometry preview ---
  auto* datRight = new QWidget(datSplit);
  auto* datRightLayout = new QVBoxLayout(datRight);
  datRightLayout->setContentsMargins(0, 0, 0, 0);
  datRightLayout->setSpacing(4);

  m_datEntryInfoLabel = new QLabel(datRight);
  m_datEntryInfoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_datEntryInfoLabel->setWordWrap(true);
  m_datEntryInfoLabel->setText("(select a row to preview)");
  datRightLayout->addWidget(m_datEntryInfoLabel, 0);

  // Transform toggle row
  auto* datTransformRow = new QWidget(datRight);
  auto* datTransformLayout = new QHBoxLayout(datTransformRow);
  datTransformLayout->setContentsMargins(0, 0, 0, 0);
  m_datApplyTransformCheck = new QCheckBox("Apply DAT transform (matrix + offset)", datTransformRow);
  m_datApplyTransformCheck->setToolTip(
      "When checked, vertex coordinates are transformed by the DAT image matrix and offset.\n"
      "The DAT transform positions the shape in the APT/Flash coordinate system.\n"
      "Warning: coordinate scale/units are reverse-engineered; results may vary.");
  datTransformLayout->addWidget(m_datApplyTransformCheck);
  datTransformLayout->addStretch(1);
  datRightLayout->addWidget(datTransformRow, 0);

  m_datPreviewScene = new QGraphicsScene(datRight);
  m_datPreviewView  = new QGraphicsView(m_datPreviewScene, datRight);
  m_datPreviewView->setRenderHint(QPainter::Antialiasing, true);
  m_datPreviewView->setDragMode(QGraphicsView::ScrollHandDrag);
  m_datPreviewView->setAlignment(Qt::AlignCenter);
  m_datPreviewView->setBackgroundBrush(QBrush(QColor(20, 20, 24)));
  m_datPreviewView->setFrameShape(QFrame::StyledPanel);
  m_datPreviewView->setMinimumHeight(200);
  datRightLayout->addWidget(m_datPreviewView, 1);

  m_datCorrelLabel = new QLabel(datRight);
  m_datCorrelLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_datCorrelLabel->setWordWrap(true);
  datRightLayout->addWidget(m_datCorrelLabel, 0);

  datSplit->addWidget(m_datImagesTable);
  datSplit->addWidget(datRight);
  datSplit->setStretchFactor(0, 2);
  datSplit->setStretchFactor(1, 3);
  datOuterLayout->addWidget(datSplit, 1);

  // Connect row selection → preview update
  connect(m_datImagesTable, &QTableWidget::itemSelectionChanged, this, [this]() {
    if (!m_datImagesTable) return;
    const int row = m_datImagesTable->currentRow();
    renderDatImageToScene(row);
  });
  // Connect transform toggle → re-render current row
  connect(m_datApplyTransformCheck, &QCheckBox::toggled, this, [this]() {
    if (!m_datImagesTable) return;
    renderDatImageToScene(m_datImagesTable->currentRow());
  });

  m_viewTabs->addTab(m_hexView, "Hex");
  m_viewTabs->addTab(textTab, "Text");
  m_textTabIndex = m_viewTabs->indexOf(textTab);

  m_viewTabs->addTab(m_textureTab, "Texture");
  m_viewTabs->addTab(m_rsfTab, "RSF");

  // APT/DAT are not part of ASTra Core. Their legacy widgets may still be
  // constructed for now, but they must not remain as free-floating children of
  // the central viewer host when they are not added to m_viewTabs.
  if (m_aptTab) {
    m_aptTab->hide();
    m_aptTab->setVisible(false);
  }
  if (m_datTab) {
    m_datTab->hide();
    m_datTab->setVisible(false);
  }

  // Text tab helpers
  connect(m_textWrapAction, &QAction::toggled, this, [this](bool on) {
    if (!m_textView) return;
    m_textView->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
  });
connect(m_textOpenExternalAction, &QAction::triggered, this, [this]() {
  const QString startDir = !m_textExternalPath.isEmpty()
                               ? QFileInfo(m_textExternalPath).absolutePath()
                               : (!m_doc.path.isEmpty() ? QFileInfo(m_doc.path).absolutePath() : QDir::homePath());
  const QString path = QFileDialog::getOpenFileName(
      this,
      "Open Text File",
      startDir,
      "Text/Config Files (*.txt *.xml *.cfg *.conf *.ini *.json *.yaml *.yml *.lua *.js *.css *.html *.htm);;All Files (*)");
  if (path.isEmpty()) return;
  (void)openExternalTextFile(path);
});

connect(m_textFindAction, &QAction::triggered, this, [this]() {
  if (!m_textView) return;
  bool ok = false;
  const QString q = QInputDialog::getText(this, "Find", "Find:", QLineEdit::Normal, QString(), &ok);
  if (!ok || q.isEmpty()) return;
  m_lastFindQuery = q;
  // start from current cursor
  QTextCursor c = m_textView->textCursor();
  c = m_textView->document()->find(q, c);
  if (c.isNull()) {
    // wrap
    c = m_textView->document()->find(q);
  }
  if (c.isNull()) {
    toastOk("No matches.");
    return;
  }
  m_textView->setTextCursor(c);
});

connect(m_textFindNextAction, &QAction::triggered, this, [this]() {
  if (!m_textView || m_lastFindQuery.isEmpty()) return;
  QTextCursor c = m_textView->textCursor();
  c = m_textView->document()->find(m_lastFindQuery, c);
  if (c.isNull()) c = m_textView->document()->find(m_lastFindQuery);
  if (c.isNull()) {
    toastOk("No more matches.");
    return;
  }
  m_textView->setTextCursor(c);
});

connect(m_textSaveShortcutAction, &QAction::triggered, this, [this]() {
  if (!m_textApplyAction) return;
  if (!m_textApplyAction->isEnabled()) return;
  m_textApplyAction->trigger();
});

connect(m_textReplaceAction, &QAction::triggered, this, [this]() {
  if (!m_textView) return;
  bool ok = false;
  const QString findQ = QInputDialog::getText(this, "Replace", "Find:", QLineEdit::Normal, m_lastFindQuery, &ok);
  if (!ok || findQ.isEmpty()) return;
  const QString repQ = QInputDialog::getText(this, "Replace", "Replace with:", QLineEdit::Normal, QString(), &ok);
  if (!ok) return;

  m_lastFindQuery = findQ;

  const auto choice = QMessageBox::question(this, "Replace", "Replace all occurrences?",
                                            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                                            QMessageBox::No);
  if (choice == QMessageBox::Cancel) return;

  if (choice == QMessageBox::Yes) {
    // Replace all in the document.
    const QString src = m_textView->toPlainText();
    QString out = src;
    out.replace(findQ, repQ);
    if (out == src) { toastOk("No matches."); return; }

    // Preserve cursor roughly at same position.
    const int cursorPos = m_textView->textCursor().position();
    m_textView->setPlainText(out);
    QTextCursor c = m_textView->textCursor();
    c.setPosition(std::min<qsizetype>(static_cast<qsizetype>(cursorPos), out.size()));
    m_textView->setTextCursor(c);
    return;
  }

  // Replace next occurrence from cursor.
  QTextCursor c = m_textView->textCursor();
  QTextCursor match = m_textView->document()->find(findQ, c);
  if (match.isNull()) match = m_textView->document()->find(findQ);
  if (match.isNull()) { toastOk("No matches."); return; }
  match.insertText(repQ);
  m_textView->setTextCursor(match);
});

connect(m_textGotoLineAction, &QAction::triggered, this, [this]() {
  if (!m_textView) return;
  const int maxLine = std::max(1, m_textView->document()->blockCount());
  bool ok = false;
  const int line = QInputDialog::getInt(this, "Go To Line", "Line number:", 1, 1, maxLine, 1, &ok);
  if (!ok) return;
  QTextBlock b = m_textView->document()->findBlockByNumber(line - 1);
  if (!b.isValid()) return;
  QTextCursor c(b);
  m_textView->setTextCursor(c);
  m_textView->centerCursor();
});
connect(m_textView->document(), &QTextDocument::modificationChanged, this, [this](bool modified) {
const bool canEditNow = m_textAptXmlMode || (editingEnabled() && (m_textExternalMode || m_textForceEdit));
if (m_textApplyAction) m_textApplyAction->setEnabled(modified && canEditNow);

// Show a simple dirty indicator + star the Text tab.
if (m_statusDirtyLabel) m_statusDirtyLabel->setText(modified ? "Dirty" : "");
if (m_viewTabs && m_textTabIndex >= 0) {
  m_viewTabs->setTabText(m_textTabIndex, modified ? "Text*" : "Text");
}
});

connect(m_textReloadAction, &QAction::triggered, this, [this]() {
  if (m_textExternalMode) {
    if (!m_textExternalPath.isEmpty()) {
      (void)openExternalTextFile(m_textExternalPath);
    }
    return;
  }
  if (m_tree) showViewerForItem(m_tree->currentItem());
});
  connect(m_textExportAction, &QAction::triggered, this, [this]() {
  if (!m_textView) return;

  QString suggested = "export.txt";
  if (m_textExternalMode) {
    if (!m_textExternalPath.isEmpty()) {
      suggested = QFileInfo(m_textExternalPath).fileName();
    } else if (!m_textExternalSuggestedName.isEmpty()) {
      suggested = m_textExternalSuggestedName;
    }
  } else if (m_tree) {
    auto* it = m_tree->currentItem();
    if (it && !it->text(0).isEmpty()) suggested = it->text(0);
  }

  const QString path = QFileDialog::getSaveFileName(
      this,
      "Export Text",
      suggested,
      "Text/Config Files (*.txt *.xml *.cfg *.conf *.ini *.json *.yaml *.yml *.lua *.js *.css *.html *.htm);;All Files (*)");
  if (path.isEmpty()) return;

  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    showErrorDialog("Export Text", "Failed to write file.", QString(), true);
    return;
  }
  const QByteArray out = m_textView->toPlainText().toUtf8();
  f.write(out);
  f.close();

  toastOk(QString("Exported %1").arg(QFileInfo(path).fileName()));
});

  
  connect(m_textEditAction, &QAction::toggled, this, [this](bool on) {
    m_textForceEdit = on;
    if (on) {
      if (m_textAptXmlMode) {
        if (m_textView) { m_textView->setReadOnly(false); m_textView->setFocus(); }
        if (m_tree) showViewerForItem(m_tree->currentItem());
        return;
      }
      if (!editingEnabled()) {
        // Can't edit if editing mode is off.
        m_textForceEdit = false;
        if (m_textEditAction) m_textEditAction->setChecked(false);
        showInfoDialog("Editing Disabled", "Turn on Editing to modify this text.");
        return;
      }
      // Only allow toggling for embedded text selections.
      QTreeWidgetItem* item = m_tree ? m_tree->currentItem() : nullptr;
      const bool isEmbedded = item ? item->data(0, Qt::UserRole + 3).toBool() : false;
      if (!isEmbedded) {
        m_textForceEdit = false;
        if (m_textEditAction) m_textEditAction->setChecked(false);
        showInfoDialog("Not Editable", "Select an embedded text entry to edit.");
        return;
      }
      if (m_textView) {
        m_textView->setReadOnly(false);
        m_textView->setFocus();
      }
    } else {
      if (m_textView) {
        if (m_textAptXmlMode) {
          m_textView->setReadOnly(true);
        } else if (!m_textExternalMode) {
          m_textView->setReadOnly(true);
        }
      }
    }
    // Refresh apply enable / dirty indicator
    if (m_tree) showViewerForItem(m_tree->currentItem());
  });

connect(m_textApplyAction, &QAction::triggered, this, [this]() {
if (!m_textView) return;

if (m_textAptXmlMode) {
  m_textAptXmlCached = m_textView->toPlainText();
  m_textView->document()->setModified(false);
  toastOk("APT XML updated (not written to disk)");
  return;
}

// External text file mode: write back to disk with safe-write + backup.
if (m_textExternalMode) {
  const QString outPath = m_textExternalPath;
  if (outPath.isEmpty()) {
    const QString savePath = QFileDialog::getSaveFileName(
        this,
        "Save Text File",
        QDir::homePath(),
        "Text/Config Files (*.txt *.xml *.cfg *.conf *.ini *.json *.yaml *.yml *.lua *.js *.css *.html *.htm);;All Files (*)");
    if (savePath.isEmpty()) return;
    m_textExternalPath = savePath;
  }

  gf::core::SafeWriteOptions opt;
  opt.make_backup = true;
  opt.max_bytes = 128ull * 1024ull * 1024ull;

  const std::string text = m_textView->toPlainText().toStdString();
  const auto res = gf::core::safe_write_text(std::filesystem::path(m_textExternalPath.toStdString()),
                                             text,
                                             opt);
  if (!res.ok) {
    showErrorDialog("Save Text", "Save failed.", QString::fromStdString(res.message), true);
    return;
  }

  m_textView->document()->setModified(false);
  QString msg = QString("Saved %1").arg(QFileInfo(m_textExternalPath).fileName());
  if (res.backup_path) {
    msg += QString(" (backup: %1)").arg(QFileInfo(QString::fromStdString(res.backup_path->string())).fileName());
  }
  toastOk(msg);
  return;
}

if (!m_tree) return;
    auto* it = m_tree->currentItem();
    if (!it) return;

    const bool embedded = it->data(0, Qt::UserRole + 3).toBool();
    if (!embedded) return;

    const QString containerPath = it->data(0, Qt::UserRole).toString();
    const qulonglong entryIndexQ = it->data(0, Qt::UserRole + 6).toULongLong();
    const std::uint32_t entryIndex = static_cast<std::uint32_t>(entryIndexQ);
    if (containerPath.isEmpty()) return;

    const QByteArray newBytes = m_textView->toPlainText().toUtf8();
    if (QMessageBox::question(this, "Apply Changes", "Replace the selected embedded entry with the edited text?\n\nA backup will be created.") != QMessageBox::Yes) {
      return;
    }

    std::string err;

    // Reuse the live in-memory editor if it covers the same container, so that
    // any previously unsaved replacements are preserved.  Fall back to loading
    // from disk only when no live editor exists for this path.
    std::optional<gf::core::AstContainerEditor> edOpt;
    if (m_liveAstEditor && m_liveAstPath == containerPath) {
      edOpt = *m_liveAstEditor;
    } else {
      edOpt = gf::core::AstContainerEditor::load(
          std::filesystem::path(containerPath.toStdString()), &err);
      if (!edOpt.has_value()) {
        showErrorDialog("Apply Changes",
                        "Failed to load container.",
                        QString::fromStdString(err),
                        true);
        return;
      }
    }

    std::vector<std::uint8_t> v(newBytes.begin(), newBytes.end());
    if (!edOpt->replaceEntryBytes(entryIndex, v,
                                  gf::core::AstContainerEditor::ReplaceMode::PreserveZlibIfPresent,
                                  &err)) {
      showErrorDialog("Apply Changes",
                      "Failed to apply changes.",
                      QString::fromStdString(err),
                      true);
      return;
    }

    // Write to disk and update the live editor so subsequent saves see the change.
    if (!edOpt->writeInPlace(&err, /*makeBackup=*/true)) {
      showErrorDialog("Apply Changes",
                      "Failed to write container.",
                      QString::fromStdString(err),
                      true);
      return;
    }

    m_liveAstEditor = std::make_unique<gf::core::AstContainerEditor>(std::move(*edOpt));
    m_liveAstPath = containerPath;
    setDirty(true);

    statusBar()->showMessage("Applied text changes (backup created)", 3000);
    if (m_tree) showViewerForItem(it);
  });

  // Re-fit the texture preview when the user switches to the Texture tab.
  connect(m_viewTabs, &QTabWidget::currentChanged, this, [this](int idx) {
    if (idx != 2) return;
    if (m_textureOriginal.isNull() || !m_imageView) return;
    applyTextureZoom();
  });

  m_viewTabs->setVisible(false);

  vlayout->addWidget(m_viewerLabel);
  vlayout->addWidget(m_viewTabs, 1);
  m_viewerHost->setLayout(vlayout);

  setCentralWidget(m_viewerHost);

  updateDocumentActions();
}

bool MainWindow::openExternalTextFile(const QString& path)
{
  if (!m_textView) return false;

  // If switching away from embedded view, clear any previous selection context.
  m_textExternalMode = true;
  m_textExternalPath = QDir::toNativeSeparators(path);

  if (m_viewTabs) {
    m_viewTabs->setVisible(true);
    // Text tab index is 1 (Hex=0, Text=1, Texture=2, RSF=3)
    m_viewTabs->setCurrentIndex(1);
  }

  if (m_textExternalPath.isEmpty()) {
    m_textView->setPlainText(QString());
    m_textView->document()->setModified(false);
    if (m_textView) m_textView->setReadOnly(!editingEnabled());
    m_textExternalSuggestedName = "untitled.txt";
    toastOk("Opened external text editor");
    return true;
  }

  QFile f(m_textExternalPath);
  if (!f.open(QIODevice::ReadOnly)) {
    showErrorDialog("Open Text File", "Failed to open file.", m_textExternalPath, true);
    return false;
  }

  static constexpr qint64 kMaxBytes = 32ll * 1024ll * 1024ll; // 32 MiB
  if (f.size() > kMaxBytes) {
    showErrorDialog("Open Text File", "File is too large for the text editor (32 MiB limit).", QString(), true);
    return false;
  }

  const QByteArray bytes = f.readAll();
  const QString text = QString::fromUtf8(bytes);
  m_textView->setPlainText(text);
  m_textView->document()->setModified(false);
    if (m_textView) m_textView->setReadOnly(!editingEnabled());

  m_textExternalSuggestedName = QFileInfo(m_textExternalPath).fileName();
  if (m_statusDocLabel) {
    m_statusDocLabel->setText(QString("Text: %1").arg(QFileInfo(m_textExternalPath).fileName()));
    m_statusDocLabel->setToolTip(m_textExternalPath);
  }

  toastOk(QString("Opened %1").arg(QFileInfo(m_textExternalPath).fileName()));
  return true;
}

void MainWindow::onOpenFile() {
  const QString startDir = m_doc.path.isEmpty() ? QDir::homePath() : QFileInfo(m_doc.path).absolutePath();
  const QString path = QFileDialog::getOpenFileName(this,
                                                    "Open Archive",
                                                    startDir,
                                                    "EA Archives (*.ast *.bgfa);;All Files (*.*)");
  if (path.isEmpty()) return;
  openStandaloneAst(path);
}

void MainWindow::onOpenApt() {
  const QString aptPath = QFileDialog::getOpenFileName(
      this,
      "Open APT",
      QString(),
      "APT files (*.apt);;All files (*.*)");
  if (aptPath.isEmpty()) return;
  openStandaloneApt(aptPath);
}

void MainWindow::openStandaloneApt(const QString& aptPath) {
  gf::core::logInfo(gf::core::LogCategory::General,
                    "Opening standalone APT", aptPath.toStdString());
  setMode(Mode::Standalone);
  m_cacheId = cacheIdFromSeed(aptPath);
  m_nameCache.loadForGame(m_cacheId);

  // This is not an AST, but we reuse DocumentLifecycle for basic window title + dirty.
  m_doc.path = aptPath;
  setDirty(false);
  ++m_treeToken;
  m_tree->clear();

  const QFileInfo fi(aptPath);
  const QString constPath = fi.dir().absoluteFilePath(fi.completeBaseName() + ".const");

  std::string err;
  const auto fileOpt = gf::apt::read_apt_file(aptPath.toStdString(), constPath.toStdString(), &err);
  if (!fileOpt) {
    showErrorDialog("Open APT Failed",
                    "Could not open this APT.\n\nAPT files require a matching .const next to them.",
                    QString::fromStdString(err));
    return;
  }
  const auto& file = *fileOpt;
  const gf::apt::AptSummary& sum = file.summary;

  auto* root = new QTreeWidgetItem(m_tree, QStringList()
                                            << ("APT: " + fi.fileName())
                                            << "APT"
                                            << "");
  root->setToolTip(0, aptPath + "\n" + constPath);
  root->setData(0, Qt::UserRole, aptPath);
  root->setData(0, Qt::UserRole + 1, qulonglong(0));
  root->setData(0, Qt::UserRole + 2, qulonglong(QFileInfo(aptPath).size()));
  root->setData(0, Qt::UserRole + 3, false);

  // Summary node
  {
    auto* meta = new QTreeWidgetItem(root, QStringList()
                                              << "Summary"
                                              << "APT"
                                              << QString("%1 bytes").arg(QFileInfo(aptPath).size()));
    meta->setData(0, Qt::UserRole, aptPath);
    meta->setData(0, Qt::UserRole + 1, qulonglong(sum.aptdataoffset));
    meta->setData(0, Qt::UserRole + 2, qulonglong(64));
    meta->setData(0, Qt::UserRole + 3, false);
    meta->setToolTip(0,
                     QString("Screen: %1x%2\nFrames: %3\nCharacters: %4\nImports: %5\nExports: %6\nMovie offset: 0x%7")
                         .arg(sum.screensizex)
                         .arg(sum.screensizey)
                         .arg(sum.framecount)
                         .arg(sum.charactercount)
                         .arg(sum.importcount)
                         .arg(sum.exportcount)
                         .arg(QString::number(sum.aptdataoffset, 16)));
  }

  // Key slices (hex preview friendly)
  auto* tables = new QTreeWidgetItem(root, QStringList() << "Tables" << "APT" << "");
  for (const auto& s : file.slices) {
    auto* it = new QTreeWidgetItem(tables, QStringList()
                                             << QString::fromStdString(s.name)
                                             << "APT"
                                             << QString::number(qulonglong(s.size)));
    it->setData(0, Qt::UserRole, aptPath);
    it->setData(0, Qt::UserRole + 1, qulonglong(s.offset));
    it->setData(0, Qt::UserRole + 2, qulonglong(s.size));
    it->setData(0, Qt::UserRole + 3, false);
    it->setToolTip(0, QString("Offset: 0x%1\nSize: %2")
                           .arg(QString::number(qulonglong(s.offset), 16))
                           .arg(QString::number(qulonglong(s.size))));
  }

  m_tree->expandItem(root);
  m_tree->expandItem(tables);

  // Parsed APT tables (best-effort)
  {
    auto* importsNode = new QTreeWidgetItem(root, QStringList() << QString("Imports (%1)").arg(file.imports.size()) << "APT" << "");
    for (std::size_t i = 0; i < file.imports.size(); ++i) {
      const auto& im = file.imports[i];
      const QString label = QString("%1: %2 :: %3")
                                .arg(qulonglong(i))
                                .arg(QString::fromStdString(im.movie))
                                .arg(QString::fromStdString(im.name));
      auto* it = new QTreeWidgetItem(importsNode, QStringList() << label << "Import" << "16");
      it->setData(0, Qt::UserRole, aptPath);
      it->setData(0, Qt::UserRole + 1, qulonglong(im.offset));
      it->setData(0, Qt::UserRole + 2, qulonglong(16));
      it->setData(0, Qt::UserRole + 3, false);
      it->setToolTip(0, QString("Character: %1\nOffset: 0x%2")
                             .arg(im.character)
                             .arg(QString::number(qulonglong(im.offset), 16)));
    }

    auto* exportsNode = new QTreeWidgetItem(root, QStringList() << QString("Exports (%1)").arg(file.exports.size()) << "APT" << "");
    for (std::size_t i = 0; i < file.exports.size(); ++i) {
      const auto& ex = file.exports[i];
      const QString label = QString("%1: %2")
                                .arg(qulonglong(i))
                                .arg(QString::fromStdString(ex.name));
      auto* it = new QTreeWidgetItem(exportsNode, QStringList() << label << "Export" << "8");
      it->setData(0, Qt::UserRole, aptPath);
      it->setData(0, Qt::UserRole + 1, qulonglong(ex.offset));
      it->setData(0, Qt::UserRole + 2, qulonglong(8));
      it->setData(0, Qt::UserRole + 3, false);
      it->setToolTip(0, QString("Character: %1\nOffset: 0x%2")
                             .arg(ex.character)
                             .arg(QString::number(qulonglong(ex.offset), 16)));
    }

    auto* framesNode = new QTreeWidgetItem(root, QStringList() << QString("Frames (%1)").arg(file.frames.size()) << "APT" << "");
    for (std::size_t i = 0; i < file.frames.size(); ++i) {
      const auto& fr = file.frames[i];
      const QString label = QString("Frame %1 (%2 items)").arg(qulonglong(i)).arg(fr.frameitemcount);
      auto* it = new QTreeWidgetItem(framesNode, QStringList() << label << "Frame" << "8");
      it->setData(0, Qt::UserRole, aptPath);
      it->setData(0, Qt::UserRole + 1, qulonglong(fr.offset));
      it->setData(0, Qt::UserRole + 2, qulonglong(8));
      it->setData(0, Qt::UserRole + 3, false);
      it->setToolTip(0, QString("Items ptr: 0x%1\nOffset: 0x%2")
                             .arg(QString::number(qulonglong(fr.items_offset), 16))
                             .arg(QString::number(qulonglong(fr.offset), 16)));
    }

    // Count local (non-null) characters for the header label.
    const std::size_t localCharCount = std::count_if(
        file.characters.begin(), file.characters.end(),
        [](const gf::apt::AptCharacter& c){ return c.type != 0; });
    auto* charsNode = new QTreeWidgetItem(root,
        QStringList() << QString("Characters (%1)").arg(localCharCount) << "APT" << "");
    for (std::size_t i = 0; i < file.characters.size(); ++i) {
      const auto& ch = file.characters[i];
      // Type-0 entries are import-placeholder slots; show them but mark clearly.
      const QString typeName = (ch.type == 0)
          ? QString("import slot")
          : QString::fromStdString(gf::apt::aptCharTypeName(ch.type));
      const QString label = QString("Character %1 — %2").arg(qulonglong(i)).arg(typeName);
      auto* it = new QTreeWidgetItem(charsNode, QStringList() << label << "Character" << "8");
      it->setData(0, Qt::UserRole, aptPath);
      it->setData(0, Qt::UserRole + 1, qulonglong(ch.offset));
      it->setData(0, Qt::UserRole + 2, qulonglong(8));
      it->setData(0, Qt::UserRole + 3, false);
      it->setToolTip(0, QString("ID: %1\nType: %2 (%3)\nSignature: 0x%4\nOffset: 0x%5")
                             .arg(qulonglong(i))
                             .arg(ch.type)
                             .arg(typeName)
                             .arg(QString::number(qulonglong(ch.signature), 16))
                             .arg(QString::number(qulonglong(ch.offset), 16)));
    }
  }

  // Status bar context
  m_statusContainerPath = aptPath;
  m_statusEntryName = "(none)";
  m_statusEntryType = "APT";
  m_statusEntrySize = 0;
  m_statusEntryFlags = 0;
  updateStatusBar();

  updateStatusSelection(root);
  showViewerForItem(root);
  updateDocumentActions();
}

void MainWindow::onSaveAs() {
  if (m_saveInProgress) {
    showInfoDialog("Save As", "A save is already in progress.");
    return;
  }

  const QString srcPath = m_liveAstEditor ? m_liveAstPath : m_doc.path;
  if (srcPath.isEmpty()) {
    showInfoDialog("Save As", "No document open.");
    return;
  }

  QFileInfo fi(srcPath);
  const QString base = fi.completeBaseName().isEmpty() ? fi.fileName() : fi.completeBaseName();
  const QString ext = fi.suffix().isEmpty() ? QString("ast") : fi.suffix();
  const QString suggested = fi.absoluteDir().absoluteFilePath(base + "_saveas." + ext);
  const QString outPath = QFileDialog::getSaveFileName(this,
                                                       "Save As",
                                                       suggested,
                                                       "EA Archives (*.ast *.bgfa);;All Files (*.*)");
  if (outPath.isEmpty()) return;

  gf::core::logBreadcrumb(gf::core::LogCategory::FileIO,
                          "Save As: " + outPath.toStdString());

  struct SaveAsResult {
    bool ok = false;
    QString error;
  };

  m_saveInProgress = true;
  if (m_actSave) m_actSave->setEnabled(false);
  if (m_actSaveAs) m_actSaveAs->setEnabled(false);
  if (m_actRevert) m_actRevert->setEnabled(false);
  statusBar()->showMessage(QString("Saving copy to %1...").arg(QDir::toNativeSeparators(outPath)));

  std::optional<gf::core::AstContainerEditor> editorSnapshot;
  if (m_liveAstEditor) editorSnapshot = *m_liveAstEditor;
  const std::string srcPathStd = srcPath.toStdString();
  const std::string outPathStd = outPath.toStdString();

  auto* watcher = new QFutureWatcher<SaveAsResult>(this);
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, outPath]() {
    const SaveAsResult result = watcher->result();
    watcher->deleteLater();

    m_saveInProgress = false;
    updateDocumentActions();

    if (!result.ok) {
      gf::core::logError(gf::core::LogCategory::FileIO,
                         "Save As write failed", result.error.toStdString());
      showErrorDialog("Save As", "Write failed.", result.error, false);
      return;
    }

    gf::core::logInfo(gf::core::LogCategory::FileIO,
                      "Save As succeeded", outPath.toStdString());
    toastOk(QString("Saved As %1").arg(QFileInfo(outPath).fileName()));
    statusBar()->showMessage(QString("Saved As → %1").arg(QDir::toNativeSeparators(outPath)), 4000);
  });

  watcher->setFuture(QtConcurrent::run([editorSnapshot, srcPathStd, outPathStd]() -> SaveAsResult {
    gf::core::SafeWriteOptions opt;
    opt.make_backup = true;

    if (editorSnapshot) {
      std::string err;
      const auto r = gf::core::safe_write_streamed(
          outPathStd,
          [&editorSnapshot, &err](std::ostream& os, std::string& cbErr) {
            if (!editorSnapshot->rebuildToStream(os, &cbErr)) {
              if (cbErr.empty()) cbErr = "Failed to rebuild AST stream.";
              err = cbErr;
              return false;
            }
            return true;
          },
          opt);
      if (!r.ok) return {false, QString::fromStdString(r.message)};
      if (!err.empty()) return {false, QString::fromStdString(err)};
      return {true, {}};
    }

    const auto r = gf::core::safe_write_streamed(
        outPathStd,
        [&srcPathStd](std::ostream& os, std::string& cbErr) {
          std::ifstream in(srcPathStd, std::ios::binary);
          if (!in) {
            cbErr = "Failed to open source file.";
            return false;
          }
          std::array<char, 1024 * 1024> buf{};
          while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const std::streamsize got = in.gcount();
            if (got > 0 && !os.write(buf.data(), got)) {
              cbErr = "Failed while writing streamed copy.";
              return false;
            }
          }
          if (!in.eof()) {
            cbErr = "Failed while reading source file.";
            return false;
          }
          return true;
        },
        opt);
    if (!r.ok) return {false, QString::fromStdString(r.message)};
    return {true, {}};
  }));
}



void MainWindow::onSave() {
  if (!m_doc.dirty) return;
  if (m_saveInProgress) {
    showInfoDialog("Save", "A save is already in progress.");
    return;
  }

  struct SaveResult {
    bool ok = false;
    QString error;
    QString astPath;
  };

  std::optional<gf::core::AstContainerEditor> editorSnapshot;
  QString astPath;
  if (m_liveAstEditor && !m_liveAstPath.isEmpty()) {
    editorSnapshot = *m_liveAstEditor;
    astPath = m_liveAstPath;
  }

  gf::core::logBreadcrumb(gf::core::LogCategory::FileIO,
                          "Save: " + (astPath.isEmpty() ? "<no path>" : astPath.toStdString()));

  m_saveInProgress = true;
  if (m_actSave) m_actSave->setEnabled(false);
  if (m_actSaveAs) m_actSaveAs->setEnabled(false);
  if (m_actRevert) m_actRevert->setEnabled(false);
  statusBar()->showMessage("Saving...");

  auto* watcher = new QFutureWatcher<SaveResult>(this);
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher]() {
    const SaveResult result = watcher->result();
    watcher->deleteLater();

    m_saveInProgress = false;
    updateDocumentActions();

    if (!result.ok) {
      gf::core::logError(gf::core::LogCategory::FileIO,
                         "Save failed: write AST container", result.error.toStdString());
      showErrorDialog("Save", "Failed to write AST container.", result.error, true);
      return;
    }

    const bool ok = m_nameCache.save();
    if (!ok) {
      gf::core::logError(gf::core::LogCategory::FileIO,
                         "Save failed: could not save name cache");
      showErrorDialog("Save", "Failed to save name cache.", QString(), true);
      return;
    }
    setDirty(false);
    QString msg = "Saved.";
    const QString backup = m_nameCache.lastBackupPath();
    if (!backup.isEmpty()) {
      msg += QString(" Backup: %1").arg(QFileInfo(backup).fileName());
    }
    if (!result.astPath.isEmpty()) {
      msg += QString(" AST backup created next to %1.").arg(QFileInfo(result.astPath).fileName());
    }
    statusBar()->showMessage(msg, 3500);
  });

  watcher->setFuture(QtConcurrent::run([editorSnapshot, astPath]() mutable -> SaveResult {
    if (editorSnapshot && !astPath.isEmpty()) {
      std::string err;
      if (!editorSnapshot->writeInPlace(&err, true)) {
        return {false, QString::fromStdString(err), astPath};
      }
    }
    return {true, {}, astPath};
  }));
}

void MainWindow::onRevert() {
  if (m_saveInProgress) {
    showInfoDialog("Revert", "Please wait for the current save to finish before reverting.");
    return;
  }
  if (!DocumentLifecycle::maybePromptDiscard(this, m_doc.dirty)) return;

  m_liveAstEditor.reset();
  m_liveAstPath.clear();
  m_lastReplaceUndo = LastReplaceUndo{};
  if (m_actUndoLastReplace) m_actUndoLastReplace->setEnabled(false);

  // Reload name cache from disk.
  if (!m_cacheId.isEmpty()) {
    m_nameCache.loadForGame(m_cacheId);
  }

  // Rebuild UI based on current mode.
  if (m_mode == Mode::Standalone) {
    const QString p = m_doc.path;
    if (!p.isEmpty()) openStandaloneAst(p);
    else setDirty(false);
  } else {
    if (!m_lastGameRootPath.isEmpty()) {
      openGame(m_lastGameDisplayName,
               m_lastGameRootPath,
               m_lastGameBaseContentDir,
               m_lastGameUpdateContentDir);
    } else {
      setDirty(false);
    }
  }

  statusBar()->showMessage("Reverted.", 2500);
}

void MainWindow::exportCopyOf(const QString& sourcePath) {
  gf::core::logBreadcrumb(gf::core::LogCategory::FileIO,
                          "Export copy: " + sourcePath.toStdString());
  QFileInfo fi(sourcePath);
  const QString base = fi.completeBaseName().isEmpty() ? fi.fileName() : fi.completeBaseName();
  QString ext = normalizeExt(fi.suffix());
  if (ext.isEmpty()) ext = "bin";

  const QString suggested = fi.absoluteDir().absoluteFilePath(base + "_copy." + ext);
  QString outPath = QFileDialog::getSaveFileName(this,
                                                 "Export Copy",
                                                 suggested,
                                                 saveFilterForExt(ext));
  if (outPath.isEmpty()) return;
  outPath = ensureHasExtension(outPath, ext);

  QFile in(sourcePath);
  if (!in.open(QIODevice::ReadOnly)) {
    gf::core::logError(gf::core::LogCategory::FileIO,
                       "Export failed: cannot open source file for reading", sourcePath.toStdString());
    showErrorDialog("Export Failed", "Failed to open source file for reading.", QString(), false);
    return;
  }

  constexpr qint64 kMaxExport = 1024ll * 1024ll * 1024ll; // 1 GiB safety cap
  if (in.size() > kMaxExport) {
    gf::core::logWarn(gf::core::LogCategory::FileIO,
                      "Export refused: file exceeds 1 GiB safety cap", sourcePath.toStdString());
    showErrorDialog("Export Failed", "Refusing to export: file is larger than the safety cap (1 GiB).", "If you need this, we can implement streaming export next.", false);
    return;
  }

  const QByteArray bytes = in.readAll();
  in.close();

  const auto* bptr = reinterpret_cast<const std::byte*>(bytes.constData());
  gf::core::SafeWriteOptions opt;
  opt.make_backup = true; // If user overwrites an existing file, preserve it.
  const auto r = gf::core::safe_write_bytes(outPath.toStdString(),
                                            std::span<const std::byte>(bptr, static_cast<std::size_t>(bytes.size())),
                                            opt);
  if (!r.ok) {
    gf::core::logError(gf::core::LogCategory::FileIO,
                       "Export failed: write error", r.message + " -> " + outPath.toStdString());
    showErrorDialog("Export Failed", "Export failed.", QString::fromStdString(r.message), false);
    return;
  }

  QString msg = "Exported successfully.";
  if (r.backup_path) {
    msg += QString("\nBackup created: %1").arg(QString::fromStdString(r.backup_path->string()));
  }
  gf::core::logInfo(gf::core::LogCategory::FileIO,
                    "Export succeeded", outPath.toStdString());
  toastOk(QString("Exported %1").arg(QFileInfo(outPath).fileName()));
  showInfoDialog("Export", msg);
}



void MainWindow::onUndoLastReplace() {
  if (!m_lastReplaceUndo.valid) {
    showInfoDialog("Undo Last Replace", "There is no in-memory replacement to undo.");
    return;
  }

  gf::core::logBreadcrumb(gf::core::LogCategory::TextureReplace,
                          "Undo last replace: " + m_lastReplaceUndo.displayName.toStdString());

  std::string err;
  std::optional<gf::core::AstContainerEditor> loaded;
  if (m_liveAstEditor && m_liveAstPath == m_lastReplaceUndo.containerPath) {
    loaded = *m_liveAstEditor;
  } else {
    loaded = gf::core::AstContainerEditor::load(m_lastReplaceUndo.containerPath.toStdString(), &err);
    if (!loaded.has_value()) {
      showErrorDialog("Undo Last Replace Failed",
                      "The AST container could not be loaded.",
                      QString::fromStdString(err),
                      false);
      return;
    }
  }

  const auto prev = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(m_lastReplaceUndo.previousStoredBytes.constData()),
      static_cast<std::size_t>(m_lastReplaceUndo.previousStoredBytes.size()));
  if (!loaded->replaceEntryBytes(m_lastReplaceUndo.entryIndex,
                                 prev,
                                 gf::core::AstContainerEditor::ReplaceMode::Raw,
                                 &err)) {
    showErrorDialog("Undo Last Replace Failed",
                    "The previous entry bytes could not be restored.",
                    QString::fromStdString(err),
                    true);
    return;
  }

  m_liveAstEditor = std::make_unique<gf::core::AstContainerEditor>(std::move(*loaded));
  m_liveAstPath = m_lastReplaceUndo.containerPath;
  setDirty(true);

  if (!m_lastReplaceUndo.itemCacheKey.isEmpty()) {
    if (auto* it = findTreeItemByCacheKey(m_tree, m_lastReplaceUndo.itemCacheKey)) {
      it->setData(0, Qt::UserRole + 30, m_lastReplaceUndo.previousStoredBytes);
      if (!m_lastReplaceUndo.previousPreviewBytes.isEmpty()) {
        it->setData(0, Qt::UserRole + 31, m_lastReplaceUndo.previousPreviewBytes);
      } else {
        it->setData(0, Qt::UserRole + 31, QVariant());
      }
      showViewerForItem(it);
    }
  }

  const QString label = m_lastReplaceUndo.displayName.isEmpty() ? "entry" : m_lastReplaceUndo.displayName;
  m_lastReplaceUndo = LastReplaceUndo{};
  if (m_actUndoLastReplace) m_actUndoLastReplace->setEnabled(false);
  statusBar()->showMessage(QString("Undid last in-memory replace for %1.").arg(label), 4000);
}

void MainWindow::refreshCurrentArchiveView() {
  const QString currentKey = m_tree && m_tree->currentItem() ? makeTreeCacheKey(m_tree->currentItem()) : QString();
  if (m_mode == Mode::Standalone) {
    const QString p = m_doc.path;
    if (!p.isEmpty()) openStandaloneAst(p);
  } else if (!m_lastGameRootPath.isEmpty()) {
    openGame(m_lastGameDisplayName,
             m_lastGameRootPath,
             m_lastGameBaseContentDir,
             m_lastGameUpdateContentDir);
  }
  if (!currentKey.isEmpty()) {
    if (auto* it = findTreeItemByCacheKey(m_tree, currentKey)) {
      m_tree->setCurrentItem(it);
    }
  }
  statusBar()->showMessage("Archive view refreshed.", 2500);
}

void MainWindow::onRestoreLatestBackup() {
  if (!editingEnabled()) {
    showInfoDialog("Restore Backup", "Editing is currently OFF.\n\nEnable Tools > Enable Editing (unsafe) to allow restore operations.");
    return;
  }
  if (m_doc.path.isEmpty()) return;

  const QString targetPath = m_doc.path;
  gf::core::logBreadcrumb(gf::core::LogCategory::FileIO,
                          "Restore latest backup: " + targetPath.toStdString());
  const QFileInfo fi(targetPath);
  const QDir dir = fi.absoluteDir();
  const QString baseName = fi.fileName();

  // Find backups created by safe_write_bytes: <filename>.bak_YYYYMMDD_HHMMSS
  const QString pattern = baseName + ".bak_*";
  const QFileInfoList backups = dir.entryInfoList(QStringList() << pattern, QDir::Files, QDir::Time);

  if (backups.isEmpty()) {
    showInfoDialog("Restore Backup", "No backups were found next to this file.");
    return;
  }

  const QFileInfo newest = backups.first();
  const auto ans = QMessageBox::question(
      this,
      "Restore Backup",
      QStringLiteral("Restore the latest backup?\n\nTarget:\n%1\n\nBackup:\n%2\n\nThis will overwrite the target (and will create a new backup of the current file).")
          .arg(QDir::toNativeSeparators(targetPath))
          .arg(QDir::toNativeSeparators(newest.absoluteFilePath())),
      QMessageBox::Yes | QMessageBox::No,
      QMessageBox::No);
  if (ans != QMessageBox::Yes) return;

  QFile b(newest.absoluteFilePath());
  if (!b.open(QIODevice::ReadOnly)) {
    showErrorDialog("Restore Backup Failed", "Failed to open the backup for reading.", QString(), false);
    return;
  }
  const QByteArray bytes = b.readAll();
  b.close();

  const auto* bptr = reinterpret_cast<const std::byte*>(bytes.constData());
  gf::core::SafeWriteOptions opt;
  opt.make_backup = true;
  const auto r = gf::core::safe_write_bytes(targetPath.toStdString(),
                                            std::span<const std::byte>(bptr, static_cast<std::size_t>(bytes.size())),
                                            opt);
  if (!r.ok) {
    gf::core::logError(gf::core::LogCategory::FileIO,
                       "Restore backup failed", r.message + " -> " + targetPath.toStdString());
    showErrorDialog("Restore Backup Failed", "Restore failed.", QString::fromStdString(r.message), false);
    return;
  }

  gf::core::logInfo(gf::core::LogCategory::FileIO,
                    "Restore backup succeeded", targetPath.toStdString());
  statusBar()->showMessage("Restored latest backup (current file was backed up).", 4000);
  // Reload UI
  openStandaloneAst(targetPath);
}


void MainWindow::updateDocumentActions() {
  const bool hasDoc = !m_doc.path.isEmpty();

  auto hasBackupNextToDoc = [&]() -> bool {
    if (!hasDoc) return false;
    const QFileInfo fi(m_doc.path);
    const QDir dir = fi.absoluteDir();
    const QString baseName = fi.fileName();
    const QString pattern = baseName + ".bak_*";
    const QFileInfoList backups = dir.entryInfoList(QStringList() << pattern, QDir::Files, QDir::Time);
    return !backups.isEmpty();
  };

  if (m_actOpen) m_actOpen->setEnabled(!m_saveInProgress);
  if (m_actRestoreBackup) m_actRestoreBackup->setEnabled(!m_saveInProgress && hasDoc && editingEnabled() && hasBackupNextToDoc());
  if (m_actSaveAs) m_actSaveAs->setEnabled(!m_saveInProgress && hasDoc);
  if (m_actSave) m_actSave->setEnabled(!m_saveInProgress && hasDoc && m_doc.dirty);
  if (m_actRevert) m_actRevert->setEnabled(!m_saveInProgress && hasDoc && m_doc.dirty);
  if (m_actUndoLastReplace) m_actUndoLastReplace->setEnabled(!m_saveInProgress && m_lastReplaceUndo.valid);
}

void MainWindow::setDirty(bool dirty) {
  if (m_doc.dirty == dirty) return;
  m_doc.dirty = dirty;
  updateWindowTitle();
  updateDocumentActions();
  updateStatusBar();
}

void MainWindow::updateWindowTitle() {
  QString title;
  if (m_mode == Mode::Game) title = "ASTra Core - AST Editor (Game Mode)";
  else title = "ASTra Core - AST Editor (Standalone)";

  if (m_mode == Mode::Standalone && !m_doc.path.isEmpty()) {
    title += QString(" — %1").arg(QFileInfo(m_doc.path).fileName());
  }
  if (m_doc.dirty) title += " *";
  setWindowTitle(title);
}

void MainWindow::startIndexing(const QString& displayName,
                               const QString& rootPath,
                               const QString& baseContentDir,
                               const QString& updateContentDir) {
  ++m_treeToken;
  m_tree->clear();
  m_parseKeysInFlight.clear();
  m_parsedKeysDone.clear();

  if (m_header) {
    m_header->setText(QString("Game Mode: %1\n%2").arg(displayName, rootPath));
  }

  // Root node
  auto* gameRoot = new QTreeWidgetItem(m_tree, QStringList() << displayName << rootPath);
  gameRoot->setExpanded(true);

  auto* baseBucket = new QTreeWidgetItem(gameRoot, QStringList() << "Base (indexing...)" << "");
  baseBucket->setToolTip(0, baseContentDir);
  baseBucket->setExpanded(true);

  QTreeWidgetItem* updateBucket = nullptr;
  if (!updateContentDir.trimmed().isEmpty()) {
    updateBucket = new QTreeWidgetItem(gameRoot, QStringList() << "Update/Patch (indexing...)" << "");
    updateBucket->setToolTip(0, updateContentDir);
    updateBucket->setExpanded(false);
  }

  // Placeholder children so expand arrows exist immediately.
  baseBucket->addChild(new QTreeWidgetItem(QStringList() << "(Indexing...)" << ""));
  if (updateBucket) updateBucket->addChild(new QTreeWidgetItem(QStringList() << "(Indexing...)" << ""));

    // Build bucket specs for the indexer (filesystem scan only).
  // We intentionally scan *directories* recursively and avoid parsing AST contents here.
  QVector<AstBucketSpec> buckets;

  auto addIfDir = [&](const QString& bucketName, const QString& dirPath) {
    if (dirPath.isEmpty()) return;
    const QFileInfo fi(dirPath);
    if (!fi.exists() || !fi.isDir()) return;
    buckets.push_back(AstBucketSpec{bucketName, fi.absoluteFilePath()});
  };

  // Heuristic root selection (safe): scan a few well-known layout roots per platform/title.
  // This prevents "0 ASTs" when a stored path is slightly off.
  const QString root = QFileInfo(rootPath).absoluteFilePath();

  QStringList baseCandidates;
  if (!baseContentDir.isEmpty()) baseCandidates << baseContentDir;

  // PS3 layout: <root>/PS3_GAME/USRDIR (common), sometimes just <root>/USRDIR.
  const QString ps3Usrdir = QDir(root).filePath("PS3_GAME/USRDIR");
  const QString ps3Game   = QDir(root).filePath("PS3_GAME");
  const QString usrdir    = QDir(root).filePath("USRDIR");

  // PS4 pkg-extracted layout often has Image0/ (and Sc0/), with data under Image0/.
  const QString image0 = QDir(root).filePath("Image0");

  // Xbox 360 extracted layout is commonly flat at <root>/ with default.xex present.
  const QString defaultXex = QDir(root).filePath("default.xex");

  // Prefer candidates that match detected layout.
  const bool looksPs3 = QFileInfo(ps3Game).exists() && QFileInfo(ps3Game).isDir();
  const bool looksPs4 = QFileInfo(image0).exists() && QFileInfo(image0).isDir();
  const bool looksX360 = QFileInfo(defaultXex).exists();

  if (looksPs3) {
    // PS3: only index from the *content* dir so the tree doesn't show platform
    // structural folders (PS3_GAME / USRDIR). This keeps the user focused on
    // the actual archives.
    baseCandidates.clear();

    const QFileInfo fiUsrdir(ps3Usrdir);
    const QFileInfo fiAltUsrdir(usrdir);
    const QFileInfo fiProvided(baseContentDir);

    if (fiUsrdir.exists() && fiUsrdir.isDir()) {
      baseCandidates << fiUsrdir.absoluteFilePath();
    } else if (fiAltUsrdir.exists() && fiAltUsrdir.isDir()) {
      baseCandidates << fiAltUsrdir.absoluteFilePath();
    } else if (!baseContentDir.isEmpty() && fiProvided.exists() && fiProvided.isDir()) {
      baseCandidates << fiProvided.absoluteFilePath();
    } else {
      // Last resort: fall back to root, but this may show PS3_GAME/USRDIR nodes.
      baseCandidates << root;
    }
  } else if (looksPs4) {
    // Prefer Image0 for PS4; avoid showing an extra Image0 folder if possible.
    baseCandidates.clear();
    const QFileInfo fiImage0(image0);
    if (fiImage0.exists() && fiImage0.isDir()) baseCandidates << fiImage0.absoluteFilePath();
    else baseCandidates << root;
  } else if (looksX360) {
    baseCandidates.clear();
    baseCandidates << root;
  } else {
    // Unknown: be permissive but still directory-only.
    baseCandidates << root;
  }

  // De-dupe while preserving order.
  QStringList uniq;
  for (const auto& c : baseCandidates) {
    const QString abs = QFileInfo(c).absoluteFilePath();
    if (!uniq.contains(abs, Qt::CaseInsensitive)) uniq << abs;
  }

  for (const auto& c : uniq) addIfDir(QStringLiteral("Base"), c);

  if (updateBucket) addIfDir(QStringLiteral("Update"), updateContentDir);

  auto future = QtConcurrent::run([buckets]() {
    return AstIndexer::indexBuckets(buckets);
  });

  auto* watcher = new QFutureWatcher<QVector<AstIndexEntry>>(this);
  connect(watcher, &QFutureWatcher<QVector<AstIndexEntry>>::finished, this,
          [this, watcher, gameRoot, baseBucket, updateBucket]() {
            const auto entries = watcher->result();
            watcher->deleteLater();
            applyIndexToTree(entries, gameRoot, baseBucket, updateBucket);

	            // UX: keep the important nodes opened so the user immediately sees ASTs.
	            if (gameRoot) gameRoot->setExpanded(true);
	            if (baseBucket) baseBucket->setExpanded(true);
	            if (updateBucket) updateBucket->setExpanded(true);
	            m_tree->resizeColumnToContents(0);
	            m_tree->resizeColumnToContents(1);

            statusBar()->showMessage("Ready");

          });
  statusBar()->showMessage("Indexing...");
  watcher->setFuture(future);
}

void MainWindow::applyIndexToTree(const QVector<AstIndexEntry>& entries,
                                  QTreeWidgetItem* gameRoot,
                                  QTreeWidgetItem* baseBucket,
                                  QTreeWidgetItem* updateBucket) {
  if (!gameRoot) return;

  const bool hadTree = (m_tree != nullptr);
  if (hadTree) m_tree->setUpdatesEnabled(false);

  if (baseBucket) baseBucket->takeChildren();
  if (updateBucket) updateBucket->takeChildren();

  int baseCount = 0;
  int updateCount = 0;

  // Folder node cache per bucket: "Base:folder/sub" -> item
  QHash<QString, QTreeWidgetItem*> folderCache;

  auto getOrCreateFolder = [&](QTreeWidgetItem* bucketItem,
                               const QString& bucketName,
                               const QString& relFolder) -> QTreeWidgetItem* {
    if (!bucketItem || relFolder.isEmpty() || relFolder == ".") return bucketItem;

    QString norm = relFolder;
    norm.replace('\\', '/');
    const auto parts = norm.split('/', Qt::SkipEmptyParts);

    QTreeWidgetItem* parent = bucketItem;
    QString built;
    for (const auto& p : parts) {
      if (!built.isEmpty()) built += "/";
      built += p;
      const QString key = bucketName + ":" + built;

      if (auto* existing = folderCache.value(key, nullptr)) {
        parent = existing;
        continue;
      }

      auto* folder = new QTreeWidgetItem(parent, QStringList() << p << "");
      folder->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
      folder->setExpanded(false);
      applyItemIcon(folder, "FOLDER", true);
      folderCache.insert(key, folder);
      parent = folder;
    }
    return parent;
  };

  for (const auto& e : entries) {
    QTreeWidgetItem* bucketItem = nullptr;
    QString bucketName = e.bucketName;

    if (bucketName.compare("Base", Qt::CaseInsensitive) == 0) {
      bucketItem = baseBucket;
      bucketName = "Base";
      ++baseCount;
    } else {
      bucketItem = updateBucket;
      bucketName = "Update";
      ++updateCount;
    }
    if (!bucketItem) continue;

    const QString relFolder = QFileInfo(e.relativePath).path();
    QTreeWidgetItem* parent = getOrCreateFolder(bucketItem, bucketName, relFolder);

    // Force correct columns: [0]=filename, [1]=bytes
    QString astLabel = e.fileName;
    const bool isQklRootAst = isLikelyQklAstPath(e.absolutePath);
    const QString rootCacheKey = NameCache::makeKey(e.absolutePath,
                                                    0,
                                                    0,
                                                    static_cast<quint64>(e.fileSize),
                                                    QStringLiteral("ASTROOT"));
    const QString cachedRootName = m_nameCache.lookup(rootCacheKey);
    if (!isQklRootAst && !cachedRootName.isEmpty() && !isQklStyleFriendlyBase(cachedRootName)) {
      astLabel = QString("%1.ast (%2)").arg(cachedRootName, e.fileName);
    }

    auto* ast = new QTreeWidgetItem(parent, QStringList()
                                            << astLabel
                                            << formatBytes(static_cast<qulonglong>(e.fileSize)));

    ast->setData(0, Qt::UserRole, e.absolutePath);
    ast->setData(0, Qt::UserRole + 60, rootCacheKey);
    const QString astTip = buildTreeItemTooltip(e.fileName, "AST", QFileInfo(e.absolutePath).path(), e.absolutePath, false);
    ast->setToolTip(0, astTip);
    ast->setToolTip(1, QDir::toNativeSeparators(e.absolutePath));
    ast->setToolTip(2, QDir::toNativeSeparators(e.absolutePath));
    ast->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    applyItemIcon(ast, "AST");

    // Stub parsing behind expand
    ast->addChild(new QTreeWidgetItem(QStringList() << "Expand to parse..." << ""));

    // Warm the top-level AST name cache in the background so strong names are available
    // even when the user never expands the AST tree.
    if (!isQklRootAst && cachedRootName.isEmpty()) {
      const QString astPath = e.absolutePath;
      const QString treeKey = makeTreeCacheKey(ast);
      const quint64 token = m_treeToken;
      auto* watcher = new QFutureWatcher<std::tuple<QString, QString, bool>>(this);
      connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, token, treeKey, rootCacheKey, originalFileName = e.fileName]() {
        const auto [chosen, kind, hasApt] = watcher->result();
        watcher->deleteLater();
        if (token != m_treeToken) return;
        if ((chosen.isEmpty() && kind.isEmpty() && !hasApt) || isQklStyleFriendlyBase(chosen)) return;
        QTreeWidgetItem* astItem = findTreeItemByCacheKey(m_tree, treeKey);
        if (!astItem) return;
        if (!chosen.isEmpty()) {
          astItem->setText(0, QString("%1.ast (%2)").arg(chosen, originalFileName));
        }
        m_nameCache.putMeta(rootCacheKey, chosen, kind, hasApt);
      });
      watcher->setFuture(QtConcurrent::run([astPath]() -> std::tuple<QString, QString, bool> {
        try {
          std::string err;
          auto idxOpt = gf::core::AstArchive::readIndex(std::filesystem::path(astPath.toStdString()), &err);
          if (!idxOpt) return {QString(), QString(), false};
          return deriveFriendlyAstContainerMeta(*idxOpt);
        } catch (const std::exception& ex) {
          gf::core::logWarn(gf::core::LogCategory::AstParsing,
                            "Exception while reading AST index for friendly name",
                            ex.what());
          return {QString(), QString(), false};
        } catch (...) {
          gf::core::logWarn(gf::core::LogCategory::AstParsing,
                            "Unknown exception while reading AST index for friendly name");
          return {QString(), QString(), false};
        }
      }));
    }
  }

  if (baseBucket) {
    baseBucket->setText(0, QString("Base (%1 ASTs)").arg(baseCount));
    baseBucket->setText(1, "");
    // keep tooltip path
  }
  if (updateBucket) {
    updateBucket->setText(0, QString("Update/Patch (%1 ASTs)").arg(updateCount));
    updateBucket->setText(1, "");
  }

  if (baseBucket && baseBucket->childCount() == 0) {
    baseBucket->addChild(new QTreeWidgetItem(QStringList() << "(No AST files found)" << ""));
  }
  if (updateBucket && updateBucket->childCount() == 0) {
    updateBucket->addChild(new QTreeWidgetItem(QStringList() << "(No AST files found)" << ""));
  }

  if (hadTree) {
    m_tree->setUpdatesEnabled(true);
    m_tree->viewport()->update();
  }
}


void MainWindow::onItemExpanded(QTreeWidgetItem* item) {
  if (!item) return;

  const QString path = item->data(0, Qt::UserRole).toString();
  if (path.isEmpty()) return;

  const qulonglong baseOffset = item->data(0, Qt::UserRole + 1).toULongLong();
  const qulonglong maxReadable = item->data(0, Qt::UserRole + 2).toULongLong();
  const bool isEmbedded = item->data(0, Qt::UserRole + 3).toBool();
  clearAptViewer();

  const QString cacheKey = isEmbedded ? QString("%1@%2@%3").arg(path).arg(baseOffset).arg(maxReadable) : path;
  // If we've already populated this node, don't re-parse.
  if (m_parsedKeysDone.contains(cacheKey)) return;
  // If a parse is currently running for this key, ignore duplicate expand events.
  if (m_parseKeysInFlight.contains(cacheKey)) return;
  m_parseKeysInFlight.insert(cacheKey);

  // UI: show busy progress while parsing.
  ++m_parseInFlight;
  if (m_parseProgress) {
    m_parseProgress->setRange(0, 0); // indeterminate
    m_parseProgress->setVisible(true);
  }

  // Clear placeholder children now (we'll repopulate once parse completes).
  item->takeChildren();

  struct ParseResult {
    std::optional<gf::core::AstArchive::Index> idx;
    QString message; // error or warning (may be set even when idx is valid)
  };

  // Run parsing off the UI thread.
  auto* watcher = new QFutureWatcher<ParseResult>(this);
  const quint64 token = m_treeToken;

  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, token, path, isEmbedded, baseOffset, maxReadable, cacheKey]() {
    const ParseResult r = watcher->result();
    const auto& idx = r.idx;
    const QString msg = r.message.trimmed();
    watcher->deleteLater();

    // If the tree has been rebuilt (or we're closing), don't touch stale UI items.
    if (token != m_treeToken || !m_tree) {
      m_parseKeysInFlight.remove(cacheKey);
      --m_parseInFlight;
      if (m_parseProgress && m_parseInFlight <= 0) {
        m_parseProgress->setVisible(false);
        m_parseProgress->setRange(0, 100);
        m_parseInFlight = 0;
      }
      return;
    }

    QTreeWidgetItem* item = findTreeItemByCacheKey(m_tree, cacheKey);
    if (!item) {
      m_parseKeysInFlight.remove(cacheKey);
      --m_parseInFlight;
      if (m_parseProgress && m_parseInFlight <= 0) {
        m_parseProgress->setVisible(false);
        m_parseProgress->setRange(0, 100);
        m_parseInFlight = 0;
      }
      return;
    }

    // Mark parse state.
    m_parseKeysInFlight.remove(cacheKey);
    if (idx) m_parsedKeysDone.insert(cacheKey);

    // Hide progress if nothing else in flight.
    --m_parseInFlight;
    if (m_parseProgress && m_parseInFlight <= 0) {
      m_parseProgress->setVisible(false);
      m_parseProgress->setRange(0, 100);
      m_parseInFlight = 0;
    }

    if (!idx) {
      // Allow retry on next expand.
      m_parsedKeysDone.remove(cacheKey);
      auto* err = new QTreeWidgetItem(QStringList()
                                      << QString("(Failed to parse — expand to retry)")
                                      << "Error"
                                      << "");
      err->setFlags(err->flags() & ~Qt::ItemIsSelectable);
      if (!msg.isEmpty()) {
        err->setToolTip(0, msg);
        err->setToolTip(1, msg);
        err->setToolTip(2, msg);
        statusBar()->showMessage(QString("AST parse failed: %1").arg(msg), 8000);
      } else {
        statusBar()->showMessage("AST parse failed.", 5000);
      }
      item->addChild(err);
      return;
    }

    // If we succeeded but have a message, treat it as a non-fatal warning (e.g., truncated index).
    if (!msg.isEmpty()) {
      auto* warn = new QTreeWidgetItem(QStringList() << QString("(Warning: %1)").arg(msg) << "Warning" << "");
      warn->setFlags(warn->flags() & ~Qt::ItemIsSelectable);
      warn->setToolTip(0, msg);
      warn->setToolTip(1, msg);
      warn->setToolTip(2, msg);
      item->addChild(warn);
      statusBar()->showMessage(QString("AST parsed with warnings: %1").arg(msg), 6000);
    }

    // Summary row
    {
      QString platformHint;
      if (idx->is_ps3) platformHint = "PS3";
      else if (idx->is_xbox) platformHint = "Xbox 360";
      else platformHint = "Unknown";

      auto* summary = new QTreeWidgetItem(QStringList()
                                          << QString("(Index: %1 files, %2 tags, %3)")
                                                 .arg(idx->file_count)
                                                 .arg(idx->tag_count)
                                                 .arg(platformHint)
                                          << ""
                                          << "");
      summary->setFlags(summary->flags() & ~Qt::ItemIsSelectable);
      item->addChild(summary);
    }

    const bool isQklRoot = (!isEmbedded && isLikelyQklAstPath(path));

    // Embedded AST naming:
// We intentionally do NOT re-categorize nested ASTs. We also avoid baking "_misc" style suffixes
// into the label here. Instead, after child entries are materialized we may rename this node to
// a strong friendly name derived from contained RSF/texture names (see below).
if (isEmbedded) {
  // no-op here; rename happens after children are created
}


    // Create EASE-style group folders for qkl_* root ASTs.
    QTreeWidgetItem* grpTextures = nullptr;
    QTreeWidgetItem* grpUI = nullptr;
    QTreeWidgetItem* grpModels = nullptr;
    QTreeWidgetItem* grpASTs = nullptr;
    QTreeWidgetItem* grpConfigs = nullptr;
    QTreeWidgetItem* grpDatabase = nullptr;
    QTreeWidgetItem* grpUnknown = nullptr;

    if (isQklRoot) {
      grpTextures = ensureGroup(item, "Textures");
      grpUI = ensureGroup(item, "UI");
      grpModels = ensureGroup(item, "Models");
      grpASTs = ensureGroup(item, "ASTs");
      grpConfigs = ensureGroup(item, "Configs");
      grpDatabase = ensureGroup(item, "Database");
      grpUnknown = ensureGroup(item, "Unknown");
    }

    // Friendly Naming v2: RSF reference map + APT export-ish names.
    const FriendlyOverrides overrides = build_friendly_overrides_v2(*idx, path, static_cast<std::uint64_t>(baseOffset));

    // Entries
    // Collect candidate names for the *container* embedded AST.
// Priority: RSF base name -> texture base name.
QStringList rsfBases;
QStringList texBases;

for (const auto& e : idx->entries) {
      const std::string& rawLabel = !e.display_name.empty() ? e.display_name : e.name;
      const QString fileName = QString("File_%1").arg(static_cast<qulonglong>(e.index), 5, 10, QChar('0'));
      QString displayName = QString::fromStdString(rawLabel.empty() ? fileName.toStdString() : rawLabel);

      // Apply extension hint if missing.
      if (!e.ext_hint.empty() && !displayName.contains('.')) {
        displayName += QString::fromStdString(e.ext_hint);
      }

      const bool rawLooksGeneric = fileName.startsWith("File_", Qt::CaseInsensitive);

      // Friendly Naming v2: if we have a strong RSF/APT-derived name, display it EASE-style.
      auto itO = overrides.byEntryIndex.find(e.index);
      if (itO != overrides.byEntryIndex.end() && (rawLabel.empty() || rawLooksGeneric)) {
        const QString ext = !e.ext_hint.empty() ? QString::fromStdString(e.ext_hint) : QString();
        const QString base = QString::fromStdString(itO->second);
        const QString fileNameWithExt = ext.isEmpty() ? fileName : (fileName + ext);
        const QString baseWithExt = ext.isEmpty() ? base : (base + ext);
        displayName = QString("%1 (%2)").arg(baseWithExt, fileNameWithExt);
      }

      // Type column should be file type only.
      QString type = e.type_hint.empty() ? "Unknown" : QString::fromStdString(e.type_hint);
      if (type == "AST/BGFA") type = "AST";
      if (type == "zlib") type = "ZLIB";
      if (type == "Text") type = "TXT";
      if (type == "Txt") type = "TXT";

      // Fallback classification based on filename when the parser couldn't infer it.
      // This prevents obvious configs (e.g., .xml) from showing up under Unknown.
      if (type == "Unknown") {
        const QString lowerName = displayName.toLower();
        if (lowerName.endsWith(".xml")) type = "XML";
        else if (lowerName.endsWith(".json")) type = "JSON";
        else if (lowerName.endsWith(".ini") || lowerName.endsWith(".cfg")) type = "INI";
        else if (lowerName.endsWith(".txt") || lowerName.endsWith(".csv") || lowerName.endsWith(".log")) type = "TXT";
        else if (lowerName.endsWith(".yaml") || lowerName.endsWith(".yml")) type = "YAML";
      }
      const QString cacheKey = NameCache::makeKey(path,
                                                  static_cast<quint64>(baseOffset),
                                                  static_cast<quint64>(e.data_offset),
                                                  static_cast<quint64>(e.compressed_size),
                                                  type);

      // Friendly Naming v4: apply persistent cache so names appear immediately (no re-derivation).
      if (rawLabel.empty() || rawLooksGeneric) {
        const QString cachedBase = m_nameCache.lookup(cacheKey);
        if (!cachedBase.isEmpty()) {
          const QString ext = !e.ext_hint.empty() ? QString::fromStdString(e.ext_hint) : QString();
          const QString fileNameWithExt = ext.isEmpty() ? fileName : (fileName + ext);
          const QString baseWithExt = ext.isEmpty() ? cachedBase : (cachedBase + ext);
          displayName = QString("%1 (%2)").arg(baseWithExt, fileNameWithExt);
        }
      }

      // Embedded AST containers use a metadata cache keyed by absolute embedded offset/size.
      // This lets qkl_* roots show friendly child AST names immediately on open, without
      // requiring a nested expansion to derive and persist the name first.
      QString embeddedMetaKey;
      if (type == "AST") {
        const std::uint64_t absOffsetForMeta = baseOffset + e.data_offset;
        embeddedMetaKey = cacheKeyForEmbedded(static_cast<quint64>(absOffsetForMeta),
                                              static_cast<std::uint64_t>(e.compressed_size));
        const QString cachedEmbeddedBase = m_nameCache.lookup(embeddedMetaKey);
        if ((rawLabel.empty() || rawLooksGeneric) && !cachedEmbeddedBase.isEmpty()) {
          displayName = QString("%1.ast (%2)").arg(cachedEmbeddedBase, fileName);
        }
      }

      // If v2 overrides provided a strong base name, persist it for next time.
      if (itO != overrides.byEntryIndex.end() && (rawLabel.empty() || rawLooksGeneric)) {
        m_nameCache.put(cacheKey, QString::fromStdString(itO->second));
      }

// Track RSF/texture candidates for renaming this embedded AST node.
// We prefer RSF names because they generally represent the "container" identity.
if (isEmbedded) {
  const QString lowerName = displayName.toLower();
  const QString baseNoParen = displayName.section('(', 0, 0).trimmed(); // strip "(File_XXXXX...)"
  if (type == "RSF") {
    QString b = baseNoParen;
    if (b.endsWith(".rsf", Qt::CaseInsensitive)) b.chop(4);
    if (!b.isEmpty()) rsfBases << b;
  } else if (type == "P3R" || type == "DDS" || type == "XPR" || type == "XPR2") {
    QString b = baseNoParen;
    // strip extension
    const int dot = b.lastIndexOf('.');
    if (dot > 0) b = b.left(dot);
    // common suffix trims
    auto trimSuffix = [&](const QString& sfx) {
      if (b.endsWith(sfx, Qt::CaseInsensitive)) b.chop(sfx.size());
    };
    trimSuffix("_COL");
    trimSuffix("_VECTOR");
    trimSuffix("_NORM");
    trimSuffix("_TRAN");
    trimSuffix("_ALPHA");
    trimSuffix("_MASK");
    if (!b.isEmpty()) texBases << b;
  }
}


      const QString info = QString("%1 bytes").arg(static_cast<qulonglong>(e.compressed_size));

      auto* child = new QTreeWidgetItem(QStringList() << displayName << type << info);
      applyItemIcon(child, type);

      // Store absolute path for selection/viewer routing
      // NOTE: for AST entries inside AST, `path` remains the parent AST file on disk.
      child->setData(0, Qt::UserRole, path);

      // For embedded entries we store baseOffset/maxReadable so we can parse children.
        // NOTE: For embedded indexes, entry offsets are relative to the embedded base.
        // Store absolute offsets into the parent AST file so we can expand nested ASTs.
        const std::uint64_t absOffset = baseOffset + e.data_offset;
        child->setData(0, Qt::UserRole + 1, static_cast<qulonglong>(absOffset));
      child->setData(0, Qt::UserRole + 2, static_cast<qulonglong>(e.compressed_size));
      child->setData(0, Qt::UserRole + 3, true);
      child->setData(0, Qt::UserRole + 4, static_cast<qulonglong>(e.flags));
      child->setData(0, Qt::UserRole + 6, static_cast<qulonglong>(e.index));

      QString extraTip;
      if (type == "AST") {
        const QString cachedKind = !embeddedMetaKey.isEmpty() ? m_nameCache.lookupKind(embeddedMetaKey) : QString();
        if (!cachedKind.isEmpty()) extraTip = QString("Derived Kind: %1").arg(cachedKind);
      }
      const QString childTip = buildTreeItemTooltip(displayName,
                                                    type,
                                                    info,
                                                    path,
                                                    true,
                                                    static_cast<quint64>(absOffset),
                                                    static_cast<quint64>(e.compressed_size),
                                                    extraTip);
      child->setToolTip(0, childTip);
      child->setToolTip(1, childTip);
      child->setToolTip(2, childTip);

      // Only embedded AST entries should be expandable.
      if (type == "AST") {
        // Add a placeholder so the expander arrow shows up.
        child->addChild(new QTreeWidgetItem(QStringList() << "(expand to parse)" << "" << ""));
      }

      QTreeWidgetItem* dest = item;
if (isQklRoot) {
  if ((type == "DDS" || type == "P3R" || type == "XPR" || type == "XPR2") && grpTextures) dest = grpTextures;
  else if ((type == "APT" || type == "APT1" || type == "CONST" || type == "APTDATA") && grpUI) {
    const QString grp = deriveUiGroupFromExportBase(displayName);
    dest = ensureGroup(grpUI, grp);
  }
  else if ((type == "RSF" || type == "RSG" || type == "STRM") && grpModels) dest = grpModels;
  else if (type == "AST" && grpASTs) {
    // Embedded AST containers can be bucketed by cached metadata (computed once by the
    // background scanner below).
    const QString ck = cacheKeyForEmbedded(absOffset, static_cast<std::uint64_t>(e.compressed_size));
    const QString kind = m_nameCache.lookupKind(ck);
    if (kind == "UI" && grpUI) {
      const QString grp = deriveUiGroupFromExportBase(displayName);
      dest = ensureGroup(grpUI, grp);
    }
    else if (kind == "Textures" && grpTextures) dest = grpTextures;
    else if (kind == "Models" && grpModels) dest = grpModels;
    else dest = grpASTs;
  }
  else if ((type == "XML" || type == "TXT" || type == "TEXT" || type == "INI" || type == "CFG" || type == "CONF" || type == "JSON" || type == "YAML" || type == "YML" || type == "PY" || type == "PYC") && grpConfigs) dest = grpConfigs;
  else if (type == "DB" && grpDatabase) dest = grpDatabase;
  else if (grpUnknown) dest = grpUnknown;
}
      dest->addChild(child);

      // Friendly Naming v3.1: Pre-name embedded AST containers WITHOUT requiring the user to
      // expand each nested AST. We do a lightweight parse of the embedded AST index (directory)
      // in the background and rename the child AST node to <RSF base>.ast (or texture base).
      // This keeps browsing fast and matches the "find by RSF" workflow.
      if (type == "AST") {
        const QString originalLabel = child->text(0);
        const QString originalBase = originalLabel.section('(', 0, 0).trimmed();
        const bool looksGeneric = originalBase.startsWith("File_", Qt::CaseInsensitive);

        const QString ck = cacheKeyForEmbedded(static_cast<quint64>(absOffset), static_cast<std::uint64_t>(e.compressed_size));
        const QString cachedKind = m_nameCache.lookupKind(ck);
        const QString cachedEmbeddedBase = m_nameCache.lookup(ck);

        auto deriveEmbeddedAstMeta = [](const QString& parentPath,
                                        quint64 childAbsOffset,
                                        quint64 childMaxReadable) -> std::tuple<QString, QString, bool> {
          try {
            auto pickMostFrequent = [](const QStringList& values) -> QString {
              if (values.isEmpty()) return {};
              QHash<QString,int> freq;
              QString best;
              int bestN = -1;
              for (const auto& v : values) freq[v] += 1;
              for (auto it = freq.begin(); it != freq.end(); ++it) {
                if (it.value() > bestN) { bestN = it.value(); best = it.key(); }
              }
              return best;
            };

            auto trimTextureBase = [](QString b) -> QString {
              const int dot = b.lastIndexOf('.');
              if (dot > 0) b = b.left(dot);
              auto trimSuffix = [&](const QString& sfx) {
                if (b.endsWith(sfx, Qt::CaseInsensitive)) b.chop(sfx.size());
              };
              trimSuffix("_COL");
              trimSuffix("_VECTOR");
              trimSuffix("_NORM");
              trimSuffix("_TRAN");
              trimSuffix("_ALPHA");
              trimSuffix("_MASK");
              return b;
            };

            std::function<void(const gf::core::AstArchive::Index&, quint64, int, QStringList&, QStringList&, QStringList&, bool&)> collectFromIndex;
            collectFromIndex = [&](const gf::core::AstArchive::Index& idx, quint64 baseAbs, int depth, QStringList& rsfBasesLocal, QStringList& texBasesLocal, QStringList& aptBasesLocal, bool& hasAptLocal) {
              if (depth > 1) return;
              for (const auto& ee : idx.entries) {
                const QString t = ee.type_hint.empty() ? QString() : QString::fromStdString(ee.type_hint).toUpper();
                const QString n = !ee.display_name.empty() ? QString::fromStdString(ee.display_name)
                                  : (!ee.name.empty() ? QString::fromStdString(ee.name) : QString());

                if (t == "APT" || t == "CONST" || n.endsWith(".apt", Qt::CaseInsensitive) || n.endsWith(".const", Qt::CaseInsensitive)) {
                  hasAptLocal = true;
                  QString b = n;
                  if (b.endsWith(".apt", Qt::CaseInsensitive)) b.chop(4);
                  if (b.endsWith(".const", Qt::CaseInsensitive)) b.chop(6);
                  if (!b.isEmpty() && !b.startsWith("File_", Qt::CaseInsensitive)) aptBasesLocal << b;
                }

                if (t == "RSF" || n.endsWith(".rsf", Qt::CaseInsensitive)) {
                  QString b = n;
                  if (b.endsWith(".rsf", Qt::CaseInsensitive)) b.chop(4);
                  if (!b.isEmpty() && !b.startsWith("File_", Qt::CaseInsensitive)) rsfBasesLocal << b;
                  continue;
                }

                if (t == "P3R" || t == "DDS" || t == "XPR" || t == "XPR2" ||
                    n.endsWith(".p3r", Qt::CaseInsensitive) || n.endsWith(".dds", Qt::CaseInsensitive)) {
                  QString b = trimTextureBase(n);
                  if (!b.isEmpty() && !b.startsWith("File_", Qt::CaseInsensitive)) texBasesLocal << b;
                  continue;
                }

                if (depth < 1 && (t == "AST" || n.endsWith(".ast", Qt::CaseInsensitive) || n.endsWith(".bgfa", Qt::CaseInsensitive))) {
                  try {
                    std::string nestedErr;
                    auto nestedOpt = gf::core::AstArchive::readEmbeddedIndexFromFileSmart(
                        std::filesystem::path(parentPath.toStdString()),
                        static_cast<std::uint64_t>(baseAbs + ee.data_offset),
                        static_cast<std::uint64_t>(ee.compressed_size),
                        &nestedErr);
                    if (nestedOpt) collectFromIndex(*nestedOpt, baseAbs + ee.data_offset, depth + 1, rsfBasesLocal, texBasesLocal, aptBasesLocal, hasAptLocal);
                  } catch (...) {}
                }
              }
            };

            std::string err;
            auto idxOpt = gf::core::AstArchive::readEmbeddedIndexFromFileSmart(
                std::filesystem::path(parentPath.toStdString()),
                static_cast<std::uint64_t>(childAbsOffset),
                static_cast<std::uint64_t>(childMaxReadable),
                &err);
            if (!idxOpt) return {QString(), QString(), false};

            QStringList rsfBasesLocal;
            QStringList texBasesLocal;
            QStringList aptBasesLocal;
            bool hasAptLocal = false;
            const QString kindLocal = classifyEmbeddedIndexKind(*idxOpt);
            collectFromIndex(*idxOpt, childAbsOffset, 0, rsfBasesLocal, texBasesLocal, aptBasesLocal, hasAptLocal);

            QString chosenLocal = pickMostFrequent(rsfBasesLocal);
            if (chosenLocal.isEmpty()) chosenLocal = pickMostFrequent(aptBasesLocal);
            if (chosenLocal.isEmpty()) chosenLocal = pickMostFrequent(texBasesLocal);
            return {chosenLocal, kindLocal, hasAptLocal};
          } catch (...) {
            return {QString(), QString(), false};
          }
        };

        // For qkl_* roots, derive child AST names during the initial parse so they show up
        // immediately, not only after the user expands the nested AST later.
        if (isQklRoot && (cachedKind.isEmpty() || cachedEmbeddedBase.isEmpty())) {
          const auto derived = deriveEmbeddedAstMeta(path,
                                                     static_cast<quint64>(absOffset),
                                                     static_cast<quint64>(e.compressed_size));
          const QString chosenNow = std::get<0>(derived);
          const QString kindNow = std::get<1>(derived);
          const bool hasAptNow = std::get<2>(derived);
          if (!chosenNow.isEmpty() || !kindNow.isEmpty() || hasAptNow) {
            const QString safeKind = kindNow.isEmpty() ? (hasAptNow ? QStringLiteral("UI") : QString()) : kindNow;
            m_nameCache.putMeta(ck, chosenNow, safeKind, hasAptNow);
            if (!chosenNow.isEmpty()) {
              child->setText(0, QString("%1.ast (%2)").arg(chosenNow, originalBase));
              const QString renamedTip = buildTreeItemTooltip(child->text(0), child->text(1), child->text(2), path, true,
                                                              static_cast<quint64>(absOffset),
                                                              static_cast<quint64>(e.compressed_size),
                                                              safeKind.isEmpty() ? QString() : QString("Derived Kind: %1").arg(safeKind));
              child->setToolTip(0, renamedTip);
              child->setToolTip(1, renamedTip);
              child->setToolTip(2, renamedTip);
            }
          }
        }

        // If we don't have cached metadata, scan in the background.
        // We still prefer to only rename generic labels, but we ALWAYS cache kind/hasApt so we can bucket correctly.
        if (m_nameCache.lookupKind(ck).isEmpty() || m_nameCache.lookup(ck).isEmpty()) {
          const quint64 token = m_treeToken;
          const QString parentPath = path;
          const quint64 childAbsOffset = static_cast<quint64>(absOffset);
          const quint64 childMaxReadable = static_cast<quint64>(e.compressed_size);

          auto* nameWatcher = new QFutureWatcher<std::tuple<QString, QString, bool>>(this);
          const QString cacheKey = ck;
          const QString childTreeKey = makeTreeCacheKey(child);

          connect(nameWatcher, &QFutureWatcherBase::finished, this, [this, nameWatcher, token, originalBase, cacheKey, childTreeKey]() {
            const auto res = nameWatcher->result();
            const QString chosen = std::get<0>(res);
            const QString kind = std::get<1>(res);
            const bool hasApt = std::get<2>(res);
            nameWatcher->deleteLater();
            if (chosen.isEmpty() && kind.isEmpty() && !hasApt) return;
            if (token != m_treeToken) return; // stale
            if (!m_tree) return;
            QTreeWidgetItem* child = findTreeItemByCacheKey(m_tree, childTreeKey);
            if (!child) return;

            // Persist metadata (name + kind) so next expansion can bucket without re-scanning.
            if (!cacheKey.isEmpty() && (!chosen.isEmpty() || !kind.isEmpty() || hasApt)) {
              const QString safeKind = kind.isEmpty() ? (hasApt ? QStringLiteral("UI") : QString()) : kind;
              if (!chosen.isEmpty()) m_nameCache.putMeta(cacheKey, chosen, safeKind, hasApt);
              else {
                // keep existing name if any, but store kind/hasApt
                const QString existingName = m_nameCache.lookup(cacheKey);
                m_nameCache.putMeta(cacheKey, existingName, safeKind, hasApt);
              }
            }
            // Rename if a better name was found.
            if (!chosen.isEmpty()) child->setText(0, QString("%1.ast (%2)").arg(chosen, originalBase));

            // Move buckets if we learned more.
            const QString decidedKind = !kind.isEmpty() ? kind : (hasApt ? QStringLiteral("UI") : QString());
            const QString hoverTip = buildTreeItemTooltip(child->text(0), child->text(1), child->text(2),
                                                          child->data(0, Qt::UserRole).toString(), true,
                                                          child->data(0, Qt::UserRole + 1).toULongLong(),
                                                          child->data(0, Qt::UserRole + 2).toULongLong(),
                                                          decidedKind.isEmpty() ? QString() : QString("Derived Kind: %1").arg(decidedKind));
            child->setToolTip(0, hoverTip);
            child->setToolTip(1, hoverTip);
            child->setToolTip(2, hoverTip);
            if (!decidedKind.isEmpty()) {
              QTreeWidgetItem* p = child->parent();
              if (p) {
                QTreeWidgetItem* root = p->parent();
                if (root) {
                  // Remove from current parent
                  const int idx = p->indexOfChild(child);
                  if (idx >= 0) p->takeChild(idx);

                  // Find target group (or fall back to current)
                  QTreeWidgetItem* target = nullptr;
                  for (int i=0;i<root->childCount();++i) {
                    if (QString::compare(root->child(i)->text(0), decidedKind, Qt::CaseInsensitive) == 0) { target = root->child(i); break; }
                  }
                  if (target) target->addChild(child);
                  else p->addChild(child);
                }
              }
            }
          });

          auto futName = QtConcurrent::run([parentPath, childAbsOffset, childMaxReadable, deriveEmbeddedAstMeta]() -> std::tuple<QString, QString, bool> {
            return deriveEmbeddedAstMeta(parentPath, childAbsOffset, childMaxReadable);
          });

          nameWatcher->setFuture(futName);
        }
      }
    }
// After materializing children, if this is an embedded AST and it still has a generic name,
// rename it to a strong friendly container name:
//   1) <RSF base>.ast
//   2) <texture base>.ast
if (isEmbedded) {
  const QString originalLabel = item->text(0);
  const QString originalBase = originalLabel.section('(', 0, 0).trimmed();
  const bool looksGeneric = originalBase.startsWith("File_", Qt::CaseInsensitive);

  if (looksGeneric) {
    QString chosen;

    // Choose the most frequent RSF base if present
    if (!rsfBases.isEmpty()) {
      QHash<QString,int> freq;
      for (const auto& b : rsfBases) freq[b] += 1;
      int bestN = -1;
      for (auto it = freq.begin(); it != freq.end(); ++it) {
        if (it.value() > bestN) { bestN = it.value(); chosen = it.key(); }
      }
    }

    // Otherwise, choose a texture-derived base (try longest common prefix-ish)
    if (chosen.isEmpty() && !texBases.isEmpty()) {
      // Prefer the shortest base (usually the shared prefix), but keep it readable.
      QString best = texBases.first();
      for (const auto& b : texBases) {
        if (b.size() < best.size()) best = b;
      }
      chosen = best;
    }

    if (!chosen.isEmpty() && !isQklStyleFriendlyBase(chosen)) {
      const QString newName = chosen + ".ast";
      // Preserve the original File_XXXXX label in parentheses for stability.
      item->setText(0, QString("%1 (%2)").arg(newName, originalBase));
      // Persist container name for next run (so the AST name appears without expanding).
      // For embedded AST containers, the stable metadata key is absolute embedded offset + readable size.
      const qulonglong embeddedAbsOffsetForKey = item->data(0, Qt::UserRole + 1).toULongLong();
      const qulonglong embeddedSizeForKey = item->data(0, Qt::UserRole + 2).toULongLong();
      if (embeddedAbsOffsetForKey && embeddedSizeForKey) {
        const QString ck = cacheKeyForEmbedded(static_cast<quint64>(embeddedAbsOffsetForKey),
                                               static_cast<std::uint64_t>(embeddedSizeForKey));
        const QString kind = m_nameCache.lookupKind(ck);
        const bool hasApt = m_nameCache.lookupHasApt(ck);
        m_nameCache.putMeta(ck, chosen, kind, hasApt);
      }

    }
  }
}



  });

  auto fut = QtConcurrent::run([path, isEmbedded, baseOffset, maxReadable]() -> ParseResult {
    ParseResult r;
    try {
      std::string err;
      if (!isEmbedded) {
        gf::core::logBreadcrumb(gf::core::LogCategory::AstParsing,
                                "readIndex: " + path.toStdString());
        r.idx = gf::core::AstArchive::readIndex(std::filesystem::path(path.toStdString()), &err);
        r.message = QString::fromStdString(err);
        if (!r.idx && !err.empty())
          gf::core::logError(gf::core::LogCategory::AstParsing,
                             "readIndex failed", err);
        return r;
      }

      // Embedded AST inside parent AST file.
      gf::core::logBreadcrumb(gf::core::LogCategory::AstParsing,
                              std::string("readEmbedded: ") + path.toStdString()
                              + " @offset=" + std::to_string(baseOffset));
      r.idx = gf::core::AstArchive::readEmbeddedIndexFromFileSmart(std::filesystem::path(path.toStdString()),
                                                                   static_cast<std::uint64_t>(baseOffset),
                                                                   static_cast<std::uint64_t>(maxReadable),
                                                                   &err);
      r.message = QString::fromStdString(err);
      if (!r.idx && !err.empty())
        gf::core::logError(gf::core::LogCategory::AstParsing,
                           "readEmbedded failed", err);
      return r;
    } catch (const std::exception& ex) {
      r.idx = std::nullopt;
      r.message = QString("Exception during AST parse: %1").arg(ex.what());
      gf::core::logError(gf::core::LogCategory::AstParsing,
                         "Exception during AST parse", ex.what());
      return r;
    } catch (...) {
      r.idx = std::nullopt;
      r.message = QString("Unknown exception during AST parse.");
      gf::core::logError(gf::core::LogCategory::AstParsing,
                         "Unknown exception during AST parse", path.toStdString());
      return r;
    }
  });
  watcher->setFuture(fut);
}


void MainWindow::onItemDoubleClicked(QTreeWidgetItem* item, int /*column*/) {
  if (!item) return;

  const QString path = item->data(0, Qt::UserRole).toString();
  if (path.isEmpty()) {
    // Folder/group node: expand/collapse.
    if (item->childCount() > 0) item->setExpanded(!item->isExpanded());
    return;
  }

  const QString type = item->text(1).trimmed().toUpper();
  const bool isEmbedded = item->data(0, Qt::UserRole + 3).toBool();
  const quint64 baseOffset = item->data(0, Qt::UserRole + 1).toULongLong();
  const quint64 maxReadable = item->data(0, Qt::UserRole + 2).toULongLong();

  auto isTextLikeType = [&](const QString& t) -> bool {
    return (t == "XML" || t == "TXT" || t == "TEXT" || t == "INI" || t == "CFG" || t == "CONF" ||
            t == "JSON" || t == "YAML" || t == "YML" || t == "LUA" || t == "JS" || t == "CSS" ||
            t == "HTML" || t == "HTM");
  };

  // Double-click text/config: jump to the integrated Text tab (no extra window).
  if (isTextLikeType(type)) {
    if (m_viewTabs) m_viewTabs->setCurrentIndex(1);
    showViewerForItem(item);
    if (m_textView) m_textView->setFocus();
    return;
  }

  // Keep the integrated viewer as default; only spawn a new MainWindow in Developer Mode.
  if (!devModeEnabled()) {
    if (item->childCount() > 0) item->setExpanded(!item->isExpanded());
    return;
  }

  auto* mw = new MainWindow();
  mw->setAttribute(Qt::WA_DeleteOnClose);
  mw->openStandaloneAst(path);
  mw->show();
}

void MainWindow::onTreeContextMenu(const QPoint& pos) {
  if (!m_tree) return;
  auto* item = m_tree->itemAt(pos);
  if (!item) return;

  const QString path = item->data(0, Qt::UserRole).toString();
  const bool hasPath = !path.isEmpty();
  const bool isEmbedded = item->data(0, Qt::UserRole + 3).toBool();
  const bool hasEntryIndex = item->data(0, Qt::UserRole + 6).isValid();
  const bool isExtractableEntry = hasPath && (isEmbedded || hasEntryIndex);
  const quint32 astFlags = static_cast<quint32>(item->data(0, Qt::UserRole + 4).toULongLong());
  const QString type = item->text(1).trimmed();
  const QString typeUpper = type.trimmed().toUpper();

  auto isTextLikeType = [&](const QString& t) -> bool {
    return (t == "XML" || t == "TXT" || t == "TEXT" || t == "INI" || t == "CFG" || t == "CONF" ||
            t == "JSON" || t == "YAML" || t == "YML" || t == "LUA" || t == "JS" || t == "CSS" ||
            t == "HTML" || t == "HTM");
  };

  auto isTextureLikeType = [&](const QString& t) -> bool {
    // ZLIB entries in PS3 ASTs are typically zlib-compressed P3R textures.
    // Including ZLIB here gives them the "Replace Texture…" menu item so the
    // TGA pipeline can inflate → detect P3R → rebuild → re-compress correctly.
    return (t == "DDS" || t == "P3R" || t == "P3R2" || t == "P3R3" || t == "P3R4" ||
            t == "XPR2" || t == "XPR" || t == "TGA" || t == "PNG" || t == "ZLIB");
  };

  QList<QTreeWidgetItem*> selectedItems = m_tree->selectedItems();
  if (!selectedItems.contains(item)) {
    selectedItems.clear();
    selectedItems.push_back(item);
  }

  QList<QTreeWidgetItem*> selectedExtractable;
  for (auto* sel : selectedItems) {
    if (!sel) continue;
    const QString selPath = sel->data(0, Qt::UserRole).toString();
    if (selPath.isEmpty()) continue;
    const bool selEmbedded = sel->data(0, Qt::UserRole + 3).toBool();
    const bool selHasEntry = sel->data(0, Qt::UserRole + 6).isValid();
    if (selEmbedded || selHasEntry) selectedExtractable.push_back(sel);
  }

  QList<QTreeWidgetItem*> folderExtractable;
  auto collectExtractableDescendants = [&](auto&& self, QTreeWidgetItem* node) -> void {
    if (!node) return;
    for (int i = 0; i < node->childCount(); ++i) {
      QTreeWidgetItem* child = node->child(i);
      if (!child) continue;
      const QString childPath = child->data(0, Qt::UserRole).toString();
      const bool childEmbedded = child->data(0, Qt::UserRole + 3).toBool();
      const bool childHasEntry = child->data(0, Qt::UserRole + 6).isValid();
      if (!childPath.isEmpty() && (childEmbedded || childHasEntry)) {
        folderExtractable.push_back(child);
      }
      self(self, child);
    }
  };
  const bool isFolderNode = !hasPath && item->childCount() > 0;
  if (isFolderNode) {
    collectExtractableDescendants(collectExtractableDescendants, item);
  }

  QMenu menu(this);

  QAction* expandAct = nullptr;
  QAction* collapseAct = nullptr;
  QAction* refreshAct = nullptr;
  if (item->childCount() > 0 || !hasPath) {
    expandAct = menu.addAction("Expand All");
    collapseAct = menu.addAction("Collapse All");
    refreshAct = menu.addAction("Refresh Archive View");
    menu.addSeparator();
  }

  QAction* openAct = nullptr;
  if (devModeEnabled() && hasPath) openAct = menu.addAction("Open in New Window");

  QAction* exportAct = nullptr;
  QAction* extractSelectedAct = nullptr;
  QAction* extractFolderAct = nullptr;
  QAction* extractAllAct = nullptr;
  if (hasPath) {
    exportAct = menu.addAction("Extract...");
    if (selectedExtractable.size() > 1) {
      extractSelectedAct = menu.addAction(QString("Extract Selected (%1)").arg(selectedExtractable.size()));
    }
    if (isExtractableEntry) {
      extractAllAct = menu.addAction("Extract All From Container...");
    }
  } else if (!folderExtractable.isEmpty()) {
    extractFolderAct = menu.addAction(QString("Extract Folder (%1)").arg(folderExtractable.size()));
  }

  QAction* copyNameAct = nullptr;
  QAction* copyPathAct = nullptr;
  if (hasPath) {
    menu.addSeparator();
    copyNameAct = menu.addAction("Copy Name");
    copyPathAct = menu.addAction("Copy Path");
  }

  QAction* textureReplaceAct = nullptr;
  QAction* replaceAct = nullptr;
  QAction* rebuildContainerToAct = nullptr;
  if (isExtractableEntry) {
    menu.addSeparator();
    if (isTextureLikeType(typeUpper)) {
      textureReplaceAct = menu.addAction("Replace Texture…");
    }
    replaceAct = menu.addAction("Import/Replace File…");
    rebuildContainerToAct = menu.addAction("Rebuild Container To…");

    if (textureReplaceAct && !editingEnabled()) {
      textureReplaceAct->setEnabled(false);
      textureReplaceAct->setToolTip("Editing is unavailable.");
    }
    if (replaceAct && !editingEnabled()) {
      replaceAct->setEnabled(false);
      replaceAct->setToolTip("Editing is unavailable.");
    }
    if (rebuildContainerToAct && !editingEnabled()) {
      rebuildContainerToAct->setEnabled(false);
      rebuildContainerToAct->setToolTip("Editing is unavailable.");
    }
  }

  QAction* rawViewAct = nullptr;
  if (isTextLikeType(typeUpper)) {
    menu.addSeparator();
    rawViewAct = menu.addAction("Open as Raw Viewer");
  }

  QAction* renameFriendlyAct = nullptr;
  if (isExtractableEntry && typeUpper == "AST") {
    menu.addSeparator();
    renameFriendlyAct = menu.addAction("Rename Friendly Name…");
  }

  auto chooseOutputDir = [&]() -> QString {
    QSettings s(kSettingsOrg, kSettingsApp);
    const QString key = QStringLiteral("ui/last_extract_dir");
    const QString startDir = s.value(key, QFileInfo(path).absolutePath()).toString();
    const QString outDir = QFileDialog::getExistingDirectory(this, "Choose Output Folder", startDir);
    if (!outDir.isEmpty()) s.setValue(key, outDir);
    return outDir;
  };

  auto exportEntry = [&](QTreeWidgetItem* sourceItem,
                         gf::core::AstContainerEditor& editor,
                         const QString& outDir,
                         QStringList* failures = nullptr) -> bool {
    if (!sourceItem) return false;
    const QString entryTypeUpper = sourceItem->text(1).trimmed().toUpper();
    const std::uint32_t entryIndex = static_cast<std::uint32_t>(sourceItem->data(0, Qt::UserRole + 6).toULongLong());
    const std::uint32_t astFlags = (entryIndex < editor.entries().size()) ? editor.entries()[entryIndex].flags : 0u;

    auto resolved = resolveTexturePayloadForEditor(sourceItem, entryTypeUpper, editor, entryIndex);
    if (resolved.bytes.empty()) {
      const QString why = QString("could not resolve current-entry texture payload\n%1")
                              .arg(summarizeResolvedTexturePayload(resolved));
      if (failures) failures->push_back(QString("%1 — %2")
                                        .arg(sourceItem->text(0).trimmed(), why));
      return false;
    }

    QByteArray outBytes(reinterpret_cast<const char*>(resolved.bytes.data()), int(resolved.bytes.size()));
    QString exportTypeUpper = entryTypeUpper;

    if (entryTypeUpper == "DDS" || entryTypeUpper.startsWith("P3R") || entryTypeUpper == "XPR2" || entryTypeUpper == "XPR") {
      QString exportDetails;
      auto ddsBytes = buildDdsForTextureExport(entryTypeUpper, resolved, astFlags, &exportDetails);
      if (!ddsBytes.has_value()) {
        if (failures) failures->push_back(QString("%1 — %2")
                                          .arg(sourceItem->text(0).trimmed(), exportDetails));
        return false;
      }
      outBytes = QByteArray(reinterpret_cast<const char*>(ddsBytes->data()), int(ddsBytes->size()));
      exportTypeUpper = "DDS";
    }

    if (exportTypeUpper == "DDS") {
      QString ddsDetails;
      if (!validateDdsForWrite(this,
                               "Extract Failed",
                               sourceItem->text(0).trimmed(),
                               std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(outBytes.constData()), static_cast<std::size_t>(outBytes.size())),
                               &ddsDetails)) {
        if (failures) failures->push_back(QString("%1 — %2").arg(sourceItem->text(0).trimmed(), ddsDetails));
        return false;
      }
    }

    const QString ext = defaultExtForTypeUpper(exportTypeUpper);
    const QString outPath = uniqueOutputPath(outDir,
                                             visibleBaseNameForItem(sourceItem, entryIndex),
                                             ext);
    const auto* bptr = reinterpret_cast<const std::byte*>(outBytes.constData());
    gf::core::SafeWriteOptions opt;
    opt.make_backup = false;
    const auto r = gf::core::safe_write_bytes(outPath.toStdString(),
                                              std::span<const std::byte>(bptr, static_cast<std::size_t>(outBytes.size())),
                                              opt);
    if (!r.ok) {
      if (failures) failures->push_back(QString("%1 — %2")
                                        .arg(sourceItem->text(0).trimmed(),
                                             QString::fromStdString(r.message)));
      return false;
    }
    return true;
  };

  auto doEmbeddedReplace = [&](bool validateTextureReplacement) {
    if (!editingEnabled()) {
      showInfoDialog(validateTextureReplacement ? "Replace Texture" : "Replace",
                     "Editing is currently OFF.\n\nEditing is unavailable.");
      return;
    }

    const qulonglong entryIndexQ = item->data(0, Qt::UserRole + 6).toULongLong();
    const std::uint32_t entryIndex = static_cast<std::uint32_t>(entryIndexQ);

    const QString containerPath = item->data(0, Qt::UserRole).toString();
    if (containerPath.isEmpty()) return;

    QString filter = "All Files (*)";
    if (typeUpper == "AST") filter = "AST files (*.ast);;All Files (*)";
    else if (validateTextureReplacement &&
             (typeUpper.startsWith("P3R") || typeUpper == "DDS" ||
              typeUpper == "XPR2" || typeUpper == "XPR" || typeUpper == "ZLIB"))
      // TGA-first replacement dialog: TGA is the recommended format, DDS is advanced.
      // ZLIB entries on PS3 are typically zlib-compressed P3R — same pipeline applies.
      filter = "TGA — recommended (*.tga);;DDS — advanced (*.dds);;All Files (*)";
    else if (typeUpper.startsWith("P3R")) filter = "DDS/P3R files (*.dds *.p3r);;All Files (*)";
    else if (typeUpper == "DDS") filter = "DDS files (*.dds);;All Files (*)";
    else if (typeUpper == "XPR2" || typeUpper == "XPR") filter = "DDS files (*.dds);;All Files (*)";
    else if (typeUpper == "PNG") filter = "PNG files (*.png);;All Files (*)";
    else if (typeUpper == "TGA") filter = "TGA files (*.tga);;All Files (*)";
    else if (isTextLikeType(typeUpper)) filter = "Text files (*.xml *.cfg *.txt *.ini *.json *.csv);;All Files (*)";

    const QString inPath = QFileDialog::getOpenFileName(this,
                                                        validateTextureReplacement ? "Replace Texture" : "Import/Replace File",
                                                        QDir::homePath(),
                                                        filter);
    if (inPath.isEmpty()) return;

    QFile rf(inPath);
    if (!rf.open(QIODevice::ReadOnly)) {
      showErrorDialog(validateTextureReplacement ? "Replace Texture Failed" : "Replace Failed",
                      "Failed to open replacement file.", QString(), false);
      return;
    }
    QByteArray newBytes = rf.readAll();
    rf.close();

    std::string err;

    // Walk ancestors: detect if this item lives inside a nested embedded AST.
    QTreeWidgetItem* astAncestor = nullptr;
    for (QTreeWidgetItem* p = item->parent(); p; p = p->parent()) {
      if (p->text(1).compare("AST", Qt::CaseInsensitive) == 0 &&
          p->data(0, Qt::UserRole + 6).isValid()) {
        astAncestor = p;
        break;
      }
    }
    const bool isNestedEntry = (astAncestor != nullptr);
    const std::uint32_t nestedOuterIdx = isNestedEntry
        ? static_cast<std::uint32_t>(astAncestor->data(0, Qt::UserRole + 6).toULongLong())
        : 0;

    // Load outer container, reusing m_liveAstEditor if the path matches to preserve
    // prior in-memory changes (e.g. a previous replace in the same session).
    std::optional<gf::core::AstContainerEditor> outerEdOpt;
    if (m_liveAstEditor && m_liveAstPath == containerPath) {
      outerEdOpt = *m_liveAstEditor;
    } else {
      outerEdOpt = gf::core::AstContainerEditor::load(containerPath.toStdString(), &err);
      if (!outerEdOpt.has_value()) {
        showErrorDialog("Load Failed", "The AST container could not be loaded.",
                        QString::fromStdString(err), false);
        return;
      }
    }

    // For nested entries: extract the embedded sub-AST and use it as the working editor.
    std::optional<gf::core::AstContainerEditor> innerEdOpt;
    if (isNestedEntry) {
      std::string exErr;
      auto nestedBytes = outerEdOpt->getEntryInflatedBytes(nestedOuterIdx, &exErr);
      if (!nestedBytes) {
        showErrorDialog("Extract Failed",
                        QString("Cannot extract nested AST (entry #%1).").arg(nestedOuterIdx),
                        QString::fromStdString(exErr), false);
        return;
      }
      const QString tmpRoot = QDir::tempPath() + "/ASTra/nestedast";
      QDir().mkpath(tmpRoot);
      const QString nestedAstPath =
          tmpRoot + QString("/nested_replace_%1.ast").arg(nestedOuterIdx);
      QFile nf(nestedAstPath);
      if (!nf.open(QIODevice::WriteOnly)) {
        showErrorDialog("Temp Write Failed",
                        QString("Cannot write nested AST to temp: %1").arg(nestedAstPath),
                        QString(), false);
        return;
      }
      nf.write(reinterpret_cast<const char*>(nestedBytes->data()),
               static_cast<qint64>(nestedBytes->size()));
      nf.close();
      std::string loadErr;
      innerEdOpt = gf::core::AstContainerEditor::load(nestedAstPath.toStdString(), &loadErr);
      if (!innerEdOpt.has_value()) {
        showErrorDialog("Load Failed", "Cannot parse nested AST.",
                        QString::fromStdString(loadErr), false);
        return;
      }
    }
    // 'ed' is the working editor that owns entryIndex.
    auto& ed = isNestedEntry ? innerEdOpt : outerEdOpt;

    // ── TGA-first texture replacement pipeline ───────────────────────────────
    // Determine import format from the file extension.
    // TGA  → full pipeline: decode pixels → generate mips → compress BC → rebuild container.
    // DDS  → advanced override: validate contract strictly, then rebuild container.
    // Non-texture replace (validateTextureReplacement==false) → passthrough as before.
    QString validationDetails;

    if (validateTextureReplacement) {
      const QString importExt = QFileInfo(inPath).suffix().toLower();
      const bool importIsTga = (importExt == "tga");
      const bool importIsDds = (importExt == "dds");

      if (!importIsTga && !importIsDds) {
        showErrorDialog("Replace Texture Failed",
                        "Unsupported import format.",
                        QString("Only TGA (recommended) and DDS (advanced) are accepted for texture replacement.\n"
                                "Selected file: %1").arg(QFileInfo(inPath).fileName()),
                        false);
        return;
      }

      // Resolve the original container payload (needed by the pipeline and XPR2 patch).
      auto resolvedBase = resolveTexturePayloadForEditor(item, typeUpper, *ed, entryIndex);
      gf::core::logBreadcrumb(gf::core::LogCategory::TextureReplace,
                              std::string("pipeline start: entry='") + item->text(0).toStdString()
                              + "' type=" + typeUpper.toStdString()
                              + " importFmt=" + (importIsTga ? "TGA" : "DDS")
                              + " src=" + resolvedBase.source.toStdString());
      { auto lg = gf::core::Log::get(); if (lg) lg->info(
            "[Texture replace] payload raw={} resolved={} sig=[{}]",
            static_cast<unsigned long long>(resolvedBase.rawSize),
            static_cast<unsigned long long>(resolvedBase.bytes.size()),
            hexSignaturePrefix(std::span<const std::uint8_t>(resolvedBase.bytes.data(), resolvedBase.bytes.size())).toStdString()); }

      if (importIsDds) {
        // Show an advisory warning so users understand DDS has stricter requirements.
        const auto warnAns = QMessageBox::warning(
            this, "DDS Import — Advanced Mode",
            "DDS import requires the file to exactly match the original texture's\n"
            "format, dimensions, and mip count.\n\n"
            "TGA is the recommended format for reliable replacement.\n\n"
            "Continue with DDS?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (warnAns != QMessageBox::Yes) return;
      }

      // Determine the original payload span.
      // For XPR2 we must use the raw XPR2 buffer (not the exported DDS).
      // For ZLIB entries the resolvedBase is already inflated by
      // resolveTexturePayloadForEditor, so the happy path is correct.
      // The fallback (resolvedBase.bytes.empty()) must inflate manually
      // because getEntryStoredBytes returns compressed bytes for ZLIB entries.
      std::vector<std::uint8_t> fallbackInflated;
      std::span<const std::uint8_t> origSpan;
      if (resolvedBase.bytes.empty()) {
        auto storedOpt = ed->getEntryStoredBytes(entryIndex);
        if (!storedOpt || storedOpt->empty()) {
          showErrorDialog("Replace Texture Failed",
                          "Cannot read the original texture entry.",
                          "Entry bytes are unavailable.", false);
          return;
        }
        // If this is a ZLIB entry, inflate before passing to the pipeline.
        if (typeUpper == "ZLIB" || looks_like_zlib_cmf_flg((*storedOpt)[0], storedOpt->size() >= 2 ? (*storedOpt)[1] : 0)) {
          try {
            fallbackInflated = zlib_inflate_unknown_size(
                std::span<const std::uint8_t>(storedOpt->data(), storedOpt->size()));
          } catch (...) {}
        }
        if (!fallbackInflated.empty()) {
          origSpan = std::span<const std::uint8_t>(fallbackInflated.data(), fallbackInflated.size());
        } else {
          origSpan = std::span<const std::uint8_t>(storedOpt->data(), storedOpt->size());
        }
      } else {
        origSpan = std::span<const std::uint8_t>(resolvedBase.bytes.data(), resolvedBase.bytes.size());
      }

      const auto importSpan = std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(newBytes.constData()),
          static_cast<std::size_t>(newBytes.size()));
      const auto importFmt = importIsTga
          ? gf::textures::TexImportFormat::TGA
          : gf::textures::TexImportFormat::DDS_Advanced;

      // Quick pre-flight validation (cheap — no BC compression).
      gf::core::logBreadcrumb(gf::core::LogCategory::TextureReplace, "preflight validation");
      const std::string preflightErr = gf::textures::validate_texture_import(
          origSpan, importSpan, importFmt, astFlags);
      if (!preflightErr.empty()) {
        gf::core::logError(gf::core::LogCategory::Validation,
                           "Texture preflight validation failed",
                           preflightErr + " | entry=" + item->text(0).toStdString());
        showErrorDialog("Replace Texture Failed",
                        "Import file is not compatible with the original texture.",
                        QString::fromStdString(preflightErr), false);
        return;
      }

      // Parse original info for the confirmation dialog summary.
      auto origInfo = gf::textures::parse_original_texture_info(origSpan, astFlags);
      validationDetails = QString("Original: %1x%2 | %3 | mips=%4\nImport: %5 (%6)")
          .arg(origInfo ? static_cast<int>(origInfo->width)  : 0)
          .arg(origInfo ? static_cast<int>(origInfo->height) : 0)
          .arg(origInfo ? QString("container=%1").arg(static_cast<int>(origInfo->container)) : QString("?"))
          .arg(origInfo ? static_cast<int>(origInfo->mipCount) : 0)
          .arg(QFileInfo(inPath).fileName())
          .arg(importIsTga ? "TGA → auto rebuild" : "DDS advanced");

      // Show confirmation dialog.
      const QString prompt =
          QString("This will MODIFY the AST container in memory until you Save.\n\nContainer:\n%1\n\n%2\n\nContinue?")
              .arg(QDir::toNativeSeparators(containerPath))
              .arg(validationDetails);
      if (QMessageBox::question(this, "ASTra Core", prompt,
                                QMessageBox::Yes | QMessageBox::No,
                                QMessageBox::No) != QMessageBox::Yes) return;

      // Run the full pipeline.
      gf::core::logBreadcrumb(gf::core::LogCategory::TextureReplace, "running replace_texture pipeline");
      std::string pipelineErr;
      auto replaceResult = gf::textures::replace_texture(
          origSpan, importSpan, importFmt, astFlags, &pipelineErr);

      if (!replaceResult || replaceResult->containerBytes.empty()) {
        gf::core::logError(gf::core::LogCategory::TextureReplace,
                           "Pipeline failed: replace_texture returned empty result",
                           pipelineErr + " | entry=" + item->text(0).toStdString());
        showErrorDialog("Replace Texture Failed",
                        "The texture replacement pipeline could not produce a valid container.",
                        QString::fromStdString(pipelineErr), false);
        return;
      }

      gf::core::logInfo(gf::core::LogCategory::TextureReplace,
                        "Pipeline succeeded",
                        replaceResult->summary + " | outSize="
                        + std::to_string(replaceResult->containerBytes.size())
                        + " | entry=" + item->text(0).toStdString());

      // Replace newBytes with the rebuilt container.
      newBytes = QByteArray(
          reinterpret_cast<const char*>(replaceResult->containerBytes.data()),
          static_cast<int>(replaceResult->containerBytes.size()));
      validationDetails = QString::fromStdString(replaceResult->summary);
    } // end validateTextureReplacement pipeline block

    QByteArray previewBytes = newBytes;

    QByteArray previousStoredBytes;
    if (const auto oldStoredForUndo = ed->getEntryStoredBytes(entryIndex); oldStoredForUndo.has_value()) {
      previousStoredBytes = QByteArray(reinterpret_cast<const char*>(oldStoredForUndo->data()),
                                       int(oldStoredForUndo->size()));
    }
    QByteArray previousPreviewBytes;
    const QVariant priorPreviewVar = item->data(0, Qt::UserRole + 31);
    if (priorPreviewVar.isValid()) previousPreviewBytes = priorPreviewVar.toByteArray();

    const auto spanBytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(newBytes.data()),
                                                         static_cast<std::size_t>(newBytes.size()));
    if (!ed->replaceEntryBytes(entryIndex, spanBytes,
                               gf::core::AstContainerEditor::ReplaceMode::PreserveZlibIfPresent, &err)) {
      gf::core::logError(gf::core::LogCategory::TextureReplace,
                         "replaceEntryBytes failed",
                         err + " | entry=" + item->text(0).toStdString());
      showErrorDialog(validateTextureReplacement ? "Replace Texture Failed" : "Replace Failed",
                      "The entry could not be replaced.", QString::fromStdString(err), true);
      return;
    }

    // For nested entries: rebuild the inner AST and repack it into the outer container.
    if (isNestedEntry) {
      std::string rebuildErr;
      auto newNestedBytes = innerEdOpt->rebuild(&rebuildErr);
      if (newNestedBytes.empty() && !rebuildErr.empty()) {
        gf::core::logError(gf::core::LogCategory::TextureReplace,
                           "Nested AST rebuild failed after replacement", rebuildErr);
        showErrorDialog("Rebuild Failed",
                        "Cannot rebuild nested AST after replacement.",
                        QString::fromStdString(rebuildErr), true);
        return;
      }
      const auto nestedSpan = std::span<const std::uint8_t>(
          newNestedBytes.data(), newNestedBytes.size());
      if (!outerEdOpt->replaceEntryBytes(nestedOuterIdx, nestedSpan,
                                         gf::core::AstContainerEditor::ReplaceMode::PreserveZlibIfPresent,
                                         &err)) {
        gf::core::logError(gf::core::LogCategory::TextureReplace,
                           "Repack nested AST into outer container failed", err);
        showErrorDialog("Repack Failed",
                        "Cannot repack nested AST into outer container.",
                        QString::fromStdString(err), true);
        return;
      }
    }

    item->setData(0, Qt::UserRole + 30, newBytes);
    item->setData(0, Qt::UserRole + 31, previewBytes);

    m_liveAstEditor = std::make_unique<gf::core::AstContainerEditor>(std::move(*outerEdOpt));
    m_liveAstPath = containerPath;
    m_lastReplaceUndo.valid = !previousStoredBytes.isEmpty();
    m_lastReplaceUndo.containerPath = containerPath;
    m_lastReplaceUndo.entryIndex = entryIndex;
    m_lastReplaceUndo.previousStoredBytes = previousStoredBytes;
    m_lastReplaceUndo.previousPreviewBytes = previousPreviewBytes;
    m_lastReplaceUndo.itemCacheKey = makeTreeCacheKey(item);
    m_lastReplaceUndo.displayName = item->text(0).trimmed();
    if (m_actUndoLastReplace) m_actUndoLastReplace->setEnabled(m_lastReplaceUndo.valid);
    gf::core::logInfo(gf::core::LogCategory::TextureReplace,
                      "Entry replace committed — pending save",
                      item->text(0).toStdString() + " | " + containerPath.toStdString());
    setDirty(true);

    if (validateTextureReplacement && m_viewTabs && m_imageView) {
      auto renderTexturePreviewNow = [this, astFlags, item](const QByteArray& bytes) -> bool {
        if (bytes.isEmpty()) return false;
        std::vector<std::uint8_t> payload(static_cast<std::size_t>(bytes.size()));
        std::memcpy(payload.data(), bytes.constData(), static_cast<std::size_t>(bytes.size()));

        const bool wasP3R = (payload.size() >= 3 &&
                             (std::memcmp(payload.data(), "p3R", 3) == 0 ||
                              std::memcmp(payload.data(), "P3R", 3) == 0));
        const bool wasXpr2 = (payload.size() >= 4 && std::memcmp(payload.data(), "XPR2", 4) == 0);
        const bool startsDds = (payload.size() >= 4 &&
                                payload[0] == 'D' && payload[1] == 'D' &&
                                payload[2] == 'S' && payload[3] == ' ');

        std::span<const std::uint8_t> texBytes(payload.data(), payload.size());
        std::vector<std::uint8_t> rebuilt;

        if (wasXpr2) {
          std::string name;
          auto dds = gf::textures::rebuild_xpr2_dds_first(texBytes, &name);
          if (dds.has_value() && !dds->empty()) {
            rebuilt = std::move(*dds);
            texBytes = std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size());
          }
        }

        if (wasP3R) {
          const auto prep = gf::textures::prepare_texture_dds_for_export(texBytes, true, astFlags);
          if (prep.ok()) {
            rebuilt = prep.ddsBytes;
            texBytes = std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size());
          }
        } else if (!startsDds) {
          rebuilt = maybe_rebuild_ea_dds(texBytes, astFlags);
          if (!rebuilt.empty()) {
            texBytes = std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size());
          }
        }

        auto info = gf::textures::parse_dds_info(texBytes);
        if (!info.has_value()) return false;
        m_currentTextureBytes = QByteArray(reinterpret_cast<const char*>(texBytes.data()), static_cast<int>(texBytes.size()));
        m_currentTextureType = wasXpr2 ? QString("XPR2") : (wasP3R ? QString("P3R") : QString("DDS"));
        m_currentTextureName = item ? item->text(0) : QString();
        m_currentTextureSelectionVersion = m_previewContext.selectionVersion;
        if (!renderCurrentTextureMip(0)) return false;
        m_viewTabs->setCurrentIndex(2);
        return true;
      };

      if (!renderTexturePreviewNow(previewBytes)) {
        showViewerForItem(item);
      }
    } else {
      showViewerForItem(item);
    }

    statusBar()->showMessage(validateTextureReplacement
                                 ? "Replaced texture in memory. Use Save to write the AST and create a backup."
                                 : "Replaced entry in memory. Use Save to write the AST and create a backup.",
                             4500);
  };

  QAction* chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
  if (!chosen) return;

  if (expandAct && chosen == expandAct) {
    std::function<void(QTreeWidgetItem*)> expandRec = [&](QTreeWidgetItem* it) {
      if (!it) return;
      it->setExpanded(true);
      for (int i = 0; i < it->childCount(); ++i) expandRec(it->child(i));
    };
    expandRec(item);
    return;
  }
  if (collapseAct && chosen == collapseAct) {
    std::function<void(QTreeWidgetItem*)> collapseRec = [&](QTreeWidgetItem* it) {
      if (!it) return;
      for (int i = 0; i < it->childCount(); ++i) collapseRec(it->child(i));
      it->setExpanded(false);
    };
    collapseRec(item);
    return;
  }
  if (refreshAct && chosen == refreshAct) {
    refreshCurrentArchiveView();
    return;
  }
  if (copyNameAct && chosen == copyNameAct) {
    QApplication::clipboard()->setText(item->text(0).trimmed());
    statusBar()->showMessage("Copied name to clipboard.", 2000);
    return;
  }
  if (copyPathAct && chosen == copyPathAct) {
    QApplication::clipboard()->setText(QDir::toNativeSeparators(path));
    statusBar()->showMessage("Copied path to clipboard.", 2000);
    return;
  }

  if (openAct && chosen == openAct) {
    auto* mw = new MainWindow();
    mw->setAttribute(Qt::WA_DeleteOnClose);
    mw->openStandaloneAst(path);
    mw->show();
    return;
  }

  if (textureReplaceAct && chosen == textureReplaceAct) {
    doEmbeddedReplace(true);
    return;
  }
  if (replaceAct && chosen == replaceAct) {
    doEmbeddedReplace(false);
    return;
  }

  if (rebuildContainerToAct && chosen == rebuildContainerToAct) {
    if (!editingEnabled()) {
      showInfoDialog("Rebuild Container", "Editing is unavailable.");
      return;
    }

    const QString containerPath = item->data(0, Qt::UserRole).toString();
    if (containerPath.isEmpty()) return;

    std::string err;
    auto ed = gf::core::AstContainerEditor::load(containerPath.toStdString(), &err);
    if (!ed.has_value()) {
      showErrorDialog("Load Failed", "The AST container could not be loaded.", QString::fromStdString(err), false);
      return;
    }
    const auto bytes = ed->rebuild(&err);
    if (bytes.empty() && !err.empty()) {
      showErrorDialog("Rebuild Failed", "The AST could not be rebuilt.", QString::fromStdString(err), true);
      return;
    }

    QFileInfo fi(containerPath);
    const QString suggested = fi.absoluteDir().absoluteFilePath(fi.completeBaseName() + "_rebuilt." + fi.suffix());
    const QString outPath = QFileDialog::getSaveFileName(this,
                                                         "Rebuild Container To",
                                                         suggested,
                                                         "EA Archives (*.ast *.bgfa);;All Files (*.*)");
    if (outPath.isEmpty()) return;

    const auto* bptr = reinterpret_cast<const std::byte*>(bytes.data());
    gf::core::SafeWriteOptions opt;
    opt.make_backup = true;
    const auto r = gf::core::safe_write_bytes(outPath.toStdString(),
                                              std::span<const std::byte>(bptr, bytes.size()),
                                              opt);
    if (!r.ok) {
      showErrorDialog("Write Failed", "The AST container could not be written.", QString::fromStdString(r.message), true);
      return;
    }
    statusBar()->showMessage(QString("Rebuilt container → %1").arg(QDir::toNativeSeparators(outPath)), 4000);
    return;
  }

  if (rawViewAct && chosen == rawViewAct) {
    showViewerForItem(item);
    if (m_viewTabs) m_viewTabs->setCurrentIndex(0);
    return;
  }

  if (exportAct && chosen == exportAct) {
    if (isExtractableEntry) {
      const QString outDir = chooseOutputDir();
      if (outDir.isEmpty()) return;

      std::string err;
      auto ed = gf::core::AstContainerEditor::load(path.toStdString(), &err);
      if (!ed.has_value()) {
        showErrorDialog("Extract Failed", "The AST container could not be loaded.", QString::fromStdString(err), false);
        return;
      }

      QStringList failures;
      int okCount = exportEntry(item, *ed, outDir, &failures) ? 1 : 0;
      if (!failures.isEmpty()) {
        showErrorDialog("Extract Failed",
                        "The selected entry could not be extracted.",
                        failures.join("\n"),
                        false);
        return;
      }
      statusBar()->showMessage(QString("Extracted %1 item to %2").arg(okCount).arg(QDir::toNativeSeparators(outDir)), 3000);
      return;
    }

    exportCopyOf(path);
    return;
  }

  if (extractFolderAct && chosen == extractFolderAct) {
    const QString outDir = chooseOutputDir();
    if (outDir.isEmpty()) return;

    QMap<QString, std::shared_ptr<gf::core::AstContainerEditor>> editors;
    QStringList failures;
    int okCount = 0;

    for (auto* sel : folderExtractable) {
      const QString selPath = sel->data(0, Qt::UserRole).toString();
      if (selPath.isEmpty()) continue;
      if (!editors.contains(selPath)) {
        std::string err;
        auto loaded = gf::core::AstContainerEditor::load(selPath.toStdString(), &err);
        if (!loaded.has_value()) {
          failures.push_back(QString("%1 — %2").arg(QFileInfo(selPath).fileName(), QString::fromStdString(err)));
          continue;
        }
        editors.insert(selPath, std::make_shared<gf::core::AstContainerEditor>(std::move(*loaded)));
      }
      auto editor = editors.value(selPath);
      if (editor && exportEntry(sel, *editor, outDir, &failures)) ++okCount;
    }

    if (!failures.isEmpty()) {
      showErrorDialog("Extract Folder",
                      QString("Extracted %1 item(s), but some entries failed.").arg(okCount),
                      failures.join("\n"),
                      false);
    } else {
      showInfoDialog("Extract Folder", QString("Extracted %1 item(s) to:\n%2").arg(okCount).arg(QDir::toNativeSeparators(outDir)));
    }
    return;
  }

  if (extractSelectedAct && chosen == extractSelectedAct) {
    const QString outDir = chooseOutputDir();
    if (outDir.isEmpty()) return;

    QMap<QString, std::shared_ptr<gf::core::AstContainerEditor>> editors;
    QStringList failures;
    int okCount = 0;

    for (auto* sel : selectedExtractable) {
      const QString selPath = sel->data(0, Qt::UserRole).toString();
      if (selPath.isEmpty()) continue;
      if (!editors.contains(selPath)) {
        std::string err;
        auto loaded = gf::core::AstContainerEditor::load(selPath.toStdString(), &err);
        if (!loaded.has_value()) {
          failures.push_back(QString("%1 — %2").arg(QFileInfo(selPath).fileName(), QString::fromStdString(err)));
          continue;
        }
        editors.insert(selPath, std::make_shared<gf::core::AstContainerEditor>(std::move(*loaded)));
      }
      auto editor = editors.value(selPath);
      if (editor && exportEntry(sel, *editor, outDir, &failures)) ++okCount;
    }

    if (!failures.isEmpty()) {
      showErrorDialog("Extract Selected",
                      QString("Extracted %1 item(s), but some entries failed.").arg(okCount),
                      failures.join("\n"),
                      false);
    } else {
      showInfoDialog("Extract Selected", QString("Extracted %1 item(s) to:\n%2").arg(okCount).arg(QDir::toNativeSeparators(outDir)));
    }
    return;
  }

  if (extractAllAct && chosen == extractAllAct) {
    const QString outDir = chooseOutputDir();
    if (outDir.isEmpty()) return;

    std::string err;
    auto ed = gf::core::AstContainerEditor::load(path.toStdString(), &err);
    if (!ed.has_value()) {
      showErrorDialog("Extract All Failed", "The AST container could not be loaded.", QString::fromStdString(err), false);
      return;
    }

    QStringList failures;
    int okCount = 0;
    const auto& ents = ed->entries();
    QFileInfo cfi(path);
    const QString containerStem = sanitizeExportName(cfi.completeBaseName().isEmpty() ? cfi.fileName() : cfi.completeBaseName());

    for (const auto& entry : ents) {
      const QString name = bytesToEntryName(entry.nameBytes, entry.index);
      const QFileInfo entryFi(name);
      const QString suffixUpper = entryFi.suffix().trimmed().toUpper();
      const bool isP3R = (suffixUpper == "P3R");

      std::string localErr;
      auto bytesOpt = isP3R
          ? ed->getEntryStoredBytes(entry.index)
          : ed->getEntryInflatedBytes(entry.index, &localErr);
      if (!bytesOpt.has_value()) {
        const QString why = isP3R
            ? QStringLiteral("could not read stored entry bytes")
            : QString::fromStdString(localErr);
        failures.push_back(QString("entry_%1 — %2").arg(entry.index).arg(why));
        continue;
      }

      QByteArray outBytes(reinterpret_cast<const char*>(bytesOpt->data()), int(bytesOpt->size()));
      QString exportTypeUpper = isP3R
          ? QStringLiteral("P3R")
          : detectTypeUpperFromBytes(std::span<const std::uint8_t>(bytesOpt->data(), bytesOpt->size()));
      QString baseName = entryFi.completeBaseName().isEmpty() ? sanitizeExportName(name) : sanitizeExportName(entryFi.completeBaseName());
      if (baseName.isEmpty()) baseName = QString("%1_entry_%2").arg(containerStem).arg(entry.index);

      if (exportTypeUpper.startsWith("P3R")) {
        std::vector<std::uint8_t> payload(bytesOpt->begin(), bytesOpt->end());
        std::vector<std::uint8_t> rebuilt = p3rToDds(std::span<const std::uint8_t>(payload.data(), payload.size()));
        if (rebuilt.size() < 4 || std::memcmp(rebuilt.data(), "DDS ", 4) != 0) {
          rebuilt = maybe_rebuild_ea_dds(std::span<const std::uint8_t>(payload.data(), payload.size()), entry.flags);
        }
        if (rebuilt.size() >= 4 && std::memcmp(rebuilt.data(), "DDS ", 4) == 0) {
          outBytes = QByteArray(reinterpret_cast<const char*>(rebuilt.data()), int(rebuilt.size()));
          exportTypeUpper = "DDS";
        } else {
          const auto prep = gf::textures::prepare_texture_dds_for_export(
              std::span<const std::uint8_t>(payload.data(), payload.size()), true, entry.flags);
          failures.push_back(QString("%1 — %2").arg(name, p3rConversionDetailsText(prep)));
          continue;
        }
      }

      if (exportTypeUpper == "DDS") {
        QString ddsDetails;
        if (!validateDdsForWrite(this,
                                 "Extract All Failed",
                                 name,
                                 std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(outBytes.constData()), static_cast<std::size_t>(outBytes.size())),
                                 &ddsDetails)) {
          failures.push_back(QString("%1 — %2").arg(name, ddsDetails));
          continue;
        }
      }

      QString ext = entryFi.suffix().trimmed().toLower();
      if (ext.isEmpty()) ext = defaultExtForTypeUpper(exportTypeUpper);
      if (ext.isEmpty()) ext = "bin";

      const QString outPath = uniqueOutputPath(outDir, baseName, ext);
      const auto* bptr = reinterpret_cast<const std::byte*>(outBytes.constData());
      gf::core::SafeWriteOptions opt;
      opt.make_backup = false;
      const auto r = gf::core::safe_write_bytes(outPath.toStdString(),
                                                std::span<const std::byte>(bptr, static_cast<std::size_t>(outBytes.size())),
                                                opt);
      if (!r.ok) {
        failures.push_back(QString("%1 — %2").arg(name, QString::fromStdString(r.message)));
        continue;
      }
      ++okCount;
    }

    if (!failures.isEmpty()) {
      showErrorDialog("Extract All",
                      QString("Extracted %1 item(s), but some entries failed.").arg(okCount),
                      failures.join("\n"),
                      false);
    } else {
      showInfoDialog("Extract All", QString("Extracted %1 item(s) to:\n%2").arg(okCount).arg(QDir::toNativeSeparators(outDir)));
    }
    return;
  }

  if (renameFriendlyAct && chosen == renameFriendlyAct) {
    const quint64 baseOffset = item->data(0, Qt::UserRole + 1).toULongLong();
    const quint64 maxReadable = item->data(0, Qt::UserRole + 2).toULongLong();
    const QString ck = cacheKeyForEmbedded(baseOffset, static_cast<std::uint64_t>(maxReadable));

    const QString label = item->text(0);
    const int paren = label.indexOf('(');
    QString suffix;
    if (paren >= 0) suffix = label.mid(paren).trimmed();

    QString current = (paren >= 0 ? label.left(paren) : label).trimmed();
    if (current.endsWith(".ast", Qt::CaseInsensitive)) current.chop(4);

    bool ok = false;
    const QString next = QInputDialog::getText(this,
                                               "Rename Friendly Name",
                                               "Friendly name (no extension):",
                                               QLineEdit::Normal,
                                               current,
                                               &ok).trimmed();
    if (!ok) return;
    if (next.isEmpty()) {
      showErrorDialog("Rename Friendly Name", "Name cannot be empty.", QString(), false);
      return;
    }

    const QString newLabel = suffix.isEmpty() ? QString("%1.ast").arg(next)
                                              : QString("%1.ast %2").arg(next, suffix);
    item->setText(0, newLabel);

    const QString kind = m_nameCache.lookupKind(ck);
    const bool hasApt = m_nameCache.lookupHasApt(ck);
    m_nameCache.putMeta(ck, next, kind, hasApt);

    setDirty(true);
    statusBar()->showMessage("Friendly name updated (unsaved).", 2500);
    return;
  }
}

void MainWindow::onSearchChanged(const QString& text) {
  const QString needle = text.trimmed().toLower();

  if (!m_tree) return;

  // Empty search: show everything
  if (needle.isEmpty()) {
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
      auto* top = m_tree->topLevelItem(i);
      if (!top) continue;
      // show all recursively
      std::function<void(QTreeWidgetItem*)> showAll = [&](QTreeWidgetItem* it) {
        it->setHidden(false);
        for (int c = 0; c < it->childCount(); ++c) showAll(it->child(c));
      };
      showAll(top);
    }
    return;
  }

  for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
    auto* top = m_tree->topLevelItem(i);
    if (!top) continue;
    filterItemRecursive(top, needle);
  }
}

bool MainWindow::filterItemRecursive(QTreeWidgetItem* item, const QString& needleLower) {
  if (!item) return false;

  bool anyChildVisible = false;
  for (int i = 0; i < item->childCount(); ++i) {
    anyChildVisible |= filterItemRecursive(item->child(i), needleLower);
  }

  const QString name = item->text(0).toLower();
  const QString tip = item->toolTip(0).toLower();

  const bool selfMatch = name.contains(needleLower) || tip.contains(needleLower);
  const bool visible = selfMatch || anyChildVisible;

  item->setHidden(!visible);
  return visible;
}

void MainWindow::onCurrentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous) {
  if (!current) return;

if (m_suppressSelectionChange) return;

// If the user is actively editing text and it's dirty, ask before switching selection.
if (previous && m_textView && m_textView->document()->isModified() && (m_textExternalMode || m_textForceEdit)) {
  QMessageBox::StandardButton choice = QMessageBox::question(
      this,
      "Unsaved Changes",
      "You have unsaved text edits.",
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
      QMessageBox::Cancel);

  if (choice == QMessageBox::Cancel) {
    m_suppressSelectionChange = true;
    m_tree->setCurrentItem(previous);
    m_suppressSelectionChange = false;
    return;
  }

  if (choice == QMessageBox::Save) {
    if (m_textApplyAction && m_textApplyAction->isEnabled()) {
      m_textApplyAction->trigger();
    }
  } else if (choice == QMessageBox::Discard) {
    m_textView->document()->setModified(false);
  }
}

updateStatusSelection(current);
  invalidatePreviewContext();

  // Default to view-only until the user explicitly toggles Edit (embedded text only).
  if (!m_textExternalMode) {
    m_textForceEdit = false;
    if (m_textEditAction) m_textEditAction->setChecked(false);
  }

  const QString path = current->data(0, Qt::UserRole).toString();
  if (path.isEmpty()) {
    // Not a file node; selection context is handled by the persistent status widgets.
    m_doc.path.clear();
    updateDocumentActions();
    return;
  }

  // Even for embedded entries, the path is the backing container on disk.
  m_doc.path = path;
  updateDocumentActions();

  // Document context for Save As / future editing.
  m_doc.path = path;
  updateDocumentActions();
  showViewerForItem(current);
}

QString MainWindow::formatHexPreview(const QByteArray& data, quint64 baseOffset) {
  QString out;
  out.reserve(data.size() * 4);

  // Keep the offset column a fixed width so alignment doesn't drift on very large
  // archives (where absolute offsets exceed 0xFFFFFFFF).
  // QByteArray::size() returns qsizetype; avoid std::max mixed-type issues.
  const quint64 maxOffset = baseOffset + (data.isEmpty() ? 0ull : static_cast<quint64>(data.size() - 1));
  const int offsetWidth = (maxOffset > 0xFFFFFFFFull) ? 16 : 8;

  const int bytesPerLine = 16;
  for (int i = 0; i < data.size(); i += bytesPerLine) {
    const int remaining = static_cast<int>(data.size() - i);
    const int n = std::min(bytesPerLine, remaining);

    out += QString("%1  ").arg(baseOffset + static_cast<quint64>(i), offsetWidth, 16, QChar('0')).toUpper();

    // Hex
    for (int j = 0; j < bytesPerLine; ++j) {
      if (j < n) {
        const unsigned char b = static_cast<unsigned char>(data.at(i + j));
        out += QString("%1 ").arg(b, 2, 16, QChar('0')).toUpper();
      } else {
        out += "   ";
      }
      if (j == 7) out += " ";
    }

    out += " |";
    // ASCII
    for (int j = 0; j < n; ++j) {
      const unsigned char b = static_cast<unsigned char>(data.at(i + j));
      const QChar c = (b >= 32 && b <= 126) ? QChar(b) : QChar('.');
      out += c;
    }
    out += "|\n";
  }
  return out;
}



static QString aptSummaryToXmlFallback(const gf::apt::AptFile& file,
                                      const QString& aptName,
                                      const QString& constName) {
  const auto& sum = file.summary;
  auto esc = [](QString v) {
    v.replace('&', "&amp;");
    v.replace('<', "&lt;");
    v.replace('>', "&gt;");
    v.replace('"', "&quot;");
    return v;
  };
  QString xml;
  xml += QString("<apt name=\"%1\" const=\"%2\">\n").arg(esc(aptName), esc(constName));
  xml += QString("  <summary screenWidth=\"%1\" screenHeight=\"%2\" frames=\"%3\" characters=\"%4\" imports=\"%5\" exports=\"%6\" aptDataOffset=\"0x%7\" />\n")
             .arg(sum.screensizex)
             .arg(sum.screensizey)
             .arg(sum.framecount)
             .arg(sum.charactercount)
             .arg(sum.importcount)
             .arg(sum.exportcount)
             .arg(QString::number(qulonglong(sum.aptdataoffset), 16).toUpper());
  xml += "  <imports>\n";
  for (std::size_t i = 0; i < file.imports.size(); ++i) {
    const auto& im = file.imports[i];
    xml += QString("    <import index=\"%1\" movie=\"%2\" name=\"%3\" character=\"%4\" offset=\"0x%5\" />\n")
               .arg(qulonglong(i))
               .arg(esc(QString::fromStdString(im.movie)))
               .arg(esc(QString::fromStdString(im.name)))
               .arg(im.character)
               .arg(QString::number(qulonglong(im.offset), 16).toUpper());
  }
  xml += "  </imports>\n";
  xml += "  <exports>\n";
  for (std::size_t i = 0; i < file.exports.size(); ++i) {
    const auto& ex = file.exports[i];
    xml += QString("    <export index=\"%1\" name=\"%2\" character=\"%3\" offset=\"0x%4\" />\n")
               .arg(qulonglong(i))
               .arg(esc(QString::fromStdString(ex.name)))
               .arg(ex.character)
               .arg(QString::number(qulonglong(ex.offset), 16).toUpper());
  }
  xml += "  </exports>\n";
  xml += "  <frames>\n";
  for (std::size_t i = 0; i < file.frames.size(); ++i) {
    const auto& fr = file.frames[i];
    xml += QString("    <frame index=\"%1\" itemCount=\"%2\" itemsOffset=\"0x%3\" offset=\"0x%4\" />\n")
               .arg(qulonglong(i))
               .arg(fr.frameitemcount)
               .arg(QString::number(qulonglong(fr.items_offset), 16).toUpper())
               .arg(QString::number(qulonglong(fr.offset), 16).toUpper());
  }
  xml += "  </frames>\n";
  xml += "  <characters>\n";
  for (std::size_t i = 0; i < file.characters.size(); ++i) {
    const auto& ch = file.characters[i];
    xml += QString("    <character index=\"%1\" type=\"%2\" signature=\"0x%3\" offset=\"0x%4\" />\n")
               .arg(qulonglong(i))
               .arg(ch.type)
               .arg(QString::number(qulonglong(ch.signature), 16).toUpper())
               .arg(QString::number(qulonglong(ch.offset), 16).toUpper());
  }
  xml += "  </characters>\n";
  xml += "  <slices>\n";
  for (std::size_t i = 0; i < file.slices.size(); ++i) {
    const auto& sl = file.slices[i];
    xml += QString("    <slice index=\"%1\" name=\"%2\" offset=\"0x%3\" size=\"%4\" />\n")
               .arg(qulonglong(i))
               .arg(esc(QString::fromStdString(sl.name)))
               .arg(QString::number(qulonglong(sl.offset), 16).toUpper())
               .arg(qulonglong(sl.size));
  }
  xml += "  </slices>\n";
  xml += "</apt>\n";
  return xml;
}

// ---------------------------------------------------------------------------
// loadAptForItem  (v0.8.3)
//   Unified helper for loading an embedded or standalone APT into the APT tab.
//   Walks all tree ancestors to find the nearest embedded-AST ancestor, extracts
//   the nested .ast context, finds the best matching .const, writes temp files,
//   populates the APT tab tree, and switches to it.
//   Sets m_textAptPath / m_textConstPath as a side-effect for the Text-tab XML
//   mode used by showViewerForItem.
//   Returns false and fills *errorOut on any failure (never silently drops errors).
// ---------------------------------------------------------------------------
bool MainWindow::loadAptForItem(QTreeWidgetItem* item, QString* errorOut) {
  auto setErr = [&](const QString& msg) -> bool {
    if (errorOut) *errorOut = msg;
    return false;
  };

  if (!item) return setErr("No item selected.");

  const QString path      = item->data(0, Qt::UserRole).toString();
  const bool isEmbedded   = item->data(0, Qt::UserRole + 3).toBool();
  const QString itemType  = item->text(1).trimmed();
  const QFileInfo fi(path);

  // Detect APT by type column (embedded) or file extension (standalone).
  const bool isAptType = (itemType.compare("APT",  Qt::CaseInsensitive) == 0) ||
                         (itemType.compare("APT1", Qt::CaseInsensitive) == 0);
  const bool isAptExt  = (fi.suffix().compare("apt",  Qt::CaseInsensitive) == 0) ||
                         (fi.suffix().compare("apt1", Qt::CaseInsensitive) == 0);
  if (!isAptType && !isAptExt)
    return setErr("Selected item is not an APT entry.");

  if (path.isEmpty())
    return setErr("Item has no associated file path.");

  auto bytesToName = [](const std::vector<std::uint8_t>& v) -> QString {
    size_t n = 0;
    while (n < v.size() && v[n] != 0) ++n;
    return QString::fromUtf8(reinterpret_cast<const char*>(v.data()), static_cast<int>(n));
  };
  auto isConstLike = [](const QString& n) -> bool {
    return n.endsWith(".const",      Qt::CaseInsensitive) ||
           n.endsWith(".const.bin",  Qt::CaseInsensitive) ||
           n.endsWith(".const.dat",  Qt::CaseInsensitive) ||
           n.compare("APT Constant File", Qt::CaseInsensitive) == 0;
  };

  // Default paths used for standalone; overridden in the embedded branch.
  QString aptPath   = path;
  QString constPath = fi.absolutePath() + "/" + fi.completeBaseName() + ".const";

  if (isEmbedded) {
    const int aptEntryIndex = item->data(0, Qt::UserRole + 6).toInt();
    if (aptEntryIndex < 0)
      return setErr("APT entry has no valid entry index.");

    // ---- Walk ALL ancestors to find the nearest embedded-AST ancestor ----
    // This correctly handles the grouped UI tree where the APT may be nested
    // several folder/group nodes away from its containing AST node.
    QTreeWidgetItem* astAncestor = nullptr;
    for (QTreeWidgetItem* p = item->parent(); p; p = p->parent()) {
      if (p->text(1).compare("AST", Qt::CaseInsensitive) == 0 &&
          p->data(0, Qt::UserRole + 6).isValid()) {
        astAncestor = p;
        break;
      }
    }

    // Load the root on-disk container.
    auto loadOuter = [&]() -> std::optional<gf::core::AstContainerEditor> {
      if (m_liveAstEditor && m_liveAstPath == path) return *m_liveAstEditor;
      return gf::core::AstContainerEditor::load(path.toStdString());
    };

    std::optional<gf::core::AstContainerEditor> contextEditor;
    QString contextStem = fi.completeBaseName();

    if (astAncestor) {
      // The APT lives inside a nested .ast; extract it first.
      // Use direct file-offset reading instead of AstContainerEditor::getEntryInflatedBytes
      // to avoid index mismatches on PS3/Xbox360 format containers where the LE-based
      // AstContainerEditor::load may only parse a fraction of the real entries.
      const std::uint64_t nestedOff =
          astAncestor->data(0, Qt::UserRole + 1).toULongLong();
      const std::uint64_t nestedSz =
          astAncestor->data(0, Qt::UserRole + 2).toULongLong();
      const std::uint32_t parentIdx =
          static_cast<std::uint32_t>(astAncestor->data(0, Qt::UserRole + 6).toULongLong());

      if (nestedSz == 0)
        return setErr(QString("Nested AST entry #%1 has zero size.").arg(parentIdx));

      auto rawBytes = read_file_range(path, nestedOff, nestedSz);
      if (rawBytes.empty())
        return setErr(QString("Cannot read nested AST bytes at offset 0x%1 (size %2).")
                          .arg(nestedOff, 0, 16).arg(nestedSz));

      // Inflate if the bytes look like a zlib stream; otherwise use as-is.
      std::vector<std::uint8_t> nestedBytesVec;
      if (rawBytes.size() >= 2 &&
          looks_like_zlib_cmf_flg(rawBytes[0], rawBytes[1])) {
        try {
          nestedBytesVec = zlib_inflate_unknown_size(
              std::span<const std::uint8_t>(rawBytes.data(), rawBytes.size()));
        } catch (const std::exception& ex) {
          return setErr(QString("Failed to decompress nested AST entry #%1: %2")
                            .arg(parentIdx)
                            .arg(QString::fromStdString(ex.what())));
        }
      } else {
        nestedBytesVec = std::move(rawBytes);
      }

      if (nestedBytesVec.empty())
        return setErr(QString("Nested AST entry #%1 decompressed to empty data.").arg(parentIdx));

      const QString tmpRoot = QDir::tempPath() + "/ASTra/nestedast";
      QDir().mkpath(tmpRoot);

      QString safeStem = QFileInfo(astAncestor->text(0)).completeBaseName();
      if (safeStem.isEmpty()) safeStem = "embedded_ast";
      for (QChar c : {QChar('\\'), QChar('/'), QChar(':'),
                      QChar('*'), QChar('?'), QChar('"'),
                      QChar('<'), QChar('>'), QChar('|'), QChar(' ')})
        safeStem.replace(c, '_');

      const QString nestedAstPath =
          tmpRoot + "/" + safeStem + QString("_nested_%1.ast").arg(parentIdx);

      QFile nf(nestedAstPath);
      if (!nf.open(QIODevice::WriteOnly))
        return setErr(QString("Cannot write nested AST to temp: %1").arg(nestedAstPath));
      nf.write(reinterpret_cast<const char*>(nestedBytesVec.data()),
               static_cast<qint64>(nestedBytesVec.size()));
      nf.close();

      std::string loadErr;
      auto nestedCtx = gf::core::AstContainerEditor::load(nestedAstPath.toStdString(), &loadErr);
      if (!nestedCtx)
        return setErr(QString("Cannot parse nested AST '%1': %2")
                          .arg(QFileInfo(nestedAstPath).fileName(),
                               QString::fromStdString(loadErr)));

      contextEditor = std::move(*nestedCtx);
      contextStem   = safeStem;
    } else {
      // No nested-AST ancestor: APT is directly in the root container.
      contextEditor = loadOuter();
      if (!contextEditor)
        return setErr(QString("Cannot open container: %1")
                          .arg(QDir::toNativeSeparators(path)));
    }

    const auto& ents = contextEditor->entries();
    if (static_cast<size_t>(aptEntryIndex) >= ents.size())
      return setErr(QString("APT entry index %1 out of range (%2 entries).")
                        .arg(aptEntryIndex).arg(ents.size()));

    // ---- Find the best matching .const entry ----
    // Primary strategy: scan tree siblings of the APT item for a CONST-type entry.
    // This uses the pre-parsed type column and never depends on nameBytes being populated,
    // which fixes the common case where entries have no stored names (only File_XXXXX labels).
    int constIdx = -1;
    {
      QTreeWidgetItem* parentTreeItem = item->parent();
      if (parentTreeItem) {
        int aptRow = -1;
        for (int r = 0; r < parentTreeItem->childCount(); ++r)
          if (parentTreeItem->child(r) == item) { aptRow = r; break; }

        if (aptRow >= 0) {
          int bestRow = -1, bestScore = std::numeric_limits<int>::max();
          for (int r = 0; r < parentTreeItem->childCount(); ++r) {
            auto* sib = parentTreeItem->child(r);
            if (!sib || sib == item) continue;
            const QString sibType = sib->text(1).trimmed();
            const QString sibName = sib->text(0).trimmed();
            // Accept CONST type, APT1 (EA's paired const companion), and
            // entries whose type or name is "APT Constant File".
            const bool isConstCandidate =
                sibType.compare("CONST",             Qt::CaseInsensitive) == 0 ||
                sibType.compare("APT1",              Qt::CaseInsensitive) == 0 ||
                sibType.compare("APT Constant File", Qt::CaseInsensitive) == 0 ||
                sibName.compare("APT Constant File", Qt::CaseInsensitive) == 0;
            if (!isConstCandidate) continue;
            // Prefer nearest; slightly prefer entries that appear before the APT.
            int score = std::abs(r - aptRow) * 10;
            if (r < aptRow) score -= 1;
            if (score < bestScore) { bestScore = score; bestRow = r; }
          }
          if (bestRow >= 0)
            constIdx = static_cast<int>(
                parentTreeItem->child(bestRow)->data(0, Qt::UserRole + 6).toULongLong());
        }
      }
    }

    // Fallback: nameBytes-based scan (handles rare cases where the tree isn't materialised
    // or the const entry is present under a different type label).
    if (constIdx < 0) {
      const QString aptName  = bytesToName(ents[static_cast<size_t>(aptEntryIndex)].nameBytes);
      const QString baseName = QFileInfo(aptName).completeBaseName();

      // Pass A: exact same-base .const name.
      if (!baseName.isEmpty()) {
        for (size_t i = 0; i < ents.size(); ++i) {
          if (QString::compare(bytesToName(ents[i].nameBytes),
                               baseName + ".const", Qt::CaseInsensitive) == 0) {
            constIdx = static_cast<int>(i);
            break;
          }
        }
      }
      // Pass B: nearby ±3 entries with a const-like file extension.
      if (constIdx < 0) {
        for (int off = -3; off <= 3; ++off) {
          int k = aptEntryIndex + off;
          if (k >= 0 && k < static_cast<int>(ents.size()) && k != aptEntryIndex &&
              isConstLike(bytesToName(ents[static_cast<size_t>(k)].nameBytes))) {
            constIdx = k;
            break;
          }
        }
      }
      // Pass C: best-scored global scan; prefer same directory, prefer before APT.
      if (constIdx < 0) {
        const QString aptDir = QFileInfo(aptName).path();
        int best = -1, bestScore2 = std::numeric_limits<int>::max();
        for (size_t i = 0; i < ents.size(); ++i) {
          if (static_cast<int>(i) == aptEntryIndex) continue;
          const QString n = bytesToName(ents[i].nameBytes);
          if (!isConstLike(n)) continue;
          int score = std::abs(static_cast<int>(i) - aptEntryIndex) * 10;
          if (!aptDir.isEmpty() &&
              QString::compare(QFileInfo(n).path(), aptDir, Qt::CaseInsensitive) == 0)
            score -= 3;
          if (static_cast<int>(i) < aptEntryIndex) score -= 1;
          if (score < bestScore2) { bestScore2 = score; best = static_cast<int>(i); }
        }
        constIdx = best;
      }
      // Pass D: content-based scan for "Apt constant file" magic in nearby entries
      // (handles synthetic-named entries where nameBytes are empty/zeroed).
      if (constIdx < 0) {
        constexpr int kRange = 5;
        constexpr std::string_view kAptConstMagic = "Apt cons";
        for (int off = -kRange; off <= kRange && constIdx < 0; ++off) {
          int k = aptEntryIndex + off;
          if (k < 0 || k >= static_cast<int>(ents.size()) || k == aptEntryIndex) continue;
          auto prefix = contextEditor->getEntryInflatedBytes(static_cast<std::uint32_t>(k));
          if (prefix && prefix->size() >= 8 &&
              std::string_view(reinterpret_cast<const char*>(prefix->data()), 8) == kAptConstMagic)
            constIdx = k;
        }
      }
    }

    if (constIdx < 0)
      return setErr(QString("No CONST entry found near APT entry #%1 "
                            "(checked tree siblings and %2 container entries).")
                        .arg(aptEntryIndex).arg(ents.size()));

    std::string exErr2;
    auto aptBytes   = contextEditor->getEntryInflatedBytes(
                          static_cast<std::uint32_t>(aptEntryIndex), &exErr2);
    auto constBytes = contextEditor->getEntryInflatedBytes(
                          static_cast<std::uint32_t>(constIdx));
    if (!aptBytes)
      return setErr(QString("Cannot extract APT entry #%1: %2")
                        .arg(aptEntryIndex).arg(QString::fromStdString(exErr2)));
    if (!constBytes)
      return setErr(QString("Cannot extract CONST entry #%1.").arg(constIdx));

    // Store embedded save context so onAptSave() can write back to the container.
    m_aptIsEmbedded           = true;
    m_aptSaveAptEntryIdx      = aptEntryIndex;
    m_aptSaveConstEntryIdx    = constIdx;
    m_aptSaveContextIsNested  = (astAncestor != nullptr);
    m_aptSaveOuterPath        = path; // on-disk root container path

    // Write temp files.
    const QString tmpRoot = QDir::tempPath() + "/ASTra/aptxml";
    QDir().mkpath(tmpRoot);
    const QString tmpBase = tmpRoot + "/" + contextStem + QString("_%1").arg(aptEntryIndex);
    aptPath   = tmpBase + ".apt";
    constPath = tmpBase + ".const";

    { QFile f(aptPath);
      if (!f.open(QIODevice::WriteOnly))
        return setErr(QString("Cannot write temp APT: %1").arg(aptPath));
      f.write(reinterpret_cast<const char*>(aptBytes->data()),
              static_cast<qint64>(aptBytes->size())); }
    { QFile f(constPath);
      if (!f.open(QIODevice::WriteOnly))
        return setErr(QString("Cannot write temp CONST: %1").arg(constPath));
      f.write(reinterpret_cast<const char*>(constBytes->data()),
              static_cast<qint64>(constBytes->size())); }
  } else {
    // Standalone APT — aptPath/constPath already point to the real files.
    m_aptIsEmbedded          = false;
    m_aptSaveAptEntryIdx     = -1;
    m_aptSaveConstEntryIdx   = -1;
    m_aptSaveContextIsNested = false;
    m_aptSaveOuterPath.clear();
  }

  // Store paths for Text-tab XML mode (side-effect read by showViewerForItem).
  m_textAptPath   = aptPath;
  m_textConstPath = constPath;

  const QString sourceLabel = item->text(0);
  if (!populateAptViewerFromFiles(aptPath, constPath, sourceLabel)) {
    // populateAptViewerFromFiles placed an error message in m_aptDetails.
    return setErr(QString("Failed to parse APT: %1\n(CONST: %2)")
                      .arg(QFileInfo(aptPath).fileName(),
                           QFileInfo(constPath).fileName()));
  }

  // Switch to APT tab.
  if (m_viewTabs && m_aptTab) {
    const int aptTabIdx = m_viewTabs->indexOf(m_aptTab);
    if (aptTabIdx >= 0) m_viewTabs->setCurrentIndex(aptTabIdx);
  }
  return true;
}

void MainWindow::clearAptViewer() {
  if (m_aptTree) m_aptTree->clear();
  if (m_aptDetails) m_aptDetails->clear();
  if (m_aptDlStatusLabel) m_aptDlStatusLabel->setText("(no APT loaded)");
  if (m_aptFrameDlDump) m_aptFrameDlDump->clear();
  if (m_aptSelectionManager) m_aptSelectionManager->clearSelection();
  if (m_aptPreviewScene) { m_aptPreviewScene->clearEditorOverlay(); m_aptPreviewScene->clear(); }
  m_currentAptFile.reset();
  m_aptHintsCache.clear();
  m_aptDirty = false;
  m_aptUpdatingUi = false;
  m_aptPreviewInProgress = false;
  m_aptCurrentFrameIndex = 0;
  m_aptCharPreviewIdx = -1;
  m_aptIsEmbedded = false;
  m_aptSaveContextIsNested = false;
  m_aptSaveAptEntryIdx = -1;
  m_aptSaveConstEntryIdx = -1;
  m_aptSaveOuterPath.clear();

  if (m_aptApplyAction)   m_aptApplyAction->setEnabled(false);
  if (m_aptSaveAction)    m_aptSaveAction->setEnabled(false);
  if (m_aptExportAction)  m_aptExportAction->setEnabled(false);
  if (m_aptBringForwardAction) m_aptBringForwardAction->setEnabled(false);
  if (m_aptSendBackwardAction) m_aptSendBackwardAction->setEnabled(false);
  if (m_aptAddPlacementAction) m_aptAddPlacementAction->setEnabled(false);
  if (m_aptRemovePlacementAction) m_aptRemovePlacementAction->setEnabled(false);
  if (m_aptDuplicatePlacementAction) m_aptDuplicatePlacementAction->setEnabled(false);
  if (m_aptPrevFrameAction) m_aptPrevFrameAction->setEnabled(false);
  if (m_aptNextFrameAction) m_aptNextFrameAction->setEnabled(false);
  if (m_aptFrameSpin) {
    m_aptFrameSpin->blockSignals(true);
    m_aptFrameSpin->setRange(0, 0);
    m_aptFrameSpin->setValue(0);
    m_aptFrameSpin->setEnabled(false);
    m_aptFrameSpin->blockSignals(false);
  }
  if (m_aptFrameCountLabel) m_aptFrameCountLabel->setText(" / 0 ");
  if (m_aptPropStack) m_aptPropStack->setCurrentWidget(m_aptDetails);
  refreshAptPreview();
}

bool MainWindow::populateAptViewerFromFiles(const QString& aptPath, const QString& constPath, const QString& sourceLabel) {
  clearAptViewer();
  if (!m_aptTree) return false;

  std::string err;
  const auto fileOpt = gf::apt::read_apt_file(aptPath.toStdString(), constPath.toStdString(), &err);
  if (!fileOpt) {
    if (m_aptDetails) {
      m_aptDetails->setPlainText(QString("Failed to parse APT.\n\n%1").arg(QString::fromStdString(err)));
    }
    return false;
  }

  m_currentAptFile = *fileOpt;
  auto& file = *m_currentAptFile;
  const auto& sum = file.summary;

  // Initialise frame navigation controls.
  m_aptCurrentFrameIndex = 0;
  {
    const int fc = static_cast<int>(file.frames.size());
    if (m_aptFrameSpin) {
      m_aptFrameSpin->blockSignals(true);
      m_aptFrameSpin->setRange(0, std::max(0, fc - 1));
      m_aptFrameSpin->setValue(0);
      m_aptFrameSpin->setEnabled(fc > 0);
      m_aptFrameSpin->blockSignals(false);
    }
    if (m_aptFrameCountLabel) m_aptFrameCountLabel->setText(QString(" / %1 ").arg(fc));
    if (m_aptPrevFrameAction) m_aptPrevFrameAction->setEnabled(false);
    if (m_aptNextFrameAction) m_aptNextFrameAction->setEnabled(fc > 1);
  }
  if (m_aptSaveAction)   m_aptSaveAction->setEnabled(true);
  if (m_aptExportAction) m_aptExportAction->setEnabled(true);
  if (m_aptBringForwardAction) m_aptBringForwardAction->setEnabled(true);
  if (m_aptSendBackwardAction) m_aptSendBackwardAction->setEnabled(true);
  if (m_aptAddPlacementAction) m_aptAddPlacementAction->setEnabled(true);
  if (m_aptRemovePlacementAction) m_aptRemovePlacementAction->setEnabled(true);
  if (m_aptDuplicatePlacementAction) m_aptDuplicatePlacementAction->setEnabled(true);

  m_aptTree->setUpdatesEnabled(false);

  auto addLeaf = [&](QTreeWidgetItem* parent, const QString& name, const QString& value, const QString& details,
                     int nodeType = kAptNodePlain, int index = -1,
                     int ownerKind = 0, int ownerIndex = -1, int placementIndex = -1) {
    auto* it = new QTreeWidgetItem(parent, QStringList() << name << value);
    it->setData(0, Qt::UserRole, details);
    it->setData(0, kAptRoleNodeType, nodeType);
    it->setData(0, kAptRoleNodeIndex, index);
    it->setData(0, kAptRoleOwnerKind, ownerKind);
    it->setData(0, kAptRoleOwnerIndex, ownerIndex);
    it->setData(0, kAptRolePlacementIndex, placementIndex);
    return it;
  };

  QString rootLabel = sourceLabel.trimmed();
  if (rootLabel.isEmpty()) rootLabel = QFileInfo(aptPath).fileName();
  auto* root = new QTreeWidgetItem(m_aptTree, QStringList() << rootLabel << "APT");
  root->setData(0, Qt::UserRole,
                QString("APT File: %1\nCONST File: %2")
                    .arg(QDir::toNativeSeparators(aptPath), QDir::toNativeSeparators(constPath)));
  root->setData(0, kAptRoleNodeType, kAptNodePlain);
  root->setData(0, kAptRoleNodeIndex, -1);

  auto* summaryNode = new QTreeWidgetItem(root, QStringList() << "Summary" << QString("%1x%2")
                                                                         .arg(sum.screensizex)
                                                                         .arg(sum.screensizey));
  summaryNode->setData(0, kAptRoleNodeType, kAptNodeSummary);
  summaryNode->setData(0, kAptRoleNodeIndex, -1);
  summaryNode->setData(0, Qt::UserRole,
                       QString("Screen Size: %1 x %2\nFrames: %3\nCharacters: %4\nImports: %5\nExports: %6\nMovie Data Offset: 0x%7")
                           .arg(sum.screensizex)
                           .arg(sum.screensizey)
                           .arg(sum.framecount)
                           .arg(sum.charactercount)
                           .arg(sum.importcount)
                           .arg(sum.exportcount)
                           .arg(QString::number(qulonglong(sum.aptdataoffset), 16).toUpper()));
  addLeaf(summaryNode, "Screen Width", QString::number(sum.screensizex), QString("Screen Width: %1").arg(sum.screensizex), kAptNodeSummary);
  addLeaf(summaryNode, "Screen Height", QString::number(sum.screensizey), QString("Screen Height: %1").arg(sum.screensizey), kAptNodeSummary);
  addLeaf(summaryNode, "Frame Count", QString::number(file.frames.size()), QString("Frames: %1").arg(file.frames.size()), kAptNodeSummary);
  addLeaf(summaryNode, "Character Count", QString::number(file.characters.size()), QString("Characters: %1").arg(file.characters.size()), kAptNodeSummary);
  addLeaf(summaryNode, "Import Count", QString::number(file.imports.size()), QString("Imports: %1").arg(file.imports.size()), kAptNodeSummary);
  addLeaf(summaryNode, "Export Count", QString::number(file.exports.size()), QString("Exports: %1").arg(file.exports.size()), kAptNodeSummary);
  addLeaf(summaryNode, "APT Data Offset", QString("0x%1").arg(QString::number(qulonglong(sum.aptdataoffset), 16).toUpper()),
          QString("APT Data Offset: 0x%1").arg(QString::number(qulonglong(sum.aptdataoffset), 16).toUpper()), kAptNodeSummary);

  auto* importsNode = new QTreeWidgetItem(root, QStringList() << QString("Imports (%1)").arg(file.imports.size()) << QString::number(file.imports.size()));
  importsNode->setData(0, Qt::UserRole, QString("Import table entries: %1").arg(file.imports.size()));
  importsNode->setData(0, kAptRoleNodeType, kAptNodePlain);
  importsNode->setData(0, kAptRoleNodeIndex, -1);
  for (std::size_t i = 0; i < file.imports.size(); ++i) {
    const auto& im = file.imports[i];
    const QString movie = QString::fromStdString(im.movie);
    const QString name = QString::fromStdString(im.name);
    const QString label = QString("%1: %2 :: %3").arg(qulonglong(i)).arg(movie.isEmpty() ? "(movie?)" : movie, name.isEmpty() ? "(name?)" : name);
    const QString detail = QString("Type: Import\nIndex: %1\nMovie: %2\nName: %3\nCharacter: %4\nOffset: 0x%5")
                               .arg(qulonglong(i))
                               .arg(movie.isEmpty() ? "(empty)" : movie)
                               .arg(name.isEmpty() ? "(empty)" : name)
                               .arg(im.character)
                               .arg(QString::number(qulonglong(im.offset), 16).toUpper());
    addLeaf(importsNode, label, QString::number(im.character), detail, kAptNodeImport, static_cast<int>(i));
  }

  auto* exportsNode = new QTreeWidgetItem(root, QStringList() << QString("Exports (%1)").arg(file.exports.size()) << QString::number(file.exports.size()));
  exportsNode->setData(0, Qt::UserRole, QString("Export table entries: %1").arg(file.exports.size()));
  exportsNode->setData(0, kAptRoleNodeType, kAptNodePlain);
  exportsNode->setData(0, kAptRoleNodeIndex, -1);
  for (std::size_t i = 0; i < file.exports.size(); ++i) {
    const auto& ex = file.exports[i];
    const QString name = QString::fromStdString(ex.name);
    const QString detail = QString("Type: Export\nIndex: %1\nName: %2\nCharacter: %3\nOffset: 0x%4")
                               .arg(qulonglong(i))
                               .arg(name.isEmpty() ? "(empty)" : name)
                               .arg(ex.character)
                               .arg(QString::number(qulonglong(ex.offset), 16).toUpper());
    addLeaf(exportsNode, QString("%1: %2").arg(qulonglong(i)).arg(name.isEmpty() ? "(empty)" : name), QString::number(ex.character), detail, kAptNodeExport, static_cast<int>(i));
  }

  auto* framesNode = new QTreeWidgetItem(root, QStringList() << QString("Frames (%1)").arg(file.frames.size()) << QString::number(file.frames.size()));
  framesNode->setData(0, Qt::UserRole, QString("Frame table entries: %1").arg(file.frames.size()));
  framesNode->setData(0, kAptRoleNodeType, kAptNodePlain);
  framesNode->setData(0, kAptRoleNodeIndex, -1);
  for (std::size_t i = 0; i < file.frames.size(); ++i) {
    const auto& fr = file.frames[i];
    // Extract FrameLabel if present (APT frames often have named labels).
    QString frameLabel;
    for (const auto& it : fr.items)
      if (it.kind == gf::apt::AptFrameItemKind::FrameLabel && !it.label.empty()) {
        frameLabel = QString::fromStdString(it.label);
        break;
      }
    const QString frameName = frameLabel.isEmpty()
        ? QString("Frame %1").arg(qulonglong(i))
        : QString("Frame %1 \"%2\"").arg(qulonglong(i)).arg(frameLabel);
    const QString detail = QString("Type: Frame\nIndex: %1\nLabel: %2\nFrame Item Count: %3\nPlacement Count: %4\nItems Offset: 0x%5\nOffset: 0x%6")
                               .arg(qulonglong(i))
                               .arg(frameLabel.isEmpty() ? "(none)" : frameLabel)
                               .arg(fr.frameitemcount)
                               .arg(fr.placements.size())
                               .arg(QString::number(qulonglong(fr.items_offset), 16).toUpper())
                               .arg(QString::number(qulonglong(fr.offset), 16).toUpper());
    auto* frameItem = addLeaf(framesNode, frameName, QString::number(fr.frameitemcount), detail, kAptNodeFrame, static_cast<int>(i), 0, -1);
    for (std::size_t p = 0; p < fr.placements.size(); ++p) {
      const auto& pl = fr.placements[p];
      addLeaf(frameItem, aptPlacementTreeLabel(pl), aptPlacementValueText(pl),
              aptPlacementDetailText(pl, "Root Movie", static_cast<int>(i), static_cast<int>(p)),
              kAptNodePlacement, static_cast<int>(i), 0, -1, static_cast<int>(p));
    }
  }

  const std::size_t localChCount = std::count_if(
      file.characters.begin(), file.characters.end(),
      [](const gf::apt::AptCharacter& c){ return c.type != 0; });
  auto* charsNode = new QTreeWidgetItem(root, QStringList() << QString("Characters (%1)").arg(localChCount) << QString::number(localChCount));
  charsNode->setData(0, Qt::UserRole, QString("Character table: %1 local, %2 total slots").arg(localChCount).arg(file.characters.size()));
  charsNode->setData(0, kAptRoleNodeType, kAptNodePlain);
  charsNode->setData(0, kAptRoleNodeIndex, -1);
  for (std::size_t i = 0; i < file.characters.size(); ++i) {
    const auto& ch = file.characters[i];
    const QString typeName = (ch.type == 0)
        ? QString("import slot")
        : QString::fromStdString(gf::apt::aptCharTypeName(ch.type));

    // Resolve import info for type=0 (import placeholder) slots.
    QString importInfo;
    if (ch.type == 0) {
      for (const auto& imp : file.imports) {
        if (imp.character == static_cast<std::uint32_t>(i)) {
          importInfo = QString("%1 :: %2").arg(
              QString::fromStdString(imp.movie),
              QString::fromStdString(imp.name));
          break;
        }
      }
      if (importInfo.isEmpty()) importInfo = "(unresolved import)";
    }

    // Resolve export linkage name for this character index.
    QString exportInfo;
    for (const auto& exp : file.exports) {
      if (exp.character == static_cast<std::uint32_t>(i)) {
        exportInfo = QString::fromStdString(exp.name);
        break;
      }
    }

    // Build bounds string if available.
    QString boundsStr;
    if (ch.bounds) {
      boundsStr = QString("L=%1 T=%2 R=%3 B=%4")
          .arg(ch.bounds->left, 0, 'f', 1)
          .arg(ch.bounds->top, 0, 'f', 1)
          .arg(ch.bounds->right, 0, 'f', 1)
          .arg(ch.bounds->bottom, 0, 'f', 1);
    }

    QString detail = QString("Type: Character\nIndex: %1\nCharacter Type: %2 (%3)\nSignature: 0x%4\nOffset: 0x%5\nNested Frames: %6")
                               .arg(qulonglong(i))
                               .arg(typeName)
                               .arg(ch.type)
                               .arg(QString::number(qulonglong(ch.signature), 16).toUpper())
                               .arg(QString::number(qulonglong(ch.offset), 16).toUpper())
                               .arg(ch.frames.size());
    if (!importInfo.isEmpty())  detail += QStringLiteral("\nImport: ") + importInfo;
    if (!exportInfo.isEmpty())  detail += QStringLiteral("\nExport: ") + exportInfo;
    if (!boundsStr.isEmpty())   detail += QStringLiteral("\nBounds: ") + boundsStr;
    auto* charItem = addLeaf(charsNode, QString("Character %1 — %2").arg(qulonglong(i)).arg(typeName), QString("type %1").arg(ch.type), detail, kAptNodeCharacter, static_cast<int>(i));
    if (!ch.frames.empty()) {
      auto* charFramesNode = new QTreeWidgetItem(charItem, QStringList() << QString("Frames (%1)").arg(ch.frames.size()) << QString::number(ch.frames.size()));
      charFramesNode->setData(0, Qt::UserRole, QString("Nested frames for Character %1").arg(qulonglong(i)));
      charFramesNode->setData(0, kAptRoleNodeType, kAptNodePlain);
      charFramesNode->setData(0, kAptRoleNodeIndex, -1);
      charFramesNode->setData(0, kAptRoleOwnerKind, 1);
      charFramesNode->setData(0, kAptRoleOwnerIndex, static_cast<int>(i));
      for (std::size_t fi = 0; fi < ch.frames.size(); ++fi) {
        const auto& fr = ch.frames[fi];
        QString chFrameLabel;
        for (const auto& it : fr.items)
          if (it.kind == gf::apt::AptFrameItemKind::FrameLabel && !it.label.empty()) {
            chFrameLabel = QString::fromStdString(it.label);
            break;
          }
        const QString chFrameName = chFrameLabel.isEmpty()
            ? QString("Frame %1").arg(qulonglong(fi))
            : QString("Frame %1 \"%2\"").arg(qulonglong(fi)).arg(chFrameLabel);
        const QString frameDetail = QString("Type: Frame\nOwner: Character %1\nIndex: %2\nLabel: %3\nFrame Item Count: %4\nPlacement Count: %5\nItems Offset: 0x%6\nOffset: 0x%7")
                                        .arg(qulonglong(i))
                                        .arg(qulonglong(fi))
                                        .arg(chFrameLabel.isEmpty() ? "(none)" : chFrameLabel)
                                        .arg(fr.frameitemcount)
                                        .arg(fr.placements.size())
                                        .arg(QString::number(qulonglong(fr.items_offset), 16).toUpper())
                                        .arg(QString::number(qulonglong(fr.offset), 16).toUpper());
        auto* frameItem = addLeaf(charFramesNode, chFrameName, QString::number(fr.frameitemcount),
                                  frameDetail, kAptNodeFrame, static_cast<int>(fi), 1, static_cast<int>(i));
        for (std::size_t p = 0; p < fr.placements.size(); ++p) {
          const auto& pl = fr.placements[p];
          addLeaf(frameItem, aptPlacementTreeLabel(pl), aptPlacementValueText(pl),
                  aptPlacementDetailText(pl, QString("Character %1").arg(qulonglong(i)), static_cast<int>(fi), static_cast<int>(p)),
                  kAptNodePlacement, static_cast<int>(fi), 1, static_cast<int>(i), static_cast<int>(p));
        }
      }
    }
  }

  auto* slicesNode = new QTreeWidgetItem(root, QStringList() << QString("Slices (%1)").arg(file.slices.size()) << QString::number(file.slices.size()));
  slicesNode->setData(0, Qt::UserRole, QString("APT file slices: %1").arg(file.slices.size()));
  slicesNode->setData(0, kAptRoleNodeType, kAptNodePlain);
  slicesNode->setData(0, kAptRoleNodeIndex, -1);
  for (std::size_t i = 0; i < file.slices.size(); ++i) {
    const auto& sl = file.slices[i];
    const QString name = QString::fromStdString(sl.name);
    const QString detail = QString("Type: Slice\nIndex: %1\nName: %2\nOffset: 0x%3\nSize: %4 bytes")
                               .arg(qulonglong(i))
                               .arg(name)
                               .arg(QString::number(qulonglong(sl.offset), 16).toUpper())
                               .arg(qulonglong(sl.size));
    addLeaf(slicesNode, name, QString::number(qulonglong(sl.size)), detail, kAptNodeSlice, static_cast<int>(i));
  }

  root->setExpanded(true);
  summaryNode->setExpanded(true);
  importsNode->setExpanded(true);
  exportsNode->setExpanded(true);
  framesNode->setExpanded(true);
  charsNode->setExpanded(true);
  slicesNode->setExpanded(true);

  m_aptTree->setUpdatesEnabled(true);
  m_aptTree->setCurrentItem(summaryNode);
  if (m_aptDetails) m_aptDetails->setPlainText(summaryNode->data(0, Qt::UserRole).toString());
  refreshAptPreview();
  return true;
}

std::optional<int> MainWindow::selectedAptFrameIndex() const {
  if (!m_aptTree || !m_currentAptFile) return std::nullopt;
  auto* item = m_aptTree->currentItem();
  if (!item) return std::nullopt;

  const int nodeType = item->data(0, kAptRoleNodeType).toInt();
  const int frameIndex = item->data(0, kAptRoleNodeIndex).toInt();
  const int ownerKind = item->data(0, kAptRoleOwnerKind).toInt();

  if (ownerKind != 0) return std::nullopt;
  if ((nodeType == kAptNodeFrame || nodeType == kAptNodePlacement) &&
      frameIndex >= 0 && frameIndex < static_cast<int>(m_currentAptFile->frames.size())) {
    return frameIndex;
  }
  return std::nullopt;
}

std::optional<int> MainWindow::selectedAptPlacementIndex() const {
  if (!m_aptTree || !m_currentAptFile) return std::nullopt;
  auto* item = m_aptTree->currentItem();
  if (!item) return std::nullopt;

  const int nodeType = item->data(0, kAptRoleNodeType).toInt();
  const int ownerKind = item->data(0, kAptRoleOwnerKind).toInt();
  const int placementIndex = item->data(0, kAptRolePlacementIndex).toInt();

  if (nodeType != kAptNodePlacement || placementIndex < 0) return std::nullopt;
  // Valid in both root-movie context (ownerKind==0) and character context (ownerKind==1).
  return placementIndex;
}

// Returns a validated canvas size from APT summary dimensions.
// Falls back to 1280x720 if dimensions are zero, 0xFFFFFFFF, or exceed 8192.
static std::pair<qreal, qreal> resolveAptPreviewCanvasSize(const gf::apt::AptSummary& summary) {
  constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;
  constexpr std::uint32_t kMaxSize = 8192u;
  const std::uint32_t w = summary.screensizex;
  const std::uint32_t h = summary.screensizey;
  const qreal rw = (w > 0 && w != kInvalid && w <= kMaxSize) ? static_cast<qreal>(w) : 1280.0;
  const qreal rh = (h > 0 && h != kInvalid && h <= kMaxSize) ? static_cast<qreal>(h) : 720.0;
  return {rw, rh};
}

// Bridge: Transform2D (library) → QTransform (Qt).
// Flash column-vector math: x'=a*x+c*y+tx, y'=b*x+d*y+ty
// QTransform(m11,m12,m21,m22,dx,dy): x'=m11*x+m21*y+dx, y'=m12*x+m22*y+dy
// → m11=a, m12=b, m21=c, m22=d, dx=tx, dy=ty.
static QTransform transform2DToQt(const gf::apt::Transform2D& t) {
  return QTransform(
      static_cast<qreal>(t.a),  // m11
      static_cast<qreal>(t.b),  // m12
      static_cast<qreal>(t.c),  // m21
      static_cast<qreal>(t.d),  // m22
      static_cast<qreal>(t.tx), // dx
      static_cast<qreal>(t.ty)  // dy
  );
}

// Recursively logs the sprite/movie sub-tree rooted at resolvedFrame.
// Called only when spdlog debug logging is active (debug overlay mode).
static void logAptSpriteTree(
    const gf::apt::AptFile&                   aptFile,
    const std::vector<gf::apt::AptCharacter>& table,
    const gf::apt::AptFrame&                  resolvedFrame,
    int                                        depth,
    const std::shared_ptr<spdlog::logger>&     lg)
{
  if (depth > 8) return;
  const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
  for (const auto& pl : resolvedFrame.placements) {
    if (pl.character >= table.size()) continue;
    const auto& ch = table[pl.character];
    if (ch.type != 5 && ch.type != 9) continue;  // only Sprite / Movie
    const char* typeName = (ch.type == 9) ? "Movie" : "Sprite";
    lg->debug("[APT] {}render charId={} type={} depth={}", indent, pl.character, typeName, pl.depth);
    if (ch.frames.empty()) {
      lg->debug("[APT] {}  {} has no frames (placeholder)", indent, typeName);
      continue;
    }
    const std::vector<gf::apt::AptCharacter>& childTable =
        (ch.type == 9 && !ch.nested_characters.empty()) ? ch.nested_characters : table;
    const gf::apt::AptFrame childDl =
        gf::apt::build_display_list_frame(ch.frames, 0);
    lg->debug("[APT] {}  entering {} timeline frame=0 nodes={}", indent, typeName, childDl.placements.size());
    logAptSpriteTree(aptFile, childTable, childDl, depth + 1, lg);
  }
  (void)aptFile; // reserved for future use (e.g. import resolution)
}

// ---------------------------------------------------------------------------
// drawRenderNodeToScene — converts one RenderNode to QGraphicsItems.
//
// Label strategy (P1):
//   best name = symbolName  (export/import symbol, e.g. "Screen")
//             → instanceName (APT placement name, e.g. "bg")
//             → "C{charId}"  (fallback)
//   Prefix shows charId + best name: "C119 Screen", "C42 bg", "C7"
//   importRef shown for cross-file imports: "import ui_common/CLOCK_PANEL"
//
// Tooltips (P2): each primary rect/poly item gets a rich tooltip.
// ---------------------------------------------------------------------------
static void drawRenderNodeToScene(
    const gf::apt::RenderNode& node,
    int highlightRootPlacementIdx,
    bool debugOverlay,
    QGraphicsScene* scene)
{
  const bool isHighlighted = (node.rootPlacementIndex == highlightRootPlacementIdx
                              && highlightRootPlacementIdx >= 0);
  const QString chainLabel = QString::fromStdString(node.parentChainLabel);
  const int nestingDepth = chainLabel.count(QStringLiteral("\u2192"));
  const bool isRootLevel = (nestingDepth == 0);

  // ── Best human-readable name for this node ────────────────────────────────
  const QString symName   = QString::fromStdString(node.symbolName);
  const QString instName  = QString::fromStdString(node.instanceName);
  const QString impRef    = QString::fromStdString(node.importRef);

  // Short label: instance name takes priority (Ion-debugger style),
  // then symbol/export name, then bare charId.
  // Format: "mcTitle (C42)"  /  "Screen (C42)"  /  "C42"
  const QString shortLabel = !instName.isEmpty()
      ? QString("%1 (C%2)").arg(instName).arg(node.characterId)
      : !symName.isEmpty()
      ? QString("%1 (C%2)").arg(symName).arg(node.characterId)
      : QString("C%1").arg(node.characterId);

  // ── Tooltip builder ───────────────────────────────────────────────────────
  auto makeTooltip = [&](const char* kindStr) -> QString {
    QString tt = QString("<b>C%1</b> — %2").arg(node.characterId).arg(QLatin1String(kindStr));
    if (!symName.isEmpty())
      tt += QString("<br>symbol: <b>%1</b>").arg(symName.toHtmlEscaped());
    if (!instName.isEmpty() && instName != symName)
      tt += QString("<br>instance: %1").arg(instName.toHtmlEscaped());
    if (!impRef.isEmpty())
      tt += QString("<br>import: %1").arg(impRef.toHtmlEscaped());
    if (node.placementDepth > 0)
      tt += QString("<br>depth: %1").arg(node.placementDepth);
    if (node.localBounds) {
      const auto& b = *node.localBounds;
      tt += QString("<br>bounds: %1×%2")
          .arg(static_cast<int>(b.right - b.left))
          .arg(static_cast<int>(b.bottom - b.top));
    }
    if (debugOverlay && !chainLabel.isEmpty())
      tt += QString("<br><small>%1</small>").arg(chainLabel.toHtmlEscaped());
    return tt;
  };

  // ── Unknown / import placeholder ──────────────────────────────────────────
  if (node.kind == gf::apt::RenderNode::Kind::Unknown) {
    const QTransform wt = transform2DToQt(node.worldTransform);
    const QPointF origin = wt.map(QPointF(0.0, 0.0));
    const QColor impCol = isHighlighted ? QColor(255, 210, 64) : QColor(255, 140, 0);
    const QPen   impPen(QColor(impCol.red(), impCol.green(), impCol.blue(), isHighlighted ? 255 : 200),
                        isHighlighted ? 2.5 : 1.5, Qt::DashLine);
    const QBrush impBrush(QColor(impCol.red(), impCol.green(), impCol.blue(), isHighlighted ? 40 : 22));
    const QRectF impRect(0.0, 0.0, 120.0, 40.0);
    auto* impItem = scene->addRect(impRect, impPen, impBrush);
    impItem->setTransform(wt);
    impItem->setToolTip(makeTooltip("import placeholder"));

    // Label: "C20 import ui/CLOCK" or "C20 import"
    QString lblText = impRef.isEmpty()
        ? QString("C%1\nimport").arg(node.characterId)
        : QString("C%1\nimport %2").arg(node.characterId).arg(impRef);
    if (!symName.isEmpty() && impRef.isEmpty())
      lblText = QString("C%1 %2\nimport").arg(node.characterId).arg(symName);
    auto* lbl = scene->addSimpleText(lblText);
    QFont f = lbl->font(); f.setPointSizeF(7.5); lbl->setFont(f);
    lbl->setPos(origin.x() + 3.0, origin.y() + 3.0);
    lbl->setBrush(QBrush(QColor(impCol.red(), impCol.green(), impCol.blue(),
                                isHighlighted ? 255 : 210)));
    return;
  }

  // ── Sprite / Movie (container) ────────────────────────────────────────────
  if (node.kind == gf::apt::RenderNode::Kind::Sprite ||
      node.kind == gf::apt::RenderNode::Kind::Movie) {
    const bool isMovie = (node.kind == gf::apt::RenderNode::Kind::Movie);
    const QTransform wt = transform2DToQt(node.worldTransform);
    const QPointF origin = wt.map(QPointF(0.0, 0.0));
    const QColor col = isHighlighted ? QColor(255, 210, 64)
                     : isMovie        ? QColor( 90, 155, 255, 220)
                                      : QColor(255, 135,  40, 220);
    const QPen   sprPen  (col, isHighlighted ? 2.0 : 1.5, Qt::DotLine);
    const QBrush sprBrush(QColor(col.red(), col.green(), col.blue(), isHighlighted ? 40 : 18));

    // Label: "C119 Screen" (short, no type tag unless debug)
    const QString typeTag = isMovie ? QStringLiteral("MOV") : QStringLiteral("SPR");
    const QString lblMain = debugOverlay
        ? QString("[%1] %2").arg(typeTag, shortLabel)
        : shortLabel;

    bool drewRect = false;
    if (node.localBounds) {
      const gf::apt::AptBounds& b = *node.localBounds;
      const qreal bw = static_cast<qreal>(b.right  - b.left);
      const qreal bh = static_cast<qreal>(b.bottom - b.top);
      if (bw > 0.0 && bh > 0.0) {
        const QRectF boundsRect(static_cast<qreal>(b.left), static_cast<qreal>(b.top), bw, bh);
        auto* bItem = scene->addRect(boundsRect, sprPen, sprBrush);
        bItem->setTransform(wt);
        bItem->setToolTip(makeTooltip(isMovie ? "Movie" : "Sprite"));
        const QPen xhPen(QColor(col.red(), col.green(), col.blue(), 140), 1.0);
        scene->addLine(origin.x() - 4, origin.y(), origin.x() + 4, origin.y(), xhPen);
        scene->addLine(origin.x(), origin.y() - 4, origin.x(), origin.y() + 4, xhPen);
        const QPointF tl = wt.map(QPointF(b.left, b.top));
        auto* lblItem = scene->addSimpleText(lblMain);
        QFont f = lblItem->font(); f.setPointSizeF(7.5); lblItem->setFont(f);
        lblItem->setPos(tl.x() + 3.0, tl.y() + 3.0);
        lblItem->setBrush(QBrush(col));
        drewRect = true;
      }
    }

    if (!drewRect) {
      const qreal r = isHighlighted ? 10.0 : 6.0;
      QPolygonF diamond;
      diamond << QPointF(0, -r) << QPointF(r, 0) << QPointF(0, r) << QPointF(-r, 0);
      auto* diamItem = scene->addPolygon(
          diamond,
          QPen(col, isHighlighted ? 2.0 : 1.5),
          QBrush(QColor(col.red(), col.green(), col.blue(), isHighlighted ? 60 : 28)));
      diamItem->setPos(origin);
      diamItem->setToolTip(makeTooltip(isMovie ? "Movie" : "Sprite"));
      auto* lblItem = scene->addSimpleText(lblMain);
      QFont f = lblItem->font(); f.setPointSizeF(7.5); lblItem->setFont(f);
      lblItem->setPos(origin + QPointF(r + 2.0, -r));
      lblItem->setBrush(QBrush(col));
    }
    return;
  }

  // ── Leaf node (Shape / Image / EditText / Button) ─────────────────────────
  QRectF localRect;
  if (node.localBounds) {
    const gf::apt::AptBounds& b = *node.localBounds;
    const qreal bw = static_cast<qreal>(b.right  - b.left);
    const qreal bh = static_cast<qreal>(b.bottom - b.top);
    if (bw > 0.0 && bh > 0.0)
      localRect = QRectF(static_cast<qreal>(b.left), static_cast<qreal>(b.top), bw, bh);
  }
  if (!localRect.isValid() || localRect.width() < 1.0 || localRect.height() < 1.0)
    localRect = QRectF(0.0, 0.0, 120.0, 48.0);

  const bool isImage    = (node.kind == gf::apt::RenderNode::Kind::Image);
  const bool isShape    = (node.kind == gf::apt::RenderNode::Kind::Shape);
  const bool isEditText = (node.kind == gf::apt::RenderNode::Kind::EditText);

  QPen pen;
  QBrush brush;
  const char* kindStr = "leaf";
  if (isHighlighted) {
    pen   = QPen(QColor(255, 210,  64), 3.0);
    brush = QBrush(QColor(255, 210,  64, 70));
    kindStr = "selected";
  } else if (isImage) {
    // Image: purple — often a texture/sprite sheet tile
    pen   = QPen(QColor(190, 120, 255, isRootLevel ? 230 : 170), isRootLevel ? 2.0 : 1.0);
    brush = QBrush(QColor(150,  80, 255, isRootLevel ? 55 : 28));
    kindStr = "Image";
  } else if (isShape) {
    // Shape: teal — geometry
    pen   = QPen(QColor( 60, 200, 180, isRootLevel ? 230 : 160), isRootLevel ? 1.5 : 1.0);
    brush = QBrush(QColor( 40, 170, 150, isRootLevel ? 45 : 22));
    kindStr = "Shape";
  } else if (isEditText) {
    // EditText: yellow-green — text field
    pen   = QPen(QColor(180, 230,  80, isRootLevel ? 230 : 160), isRootLevel ? 1.5 : 1.0);
    brush = QBrush(QColor(150, 200,  60, isRootLevel ? 40 : 18));
    kindStr = "EditText";
  } else if (isRootLevel) {
    pen   = QPen(QColor(130, 185, 255), 2.0);
    brush = QBrush(QColor( 90, 160, 255, 45));
    kindStr = "leaf";
  } else {
    pen   = QPen(QColor( 80, 210, 160, 170), 1.0);
    brush = QBrush(QColor( 60, 180, 130, 22));
    kindStr = "leaf";
  }

  const QTransform wt = transform2DToQt(node.worldTransform);
  auto* rectItem = scene->addRect(localRect, pen, brush);
  rectItem->setTransform(wt);
  rectItem->setToolTip(makeTooltip(kindStr));

  // Labels: always show on root-level items; show in debug mode for nested.
  // In normal mode, nested items only show a label if they have a name.
  const bool showLabel = isRootLevel || debugOverlay
                      || !symName.isEmpty() || !instName.isEmpty();
  if (showLabel) {
    // Normal mode: "C42 Screen" or "C42 bg" — compact, no depth spam
    // Debug mode: adds depth/chain info
    QString label = shortLabel;
    if (debugOverlay)
      label += QString("\nD%1").arg(node.placementDepth);
    if (debugOverlay && !chainLabel.isEmpty())
      label += QString("\n%1").arg(chainLabel);

    const QPointF anchor = wt.map(localRect.topLeft());
    auto* textItem = scene->addSimpleText(label);
    QFont f = textItem->font(); f.setPointSizeF(7.5); textItem->setFont(f);
    textItem->setPos(anchor.x() + 4.0, anchor.y() + 4.0);
    textItem->setBrush(QBrush(isHighlighted ? QColor(255, 235, 120)
                            : isRootLevel   ? QColor(220, 228, 255)
                                            : QColor(185, 240, 210, 220)));
  }
}

static void renderNodesToScene(
    const std::vector<gf::apt::RenderNode>& nodes,
    int highlightRootPlacementIdx,
    bool debugOverlay,
    QGraphicsScene* scene)
{
  for (const gf::apt::RenderNode& node : nodes)
    drawRenderNodeToScene(node, highlightRootPlacementIdx, debugOverlay, scene);
}

// ---------------------------------------------------------------------------
// DAT resolver + APT fallback renderer
// ---------------------------------------------------------------------------

QString MainWindow::buildDlSummaryText(const std::vector<gf::apt::AptFrame>& frames,
                                       std::size_t frameIndex,
                                       const QString& contextLabel) const
{
  QStringList lines;
  lines << QStringLiteral("Context: %1").arg(contextLabel.isEmpty() ? QStringLiteral("APT") : contextLabel);
  lines << QStringLiteral("Frame: %1 / %2")
               .arg(static_cast<qulonglong>(frameIndex))
               .arg(static_cast<qulonglong>(frames.size()));

  if (frames.empty()) {
    lines << QStringLiteral("No frames.");
    return lines.join(QLatin1Char('\n'));
  }

  if (frameIndex >= frames.size()) {
    lines << QStringLiteral("Frame index out of range.");
    return lines.join(QLatin1Char('\n'));
  }

  const gf::apt::AptFrame resolved = gf::apt::build_display_list_frame(frames, frameIndex);
  lines << QStringLiteral("Display list nodes: %1").arg(resolved.placements.size());

  if (resolved.placements.empty()) {
    lines << QStringLiteral("(empty)");
    return lines.join(QLatin1Char('\n'));
  }

  std::vector<std::size_t> order(resolved.placements.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return resolved.placements[a].depth < resolved.placements[b].depth;
  });

  for (const std::size_t idx : order) {
    const auto& pl = resolved.placements[idx];
    lines << QStringLiteral("[%1] depth=%2 charId=%3 name=%4 tx=(%5,%6) m=[%7 %8; %9 %10]")
                 .arg(static_cast<qulonglong>(idx))
                 .arg(pl.depth)
                 .arg(pl.character)
                 .arg(pl.instance_name.empty()
                          ? QStringLiteral("-")
                          : QString::fromStdString(pl.instance_name))
                 .arg(pl.transform.x, 0, 'f', 2)
                 .arg(pl.transform.y, 0, 'f', 2)
                 .arg(pl.transform.scale_x, 0, 'f', 3)
                 .arg(pl.transform.rotate_skew_1, 0, 'f', 3)
                 .arg(pl.transform.rotate_skew_0, 0, 'f', 3)
                 .arg(pl.transform.scale_y, 0, 'f', 3);
  }

  return lines.join(QLatin1Char('\n'));
}

void MainWindow::renderDatImageToScene(int imageIndex)
{
  if (!m_datPreviewScene) return;

  m_datPreviewScene->clear();

  if (!m_currentDatFile) {
    if (m_datEntryInfoLabel) m_datEntryInfoLabel->setText(QStringLiteral("(no DAT loaded)"));
    return;
  }

  const auto& images = m_currentDatFile->images;
  if (imageIndex < 0 || imageIndex >= static_cast<int>(images.size())) {
    if (m_datEntryInfoLabel) m_datEntryInfoLabel->setText(QStringLiteral("(select a row to preview)"));
    return;
  }

  const auto& img = images[static_cast<std::size_t>(imageIndex)];
  const bool applyTransform = m_datApplyTransformCheck && m_datApplyTransformCheck->isChecked();
  const QColor fill(img.color_r, img.color_g, img.color_b, std::max<int>(img.color_a, 24));
  const QColor stroke(std::min<int>(img.color_r + 24, 255),
                      std::min<int>(img.color_g + 24, 255),
                      std::min<int>(img.color_b + 24, 255),
                      230);
  const QPen pen(stroke, 0.0);
  const QBrush brush(fill);

  QTransform datTransform(static_cast<qreal>(img.matrix_m00),
                          static_cast<qreal>(img.matrix_m10),
                          static_cast<qreal>(img.matrix_m01),
                          static_cast<qreal>(img.matrix_m11),
                          static_cast<qreal>(img.offset_x),
                          static_cast<qreal>(img.offset_y));

  QRectF sceneBounds;
  bool haveBounds = false;

  for (const auto& tri : img.triangles) {
    QPolygonF poly;
    for (int v = 0; v < 3; ++v) {
      QPointF pt(static_cast<qreal>(tri.v[v].x), static_cast<qreal>(tri.v[v].y));
      if (applyTransform) pt = datTransform.map(pt);
      poly << pt;
    }
    auto* item = m_datPreviewScene->addPolygon(poly, pen, brush);
    item->setZValue(1.0);
    const QRectF br = item->sceneBoundingRect();
    sceneBounds = haveBounds ? sceneBounds.united(br) : br;
    haveBounds = true;
  }

  const QPointF origin = applyTransform
      ? datTransform.map(QPointF(0.0, 0.0))
      : QPointF(0.0, 0.0);
  const QPen axisPen(QColor(255, 255, 255, 160), 0.0, Qt::DashLine);
  m_datPreviewScene->addLine(origin.x() - 8.0, origin.y(), origin.x() + 8.0, origin.y(), axisPen)->setZValue(2.0);
  m_datPreviewScene->addLine(origin.x(), origin.y() - 8.0, origin.x(), origin.y() + 8.0, axisPen)->setZValue(2.0);

  auto* label = m_datPreviewScene->addSimpleText(QStringLiteral("DAT #%1 / charId=%2")
                                                     .arg(imageIndex)
                                                     .arg(img.charId));
  label->setBrush(QBrush(QColor(230, 230, 240)));
  label->setPos(origin + QPointF(6.0, -label->boundingRect().height() - 4.0));
  label->setZValue(3.0);
  const QRectF labelBr = label->sceneBoundingRect();
  sceneBounds = haveBounds ? sceneBounds.united(labelBr) : labelBr;
  haveBounds = true;

  if (!haveBounds || !sceneBounds.isValid() || sceneBounds.isEmpty())
    sceneBounds = QRectF(-32.0, -32.0, 64.0, 64.0);

  sceneBounds = sceneBounds.adjusted(-24.0, -24.0, 24.0, 24.0);
  m_datPreviewScene->setSceneRect(sceneBounds);
  if (m_datPreviewView) m_datPreviewView->fitInView(sceneBounds, Qt::KeepAspectRatio);

  if (m_datEntryInfoLabel) {
    m_datEntryInfoLabel->setText(
        QStringLiteral("Row: %1\nCharId: %2\nTriangles: %3\nRGBA: (%4, %5, %6, %7)\n"
                       "Matrix: [%8 %9; %10 %11]\nOffset: (%12, %13)\nFile offset: 0x%14")
            .arg(imageIndex)
            .arg(img.charId)
            .arg(img.triangles.size())
            .arg(img.color_r).arg(img.color_g).arg(img.color_b).arg(img.color_a)
            .arg(img.matrix_m00, 0, 'f', 3)
            .arg(img.matrix_m01, 0, 'f', 3)
            .arg(img.matrix_m10, 0, 'f', 3)
            .arg(img.matrix_m11, 0, 'f', 3)
            .arg(img.offset_x, 0, 'f', 2)
            .arg(img.offset_y, 0, 'f', 2)
            .arg(img.file_offset, 0, 16));
  }
}

const gf::dat::DatImage* MainWindow::findDatImageByCharId(std::uint32_t charId) const noexcept
{
  if (!m_currentDatFile) return nullptr;
  for (const auto& img : m_currentDatFile->images)
    if (img.charId == charId) return &img;
  return nullptr;
}

void MainWindow::renderDatGeometryToAptScene(const gf::dat::DatImage& img,
                                              const QTransform& worldTransform,
                                              QGraphicsScene* scene,
                                              bool debugOverlay) const
{
  // Render DAT triangles using an APT world transform (the placement transform
  // from the APT character table).  The DAT entry's own matrix/offset is NOT
  // applied here because the APT placement transform is assumed to already
  // encode the final screen position.  This is a debug fallback; accuracy is
  // uncertain until the coordinate system is fully confirmed.

  if (img.triangles.empty()) return;

  // Diagnostic fill: cyan/teal with dashed white outline — visually distinct
  // from native APT rendering so fallback geometry is immediately recognisable.
  const QColor datFill(0, 200, 200, 60);
  const QPen   datPen(QColor(0, 220, 220, 200), 0, Qt::DashLine);

  for (const auto& tri : img.triangles) {
    QPolygonF local;
    for (int v = 0; v < 3; ++v)
      local << QPointF(static_cast<qreal>(tri.v[v].x),
                       static_cast<qreal>(tri.v[v].y));
    const QPolygonF world = worldTransform.map(local);
    scene->addPolygon(world, datPen, QBrush(datFill));
  }

  if (debugOverlay) {
    // Label placed at origin of the world transform
    const QPointF origin = worldTransform.map(QPointF(0.0, 0.0));
    auto* lbl = scene->addSimpleText(
        QStringLiteral("DAT charId=%1 (%2 tri)").arg(img.charId).arg(img.triangles.size()));
    lbl->setBrush(QBrush(QColor(0, 220, 200)));
    lbl->setPos(origin + QPointF(4.0, -lbl->boundingRect().height() - 4.0));
  }
}


// ── RatNode ─ Runtime Attach Tree node ────────────────────────────────────
// Represents one reconstructed child relationship (attachMovie, duplicateMovieClip, etc.)
// discovered by parsing AVM1 action bytecode. Defined at file scope (not inside a lambda)
// to avoid GCC local-struct-in-lambda template instantiation restrictions.
namespace {
struct RatNode {
  enum class Src : std::uint8_t {
    AttachMovie, DuplicateMovieClip, CreateEmpty, Import, HeuristicFallback
  };
  static constexpr std::uint32_t kUnresolved = UINT32_MAX;

  std::uint32_t charId        = kUnresolved;   // resolved charId; kUnresolved = not found
  std::string   symbolName;                    // symbol/export name used in the attach call
  std::string   instanceName;                  // as= instance name
  std::string   importRef;                     // "movie/name" for cross-file imports
  Src           source        = Src::HeuristicFallback;
  int           tier          = 3;             // 0=leaf 1=framed 2=runtime-ctr 3=import
  int           confidence    = 0;             // 0=low 1=medium 2=high
  bool          resolvedLocally = true;        // false = symbol resolved via import table

  int           depth         = -1;            // recovered AVM1 depth (-1 = unknown)

  // Recovered property-assignment flags + values.
  // Boolean flags are set from MemberName-role strings (presence only).
  // Value fields are set when a numeric constant was also recovered from the
  // push-then-SetMember pattern in the bytecode. NaN means value not known.
  static constexpr double kNoVal = std::numeric_limits<double>::quiet_NaN();
  bool   propXAssigned        = false;
  double propXValue           = kNoVal;
  bool   propYAssigned        = false;
  double propYValue           = kNoVal;
  bool   propAlphaAssigned    = false;
  double propAlphaValue       = kNoVal;
  bool   propVisibleAssigned  = false;
  bool   propScaleAssigned    = false;
  double propScaleValue       = kNoVal;
  bool   propRotationAssigned = false;
  double propRotValue         = kNoVal;

  std::string gotoLabel;      // first gotoAndStop/gotoAndPlay label seen near this node
  bool hasSubActions = false;  // a setTarget path pointed at instanceName

  bool hasX() const { return propXAssigned && !std::isnan(propXValue); }
  bool hasY() const { return propYAssigned && !std::isnan(propYValue); }
};
} // anonymous namespace

// ---------------------------------------------------------------------------
// getCachedAptHints — builds and caches merged AptActionHints for a charId.
// Scans:
//   1. All Action/InitAction byte-blocks on the character's own timeline.
//   2. All root-movie InitAction byte-blocks that target this charId.
// Results are stored in m_aptHintsCache[charId].  The cache is cleared
// whenever a new APT file is loaded (clearAptViewer → m_aptHintsCache.clear()).
// ---------------------------------------------------------------------------
const gf::apt::AptActionHints& MainWindow::getCachedAptHints(
    std::uint32_t charId,
    const gf::apt::AptCharacter& ch,
    const std::vector<gf::apt::AptCharacter>& /*characterTable*/)
{
  auto it = m_aptHintsCache.find(charId);
  if (it != m_aptHintsCache.end()) return it->second;

  gf::apt::AptActionHints merged;

  if (m_currentAptFile && !m_currentAptFile->original_apt.empty()) {
    const auto& aptBuf   = m_currentAptFile->original_apt;
    const auto& constBuf = m_currentAptFile->original_const;

    auto merge = [&](const gf::apt::AptActionHints& h) {
      // Merge strings (de-dup by value).
      for (const auto& s : h.strings) {
        bool dup = false;
        for (const auto& e : merged.strings) if (e.value == s.value) { dup = true; break; }
        if (!dup) merged.strings.push_back(s);
      }
      // Merge detected_calls (de-dup by call_name + first arg).
      for (const auto& dc : h.detected_calls) {
        bool dup = false;
        for (const auto& e : merged.detected_calls) {
          if (e.call_name == dc.call_name &&
              ((e.arg_strings.empty() && dc.arg_strings.empty()) ||
               (!e.arg_strings.empty() && !dc.arg_strings.empty() &&
                e.arg_strings[0] == dc.arg_strings[0]))) {
            dup = true; break;
          }
        }
        if (!dup) merged.detected_calls.push_back(dc);
      }
      // Merge set_members (de-dup by property+target).
      for (const auto& sm : h.set_members) {
        bool dup = false;
        for (const auto& e : merged.set_members)
          if (e.property == sm.property && e.target == sm.target) { dup = true; break; }
        if (!dup) merged.set_members.push_back(sm);
      }
      if (h.has_call_patterns)   merged.has_call_patterns   = true;
      if (h.likely_attach_movie) merged.likely_attach_movie  = true;
      merged.opcode_count += h.opcode_count;
    };

    // Own timeline Action/InitAction items.
    for (const auto& f : ch.frames)
      for (const auto& item : f.items)
        if (item.action_bytes_offset)
          merge(gf::apt::inspect_apt_actions(aptBuf, item.action_bytes_offset, constBuf));

    // Root-movie InitAction items targeting this sprite.
    for (const auto& f : m_currentAptFile->frames)
      for (const auto& item : f.items)
        if (item.kind == gf::apt::AptFrameItemKind::InitAction
            && item.init_sprite == charId && item.action_bytes_offset)
          merge(gf::apt::inspect_apt_actions(aptBuf, item.action_bytes_offset, constBuf));
  }

  auto [ins, _] = m_aptHintsCache.emplace(charId, std::move(merged));
  return ins->second;
}

void MainWindow::renderAptCharacterRecursive(std::uint32_t charId,
                                             const QTransform& parentTransform,
                                             const std::vector<gf::apt::AptCharacter>& characterTable,
                                             int rootPlacementIndex,
                                             int highlightRootPlacementIdx,
                                             bool debugOverlay,
                                             int recursionDepth,
                                             const QString& parentChainLabel)
{
  constexpr int kMaxRecursionDepth = 16;
  if (!m_currentAptFile || !m_aptPreviewScene) return;

  auto resolveCharacter = [&](std::uint32_t cid) -> const gf::apt::AptCharacter* {
    if (cid < characterTable.size() && characterTable[cid].type != 0)
      return &characterTable[cid];
    if (&characterTable != &m_currentAptFile->characters
        && cid < m_currentAptFile->characters.size()
        && m_currentAptFile->characters[cid].type != 0)
      return &m_currentAptFile->characters[cid];
    return nullptr;
  };

  // ── Export/import name resolver (P1) ─────────────────────────────────────
  // Returns {symbolName, importRef} for a charId using the current APT file.
  auto resolveNames = [&](std::uint32_t cid)
      -> std::pair<std::string, std::string> {
    if (!m_currentAptFile) return {};
    for (const auto& ex : m_currentAptFile->exports)
      if (ex.character == cid) return {ex.name, {}};
    for (const auto& im : m_currentAptFile->imports)
      if (im.character == cid) return {im.name, im.movie + "/" + im.name};
    return {};
  };

  const gf::apt::AptCharacter* ch = resolveCharacter(charId);
  if (!ch) {
    gf::apt::RenderNode unknown;
    unknown.kind = gf::apt::RenderNode::Kind::Unknown;
    unknown.characterId = charId;
    unknown.worldTransform = gf::apt::Transform2D::identity();
    unknown.worldTransform.a = static_cast<float>(parentTransform.m11());
    unknown.worldTransform.b = static_cast<float>(parentTransform.m12());
    unknown.worldTransform.c = static_cast<float>(parentTransform.m21());
    unknown.worldTransform.d = static_cast<float>(parentTransform.m22());
    unknown.worldTransform.tx = static_cast<float>(parentTransform.dx());
    unknown.worldTransform.ty = static_cast<float>(parentTransform.dy());
    unknown.rootPlacementIndex = rootPlacementIndex;
    unknown.parentChainLabel = parentChainLabel.toStdString();
    std::tie(unknown.symbolName, unknown.importRef) = resolveNames(charId);
    drawRenderNodeToScene(unknown, highlightRootPlacementIdx, debugOverlay, m_aptPreviewScene);
    if (m_aptRenderMode != AptRenderMode::Boxes) {
      if (const gf::dat::DatImage* datImg = findDatImageByCharId(charId))
        renderDatGeometryToAptScene(*datImg, parentTransform, m_aptPreviewScene, debugOverlay);
    }
    return;
  }

  if (ch->type != 5 && ch->type != 9) {
    gf::apt::RenderNode leaf;
    leaf.kind = gf::apt::kindFromCharType(ch->type);
    leaf.characterId = charId;
    leaf.worldTransform.a = static_cast<float>(parentTransform.m11());
    leaf.worldTransform.b = static_cast<float>(parentTransform.m12());
    leaf.worldTransform.c = static_cast<float>(parentTransform.m21());
    leaf.worldTransform.d = static_cast<float>(parentTransform.m22());
    leaf.worldTransform.tx = static_cast<float>(parentTransform.dx());
    leaf.worldTransform.ty = static_cast<float>(parentTransform.dy());
    leaf.localBounds = ch->bounds;
    leaf.rootPlacementIndex = rootPlacementIndex;
    leaf.parentChainLabel = parentChainLabel.toStdString();
    std::tie(leaf.symbolName, leaf.importRef) = resolveNames(charId);
    const bool isImage = (ch->type == 7);
    const bool drawBox = (m_aptRenderMode != AptRenderMode::Geometry || !isImage);
    const bool drawDat = (m_aptRenderMode != AptRenderMode::Boxes);
    if (drawBox)
      drawRenderNodeToScene(leaf, highlightRootPlacementIdx, debugOverlay, m_aptPreviewScene);
    if (isImage && drawDat) {
      if (const gf::dat::DatImage* datImg = findDatImageByCharId(charId))
        renderDatGeometryToAptScene(*datImg, parentTransform, m_aptPreviewScene, debugOverlay);
    }
    return;
  }

  auto lg = gf::core::Log::get();
  const QString indent(recursionDepth * 2, QLatin1Char(' '));
  if (lg) lg->debug("[APT] {}renderSprite charId={} depth={}", indent.toStdString(), charId, recursionDepth);

  if (recursionDepth >= kMaxRecursionDepth) {
    if (lg) lg->warn("[APT] {}max sprite recursion depth hit for charId={}", indent.toStdString(), charId);
    gf::apt::RenderNode plh;
    plh.kind = (ch->type == 9) ? gf::apt::RenderNode::Kind::Movie : gf::apt::RenderNode::Kind::Sprite;
    plh.characterId = charId;
    plh.worldTransform.a = static_cast<float>(parentTransform.m11());
    plh.worldTransform.b = static_cast<float>(parentTransform.m12());
    plh.worldTransform.c = static_cast<float>(parentTransform.m21());
    plh.worldTransform.d = static_cast<float>(parentTransform.m22());
    plh.worldTransform.tx = static_cast<float>(parentTransform.dx());
    plh.worldTransform.ty = static_cast<float>(parentTransform.dy());
    plh.localBounds = ch->bounds;
    plh.rootPlacementIndex = rootPlacementIndex;
    plh.parentChainLabel = parentChainLabel.toStdString();
    std::tie(plh.symbolName, plh.importRef) = resolveNames(charId);
    drawRenderNodeToScene(plh, highlightRootPlacementIdx, debugOverlay, m_aptPreviewScene);
    return;
  }

  // ── Type helpers ──────────────────────────────────────────────────────────
  const bool isMovieChar   = (ch->type == 9);
  const char* charTypeName = isMovieChar ? "Movie" : "Sprite";
  const char  charTypeTag  = isMovieChar ? 'M' : 'S';

  // ── Static classification signals ─────────────────────────────────────────
  // Detect whether this container has ActionScript involvement by inspecting the
  // already-parsed frame items (no bytecode execution — purely structural).

  // 1. Does the character's own timeline include any Action or InitAction items?
  bool selfHasActions = false;
  for (const auto& f : ch->frames) {
    for (const auto& item : f.items) {
      if (item.kind == gf::apt::AptFrameItemKind::Action
          || item.kind == gf::apt::AptFrameItemKind::InitAction) {
        selfHasActions = true;
        break;
      }
    }
    if (selfHasActions) break;
  }

  // 2. Does the ROOT movie have an InitAction record that targets this charId?
  //    InitAction = "run this bytecode when character X is first instantiated."
  bool hasInitActionFor = false;
  if (m_currentAptFile) {
    for (const auto& f : m_currentAptFile->frames) {
      for (const auto& item : f.items) {
        if (item.kind == gf::apt::AptFrameItemKind::InitAction
            && item.init_sprite == charId) {
          hasInitActionFor = true;
          break;
        }
      }
      if (hasInitActionFor) break;
    }
  }

  // 3. Is this character exported (can be instantiated by linkage name)?
  std::string exportName;
  if (m_currentAptFile) {
    for (const auto& exp : m_currentAptFile->exports)
      if (exp.character == charId) { exportName = exp.name; break; }
  }

  // 4. Get merged action hints for this character (cached — computed once per charId per file).
  const gf::apt::AptActionHints& actionHints = getCachedAptHints(charId, *ch, characterTable);

  // ── Semantic classification of action strings ─────────────────────────────
  // Determines what each action string likely represents given the APT context.
  // Conservative: only upgrades classification, never claims execution certainty.
  //
  // Order of precedence:
  //   1. Exact export name match      → VisualSymbol (highest confidence)
  //   2. Exact import name match      → ImportSymbol
  //   3. near_call + PushLiteral      → LikelySymbol (structural evidence)
  //   4. Known visual prefix (mc_, btn_, tf_, etc.) → LikelyVisual
  //   5. MemberName / VarName role    → ScriptNoise
  //   6. UrlTarget / FunctionName     → ScriptNoise
  enum class AptStrSem : std::uint8_t {
    Unknown = 0, VisualSymbol, ImportSymbol, LikelySymbol, LikelyVisual, ScriptNoise
  };
  auto classifyStr = [&](const gf::apt::AptActionString& as) -> AptStrSem {
    if (m_currentAptFile) {
      for (const auto& ex : m_currentAptFile->exports)
        if (ex.name == as.value) return AptStrSem::VisualSymbol;
      for (const auto& im : m_currentAptFile->imports)
        if (im.name == as.value) return AptStrSem::ImportSymbol;
    }
    if (as.role == gf::apt::AptStringRole::UrlTarget    ||
        as.role == gf::apt::AptStringRole::FunctionName ||
        as.role == gf::apt::AptStringRole::TargetPath)
      return AptStrSem::ScriptNoise;
    if (as.role == gf::apt::AptStringRole::FrameLabel)
      return AptStrSem::ScriptNoise;
    if (as.near_call && as.role == gf::apt::AptStringRole::PushLiteral)
      return AptStrSem::LikelySymbol;
    // Visual prefix heuristics: common EA/Flash UI naming conventions
    const auto& v = as.value;
    if (v.size() >= 3) {
      const std::string lp3 = { static_cast<char>(std::tolower((unsigned char)v[0])),
                                 static_cast<char>(std::tolower((unsigned char)v[1])),
                                 static_cast<char>(std::tolower((unsigned char)v[2])) };
      if (lp3 == "mc_" || lp3 == "btn" || lp3 == "tf_" || lp3 == "spr" || lp3 == "img")
        return AptStrSem::LikelyVisual;
    }
    if (v.size() >= 4) {
      const std::string lp4 = { static_cast<char>(std::tolower((unsigned char)v[0])),
                                 static_cast<char>(std::tolower((unsigned char)v[1])),
                                 static_cast<char>(std::tolower((unsigned char)v[2])),
                                 static_cast<char>(std::tolower((unsigned char)v[3])) };
      if (lp4 == "txt_" || lp4 == "text" || lp4 == "lbl_" || lp4 == "clip")
        return AptStrSem::LikelyVisual;
    }
    return AptStrSem::Unknown;
  };

  // Count strings per category for annotation
  int nVisualSymbol = 0, nImportSymbol = 0, nLikelySymbol = 0, nLikelyVisual = 0;
  for (const auto& as : actionHints.strings) {
    switch (classifyStr(as)) {
      case AptStrSem::VisualSymbol: ++nVisualSymbol; break;
      case AptStrSem::ImportSymbol: ++nImportSymbol; break;
      case AptStrSem::LikelySymbol: ++nLikelySymbol; break;
      case AptStrSem::LikelyVisual: ++nLikelyVisual; break;
      default: break;
    }
  }

  // ── Build compact annotation: "[A:2E+1L+I+E]" ────────────────────────────
  //   A:NE = N exact export/import symbol matches from action strings
  //   A:NL = N likely-symbol (near_call) strings
  //   A:Nv = N visual-prefix heuristic strings
  //   I    = has InitAction targeting this character
  //   E    = this character is exported by name
  QStringList annotParts;
  if (selfHasActions || actionHints.opcode_count > 0) {
    const int nStrong = nVisualSymbol + nImportSymbol;
    const int nWeak   = nLikelySymbol + nLikelyVisual;
    if (nStrong > 0 || nWeak > 0) {
      QString aTag = QStringLiteral("A:");
      if (nStrong > 0) aTag += QStringLiteral("%1E").arg(nStrong);
      if (nWeak   > 0) aTag += QStringLiteral("%1L").arg(nWeak);
      annotParts << aTag;
    } else if (!actionHints.strings.empty()) {
      annotParts << QStringLiteral("A:%1s").arg(actionHints.strings.size());
    } else {
      annotParts << QStringLiteral("A");
    }
  }
  if (hasInitActionFor)    annotParts << QStringLiteral("I");
  if (!exportName.empty()) annotParts << QStringLiteral("E");
  const QString annot = annotParts.isEmpty()
      ? QString()
      : QStringLiteral("[") + annotParts.join(QLatin1Char('+')) + QStringLiteral("]");

  // ── Candidate table selection ──────────────────────────────────────────────
  // Movie: use its own nested_characters scope (authoritative).
  // Sprite: no scoped table — inherit the caller's characterTable.
  const std::vector<gf::apt::AptCharacter>& fallbackScanTable =
      (isMovieChar && !ch->nested_characters.empty())
          ? ch->nested_characters
          : characterTable;

  // ── Candidate discovery ────────────────────────────────────────────────────
  // Visual types: Shape=1, Image=7, Sprite=5, Movie=9, Import placeholder=0.
  // Movie: scan full nested_characters (everything in scope belongs to this Movie).
  // Sprite: use a proximity window [charId+1 .. charId+kProximityWindow] in the
  //         parent table. EA APT authoring tends to place a widget's children at
  //         consecutive IDs immediately after the container. If the window yields
  //         nothing, fall back to exported characters from the root file.
  constexpr std::size_t   kMaxFallbackChildren = 24;
  constexpr std::uint32_t kProximityWindow     = 16;

  // Reconstruction confidence: what drove the candidate list?
  //   Strong   — exact action-string → export/import name match
  //   Medium   — import/export table evidence but no direct action match
  //   Weak     — heuristic proximity/global scan, no action evidence
  enum class ReconConf { Weak, Medium, Strong };
  ReconConf reconConf = ReconConf::Weak;

  auto collectCandidates = [&]() -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> ids;
    const auto tableSize = static_cast<std::uint32_t>(fallbackScanTable.size());

    auto isVisualType = [](std::uint8_t t) {
      return t == 0 || t == 1 || t == 7 || t == 5 || t == 9;
    };
    auto addUnique = [&](std::uint32_t cid) {
      if (std::find(ids.begin(), ids.end(), cid) == ids.end())
        ids.push_back(cid);
    };

    // ── Tier 0: detected_calls → direct symbol match (highest confidence) ──
    // When a CALLNAMED* opcode fired with a known function name from the pool
    // AND the function is an attach/instantiate pattern, the first arg string
    // is the symbol name (AVM1 pushes args leftmost-first in our captured list).
    // This is stronger than the string scan below because we know the exact
    // runtime role of the string (it was the first argument to attachMovie).
    if (!actionHints.detected_calls.empty() && m_currentAptFile) {
      static const std::unordered_set<std::string> kInstantiateCalls = {
          "attachMovie", "duplicateMovieClip"
      };
      for (const auto& dc : actionHints.detected_calls) {
        if (!kInstantiateCalls.count(dc.call_name)) continue;
        if (dc.arg_strings.empty()) continue;
        const std::string& sym = dc.arg_strings[0]; // symbolName = first arg
        if (sym.empty()) continue;
        for (const auto& exp : m_currentAptFile->exports) {
          if (exp.name == sym && exp.character != charId
              && exp.character < tableSize
              && isVisualType(fallbackScanTable[exp.character].type)) {
            if (std::find(ids.begin(), ids.end(), exp.character) == ids.end()) {
              ids.push_back(exp.character);
              reconConf = ReconConf::Strong;
            }
          }
        }
        for (const auto& imp : m_currentAptFile->imports) {
          if (imp.name == sym && imp.character != charId
              && imp.character < tableSize) {
            if (std::find(ids.begin(), ids.end(), imp.character) == ids.end()) {
              ids.push_back(imp.character);
              if (reconConf != ReconConf::Strong) reconConf = ReconConf::Strong;
            }
          }
        }
      }
    }

    // ── Tier 1: exact action-string → export name match ───────────────────
    // ── Tier 2: exact action-string → import name match ──────────────────
    // Both are the strongest signal: AVM1 code is mentioning a specific symbol.
    if (!actionHints.strings.empty() && m_currentAptFile) {
      std::vector<std::uint32_t> tier1, tier2;
      for (const auto& as : actionHints.strings) {
        // Only PushLiteral and near_call strings are credible symbol references.
        // VarName/MemberName are property accesses, not linkage names.
        const bool isCredible =
            (as.role == gf::apt::AptStringRole::PushLiteral) ||
            (as.role == gf::apt::AptStringRole::VarName && as.near_call);
        if (!isCredible) continue;

        for (const auto& exp : m_currentAptFile->exports) {
          if (exp.name == as.value && exp.character != charId && exp.character < tableSize
              && isVisualType(fallbackScanTable[exp.character].type)) {
            if (std::find(tier1.begin(), tier1.end(), exp.character) == tier1.end())
              tier1.push_back(exp.character);
          }
        }
        for (const auto& imp : m_currentAptFile->imports) {
          if (imp.name == as.value && imp.character != charId && imp.character < tableSize) {
            if (std::find(tier2.begin(), tier2.end(), imp.character) == tier2.end())
              tier2.push_back(imp.character);
          }
        }
      }
      for (auto c : tier1) addUnique(c);
      for (auto c : tier2) addUnique(c);
      if (!ids.empty()) reconConf = ReconConf::Strong;
    }

    // ── Tier 3: LikelySymbol/LikelyVisual strings → fuzzy export prefix match ─
    // If a string looks like a visual symbol name but wasn't found in exports exactly,
    // try to find exports/imports whose names contain that string (case-insensitive).
    // This handles cases like action "BTN_Play" matching export "HUD_BTN_Play".
    if (!actionHints.strings.empty() && m_currentAptFile) {
      for (const auto& as : actionHints.strings) {
        const AptStrSem sem = classifyStr(as);
        if (sem != AptStrSem::LikelySymbol && sem != AptStrSem::LikelyVisual) continue;
        if (as.value.size() < 3) continue;

        // Case-insensitive substring search against export names
        const std::string lval = [&]{
          std::string lv = as.value;
          std::transform(lv.begin(), lv.end(), lv.begin(),
                         [](unsigned char c){ return (char)std::tolower(c); });
          return lv;
        }();

        for (const auto& exp : m_currentAptFile->exports) {
          if (exp.character == charId || exp.character >= tableSize) continue;
          if (!isVisualType(fallbackScanTable[exp.character].type)) continue;
          std::string lexp = exp.name;
          std::transform(lexp.begin(), lexp.end(), lexp.begin(),
                         [](unsigned char c){ return (char)std::tolower(c); });
          if (lexp.find(lval) != std::string::npos || lval.find(lexp) != std::string::npos) {
            if (std::find(ids.begin(), ids.end(), exp.character) == ids.end()) {
              ids.push_back(exp.character);
              if (reconConf == ReconConf::Weak) reconConf = ReconConf::Medium;
            }
          }
        }
      }
    }

    const std::size_t afterActionTiers = ids.size();

    // ── Tier 4: structural scan (Movie: full table; Sprite: proximity) ─────
    if (isMovieChar) {
      for (std::uint32_t cid = 0; cid < tableSize && ids.size() < kMaxFallbackChildren; ++cid) {
        if (cid == charId) continue;
        if (isVisualType(fallbackScanTable[cid].type)) addUnique(cid);
      }
      // If this movie has an export name or action evidence, call it medium
      if (reconConf == ReconConf::Weak && (!exportName.empty() || hasInitActionFor))
        reconConf = ReconConf::Medium;
    } else {
      const std::uint32_t wEnd = std::min(tableSize, charId + 1 + kProximityWindow);
      for (std::uint32_t cid = charId + 1; cid < wEnd; ++cid) {
        if (isVisualType(fallbackScanTable[cid].type)) addUnique(cid);
      }

      // ── Tier 5: Sprite with no proximity hits → try root-level exports ──
      if (ids.size() == afterActionTiers && m_currentAptFile) {
        for (const auto& exp : m_currentAptFile->exports) {
          const std::uint32_t ecid = exp.character;
          if (ecid == charId || ecid >= tableSize) continue;
          if (isVisualType(fallbackScanTable[ecid].type)) addUnique(ecid);
          if (ids.size() >= kMaxFallbackChildren) break;
        }
      }
    }

    if (ids.size() > kMaxFallbackChildren) ids.resize(kMaxFallbackChildren);
    return ids;
  };

  // ── Attach tree reconstruction ─────────────────────────────────────────────
  // Parses detected action calls (attachMovie, duplicateMovieClip, createEmptyMovieClip)
  // to build a structured list of runtime children in call order. Each node carries:
  //   - resolved charId (via exports/imports), or kUnresolved if not found
  //   - instance name for target-path binding and display labels
  //   - recovered property-assignment hints (presence only, not values)
  //   - gotoAndStop/gotoAndPlay label where attributable
  //   - hasSubActions flag when a setTarget path named this instance
  // Returns an empty vector when no instantiate calls are present (weak evidence cases).
  auto buildAttachTree = [&]() -> std::vector<RatNode> {
    std::vector<RatNode> tree;
    if (actionHints.detected_calls.empty()) return tree;

    // instanceName → tree index for target-path and property attribution.
    std::unordered_map<std::string, std::size_t> instMap;

    static const std::unordered_set<std::string> kInstCalls = {
        "attachMovie", "duplicateMovieClip", "createEmptyMovieClip"
    };
    static const std::unordered_set<std::string> kGotoCalls = {"gotoAndStop", "gotoAndPlay"};
    static const std::unordered_set<std::string> kKnownProps = {
        "_x","_y","_alpha","_visible","_xscale","_yscale","_rotation","_width","_height"
    };

    // Pass 1 — one RatNode per instantiate call, in detected order.
    for (const auto& dc : actionHints.detected_calls) {
      if (!kInstCalls.count(dc.call_name)) continue;
      RatNode node;
      if (dc.call_name == "attachMovie") {
        node.source = RatNode::Src::AttachMovie;
        if (!dc.arg_strings.empty())         node.symbolName   = dc.arg_strings[0];
        if (dc.arg_strings.size() > 1)       node.instanceName = dc.arg_strings[1];
      } else if (dc.call_name == "duplicateMovieClip") {
        node.source = RatNode::Src::DuplicateMovieClip;
        if (!dc.arg_strings.empty())         node.symbolName   = dc.arg_strings[0];
        if (dc.arg_strings.size() > 1)       node.instanceName = dc.arg_strings[1];
      } else { // createEmptyMovieClip
        node.source = RatNode::Src::CreateEmpty;
        if (!dc.arg_strings.empty())         node.instanceName = dc.arg_strings[0];
      }
      // Recover depth from the first numeric argument (arg_numbers[0]).
      // EA scripts push args left-to-right, so depth comes after the string args.
      if (!dc.arg_numbers.empty())
        node.depth = static_cast<int>(dc.arg_numbers[0]);

      // Resolve charId: exports first, then imports.
      if (!node.symbolName.empty() && m_currentAptFile) {
        for (const auto& ex : m_currentAptFile->exports) {
          if (ex.name == node.symbolName) {
            node.charId = ex.character;
            node.resolvedLocally = true;
            node.confidence = 2;
            break;
          }
        }
        if (node.charId == RatNode::kUnresolved) {
          for (const auto& im : m_currentAptFile->imports) {
            if (im.name == node.symbolName) {
              node.charId = im.character;
              node.importRef = im.movie + "/" + im.name;
              node.resolvedLocally = false;
              node.source = RatNode::Src::Import;
              node.confidence = 2;
              break;
            }
          }
        }
        if (node.charId == RatNode::kUnresolved) node.confidence = 1;
      } else if (node.source == RatNode::Src::CreateEmpty) {
        node.confidence = 1; // known to create a container; no symbol to resolve
      }

      // Probe visual tier from the scan table.
      if (node.charId != RatNode::kUnresolved && node.charId < fallbackScanTable.size()) {
        const auto& c2 = fallbackScanTable[node.charId];
        if (c2.type == 0)                       node.tier = 3;
        else if (c2.type != 5 && c2.type != 9)  node.tier = 0;
        else {
          bool hp = false;
          for (const auto& f2 : c2.frames) if (!f2.placements.empty()) { hp = true; break; }
          node.tier = hp ? 1 : 2;
        }
      }

      if (!node.instanceName.empty()) instMap[node.instanceName] = tree.size();
      tree.push_back(std::move(node));
    }
    if (tree.empty()) return tree;

    // Pass 2 — associate gotoAndStop/gotoAndPlay labels.
    // Without full AVM1 ordering we can't know which instance each goto targets,
    // so we assign it to the first unset high-confidence node (follows EA patterns).
    for (const auto& dc : actionHints.detected_calls) {
      if (!kGotoCalls.count(dc.call_name) || dc.arg_strings.empty()) continue;
      for (auto& n : tree) {
        if (n.confidence >= 2 && n.gotoLabel.empty()) {
          n.gotoLabel = dc.call_name + "(\"" + dc.arg_strings[0] + "\")";
          break;
        }
      }
    }

    // Pass 3 — TargetPath strings mark the corresponding instance as having sub-actions.
    for (const auto& as : actionHints.strings) {
      if (as.role != gf::apt::AptStringRole::TargetPath) continue;
      auto it = instMap.find(as.value);
      if (it != instMap.end()) tree[it->second].hasSubActions = true;
    }

    // Pass 4 — MemberName property hints attributed via last-seen TargetPath.
    // Sequence: setTarget("inst") then MemberName(_x) → assign _x to that node.
    // Without a prior setTarget the assignment goes to the first strong-confidence node.
    std::string activeTarget;
    for (const auto& as : actionHints.strings) {
      if (as.role == gf::apt::AptStringRole::TargetPath) {
        activeTarget = as.value;
      } else if (as.role == gf::apt::AptStringRole::MemberName
                 && kKnownProps.count(as.value)) {
        RatNode* tgt = nullptr;
        if (!activeTarget.empty()) {
          auto it = instMap.find(activeTarget);
          if (it != instMap.end()) tgt = &tree[it->second];
        }
        if (!tgt)
          for (auto& n : tree) if (n.confidence >= 2) { tgt = &n; break; }
        if (tgt) {
          if      (as.value == "_x")                               tgt->propXAssigned        = true;
          else if (as.value == "_y")                               tgt->propYAssigned        = true;
          else if (as.value == "_alpha")                           tgt->propAlphaAssigned    = true;
          else if (as.value == "_visible")                         tgt->propVisibleAssigned  = true;
          else if (as.value == "_xscale" || as.value == "_yscale") tgt->propScaleAssigned    = true;
          else if (as.value == "_rotation")                        tgt->propRotationAssigned = true;
        }
      }
    }

    // Pass 5 — numeric property values from AptSetMemberHints.
    // The inspector emitted these when it detected push-then-SetMember bytecode patterns.
    // Each hint carries a property name, the recovered numeric value, and the active
    // setTarget instance name (or empty = root container context).
    for (const auto& smh : actionHints.set_members) {
      RatNode* tgt = nullptr;
      if (!smh.target.empty()) {
        auto it = instMap.find(smh.target);
        if (it != instMap.end()) tgt = &tree[it->second];
      }
      if (!tgt)
        for (auto& n : tree) if (n.confidence >= 2) { tgt = &n; break; }
      if (!tgt) continue;
      if      (smh.property == "_x")   { tgt->propXAssigned = true; tgt->propXValue = smh.value; }
      else if (smh.property == "_y")   { tgt->propYAssigned = true; tgt->propYValue = smh.value; }
      else if (smh.property == "_alpha")
        { tgt->propAlphaAssigned = true; tgt->propAlphaValue = smh.value; }
      else if (smh.property == "_xscale" || smh.property == "_yscale")
        { tgt->propScaleAssigned = true; tgt->propScaleValue = smh.value; }
      else if (smh.property == "_rotation")
        { tgt->propRotationAssigned = true; tgt->propRotValue = smh.value; }
    }

    return tree;
  };

  // ── Cycle protection ──────────────────────────────────────────────────────
  // track the charIds currently being rendered via runtime fallback; prevents
  // A→B→A loops that the depth guard alone cannot distinguish.
  thread_local static std::unordered_set<std::uint32_t> s_runtimeActivePath;

  // ── Helper: draw the runtime container outline + label ───────────────────
  // Normal mode: "C119 Screen [runtime]" — concise, shows export name.
  // Debug mode: adds confidence tag and annot.
  const QColor rCol = isMovieChar ? QColor(255, 100, 60, 220) : QColor(255, 160, 60, 220);
  auto drawRuntimeOutline = [&]() {
    // Build label: prefer export name, fall back to C{id}.
    const QString nameStr = exportName.empty()
        ? QString("C%1").arg(charId)
        : QString("C%1 %2").arg(charId).arg(QString::fromStdString(exportName));
    QString rLbl = nameStr + QStringLiteral(" [runtime]");
    if (debugOverlay) {
      const QString confTag = (reconConf == ReconConf::Strong) ? QStringLiteral("[strong]")
                            : (reconConf == ReconConf::Medium) ? QStringLiteral("[export]")
                                                               : QStringLiteral("[heuristic]");
      rLbl += annot + confTag;
    }

    const QPen   rPen  (rCol, 1.5, Qt::DashLine);
    const QBrush rBrush(QColor(rCol.red(), rCol.green(), rCol.blue(), 12));
    const QPointF origin = parentTransform.map(QPointF(0.0, 0.0));
    bool drewRect = false;
    if (ch->bounds) {
      const gf::apt::AptBounds& b = *ch->bounds;
      const qreal bw = static_cast<qreal>(b.right - b.left);
      const qreal bh = static_cast<qreal>(b.bottom - b.top);
      if (bw > 0.0 && bh > 0.0) {
        auto* rItem = m_aptPreviewScene->addRect(
            QRectF(static_cast<qreal>(b.left), static_cast<qreal>(b.top), bw, bh),
            rPen, rBrush);
        rItem->setTransform(parentTransform);
        // Tooltip on the outline box.
        rItem->setToolTip(QString("<b>C%1</b> %2<br>runtime-only container<br>%3")
            .arg(charId)
            .arg(QString::fromStdString(exportName).toHtmlEscaped())
            .arg(isMovieChar ? "Movie" : "Sprite"));
        const QPointF tl = parentTransform.map(QPointF(b.left, b.top));
        auto* lblItem = m_aptPreviewScene->addSimpleText(rLbl);
        QFont f = lblItem->font(); f.setPointSizeF(7.5); lblItem->setFont(f);
        lblItem->setPos(tl.x() + 3.0, tl.y() + 3.0);
        lblItem->setBrush(QBrush(rCol));
        drewRect = true;
      }
    }
    if (!drewRect) {
      auto* lblItem = m_aptPreviewScene->addSimpleText(rLbl);
      QFont f = lblItem->font(); f.setPointSizeF(7.5); lblItem->setFont(f);
      lblItem->setPos(origin + QPointF(4.0, 4.0));
      lblItem->setBrush(QBrush(rCol));
    }
  };

  // ── Helper: emit static placeholder node ──────────────────────────────────
  auto drawFallbackPlaceholder = [&]() {
    gf::apt::RenderNode plh;
    plh.kind = isMovieChar ? gf::apt::RenderNode::Kind::Movie : gf::apt::RenderNode::Kind::Sprite;
    plh.characterId = charId;
    plh.worldTransform.a = static_cast<float>(parentTransform.m11());
    plh.worldTransform.b = static_cast<float>(parentTransform.m12());
    plh.worldTransform.c = static_cast<float>(parentTransform.m21());
    plh.worldTransform.d = static_cast<float>(parentTransform.m22());
    plh.worldTransform.tx = static_cast<float>(parentTransform.dx());
    plh.worldTransform.ty = static_cast<float>(parentTransform.dy());
    plh.localBounds = ch->bounds;
    plh.rootPlacementIndex = rootPlacementIndex;
    plh.parentChainLabel = parentChainLabel.toStdString();
    plh.symbolName = exportName; // already resolved above
    drawRenderNodeToScene(plh, highlightRootPlacementIdx, debugOverlay, m_aptPreviewScene);
  };

  // ── Runtime fallback renderer ──────────────────────────────────────────────
  // Discovers visual child candidates and renders them in a 3-column diagnostic
  // grid. Positions are not game-accurate — ActionScript places objects at runtime.
  // Returns true when at least one candidate was rendered.
  auto doRuntimeFallback = [&](const char* stage) -> bool {
    if (recursionDepth >= kMaxRecursionDepth) return false;

    // Cycle guard: if charId is already on the active fallback stack, stop here.
    if (s_runtimeActivePath.count(charId)) {
      if (lg) lg->warn("[APT] {}runtime fallback cycle detected for charId={}",
                       indent.toStdString(), charId);
      return false;
    }

    // Scene-item budget guard: bail out if the scene already has too many items.
    // Prevents quadratic blow-up when runtime containers nest other runtime containers.
    constexpr int kMaxSceneItems = 500;
    if (m_aptPreviewScene->items().count() >= kMaxSceneItems) {
      if (lg) lg->warn("[APT] {}scene item budget ({}) reached, skipping fallback for charId={}",
                       indent.toStdString(), kMaxSceneItems, charId);
      return false;
    }

    const std::vector<std::uint32_t> visualIds = collectCandidates();
    // reconConf is now set by collectCandidates.
    // Build the attach tree from detected action calls (strong-evidence path).
    const std::vector<RatNode> attachTree = buildAttachTree();

    if (lg) {
      const char* confStr = (reconConf == ReconConf::Strong) ? "strong"
                          : (reconConf == ReconConf::Medium) ? "export/table"
                                                             : "heuristic";
      lg->debug("[APT] {}runtime container charId={} type={}{} confidence={}",
                indent.toStdString(), charId, charTypeName, annot.toStdString(), confStr);
      lg->debug("[APT] {}  stage: {}", indent.toStdString(), stage);
      lg->debug("[APT] {}  opcodes={} strings={} likely_attach={} detected_calls={}",
                indent.toStdString(),
                actionHints.opcode_count, actionHints.strings.size(),
                actionHints.likely_attach_movie ? "yes" : "no",
                actionHints.detected_calls.size());
      if (!actionHints.detected_calls.empty()) {
        for (const auto& dc : actionHints.detected_calls) {
          std::string args;
          for (std::size_t ai = 0; ai < dc.arg_strings.size(); ++ai) {
            if (ai) args += ", ";
            args += "\"" + dc.arg_strings[ai] + "\"";
          }
          lg->debug("[APT] {}    call \"{}\"({}) pool={}",
                    indent.toStdString(), dc.call_name, args, dc.from_pool ? "yes" : "no");
        }
      }
      if (!actionHints.strings.empty()) {
        // Log each string with its role and classification
        for (const auto& as : actionHints.strings) {
          const char* roleStr =
              (as.role == gf::apt::AptStringRole::PushLiteral)  ? "push"
            : (as.role == gf::apt::AptStringRole::VarName)      ? "var"
            : (as.role == gf::apt::AptStringRole::MemberName)   ? "member"
            : (as.role == gf::apt::AptStringRole::TargetPath)   ? "target"
            : (as.role == gf::apt::AptStringRole::FrameLabel)   ? "label"
            : (as.role == gf::apt::AptStringRole::UrlTarget)    ? "url"
            : (as.role == gf::apt::AptStringRole::FunctionName) ? "func"
            : "?";
          const AptStrSem sem = classifyStr(as);
          const char* semStr =
              (sem == AptStrSem::VisualSymbol)  ? "VisualSymbol"
            : (sem == AptStrSem::ImportSymbol)  ? "ImportSymbol"
            : (sem == AptStrSem::LikelySymbol)  ? "LikelySymbol"
            : (sem == AptStrSem::LikelyVisual)  ? "LikelyVisual"
            : (sem == AptStrSem::ScriptNoise)   ? "Noise"
            : "Unknown";
          lg->debug("[APT] {}    str \"{}\" role={} near_call={} sem={}",
                    indent.toStdString(), as.value, roleStr,
                    as.near_call ? "yes" : "no", semStr);
        }
      }
      lg->debug("[APT] {}  fallback candidates={}", indent.toStdString(), visualIds.size());
      if (visualIds.size() >= kMaxFallbackChildren)
        lg->debug("[APT] {}  candidate list capped at {}", indent.toStdString(), kMaxFallbackChildren);
    }
    if (visualIds.empty()) return false;

    // ── Per-candidate tier classification ────────────────────────────────────
    // Tier 0 = leaf visual type (Shape/Image/EditText/Button) — produces colored rect
    // Tier 1 = Sprite/Movie with >=1 framed placement         — recursively renders art
    // Tier 2 = frameless Sprite/Movie (runtime container)     — produces RUNTIME box
    // Tier 3 = import placeholder (type=0)                    — named import tile
    //
    // Use parallel vectors to avoid local-class-in-lambda template restrictions.
    const std::size_t nCands = visualIds.size();
    std::vector<int>           slotTiers    (nCands, 3);
    std::vector<std::string>   slotSources  (nCands);
    std::vector<std::string>   slotImportRefs(nCands);

    for (std::size_t si = 0; si < nCands; ++si) {
      const std::uint32_t c = visualIds[si];
      // Probe tier
      if (c < fallbackScanTable.size()) {
        const auto& c2 = fallbackScanTable[c];
        if (c2.type != 0 && c2.type != 5 && c2.type != 9) {
          slotTiers[si] = 0; // leaf
        } else if (c2.type == 5 || c2.type == 9) {
          bool hasPlaced = false;
          for (const auto& f2 : c2.frames) if (!f2.placements.empty()) { hasPlaced = true; break; }
          slotTiers[si] = hasPlaced ? 1 : 2;
        }
        // else type==0 stays tier=3
      }
      // Build source label
      if (m_currentAptFile) {
        static const std::unordered_set<std::string> kIC = {"attachMovie","duplicateMovieClip"};
        bool found = false;
        for (const auto& dc : actionHints.detected_calls) {
          if (!kIC.count(dc.call_name) || dc.arg_strings.empty()) continue;
          const std::string& sym = dc.arg_strings[0];
          for (const auto& ex : m_currentAptFile->exports)
            if (ex.name == sym && ex.character == c)
              { slotSources[si] = dc.call_name + "(\"" + sym + "\")"; found = true; break; }
          if (found) break;
          for (const auto& im : m_currentAptFile->imports)
            if (im.name == sym && im.character == c)
              { slotSources[si] = dc.call_name + "(\"" + sym + "\") [imp]"; found = true; break; }
          if (found) break;
        }
        if (!found) {
          for (const auto& as : actionHints.strings) {
            if (as.role != gf::apt::AptStringRole::PushLiteral && !as.near_call) continue;
            for (const auto& ex : m_currentAptFile->exports)
              if (ex.name == as.value && ex.character == c)
                { slotSources[si] = "str(\"" + as.value + "\")"; found = true; break; }
            if (found) break;
            for (const auto& im : m_currentAptFile->imports)
              if (im.name == as.value && im.character == c)
                { slotSources[si] = "str(\"" + as.value + "\") [imp]"; found = true; break; }
            if (found) break;
          }
        }
        if (!found)
          slotSources[si] = (c > charId && c <= charId + kProximityWindow) ? "proximity" : "export-scan";
      } else {
        slotSources[si] = "heuristic";
      }
      // Resolve import ref for tier=3
      if (slotTiers[si] == 3 && m_currentAptFile) {
        for (const auto& imp : m_currentAptFile->imports)
          if (imp.character == c) { slotImportRefs[si] = imp.movie + "/" + imp.name; break; }
      }
    }

    // Build sorted-index array (leaves first, framed second, runtime-ctr third, imports last).
    std::vector<std::size_t> order(nCands);
    std::iota(order.begin(), order.end(), std::size_t(0));
    std::stable_sort(order.begin(), order.end(),
        [&slotTiers](std::size_t a, std::size_t b){ return slotTiers[a] < slotTiers[b]; });

    // primaryCount drives secondaryYBase:
    //   tree path  → all tree nodes are primary (action-grounded)
    //   flat path  → tier<2 candidates are primary
    std::size_t primaryCount = 0;
    if (!attachTree.empty()) {
      primaryCount = attachTree.size();
    } else {
      for (std::size_t si = 0; si < nCands; ++si) if (slotTiers[si] < 2) ++primaryCount;
    }

    drawRuntimeOutline();

    constexpr std::size_t kGridCols = 3;
    constexpr qreal       kColStep  = 170.0;
    constexpr qreal       kRowStep  = 130.0;
    const std::size_t primaryRows = (primaryCount + kGridCols - 1) / kGridCols;
    const qreal secondaryYBase    = (primaryRows > 0)
        ? static_cast<qreal>(primaryRows) * kRowStep + 22.0
        : 0.0;

    const QString childChain = parentChainLabel
        + QString(" \u2192 RUNTIME%1#%2").arg(QLatin1Char(charTypeTag)).arg(charId);

    s_runtimeActivePath.insert(charId);
    bool addedSecSep = false;
    std::size_t primaryIdx = 0, secondaryIdx = 0;

    if (!attachTree.empty()) {
      // ── Tree-based render path ─────────────────────────────────────────────
      // Nodes rendered in action-call order. Instance-name labels are shown above
      // each tile. Heuristic candidates not matched by the tree are demoted to
      // a secondary section so they do not crowd the action-grounded children.

      std::unordered_set<std::uint32_t> treeCovered;
      for (const auto& n : attachTree)
        if (n.charId != RatNode::kUnresolved) treeCovered.insert(n.charId);

      for (std::size_t ni = 0; ni < attachTree.size(); ++ni) {
        const RatNode& node = attachTree[ni];
        const std::uint32_t cid = node.charId;
        const std::uint8_t ct = (cid < fallbackScanTable.size())
                                 ? fallbackScanTable[cid].type : 0xFF;
        const char* ctName = (ct == 9) ? "Movie"
                           : (ct == 5) ? "Sprite"
                           : (ct == 7) ? "Image"
                           : (ct == 0) ? "Import"
                           : (ct == 0xFF) ? "?" : "Shape";

        // Position: use recovered _x/_y when available; otherwise synthetic grid.
        const qreal gridX = static_cast<qreal>(primaryIdx % kGridCols) * kColStep;
        const qreal gridY = static_cast<qreal>(primaryIdx / kGridCols) * kRowStep;
        ++primaryIdx;
        const bool useRecovered = node.hasX() || node.hasY();
        const qreal xOff = useRecovered ? (node.hasX() ? node.propXValue : gridX) : gridX;
        const qreal yOff = useRecovered ? (node.hasY() ? node.propYValue : gridY) : gridY;
        const QTransform childXf = parentTransform * QTransform::fromTranslate(xOff, yOff);

        if (lg) {
          const char* srcName =
              (node.source == RatNode::Src::AttachMovie)        ? "attachMovie"
            : (node.source == RatNode::Src::DuplicateMovieClip) ? "duplicateMovieClip"
            : (node.source == RatNode::Src::CreateEmpty)        ? "createEmpty"
            : (node.source == RatNode::Src::Import)             ? "import"
                                                                : "heuristic";
          lg->debug("[APT] {}tree[{}] {} sym=\"{}\" inst=\"{}\" C{} {} tier={} pos={}",
                    indent.toStdString(), ni, srcName,
                    node.symbolName, node.instanceName,
                    cid == RatNode::kUnresolved ? 0u : cid, ctName, node.tier,
                    useRecovered ? "recovered" : "grid");
        }

        // Instance-name label shown above every tile (not just debug mode) so
        // the user can identify the scorebug element at a glance.
        if (m_aptPreviewScene) {
          const QString instLbl = node.instanceName.empty()
              ? (QStringLiteral("C")
                 + (cid == RatNode::kUnresolved ? QStringLiteral("?") : QString::number(cid)))
              : (QStringLiteral("\u25B6 \"") + QString::fromStdString(node.instanceName)
                 + QStringLiteral("\""));
          const QPointF labelPos = parentTransform.map(QPointF(xOff + 2.0, yOff - 13.0));
          auto* lbl = m_aptPreviewScene->addSimpleText(instLbl);
          lbl->setPos(labelPos);
          QFont f = lbl->font(); f.setPointSizeF(7.0); lbl->setFont(f);
          lbl->setBrush(QBrush(QColor(120, 220, 255, 230)));
        }

        const int itemsBefore = m_aptPreviewScene ? m_aptPreviewScene->items().count() : 0;

        if (cid == RatNode::kUnresolved) {
          // Unresolved symbol or createEmpty — yellow dashed placeholder tile.
          if (m_aptPreviewScene) {
            const QPointF tlPos = parentTransform.map(QPointF(xOff, yOff));
            const QColor uCol(180, 180, 60, 200);
            m_aptPreviewScene->addRect(QRectF(tlPos.x(), tlPos.y(), 160.0, 70.0),
                QPen(uCol, 1.5, Qt::DashLine), QBrush(QColor(180, 180, 60, 12)));
            const QString lbl = (node.source == RatNode::Src::CreateEmpty)
                ? (QStringLiteral("createEmpty\n\"")
                   + QString::fromStdString(node.instanceName) + QStringLiteral("\""))
                : (QStringLiteral("? unresolved\n\"")
                   + QString::fromStdString(node.symbolName) + QStringLiteral("\""));
            auto* pLbl = m_aptPreviewScene->addSimpleText(lbl);
            pLbl->setPos(tlPos + QPointF(5.0, 4.0));
            QFont pf = pLbl->font(); pf.setPointSizeF(7.5); pLbl->setFont(pf);
            pLbl->setBrush(QBrush(uCol));
          }
        } else if (node.tier == 3 || node.source == RatNode::Src::Import) {
          // Import tile — orange dashed-border with importRef and instance name.
          if (m_aptPreviewScene) {
            const QPointF tlPos = parentTransform.map(QPointF(xOff, yOff));
            const QColor impCol(255, 150, 40, 200);
            m_aptPreviewScene->addRect(QRectF(tlPos.x(), tlPos.y(), 160.0, 70.0),
                QPen(impCol, 1.5, Qt::DashDotLine), QBrush(QColor(255, 130, 20, 14)));
            const QString impName = node.importRef.empty()
                ? QStringLiteral("C") + QString::number(cid)
                : QString::fromStdString(node.importRef);
            const QString instSuffix = node.instanceName.empty() ? QString()
                : (QStringLiteral("\ninst: \"") + QString::fromStdString(node.instanceName)
                   + QStringLiteral("\""));
            auto* impLbl = m_aptPreviewScene->addSimpleText(
                QStringLiteral("IMPORT\n") + impName + instSuffix);
            impLbl->setPos(tlPos + QPointF(5.0, 4.0));
            QFont impF = impLbl->font(); impF.setPointSizeF(7.5); impLbl->setFont(impF);
            impLbl->setBrush(QBrush(impCol));
          }
        } else {
          renderAptCharacterRecursive(cid, childXf, fallbackScanTable,
                                      rootPlacementIndex, highlightRootPlacementIdx, debugOverlay,
                                      recursionDepth + 1, childChain);
        }

        const int itemsAfter = m_aptPreviewScene ? m_aptPreviewScene->items().count() : 0;
        if (lg) {
          lg->debug("[APT] {}  +{} scene items (total {})",
                    indent.toStdString(), itemsAfter - itemsBefore, itemsAfter);
        }
      }

      // ── Residual heuristic candidates not matched by the tree ─────────────
      // These are demoted to a secondary section so they do not dominate the view.
      for (std::size_t si = 0; si < nCands; ++si) {
        const std::size_t i = order[si];
        const std::uint32_t cid = visualIds[i];
        if (treeCovered.count(cid)) continue; // already in primary

        if (!addedSecSep && m_aptPreviewScene) {
          const QPointF sepPos = parentTransform.map(QPointF(0.0, secondaryYBase - 16.0));
          auto* sep = m_aptPreviewScene->addSimpleText(
              QStringLiteral("\u2500\u2500 heuristic / unmatched candidates \u2500\u2500"));
          sep->setPos(sepPos);
          QFont sf = sep->font(); sf.setPointSizeF(7.0); sep->setFont(sf);
          sep->setBrush(QBrush(QColor(130, 130, 160, 180)));
          addedSecSep = true;
        }
        const qreal xr = static_cast<qreal>(secondaryIdx % kGridCols) * kColStep;
        const qreal yr = secondaryYBase + static_cast<qreal>(secondaryIdx / kGridCols) * kRowStep;
        ++secondaryIdx;
        const QTransform xfr = parentTransform * QTransform::fromTranslate(xr, yr);
        renderAptCharacterRecursive(cid, xfr, fallbackScanTable,
                                    rootPlacementIndex, highlightRootPlacementIdx, debugOverlay,
                                    recursionDepth + 1, childChain);
      }

    } else {
      // ── Flat-grid fallback (no action-call evidence; tier-sorted buckets) ──
      for (std::size_t si = 0; si < nCands; ++si) {
        const std::size_t    i    = order[si];
        const std::uint32_t  cid  = visualIds[i];
        const int            tier = slotTiers[i];
        const std::string&   src  = slotSources[i];
        const std::string&   impr = slotImportRefs[i];

        const std::uint8_t ct = (cid < fallbackScanTable.size())
                                 ? fallbackScanTable[cid].type : 0;
        const char* ctName = (ct == 9) ? "Movie"
                           : (ct == 5) ? "Sprite"
                           : (ct == 7) ? "Image"
                           : (ct == 0) ? "Import"
                                       : "Shape";

        qreal xOff, yOff;
        if (tier < 2) {
          xOff = static_cast<qreal>(primaryIdx % kGridCols) * kColStep;
          yOff = static_cast<qreal>(primaryIdx / kGridCols) * kRowStep;
          ++primaryIdx;
        } else {
          if (!addedSecSep && m_aptPreviewScene) {
            const QPointF sepPos = parentTransform.map(QPointF(0.0, secondaryYBase - 16.0));
            auto* sep = m_aptPreviewScene->addSimpleText(
                QStringLiteral("\u2500\u2500 heuristic / secondary \u2500\u2500"));
            sep->setPos(sepPos);
            QFont sf = sep->font(); sf.setPointSizeF(7.0); sep->setFont(sf);
            sep->setBrush(QBrush(QColor(130, 130, 160, 180)));
            addedSecSep = true;
          }
          xOff = static_cast<qreal>(secondaryIdx % kGridCols) * kColStep;
          yOff = secondaryYBase + static_cast<qreal>(secondaryIdx / kGridCols) * kRowStep;
          ++secondaryIdx;
        }
        const QTransform childTransform = parentTransform * QTransform::fromTranslate(xOff, yOff);

        if (lg) {
          lg->debug("[APT] {}scaffold[{}] C{} {} tier={} source=\"{}\" import={}",
                    indent.toStdString(), si, cid, ctName, tier,
                    src, impr.empty() ? "-" : impr);
        }
        if (debugOverlay && m_aptPreviewScene) {
          const QString srcLbl = (tier == 3)
              ? (QStringLiteral("\u25B6 import ")
                 + QString::fromStdString(impr.empty()
                                          ? std::string("C") + std::to_string(cid) : impr))
              : (QStringLiteral("\u25B6 ") + QString::fromStdString(src));
          const QPointF labelPos = parentTransform.map(QPointF(xOff + 2.0, yOff - 13.0));
          auto* srcLblItem = m_aptPreviewScene->addSimpleText(srcLbl);
          srcLblItem->setPos(labelPos);
          QFont f = srcLblItem->font(); f.setPointSizeF(7.0); srcLblItem->setFont(f);
          srcLblItem->setBrush(QBrush(QColor(255, 210, 80, 230)));
        }

        const int itemsBefore = m_aptPreviewScene ? m_aptPreviewScene->items().count() : 0;
        if (tier == 3) {
          const QPointF tlPos = parentTransform.map(QPointF(xOff, yOff));
          const QColor impCol(255, 150, 40, 200);
          m_aptPreviewScene->addRect(QRectF(tlPos.x(), tlPos.y(), 160.0, 70.0),
              QPen(impCol, 1.5, Qt::DashDotLine), QBrush(QColor(255, 130, 20, 14)));
          const QString impName = impr.empty()
              ? QStringLiteral("C") + QString::number(cid)
              : QString::fromStdString(impr);
          auto* impLbl = m_aptPreviewScene->addSimpleText(
              QStringLiteral("IMPORT\n") + impName
              + QStringLiteral("\ncharId ") + QString::number(cid));
          impLbl->setPos(tlPos + QPointF(5.0, 4.0));
          QFont impF = impLbl->font(); impF.setPointSizeF(7.5); impLbl->setFont(impF);
          impLbl->setBrush(QBrush(impCol));
        } else {
          renderAptCharacterRecursive(cid, childTransform, fallbackScanTable,
                                      rootPlacementIndex, highlightRootPlacementIdx, debugOverlay,
                                      recursionDepth + 1, childChain);
        }
        const int itemsAfter = m_aptPreviewScene ? m_aptPreviewScene->items().count() : 0;
        if (lg) {
          lg->debug("[APT] {}  +{} scene items (total {})",
                    indent.toStdString(), itemsAfter - itemsBefore, itemsAfter);
        }
      }
    } // end flat-grid fallback

    s_runtimeActivePath.erase(charId);

    if (lg) {
      if (!attachTree.empty()) {
        int nL=0, nF=0, nR=0, nI=0, nU=0;
        for (const auto& n : attachTree) {
          if      (n.tier == 0)                   ++nL;
          else if (n.tier == 1)                   ++nF;
          else if (n.tier == 2)                   ++nR;
          else if (n.tier == 3)                   ++nI;
          if      (n.charId == RatNode::kUnresolved) ++nU;
        }
        lg->debug("[APT] {}tree done: {} nodes — {} leaf, {} framed, {} runtime-ctr, {} import, {} unresolved",
                  indent.toStdString(), attachTree.size(), nL, nF, nR, nI, nU);
      } else {
        int nL=0, nF=0, nR=0, nI=0;
        for (std::size_t si2 = 0; si2 < nCands; ++si2) {
          if      (slotTiers[si2] == 0) ++nL;
          else if (slotTiers[si2] == 1) ++nF;
          else if (slotTiers[si2] == 2) ++nR;
          else if (slotTiers[si2] == 3) ++nI;
        }
        lg->debug("[APT] {}scaffold done: {} total — {} leaf, {} framed, {} runtime-ctr, {} import",
                  indent.toStdString(), nCands, nL, nF, nR, nI);
      }
    }
    return true;
  };

  // ── Stage 1: no timeline data at all ──────────────────────────────────────
  if (ch->frames.empty()) {
    if (!doRuntimeFallback("no static frames"))
      drawFallbackPlaceholder();
    return;
  }

  // ── Stage 2: timeline exists; try normal static rendering ─────────────────
  const std::vector<gf::apt::AptCharacter>& childTable =
      (isMovieChar && !ch->nested_characters.empty()) ? ch->nested_characters : characterTable;
  const gf::apt::AptFrame spriteDl = gf::apt::build_display_list_frame(ch->frames, 0);
  if (lg) lg->debug("[APT] {}sprite nodes={}", indent.toStdString(), spriteDl.placements.size());

  // ── Stage 3: frame 0 DL is empty — classify then fall back ────────────────
  if (spriteDl.placements.empty()) {
    if (lg) {
      lg->debug("[APT] {}static frame empty — classifying container:", indent.toStdString());
      lg->debug("[APT] {}  selfHasActions={} hasInitAction={} exported={}",
                indent.toStdString(),
                selfHasActions, hasInitActionFor, !exportName.empty());
      lg->debug("[APT] {}static frame empty, using runtime fallback", indent.toStdString());
    }
    if (!doRuntimeFallback("static frame empty"))
      drawFallbackPlaceholder();
    return;
  }

  // ── Stage 4: normal recursive rendering ───────────────────────────────────
  renderAptResolvedFrameRecursive(spriteDl, parentTransform, childTable,
                                  rootPlacementIndex, highlightRootPlacementIdx, debugOverlay,
                                  recursionDepth + 1,
                                  parentChainLabel + QStringLiteral(" \u2192 S#%1").arg(charId));
}

void MainWindow::renderAptResolvedFrameRecursive(const gf::apt::AptFrame& resolvedFrame,
                                                 const QTransform& parentTransform,
                                                 const std::vector<gf::apt::AptCharacter>& characterTable,
                                                 int rootPlacementIndex,
                                                 int highlightRootPlacementIdx,
                                                 bool debugOverlay,
                                                 int recursionDepth,
                                                 const QString& parentChainLabel)
{
  std::vector<std::size_t> order(resolvedFrame.placements.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return resolvedFrame.placements[a].depth < resolvedFrame.placements[b].depth;
  });

  for (const std::size_t pi : order) {
    const auto& pl = resolvedFrame.placements[pi];
    const int effectiveRootIndex = (rootPlacementIndex >= 0)
        ? rootPlacementIndex
        : static_cast<int>(pi);

    const QTransform local = transform2DToQt(gf::apt::toTransform2D(pl.transform));
    const QTransform childWorld = parentTransform * local;

    auto resolveCharacter = [&](std::uint32_t cid) -> const gf::apt::AptCharacter* {
      if (cid < characterTable.size() && characterTable[cid].type != 0)
        return &characterTable[cid];
      if (m_currentAptFile && &characterTable != &m_currentAptFile->characters
          && cid < m_currentAptFile->characters.size()
          && m_currentAptFile->characters[cid].type != 0)
        return &m_currentAptFile->characters[cid];
      return nullptr;
    };
    const gf::apt::AptCharacter* ch = resolveCharacter(pl.character);

    if (ch && (ch->type == 5 || ch->type == 9)) {
      renderAptCharacterRecursive(pl.character, childWorld, characterTable,
                                  effectiveRootIndex, highlightRootPlacementIdx, debugOverlay,
                                  recursionDepth, parentChainLabel);
      continue;
    }

    gf::apt::RenderNode node;
    node.kind = ch ? gf::apt::kindFromCharType(ch->type) : gf::apt::RenderNode::Kind::Unknown;
    node.characterId = pl.character;
    node.placementDepth = pl.depth;
    node.instanceName = pl.instance_name;
    node.worldTransform.a = static_cast<float>(childWorld.m11());
    node.worldTransform.b = static_cast<float>(childWorld.m12());
    node.worldTransform.c = static_cast<float>(childWorld.m21());
    node.worldTransform.d = static_cast<float>(childWorld.m22());
    node.worldTransform.tx = static_cast<float>(childWorld.dx());
    node.worldTransform.ty = static_cast<float>(childWorld.dy());
    node.localBounds = ch ? ch->bounds : std::optional<gf::apt::AptBounds>{};
    node.rootPlacementIndex = effectiveRootIndex;
    node.parentChainLabel = parentChainLabel.toStdString();
    // Resolve export/import name for labeling (P1).
    if (m_currentAptFile) {
      for (const auto& ex : m_currentAptFile->exports)
        if (ex.character == pl.character) { node.symbolName = ex.name; break; }
      if (node.symbolName.empty()) {
        for (const auto& im : m_currentAptFile->imports)
          if (im.character == pl.character) {
            node.symbolName = im.name;
            node.importRef  = im.movie + "/" + im.name;
            break;
          }
      }
    }
    const bool nodeIsImage = (!ch || ch->type == 7);
    const bool drawBoxR    = (m_aptRenderMode != AptRenderMode::Geometry || !nodeIsImage);
    const bool drawDatR    = (m_aptRenderMode != AptRenderMode::Boxes);
    if (drawBoxR)
      drawRenderNodeToScene(node, highlightRootPlacementIdx, debugOverlay, m_aptPreviewScene);
    // DAT geometry fallback for unresolved or Image characters.
    if (nodeIsImage && drawDatR) {
      if (const gf::dat::DatImage* datImg = findDatImageByCharId(pl.character))
        renderDatGeometryToAptScene(*datImg, childWorld, m_aptPreviewScene, debugOverlay);
    }
  }
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------

void MainWindow::renderAptFrameToScene(const gf::apt::AptFrame* frame,
                                       int highlightedPlacementIndex,
                                       const std::vector<gf::apt::AptCharacter>* characterTable,
                                       const QString& noFrameMsg,
                                       bool fitToContent,
                                       bool suppressContainerMsg) {
  if (!m_aptPreviewScene || !m_currentAptFile) return;

  m_aptPreviewScene->clearEditorOverlay();
  { QSignalBlocker _sb(m_aptPreviewScene); m_aptPreviewScene->clear(); }

  const auto [sceneWidth, sceneHeight] = resolveAptPreviewCanvasSize(m_currentAptFile->summary);
  const QRectF canvasRect(0.0, 0.0, sceneWidth, sceneHeight);
  m_aptPreviewScene->setSceneRect(canvasRect.adjusted(-40.0, -40.0, 40.0, 40.0));

  const QPen gridPen(QColor(58, 58, 66, 120));
  constexpr qreal gridStep = 100.0;
  for (qreal x = 0.0; x <= sceneWidth; x += gridStep)
    m_aptPreviewScene->addLine(x, 0.0, x, sceneHeight, gridPen);
  for (qreal y = 0.0; y <= sceneHeight; y += gridStep)
    m_aptPreviewScene->addLine(0.0, y, sceneWidth, y, gridPen);

  m_aptPreviewScene->addRect(canvasRect, QPen(QColor(200, 200, 210), 2.0), QBrush(QColor(36, 36, 42)));
  m_aptPreviewScene->addLine(0.0, 0.0, 40.0, 0.0, QPen(QColor(220, 80, 80), 2.0));
  m_aptPreviewScene->addLine(0.0, 0.0, 0.0, 40.0, QPen(QColor(80, 200, 120), 2.0));

  auto* originLabel = m_aptPreviewScene->addSimpleText("Origin (0,0)");
  originLabel->setBrush(QBrush(QColor(225, 225, 230)));
  originLabel->setPos(6.0, 6.0);

  if (!frame) {
    if (m_aptDlStatusLabel) m_aptDlStatusLabel->setText("(no frame selected)");
    const QString msg = noFrameMsg.isEmpty()
        ? QStringLiteral("Select a frame or character to preview.")
        : noFrameMsg;
    auto* msgItem = m_aptPreviewScene->addSimpleText(msg);
    msgItem->setBrush(QBrush(QColor(160, 165, 180)));
    msgItem->setPos(24.0, sceneHeight * 0.5 - msgItem->boundingRect().height() * 0.5);
    if (auto* v = qobject_cast<AptPreviewView*>(m_aptPreviewView)) v->fitContent();
    return;
  }

  const bool debugOverlay = m_aptDebugAction && m_aptDebugAction->isChecked();

  gf::apt::RenderOptions opts;
  opts.highlightRootPlacementIdx = highlightedPlacementIndex;
  opts.maxRecursionDepth = 8;
  opts.collectParentChain = debugOverlay;

  // Use the caller-supplied character table when rendering a nested character frame,
  // otherwise fall back to the root movie's character table.
  const std::vector<gf::apt::AptCharacter>& table =
      characterTable ? *characterTable : m_currentAptFile->characters;

  std::vector<gf::apt::RenderNode> nodes;
  bool renderedTimeline = false;
  if (m_aptCharPreviewIdx >= 0
      && m_aptCharPreviewIdx < static_cast<int>(m_currentAptFile->characters.size())) {
    const auto& ch = m_currentAptFile->characters[static_cast<std::size_t>(m_aptCharPreviewIdx)];
    if (!ch.frames.empty() && frame->index < ch.frames.size() && &ch.frames[frame->index] == frame) {
      nodes = gf::apt::renderAptTimelineFrame(*m_currentAptFile, ch.frames, frame->index, table,
                                              gf::apt::Transform2D::identity(), opts);
      renderedTimeline = true;
    }
  }
  if (!renderedTimeline && frame->index < m_currentAptFile->frames.size()
      && &m_currentAptFile->frames[frame->index] == frame) {
    nodes = gf::apt::renderAptTimelineFrame(*m_currentAptFile, m_currentAptFile->frames, frame->index, table,
                                            gf::apt::Transform2D::identity(), opts);
    renderedTimeline = true;
  }
  if (!renderedTimeline) {
    nodes = gf::apt::renderAptFrame(*m_currentAptFile, *frame, table,
                                    gf::apt::Transform2D::identity(), opts);
  }

  // ── Display-list diagnostics ──────────────────────────────────────────────
  {
    // Count node categories for the status line.
    int unknownCnt = 0, containerCnt = 0;
    for (const auto& n : nodes) {
      if (n.kind == gf::apt::RenderNode::Kind::Unknown) ++unknownCnt;
      else if (n.kind == gf::apt::RenderNode::Kind::Sprite
            || n.kind == gf::apt::RenderNode::Kind::Movie) ++containerCnt;
    }

    // Find frame label for the status line (FrameLabel item in current frame).
    QString frameLabel;
    if (frame->index < m_currentAptFile->frames.size()
        && &m_currentAptFile->frames[frame->index] == frame) {
      for (const auto& it : m_currentAptFile->frames[frame->index].items)
        if (it.kind == gf::apt::AptFrameItemKind::FrameLabel && !it.label.empty()) {
          frameLabel = QString::fromStdString(it.label);
          break;
        }
    } else if (m_aptCharPreviewIdx >= 0
               && m_aptCharPreviewIdx < static_cast<int>(m_currentAptFile->characters.size())) {
      const auto& ch = m_currentAptFile->characters[static_cast<std::size_t>(m_aptCharPreviewIdx)];
      if (frame->index < ch.frames.size())
        for (const auto& it : ch.frames[frame->index].items)
          if (it.kind == gf::apt::AptFrameItemKind::FrameLabel && !it.label.empty()) {
            frameLabel = QString::fromStdString(it.label);
            break;
          }
    }

    // Build the total-frame count for the status line.
    int totalFrames = 0;
    if (m_aptCharPreviewIdx >= 0
        && m_aptCharPreviewIdx < static_cast<int>(m_currentAptFile->characters.size()))
      totalFrames = static_cast<int>(
          m_currentAptFile->characters[static_cast<std::size_t>(m_aptCharPreviewIdx)].frames.size());
    else
      totalFrames = static_cast<int>(m_currentAptFile->frames.size());

    // Update status label.
    if (m_aptDlStatusLabel) {
      const QString mode = renderedTimeline ? "timeline DL" : "single-frame";
      QString status = QStringLiteral("[%1] Frame %2/%3%4 · %5 node%6")
                           .arg(mode)
                           .arg(frame->index + 1).arg(totalFrames)
                           .arg(frameLabel.isEmpty() ? QString() : QStringLiteral(" \"%1\"").arg(frameLabel))
                           .arg(nodes.size()).arg(nodes.size() == 1 ? "" : "s");
      if (unknownCnt > 0)    status += QStringLiteral(" · %1 unresolved").arg(unknownCnt);
      if (containerCnt > 0)  status += QStringLiteral(" · %1 container placeholder%2").arg(containerCnt).arg(containerCnt == 1 ? "" : "s");
      m_aptDlStatusLabel->setText(status);
    }

    // spdlog — frame + DL composition.
    if (auto lg = gf::core::Log::get()) {
      lg->debug("[APT-DL] Frame {}{} — {} ({} node{}, {} unresolved, {} containers)",
                frame->index,
                frameLabel.isEmpty() ? "" : " \"" + frameLabel.toStdString() + "\"",
                renderedTimeline ? "timeline DL" : "single-frame",
                nodes.size(), nodes.size() == 1 ? "" : "s",
                unknownCnt, containerCnt);
      for (const auto& n : nodes) {
        const char* kindStr = "?";
        switch (n.kind) {
          case gf::apt::RenderNode::Kind::Shape:    kindStr = "Shape";    break;
          case gf::apt::RenderNode::Kind::Image:    kindStr = "Image";    break;
          case gf::apt::RenderNode::Kind::EditText: kindStr = "EditText"; break;
          case gf::apt::RenderNode::Kind::Button:   kindStr = "Button";   break;
          case gf::apt::RenderNode::Kind::Sprite:   kindStr = "Sprite";   break;
          case gf::apt::RenderNode::Kind::Movie:    kindStr = "Movie";    break;
          case gf::apt::RenderNode::Kind::Unknown:  kindStr = "Unknown";  break;
        }
        lg->debug("[APT-DL]   D{} C{} {} tx={:.1f} ty={:.1f}{}",
                  n.placementDepth, n.characterId, kindStr,
                  n.worldTransform.tx, n.worldTransform.ty,
                  n.instanceName.empty() ? "" : " \"" + n.instanceName + "\"");
      }

      // Sprite-tree walk: log recursive sprite/movie sub-structure in debug mode.
      if (debugOverlay) {
        lg->debug("[APT] --- sprite tree walk ---");
        const std::vector<gf::apt::AptCharacter>& tbl =
            characterTable ? *characterTable : m_currentAptFile->characters;
        // Determine the source frames to build the resolved DL from.
        const std::vector<gf::apt::AptFrame>* srcFrames = nullptr;
        if (m_aptCharPreviewIdx >= 0
            && m_aptCharPreviewIdx < static_cast<int>(m_currentAptFile->characters.size())) {
          const auto& ch = m_currentAptFile->characters[static_cast<std::size_t>(m_aptCharPreviewIdx)];
          if (!ch.frames.empty() && frame->index < ch.frames.size()
              && &ch.frames[frame->index] == frame)
            srcFrames = &ch.frames;
        }
        if (!srcFrames && frame->index < m_currentAptFile->frames.size()
            && &m_currentAptFile->frames[frame->index] == frame)
          srcFrames = &m_currentAptFile->frames;
        if (srcFrames) {
          const gf::apt::AptFrame dl =
              gf::apt::build_display_list_frame(*srcFrames, frame->index);
          logAptSpriteTree(*m_currentAptFile, tbl, dl, 0, lg);
        }
        lg->debug("[APT] --- end sprite tree ---");
      }
    }

    // Debug scene: DL text overlay in the top-left corner (outside canvas bounds).
    if (debugOverlay && !nodes.empty()) {
      QString dlText = QStringLiteral("─ DL frame %1 ─\n").arg(frame->index);
      for (const auto& n : nodes) {
        const char* k = "?";
        switch (n.kind) {
          case gf::apt::RenderNode::Kind::Shape:    k = "Shp"; break;
          case gf::apt::RenderNode::Kind::Image:    k = "Img"; break;
          case gf::apt::RenderNode::Kind::EditText: k = "Txt"; break;
          case gf::apt::RenderNode::Kind::Sprite:   k = "Spr"; break;
          case gf::apt::RenderNode::Kind::Movie:    k = "Mov"; break;
          case gf::apt::RenderNode::Kind::Unknown:  k = "???"; break;
          default: break;
        }
        dlText += QStringLiteral("D%-3 C%-4 %5 tx=%.0f ty=%.0f\n")
                      .arg(n.placementDepth).arg(n.characterId).arg(k)
                      .arg(n.worldTransform.tx).arg(n.worldTransform.ty);
      }
      auto* dlItem = m_aptPreviewScene->addSimpleText(dlText);
      dlItem->setBrush(QBrush(QColor(180, 220, 180, 220)));
      // Place it to the left of the canvas.
      dlItem->setPos(-dlItem->boundingRect().width() - 8.0, 0.0);
    }
  }

  const std::vector<gf::apt::AptFrame>* renderSourceFrames = nullptr;
  if (m_aptCharPreviewIdx >= 0
      && m_aptCharPreviewIdx < static_cast<int>(m_currentAptFile->characters.size())) {
    const auto& ch = m_currentAptFile->characters[static_cast<std::size_t>(m_aptCharPreviewIdx)];
    if (!ch.frames.empty() && frame->index < ch.frames.size() && &ch.frames[frame->index] == frame)
      renderSourceFrames = &ch.frames;
  }
  if (!renderSourceFrames && frame->index < m_currentAptFile->frames.size()
      && &m_currentAptFile->frames[frame->index] == frame) {
    renderSourceFrames = &m_currentAptFile->frames;
  }

  if (renderSourceFrames) {
    const gf::apt::AptFrame resolvedFrame =
        gf::apt::build_display_list_frame(*renderSourceFrames, frame->index);
    renderAptResolvedFrameRecursive(resolvedFrame, QTransform(), table,
                                    -1, highlightedPlacementIndex, debugOverlay, 0, QStringLiteral("Root"));
  } else {
    renderAptResolvedFrameRecursive(*frame, QTransform(), table,
                                    -1, highlightedPlacementIndex, debugOverlay, 0, QStringLiteral("Root"));
  }

  // Option C: if every node is a Sprite/Movie/Unknown placeholder (no leaf content),
  // show an explanatory message so the user knows to browse individual characters.
  // This happens when the root frame places only runtime-assembled containers (e.g.
  // ActionScript-driven scorebugs where character 0's Frame 0 places an empty Sprite).
  {
    const bool allPlaceholders = !nodes.empty() && std::all_of(nodes.begin(), nodes.end(),
        [](const gf::apt::RenderNode& n) {
          return n.kind == gf::apt::RenderNode::Kind::Sprite
              || n.kind == gf::apt::RenderNode::Kind::Movie
              || n.kind == gf::apt::RenderNode::Kind::Unknown;
        });

    if (allPlaceholders && !suppressContainerMsg) {
      // Build a short summary of the placeholder(s) for context.
      const gf::apt::RenderNode& first = nodes[0];
      const std::string& iname = first.instanceName;
      const QString hint = iname.empty()
          ? QString("C%1").arg(first.characterId)
          : QString::fromStdString(iname);
      const QString msg =
          QString("No static content in this frame.\n"
                  "\"%1\" is a runtime-only container assembled by ActionScript.\n"
                  "Select a character in the Characters section to preview its static frames.")
          .arg(hint);
      auto* msgItem = m_aptPreviewScene->addSimpleText(msg);
      msgItem->setBrush(QBrush(QColor(160, 165, 180)));
      // Centre it vertically in the canvas.
      const qreal my = sceneHeight * 0.5 - msgItem->boundingRect().height() * 0.5;
      msgItem->setPos(24.0, my);
    }

    // In debug mode, always show node count in the top-right corner.
    if (debugOverlay) {
      const QString diagText = nodes.empty()
          ? QString("0 nodes — %1 placements in frame").arg(frame->placements.size())
          : QString("%1 node%2").arg(nodes.size()).arg(nodes.size() == 1 ? "" : "s");
      auto* diagLabel = m_aptPreviewScene->addSimpleText(diagText);
      diagLabel->setBrush(QBrush(QColor(100, 200, 120, 200)));
      diagLabel->setPos(sceneWidth - diagLabel->boundingRect().width() - 8.0, 8.0);
    }
  }

  syncAptEditorSceneContext();

  // Expand scene rect to include any content that overflowed the canvas bounds
  // (runtime-fallback nodes laid out in a grid may extend below the 1280×720 area).
  if (!m_aptPreviewScene->items().isEmpty()) {
    const QRectF itemsBounds = m_aptPreviewScene->itemsBoundingRect();
    const QRectF current     = m_aptPreviewScene->sceneRect();
    m_aptPreviewScene->setSceneRect(current.united(itemsBounds).adjusted(-40, -40, 40, 40));
  }

  // Zoom handling: on initial load (fitToContent=true) or when the view is still in
  // its default fit-mode, auto-fit the content.  Once the user has manually zoomed,
  // frame navigation preserves their zoom level (m_fitActive=false).
  if (m_aptPreviewView) {
    auto* aptView = qobject_cast<AptPreviewView*>(m_aptPreviewView);
    if (fitToContent || !aptView) {
      // Explicit fit request — always fit and re-enable fit-on-resize.
      if (aptView) aptView->fitContent();
      else m_aptPreviewView->fitInView(m_aptPreviewScene->sceneRect(), Qt::KeepAspectRatio);
    }
    // If aptView is in fit mode (user hasn't manually zoomed), keep fitting.
    // Otherwise leave the zoom where it is (don't call fitInView at all).
  }
}

void MainWindow::setAptFrameIndex(int idx) {
  if (!m_currentAptFile) return;

  // Character-preview context: navigate within the selected character's frames.
  if (m_aptCharPreviewIdx >= 0
      && m_aptCharPreviewIdx < static_cast<int>(m_currentAptFile->characters.size())) {
    const auto& ch = m_currentAptFile->characters[static_cast<std::size_t>(m_aptCharPreviewIdx)];
    const int count = static_cast<int>(ch.frames.size());
    if (count == 0) return;
    idx = std::max(0, std::min(idx, count - 1));
    m_aptCurrentFrameIndex = idx;
    if (m_aptFrameSpin) {
      m_aptFrameSpin->blockSignals(true);
      m_aptFrameSpin->setValue(idx);
      m_aptFrameSpin->blockSignals(false);
    }
    if (m_aptPrevFrameAction) m_aptPrevFrameAction->setEnabled(idx > 0);
    if (m_aptNextFrameAction) m_aptNextFrameAction->setEnabled(idx + 1 < count);
    const std::vector<gf::apt::AptCharacter>* table =
        ch.nested_characters.empty() ? nullptr : &ch.nested_characters;
    renderAptFrameToScene(&ch.frames[static_cast<std::size_t>(idx)], -1, table);
    return;
  }

  // Root-movie context.
  const int count = static_cast<int>(m_currentAptFile->frames.size());
  if (count == 0) return;
  idx = std::max(0, std::min(idx, count - 1));
  m_aptCurrentFrameIndex = idx;
  if (m_aptFrameSpin) {
    m_aptFrameSpin->blockSignals(true);
    m_aptFrameSpin->setValue(idx);
    m_aptFrameSpin->blockSignals(false);
  }
  if (m_aptPrevFrameAction) m_aptPrevFrameAction->setEnabled(idx > 0);
  if (m_aptNextFrameAction) m_aptNextFrameAction->setEnabled(idx + 1 < count);
  const int hi = selectedAptPlacementIndex().value_or(-1);
  renderAptFrameToScene(&m_currentAptFile->frames[static_cast<std::size_t>(idx)], hi);
}

void MainWindow::refreshAptPreview() {
  if (!m_aptPreviewScene) return;
  if (m_aptPreviewInProgress) return;
  m_aptPreviewInProgress = true;
  const auto clearInProgress = qScopeGuard([this]{ m_aptPreviewInProgress = false; });

  if (!m_currentAptFile || m_currentAptFile->frames.empty()) {
    renderAptFrameToScene(nullptr, -1);
    return;
  }

  // Helper: switch the frame-navigation controls to a new context.
  // frameCount = total frames available; currentFrame = frame to show now.
  auto syncNavControls = [&](int frameCount, int currentFrame, const QString& tooltip) {
    if (m_aptFrameSpin) {
      m_aptFrameSpin->blockSignals(true);
      m_aptFrameSpin->setRange(0, std::max(0, frameCount - 1));
      m_aptFrameSpin->setValue(currentFrame);
      m_aptFrameSpin->setEnabled(frameCount > 0);
      m_aptFrameSpin->setToolTip(tooltip);
      m_aptFrameSpin->blockSignals(false);
    }
    if (m_aptFrameCountLabel)
      m_aptFrameCountLabel->setText(QString(" / %1 ").arg(frameCount));
    if (m_aptPrevFrameAction) m_aptPrevFrameAction->setEnabled(currentFrame > 0);
    if (m_aptNextFrameAction) m_aptNextFrameAction->setEnabled(currentFrame + 1 < frameCount);
  };

  if (m_aptTree) {
    QTreeWidgetItem* cur = m_aptTree->currentItem();
    if (cur) {
      const int nodeType  = cur->data(0, kAptRoleNodeType).toInt();
      const int ownerKind = cur->data(0, kAptRoleOwnerKind).toInt();
      const int ownerIdx  = cur->data(0, kAptRoleOwnerIndex).toInt();
      const int nodeIdx   = cur->data(0, kAptRoleNodeIndex).toInt();

      // ── Character node selected directly (ownerKind==0, kAptNodeCharacter) ──
      // Enter character-preview context so the frame spinner navigates this character.
      if (nodeType == kAptNodeCharacter && nodeIdx >= 0
          && nodeIdx < static_cast<int>(m_currentAptFile->characters.size())) {
        const auto& ch = m_currentAptFile->characters[static_cast<std::size_t>(nodeIdx)];

        if (ch.type == 5 || ch.type == 9) {
          // Sprite / Movie with parseable frames.
          if (!ch.frames.empty()) {
            m_aptCharPreviewIdx = nodeIdx;
            m_aptCurrentFrameIndex = 0;
            const int fc = static_cast<int>(ch.frames.size());
            syncNavControls(fc, 0,
                QString("Frame index within Character %1 (0-based)").arg(nodeIdx));
            const std::vector<gf::apt::AptCharacter>* table =
                ch.nested_characters.empty() ? nullptr : &ch.nested_characters;
            renderAptFrameToScene(&ch.frames[0], -1, table);
          } else {
            // Frameless Sprite/Movie — runtime-only container.
            // Build a synthetic placement so renderAptCharacterRecursive is called,
            // which activates doRuntimeFallback and renders the scaffold preview.
            m_aptCharPreviewIdx = -1;
            syncNavControls(0, 0, QStringLiteral("No frame navigation — runtime-only container"));
            gf::apt::AptFrame synthFrame;
            synthFrame.frameitemcount = 1;
            gf::apt::AptPlacement synthPl;
            synthPl.character = static_cast<std::uint32_t>(nodeIdx);
            synthPl.depth = 0;
            synthFrame.placements.push_back(std::move(synthPl));
            const std::vector<gf::apt::AptCharacter>* table =
                ch.nested_characters.empty() ? nullptr : &ch.nested_characters;
            // suppressContainerMsg=true: the scaffold renderer draws its own overlay;
            // the generic "runtime-only container" message would be redundant.
            renderAptFrameToScene(&synthFrame, -1, table, {}, true, true);
          }
          return;
        }

        // Leaf character (Shape, Image, EditText, Button, Text, Morph).
        // Render the character standalone at identity transform via a synthetic placement.
        if (ch.type != 0) {
          m_aptCharPreviewIdx = -1;
          // Disable frame navigation — leaves have no frames.
          syncNavControls(0, 0, QStringLiteral("No frame navigation — leaf character"));
          gf::apt::AptFrame synthFrame;
          synthFrame.frameitemcount = 1;
          gf::apt::AptPlacement pl;
          pl.character  = static_cast<std::uint32_t>(nodeIdx);
          pl.depth      = 0;
          // AptTransform defaults to identity (scale=1, everything else 0).
          synthFrame.placements.push_back(std::move(pl));
          // fitToContent=true so the view zooms to the character bounds
          // rather than showing it tiny inside the full 1280×720 canvas.
          renderAptFrameToScene(&synthFrame, -1, nullptr, {}, true);
          return;
        }
        // type==0 (import slot): fall through to root-movie rendering.
      }

      // ── Frame or Placement node under a character (ownerKind==1) ──
      // Sync character context and render the specified character frame.
      if (ownerKind == 1
          && (nodeType == kAptNodeFrame || nodeType == kAptNodePlacement)
          && ownerIdx >= 0
          && ownerIdx < static_cast<int>(m_currentAptFile->characters.size())) {
        const auto& ch = m_currentAptFile->characters[static_cast<std::size_t>(ownerIdx)];
        const int fi = nodeIdx; // frame index stored in kAptRoleNodeIndex for both node types
        if (fi >= 0 && fi < static_cast<int>(ch.frames.size())) {
          m_aptCharPreviewIdx = ownerIdx;
          m_aptCurrentFrameIndex = fi;
          const int fc = static_cast<int>(ch.frames.size());
          syncNavControls(fc, fi,
              QString("Frame index within Character %1 (0-based)").arg(ownerIdx));
          const int hiPl = (nodeType == kAptNodePlacement)
              ? cur->data(0, kAptRolePlacementIndex).toInt() : -1;
          const std::vector<gf::apt::AptCharacter>* table =
              ch.nested_characters.empty() ? nullptr : &ch.nested_characters;
          renderAptFrameToScene(&ch.frames[static_cast<std::size_t>(fi)], hiPl, table);
          return;
        }
      }
    }
  }

  // ── Root-movie context ────────────────────────────────────────────────────
  // Clear character context and render the root movie's current frame.
  if (m_aptCharPreviewIdx != -1) {
    m_aptCharPreviewIdx = -1;
    const int fc = static_cast<int>(m_currentAptFile->frames.size());
    syncNavControls(fc, m_aptCurrentFrameIndex,
                    QStringLiteral("Root movie frame index (0-based)"));
  }

  // A root-movie frame node in the tree overrides the spinner.
  if (auto treeFrame = selectedAptFrameIndex()) {
    if (*treeFrame != m_aptCurrentFrameIndex) {
      m_aptCurrentFrameIndex = *treeFrame;
      if (m_aptFrameSpin) {
        m_aptFrameSpin->blockSignals(true);
        m_aptFrameSpin->setValue(m_aptCurrentFrameIndex);
        m_aptFrameSpin->blockSignals(false);
      }
      const int count = static_cast<int>(m_currentAptFile->frames.size());
      if (m_aptPrevFrameAction) m_aptPrevFrameAction->setEnabled(m_aptCurrentFrameIndex > 0);
      if (m_aptNextFrameAction) m_aptNextFrameAction->setEnabled(m_aptCurrentFrameIndex + 1 < count);
    }
  }

  const int fi = std::max(0, std::min(m_aptCurrentFrameIndex,
                                      static_cast<int>(m_currentAptFile->frames.size()) - 1));
  const int hi = selectedAptPlacementIndex().value_or(-1);
  renderAptFrameToScene(&m_currentAptFile->frames[static_cast<std::size_t>(fi)], hi);
}

void MainWindow::syncAptEditorSceneContext() {
  if (!m_aptPreviewScene || !m_currentAptFile) return;

  gf::gui::apt_editor::AptPreviewScene::Context ctx;
  ctx.file = &*m_currentAptFile;
  ctx.debugOverlay = m_aptDebugAction && m_aptDebugAction->isChecked();

  if (m_aptCharPreviewIdx >= 0
      && m_aptCharPreviewIdx < static_cast<int>(m_currentAptFile->characters.size())) {
    auto& ch = m_currentAptFile->characters[static_cast<std::size_t>(m_aptCharPreviewIdx)];
    ctx.timelineFrames = &ch.frames;
    ctx.characterTable = ch.nested_characters.empty() ? &m_currentAptFile->characters : &ch.nested_characters;
    ctx.ownerKind = 1;
    ctx.ownerIndex = m_aptCharPreviewIdx;
    ctx.frameIndex = std::max(0, std::min(m_aptCurrentFrameIndex, static_cast<int>(ch.frames.size()) - 1));
  } else {
    ctx.timelineFrames = &m_currentAptFile->frames;
    ctx.characterTable = &m_currentAptFile->characters;
    ctx.ownerKind = 0;
    ctx.ownerIndex = -1;
    ctx.frameIndex = std::max(0, std::min(m_aptCurrentFrameIndex, static_cast<int>(m_currentAptFile->frames.size()) - 1));
  }

  m_aptPreviewScene->setContext(ctx);
  m_aptPreviewScene->syncSelectionFromExternal(selectedAptPlacementIndex().value_or(-1));
}

QTreeWidgetItem* MainWindow::findAptPlacementTreeItem(int ownerKind, int ownerIndex, int frameIndex, int placementIndex) const {
  if (!m_aptTree) return nullptr;
  for (int i = 0; i < m_aptTree->topLevelItemCount(); ++i) {
    QTreeWidgetItem* root = m_aptTree->topLevelItem(i);
    QList<QTreeWidgetItem*> stack{root};
    while (!stack.isEmpty()) {
      QTreeWidgetItem* item = stack.takeLast();
      if (item->data(0, kAptRoleNodeType).toInt() == kAptNodePlacement
          && item->data(0, kAptRoleOwnerKind).toInt() == ownerKind
          && item->data(0, kAptRoleOwnerIndex).toInt() == ownerIndex
          && item->data(0, kAptRoleNodeIndex).toInt() == frameIndex
          && item->data(0, kAptRolePlacementIndex).toInt() == placementIndex) {
        return item;
      }
      for (int c = 0; c < item->childCount(); ++c) stack.append(item->child(c));
    }
  }
  return nullptr;
}

void MainWindow::refreshAptPlacementTreeLabels() {
  if (!m_currentAptFile || !m_aptTree) return;

  auto findFrameNode = [&](int ownerKind, int ownerIndex, int frameIndex) -> QTreeWidgetItem* {
    for (int i = 0; i < m_aptTree->topLevelItemCount(); ++i) {
      QList<QTreeWidgetItem*> stack{m_aptTree->topLevelItem(i)};
      while (!stack.isEmpty()) {
        QTreeWidgetItem* item = stack.takeLast();
        if (item->data(0, kAptRoleNodeType).toInt() == kAptNodeFrame
            && item->data(0, kAptRoleOwnerKind).toInt() == ownerKind
            && item->data(0, kAptRoleOwnerIndex).toInt() == ownerIndex
            && item->data(0, kAptRoleNodeIndex).toInt() == frameIndex) {
          return item;
        }
        for (int c = 0; c < item->childCount(); ++c) stack.append(item->child(c));
      }
    }
    return nullptr;
  };

  auto rebuildFrameChildren = [&](const std::vector<gf::apt::AptFrame>& frames, int ownerKind, int ownerIndex, const QString& ownerLabel) {
    for (std::size_t fi = 0; fi < frames.size(); ++fi) {
      const auto& frame = frames[fi];
      QTreeWidgetItem* frameNode = findFrameNode(ownerKind, ownerIndex, static_cast<int>(fi));
      if (!frameNode) continue;
      while (frameNode->childCount() > 0) {
        delete frameNode->takeChild(frameNode->childCount() - 1);
      }
      frameNode->setText(1, QString::number(frame.frameitemcount));
      frameNode->setData(0, Qt::UserRole,
                         QString("Type: Frame\n"
                                 "Index: %1\n"
                                 "Frame Item Count: %2\n"
                                 "Placement Count: %3\n"
                                 "Items Offset: 0x%4\n"
                                 "Offset: 0x%5")
                             .arg(qulonglong(fi))
                             .arg(frame.frameitemcount)
                             .arg(frame.placements.size())
                             .arg(QString::number(qulonglong(frame.items_offset), 16).toUpper())
                             .arg(QString::number(qulonglong(frame.offset), 16).toUpper()));
      for (std::size_t pi = 0; pi < frame.placements.size(); ++pi) {
        const auto& placement = frame.placements[pi];
        auto* child = new QTreeWidgetItem(frameNode, QStringList() << aptPlacementTreeLabel(placement) << aptPlacementValueText(placement));
        child->setData(0, Qt::UserRole,
                       aptPlacementDetailText(placement, ownerLabel, static_cast<int>(fi), static_cast<int>(pi)));
        child->setData(0, kAptRoleNodeType, kAptNodePlacement);
        child->setData(0, kAptRoleNodeIndex, static_cast<int>(fi));
        child->setData(0, kAptRoleOwnerKind, ownerKind);
        child->setData(0, kAptRoleOwnerIndex, ownerIndex);
        child->setData(0, kAptRolePlacementIndex, static_cast<int>(pi));
      }
    }
  };

  rebuildFrameChildren(m_currentAptFile->frames, 0, -1, QStringLiteral("Root Movie"));
  for (std::size_t ci = 0; ci < m_currentAptFile->characters.size(); ++ci) {
    rebuildFrameChildren(m_currentAptFile->characters[ci].frames, 1, static_cast<int>(ci), QString("Character %1").arg(ci));
  }
}

void MainWindow::onAptSceneSelectionChanged(int placementIndex) {
  if (!m_currentAptFile || !m_aptTree) return;
  const int ownerKind = (m_aptCharPreviewIdx >= 0) ? 1 : 0;
  const int ownerIndex = (m_aptCharPreviewIdx >= 0) ? m_aptCharPreviewIdx : -1;
  if (placementIndex < 0) return;
  if (QTreeWidgetItem* item = findAptPlacementTreeItem(ownerKind, ownerIndex, m_aptCurrentFrameIndex, placementIndex)) {
    m_suppressSelectionChange = true;
    m_aptTree->setCurrentItem(item);
    m_suppressSelectionChange = false;
    syncAptPropEditorFromItem(item);
  }
}

void MainWindow::onAptScenePlacementEdited(int placementIndex, bool interactive) {
  if (!m_currentAptFile) return;
  refreshAptPlacementTreeLabels();
  m_aptDirty = true;
  setDirty(true);
  if (QTreeWidgetItem* item = findAptPlacementTreeItem((m_aptCharPreviewIdx >= 0) ? 1 : 0,
                                                       (m_aptCharPreviewIdx >= 0) ? m_aptCharPreviewIdx : -1,
                                                       m_aptCurrentFrameIndex,
                                                       placementIndex)) {
    syncAptPropEditorFromItem(item);
  }
  if (!interactive) {
    refreshAptPreview();
  }
}

void MainWindow::syncAptPropEditorFromItem(QTreeWidgetItem* item) {
  if (!m_aptPropStack || !m_aptDetails) return;

  m_aptUpdatingUi = true;
  if (m_aptApplyAction) m_aptApplyAction->setEnabled(false);

  if (!item || !m_currentAptFile) {
    m_aptDetails->setPlainText(item ? item->data(0, Qt::UserRole).toString() : QString());
    m_aptPropStack->setCurrentWidget(m_aptDetails);
    m_aptUpdatingUi = false;
    return;
  }

  const int nodeType = item->data(0, kAptRoleNodeType).toInt();
  const int index = item->data(0, kAptRoleNodeIndex).toInt();
  const int ownerKind = item->data(0, kAptRoleOwnerKind).toInt();
  const int ownerIndex = item->data(0, kAptRoleOwnerIndex).toInt();
  const int placementIndex = item->data(0, kAptRolePlacementIndex).toInt();
  const auto& file = *m_currentAptFile;

  switch (nodeType) {
    case kAptNodeSummary:
      m_aptSumWidthSpin->setValue(static_cast<int>(file.summary.screensizex));
      m_aptSumHeightSpin->setValue(static_cast<int>(file.summary.screensizey));
      m_aptSumFrameCountLabel->setText(QString::number(file.frames.size()));
      m_aptSumCharCountLabel->setText(QString::number(file.characters.size()));
      m_aptSumImportCountLabel->setText(QString::number(file.imports.size()));
      m_aptSumExportCountLabel->setText(QString::number(file.exports.size()));
      m_aptSumOffsetLabel->setText(QString("0x%1").arg(QString::number(qulonglong(file.summary.aptdataoffset), 16).toUpper()));
      m_aptPropStack->setCurrentWidget(m_aptSummaryPage);
      break;

    case kAptNodeImport:
      if (index >= 0 && index < static_cast<int>(file.imports.size())) {
        const auto& im = file.imports[static_cast<std::size_t>(index)];
        m_aptImportMovieEdit->setText(QString::fromStdString(im.movie));
        m_aptImportNameEdit->setText(QString::fromStdString(im.name));
        m_aptImportCharLabel->setText(QString::number(im.character));
        m_aptImportOffsetLabel->setText(QString("0x%1").arg(QString::number(qulonglong(im.offset), 16).toUpper()));
        m_aptPropStack->setCurrentWidget(m_aptImportPage);
      } else {
        m_aptDetails->setPlainText(item->data(0, Qt::UserRole).toString());
        m_aptPropStack->setCurrentWidget(m_aptDetails);
      }
      break;

    case kAptNodeExport:
      if (index >= 0 && index < static_cast<int>(file.exports.size())) {
        const auto& ex = file.exports[static_cast<std::size_t>(index)];
        m_aptExportNameEdit->setText(QString::fromStdString(ex.name));
        m_aptExportCharLabel->setText(QString::number(ex.character));
        m_aptExportOffsetLabel->setText(QString("0x%1").arg(QString::number(qulonglong(ex.offset), 16).toUpper()));
        m_aptPropStack->setCurrentWidget(m_aptExportPage);
      } else {
        m_aptDetails->setPlainText(item->data(0, Qt::UserRole).toString());
        m_aptPropStack->setCurrentWidget(m_aptDetails);
      }
      break;

    case kAptNodeCharacter:
      if (index >= 0 && index < static_cast<int>(file.characters.size())) {
        const auto& ch = file.characters[static_cast<std::size_t>(index)];
        const std::uint32_t charIdx = static_cast<std::uint32_t>(index);

        m_aptCharTypeLabel->setText(
            QString("%1 (%2)").arg(QString::fromStdString(gf::apt::aptCharTypeName(ch.type))).arg(ch.type));
        m_aptCharSigLabel->setText(
            QString("0x%1").arg(QString::number(qulonglong(ch.signature), 16).toUpper()));
        m_aptCharOffsetLabel->setText(
            QString("0x%1").arg(QString::number(qulonglong(ch.offset), 16).toUpper()));

        if (m_aptCharFrameCountLabel) {
          m_aptCharFrameCountLabel->setText(
              (ch.type == 5 || ch.type == 9)
              ? QString::number(ch.frames.size())
              : QStringLiteral("—"));
        }

        if (m_aptCharBoundsLabel) {
          m_aptCharBoundsLabel->setText(
              ch.bounds
              ? QString("L=%1 T=%2 R=%3 B=%4")
                    .arg(ch.bounds->left,  0, 'f', 1)
                    .arg(ch.bounds->top,   0, 'f', 1)
                    .arg(ch.bounds->right, 0, 'f', 1)
                    .arg(ch.bounds->bottom,0, 'f', 1)
              : QStringLiteral("(none)"));
        }

        if (m_aptCharImportLabel) {
          if (ch.type == 0) {
            // Import placeholder: find the matching import record.
            QString impText = QStringLiteral("(unresolved)");
            for (const auto& imp : file.imports) {
              if (imp.character == charIdx) {
                impText = QString("%1 :: %2")
                    .arg(QString::fromStdString(imp.movie),
                         QString::fromStdString(imp.name));
                break;
              }
            }
            m_aptCharImportLabel->setText(impText);
          } else {
            // Show export linkage name if this character is exported.
            QString expText = QStringLiteral("—");
            for (const auto& exp : file.exports) {
              if (exp.character == charIdx) {
                expText = QStringLiteral("export: ") + QString::fromStdString(exp.name);
                break;
              }
            }
            m_aptCharImportLabel->setText(expText);
          }
        }

        // ── Scaffold breakdown for runtime-only containers ────────────────
        // Shown when a frameless Sprite/Movie is selected (no static display list).
        // Scans action bytecodes to find attachMovie/duplicateMovieClip calls and
        // builds a human-readable summary of how the scaffold was constructed.
        if (m_aptScaffoldDump) {
          const bool isRuntimeContainer = (ch.type == 5 || ch.type == 9) && ch.frames.empty();
          m_aptScaffoldDump->setVisible(isRuntimeContainer);
          if (isRuntimeContainer) {
            // Use the shared cache — same result that renderAptCharacterRecursive uses.
            const gf::apt::AptActionHints& hints = getCachedAptHints(charIdx, ch, file.characters);

            // Find export name for this container.
            QString exportNameStr;
            for (const auto& ex : file.exports)
              if (ex.character == charIdx) { exportNameStr = QString::fromStdString(ex.name); break; }

            // Helper: probe the visual tier of a resolved character.
            //   0=leaf, 1=framed-container, 2=runtime-container, 3=import
            const std::vector<gf::apt::AptCharacter>& scanTable =
                (ch.type == 9 && !ch.nested_characters.empty())
                ? ch.nested_characters
                : file.characters;
            auto probeTier = [&](std::uint32_t cid) -> int {
              if (cid >= scanTable.size()) return -1;
              const auto& c2 = scanTable[cid];
              if (c2.type == 0) return 3;
              if (c2.type != 5 && c2.type != 9) return 0;
              for (const auto& f2 : c2.frames)
                if (!f2.placements.empty()) return 1;
              return 2;
            };
            auto tierLabel = [](int t) -> QLatin1String {
              switch (t) {
                case 0: return QLatin1String("leaf");
                case 1: return QLatin1String("framed");
                case 2: return QLatin1String("runtime-ctr");
                case 3: return QLatin1String("import");
                default: return QLatin1String("?");
              }
            };

            // ── Rebuild the attach tree for the breakdown panel ──────────────
            // Mirrors buildAttachTree() inside renderAptCharacterRecursive but
            // uses the local `hints`, `file`, `scanTable` and `probeTier` lambda.
            static const std::unordered_set<std::string> kInstCalls =
                {"attachMovie", "duplicateMovieClip", "createEmptyMovieClip"};
            static const std::unordered_set<std::string> kGotoCalls =
                {"gotoAndStop", "gotoAndPlay"};
            static const std::unordered_set<std::string> kKnownProps =
                {"_x","_y","_alpha","_visible","_xscale","_yscale","_rotation"};

            static constexpr double kDumpNaN = std::numeric_limits<double>::quiet_NaN();
            struct DumpNode {
              QString callLine;
              QString resolvedStr;
              QString instanceName;
              QString gotoLabel;
              int     vizTier    = -1;
              int     depth      = -1;
              bool    resolvedLocal = true;
              bool    hasSubActions = false;
              bool   propX=false; double xVal=kDumpNaN;
              bool   propY=false; double yVal=kDumpNaN;
              bool   propAlpha=false; double alphaVal=kDumpNaN;
              bool   propVis=false;
              bool   propScale=false; double scaleVal=kDumpNaN;
              bool   propRot=false; double rotVal=kDumpNaN;
            };
            std::vector<DumpNode> treeNodes;
            std::unordered_map<std::string, std::size_t> dumpInstMap;

            for (const auto& dc : hints.detected_calls) {
              if (!kInstCalls.count(dc.call_name)) continue;
              DumpNode dn;
              const QString sym  = dc.arg_strings.empty()
                  ? QStringLiteral("?") : QString::fromStdString(dc.arg_strings[0]);
              const QString inst = (dc.arg_strings.size() > 1 && !dc.arg_strings[1].empty())
                  ? QString::fromStdString(dc.arg_strings[1]) : QString();
              dn.instanceName = inst;
              dn.callLine = QString::fromStdString(dc.call_name)
                          + QStringLiteral("(\"") + sym + QStringLiteral("\")");

              if (!dc.arg_strings.empty()) {
                for (const auto& ex : file.exports)
                  if (ex.name == dc.arg_strings[0]) {
                    dn.vizTier = probeTier(ex.character);
                    dn.resolvedStr = QString("C%1 [%2]")
                        .arg(ex.character).arg(tierLabel(dn.vizTier));
                    if (dn.vizTier == 3) {
                      for (const auto& im : file.imports)
                        if (im.character == ex.character) {
                          dn.resolvedStr += QStringLiteral(" [")
                              + QString::fromStdString(im.movie) + QStringLiteral("/")
                              + QString::fromStdString(im.name) + QStringLiteral("]");
                          dn.resolvedLocal = false;
                        }
                    }
                    break;
                  }
                if (dn.resolvedStr.isEmpty())
                  for (const auto& im : file.imports)
                    if (im.name == dc.arg_strings[0]) {
                      dn.vizTier = 3;
                      dn.resolvedLocal = false;
                      dn.resolvedStr = QString("C%1 [import: %2/%3]")
                          .arg(im.character)
                          .arg(QString::fromStdString(im.movie))
                          .arg(QString::fromStdString(im.name));
                      break;
                    }
                if (dn.resolvedStr.isEmpty()) dn.resolvedStr = QStringLiteral("(unresolved)");
              }

              // Depth from numeric arg[0].
              if (!dc.arg_numbers.empty())
                dn.depth = static_cast<int>(dc.arg_numbers[0]);

              if (!inst.isEmpty()) dumpInstMap[inst.toStdString()] = treeNodes.size();
              treeNodes.push_back(std::move(dn));
            }

            // gotoAndStop/gotoAndPlay → first unset tree node.
            for (const auto& dc : hints.detected_calls) {
              if (!kGotoCalls.count(dc.call_name) || dc.arg_strings.empty()) continue;
              for (auto& dn : treeNodes) {
                if (dn.gotoLabel.isEmpty() && dn.vizTier >= 0) {
                  dn.gotoLabel = QString::fromStdString(dc.call_name + "(\"" + dc.arg_strings[0] + "\")");
                  break;
                }
              }
            }
            // setTarget → hasSubActions.
            for (const auto& as : hints.strings) {
              if (as.role != gf::apt::AptStringRole::TargetPath) continue;
              auto it = dumpInstMap.find(as.value);
              if (it != dumpInstMap.end()) treeNodes[it->second].hasSubActions = true;
            }
            // Property hints.
            std::string dumpActiveTarget;
            for (const auto& as : hints.strings) {
              if (as.role == gf::apt::AptStringRole::TargetPath) {
                dumpActiveTarget = as.value;
              } else if (as.role == gf::apt::AptStringRole::MemberName
                         && kKnownProps.count(as.value)) {
                DumpNode* tgt = nullptr;
                if (!dumpActiveTarget.empty()) {
                  auto it = dumpInstMap.find(dumpActiveTarget);
                  if (it != dumpInstMap.end()) tgt = &treeNodes[it->second];
                }
                if (!tgt && !treeNodes.empty()) tgt = &treeNodes[0];
                if (tgt) {
                  if      (as.value == "_x")                               tgt->propX     = true;
                  else if (as.value == "_y")                               tgt->propY     = true;
                  else if (as.value == "_alpha")                           tgt->propAlpha = true;
                  else if (as.value == "_visible")                         tgt->propVis   = true;
                  else if (as.value == "_xscale" || as.value == "_yscale") tgt->propScale = true;
                  else if (as.value == "_rotation")                        tgt->propRot   = true;
                }
              }
            }

            // Numeric property values from set_members hints.
            for (const auto& smh : hints.set_members) {
              DumpNode* tgt = nullptr;
              if (!smh.target.empty()) {
                auto it = dumpInstMap.find(smh.target);
                if (it != dumpInstMap.end()) tgt = &treeNodes[it->second];
              }
              if (!tgt && !treeNodes.empty()) tgt = &treeNodes[0];
              if (!tgt) continue;
              if      (smh.property == "_x")   { tgt->propX = true; tgt->xVal = smh.value; }
              else if (smh.property == "_y")   { tgt->propY = true; tgt->yVal = smh.value; }
              else if (smh.property == "_alpha"){ tgt->propAlpha = true; tgt->alphaVal = smh.value; }
              else if (smh.property == "_xscale" || smh.property == "_yscale")
                { tgt->propScale = true; tgt->scaleVal = smh.value; }
              else if (smh.property == "_rotation")
                { tgt->propRot = true; tgt->rotVal = smh.value; }
            }

            // Tier counts.
            int nLeaf=0, nFramed=0, nRuntime=0, nImp=0, nUnres=0;
            for (const auto& dn : treeNodes) {
              if      (dn.vizTier == 0) ++nLeaf;
              else if (dn.vizTier == 1) ++nFramed;
              else if (dn.vizTier == 2) ++nRuntime;
              else if (dn.vizTier == 3) ++nImp;
              else                      ++nUnres;
            }

            // Near-call strings not already in tree nodes.
            auto alreadyCovered = [&](const std::string& val) {
              for (const auto& dc : hints.detected_calls)
                if (!dc.arg_strings.empty() && dc.arg_strings[0] == val) return true;
              return false;
            };

            // ── Build breakdown text ──────────────────────────────────────
            QString stext;
            stext += QString("Runtime attach tree: C%1 %2%3\n")
                .arg(charIdx)
                .arg(QString::fromStdString(gf::apt::aptCharTypeName(ch.type)))
                .arg(exportNameStr.isEmpty() ? QString()
                     : QStringLiteral(" [export: ") + exportNameStr + QStringLiteral("]"));
            stext += QString("%1 opcode%2 scanned")
                .arg(hints.opcode_count).arg(hints.opcode_count == 1 ? "" : "s");
            {
              QStringList parts;
              if (nLeaf)    parts << QString("%1 leaf").arg(nLeaf);
              if (nFramed)  parts << QString("%1 framed").arg(nFramed);
              if (nRuntime) parts << QString("%1 runtime-ctr").arg(nRuntime);
              if (nImp)     parts << QString("%1 import").arg(nImp);
              if (nUnres)   parts << QString("%1 unresolved").arg(nUnres);
              if (!parts.isEmpty())
                stext += QStringLiteral(" | ") + parts.join(QStringLiteral(" \u2022 "));
            }
            stext += QStringLiteral("\n\n");

            if (!treeNodes.empty()) {
              stext += QStringLiteral("Attached children (action-grounded):\n");
              for (std::size_t ni = 0; ni < treeNodes.size(); ++ni) {
                const auto& dn = treeNodes[ni];
                stext += QString("[%1] ").arg(ni + 1) + dn.callLine;
                if (!dn.instanceName.isEmpty())
                  stext += QStringLiteral(" inst=\"") + dn.instanceName + QStringLiteral("\"");
                stext += QStringLiteral(" \u2192 ") + dn.resolvedStr + QStringLiteral("\n");
                // Sub-detail lines.
                if (dn.depth >= 0)
                  stext += QStringLiteral("    \u25CF depth ") + QString::number(dn.depth) + QStringLiteral("\n");
                if (dn.hasSubActions)
                  stext += QStringLiteral("    \u25CF setTarget points here\n");
                if (!dn.gotoLabel.isEmpty())
                  stext += QStringLiteral("    \u25CF ") + dn.gotoLabel + QStringLiteral("\n");
                // Numeric property values — show value when recovered, "?" otherwise.
                auto fmtVal = [](double v) -> QString {
                  if (std::isnan(v)) return QStringLiteral("?");
                  return QString::number(v, 'g', 6);
                };
                if (dn.propX || dn.propY) {
                  stext += QStringLiteral("    \u25CF pos: x=") + fmtVal(dn.xVal)
                         + QStringLiteral(" y=") + fmtVal(dn.yVal) + QStringLiteral("\n");
                }
                if (dn.propAlpha)
                  stext += QStringLiteral("    \u25CF _alpha=") + fmtVal(dn.alphaVal) + QStringLiteral("\n");
                if (dn.propScale)
                  stext += QStringLiteral("    \u25CF _scale=") + fmtVal(dn.scaleVal) + QStringLiteral("\n");
                if (dn.propRot)
                  stext += QStringLiteral("    \u25CF _rotation=") + fmtVal(dn.rotVal) + QStringLiteral("\n");
                if (dn.propVis)
                  stext += QStringLiteral("    \u25CF _visible assigned\n");
              }
            } else {
              // No detected_calls — fall back to near-call string listing.
              bool anyNearCall = false;
              for (const auto& as : hints.strings) {
                if (!as.near_call || as.role != gf::apt::AptStringRole::PushLiteral) continue;
                if (!anyNearCall) { stext += QStringLiteral("Near-call strings (no resolved calls):\n"); anyNearCall = true; }
                QString resolvedStr;
                for (const auto& ex : file.exports)
                  if (ex.name == as.value) {
                    const int t = probeTier(ex.character);
                    resolvedStr = QString(" \u2192 C%1 [%2]").arg(ex.character).arg(tierLabel(t));
                    break;
                  }
                for (const auto& im : file.imports)
                  if (im.name == as.value && resolvedStr.isEmpty()) {
                    resolvedStr = QString(" \u2192 C%1 [import]").arg(im.character);
                    break;
                  }
                stext += QStringLiteral("  \"") + QString::fromStdString(as.value)
                       + QStringLiteral("\"") + resolvedStr + QStringLiteral("\n");
              }
              if (!anyNearCall && hints.opcode_count > 0)
                stext += QStringLiteral("(no instantiate calls detected)\n");
              else if (hints.opcode_count == 0)
                stext += QStringLiteral("(no action bytecode found)\n");
            }

            // File imports table (diagnostic; always shown at bottom, capped).
            if (!file.imports.empty()) {
              stext += QStringLiteral("\nFile imports (")
                     + QString::number(file.imports.size()) + QStringLiteral(" total");
              const int showImp = std::min(static_cast<int>(file.imports.size()), 12);
              stext += QStringLiteral(", showing ") + QString::number(showImp) + QStringLiteral("):\n");
              int shown = 0;
              for (const auto& im : file.imports) {
                stext += QString("  C%1 \u2190 %2/%3\n")
                    .arg(im.character)
                    .arg(QString::fromStdString(im.movie))
                    .arg(QString::fromStdString(im.name));
                if (++shown >= 12) break;
              }
            }

            m_aptScaffoldDump->setPlainText(stext);
          }
        }

        m_aptPropStack->setCurrentWidget(m_aptCharPage);
      } else {
        m_aptDetails->setPlainText(item->data(0, Qt::UserRole).toString());
        m_aptPropStack->setCurrentWidget(m_aptDetails);
      }
      break;

    case kAptNodeFrame: {
      const gf::apt::AptFrame* frame = nullptr;
      if (ownerKind == 1) {
        if (ownerIndex >= 0 && ownerIndex < static_cast<int>(file.characters.size())) {
          const auto& ch = file.characters[static_cast<std::size_t>(ownerIndex)];
          if (index >= 0 && index < static_cast<int>(ch.frames.size())) {
            frame = &ch.frames[static_cast<std::size_t>(index)];
          }
        }
      } else if (index >= 0 && index < static_cast<int>(file.frames.size())) {
        frame = &file.frames[static_cast<std::size_t>(index)];
      }

      if (frame) {
        m_aptFrameItemCountLabel->setText(QString("%1 (%2 placements)").arg(frame->frameitemcount).arg(frame->placements.size()));
        m_aptFrameItemsOffsetLabel->setText(QString("0x%1").arg(QString::number(qulonglong(frame->items_offset), 16).toUpper()));

        // Populate the cumulative display-list dump.
        if (m_aptFrameDlDump) {
          const QString ctx = (ownerKind == 1)
              ? QStringLiteral("Character %1").arg(ownerIndex)
              : QStringLiteral("Root Movie");
          const std::vector<gf::apt::AptFrame>* frameList = nullptr;
          if (ownerKind == 1 && ownerIndex >= 0
              && ownerIndex < static_cast<int>(file.characters.size()))
            frameList = &file.characters[static_cast<std::size_t>(ownerIndex)].frames;
          else
            frameList = &file.frames;

          if (frameList && !frameList->empty())
            m_aptFrameDlDump->setPlainText(
                buildDlSummaryText(*frameList, static_cast<std::size_t>(index), ctx));
          else
            m_aptFrameDlDump->setPlainText("(no frames in this context)");
        }

        m_aptPropStack->setCurrentWidget(m_aptFramePage);
      } else {
        m_aptDetails->setPlainText(item->data(0, Qt::UserRole).toString());
        m_aptPropStack->setCurrentWidget(m_aptDetails);
      }
      break;
    }

    case kAptNodePlacement: {
      const gf::apt::AptPlacement* placement = nullptr;
      if (ownerKind == 1) {
        if (ownerIndex >= 0 && ownerIndex < static_cast<int>(file.characters.size())) {
          const auto& ch = file.characters[static_cast<std::size_t>(ownerIndex)];
          if (index >= 0 && index < static_cast<int>(ch.frames.size())) {
            const auto& fr = ch.frames[static_cast<std::size_t>(index)];
            if (placementIndex >= 0 && placementIndex < static_cast<int>(fr.placements.size())) {
              placement = &fr.placements[static_cast<std::size_t>(placementIndex)];
            }
          }
        }
      } else if (index >= 0 && index < static_cast<int>(file.frames.size())) {
        const auto& fr = file.frames[static_cast<std::size_t>(index)];
        if (placementIndex >= 0 && placementIndex < static_cast<int>(fr.placements.size())) {
          placement = &fr.placements[static_cast<std::size_t>(placementIndex)];
        }
      }

      if (placement) {
        m_aptPlacementDepthSpin->setValue(static_cast<int>(placement->depth));
        m_aptPlacementCharSpin->setValue(static_cast<int>(placement->character));
        m_aptPlacementNameEdit->setText(QString::fromStdString(placement->instance_name));
        m_aptPlacementXSpin->setValue(placement->transform.x);
        m_aptPlacementYSpin->setValue(placement->transform.y);
        m_aptPlacementScaleXSpin->setValue(placement->transform.scale_x);
        if (m_aptPlacementRotSkew0Spin) m_aptPlacementRotSkew0Spin->setValue(placement->transform.rotate_skew_0);
        if (m_aptPlacementRotSkew1Spin) m_aptPlacementRotSkew1Spin->setValue(placement->transform.rotate_skew_1);
        m_aptPlacementScaleYSpin->setValue(placement->transform.scale_y);
        m_aptPlacementOffsetLabel->setText(QString("0x%1").arg(QString::number(qulonglong(placement->offset), 16).toUpper()));
        m_aptPropStack->setCurrentWidget(m_aptPlacementPage);
      } else {
        m_aptDetails->setPlainText(item->data(0, Qt::UserRole).toString());
        m_aptPropStack->setCurrentWidget(m_aptDetails);
      }
      break;
    }

    default:
      m_aptDetails->setPlainText(item->data(0, Qt::UserRole).toString());
      m_aptPropStack->setCurrentWidget(m_aptDetails);
      break;
  }

  m_aptUpdatingUi = false;
}

void MainWindow::onAptApply() {
  if (!m_currentAptFile || !m_aptTree) return;

  auto* item = m_aptTree->currentItem();
  if (!item) return;

  const int nodeType = item->data(0, kAptRoleNodeType).toInt();
  const int index = item->data(0, kAptRoleNodeIndex).toInt();
  const int ownerKind = item->data(0, kAptRoleOwnerKind).toInt();
  const int ownerIndex = item->data(0, kAptRoleOwnerIndex).toInt();
  const int placementIndex = item->data(0, kAptRolePlacementIndex).toInt();

  auto& file = *m_currentAptFile;
  bool changed = false;

  switch (nodeType) {
    case kAptNodeSummary: {
      const auto newW = static_cast<std::uint32_t>(m_aptSumWidthSpin->value());
      const auto newH = static_cast<std::uint32_t>(m_aptSumHeightSpin->value());
      changed = (file.summary.screensizex != newW) || (file.summary.screensizey != newH);
      file.summary.screensizex = newW;
      file.summary.screensizey = newH;

      QTreeWidgetItem* summaryNode = item;
      while (summaryNode && summaryNode->data(0, kAptRoleNodeType).toInt() != kAptNodeSummary) {
        summaryNode = summaryNode->parent();
      }
      if (summaryNode) {
        summaryNode->setText(1, QString("%1x%2").arg(newW).arg(newH));
        summaryNode->setData(0, Qt::UserRole,
                      QString("Screen Size: %1 x %2\nFrames: %3\nCharacters: %4\nImports: %5\nExports: %6\nMovie Data Offset: 0x%7")
                          .arg(newW)
                          .arg(newH)
                          .arg(file.frames.size())
                          .arg(file.characters.size())
                          .arg(file.imports.size())
                          .arg(file.exports.size())
                          .arg(QString::number(qulonglong(file.summary.aptdataoffset), 16).toUpper()));
      }
      break;
    }

    case kAptNodeImport:
      if (index >= 0 && index < static_cast<int>(file.imports.size())) {
        auto& im = file.imports[static_cast<std::size_t>(index)];
        const QString movie = m_aptImportMovieEdit->text().trimmed();
        const QString name = m_aptImportNameEdit->text().trimmed();
        changed = (QString::fromStdString(im.movie) != movie) || (QString::fromStdString(im.name) != name);
        im.movie = movie.toStdString();
        im.name = name.toStdString();
        item->setText(0, QString("%1: %2 :: %3").arg(index).arg(movie.isEmpty() ? "(movie?)" : movie, name.isEmpty() ? "(name?)" : name));
        item->setData(0, Qt::UserRole,
                      QString("Type: Import\nIndex: %1\nMovie: %2\nName: %3\nCharacter: %4\nOffset: 0x%5")
                          .arg(index)
                          .arg(movie.isEmpty() ? "(empty)" : movie)
                          .arg(name.isEmpty() ? "(empty)" : name)
                          .arg(im.character)
                          .arg(QString::number(qulonglong(im.offset), 16).toUpper()));
      }
      break;

    case kAptNodeExport:
      if (index >= 0 && index < static_cast<int>(file.exports.size())) {
        auto& ex = file.exports[static_cast<std::size_t>(index)];
        const QString name = m_aptExportNameEdit->text().trimmed();
        changed = (QString::fromStdString(ex.name) != name);
        ex.name = name.toStdString();
        item->setText(0, QString("%1: %2").arg(index).arg(name.isEmpty() ? "(empty)" : name));
        item->setData(0, Qt::UserRole,
                      QString("Type: Export\nIndex: %1\nName: %2\nCharacter: %3\nOffset: 0x%4")
                          .arg(index)
                          .arg(name.isEmpty() ? "(empty)" : name)
                          .arg(ex.character)
                          .arg(QString::number(qulonglong(ex.offset), 16).toUpper()));
      }
      break;

    case kAptNodePlacement: {
      gf::apt::AptPlacement* placement = nullptr;
      QString ownerLabel = "Root Movie";

      if (ownerKind == 1) {
        if (ownerIndex >= 0 && ownerIndex < static_cast<int>(file.characters.size())) {
          auto& ch = file.characters[static_cast<std::size_t>(ownerIndex)];
          if (index >= 0 && index < static_cast<int>(ch.frames.size())) {
            auto& fr = ch.frames[static_cast<std::size_t>(index)];
            if (placementIndex >= 0 && placementIndex < static_cast<int>(fr.placements.size())) {
              placement = &fr.placements[static_cast<std::size_t>(placementIndex)];
              ownerLabel = QString("Character %1").arg(ownerIndex);
            }
          }
        }
      } else if (index >= 0 && index < static_cast<int>(file.frames.size())) {
        auto& fr = file.frames[static_cast<std::size_t>(index)];
        if (placementIndex >= 0 && placementIndex < static_cast<int>(fr.placements.size())) {
          placement = &fr.placements[static_cast<std::size_t>(placementIndex)];
        }
      }

      if (placement) {
        const std::uint32_t newDepth     = static_cast<std::uint32_t>(m_aptPlacementDepthSpin->value());
        const std::uint32_t newCharacter = static_cast<std::uint32_t>(m_aptPlacementCharSpin->value());
        const QString newName            = m_aptPlacementNameEdit->text().trimmed();
        const double newX       = m_aptPlacementXSpin->value();
        const double newY       = m_aptPlacementYSpin->value();
        const double newScaleX  = m_aptPlacementScaleXSpin->value();
        const double newScaleY  = m_aptPlacementScaleYSpin->value();
        const double newRSkew0  = m_aptPlacementRotSkew0Spin ? m_aptPlacementRotSkew0Spin->value() : placement->transform.rotate_skew_0;
        const double newRSkew1  = m_aptPlacementRotSkew1Spin ? m_aptPlacementRotSkew1Spin->value() : placement->transform.rotate_skew_1;

        changed = placement->depth     != newDepth     ||
                  placement->character != newCharacter  ||
                  QString::fromStdString(placement->instance_name) != newName ||
                  placement->transform.x             != newX      ||
                  placement->transform.y             != newY      ||
                  placement->transform.scale_x       != newScaleX ||
                  placement->transform.scale_y       != newScaleY ||
                  placement->transform.rotate_skew_0 != newRSkew0 ||
                  placement->transform.rotate_skew_1 != newRSkew1;

        placement->depth     = newDepth;
        placement->character = newCharacter;
        placement->instance_name = newName.toStdString();
        placement->transform.x             = newX;
        placement->transform.y             = newY;
        placement->transform.scale_x       = newScaleX;
        placement->transform.scale_y       = newScaleY;
        placement->transform.rotate_skew_0 = newRSkew0;
        placement->transform.rotate_skew_1 = newRSkew1;

        item->setText(0, aptPlacementTreeLabel(*placement));
        item->setText(1, aptPlacementValueText(*placement));
        item->setData(0, Qt::UserRole, aptPlacementDetailText(*placement, ownerLabel, index, placementIndex));
      }
      break;
    }

    default:
      break;
  }

  if (!changed) {
    if (m_aptApplyAction) m_aptApplyAction->setEnabled(false);
    return;
  }

  m_aptDirty = true;
  setDirty(true);
  if (m_aptApplyAction) m_aptApplyAction->setEnabled(false);
  syncAptPropEditorFromItem(item);
  refreshAptPreview();
  statusBar()->showMessage("APT changes applied to in-memory model.", 3000);
}

void MainWindow::onAptSave() {
  if (!m_currentAptFile) return;

  const auto aptBytes = m_currentAptFile->rebuild_binary();
  if (aptBytes.empty()) {
    statusBar()->showMessage("APT rebuild produced empty output — save aborted.", 5000);
    return;
  }
  const std::span<const std::byte> byteSpan(
      reinterpret_cast<const std::byte*>(aptBytes.data()), aptBytes.size());

  if (!m_aptIsEmbedded) {
    // Standalone APT: write directly to the original file with backup.
    if (m_textAptPath.isEmpty()) {
      statusBar()->showMessage("No APT file path — use Export APT... instead.", 4000);
      return;
    }
    gf::core::SafeWriteOptions opts;
    opts.make_backup = true;
    const auto res = gf::core::safe_write_bytes(
        std::filesystem::path(m_textAptPath.toStdString()), byteSpan, opts);
    if (!res.ok) {
      QMessageBox::critical(this, "Save APT Failed", QString::fromStdString(res.message));
      return;
    }
    const QString backupInfo = res.backup_path
        ? QString(" (backup: %1)").arg(QString::fromStdString(res.backup_path->string()))
        : QString();
    statusBar()->showMessage(
        QString("APT saved: %1%2").arg(QFileInfo(m_textAptPath).fileName()).arg(backupInfo), 5000);
    m_aptDirty = false;
    return;
  }

  // Embedded APT — write the patched entry back to the AST container.
  if (m_aptSaveContextIsNested) {
    QMessageBox::information(this, "Save to Nested Container",
        "This APT lives inside a nested .ast container.\n"
        "Direct save into nested containers is not yet supported.\n\n"
        "Use 'Export APT...' to save the patched bytes to a file,\n"
        "then replace the entry manually via the AST editor.");
    return;
  }

  if (m_aptSaveAptEntryIdx < 0 || m_aptSaveOuterPath.isEmpty()) {
    statusBar()->showMessage("No save context — reload the APT and try again.", 4000);
    return;
  }

  // Reuse m_liveAstEditor if it already covers this container; otherwise load.
  if (!m_liveAstEditor || m_liveAstPath != m_aptSaveOuterPath) {
    std::string loadErr;
    auto editor = gf::core::AstContainerEditor::load(
        m_aptSaveOuterPath.toStdString(), &loadErr);
    if (!editor) {
      QMessageBox::critical(this, "Save APT Failed",
          QString("Cannot open container:\n%1").arg(QString::fromStdString(loadErr)));
      return;
    }
    m_liveAstEditor = std::make_unique<gf::core::AstContainerEditor>(std::move(*editor));
    m_liveAstPath = m_aptSaveOuterPath;
  }

  const std::span<const std::uint8_t> aptSpan(aptBytes.data(), aptBytes.size());
  std::string replErr;
  if (!m_liveAstEditor->replaceEntryBytes(
          static_cast<std::uint32_t>(m_aptSaveAptEntryIdx), aptSpan,
          gf::core::AstContainerEditor::ReplaceMode::PreserveZlibIfPresent, &replErr)) {
    QMessageBox::critical(this, "Save APT Failed",
        QString("Failed to replace APT entry #%1:\n%2")
            .arg(m_aptSaveAptEntryIdx).arg(QString::fromStdString(replErr)));
    return;
  }

  std::string writeErr;
  if (!m_liveAstEditor->writeInPlace(&writeErr, /*makeBackup=*/true, /*maxBytes=*/0)) {
    QMessageBox::critical(this, "Save APT Failed",
        QString("Failed to write container:\n%1").arg(QString::fromStdString(writeErr)));
    return;
  }

  m_aptDirty = false;
  setDirty(false);
  statusBar()->showMessage(
      QString("APT entry #%1 saved to %2")
          .arg(m_aptSaveAptEntryIdx)
          .arg(QFileInfo(m_aptSaveOuterPath).fileName()),
      5000);
}

void MainWindow::onAptExport() {
  if (!m_currentAptFile) return;

  const auto aptBytes = m_currentAptFile->rebuild_binary();
  if (aptBytes.empty()) {
    statusBar()->showMessage("APT rebuild produced empty output.", 4000);
    return;
  }

  const QString defaultName = m_textAptPath.isEmpty()
      ? "export.apt"
      : QFileInfo(m_textAptPath).fileName();
  const QString outPath = QFileDialog::getSaveFileName(
      this, "Export APT Bytes", defaultName,
      "APT files (*.apt *.apt1);;All files (*)");
  if (outPath.isEmpty()) return;

  gf::core::SafeWriteOptions opts;
  opts.make_backup = false;
  const auto res = gf::core::safe_write_bytes(
      std::filesystem::path(outPath.toStdString()),
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(aptBytes.data()), aptBytes.size()),
      opts);
  if (!res.ok) {
    QMessageBox::critical(this, "Export APT Failed", QString::fromStdString(res.message));
    return;
  }
  statusBar()->showMessage(QString("APT exported to %1").arg(outPath), 4000);
}



static bool isMostlyPrintableBytes(const QByteArray& b) {
  if (b.isEmpty()) return false;
  int printable = 0;
  int nonPrintable = 0;
  for (int i = 0; i < b.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(b[i]);
    if (c == 0) return false;
    if (c == '\n' || c == '\r' || c == '\t') { printable++; continue; }
    if (c >= 32 && c <= 126) printable++; else nonPrintable++;
  }
  const int total = printable + nonPrintable;
  if (total == 0) return false;
  return (printable * 100) / total >= 85;
}

static bool looksZlibPreviewBytes(const QByteArray& b) {
  if (b.size() < 2) return false;
  const unsigned char b0 = static_cast<unsigned char>(b[0]);
  const unsigned char b1 = static_cast<unsigned char>(b[1]);
  return (b0 == 0x78) && (b1 == 0x01 || b1 == 0x9C || b1 == 0xDA);
}

static std::optional<QString> decodeTextCandidateBytes(const QByteArray& bytes) {
  if (bytes.isEmpty()) return std::nullopt;

  int off = 0;
  if (bytes.size() >= 3 &&
      static_cast<unsigned char>(bytes[0]) == 0xEF &&
      static_cast<unsigned char>(bytes[1]) == 0xBB &&
      static_cast<unsigned char>(bytes[2]) == 0xBF) {
    off = 3;
  }

  if (bytes.size() >= 2 &&
      static_cast<unsigned char>(bytes[0]) == 0xFF &&
      static_cast<unsigned char>(bytes[1]) == 0xFE) {
    const int n = (bytes.size() - 2) & ~1;
    const auto* u16 = reinterpret_cast<const ushort*>(bytes.constData() + 2);
    return QString::fromUtf16(u16, n / 2);
  }

  {
    const int check = std::min<int>(bytes.size(), 64);
    int zerosOdd = 0, zerosEven = 0;
    for (int i = 0; i < check; ++i) {
      if (bytes[i] == 0) {
        if (i & 1) zerosOdd++; else zerosEven++;
      }
    }
    if (zerosOdd >= 8 && zerosOdd > zerosEven) {
      const int n = bytes.size() & ~1;
      const auto* u16 = reinterpret_cast<const ushort*>(bytes.constData());
      return QString::fromUtf16(u16, n / 2);
    }
  }

  if (!isMostlyPrintableBytes(bytes.mid(off))) return std::nullopt;
  return QString::fromUtf8(bytes.constData() + off, bytes.size() - off);
}

static QString previewBufferSummary(const QByteArray& bytes, const QString& source) {
  return QStringLiteral("%1 bytes from %2")
      .arg(QString::number(bytes.size()), source.isEmpty() ? QStringLiteral("(none)") : source);
}

void MainWindow::invalidatePreviewContext() {
  ++m_previewSelectionVersion;
  m_previewContext = PreviewSelectionContext{};
  m_previewContext.selectionVersion = m_previewSelectionVersion;

  clearCurrentTextureState();
  m_currentTextureSelectionVersion = 0;

  if (m_textView) {
    m_textView->setPlainText(QStringLiteral("(No selection)"));
    if (m_textView->document()) m_textView->document()->setModified(false);
    m_textView->setReadOnly(true);
  }
  if (m_hexView) m_hexView->clear();
  if (m_imageView) {
    m_imageView->setPixmap(QPixmap());
    m_imageView->setText(QString());
  }
  if (m_textureInfo) {
    m_textureInfo->clear();
    m_textureInfo->setVisible(false);
  }
}

static bool looksLikeTexturePreviewBytes(const QByteArray& bytes);

// Returns true when `item` is a leaf entry that lives inside a nested embedded
// sub-AST, meaning it has at least one ancestor QTreeWidgetItem whose type column
// reads "AST" and which carries a valid entry-index role.
//
// In this situation item->data(0, Qt::UserRole+6) holds an index that is relative
// to the *inner* sub-AST directory – it cannot be used with the outer container's
// AstContainerEditor.  The safe read path is always the direct file-range read
// using UserRole+1 (which stores the absolute byte offset of the payload).
static bool itemIsNestedSubEntry(const QTreeWidgetItem* item) {
  if (!item) return false;
  for (const QTreeWidgetItem* p = item->parent(); p; p = p->parent()) {
    if (p->text(1).compare(QStringLiteral("AST"), Qt::CaseInsensitive) == 0 &&
        p->data(0, Qt::UserRole + 6).isValid()) {
      return true;
    }
  }
  return false;
}

MainWindow::PreviewSelectionContext MainWindow::buildPreviewContextForItem(QTreeWidgetItem* item) const {
  PreviewSelectionContext ctx;
  ctx.selectionVersion = m_previewSelectionVersion;
  if (!item) return ctx;

  const QString path = item->data(0, Qt::UserRole).toString();
  const bool isEmbedded = item->data(0, Qt::UserRole + 3).toBool();
  const quint64 baseOffset = item->data(0, Qt::UserRole + 1).toULongLong();
  const quint64 maxReadable = item->data(0, Qt::UserRole + 2).toULongLong();
  const quint32 entryIndex = static_cast<quint32>(item->data(0, Qt::UserRole + 6).toULongLong());
  const QString type = item->text(1).trimmed();

  ctx.entryPath = path;
  ctx.entryDisplayName = item->text(0);
  ctx.entryType = type;
  ctx.entryIndex = entryIndex;
  ctx.isEmbedded = isEmbedded;
  // Determine whether this item is a sub-entry inside a nested embedded AST.
  // When true, entryIndex is scoped to the inner sub-AST and must NOT be used
  // with the outer m_liveAstEditor.  See itemIsNestedSubEntry() for rationale.
  ctx.isNestedSubEntry = itemIsNestedSubEntry(item);

  constexpr quint64 kPreviewMax = 4096;
  constexpr quint64 kMaxStoredRead = 2ull * 1024ull * 1024ull;
  const quint64 wantStored = std::min<quint64>(maxReadable ? maxReadable : kPreviewMax, kMaxStoredRead);

  auto readDirectRange = [&](quint64 len) -> QByteArray {
    if (path.isEmpty() || len == 0) return {};
    const std::uint64_t off = isEmbedded ? static_cast<std::uint64_t>(baseOffset) : 0ull;
    std::vector<std::uint8_t> raw = read_file_range(path, off, static_cast<std::uint64_t>(len));
    if (raw.empty()) return {};
    return QByteArray(reinterpret_cast<const char*>(raw.data()), static_cast<int>(std::min<std::size_t>(raw.size(), static_cast<std::size_t>(std::numeric_limits<int>::max()))));
  };

  if (isEmbedded) {
    const QVariant pendingVar = item->data(0, Qt::UserRole + 30);
    if (pendingVar.isValid()) {
      const QByteArray pending = pendingVar.toByteArray();
      if (!pending.isEmpty()) {
        ctx.rawBytes = pending;
        ctx.rawSource = QStringLiteral("tree.pendingBytes");
      }
    }
  }

  if (ctx.rawBytes.isEmpty() && isEmbedded && !ctx.isNestedSubEntry &&
      m_liveAstEditor && m_liveAstPath == path) {
    // Only safe when this item is a *direct* entry of the outer on-disk container
    // that m_liveAstEditor was loaded from.  For entries inside a nested embedded
    // sub-AST, entryIndex is relative to the inner BGFA and getEntryStoredBytes()
    // would silently return the wrong outer entry (often a tiny XML blob).
    if (auto storedOpt = m_liveAstEditor->getEntryStoredBytes(entryIndex); storedOpt.has_value() && !storedOpt->empty()) {
      ctx.rawBytes = QByteArray(reinterpret_cast<const char*>(storedOpt->data()), static_cast<int>(std::min<std::size_t>(storedOpt->size(), static_cast<std::size_t>(std::numeric_limits<int>::max()))));
      ctx.rawSource = QStringLiteral("editor.getEntryStoredBytes");
    }
  }

  if (ctx.rawBytes.isEmpty()) {
    ctx.rawBytes = readDirectRange(wantStored);
    ctx.rawSource = isEmbedded ? QStringLiteral("container.range") : QStringLiteral("file.range");
  }

  if (isEmbedded) {
    const QVariant previewVar = item->data(0, Qt::UserRole + 31);
    if (previewVar.isValid()) {
      const QByteArray preview = previewVar.toByteArray();
      if (!preview.isEmpty() && looksLikeTexturePreviewBytes(preview)) {
        ctx.textureBytes = preview;
        ctx.textureSource = QStringLiteral("tree.previewBytes");
      }
    }
  }

  if (ctx.textureBytes.isEmpty()) {
    ctx.textureBytes = ctx.rawBytes;
    ctx.textureSource = ctx.rawSource;
  }

  QByteArray inflated;
  QString inflatedSource;
  const bool texturePrefersStored = textureTypePrefersStoredBytes(type.toUpper());
  const bool entryLooksCompressed = (type.compare("ZLIB", Qt::CaseInsensitive) == 0) || looksZlibPreviewBytes(ctx.rawBytes);
  if (entryLooksCompressed && isEmbedded && !ctx.isNestedSubEntry &&
      m_liveAstEditor && m_liveAstPath == path) {
    // Same guard as above: only valid for direct outer entries.
    std::string inflateErr;
    if (auto fullOpt = m_liveAstEditor->getEntryInflatedBytes(entryIndex, &inflateErr); fullOpt.has_value() && !fullOpt->empty()) {
      inflated = QByteArray(reinterpret_cast<const char*>(fullOpt->data()), static_cast<int>(std::min<std::size_t>(fullOpt->size(), static_cast<std::size_t>(std::numeric_limits<int>::max()))));
      inflatedSource = QStringLiteral("editor.getEntryInflatedBytes");
    }
  }
  if (entryLooksCompressed && inflated.isEmpty() && looksZlibPreviewBytes(ctx.rawBytes)) {
    std::vector<std::uint8_t> zIn(static_cast<std::size_t>(ctx.rawBytes.size()));
    if (!ctx.rawBytes.isEmpty()) std::memcpy(zIn.data(), ctx.rawBytes.constData(), static_cast<std::size_t>(ctx.rawBytes.size()));
    const auto inflatedPreview = gf::core::AstArchive::inflateZlibPreview(zIn, 8u * 1024u * 1024u);
    if (!inflatedPreview.empty()) {
      inflated = QByteArray(reinterpret_cast<const char*>(inflatedPreview.data()), static_cast<int>(inflatedPreview.size()));
      inflatedSource = QStringLiteral("zlib.inflatePreview");
    }
  }
  ctx.inflatedBytes = inflated;
  ctx.inflatedSource = inflatedSource;
  Q_ASSERT(!texturePrefersStored || entryLooksCompressed || ctx.inflatedBytes.isEmpty());

  ctx.hexBytes = !ctx.inflatedBytes.isEmpty()
      ? ctx.inflatedBytes.left(static_cast<int>(std::min<quint64>(kPreviewMax, static_cast<quint64>(ctx.inflatedBytes.size()))))
      : ctx.rawBytes.left(static_cast<int>(std::min<quint64>(kPreviewMax, static_cast<quint64>(ctx.rawBytes.size()))));
  ctx.hexSource = !ctx.inflatedBytes.isEmpty() ? ctx.inflatedSource : ctx.rawSource;

  const QByteArray textCandidatePrimary = (!ctx.inflatedBytes.isEmpty() && !texturePrefersStored) ? ctx.inflatedBytes : ctx.rawBytes;
  if (decodeTextCandidateBytes(textCandidatePrimary).has_value()) {
    ctx.textBytes = textCandidatePrimary;
    ctx.textSource = !ctx.inflatedBytes.isEmpty() ? ctx.inflatedSource : ctx.rawSource;
    ctx.textDetectedType = QStringLiteral("plain-text");
  } else {
    ctx.textDetectedType = QStringLiteral("binary/non-text");
  }

  if (ctx.textureBytes.isEmpty() && !ctx.inflatedBytes.isEmpty() && !texturePrefersStored) {
    ctx.textureBytes = ctx.inflatedBytes;
    ctx.textureSource = ctx.inflatedSource;
  }
  if (!ctx.textureBytes.isEmpty()) {
    const QByteArray probe = ctx.textureBytes.left(4);
    if (probe == "DDS ") ctx.textureDetectedType = QStringLiteral("DDS");
    else if (probe == "XPR2") ctx.textureDetectedType = QStringLiteral("XPR2");
    else if (probe.size() >= 3 && (probe.startsWith("p3R") || probe.startsWith("P3R"))) ctx.textureDetectedType = QStringLiteral("P3R");
    else ctx.textureDetectedType = QStringLiteral("unknown");
  }

  return ctx;
}


static bool looksLikeTexturePreviewBytes(const QByteArray& bytes) {
  if (bytes.isEmpty()) return false;
  if (bytes.size() >= 4 && std::memcmp(bytes.constData(), "DDS ", 4) == 0) return true;
  if (bytes.size() >= 4 && std::memcmp(bytes.constData(), "XPR2", 4) == 0) return true;
  if (bytes.size() >= 3 && (std::memcmp(bytes.constData(), "p3R", 3) == 0 || std::memcmp(bytes.constData(), "P3R", 3) == 0)) return true;
  const int max = std::min<int>(bytes.size(), 0x4000);
  for (int i = 0; i + 3 < max; ++i) {
    if (bytes[i] == 'D' && bytes[i + 1] == 'D' && bytes[i + 2] == 'S' && bytes[i + 3] == ' ') return true;
  }
  return false;
}

QString MainWindow::buildPreviewDiagnosticsText(const PreviewSelectionContext& ctx) const {
  QStringList lines;
  lines << QStringLiteral("Selection v%1 | entry=%2 | type=%3 | embedded=%4 | nested-sub=%5 | index=%6")
              .arg(ctx.selectionVersion)
              .arg(ctx.entryDisplayName.isEmpty() ? QStringLiteral("(none)") : ctx.entryDisplayName)
              .arg(ctx.entryType.isEmpty() ? QStringLiteral("(unknown)") : ctx.entryType)
              .arg(ctx.isEmbedded ? QStringLiteral("yes") : QStringLiteral("no"))
              .arg(ctx.isNestedSubEntry ? QStringLiteral("yes") : QStringLiteral("no"))
              .arg(ctx.entryIndex);
  if (!ctx.entryPath.isEmpty()) lines << ctx.entryPath;
  if (ctx.isNestedSubEntry) {
    lines << QStringLiteral("NOTE: nested-sub-entry — live editor NOT used; bytes from direct file range read (UserRole+1 = absolute payload offset)");
  }
  lines << QStringLiteral("raw: %1").arg(previewBufferSummary(ctx.rawBytes, ctx.rawSource));
  lines << QStringLiteral("inflated: %1").arg(previewBufferSummary(ctx.inflatedBytes, ctx.inflatedSource));
  lines << QStringLiteral("hex-tab: %1").arg(previewBufferSummary(ctx.hexBytes, ctx.hexSource));
  lines << QStringLiteral("text-tab: %1 [%2]").arg(previewBufferSummary(ctx.textBytes, ctx.textSource), ctx.textDetectedType.isEmpty() ? QStringLiteral("n/a") : ctx.textDetectedType);
  lines << QStringLiteral("texture-tab: %1 [%2]").arg(previewBufferSummary(ctx.textureBytes, ctx.textureSource), ctx.textureDetectedType.isEmpty() ? QStringLiteral("n/a") : ctx.textureDetectedType);
  return lines.join('\n');
}

void MainWindow::showViewerForItem(QTreeWidgetItem* item) {
  if (!m_viewerLabel || !item) return;

  // Default: leave APT XML mode unless we explicitly enter it for an APT selection.
  m_textAptXmlMode = false;

  // Switching selection within an archive should exit external text mode.
  // External text mode is only for standalone text files opened via the Text tab.
  if (item->data(0, Qt::UserRole + 3).toBool()) {
    m_textExternalMode = false;
  }

  const QString path = item->data(0, Qt::UserRole).toString();
  if (path.isEmpty()) {
    m_viewerLabel->setPlainText(QString("%1").arg(item->text(0)));
    if (m_viewTabs) m_viewTabs->setVisible(false);
    return;
  }

  const bool isEmbedded = item->data(0, Qt::UserRole + 3).toBool();
  const quint64 baseOffset = item->data(0, Qt::UserRole + 1).toULongLong();
  const quint64 maxReadable = item->data(0, Qt::UserRole + 2).toULongLong();
  const quint32 astFlags = static_cast<quint32>(item->data(0, Qt::UserRole + 4).toULongLong());

  QFileInfo fi(path);
  const QString shownName = fi.fileName().isEmpty() ? path : fi.fileName();

  // If this is an APT, generate XML into the Text tab for in-tool editing.
  // Standalone: use files directly.
  // APT: show converter-style XML in the Text tab.
  // NOTE: for embedded entries, `path` is the *container* (.ast) path, so we cannot rely on QFileInfo(path).suffix().
  const QString itemType = item ? item->text(1) : QString();
  const bool isAptItem = (itemType.compare("APT", Qt::CaseInsensitive) == 0) ||
                         (itemType.compare("APT1", Qt::CaseInsensitive) == 0) ||
                         (fi.suffix().compare("apt", Qt::CaseInsensitive) == 0) ||
                         (fi.suffix().compare("apt1", Qt::CaseInsensitive) == 0);

  // Use the unified loadAptForItem helper for all APT loading (embedded + standalone).
  if (isAptItem) {
    if (m_viewTabs) m_viewTabs->setVisible(true);

    QString aptErr;
    const bool aptOk = loadAptForItem(item, &aptErr);

    if (!aptOk) {
      // Surface the failure in the status bar AND the viewer label so it's never silent.
      statusBar()->showMessage(QString("APT load failed: %1").arg(aptErr), 7000);
      m_viewerLabel->setPlainText(
          QString("APT: %1\n\nLoad failed:\n%2").arg(shownName, aptErr));
      // Switch to APT tab so the user sees any parse error placed in m_aptDetails.
      if (m_viewTabs && m_aptTab) {
        const int aptTabIdx = m_viewTabs->indexOf(m_aptTab);
        if (aptTabIdx >= 0) m_viewTabs->setCurrentIndex(aptTabIdx);
      }
      return;
    }

    // loadAptForItem set m_textAptPath / m_textConstPath and switched to the APT tab.
    m_textAptXmlMode = true;

    // Generate XML for the Text tab.
    std::string xmlErr;
    const auto xmlOpt = gf::apt::apt_to_xml_string(
        m_textAptPath.toStdString(), m_textConstPath.toStdString(), &xmlErr);

    QString xmlText;
    if (xmlOpt) {
      xmlText = QString::fromStdString(*xmlOpt);
    } else {
      std::string sumErr;
      const auto fileFallbackOpt = gf::apt::read_apt_file(
          m_textAptPath.toStdString(), m_textConstPath.toStdString(), &sumErr);
      if (fileFallbackOpt) {
        xmlText = aptSummaryToXmlFallback(*fileFallbackOpt,
                                          QFileInfo(m_textAptPath).fileName(),
                                          QFileInfo(m_textConstPath).fileName());
      } else {
        xmlText = QString("<error>%1</error>")
                      .arg(QString::fromStdString(xmlErr.empty() ? sumErr : xmlErr));
      }
    }

    if (m_textView) {
      m_textView->setPlainText(xmlText);
      m_textView->document()->setModified(false);
      m_textAptXmlCached = m_textView->toPlainText();
      m_textView->setReadOnly(true);
    }
    if (m_textEditAction) {
      m_textEditAction->setEnabled(true);
      m_textEditAction->setChecked(false);
      m_textEditAction->setToolTip("Edit the generated APT XML (in-memory)");
    }
    if (m_textApplyAction) m_textApplyAction->setEnabled(false);

    m_viewerLabel->setPlainText(
        QString("APT: %1\nCONST: %2\n\nText tab contains generated XML (Edit to modify; Apply stores in-memory only).")
            .arg(QFileInfo(m_textAptPath).fileName(),
                 QFileInfo(m_textConstPath).fileName()));
    return;
  }


  m_previewContext = buildPreviewContextForItem(item);
  const QByteArray stored = m_previewContext.rawBytes;
  const QByteArray view = m_previewContext.hexBytes;
  const QString type = item->text(1);

  const QString where = isEmbedded ? QString("embedded @ 0x%1").arg(baseOffset, 0, 16) : QString("file");
  m_viewerLabel->setPlainText(QString("Selected: %1 (%2)\n%3\n%4\n\n%5")
                         .arg(item->text(0), type, fi.absoluteFilePath(), where, buildPreviewDiagnosticsText(m_previewContext)));

  if (m_viewTabs) m_viewTabs->setVisible(true);

  // --- Hex tab (always available) ---
  if (m_hexView) {
    m_hexView->setPlainText(formatHexPreview(view, 0));
  }

  // --- Text tab (explicit current-entry source only) ---
  bool hasText = false;
  if (m_textView) {
    auto decoded = decodeTextCandidateBytes(m_previewContext.textBytes);
    if (decoded) {
      m_textView->setPlainText(*decoded);
      if (m_textView->document()) m_textView->document()->setModified(false);
      hasText = true;
    } else {
      m_textView->setPlainText(QStringLiteral("Selected entry is binary / non-text.\n\nSource: %1")
                                   .arg(m_previewContext.textSource.isEmpty() ? QStringLiteral("(no text source)") : m_previewContext.textSource));
      if (m_textView->document()) m_textView->document()->setModified(false);
    }

    {
      const bool canEditEmbedded = isEmbedded && hasText && m_textForceEdit;
      const bool canEdit = editingEnabled() && (m_textExternalMode || canEditEmbedded);

      if (m_textEditAction) {
        const bool canToggle = editingEnabled() && isEmbedded && hasText;
        m_textEditAction->setEnabled(canToggle);
        if (!canToggle) {
          m_textEditAction->setChecked(false);
          m_textForceEdit = false;
        }
      }

      m_textView->setReadOnly(!canEdit);

      if (m_textApplyAction) {
        const bool modified = m_textView->document() ? m_textView->document()->isModified() : false;
        m_textApplyAction->setEnabled(canEdit && modified);
      }
      if (m_statusDirtyLabel && m_textView->document()) {
        m_statusDirtyLabel->setText(m_textView->document()->isModified() ? "Dirty" : "");
      }
    }
  }

  // --- RSF tab ---
  bool hasRsf = false;
  if (m_rsfTab && m_rsfMaterialsTable && m_rsfParamsTable) {
    auto clearRsfUi = [this]() {
      if (m_rsfNameValue) m_rsfNameValue->setText({});
      if (m_rsfModelCountValue) m_rsfModelCountValue->setText("0");
      if (m_rsfMaterialCountValue) m_rsfMaterialCountValue->setText("0");
      if (m_rsfTextureCountValue) m_rsfTextureCountValue->setText("0");
      m_rsfMaterialsTable->setRowCount(0);
      m_rsfParamsTable->setRowCount(0);
      m_rsfCurrentDoc.reset();
      m_rsfPreviewDoc.reset();
      m_rsfOriginalBytes.clear();
      if (m_rsfPreviewWidget) m_rsfPreviewWidget->clear();
      m_rsfSourcePath.clear();
      m_rsfSourceEmbedded = false;
      m_rsfSourceEntryIndex = 0;
      if (m_rsfEditAction) { m_rsfEditAction->setEnabled(false); m_rsfEditAction->setChecked(true); }
      setRsfDirty(false);
    };
    clearRsfUi();

    QByteArray rsfBytes = stored;
    const bool storedLooksBinaryRsf = isBinaryRsfBytes(stored);
    const bool storedLooksXmlRsf = isXmlLikeBytes(stored);
    if (isEmbedded) {
      const quint64 absOffset = item->data(0, Qt::UserRole + 1).toULongLong();
      const quint64 compSize = item->data(0, Qt::UserRole + 2).toULongLong();
      QByteArray rawEntryBytes;
      if (!path.isEmpty() && compSize > 0) {
        auto raw = read_file_range(path, static_cast<std::uint64_t>(absOffset), static_cast<std::uint64_t>(compSize));
        if (!raw.empty()) {
          rawEntryBytes = QByteArray(reinterpret_cast<const char*>(raw.data()), static_cast<int>(raw.size()));
        }
      }
      QByteArray inflatedBytes;
      if (!rawEntryBytes.isEmpty() && looksZlibPreviewBytes(rawEntryBytes)) {
        std::vector<std::uint8_t> zIn(static_cast<std::size_t>(rawEntryBytes.size()));
        std::memcpy(zIn.data(), rawEntryBytes.constData(), static_cast<std::size_t>(rawEntryBytes.size()));
        const auto inflated = gf::core::AstArchive::inflateZlibPreview(zIn, 8u * 1024u * 1024u);
        if (!inflated.empty()) {
          inflatedBytes = QByteArray(reinterpret_cast<const char*>(inflated.data()), static_cast<int>(inflated.size()));
        }
      }

      if (storedLooksBinaryRsf) rsfBytes = stored;
      else if (isBinaryRsfBytes(rawEntryBytes)) rsfBytes = rawEntryBytes;
      else if (isBinaryRsfBytes(inflatedBytes)) rsfBytes = inflatedBytes;
      else if (storedLooksXmlRsf) rsfBytes = stored;
      else if (isXmlLikeBytes(rawEntryBytes)) rsfBytes = rawEntryBytes;
      else if (isXmlLikeBytes(inflatedBytes)) rsfBytes = inflatedBytes;
      else if (!rawEntryBytes.isEmpty()) rsfBytes = rawEntryBytes;
      else if (!inflatedBytes.isEmpty()) rsfBytes = inflatedBytes;
    }

    auto setRsfMessage = [this](const QString& msg) {
      m_rsfMaterialsTable->setRowCount(1);
      m_rsfMaterialsTable->setItem(0, 0, new QTableWidgetItem(msg));
      for (int c = 1; c < m_rsfMaterialsTable->columnCount(); ++c) {
        m_rsfMaterialsTable->setItem(0, c, new QTableWidgetItem(QString()));
      }
    };

    auto tryInflateForRsf = [&](const QByteArray& src, std::size_t outCap = 8u * 1024u * 1024u) -> QByteArray {
      if (src.isEmpty()) return {};
      std::vector<std::uint8_t> zIn(static_cast<std::size_t>(src.size()));
      std::memcpy(zIn.data(), src.constData(), static_cast<std::size_t>(src.size()));
      const auto inflated = gf::core::AstArchive::inflateZlibPreview(zIn, outCap);
      if (inflated.empty()) return {};
      return QByteArray(reinterpret_cast<const char*>(inflated.data()), static_cast<int>(inflated.size()));
    };

    if (!isBinaryRsfBytes(rsfBytes) && !isXmlLikeBytes(rsfBytes)) {
      if (looksZlibPreviewBytes(rsfBytes)) {
        const QByteArray inflated = tryInflateForRsf(rsfBytes);
        if (!inflated.isEmpty()) rsfBytes = inflated;
      } else if (looksZlibPreviewBytes(stored)) {
        const QByteArray inflated = tryInflateForRsf(stored);
        if (!inflated.isEmpty()) rsfBytes = inflated;
      }
    }

    const bool rsfByExt = fi.suffix().compare("rsf", Qt::CaseInsensitive) == 0 || type.compare("RSF", Qt::CaseInsensitive) == 0;
    const bool rsfByHeader = isBinaryRsfBytes(rsfBytes);
    const bool rsfByXml = isXmlLikeBytes(rsfBytes);

    if (rsfByExt || rsfByHeader || rsfByXml) {
      m_viewTabs->setCurrentWidget(m_rsfTab);

      if (rsfByHeader) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(rsfBytes.size()));
        if (!rsfBytes.isEmpty()) {
          std::memcpy(bytes.data(), rsfBytes.constData(), static_cast<std::size_t>(rsfBytes.size()));
        }

        if (auto docOpt = gf::models::rsf::parse(std::span<const std::uint8_t>(bytes.data(), bytes.size())); docOpt.has_value()) {
          m_rsfCurrentDoc = docOpt.value();
          m_rsfOriginalBytes = rsfBytes;
          m_rsfSourcePath = path;
          m_rsfSourceEmbedded = isEmbedded;
          m_rsfSourceEntryIndex = static_cast<std::uint32_t>(item->data(0, Qt::UserRole + 6).toULongLong());
          if (m_rsfNameValue) {
            QString nm = rsfInfoNameFromBytes(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
            if (nm.isEmpty()) nm = fi.fileName();
            m_rsfNameValue->setText(nm);
          }
          if (m_rsfModelCountValue) m_rsfModelCountValue->setText(QString::number(rsfGeomCountFromBytes(std::span<const std::uint8_t>(bytes.data(), bytes.size()))));
          if (m_rsfMaterialCountValue) m_rsfMaterialCountValue->setText(QString::number(int(m_rsfCurrentDoc->materials.size())));
          if (m_rsfTextureCountValue) m_rsfTextureCountValue->setText(QString::number(int(m_rsfCurrentDoc->textures.size())));
          if (m_rsfEditAction) { m_rsfEditAction->setEnabled(false); m_rsfEditAction->setChecked(true); }
          refreshRsfMaterialsTable();
          if (!m_rsfCurrentDoc->materials.empty()) refreshRsfParamsTable(0); else m_rsfParamsTable->setRowCount(0);
          setRsfDirty(false);
          hasRsf = true;
        } else {
          if (m_rsfNameValue) m_rsfNameValue->setText(fi.fileName());
          setRsfMessage("(RSF parse failed)");
          if (m_rsfPreviewWidget) m_rsfPreviewWidget->clear();
          hasRsf = true;
        }
      } else if (rsfByXml) {
        if (m_rsfNameValue) m_rsfNameValue->setText(fi.fileName());
        setRsfMessage("(XML RSF / state file)");
        if (m_rsfPreviewWidget) m_rsfPreviewWidget->clear();
        hasRsf = true;
      }
    } else {
      setRsfMessage("(Not an RSF)");
      if (m_rsfPreviewWidget) m_rsfPreviewWidget->clear();
    }
  }

  // --- DAT tab ---
  bool hasDat = false;
  if (m_datTab && m_datImagesTable && m_datSummaryLabel) {
    m_datSummaryLabel->setText({});
    m_datImagesTable->setRowCount(0);
    m_currentDatFile.reset();

    // Resolve raw bytes for the entry (same inflate logic as RSF).
    QByteArray datBytes = stored;
    if (isEmbedded) {
      const quint64 absOffset = item->data(0, Qt::UserRole + 1).toULongLong();
      const quint64 compSize  = item->data(0, Qt::UserRole + 2).toULongLong();
      if (!path.isEmpty() && compSize > 0) {
        auto raw = read_file_range(path,
                                   static_cast<std::uint64_t>(absOffset),
                                   static_cast<std::uint64_t>(compSize));
        if (!raw.empty()) {
          QByteArray rawBytes(reinterpret_cast<const char*>(raw.data()),
                              static_cast<int>(raw.size()));
          if (looksZlibPreviewBytes(rawBytes)) {
            std::vector<std::uint8_t> zIn(raw.begin(), raw.end());
            auto inflated = gf::core::AstArchive::inflateZlibPreview(zIn, 8u * 1024u * 1024u);
            if (!inflated.empty())
              datBytes = QByteArray(reinterpret_cast<const char*>(inflated.data()),
                                    static_cast<int>(inflated.size()));
            else
              datBytes = rawBytes;
          } else {
            datBytes = rawBytes;
          }
        }
      }
    }

    const bool datByExt  = fi.suffix().compare("dat", Qt::CaseInsensitive) == 0
                        || type.compare("DAT", Qt::CaseInsensitive) == 0;
    const bool datByHeur = datBytes.size() >= 16 &&
                           gf::dat::looks_like_dat(
                               reinterpret_cast<const std::uint8_t*>(datBytes.constData()),
                               static_cast<std::size_t>(datBytes.size()));

    if (datByExt || datByHeur) {
      m_viewTabs->setCurrentWidget(m_datTab);

      std::string parseErr;
      auto datOpt = gf::dat::parse_dat(
          reinterpret_cast<const std::uint8_t*>(datBytes.constData()),
          static_cast<std::size_t>(datBytes.size()),
          &parseErr);

      if (m_datEntryInfoLabel) m_datEntryInfoLabel->setText("(select a row to preview)");
      if (m_datCorrelLabel)    m_datCorrelLabel->setText({});
      if (m_datPreviewScene)   m_datPreviewScene->clear();

      if (datOpt.has_value()) {
        m_currentDatFile = std::move(datOpt);
        const auto& dat  = *m_currentDatFile;
        m_datSummaryLabel->setText(
            QStringLiteral("Images: %1   FileLen: %2 B   FirstOffset: 0x%3")
                .arg(dat.summary.num_images)
                .arg(dat.summary.file_len)
                .arg(dat.summary.first_image_offset, 0, 16));

        m_datImagesTable->setRowCount(static_cast<int>(dat.images.size()));
        for (int row = 0; row < static_cast<int>(dat.images.size()); ++row) {
          const auto& img = dat.images[row];
          auto setCell = [&](int col, const QString& text) {
            auto* it = new QTableWidgetItem(text);
            it->setFlags(it->flags() & ~Qt::ItemIsEditable);
            m_datImagesTable->setItem(row, col, it);
          };
          setCell(0, QString::number(row));
          setCell(1, QString::number(img.charId));
          setCell(2, QStringLiteral("rgba(%1,%2,%3,%4)")
                         .arg(img.color_r).arg(img.color_g).arg(img.color_b).arg(img.color_a));
          setCell(3, QString::number(static_cast<double>(img.offset_x), 'f', 1));
          setCell(4, QString::number(static_cast<double>(img.offset_y), 'f', 1));
          setCell(5, QString::number(img.num_shapes));
          setCell(6, QStringLiteral("0x%1").arg(img.file_offset, 0, 16));
        }
        m_datImagesTable->resizeColumnsToContents();

        // APT/DAT correlation: compare DAT charIds against loaded APT character table
        if (m_datCorrelLabel) {
          QStringList datCharIds;
          for (const auto& img : dat.images)
            datCharIds << QString::number(img.charId);
          QString correlText = QStringLiteral("DAT charIds: %1").arg(datCharIds.join(", "));

          if (m_currentAptFile.has_value()) {
            const auto& chars = m_currentAptFile->characters;
            QStringList matched, unmatched;
            for (const auto& img : dat.images) {
              const bool inApt = img.charId < static_cast<std::uint32_t>(chars.size());
              (inApt ? matched : unmatched) << QString::number(img.charId);
            }
            if (!matched.isEmpty())
              correlText += QStringLiteral("\nMatched in APT character table: %1").arg(matched.join(", "));
            if (!unmatched.isEmpty())
              correlText += QStringLiteral("\nNot in APT character table: %1").arg(unmatched.join(", "));
          } else {
            correlText += "\n(No APT loaded — load an APT file to check charId correlation)";
          }
          m_datCorrelLabel->setText(correlText);
        }

        // Auto-render first entry
        m_datImagesTable->selectRow(0);
        renderDatImageToScene(0);
        hasDat = true;
      } else {
        m_datSummaryLabel->setText(QStringLiteral("Parse failed: %1")
                                       .arg(QString::fromStdString(parseErr)));
        if (m_datPreviewScene) {
          auto* t = m_datPreviewScene->addText(QStringLiteral("Parse failed:\n%1")
                                                   .arg(QString::fromStdString(parseErr)));
          t->setDefaultTextColor(QColor(255, 100, 100));
        }
        hasDat = true; // show the tab regardless so the error is visible
      }
    }
  }

  // --- Texture tab (DDS / EA-wrapped DDS (P3R) preview) ---
  bool hasTexture = false;
  if (m_imageView) {
      m_imageView->setPixmap(QPixmap());

    const QString type = item->text(1).trimmed();

    // For textures we often need more than the 2MB "stored" cap above.
    // Read the full stored payload (capped) and inflate if needed.
    auto load_full_payload = [&]() -> std::vector<std::uint8_t> {
      constexpr std::uint64_t kMaxStored = 64ull * 1024ull * 1024ull;
      constexpr std::uint64_t kMaxInflated = 128ull * 1024ull * 1024ull;

      auto toVector = [](const QByteArray& in) -> std::vector<std::uint8_t> {
        std::vector<std::uint8_t> out(static_cast<std::size_t>(in.size()));
        if (!in.isEmpty()) std::memcpy(out.data(), in.constData(), static_cast<std::size_t>(in.size()));
        return out;
      };

      if (!m_previewContext.textureBytes.isEmpty() &&
          m_previewContext.textureSource == QStringLiteral("converted.current-selection")) {
        return toVector(m_previewContext.textureBytes);
      }

      // Prefer the live in-memory AST editor for embedded entries so preview reflects
      // unsaved Replace Texture / Replace File changes immediately, but keep raw/inflated
      // source-of-truth handling explicit for texture entries.
      //
      // IMPORTANT: Only use m_liveAstEditor when this item is a DIRECT entry of the outer
      // on-disk AST.  For items that are leaves of a nested embedded sub-AST, entryIndex
      // is scoped to the inner sub-AST directory; querying the outer editor with it returns
      // the wrong entry (often a tiny XML stub, as seen with P3R jersey textures).
      // In that case the direct file-range read below (using UserRole+1 = absolute offset)
      // is the correct path.
      if (isEmbedded && !itemIsNestedSubEntry(item) &&
          m_liveAstEditor && m_liveAstPath == path) {
        const qulonglong entryIndexQ = item->data(0, Qt::UserRole + 6).toULongLong();
        const std::uint32_t entryIndex = static_cast<std::uint32_t>(entryIndexQ);
        auto resolved = resolveTexturePayloadForEditor(item, type.toUpper(), *m_liveAstEditor, entryIndex);
        if (!resolved.bytes.empty()) {
          if (resolved.bytes.size() > kMaxInflated) {
            resolved.bytes.resize(static_cast<std::size_t>(kMaxInflated));
          }
          return resolved.bytes;
        }
      }

      if (isEmbedded) {
        const QVariant pendingVar = item->data(0, Qt::UserRole + 30);
        if (pendingVar.isValid()) {
          const QByteArray pending = pendingVar.toByteArray();
          if (!pending.isEmpty()) return toVector(pending);
        }
      }

      const std::uint64_t off = static_cast<std::uint64_t>(baseOffset);
      std::uint64_t len = static_cast<std::uint64_t>(maxReadable);
      if (len == 0) {
        // Fallback for non-embedded files: read as much as we can.
        const std::uint64_t fsz = static_cast<std::uint64_t>(QFileInfo(path).size());
        len = (fsz > off) ? (fsz - off) : 0;
      }
      len = std::min<std::uint64_t>(len, kMaxStored);

      std::vector<std::uint8_t> raw = read_file_range(path, off, len);
      if (raw.empty()) return raw;

      // ZLIB in AST often stores compressed bytes, and "view" is only a preview.
      const bool isZlibType = (type == "ZLIB");
      const bool looksZlib = (!isZlibType && raw.size() >= 2 && looks_like_zlib_cmf_flg(raw[0], raw[1]));
      if (!isZlibType && !looksZlib) return raw;

      try {
        std::vector<std::uint8_t> inflated = zlib_inflate_unknown_size(std::span<const std::uint8_t>(raw.data(), raw.size()));
        if (inflated.size() > kMaxInflated) {
          inflated.resize(static_cast<std::size_t>(kMaxInflated));
        }
        return inflated;
      } catch (...) {
        return raw; // fall back
      }
    };

    // Only attempt heavy decode for files that look like textures.
const QString lowerName = QFileInfo(path).fileName().toLower();
auto containsDdsMagic = [](const QByteArray& b) -> bool {
  const int max = std::min<int>(b.size(), 0x4000);
  for (int i = 0; i + 3 < max; ++i) {
    if (b[i] == 'D' && b[i+1] == 'D' && b[i+2] == 'S' && b[i+3] == ' ') return true;
  }
  return false;
};
const bool wantsTexture =
    (type == "DDS" || type == "P3R" || type == "XPR2") ||
    lowerName.endsWith(".dds") || lowerName.endsWith(".p3r") ||
    lowerName.endsWith(".xpr") || lowerName.endsWith(".xpr2") ||
    (!stored.isEmpty() && (stored.startsWith("DDS ") || stored.startsWith("p3R") || stored.startsWith("P3R") || stored.startsWith("XPR2"))) ||
    containsDdsMagic(stored) || containsDdsMagic(view) ||
    (!stored.isEmpty() && stored.size() >= 4 && std::memcmp(stored.data(), "XPR2", 4) == 0) ||
    (!view.isEmpty() && view.size() >= 4 && std::memcmp(view.data(), "XPR2", 4) == 0);

if (wantsTexture) {
  clearCurrentTextureState();
  // Ensure we don't show stale preview if decode fails.
  if (m_imageView) m_imageView->setPixmap(QPixmap());
  if (m_imageView) m_imageView->setText("");
  m_textureOriginal = QPixmap();
  if (m_viewTabs) m_viewTabs->setCurrentIndex(2);

  std::vector<std::uint8_t> payload = load_full_payload();
  if (!payload.empty()) {
    const bool wasXpr2 = (payload.size() >= 4 && std::memcmp(payload.data(), "XPR2", 4) == 0);
    const bool wasP3R = (payload.size() >= 3 &&
                         (std::memcmp(payload.data(), "p3R", 3) == 0 ||
                          std::memcmp(payload.data(), "P3R", 3) == 0));

    // Many textures are either:
    //  - Standard DDS (starts with "DDS ")
    //  - P3R-wrapped DDS (starts with "p3R"/"P3R", often just a magic swap)
    //  - EA-wrapped DDS payload (no DDS header; requires rebuild using EA header + AST flags)
    std::span<const std::uint8_t> texBytes(payload.data(), payload.size());
    std::vector<std::uint8_t> rebuilt;

    const bool startsDds = (payload.size() >= 4 &&
                            payload[0] == 'D' && payload[1] == 'D' &&
                            payload[2] == 'S' && payload[3] == ' ');

    if (wasXpr2) {
      // Xbox 360 texture container (tiled DXT/ATI2). Rebuild a standard DDS before decode.
      std::string name;
      auto dds = gf::textures::rebuild_xpr2_dds_first(texBytes, &name);
      if (dds.has_value() && !dds->empty()) {
        rebuilt = std::move(*dds);
        texBytes = std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size());
        if (m_textureInfo) {
          auto xprInfoD = gf::textures::parse_xpr2_info(
              std::span<const std::uint8_t>(payload.data(), payload.size()));
          if (xprInfoD) {
            const char* fmtName = (xprInfoD->fmt_code == 0x52) ? "DXT1" :
                                  (xprInfoD->fmt_code == 0x53) ? "DXT3" :
                                  (xprInfoD->fmt_code == 0x54) ? "DXT5" :
                                  (xprInfoD->fmt_code == 0x71) ? "ATI2" :
                                  (xprInfoD->fmt_code == 0x7C) ? "DXT1(NM)" : "Unknown";
            QString infoText = QString("XPR2 | %1\u00D7%2 | %3")
                .arg(xprInfoD->width).arg(xprInfoD->height).arg(fmtName);
            if (!xprInfoD->first_name.empty())
              infoText += QString(" | %1").arg(QString::fromStdString(xprInfoD->first_name));
            m_textureInfo->setText(infoText);
          } else if (!name.empty()) {
            m_textureInfo->setText(QString("XPR2: %1").arg(QString::fromStdString(name)));
          }
        }
      }
    }

    if (wasP3R) {
      ResolvedTexturePayload resolvedPreview;
      resolvedPreview.bytes = payload;
      resolvedPreview.rawBytes = payload;
      resolvedPreview.rawSize = payload.size();
      resolvedPreview.source = QStringLiteral("preview.current-entry");
      resolvedPreview.rawSource = resolvedPreview.source;
      QString exportDetails;
      if (auto built = buildDdsForTextureExport(QStringLiteral("P3R"), resolvedPreview, astFlags, &exportDetails); built.has_value()) {
        rebuilt = std::move(*built);
        texBytes = std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size());
      } else if (m_textureInfo) {
        m_textureInfo->setText(exportDetails);
      }
    } else if (!startsDds) {
      // Non-P3R: attempt EA rebuild when the payload doesn't start with "DDS ".
      rebuilt = maybe_rebuild_ea_dds(texBytes, astFlags);
      if (!rebuilt.empty()) {
        texBytes = std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size());
      }
    }


    try {
      auto info = gf::textures::parse_dds_info(texBytes);
      if (info.has_value()) {
        m_currentTextureBytes = QByteArray(reinterpret_cast<const char*>(texBytes.data()), static_cast<int>(texBytes.size()));
        m_currentTextureType = wasXpr2 ? QString("XPR2") : (wasP3R ? QString("P3R") : QString("DDS"));
        m_currentTextureName = item->text(0);
        m_currentTextureSelectionVersion = m_previewContext.selectionVersion;
        m_previewContext.textureBytes = m_currentTextureBytes;
        m_previewContext.textureSource = QStringLiteral("converted.current-selection");
        m_previewContext.textureDetectedType = QStringLiteral("DDS");
        m_viewerLabel->setPlainText(QString("Selected: %1 (%2)\n%3\n%4\n\n%5")
                         .arg(item->text(0), type, fi.absoluteFilePath(), where, buildPreviewDiagnosticsText(m_previewContext)));
        if (renderCurrentTextureMip(0)) {
          { auto lg = gf::core::Log::get(); if (lg) lg->info("TextureDecode OK: {} ({}x{}, {})", path.toStdString(), info->width, info->height, ddsFormatToString(info->format).toStdString()); }
          hasTexture = true;
        } else {
          m_imageView->setPixmap(QPixmap());
          if (m_textureInfo) m_textureInfo->setText(QString("Texture %1x%2 (%3) - decode unsupported").arg(info->width).arg(info->height).arg(ddsFormatToString(info->format)));
          { auto lg = gf::core::Log::get(); if (lg) lg->warn("TextureDecode unsupported: {} ({}x{}, {})", path.toStdString(), info->width, info->height, ddsFormatToString(info->format).toStdString()); }
        }
      } else if (wasP3R) {
        m_imageView->setPixmap(QPixmap());
        if (m_textureInfo) m_textureInfo->setText("P3R detected, but no conversion stage produced a valid DDS buffer.");
        { auto lg = gf::core::Log::get(); if (lg) lg->warn("TextureDecode failed: P3R but rebuild failed for {}", path.toStdString()); }
      }
    } catch (const std::exception& e) {
      if (m_imageView) m_imageView->setPixmap(QPixmap());
      m_textureOriginal = QPixmap();
      if (m_textureInfo) m_textureInfo->setText(QString("Texture parse failed: %1").arg(e.what()));
    }
  }

	  // Auto-select best viewer.
  if (!hasTexture) {
    clearCurrentTextureState();
    if (m_textureInfo) m_textureInfo->setVisible(false);
  }

  if (m_viewTabs) {
    if (hasDat)
      m_viewTabs->setCurrentWidget(m_datTab);
    else {
      int idx = 0; // Hex
      if (hasTexture) idx = 2;
      else if (hasRsf) idx = 3;
      else if (hasText) idx = 1;
      m_viewTabs->setCurrentIndex(idx);
    }
  }
}
} // end MainWindow::showViewerForItem

} // namespace gf::gui

}

#include "MainWindow.moc"
