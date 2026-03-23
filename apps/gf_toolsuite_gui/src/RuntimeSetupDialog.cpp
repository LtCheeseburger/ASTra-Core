#include "RuntimeSetupDialog.hpp"
#include "RuntimeTargetManager.hpp"

#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace gf::gui {

RuntimeSetupDialog::RuntimeSetupDialog(const QString& gameId,
                                       const QString& gameDisplayName,
                                       QWidget*       parent)
    : QDialog(parent)
    , m_gameId(gameId)
{
    setWindowTitle(QString("Configure Runtime \u2014 %1").arg(gameDisplayName));
    setMinimumWidth(560);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(8);

    // Header
    auto* hdr = new QLabel(
        QString("<b>Game:</b> %1").arg(gameDisplayName.toHtmlEscaped()), this);
    outer->addWidget(hdr);

    auto* sub = new QLabel(
        "Configure the emulator and content directories ASTra will use for this game.",
        this);
    sub->setWordWrap(true);
    sub->setStyleSheet("color: palette(mid);");
    outer->addWidget(sub);

    // ── Core settings form ────────────────────────────────────────────────────
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 4, 0, 4);
    form->setSpacing(6);

    // Platform (informational only — RPCS3 is the only supported target)
    auto* platformLabel = new QLabel("RPCS3", this);
    platformLabel->setStyleSheet("font-weight: bold;");
    form->addRow("Platform:", platformLabel);

    // RPCS3 exe row
    auto* rpcs3Row = new QHBoxLayout();
    rpcs3Row->setSpacing(6);
    m_rpcs3Edit = new QLineEdit(this);
    m_rpcs3Edit->setPlaceholderText("Path to rpcs3.exe\u2026");
    auto* btnBrowseRpcs3 = new QPushButton("Browse\u2026", this);
    btnBrowseRpcs3->setFixedWidth(80);
    rpcs3Row->addWidget(m_rpcs3Edit, 1);
    rpcs3Row->addWidget(btnBrowseRpcs3);
    form->addRow("RPCS3 Executable:", rpcs3Row);

    // Base Game AST directory row
    auto* astRow = new QHBoxLayout();
    astRow->setSpacing(6);
    m_astDirEdit = new QLineEdit(this);
    m_astDirEdit->setPlaceholderText("Directory containing qkl_*.AST files (base game)\u2026");
    auto* btnBrowseAst = new QPushButton("Browse\u2026", this);
    btnBrowseAst->setFixedWidth(80);
    astRow->addWidget(m_astDirEdit, 1);
    astRow->addWidget(btnBrowseAst);
    form->addRow("Base Game AST Dir:", astRow);

    outer->addLayout(form);

    // ── Phase 5A: Additional content roots ────────────────────────────────────
    auto* extraGroup = new QGroupBox("Additional Content Roots (optional)", this);
    auto* extraForm  = new QFormLayout(extraGroup);
    extraForm->setContentsMargins(8, 8, 8, 8);
    extraForm->setSpacing(6);

    // Update root row
    auto* updateRow = new QHBoxLayout();
    updateRow->setSpacing(6);
    m_updateRootEdit = new QLineEdit(extraGroup);
    m_updateRootEdit->setPlaceholderText("Directory containing update qkl_*.AST files (optional)\u2026");
    auto* btnBrowseUpdate = new QPushButton("Browse\u2026", extraGroup);
    btnBrowseUpdate->setFixedWidth(80);
    updateRow->addWidget(m_updateRootEdit, 1);
    updateRow->addWidget(btnBrowseUpdate);
    extraForm->addRow("Update Root:", updateRow);

    // DLC roots list
    auto* dlcNote = new QLabel(
        "Additional DLC or custom content roots that also contain qkl_*.AST files:",
        extraGroup);
    dlcNote->setWordWrap(true);
    dlcNote->setStyleSheet("color: palette(mid); font-size: small;");
    extraForm->addRow(dlcNote);

    m_dlcList = new QListWidget(extraGroup);
    m_dlcList->setFixedHeight(80);
    m_dlcList->setAlternatingRowColors(true);
    extraForm->addRow("DLC Roots:", m_dlcList);

    auto* dlcBtns = new QHBoxLayout();
    dlcBtns->setSpacing(6);
    auto* btnAddDlc = new QPushButton("Add\u2026", extraGroup);
    btnAddDlc->setFixedWidth(72);
    m_btnRemoveDlc = new QPushButton("Remove", extraGroup);
    m_btnRemoveDlc->setFixedWidth(72);
    m_btnRemoveDlc->setEnabled(false);
    dlcBtns->addStretch(1);
    dlcBtns->addWidget(btnAddDlc);
    dlcBtns->addWidget(m_btnRemoveDlc);
    extraForm->addRow(dlcBtns);

    outer->addWidget(extraGroup);

    // ── Validation status ─────────────────────────────────────────────────────
    m_statusArea = new QTextBrowser(this);
    m_statusArea->setFixedHeight(80);
    m_statusArea->setReadOnly(true);
    m_statusArea->setOpenLinks(false);
    m_statusArea->setHtml(
        "<i style='color:gray;'>Fill in the paths above, then click Validate &amp; Save.</i>");
    outer->addWidget(m_statusArea);

    // ── Buttons ───────────────────────────────────────────────────────────────
    auto* btns = new QHBoxLayout();
    btns->addStretch(1);
    m_btnSave = new QPushButton("Validate \u0026 Save", this);
    m_btnSave->setDefault(true);
    auto* btnCancel = new QPushButton("Cancel", this);
    btns->addWidget(m_btnSave);
    btns->addWidget(btnCancel);
    outer->addLayout(btns);

    // ── Pre-fill from existing config ─────────────────────────────────────────
    {
        const auto existing = RuntimeTargetManager::load(gameId);
        if (existing.has_value()) {
            m_rpcs3Edit->setText(existing->rpcs3ExePath);
            m_astDirEdit->setText(existing->astDirPath);

            // Restore content roots
            for (const RuntimeContentRoot& cr : existing->contentRoots) {
                if (cr.kind == RuntimeContentKind::Update) {
                    m_updateRootEdit->setText(cr.path);
                } else {
                    m_dlcRoots.append(cr);
                    const QString label = cr.displayName.isEmpty()
                        ? runtimeContentKindLabel(cr.kind) + ": " + cr.path
                        : cr.displayName + "  [" + cr.path + "]";
                    m_dlcList->addItem(label);
                }
            }
        }
    }

    // ── Connections ───────────────────────────────────────────────────────────
    connect(btnBrowseRpcs3,   &QPushButton::clicked, this, &RuntimeSetupDialog::onBrowseRpcs3);
    connect(btnBrowseAst,     &QPushButton::clicked, this, &RuntimeSetupDialog::onBrowseAstDir);
    connect(btnBrowseUpdate,  &QPushButton::clicked, this, &RuntimeSetupDialog::onBrowseUpdateRoot);
    connect(btnAddDlc,        &QPushButton::clicked, this, &RuntimeSetupDialog::onAddDlcRoot);
    connect(m_btnRemoveDlc,   &QPushButton::clicked, this, &RuntimeSetupDialog::onRemoveDlcRoot);
    connect(m_btnSave,        &QPushButton::clicked, this, &RuntimeSetupDialog::onValidateAndSave);
    connect(btnCancel,        &QPushButton::clicked, this, &QDialog::reject);

    connect(m_dlcList, &QListWidget::currentRowChanged, this, [this](int row) {
        m_btnRemoveDlc->setEnabled(row >= 0);
    });
}

