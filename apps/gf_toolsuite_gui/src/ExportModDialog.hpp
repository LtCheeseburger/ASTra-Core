#pragma once
#include "ModPackageSpec.hpp"
#include <QDialog>
#include <optional>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextBrowser;

namespace gf::gui {

// Modal dialog that collects the metadata needed to export an ASTra mod package.
//
// Pre-fills targetGameIds and platforms where possible.  The caller invokes
// ModPackageExporter::exportPackage() after the dialog is accepted.
//
// Accepted state exposes:
//   spec()      — the filled-in ModExportSpec (includes iconSourcePath / previewSourcePaths)
//   outputDir() — the parent directory chosen by the user
class ExportModDialog : public QDialog {
    Q_OBJECT
public:
    // gameId      — profile ID to pre-fill target_game_ids
    // profileName — active profile name, used for display only
    explicit ExportModDialog(const QString& gameId,
                             const QString& profileName,
                             QWidget*       parent = nullptr);

    ModExportSpec spec()      const;
    QString       outputDir() const;

private slots:
    void onBrowseOutputDir();
    void onBrowseIcon();
    void onClearIcon();
    void onAddPreview();
    void onRemovePreview();
    void onExport();

private:
    void updateStatus(const QStringList& errors);
    void setIconPath(const QString& absPath);

    QString m_gameId;

    QLineEdit*    m_modIdEdit      = nullptr;
    QLineEdit*    m_nameEdit       = nullptr;
    QLineEdit*    m_versionEdit    = nullptr;
    QLineEdit*    m_authorEdit     = nullptr;
    QLineEdit*    m_descEdit       = nullptr;
    QLineEdit*    m_categoryEdit   = nullptr;
    QLineEdit*    m_outputDirEdit  = nullptr;

    // Phase 5C: icon
    QLineEdit*    m_iconPathEdit   = nullptr;   // shows absolute path; read-only
    QLabel*       m_iconPreview    = nullptr;   // 64×64 thumbnail

    // Phase 5C: preview images
    QListWidget*  m_previewList    = nullptr;
    QPushButton*  m_btnAddPreview  = nullptr;
    QPushButton*  m_btnRemPreview  = nullptr;

    QTextBrowser* m_statusArea     = nullptr;
    QPushButton*  m_btnExport      = nullptr;
};

} // namespace gf::gui
