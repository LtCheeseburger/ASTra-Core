#pragma once
#include <QDialog>
#include <QString>
#include <functional>

class QLabel;
class QProgressBar;
class QPushButton;
class QPlainTextEdit;
class QWidget;
class QFormLayout;
class QGridLayout;
class QFrame;

namespace gf::gui {

// ── OperationContext ──────────────────────────────────────────────────────────
// Structured diagnostic data attached to an operation.
// All fields are optional — populate what is known.
struct OperationContext {
    QString operationType;    // "Extract", "Export", "Import", "Save", "Save As"
    QString archivePath;      // path to the source AST/BGFA
    QString entryName;        // selected entry inside the archive
    QString sourcePath;       // input file path (for import/replace)
    QString destinationPath;  // output folder or file
    QString detectedFormat;   // e.g. "DDS BC3", "P3R"
    QString validationResult; // "Passed" / "Failed" / ""
    QString stageReached;     // last pipeline stage reached, e.g. "load", "decode"
    QString errorText;        // raw error string from the underlying failure

    // Clipboard-ready formatted report (clean header block, full paths).
    QString toClipboardText() const;
};

// ── OperationResult ───────────────────────────────────────────────────────────
struct OperationResult {
    bool             success    = false;
    QString          summary;       // one-line description of the failure
    QString          outputPath;    // enables "Open Folder" on success
    OperationContext ctx;           // diagnostic context (used on failure)
};

// ── OperationStatusDialog ─────────────────────────────────────────────────────
// Compact 3-state modal dialog:
//
//  InProgress — indeterminate bar, "Please wait…"
//  Success    — green bar, auto-closes after ~1.1 s
//  Failure    — no bar, structured info grid + highlighted error block,
//               Details pane (collapsed), Copy Details / Open Log / Close
//
class OperationStatusDialog : public QDialog {
    Q_OBJECT
public:
    explicit OperationStatusDialog(const QString& operationType,
                                   QWidget* parent = nullptr);

    // Update the running stage hint shown during InProgress.
    void setStageText(const QString& stage);

    // Transition to success.  outputPath enables "Open Folder".
    void setSuccess(const QString& outputPath = {});

    // Transition to failure with structured diagnostics.
    void setFailure(const QString& summary, const OperationContext& ctx = {});

    // Auto-close delay after setSuccess() — default 1100 ms.
    void setAutoCloseDelay(int ms) { m_autoCloseMs = ms; }

private slots:
    void onCopyDetails();
    void onOpenLog();
    void onOpenFolder();
    void onToggleDetails();

private:
    // Helper: elide a long path for display (preserves full path in m_ctx).
    static QString elidedPath(const QString& path, int maxChars = 58);

    // Populate the info grid rows from m_ctx.
    void populateInfoGrid(const OperationContext& ctx);

    QString           m_operationType;
    QString           m_outputPath;
    int               m_autoCloseMs = 1100;
    OperationContext  m_ctx;          // stored for clipboard formatting
    bool              m_detailsOpen  = false;

    // ── In-progress / success widgets ────────────────────────────────────────
    QLabel*       m_titleLabel      = nullptr;  // "Saving…" / "Saved" / "Save Failed"
    QLabel*       m_statusLabel     = nullptr;  // secondary message
    QProgressBar* m_progress        = nullptr;

    // ── Failure-specific widgets ──────────────────────────────────────────────
    QWidget*      m_failureBody     = nullptr;  // entire failure section (hidden until failure)
    QWidget*      m_infoGrid        = nullptr;  // Operation / Archive / Entry / Stage grid
    QGridLayout*  m_infoLayout      = nullptr;
    QFrame*       m_errorBlock      = nullptr;  // highlighted error text block
    QLabel*       m_errorLabel      = nullptr;

    // Collapsible advanced details (raw dump)
    QWidget*      m_detailsPanel    = nullptr;
    QPlainTextEdit* m_detailsView   = nullptr;

    // ── Buttons ───────────────────────────────────────────────────────────────
    QPushButton*  m_btnDetails      = nullptr;  // "Details ▾"
    QPushButton*  m_btnCopy         = nullptr;  // "Copy Report"
    QPushButton*  m_btnOpenLog      = nullptr;  // "Open Log"
    QPushButton*  m_btnOpenFolder   = nullptr;  // "Open Folder" (success)
    QPushButton*  m_btnClose        = nullptr;
};

// ── runWithProgress ───────────────────────────────────────────────────────────
// Sync convenience wrapper: opens the dialog, runs work(), transitions state.
//
// Example:
//   OperationContext ctx;
//   ctx.operationType = "Extract";
//   ctx.archivePath   = path;
//   ctx.entryName     = entry;
//
//   runWithProgress(this, "Extract", [&]() -> OperationResult {
//       ...do work...
//       if (failed) return {false, "Could not open archive.", {}, ctx};
//       return {true, {}, outDir};
//   });
void runWithProgress(QWidget* parent,
                     const QString& operationType,
                     std::function<OperationResult()> work);

} // namespace gf::gui
