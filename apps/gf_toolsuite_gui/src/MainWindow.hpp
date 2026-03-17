#pragma once
#include <QMainWindow>
#include "DocumentLifecycle.hpp"
#include <QHash>
#include <QSet>
#include "NameCache.hpp"
#include <QVector>
#include <QPoint>
#include <optional>

#include "AstIndexer.hpp"
#include <gf/models/rsf.hpp>
#include <gf/models/rsf_preview.hpp>
#include <gf/core/AstContainerEditor.hpp>
#include <gf/apt/apt_reader.hpp>
#include <gf/apt/apt_action_inspector.hpp>
#include <gf/dat/dat_reader.hpp>
#include <memory>
#include <unordered_map>

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QPlainTextEdit;
class QDockWidget;
class QLineEdit;
class QWidget;
class QProgressBar;
class QTabWidget;
class QTableWidget;
class QAction;
class QToolBar;
class QCloseEvent;
class QStatusBar;
class QScrollArea;
class QComboBox;
class QSplitter;
class QStackedWidget;
class QSpinBox;
class QDoubleSpinBox;
class QGraphicsView;
class QGraphicsScene;
class QCheckBox;
class QTransform;

namespace gf::gui::apt_editor {
class AptPreviewScene;
class AptSelectionManager;
}
namespace gf::gui::rsf_editor {
class RsfPreviewWidget;
}

namespace gf::gui {

class MainWindow final : public QMainWindow {
  Q_OBJECT
 public:
  enum class Mode { Standalone, Game };

  explicit MainWindow(QWidget* parent = nullptr);

  void setMode(Mode m);
  void openStandaloneAst(const QString& astPath);

  // Game mode entrypoint (called by GameSelectorWindow)
  void openGame(const QString& displayName,
                const QString& rootPath,
                const QString& baseContentDir,
                const QString& updateContentDir);

protected:
  void closeEvent(QCloseEvent* e) override;

public slots:
  // Open a plain text file into the in-pane Text tab (no separate window).
  bool openExternalTextFile(const QString& path);


private slots:

    void onRestoreLatestBackup();
  void onItemExpanded(QTreeWidgetItem* item);
  void onItemDoubleClicked(QTreeWidgetItem* item, int column);
  void onTreeContextMenu(const QPoint& pos);
  void onSearchChanged(const QString& text);
  void onCurrentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous);
  void onOpenFile();
  void onOpenApt();
  void onAptSave();
  void onAptExport();
  void setAptFrameIndex(int idx);
  void onSave();
  void onShowCoreHelp();
  void onCheckForUpdates();
  void onExportLogs();
  void onSaveAs();
  void onRevert();
  void onAptApply();

private:
  Mode m_mode = Mode::Standalone;

  // Editing mode is intentionally explicit and off by default.
  // When disabled, all write/replace operations are gated.
  bool editingEnabled() const;
  void setEditingEnabled(bool enabled);

  bool devModeEnabled() const;
  void setDevModeEnabled(bool enabled);

  void buildUi();
  void startIndexing(const QString& displayName,
                     const QString& rootPath,
                     const QString& baseContentDir,
                     const QString& updateContentDir);
  void applyIndexToTree(const QVector<AstIndexEntry>& entries,
                        QTreeWidgetItem* gameRoot,
                        QTreeWidgetItem* baseBucket,
                        QTreeWidgetItem* updateBucket);

  // Search helper
  bool filterItemRecursive(QTreeWidgetItem* item, const QString& needleLower);

  // Right-side preview (currently: hex preview for any selected item)
  void showViewerForItem(QTreeWidgetItem* item);
  static QString formatHexPreview(const QByteArray& data, quint64 baseOffset);

  // File IO helpers (v0.6.1 foundation)
  void exportCopyOf(const QString& sourcePath);
  void onUndoLastReplace();
  void refreshCurrentArchiveView();

