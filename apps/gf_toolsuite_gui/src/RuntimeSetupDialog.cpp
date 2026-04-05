#include "RuntimeSetupDialog.hpp"
#include "RuntimeTargetManager.hpp"

#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
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
    setMinimumWidth(580);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(8);

    // Header
    auto* hdr = new QLabel(
        QString("<b>Game:</b> %1").arg(gameDisplayName.toHtmlEscaped()), this);
    outer->addWidget(hdr);

    auto* sub = new QLabel(
        "Configure the emulator and content root directories ASTra will use for this game.\n"
        "ASTra will automatically discover all AST and encrypted EDAT content "
        "under the update root, including DLC subfolders.",
        this);
    sub->setWordWrap(true);
    sub->setStyleSheet("color: palette(mid);");
    outer->addWidget(sub);

    // ── Core settings form ─────────────────────────────────────────────────────
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 4, 0, 4);
    form->setSpacing(6);

    // Platform (informational)
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

    // Base Content Root row
    auto* baseRow = new QHBoxLayout();
    baseRow->setSpacing(6);
    m_baseRootEdit = new QLineEdit(this);
    m_baseRootEdit->setPlaceholderText(
        "Base game content root (directory containing *.AST files)\u2026");
    auto* btnBrowseBase = new QPushButton("Browse\u2026", this);
    btnBrowseBase->setFixedWidth(80);
    baseRow->addWidget(m_baseRootEdit, 1);
    baseRow->addWidget(btnBrowseBase);
    form->addRow("Base Content Root:", baseRow);

    // Update Content Root row (optional)
    auto* updateRow = new QHBoxLayout();
    updateRow->setSpacing(6);
    m_updateRootEdit = new QLineEdit(this);
    m_updateRootEdit->setPlaceholderText(
        "Update content root (optional — ASTra scans recursively)\u2026");
    auto* btnBrowseUpdate = new QPushButton("Browse\u2026", this);
    btnBrowseUpdate->setFixedWidth(80);
    updateRow->addWidget(m_updateRootEdit, 1);
    updateRow->addWidget(btnBrowseUpdate);
    form->addRow("Update Content Root:", updateRow);

    outer->addLayout(form);

    // ── DLC note ──────────────────────────────────────────────────────────────
    auto* dlcNote = new QLabel(
        "DLC and patch content (e.g. DLC/CFBR_DLC_V19/, qkl_patch.ast) "
        "is automatically discovered under the update root. "
        "No explicit DLC folder configuration is needed.",
        this);
    dlcNote->setWordWrap(true);
    dlcNote->setStyleSheet("color: palette(mid); font-size: small;");
    outer->addWidget(dlcNote);

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
            m_baseRootEdit->setText(existing->astDirPath);

            // Restore update root if present
            for (const RuntimeContentRoot& cr : existing->contentRoots) {
                if (cr.kind == RuntimeContentKind::Update) {
                    m_updateRootEdit->setText(cr.path);
                    break;
                }
            }
            // Note: old DLC roots from the config are not shown (Phase 6A removes DLC UI),
            // but they are preserved in the JSON — the new save will drop them.
        }
    }

    // ── Connections ───────────────────────────────────────────────────────────
    connect(btnBrowseRpcs3,  &QPushButton::clicked, this, &RuntimeSetupDialog::onBrowseRpcs3);
    connect(btnBrowseBase,   &QPushButton::clicked, this, &RuntimeSetupDialog::onBrowseBaseRoot);
    connect(btnBrowseUpdate, &QPushButton::clicked, this, &RuntimeSetupDialog::onBrowseUpdateRoot);
    connect(m_btnSave,       &QPushButton::clicked, this, &RuntimeSetupDialog::onValidateAndSave);
    connect(btnCancel,       &QPushButton::clicked, this, &QDialog::reject);
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

void RuntimeSetupDialog::onBrowseBaseRoot() {
    const QString path = QFileDialog::getExistingDirectory(
        this,
        "Select Base Content Root Directory",
        m_baseRootEdit->text().isEmpty()
            ? QDir::homePath()
            : m_baseRootEdit->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!path.isEmpty())
        m_baseRootEdit->setText(QDir::toNativeSeparators(path));
}

void RuntimeSetupDialog::onBrowseUpdateRoot() {
    const QString path = QFileDialog::getExistingDirectory(
        this,
        "Select Update Content Root Directory",
        m_updateRootEdit->text().isEmpty()
            ? QDir::homePath()
            : m_updateRootEdit->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!path.isEmpty())
        m_updateRootEdit->setText(QDir::toNativeSeparators(path));
}

void RuntimeSetupDialog::onValidateAndSave() {
    RuntimeTargetConfig cfg;
    cfg.gameId       = m_gameId;
    cfg.platform     = RuntimePlatform::RPCS3;
    cfg.rpcs3ExePath = m_rpcs3Edit->text().trimmed();
    cfg.astDirPath   = m_baseRootEdit->text().trimmed();
    cfg.configuredAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    // Phase 6A: build content roots from update root only.
    // DLC roots are no longer configured via the UI.
    const QString updatePath = m_updateRootEdit->text().trimmed();
    if (!updatePath.isEmpty()) {
        RuntimeContentRoot cr;
        cr.kind        = RuntimeContentKind::Update;
        cr.path        = updatePath;
        cr.displayName = "Update";
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
