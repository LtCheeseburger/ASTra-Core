#pragma once
#include <QMainWindow>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>

class QEvent;
class QStackedWidget;

namespace gf::gui {
class ModProfileManager;
}

namespace gf::gui::update {
class VersionBadgeWidget;
}
class QSplitter;
class QTextBrowser;
class QCheckBox;
class QToolButton;
class QAction;

namespace gf::gui {

class GameLibrary;
class GameIconProvider;

// NOTE: Qualify Qt base classes to avoid accidental namespace shadowing.
class GameSelectorWindow final : public ::QMainWindow {
  Q_OBJECT
public:
  explicit GameSelectorWindow(::QWidget* parent = nullptr);

protected:
  bool eventFilter(::QObject* obj, ::QEvent* event) override;

signals:
  // Fired when user opens a game from the selector
  void openGameRequested(const QString& gameId);
  // Fired when user wants standalone AST editor mode
  void openAstEditorRequested();

private slots:
  void refresh();
  void onAddGame();
  void onRemoveGame();
  void onOpenGameFiles();
  void onOpenAstEditor();
  void onSelectionChanged();
  void onItemDoubleClicked(QListWidgetItem* item);
  void onManageProfiles();
  void onInstallMod();
  void onShowInstalledMods();
  void onConfigureRuntime();
  void onCreateBaseline();
  void onExportPackage();
  void onEditPackageMetadata();
  void onApplyProfile();
  void onApplyAndLaunch();
  void onLaunch();
  void onAbout();
  void onBetaTestingGuide();
  void onCheckForUpdates();
  void onStartupUpdateCheck();

private:
  void buildUi();
  QString selectedGameId() const;
  void rescanGameEntry(const QString& gameId, bool quiet);
  void exportDiagnosticsBundle();
  // v0.5.9: library import/export (tester reproduction / portability)
  void exportLibraryJson();
  void importLibraryJson();
  void updateCandidatePickers();

  GameLibrary* m_library = nullptr;
  GameIconProvider* m_icons = nullptr;

  QListWidget* m_list = nullptr;
  QWidget* m_emptyState = nullptr;
  QLabel* m_status = nullptr;

  QComboBox* m_viewMode = nullptr;
  QComboBox* m_sortMode = nullptr;
  QComboBox* m_platformFilter = nullptr;
  QComboBox* m_franchiseFilter = nullptr;
  QLineEdit* m_search = nullptr;

  class GameIconProvider* m_iconProvider = nullptr;

  QPushButton* m_btnExit = nullptr;
  QPushButton* m_btnOpenAstEditor = nullptr;
  QPushButton* m_btnAddGame = nullptr;
  QPushButton* m_btnRemoveGame = nullptr;
  QPushButton* m_btnOpenGameFiles = nullptr;

  // Selected-game / mod-profile panel (below the list)
  class QFrame* m_profilePanel    = nullptr;
  QLabel*       m_ppGameLabel     = nullptr;   // current game name
  QLabel*       m_ppProfileLabel  = nullptr;   // active profile status
  QPushButton*  m_ppManageBtn          = nullptr;   // "Manage Profiles…"
  QPushButton*  m_ppInstallBtn         = nullptr;   // "Install Mod…"
  QPushButton*  m_ppBrowseBtn          = nullptr;   // "Installed Mods…"
  QPushButton*  m_ppConfigureRuntimeBtn  = nullptr;  // "Configure Runtime…"
  QPushButton*  m_ppCreateBaselineBtn   = nullptr;  // "Create Baseline…"
  QPushButton*  m_ppExportBtn           = nullptr;  // "Export Package…"
  QPushButton*  m_ppEditMetadataBtn     = nullptr;  // "Edit Package Metadata…"
  QPushButton*  m_ppApplyBtn            = nullptr;  // "Apply Profile"
  QPushButton*  m_ppApplyLaunchBtn      = nullptr;  // "Apply & Launch"
  QPushButton*  m_ppLaunchBtn           = nullptr;  // "Launch"
  QLabel*       m_ppDeployStatusLabel   = nullptr;  // Deployment status indicator

  // v0.5.2: Tool hub / modules
  QListWidget* m_modules = nullptr;
  QStackedWidget* m_moduleStack = nullptr;
  QTextBrowser* m_moduleInfo = nullptr; // generic info panel for context
  QPushButton* m_btnExportDiagnostics = nullptr;

  QAction* m_actExportDiagnostics = nullptr;

  // v0.5.4: candidate file pickers (read-only, no decoding)
  QListWidget* m_astCandidates = nullptr;
  QListWidget* m_rsfCandidates = nullptr;
  QListWidget* m_texCandidates = nullptr;
  QListWidget* m_aptCandidates = nullptr;
  QListWidget* m_dbCandidates = nullptr;
  // v0.6.2: plain text/xml/config editor candidates
  QListWidget* m_textCandidates = nullptr;
  QLabel* m_astCandidatesStatus = nullptr;
  QLabel* m_rsfCandidatesStatus = nullptr;
  QLabel* m_texCandidatesStatus = nullptr;
  QLabel* m_aptCandidatesStatus = nullptr;
  QLabel* m_dbCandidatesStatus = nullptr;
  QLabel* m_textCandidatesStatus = nullptr;

  // v0.5.5: per-module search scope toggles (scan root vs full game root)
  QCheckBox* m_astScopeFullRoot = nullptr;
  QCheckBox* m_rsfScopeFullRoot = nullptr;
  QCheckBox* m_texScopeFullRoot = nullptr;
  QCheckBox* m_aptScopeFullRoot = nullptr;
  QCheckBox* m_dbScopeFullRoot = nullptr;

  // v0.6.2: optional scope toggle for text candidates
  QCheckBox* m_textScopeFullRoot = nullptr;


  // Version badge (top-right of menu bar chrome)
  gf::gui::update::VersionBadgeWidget* m_versionBadge = nullptr;
  void triggerUpdateCheck(bool silent);

  // Mod profile manager (game library context)
  ModProfileManager* m_profileManager = nullptr;

  // Active game context (selected in library)
  QString m_activeGameId;
};

} // namespace gf::gui
