#pragma once
#include "ModProfile.hpp"
#include <QDialog>
#include <optional>

class QListWidget;
class QLabel;
class QPushButton;
class QTextBrowser;

namespace gf::gui {

class ModProfileManager;

// Profile management dialog for a single game.
//
// Shows the profile list with the active entry indicated, and provides
// create / rename / delete / set-active / deactivate actions.
//
// Phase 5D additions:
//   - Right-click context menu on the profile list (Set Active, Rename, Delete,
//     Open Profile Location, Copy Workspace Path).
//   - "Open Location" and "Copy Path" buttons in the details pane.
//   - Improved details: baseline badge, monospace path display.
//   - New profiles created via this dialog use the human-readable workspace
//     folder naming (game-slug/profile-slug__shortId/).
//
// The dialog is non-modal: it can stay open while the user works.
class ModProfilesDialog : public QDialog {
    Q_OBJECT
public:
    explicit ModProfilesDialog(ModProfileManager* mgr,
                               const QString&     gameId,
                               const QString&     gameDisplayName,
                               QWidget*           parent = nullptr);

private slots:
    void onSelectionChanged();
    void onCreate();
    void onRename();
    void onDelete();
    void onSetActive();
    void onDeactivate();
    void onOpenLocation();
    void onCopyPath();
    void onContextMenu(const QPoint& pos);

private:
    void rebuildList();
    void updateButtons();
    std::optional<ModProfile> selectedProfile() const;

    ModProfileManager* m_mgr;
    QString            m_gameId;
    QString            m_gameDisplayName;

    QListWidget*  m_list           = nullptr;
    QTextBrowser* m_detailsPane    = nullptr;
    QPushButton*  m_btnCreate      = nullptr;
    QPushButton*  m_btnRename      = nullptr;
    QPushButton*  m_btnDelete      = nullptr;
    QPushButton*  m_btnSetActive   = nullptr;
    QPushButton*  m_btnDeactivate  = nullptr;
    QPushButton*  m_btnOpenLocation = nullptr;
    QPushButton*  m_btnCopyPath     = nullptr;
};

} // namespace gf::gui
