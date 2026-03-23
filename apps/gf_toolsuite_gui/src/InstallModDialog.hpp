#pragma once
#include "ModManifest.hpp"
#include "ModInstallPlan.hpp"
#include "ModProfile.hpp"
#include "ModRegistryStore.hpp"
#include <QDialog>
#include <optional>

class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;

namespace gf::gui {

// Modal dialog for installing a local mod into an active profile workspace.
//
// Flow:
//   1. User browses to a mod folder (must contain astra_mod.json + files/)
//   2. Dialog reads and validates the manifest immediately on folder selection
//   3. Builds an InstallPlan and shows a validation/plan summary
//   4. "Install" is enabled only when plan.canProceed()
//   5. On confirmation, ModInstaller::install() is called
//   6. Result summary is shown; user can close with "Done"
//
// Pre-condition: profile must be valid and have a workspace.
class InstallModDialog : public QDialog {
    Q_OBJECT
public:
    explicit InstallModDialog(const ModProfile& profile,
                              QWidget*          parent = nullptr);

private slots:
    void onBrowse();
    void onInstall();

private:
    void validateFolder(const QString& folder);
    void showPlanSummary(const InstallPlan& plan);
    void showResult(bool success, const QString& message,
                    const QStringList& warnings);

    ModProfile                  m_profile;
    ModRegistryStore            m_registry; // stateless store — constructed per dialog

    std::optional<ModManifest>  m_manifest;
    std::optional<InstallPlan>  m_plan;

    QLineEdit*   m_pathEdit   = nullptr;
    QPushButton* m_btnBrowse  = nullptr;
    QTextBrowser* m_summary   = nullptr;
    QPushButton* m_btnInstall = nullptr;
    QPushButton* m_btnClose   = nullptr;
};

} // namespace gf::gui
