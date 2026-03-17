#include "UpdateDialog.hpp"

#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>

namespace gf::gui::update {

// ── UpdateDialog ─────────────────────────────────────────────────────────────

UpdateDialog::UpdateDialog(const ReleaseInfo& info, QWidget* parent)
    : QDialog(parent)
    , m_info(info)
{
    setWindowTitle(tr("New Version Available"));
    setMinimumWidth(500);
    setModal(true);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);

    // Title label
    auto* titleLabel = new QLabel(
        QStringLiteral("<b style=\"font-size:14px;\">ASTra %1</b>").arg(info.tagName),
        this);
    root->addWidget(titleLabel);

    // Horizontal rule
    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    root->addWidget(line);

    // Release notes header
    auto* notesHeader = new QLabel(tr("Release Notes:"), this);
    notesHeader->setStyleSheet("font-weight: bold;");
    root->addWidget(notesHeader);

    // Release notes body (read-only)
    auto* notes = new QPlainTextEdit(this);
    notes->setReadOnly(true);
    notes->setPlainText(info.body.isEmpty() ? tr("No release notes available.") : info.body);
    notes->setFixedHeight(180);
    root->addWidget(notes);

    // Spacer
    root->addSpacing(4);

    // Buttons
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();

    m_btnUpdate = new QPushButton(tr("Update Now"), this);
    m_btnUpdate->setDefault(true);
    m_btnUpdate->setStyleSheet(
        "QPushButton { background-color: #6f42c1; color: white; padding: 6px 18px; border-radius: 4px; }"
        "QPushButton:hover { background-color: #8a5cd0; }"
    );

    m_btnLater = new QPushButton(tr("Later"), this);
    m_btnLater->setStyleSheet("QPushButton { padding: 6px 14px; }");

    btnRow->addWidget(m_btnUpdate);
    btnRow->addWidget(m_btnLater);
    root->addLayout(btnRow);

    connect(m_btnUpdate, &QPushButton::clicked, this, &UpdateDialog::onUpdateNow);
    connect(m_btnLater,  &QPushButton::clicked, this, &UpdateDialog::onLater);
}

void UpdateDialog::onUpdateNow() {
    emit updateRequested(m_info);
    accept();
}

void UpdateDialog::onLater() {
    reject();
}

// ── UpToDateDialog ────────────────────────────────────────────────────────────

UpToDateDialog::UpToDateDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Check for Updates"));
    setModal(true);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(12);

    auto* label = new QLabel(
        tr("You are running the latest version of ASTra."), this);
    label->setAlignment(Qt::AlignCenter);
    root->addWidget(label);

    auto* btn = new QPushButton(tr("OK"), this);
    btn->setDefault(true);
    btn->setFixedWidth(80);

    auto* row = new QHBoxLayout;
    row->addStretch();
    row->addWidget(btn);
    row->addStretch();
    root->addLayout(row);

    connect(btn, &QPushButton::clicked, this, &QDialog::accept);
}

} // namespace gf::gui::update
