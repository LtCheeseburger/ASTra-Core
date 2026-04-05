#pragma once
#include "ProfileResolverService.hpp"
#include "RuntimeTargetConfig.hpp"
#include <QDialog>

class QTextBrowser;
class QPushButton;

namespace gf::gui {

// Shows a pre-apply summary of what files will be written and to which roots.
//
// Grouped by destination root:
//   Base:    <N files>  → <astDirPath>
//   Update:  <M files>  → <updateRootPath>
//
// Lists the first few representative file paths in each group.
//
// Returns QDialog::Accepted if the user clicks "Apply", Rejected on Cancel.
class ApplyPreviewDialog : public QDialog {
    Q_OBJECT
public:
    explicit ApplyPreviewDialog(const ProfileResolvedMap&  resolved,
                                 const RuntimeTargetConfig& runtime,
                                 const QString&             profileName,
                                 QWidget*                   parent = nullptr);

private:
    void buildReport(const ProfileResolvedMap&  resolved,
                     const RuntimeTargetConfig& runtime);

    QTextBrowser* m_report  = nullptr;
    QPushButton*  m_btnApply  = nullptr;
    QPushButton*  m_btnCancel = nullptr;
};

} // namespace gf::gui
