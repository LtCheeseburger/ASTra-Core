#pragma once
#include <QDialog>
#include <QStringList>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextBrowser;

namespace gf::gui {

// Dialog for editing the metadata of an existing ASTra mod package on disk.
//
// Opens <packageDir>/astra_mod.json, presents all editable fields including
// icon and preview images, and writes the updated manifest back on Save.
//
// Asset handling:
//   - If the user selects a new icon from outside the package, it is copied
//     to <packageDir>/icon.png on Save.
//   - Previews added from outside the package are copied to
//     preview_1.ext, preview_2.ext, etc. on Save.
//   - Removed previews are dropped from the manifest; source files are NOT
//     deleted from disk.
//
// Pre-condition: packageDir must contain a valid astra_mod.json.
class ModMetadataEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit ModMetadataEditorDialog(const QString& packageDir,
                                     QWidget*       parent = nullptr);

private slots:
    void onSave();
    void onBrowseIcon();
    void onClearIcon();
    void onAddPreview();
    void onRemovePreview();

private:
    bool loadManifest();
    bool saveManifest();
    void updateIconPreview(const QString& absPath);
    void setStatus(const QString& msg, bool isError);
    void rebuildPreviewLabels();

    QString m_packageDir;

    QLineEdit*   m_modIdEdit      = nullptr;
    QLineEdit*   m_nameEdit       = nullptr;
    QLineEdit*   m_versionEdit    = nullptr;
    QLineEdit*   m_authorEdit     = nullptr;
    QLineEdit*   m_descEdit       = nullptr;
    QLineEdit*   m_categoryEdit   = nullptr;
    QLineEdit*   m_tagsEdit       = nullptr;   // comma-separated
    QLineEdit*   m_notesEdit      = nullptr;

    // Icon
    QLineEdit*   m_iconPathEdit   = nullptr;   // absolute source path (read-only display)
    QLabel*      m_iconPreview    = nullptr;   // 64×64 thumbnail

    // Preview images — UserRole stores absolute source path
    QListWidget* m_previewList    = nullptr;
    QPushButton* m_btnAddPreview  = nullptr;
    QPushButton* m_btnRemPreview  = nullptr;

    // Status + actions
    QLabel*      m_statusLabel    = nullptr;
    QPushButton* m_btnSave        = nullptr;
};

} // namespace gf::gui