void RuntimeSetupDialog::onBrowseRpcs3() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Select RPCS3 Executable",
        m_rpcs3Edit->text().isEmpty()
            ? QDir::homePath()
            : QFileInfo(m_rpcs3Edit->text()).absolutePath(),
        "Executables (*.exe);;All Files (*)");
    if (!path.isEmpty())
        m_rpcs3Edit->setText(QDir::toNativeSeparators(path));
}

void RuntimeSetupDialog::onBrowseAstDir() {
    const QString path = QFileDialog::getExistingDirectory(
        this,
        "Select Base Game AST Directory",
        m_astDirEdit->text().isEmpty()
            ? QDir::homePath()
            : m_astDirEdit->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!path.isEmpty())
        m_astDirEdit->setText(QDir::toNativeSeparators(path));
}

void RuntimeSetupDialog::onBrowseUpdateRoot() {
    const QString path = QFileDialog::getExistingDirectory(
        this,
        "Select Update Root Directory",
        m_updateRootEdit->text().isEmpty()
            ? QDir::homePath()
            : m_updateRootEdit->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!path.isEmpty())
        m_updateRootEdit->setText(QDir::toNativeSeparators(path));
}

void RuntimeSetupDialog::onAddDlcRoot() {
    // Ask user to select a directory
    const QString path = QFileDialog::getExistingDirectory(
        this,
        "Select DLC / Custom Content Root Directory",
        QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (path.isEmpty()) return;

    // Ask for a display name (optional)
    bool ok = false;
    const QString name = QInputDialog::getText(
        this,
        "DLC Root Display Name",
        "Enter a short label for this content root (optional):",
        QLineEdit::Normal,
        QString(),
        &ok);
    if (!ok) return; // user cancelled

    RuntimeContentRoot cr;
    cr.kind        = RuntimeContentKind::Dlc;
    cr.path        = QDir::toNativeSeparators(path);
    cr.displayName = name.trimmed();

    m_dlcRoots.append(cr);

    const QString label = cr.displayName.isEmpty()
        ? cr.path
        : cr.displayName + "  [" + cr.path + "]";
    m_dlcList->addItem(label);
}

void RuntimeSetupDialog::onRemoveDlcRoot() {
    const int row = m_dlcList->currentRow();
    if (row < 0 || row >= m_dlcRoots.size()) return;

    delete m_dlcList->takeItem(row);
    m_dlcRoots.removeAt(row);
    m_btnRemoveDlc->setEnabled(m_dlcList->currentRow() >= 0);
}

void RuntimeSetupDialog::onValidateAndSave() {
    RuntimeTargetConfig cfg;
    cfg.gameId       = m_gameId;
    cfg.platform     = RuntimePlatform::RPCS3;
    cfg.rpcs3ExePath = m_rpcs3Edit->text().trimmed();
    cfg.astDirPath   = m_astDirEdit->text().trimmed();
    cfg.configuredAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    // Phase 5A: build content roots from update root + DLC list.
    const QString updatePath = m_updateRootEdit->text().trimmed();
    if (!updatePath.isEmpty()) {
        RuntimeContentRoot cr;
        cr.kind        = RuntimeContentKind::Update;
        cr.path        = updatePath;
        cr.displayName = "Update";
        cfg.contentRoots.append(cr);
    }
    for (const RuntimeContentRoot& cr : m_dlcRoots) {
        cfg.contentRoots.append(cr);
    }

    QStringList errors;
    if (!RuntimeTargetManager::validate(cfg, &errors)) {
        updateValidationStatus(errors);
        return;
    }

    QString saveErr;
    if (!RuntimeTargetManager::save(cfg, &saveErr)) {
        updateValidationStatus({QString("Save failed: %1").arg(saveErr)});
        return;
    }

    m_savedConfig = cfg;
    accept();
}

void RuntimeSetupDialog::updateValidationStatus(const QStringList& errors) {
    if (errors.isEmpty()) {
        m_statusArea->setHtml("<p style='color:#27ae60;'><b>Configuration is valid.</b></p>");
        return;
    }
    QString html = "<p style='color:#c0392b;'><b>Validation failed:</b></p><ul>";
    for (const auto& e : errors)
        html += QString("<li style='color:#c0392b;'>%1</li>").arg(e.toHtmlEscaped());
    html += "</ul>";
    m_statusArea->setHtml(html);
}

} // namespace gf::gui
