#pragma once

#include <QDialog>

class QLabel;
class QPlainTextEdit;
class QPushButton;

// ─────────────────────────────────────────────────────────────────────────────
// MediaImportDialog
//
// Presents the result of prepareEaVp6Import() to the user before committing to
// an import.  Responsibilities:
//   - Shows a prominent POSSIBLE / NOT POSSIBLE header.
//   - Displays the full describeImportPreparation() text in a scrollable pane.
//   - Enables "Proceed" only when result.possible == true AND the backend is
//     available; otherwise shows the button disabled with a tooltip explaining
//     why.
//   - "Close" is always present.
//
// After exec(), call userWantsToProceed() to know whether to start the import.
// ─────────────────────────────────────────────────────────────────────────────

class MediaImportDialog : public QDialog {
    Q_OBJECT

public:
    // possible        — prepareEaVp6Import() result.possible
    // backendAvailable — EaVp6ReplacementBackend::available()
    // diagnosticText  — describeImportPreparation(result)
    explicit MediaImportDialog(bool          possible,
                               bool          backendAvailable,
                               const QString& diagnosticText,
                               QWidget*       parent = nullptr);

    // Returns true only when the user clicked "Proceed" (and it was enabled).
    bool userWantsToProceed() const;

private:
    bool m_wantsProceed = false;
};
