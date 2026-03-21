#include "OperationStatusDialog.hpp"

#include <gf/core/log.hpp>

#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QTimer>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>
#include <QSizePolicy>

namespace gf::gui {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Truncate long paths to keep the grid readable.
// The full path is always preserved in m_ctx for the clipboard copy.
// static
QString OperationStatusDialog::elidedPath(const QString& path, int maxChars) {
    if (path.length() <= maxChars) return path;
    // Keep the start and the filename — most useful parts.
    const QString filename = QFileInfo(path).fileName();
    const int prefixLen = maxChars - filename.length() - 5;  // 5 = " … / "
    if (prefixLen < 6)
        return QStringLiteral("\u2026") + path.right(maxChars - 1);
    return path.left(prefixLen) + QStringLiteral("\u2026/") + filename;
}

// Small read-only label used inside the info grid (right column).
static QLabel* makeValueLabel(const QString& text, QWidget* parent) {
    auto* lbl = new QLabel(text, parent);
    lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lbl->setWordWrap(false);
    lbl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    lbl->setStyleSheet("QLabel { color: #dddddd; }");
    return lbl;
}

// Small dimmed label for the key column of the info grid.
static QLabel* makeKeyLabel(const QString& text, QWidget* parent) {
    auto* lbl = new QLabel(text + ':', parent);
    lbl->setAlignment(Qt::AlignRight | Qt::AlignTop);
    lbl->setStyleSheet("QLabel { color: #777777; font-size: 11px; }");
    lbl->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    return lbl;
}

// ── OperationContext::toClipboardText ─────────────────────────────────────────

QString OperationContext::toClipboardText() const {
    QString out = QStringLiteral("--- ASTra Operation Failure ---\n");

    // Fixed-width key column for clipboard alignment.
    auto pad = [](const QString& s) -> QString {
        return s.leftJustified(14, ' ');
    };

    if (!operationType.isEmpty())    out += pad("Operation")    + " " + operationType    + "\n";
    if (!archivePath.isEmpty())      out += pad("Archive")      + " " + archivePath      + "\n";
    if (!entryName.isEmpty())        out += pad("Entry")        + " " + entryName        + "\n";
    if (!sourcePath.isEmpty())       out += pad("Source")       + " " + sourcePath       + "\n";
    if (!destinationPath.isEmpty())  out += pad("Destination")  + " " + destinationPath  + "\n";
    if (!detectedFormat.isEmpty())   out += pad("Format")       + " " + detectedFormat   + "\n";
    if (!validationResult.isEmpty()) out += pad("Validation")   + " " + validationResult + "\n";
    if (!stageReached.isEmpty())     out += pad("Stage")        + " " + stageReached     + "\n";

    const QString logPath = QString::fromStdString(gf::core::Log::logFilePath());
    if (!logPath.isEmpty())          out += pad("Log File")     + " " + logPath          + "\n";

    if (!errorText.isEmpty())
        out += QStringLiteral("\nError:\n") + errorText + "\n";

    return out.trimmed();
}

// ── OperationStatusDialog ─────────────────────────────────────────────────────

OperationStatusDialog::OperationStatusDialog(const QString& operationType,
                                             QWidget* parent)
    : QDialog(parent)
    , m_operationType(operationType)
{
    setWindowTitle(operationType + QStringLiteral("\u2026"));
    setModal(true);
    setMinimumWidth(460);
    setMaximumWidth(640);
    setSizeGripEnabled(false);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(0);
    root->setContentsMargins(20, 18, 20, 16);

    // ── Header: icon + title ──────────────────────────────────────────────────
    auto* headerRow = new QHBoxLayout;
    headerRow->setSpacing(10);

    // State indicator (unicode bullet, re-styled per state)
    // Using a filled circle — clean, cross-platform, no icon resources needed.
    m_titleLabel = new QLabel(
        QStringLiteral("\u25cf  <b style=\"font-size:13px;\">%1\u2026</b>").arg(operationType),
        this);
    m_titleLabel->setTextFormat(Qt::RichText);
    headerRow->addWidget(m_titleLabel, 1);
    root->addLayout(headerRow);
    root->addSpacing(6);

    // ── Status / secondary message ────────────────────────────────────────────
    m_statusLabel = new QLabel(tr("Please wait while ASTra processes the file."), this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet("QLabel { color: #888888; font-size: 11px; }");
    root->addWidget(m_statusLabel);
    root->addSpacing(10);

    // ── Progress bar (visible during InProgress and briefly on Success) ───────
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);   // indeterminate
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(4);
    m_progress->setStyleSheet(
        "QProgressBar { border: none; background: #2a2a2a; border-radius: 2px; }"
        "QProgressBar::chunk { background: #555555; border-radius: 2px; }");
    root->addWidget(m_progress);

    // ── Failure body (hidden during InProgress / Success) ─────────────────────
    m_failureBody = new QWidget(this);
    m_failureBody->hide();
    auto* failLayout = new QVBoxLayout(m_failureBody);
    failLayout->setContentsMargins(0, 12, 0, 0);
    failLayout->setSpacing(10);

    // Info grid (Operation / Archive / Entry / Destination / Stage)
    m_infoGrid = new QWidget(m_failureBody);
    m_infoLayout = new QGridLayout(m_infoGrid);
    m_infoLayout->setContentsMargins(0, 0, 0, 0);
    m_infoLayout->setHorizontalSpacing(10);
    m_infoLayout->setVerticalSpacing(4);
    m_infoLayout->setColumnStretch(1, 1);
    failLayout->addWidget(m_infoGrid);

    // Error block — highlighted with a left-border accent
    m_errorBlock = new QFrame(m_failureBody);
    m_errorBlock->setFrameShape(QFrame::NoFrame);
    m_errorBlock->setStyleSheet(
        "QFrame {"
        "  background-color: rgba(192, 57, 43, 0.12);"
        "  border-left: 3px solid #c0392b;"
        "  border-radius: 0 3px 3px 0;"
        "}");
    auto* errorLayout = new QVBoxLayout(m_errorBlock);
    errorLayout->setContentsMargins(10, 7, 10, 7);

    auto* errorKey = new QLabel(tr("Error"), m_errorBlock);
    errorKey->setStyleSheet("QLabel { color: #888888; font-size: 10px; text-transform: uppercase; }");
    errorLayout->addWidget(errorKey);

    m_errorLabel = new QLabel(m_errorBlock);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_errorLabel->setStyleSheet("QLabel { color: #e74c3c; font-size: 12px; }");
    errorLayout->addWidget(m_errorLabel);

    failLayout->addWidget(m_errorBlock);

    // ── Collapsible advanced details panel ─────────────────────────────────
    m_detailsPanel = new QWidget(m_failureBody);
    m_detailsPanel->hide();
    auto* detailsLayout = new QVBoxLayout(m_detailsPanel);
    detailsLayout->setContentsMargins(0, 4, 0, 0);
    detailsLayout->setSpacing(0);

    auto* detailsSep = new QFrame(m_detailsPanel);
    detailsSep->setFrameShape(QFrame::HLine);
    detailsSep->setFrameShadow(QFrame::Sunken);
    detailsLayout->addWidget(detailsSep);
    detailsLayout->addSpacing(4);

    m_detailsView = new QPlainTextEdit(m_detailsPanel);
    m_detailsView->setReadOnly(true);
    m_detailsView->setFixedHeight(130);
    m_detailsView->setStyleSheet(
        "QPlainTextEdit {"
        "  font-family: Consolas, 'Courier New', monospace;"
        "  font-size: 11px;"
        "  background: #161616;"
        "  color: #aaaaaa;"
        "  border: 1px solid #333333;"
        "  border-radius: 3px;"
        "}");
    detailsLayout->addWidget(m_detailsView);

    failLayout->addWidget(m_detailsPanel);
    root->addWidget(m_failureBody);

    // ── Path / output label (success state) ───────────────────────────────────
    m_btnOpenFolder = new QPushButton(tr("Open Folder"), this);
    m_btnOpenFolder->setStyleSheet("QPushButton { padding: 4px 10px; }");
    m_btnOpenFolder->hide();
    connect(m_btnOpenFolder, &QPushButton::clicked, this, &OperationStatusDialog::onOpenFolder);

    // ── Button row ────────────────────────────────────────────────────────────
    root->addSpacing(14);

    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);