  // --- APT viewer groundwork (v0.6.21.0) ---
  void openStandaloneApt(const QString& aptPath);
  void updateDocumentActions();
  void syncDirtyFromUi();
  void setDirty(bool dirty);
  void updateWindowTitle();

  // Status bar: persistent context widgets (v0.6.20.9)
  void updateStatusBar();
  void updateStatusSelection(QTreeWidgetItem* current);

  // Centralized user-facing dialogs (v0.6.20.12 UX hardening)
  void showErrorDialog(const QString& title,
                       const QString& message,
                       const QString& details = QString(),
                       bool noChangesSaved = false);
  void showInfoDialog(const QString& title, const QString& message);
  void toastOk(const QString& message);

  void setRsfDirty(bool dirty);
  void refreshRsfMaterialsTable();
  void refreshRsfParamsTable(int materialIndex);
  void pullRsfUiIntoDocument();
  void refreshRsfPreview();
  void onRsfPreviewSelectionChanged(int materialIndex);
  void onRsfPreviewTransformEdited(int materialIndex, const gf::models::rsf::preview_transform& transform, bool interactive);
  std::optional<QByteArray> tryResolveRsfTextureBytes(const std::string& textureName, const std::string& textureFilename) const;
  bool applyRsfChanges();
  void clearAptViewer();
  bool populateAptViewerFromFiles(const QString& aptPath, const QString& constPath, const QString& sourceLabel = QString());
  bool loadAptForItem(QTreeWidgetItem* item, QString* errorOut = nullptr);

  // APT property editor helpers (v0.8.4)
  void syncAptPropEditorFromItem(QTreeWidgetItem* item);
  void refreshAptPreview();
  void syncAptEditorSceneContext();
  void onAptSceneSelectionChanged(int placementIndex);
  void onAptScenePlacementEdited(int placementIndex, bool interactive);
  void refreshAptPlacementTreeLabels();
  QTreeWidgetItem* findAptPlacementTreeItem(int ownerKind, int ownerIndex, int frameIndex, int placementIndex) const;
  void renderAptFrameToScene(const gf::apt::AptFrame* frame,
                             int highlightedPlacementIndex = -1,
                             const std::vector<gf::apt::AptCharacter>* characterTable = nullptr,
                             const QString& noFrameMsg = {},
                             bool fitToContent = false,
                             bool suppressContainerMsg = false);
  std::optional<int> selectedAptFrameIndex() const;
  std::optional<int> selectedAptPlacementIndex() const;

  // APT display-list diagnostics
  // Returns a formatted human-readable dump of the cumulative display list at frameIndex.
  QString buildDlSummaryText(const std::vector<gf::apt::AptFrame>& frames,
                             std::size_t frameIndex,
                             const QString& contextLabel) const;

  // DAT geometry preview
  void renderDatImageToScene(int imageIndex);
  // Lookup a DAT image by charId in the currently loaded DAT file.
  // Returns nullptr if no DAT is loaded or no entry matches.
  const gf::dat::DatImage* findDatImageByCharId(std::uint32_t charId) const noexcept;
  // Render DAT geometry triangles into an arbitrary scene using a given world transform.
  // Used for the APT->DAT fallback path. debugOverlay adds a "DAT fallback" label.
  void renderDatGeometryToAptScene(const gf::dat::DatImage& img,
                                   const QTransform& worldTransform,
                                   QGraphicsScene* scene,
                                   bool debugOverlay) const;

  // Builds (or returns cached) merged AptActionHints for a Sprite/Movie character.
  // Scans the character's own timeline Action bytes plus root-movie InitAction bytes.
  // Results are cached by charId and cleared when a new APT file is loaded.
  const gf::apt::AptActionHints& getCachedAptHints(
      std::uint32_t charId,
      const gf::apt::AptCharacter& ch,
      const std::vector<gf::apt::AptCharacter>& characterTable);

