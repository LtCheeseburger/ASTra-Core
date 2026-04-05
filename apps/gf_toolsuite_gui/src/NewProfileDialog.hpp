#pragma once
#include <QDialog>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace gf::gui {

class ModProfileManager;

// What the user chose in NewProfileDialog.
struct NewProfileSpec {
    QString name;
    // Source profile to copy workspace content from (overlay/ + mods/).
    // Empty = start with a blank workspace.
    QString sourceProfileId;
    // Phase 7: absolute path to the game content directory to copy into game_copy/.
    // Empty = no copy (user can build the copy later from ModProfilesDialog).
    QString sourceContentPath;
    // Phase 7: if true and sourceContentPath is non-empty, copy game files into
    // game_copy/ immediately after the profile workspace is created.
    bool    copyContentNow = false;
    bool    setActive = false; // activate the new profile immediately after creation
};

// Modal "New Mod Profile" dialog presented when the user clicks "New..." in
// ModProfilesDialog.
//
// The dialog lets the user choose a name and a creation source:
//   - Baseline profile (copy captured content into the new workspace)
//   - Any existing profile (clone that profile's workspace)
//   - Blank workspace (empty shell — the previous default behavior)
//
// If any baseline profiles exist they are listed first and one is pre-selected,
// because starting from a baseline is the recommended workflow.
//
// The OK button is only enabled when the name field is non-empty.
class NewProfileDialog : public QDialog {
    Q_OBJECT
public:
    // sourceContentPath: pre-populated game content directory for the Phase 7 copy.
    // Pass the game's configured AST dir (or usrdirPath) so the field is ready to use.
    // Passing empty is fine — the field will be blank and the copy checkbox will be disabled.
    explicit NewProfileDialog(ModProfileManager* mgr,
                               const QString&     gameId,
                               const QString&     sourceContentPath = {},
                               QWidget*           parent            = nullptr);

    // Call after exec() == Accepted to retrieve the user's choices.
    NewProfileSpec spec() const;

private slots:
    void onNameChanged();
    void onBrowseContentPath();
    void onContentPathChanged();

private:
    void updateSourceHint(int comboIndex);
    QLineEdit*   m_nameEdit      = nullptr;
    QComboBox*   m_sourceCombo   = nullptr;
    QLabel*      m_sourceHint    = nullptr;
    // Phase 7: game content path + copy-now option
    QLineEdit*   m_srcPathEdit   = nullptr;
    QCheckBox*   m_chkCopyNow   = nullptr;
    QCheckBox*   m_chkSetActive  = nullptr;
    QPushButton* m_btnOk         = nullptr;

    // Parallel list: m_sourceIds[i] is the profile id for combo item i.
    // Empty string means "blank workspace".
    QStringList  m_sourceIds;
};

} // namespace gf::gui
