#pragma once
#include "ModInstallPlan.hpp"
#include <QStringList>

namespace gf::gui {

// Result of a completed (or failed) install transaction.
struct InstallResult {
    bool        success = false;
    QString     message;          // Human-readable summary
    QStringList installedFiles;   // Relative paths successfully committed (empty on failure)
    QStringList errors;           // Error details on failure
};

// Executes an InstallPlan with staged-commit semantics:
//   1. Stage all payload files into a sibling temp directory
//   2. Rename the temp directory atomically into the destination slot
//   3. On any failure: roll back by removing temp and any partial destination
//
// Does NOT touch the registry — that is the caller's responsibility after a
// successful result.
class ModInstallTransaction {
public:
    // Execute the plan.  The workspace must already exist.
    // Returns a result indicating success or failure with diagnostics.
    static InstallResult execute(const InstallPlan& plan);

private:
    static bool stageFiles(const InstallPlan& plan,
                           const QString&     tempRoot,
                           QStringList&       outStaged,
                           QString&           outErr);

    static bool commitStage(const QString& tempRoot,
                            const QString& dstRoot,
                            QString&       outErr);

    static void rollback(const QString& tempRoot, const QString& dstRoot);
};

} // namespace gf::gui