  void renderAptCharacterRecursive(std::uint32_t charId,
                                   const QTransform& parentTransform,
                                   const std::vector<gf::apt::AptCharacter>& characterTable,
                                   int rootPlacementIndex,
                                   int highlightRootPlacementIdx,
                                   bool debugOverlay,
                                   int recursionDepth,
                                   const QString& parentChainLabel);
  void renderAptResolvedFrameRecursive(const gf::apt::AptFrame& resolvedFrame,
                                       const QTransform& parentTransform,
                                       const std::vector<gf::apt::AptCharacter>& characterTable,
                                       int rootPlacementIndex,
                                       int highlightRootPlacementIdx,
                                       bool debugOverlay,
                                       int recursionDepth,
                                       const QString& parentChainLabel);

  QLabel* m_header = nullptr;
  QLineEdit* m_search = nullptr;
  QTreeWidget* m_tree = nullptr;

  QDockWidget* m_treeDock = nullptr;
  QWidget* m_viewerHost = nullptr;
  QPlainTextEdit* m_viewerLabel = nullptr;
  QTabWidget* m_viewTabs = nullptr;
  QPlainTextEdit* m_hexView = nullptr;
  QPlainTextEdit* m_textView = nullptr;
  QToolBar* m_textToolbar = nullptr;
  QAction* m_actCoreHelp = nullptr;
  QAction* m_textWrapAction = nullptr;
  QAction* m_textApplyAction = nullptr;
  QAction* m_textEditAction = nullptr; // Toggle editability for embedded text
  bool m_textForceEdit = false;
  QAction* m_textExportAction = nullptr;
  QAction* m_textReloadAction = nullptr;
  QAction* m_textOpenExternalAction = nullptr;
  QAction* m_textFindAction = nullptr;
  QAction* m_textFindNextAction = nullptr;

  QAction* m_textReplaceAction = nullptr;
  QAction* m_textGotoLineAction = nullptr;
  QAction* m_textSaveShortcutAction = nullptr; // Ctrl+S triggers Apply when possible
  int m_textTabIndex = -1;
  bool m_suppressSelectionChange = false;

  // Text tab external-file mode (no separate window)
  bool m_textExternalMode = false;
  QString m_textExternalPath;
  QString m_textExternalSuggestedName;

  // Text tab APT->XML mode (in-memory; no file writes unless user explicitly exports).
  bool m_textAptXmlMode = false;
  QString m_textAptPath;
  QString m_textConstPath;
  QString m_textAptXmlCached;
  QString m_lastFindQuery;
  QWidget* m_textureTab = nullptr;
  QLabel* m_textureInfo = nullptr;
  QComboBox* m_textureMipSelector = nullptr;
  QLabel* m_imageView = nullptr;
  QScrollArea* m_imageScroll = nullptr;
  // Keep the unscaled texture pixmap so we can re-fit it when the window/tab resizes.
  QPixmap m_textureOriginal;
  double m_textureZoom = 1.0;
  bool m_textureFitToView = true;
  QByteArray m_currentTextureBytes;
  QString m_currentTextureType;
  QString m_currentTextureName;
  int m_currentTextureMipCount = 0;
  int m_currentTextureMipShown = 0;