    m_btnDetails = new QPushButton(tr("Details \u25be"), this);
    m_btnDetails->setStyleSheet("QPushButton { padding: 4px 10px; }");
    m_btnDetails->hide();
    connect(m_btnDetails, &QPushButton::clicked, this, &OperationStatusDialog::onToggleDetails);

    m_btnCopy = new QPushButton(tr("Copy Report"), this);
    m_btnCopy->setStyleSheet("QPushButton { padding: 4px 10px; }");
    m_btnCopy->hide();
    connect(m_btnCopy, &QPushButton::clicked, this, &OperationStatusDialog::onCopyDetails);

    m_btnOpenLog = new QPushButton(tr("Open Log"), this);
    m_btnOpenLog->setStyleSheet("QPushButton { padding: 4px 10px; }");
    m_btnOpenLog->hide();
    connect(m_btnOpenLog, &QPushButton::clicked, this, &OperationStatusDialog::onOpenLog);

    m_btnClose = new QPushButton(tr("Close"), this);
    m_btnClose->setDefault(true);
    m_btnClose->setStyleSheet("QPushButton { padding: 4px 14px; }");
    m_btnClose->hide();
    connect(m_btnClose, &QPushButton::clicked, this, &QDialog::accept);

    btnRow->addWidget(m_btnDetails);
    btnRow->addWidget(m_btnCopy);
    btnRow->addWidget(m_btnOpenLog);
    btnRow->addStretch();
    btnRow->addWidget(m_btnOpenFolder);
    btnRow->addWidget(m_btnClose);

