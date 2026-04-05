#include "MediaImportDialog.hpp"

#include <QDialogButtonBox>
#include <QFont>
#include <QFrame>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

MediaImportDialog::MediaImportDialog(bool           possible,
                                     bool           backendAvailable,
                                     const QString& diagnosticText,
                                     QWidget*       parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Replace with MP4 — Preflight"));
    setMinimumSize(620, 460);

    auto* layout = new QVBoxLayout(this);

    // ── Verdict header ────────────────────────────────────────────────────────
    auto* headerLabel = new QLabel(this);
    headerLabel->setAlignment(Qt::AlignCenter);
    headerLabel->setWordWrap(false);

    if (possible) {
        headerLabel->setText(QStringLiteral("Import POSSIBLE"));
        headerLabel->setStyleSheet(
            QStringLiteral("font-size:16px; font-weight:bold; color:#2a9d2a;"
                           " padding:6px; background:#e8f5e9; border-radius:4px;"));
    } else {
        headerLabel->setText(QStringLiteral("Import NOT POSSIBLE"));
        headerLabel->setStyleSheet(
            QStringLiteral("font-size:16px; font-weight:bold; color:#c62828;"
                           " padding:6px; background:#ffebee; border-radius:4px;"));
    }
    layout->addWidget(headerLabel);

    // ── Diagnostic text ───────────────────────────────────────────────────────
    auto* textPane = new QPlainTextEdit(this);
    textPane->setReadOnly(true);
    textPane->setPlainText(diagnosticText);
    textPane->setFont(QFont(QStringLiteral("Courier New"), 9));
    layout->addWidget(textPane, /*stretch=*/1);

    // ── VP6 encoder requirements info ────────────────────────────────────────
    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    layout->addWidget(sep);

    auto* reqLabel = new QLabel(this);
    reqLabel->setWordWrap(true);
    reqLabel->setTextFormat(Qt::RichText);

    const QString statusIcon = backendAvailable
        ? QStringLiteral("<span style='color:#2a9d2a;font-weight:bold;'>&#x2714;</span>")
        : QStringLiteral("<span style='color:#c62828;font-weight:bold;'>&#x2718;</span>");
    const QString statusText = backendAvailable
        ? QStringLiteral("VP6 encoder detected")
        : QStringLiteral("No VP6 encoder found");

    reqLabel->setText(
        QStringLiteral(
            "<b>VP6 Encoding Requirements</b><br>"
            "%1 %2<br><br>"
            "<b>Recommended (best quality):</b> "
            "Install <b>VirtualDub</b> (1.x or 2.x) "
            "and the <b>On2 VP62 VFW codec</b>. "
            "ASTra will use VirtualDub to encode VP6 via the codec.<br><br>"
            "<b>Fallback:</b> A build of <b>FFmpeg</b> compiled with VP6 encoder support "
            "(rare — standard FFmpeg builds do not include it). "
            "Quality is lower than the VFW codec path."
        ).arg(statusIcon, statusText));
    reqLabel->setStyleSheet(
        QStringLiteral("padding:8px; background:#f5f5f5; border:1px solid #ddd; border-radius:4px;"));
    layout->addWidget(reqLabel);

    // ── Buttons ───────────────────────────────────────────────────────────────
    auto* buttonBox = new QDialogButtonBox(this);

    auto* proceedBtn = buttonBox->addButton(
        QStringLiteral("Proceed"), QDialogButtonBox::AcceptRole);
    buttonBox->addButton(QStringLiteral("Close"), QDialogButtonBox::RejectRole);

    const bool proceedEnabled = possible && backendAvailable;
    proceedBtn->setEnabled(proceedEnabled);

    if (possible && !backendAvailable) {
        proceedBtn->setToolTip(
            QStringLiteral("VP6 replacement backend is not available in this build.\n"
                           "Import is technically feasible but cannot be performed here."));
    } else if (!possible) {
        proceedBtn->setToolTip(
            QStringLiteral("Import is not possible — see the diagnostic details above."));
    }

    connect(buttonBox, &QDialogButtonBox::accepted, this, [this] {
        m_wantsProceed = true;
        accept();
    });
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    layout->addWidget(buttonBox);
    setLayout(layout);
}

bool MediaImportDialog::userWantsToProceed() const
{
    return m_wantsProceed;
}