  // APT tab widgets
  QWidget* m_aptTab = nullptr;
  QToolBar* m_aptToolbar = nullptr;
  QAction* m_aptApplyAction = nullptr;
  QTreeWidget* m_aptTree = nullptr;
  QStackedWidget* m_aptPropStack = nullptr;
  QWidget* m_aptRightPane = nullptr;
  QGraphicsView* m_aptPreviewView = nullptr;
  gf::gui::apt_editor::AptPreviewScene* m_aptPreviewScene = nullptr;
  gf::gui::apt_editor::AptSelectionManager* m_aptSelectionManager = nullptr;
  QAction* m_aptDebugAction = nullptr;
  // APT zoom actions
  QAction* m_aptZoomInAction    = nullptr;
  QAction* m_aptZoomOutAction   = nullptr;
  QAction* m_aptZoomFitAction   = nullptr;
  QAction* m_aptZoom100Action   = nullptr;
  // APT render mode
  enum class AptRenderMode { Mixed = 0, Boxes = 1, Geometry = 2 };
  AptRenderMode m_aptRenderMode = AptRenderMode::Mixed;
  QComboBox* m_aptRenderModeCombo = nullptr;
  QLabel* m_aptDlStatusLabel = nullptr; // frame+DL summary shown below the preview
  // Page 0: plain text details (fallback / group / slice nodes)
  QPlainTextEdit* m_aptDetails = nullptr;
  // Page 1: Summary editor
  QWidget* m_aptSummaryPage = nullptr;
  QSpinBox* m_aptSumWidthSpin = nullptr;
  QSpinBox* m_aptSumHeightSpin = nullptr;
  QLabel* m_aptSumFrameCountLabel = nullptr;
  QLabel* m_aptSumCharCountLabel = nullptr;
  QLabel* m_aptSumImportCountLabel = nullptr;
  QLabel* m_aptSumExportCountLabel = nullptr;
  QLabel* m_aptSumOffsetLabel = nullptr;
  // Page 2: Import editor
  QWidget* m_aptImportPage = nullptr;
  QLineEdit* m_aptImportMovieEdit = nullptr;
  QLineEdit* m_aptImportNameEdit = nullptr;
  QLabel* m_aptImportCharLabel = nullptr;
  QLabel* m_aptImportOffsetLabel = nullptr;
  // Page 3: Export editor
  QWidget* m_aptExportPage = nullptr;
  QLineEdit* m_aptExportNameEdit = nullptr;
  QLabel* m_aptExportCharLabel = nullptr;
  QLabel* m_aptExportOffsetLabel = nullptr;
  // Page 4: Character info (read-only)
  QWidget* m_aptCharPage = nullptr;
  QLabel* m_aptCharTypeLabel = nullptr;
  QLabel* m_aptCharSigLabel = nullptr;
  QLabel* m_aptCharOffsetLabel = nullptr;
  QLabel* m_aptCharFrameCountLabel = nullptr;  // frame count (Sprite/Movie only)
  QLabel* m_aptCharBoundsLabel = nullptr;      // LTRB bounds if present
  QLabel* m_aptCharImportLabel = nullptr;      // import resolution for type=0 slots
  QPlainTextEdit* m_aptScaffoldDump = nullptr; // scaffold breakdown for runtime-only containers
  // Page 5: Frame info (read-only)
  QWidget* m_aptFramePage = nullptr;
  QLabel* m_aptFrameItemCountLabel = nullptr;
  QLabel* m_aptFrameItemsOffsetLabel = nullptr;
  QPlainTextEdit* m_aptFrameDlDump = nullptr; // cumulative display-list dump for selected frame
  // Page 6: Placement editor
  QWidget* m_aptPlacementPage = nullptr;
  QSpinBox* m_aptPlacementDepthSpin = nullptr;
  QSpinBox* m_aptPlacementCharSpin = nullptr;
  QLineEdit* m_aptPlacementNameEdit = nullptr;
  QDoubleSpinBox* m_aptPlacementXSpin = nullptr;
  QDoubleSpinBox* m_aptPlacementYSpin = nullptr;
  QDoubleSpinBox* m_aptPlacementScaleXSpin = nullptr;
  QDoubleSpinBox* m_aptPlacementScaleYSpin = nullptr;
  QLabel* m_aptPlacementOffsetLabel = nullptr;
  // APT in-memory model
  std::optional<gf::apt::AptFile> m_currentAptFile;
  bool m_aptDirty = false;
  bool m_aptUpdatingUi = false;
  bool m_aptPreviewInProgress = false; // re-entrancy guard for refreshAptPreview
  // Per-charId cache of merged AptActionHints. Cleared when a new APT file is loaded.
  std::unordered_map<std::uint32_t, gf::apt::AptActionHints> m_aptHintsCache;