    root->addLayout(btnRow);
}

// ── populateInfoGrid ──────────────────────────────────────────────────────────

void OperationStatusDialog::populateInfoGrid(const OperationContext& ctx) {
    // Clear any previous rows.
    while (m_infoLayout->count()) {
        auto* item = m_infoLayout->takeAt(0);
        delete item->widget();
        delete item;
    }

    int row = 0;
    auto addRow = [&](const QString& key, const QString& fullValue) {
        if (fullValue.isEmpty()) return;
        m_infoLayout->addWidget(makeKeyLabel(key, m_infoGrid), row, 0);
        m_infoLayout->addWidget(makeValueLabel(elidedPath(fullValue), m_infoGrid), row, 1);
        ++row;
    };

    addRow(tr("Operation"),   ctx.operationType);
    addRow(tr("Archive"),     ctx.archivePath);
    addRow(tr("Entry"),       ctx.entryName);
    addRow(tr("Source"),      ctx.sourcePath);
    addRow(tr("Destination"), ctx.destinationPath);
    addRow(tr("Format"),      ctx.detectedFormat);
    addRow(tr("Validation"),  ctx.validationResult);
    if (!ctx.stageReached.isEmpty()) {
        // Capitalise first letter for display.
        QString stage = ctx.stageReached;
        if (!stage.isEmpty()) stage[0] = stage[0].toUpper();
        addRow(tr("Failed at"), stage);
    }
}

// ── State transitions ─────────────────────────────────────────────────────────

void OperationStatusDialog::setStageText(const QString& stage) {
    m_statusLabel->setText(stage);
}

