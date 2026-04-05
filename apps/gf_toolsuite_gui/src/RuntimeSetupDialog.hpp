#pragma once
#include "RuntimeTargetConfig.hpp"
#include <QDialog>
#include <optional>

class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;

namespace gf::gui {

// Modal dialog for configuring the runtime target for a specific game.
//
// Phase 6A fields:
//   - Platform (informational — RPCS3 only)
//   - RPCS3 executable path
//   - Base Content Root (directory containing flat *.ast / *.AST files)
//   - Update Content Root (optional; ASTra recursively discovers *.ast,
//     *.ast.edat, and DLC subfolders automatically — no explicit DLC list)
//
// Flow:
//   1. User fills in paths (Browse buttons provided)
//   2. Click "Validate & Save" — validates existence + base content file count
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
    void onBrowseBaseRoot();
    void onBrowseUpdateRoot();
    void onValidateAndSave();

private:
    void updateValidationStatus(const QStringList& errors);

    QString m_gameId;

    QLineEdit*    m_rpcs3Edit        = nullptr;
    QLineEdit*    m_baseRootEdit     = nullptr;
    QLineEdit*    m_updateRootEdit   = nullptr;
    QTextBrowser* m_statusArea       = nullptr;
    QPushButton*  m_btnSave          = nullptr;

    std::optional<RuntimeTargetConfig> m_savedConfig;
};

} // namespace gf::gui