  // APT frame navigation controls
  QSpinBox* m_aptFrameSpin = nullptr;
  QLabel* m_aptFrameCountLabel = nullptr;
  QAction* m_aptPrevFrameAction = nullptr;
  QAction* m_aptNextFrameAction = nullptr;
  int m_aptCurrentFrameIndex = 0;
  // -1 = previewing root movie frames; >= 0 = index of character being previewed
  int m_aptCharPreviewIdx = -1;

  // APT placement editor: rotation/skew matrix fields (b and c)
  QDoubleSpinBox* m_aptPlacementRotSkew0Spin = nullptr; // b = rotate_skew_0
  QDoubleSpinBox* m_aptPlacementRotSkew1Spin = nullptr; // c = rotate_skew_1

  // APT save/export toolbar actions
  QAction* m_aptSaveAction = nullptr;
  QAction* m_aptExportAction = nullptr;
  QAction* m_aptBringForwardAction = nullptr;
  QAction* m_aptSendBackwardAction = nullptr;
  QAction* m_aptAddPlacementAction = nullptr;
  QAction* m_aptRemovePlacementAction = nullptr;
  QAction* m_aptDuplicatePlacementAction = nullptr;

  // APT save context (set by loadAptForItem, cleared by clearAptViewer)
  bool m_aptIsEmbedded = false;
  bool m_aptSaveContextIsNested = false;
  int m_aptSaveAptEntryIdx = -1;
  int m_aptSaveConstEntryIdx = -1;
  QString m_aptSaveOuterPath;

  // DAT viewer tab
  QWidget*        m_datTab               = nullptr;
  QLabel*         m_datSummaryLabel      = nullptr;
  QTableWidget*   m_datImagesTable       = nullptr;
  QGraphicsView*  m_datPreviewView       = nullptr;
  QGraphicsScene* m_datPreviewScene      = nullptr;
  QLabel*         m_datEntryInfoLabel    = nullptr;
  QLabel*         m_datCorrelLabel       = nullptr;
  QCheckBox*      m_datApplyTransformCheck = nullptr;
  std::optional<gf::dat::DatFile> m_currentDatFile;

  QWidget* m_rsfTab = nullptr;
  QLabel* m_rsfNameValue = nullptr;
  QLabel* m_rsfModelCountValue = nullptr;
  QLabel* m_rsfMaterialCountValue = nullptr;
  QLabel* m_rsfTextureCountValue = nullptr;
  QTableWidget* m_rsfMaterialsTable = nullptr;
  QTableWidget* m_rsfParamsTable = nullptr;
  QTableWidget* m_rsfTexturesTable = nullptr;
  gf::gui::rsf_editor::RsfPreviewWidget* m_rsfPreviewWidget = nullptr;
  std::optional<gf::models::rsf::preview_document> m_rsfPreviewDoc;
  QToolBar* m_rsfToolbar = nullptr;
  QAction* m_rsfEditAction = nullptr;
  QAction* m_rsfApplyAction = nullptr;
  bool m_rsfEditMode = true;
  bool m_rsfDirty = false;
  bool m_rsfUpdatingUi = false;
  QString m_rsfSourcePath;
  bool m_rsfSourceEmbedded = false;
  std::uint32_t m_rsfSourceEntryIndex = 0;
  QByteArray m_rsfOriginalBytes;
  std::optional<gf::models::rsf::document> m_rsfCurrentDoc;


  // In-memory AST edit session for the currently edited container.
  std::unique_ptr<gf::core::AstContainerEditor> m_liveAstEditor;
  QString m_liveAstPath;
  bool m_saveInProgress = false;

  // Cache: key -> parsed? (key is absolutePath or absolutePath@offset)
  // Parsing state for expandable (embedded) AST nodes.
  // We keep an in-flight set to avoid launching duplicate parses, and a done set
  // so re-expanding doesn't re-parse.
  QSet<QString> m_parseKeysInFlight;
  QSet<QString> m_parsedKeysDone;

  // Persistent friendly-name cache (per game)
  NameCache m_nameCache;

  // UI progress (top-right via status bar permanent widget)
  QProgressBar* m_parseProgress = nullptr;
  int m_parseInFlight = 0;

