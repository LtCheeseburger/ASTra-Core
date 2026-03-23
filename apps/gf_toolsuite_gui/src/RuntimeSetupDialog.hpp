#pragma once
#include "RuntimeTargetConfig.hpp"
#include <QDialog>
#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextBrowser;

namespace gf::gui {

// Modal dialog for configuring the runtime target for a specific game.
//
// Fields:
//   - Platform (dropdown — currently RPCS3 only)
//   - RPCS3 executable path
//   - Base Game AST directory path (directory containing qkl_*.AST files)
//   - Phase 5A: Update Root path (optional)
//   - Phase 5A: DLC Roots list (zero or more additional content roots)
//
// Flow:
//   1. User fills in paths (Browse buttons provided)
//   2. Click "Validate & Save" — validates existence + AST file count
//   3. On success: saves via RuntimeTargetManager and accepts
//   4. On failure: shows error detail, lets user correct and retry
//
// The dialog does NOT prompt for baseline capture.  That decision belongs
// to the caller (GameSelectorWindow::onConfigureRuntime).
class RuntimeSetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit RuntimeSetupDialog(const QString& gameId,
                                const QString& gameDisplayName,
                                QWidget*       parent = nullptr);

    // After accepted(), the saved config is accessible here.
    std::optional<RuntimeTargetConfig> savedConfig() const { return m_savedConfig; }

private slots:
    void onBrowseRpcs3();
    void onBrowseAstDir();
    void onBrowseUpdateRoot();
    void onAddDlcRoot();
    void onRemoveDlcRoot();
    void onValidateAndSave();

private:
    void updateValidationStatus(const QStringList& errors);

    QString m_gameId;

    QLineEdit*   m_rpcs3Edit       = nullptr;
    QLineEdit*   m_astDirEdit      = nullptr;
    // Phase 5A: optional update root
    QLineEdit*   m_updateRootEdit  = nullptr;
    // Phase 5A: DLC roots
    QListWidget* m_dlcList         = nullptr;
    QPushButton* m_btnRemoveDlc    = nullptr;
    QTextBrowser* m_statusArea     = nullptr;
    QPushButton* m_btnSave         = nullptr;

    // In-memory list of DLC/custom content roots (does not include base or update).
    QVector<RuntimeContentRoot> m_dlcRoots;

    std::optional<RuntimeTargetConfig> m_savedConfig;
};

} // namespace gf::gui
