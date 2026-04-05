#pragma once
#include "ModCatalogEntry.hpp"
#include "ModProfile.hpp"
#include <QDialog>

class QTableView;
class QTextBrowser;
class QPushButton;
class QLabel;

namespace gf::gui {

class InstalledModsModel;

// Profile-scoped installed mod browser.
//
// Shows all mods installed under the active profile's workspace, their status,
// and allows the user to enable/disable individual mods and refresh the view.
//
// The dialog reads directly from the profile's installed_registry.json via
// ModCatalogService.  All mutations (enable/disable) are persisted atomically.
//
// Pre-condition: profile must be valid with a non-empty workspacePath.
class InstalledModsDialog : public QDialog {
    Q_OBJECT
public:
    explicit InstalledModsDialog(const ModProfile& profile,
                                 const QString&    gameDisplayName,
                                 QWidget*          parent = nullptr);

private slots:
    void onSelectionChanged();
    void onEnable();
    void onDisable();
    void onUninstall();
    void onRefresh();

private:
    void populate();
    void updateButtons();
    void showEntryDetails(const ModCatalogEntry& entry);
    void clearDetails();

    std::optional<ModCatalogEntry> selectedEntry() const;

    ModProfile  m_profile;
    QString     m_gameDisplayName;

    InstalledModsModel* m_model        = nullptr;
    QTableView*         m_table        = nullptr;
    QTextBrowser*       m_details      = nullptr;
    QLabel*             m_contextLabel = nullptr;
    QLabel*             m_countLabel   = nullptr;

    QPushButton* m_btnEnable    = nullptr;
    QPushButton* m_btnDisable   = nullptr;
    QPushButton* m_btnUninstall = nullptr;
    QPushButton* m_btnRefresh   = nullptr;
    QPushButton* m_btnClose     = nullptr;
};

} // namespace gf::gui