  // Status bar context widgets
  QLabel* m_statusDocLabel = nullptr;
  QLabel* m_statusEntryLabel = nullptr;
  QLabel* m_statusMetaLabel = nullptr;
  QLabel* m_statusDirtyLabel = nullptr;

  // Cached selection context (avoid depending on live QTreeWidgetItem*)
  QString m_statusContainerPath;
  QString m_statusEntryName;
  QString m_statusEntryType;
  qulonglong m_statusEntrySize = 0;
  qulonglong m_statusEntryFlags = 0;

  // Toolbar/menu actions
  QAction* m_actOpen = nullptr;
  QAction* m_actOpenApt = nullptr;
  QAction* m_actRestoreBackup = nullptr;
  QAction* m_actSave = nullptr;
  QAction* m_actSaveAs = nullptr;
  QAction* m_actRevert = nullptr;

  QAction* m_actEnableEditing = nullptr;
  QLabel* m_editModeLabel = nullptr;

  QAction* m_actDevMode = nullptr;
  QAction* m_actUndoLastReplace = nullptr;
  QAction* m_actCheckForUpdates = nullptr;
  bool m_devMode = false;

  bool m_editingEnabled = false;

  struct LastReplaceUndo {
    bool valid = false;
    QString containerPath;
    std::uint32_t entryIndex = 0;
    QByteArray previousStoredBytes;
    QByteArray previousPreviewBytes;
    QString itemCacheKey;
    QString displayName;
  };
  LastReplaceUndo m_lastReplaceUndo;

  // Current "document" context (still read-only in v0.6.1)
  DocumentLifecycle m_doc;

  // Used to reload NameCache + rebuild UI on Revert.
  QString m_cacheId;
  QString m_lastGameDisplayName;
  QString m_lastGameRootPath;
  QString m_lastGameBaseContentDir;
  QString m_lastGameUpdateContentDir;

  // Incremented whenever we rebuild the tree. Used to ignore late async updates.
  quint64 m_treeToken = 0;

protected:
  void resizeEvent(QResizeEvent* e) override;
  bool eventFilter(QObject* obj, QEvent* event) override;

private:
  struct PreviewSelectionContext {
    quint64 selectionVersion = 0;
    QString entryPath;
    QString entryDisplayName;
    QString entryType;
    quint32 entryIndex = 0;
    bool isEmbedded = false;
    // True when this item is a leaf entry inside a *nested* embedded sub-AST
    // (i.e. it has an ancestor tree item whose type is "AST").
    // In that case entryIndex is relative to the inner sub-AST directory, NOT to the
    // outer on-disk container that m_liveAstEditor represents.  Using
    // m_liveAstEditor->getEntryStoredBytes(entryIndex) would silently return the wrong
    // outer-AST entry – typically a tiny XML metadata stub.  We must use the direct
    // file-range read (which uses UserRole+1 = absolute byte offset) instead.
    bool isNestedSubEntry = false;

    QByteArray rawBytes;
    QByteArray inflatedBytes;
    QByteArray hexBytes;
    QByteArray textBytes;
    QByteArray textureBytes;

    QString rawSource;
    QString inflatedSource;
    QString hexSource;
    QString textSource;
    QString textureSource;
    QString textDetectedType;
    QString textureDetectedType;
  };

  void invalidatePreviewContext();
  void applyTextureZoom();
  void stepTextureZoom(int direction);
  void resetTextureZoomToFit();
  void populateTextureMipSelector(int mipCount);
  bool renderCurrentTextureMip(int mipIndex);
  void clearCurrentTextureState();
  PreviewSelectionContext buildPreviewContextForItem(QTreeWidgetItem* item) const;
  QString buildPreviewDiagnosticsText(const PreviewSelectionContext& ctx) const;

private:
  quint64 m_previewSelectionVersion = 0;
  PreviewSelectionContext m_previewContext;
  quint64 m_currentTextureSelectionVersion = 0;
};

} // namespace gf::gui
