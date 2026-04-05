#pragma once
#include "ModProfile.hpp"
#include <QDialog>
#include <optional>

class QListWidget;
class QListWidgetItem;
class QLabel;
class QPushButton;
class QTextBrowser;

namespace gf::gui {

class ModProfileManager;

// Profile management dialog for a single game.
//
// Phase 5D additions:
//   - Right-click context menu (Set Active, Rename, Delete, Open Location, Copy Path).
//   - "Open Location" and "Copy Path" buttons in the details pane.
//   - Improved details: baseline badge, monospace path display.
//   - New profiles use the human-readable workspace folder naming.
//
// Phase 6F additions:
//   - Custom profile icon (stored as <workspace>/profile_icon.png).
//   - "Set Icon…" / "Clear Icon" actions (context menu + details pane buttons).
//   - ProfileListDelegate: icon thumbnail + name + Active/Baseline badges per row.
//   - 64×64 icon preview in the details pane.
//   - Double-click a row to activate that profile.
//   - "Activate & Close" button for fast one-click profile switching.
//
// The dialog is non-modal: it can stay open while the user works.
class ModProfilesDialog : public QDialog {
    Q_OBJECT
public:
    // sourceContentPath: absolute path to the game's content directory, forwarded to
    // NewProfileDialog so new profiles can copy game files on creation.
    // Pass empty if not yet known — the user can browse inside NewProfileDialog.
    explicit ModProfilesDialog(ModProfileManager* mgr,
                               const QString&     gameId,
                               const QString&     gameDisplayName,
                               const QString&     sourceContentPath = {},
                               QWidget*           parent            = nullptr);

private slots:
    void onSelectionChanged();
    void onItemDoubleClicked(QListWidgetItem* item);
    void onCreate();
    void onClone();
    void onRename();
    void onDelete();
    void onSetActive();
    void onSetActiveAndClose();
    void onDeactivate();
    void onSetIcon();
    void onClearIcon();
    void onOpenLocation();
    void onCopyPath();
    void onBuildCopy();   // Phase 7: populate game_copy/ for the selected profile
    void onContextMenu(const QPoint& pos);

private:
    void rebuildList();
    void updateButtons();
    void updateDetailsIcon();
    std::optional<ModProfile> selectedProfile() const;

    ModProfileManager* m_mgr;
    QString            m_gameId;
    QString            m_gameDisplayName;
    QString            m_sourceContentPath; // Phase 7: forwarded to NewProfileDialog

    QListWidget*  m_list               = nullptr;
    QLabel*       m_iconLabel          = nullptr;  // Phase 6F: icon preview
    QTextBrowser* m_detailsPane        = nullptr;
    QPushButton*  m_btnCreate          = nullptr;
    QPushButton*  m_btnClone           = nullptr;
    QPushButton*  m_btnRename          = nullptr;
    QPushButton*  m_btnDelete          = nullptr;
    QPushButton*  m_btnSetActive       = nullptr;
    QPushButton*  m_btnSetActiveClose  = nullptr;  // Phase 6F
    QPushButton*  m_btnDeactivate      = nullptr;
    QPushButton*  m_btnOpenLocation    = nullptr;
    QPushButton*  m_btnCopyPath        = nullptr;
    QPushButton*  m_btnSetIcon         = nullptr;  // Phase 6F
    QPushButton*  m_btnClearIcon       = nullptr;  // Phase 6F
    QPushButton*  m_btnBuildCopy       = nullptr;  // Phase 7: build/rebuild game_copy/
};

} // namespace gf::gui