void OperationStatusDialog::setSuccess(const QString& outputPath) {
    m_outputPath = outputPath;

    setWindowTitle(m_operationType);
    m_titleLabel->setText(
        QStringLiteral("<span style=\"color:#27ae60;\">&#x25cf;</span>"
                       "&nbsp;&nbsp;<b style=\"font-size:13px;\">%1</b>")
        .arg(m_operationType.toHtmlEscaped()));

    m_statusLabel->setText(tr("Completed successfully."));
    m_statusLabel->setStyleSheet("QLabel { color: #27ae60; font-size: 11px; }");

    m_progress->setRange(0, 100);
    m_progress->setValue(100);
    m_progress->setStyleSheet(
        "QProgressBar { border: none; background: #1a3a2a; border-radius: 2px; }"
        "QProgressBar::chunk { background: #27ae60; border-radius: 2px; }");

    if (!outputPath.isEmpty())
        m_btnOpenFolder->show();

    QTimer::singleShot(m_autoCloseMs, this, &QDialog::accept);
}

void OperationStatusDialog::setFailure(const QString& summary,
                                        const OperationContext& ctx) {
    m_ctx = ctx;

    setWindowTitle(m_operationType + tr(" \u2014 Failed"));

    // Header: red indicator
    m_titleLabel->setText(
        QStringLiteral("<span style=\"color:#c0392b;\">&#x25cf;</span>"
                       "&nbsp;&nbsp;<b style=\"font-size:13px;\">%1 Failed</b>")
        .arg(m_operationType.toHtmlEscaped()));

    // Secondary message: one-line summary
    const QString msg = summary.isEmpty()
        ? tr("The operation could not be completed.")
        : summary;
    m_statusLabel->setText(msg);
    m_statusLabel->setStyleSheet("QLabel { color: #aaaaaa; font-size: 11px; }");

    // Hide progress bar entirely — it has no meaning in failure state.
    m_progress->hide();

    // Error block text
    const QString errText = ctx.errorText.isEmpty() ? msg : ctx.errorText;
    m_errorLabel->setText(errText);
    m_errorBlock->setVisible(!errText.isEmpty());

    // Populate info grid
    populateInfoGrid(ctx);

    // Advanced details: full clipboard text (raw, for testers)
    m_detailsView->setPlainText(ctx.toClipboardText());

    // Show the failure body and relevant buttons.
    m_failureBody->show();
    m_btnDetails->show();
    m_btnCopy->show();

    const QString logPath = QString::fromStdString(gf::core::Log::logFilePath());
    if (!logPath.isEmpty())
        m_btnOpenLog->show();

    m_btnClose->show();

    adjustSize();
}

// ── Slot implementations ──────────────────────────────────────────────────────

void OperationStatusDialog::onCopyDetails() {
    QApplication::clipboard()->setText(m_ctx.toClipboardText());
}

void OperationStatusDialog::onOpenLog() {
    const QString logPath = QString::fromStdString(gf::core::Log::logFilePath());
    if (!logPath.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(logPath));
}

void OperationStatusDialog::onOpenFolder() {
    if (m_outputPath.isEmpty()) return;
    const QFileInfo fi(m_outputPath);
    const QString dir = fi.isDir() ? m_outputPath : fi.absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void OperationStatusDialog::onToggleDetails() {
    m_detailsOpen = !m_detailsOpen;
    m_detailsPanel->setVisible(m_detailsOpen);
    m_btnDetails->setText(m_detailsOpen ? tr("Details \u25b4") : tr("Details \u25be"));
    adjustSize();
}

// ── runWithProgress ───────────────────────────────────────────────────────────

void runWithProgress(QWidget* parent,
                     const QString& operationType,
                     std::function<OperationResult()> work) {
    auto* dlg = new OperationStatusDialog(operationType, parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->open();

    // Paint the dialog before blocking work starts.
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    const OperationResult result = work();

    if (result.success) {
        dlg->setSuccess(result.outputPath);
    } else {
        dlg->setFailure(result.summary, result.ctx);
    }

    if (dlg->isVisible())
        dlg->exec();
}

} // namespace gf::gui
