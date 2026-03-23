#include "ExportModDialog.hpp"
#include "ModAssetValidator.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace gf::gui {

ExportModDialog::ExportModDialog(const QString& gameId,
                                  const QString& profileName,
                                  QWidget*       parent)
    : QDialog(parent)
    , m_gameId(gameId)
{
    setWindowTitle("Export Mod Package");
    setMinimumWidth(520);
    setAttribute(Qt::WA_DeleteOnClose);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(8);

    // Header
    auto* hdr = new QLabel(
        QString("Export profile <b>%1</b> as a portable ASTra mod package.")
            .arg(profileName.toHtmlEscaped()),
        this);
    hdr->setWordWrap(true);
    outer->addWidget(hdr);

    auto* sub = new QLabel(
        "The exported folder can be re-imported with \u201cInstall Mod\u2026\u201d on any compatible setup.",
        this);
    sub->setWordWrap(true);
    sub->setStyleSheet("color: palette(mid);");
    outer->addWidget(sub);

    // ── Metadata form ─────────────────────────────────────────────────────────
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 4, 0, 4);
    form->setSpacing(6);

    m_modIdEdit = new QLineEdit(this);
    m_modIdEdit->setPlaceholderText("e.g. ncaa14_jersey_retro");
    m_modIdEdit->setMaxLength(64);
    form->addRow("Mod ID *:", m_modIdEdit);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText("Human-readable mod name");
    form->addRow("Name *:", m_nameEdit);

    m_versionEdit = new QLineEdit(this);
    m_versionEdit->setText("1.0.0");
    form->addRow("Version *:", m_versionEdit);

    m_authorEdit = new QLineEdit(this);
    m_authorEdit->setPlaceholderText("Optional");
    form->addRow("Author:", m_authorEdit);

    m_descEdit = new QLineEdit(this);
    m_descEdit->setPlaceholderText("Optional short description");
    form->addRow("Description:", m_descEdit);

    m_categoryEdit = new QLineEdit(this);
    m_categoryEdit->setPlaceholderText("Optional, e.g. Uniforms");
    form->addRow("Category:", m_categoryEdit);

    // Output directory
    auto* outRow = new QHBoxLayout();
    outRow->setSpacing(6);
    m_outputDirEdit = new QLineEdit(this);
    m_outputDirEdit->setPlaceholderText("Parent folder for the exported package\u2026");
    auto* btnBrowseOut = new QPushButton("Browse\u2026", this);
    btnBrowseOut->setFixedWidth(80);
    outRow->addWidget(m_outputDirEdit, 1);
    outRow->addWidget(btnBrowseOut);
    form->addRow("Output Folder *:", outRow);

    outer->addLayout(form);

    // Note about target game ID
    auto* note = new QLabel(
        QString("<i>Target game ID will be set to <tt>%1</tt> automatically.</i>")
            .arg(gameId.toHtmlEscaped()),
        this);
    note->setWordWrap(true);
    outer->addWidget(note);

    // ── Phase 5C: Icon ────────────────────────────────────────────────────────
    auto* iconGroup = new QGroupBox("Icon (optional)", this);
    auto* iconLayout = new QHBoxLayout(iconGroup);
    iconLayout->setSpacing(8);

    m_iconPreview = new QLabel(iconGroup);
    m_iconPreview->setFixedSize(64, 64);
    m_iconPreview->setAlignment(Qt::AlignCenter);
    m_iconPreview->setStyleSheet(
        "border: 1px solid palette(mid); background: palette(base);");
    m_iconPreview->setText("None");
    iconLayout->addWidget(m_iconPreview);

    auto* iconRightCol = new QVBoxLayout();
    m_iconPathEdit = new QLineEdit(iconGroup);
    m_iconPathEdit->setPlaceholderText("No icon selected\u2026");
    m_iconPathEdit->setReadOnly(true);
    iconRightCol->addWidget(m_iconPathEdit);

    auto* iconBtnRow = new QHBoxLayout();
    auto* btnBrowseIcon = new QPushButton("Browse\u2026", iconGroup);
    auto* btnClearIcon  = new QPushButton("Clear", iconGroup);
    btnClearIcon->setFixedWidth(60);
    iconBtnRow->addWidget(btnBrowseIcon);
    iconBtnRow->addWidget(btnClearIcon);
    iconBtnRow->addStretch(1);
    iconRightCol->addLayout(iconBtnRow);
    iconRightCol->addStretch(1);
    iconLayout->addLayout(iconRightCol, 1);
    outer->addWidget(iconGroup);

    // ── Phase 5C: Preview images ──────────────────────────────────────────────
    auto* prevGroup = new QGroupBox("Preview Images (optional)", this);
    auto* prevLayout = new QVBoxLayout(prevGroup);
    prevLayout->setSpacing(6);

    m_previewList = new QListWidget(prevGroup);
    m_previewList->setFixedHeight(72);
    m_previewList->setSelectionMode(QAbstractItemView::SingleSelection);
    prevLayout->addWidget(m_previewList);

    auto* prevBtnRow = new QHBoxLayout();
    m_btnAddPreview = new QPushButton("Add Image\u2026", prevGroup);
    m_btnRemPreview = new QPushButton("Remove", prevGroup);
    m_btnRemPreview->setEnabled(false);
    prevBtnRow->addWidget(m_btnAddPreview);
    prevBtnRow->addWidget(m_btnRemPreview);
    prevBtnRow->addStretch(1);
    prevLayout->addLayout(prevBtnRow);
    outer->addWidget(prevGroup);

    // ── Status area ───────────────────────────────────────────────────────────
    m_statusArea = new QTextBrowser(this);
    m_statusArea->setFixedHeight(60);
    m_statusArea->setReadOnly(true);
    m_statusArea->setOpenLinks(false);
    m_statusArea->setHtml(
        "<i style='color:gray;'>Fill in the required fields (*) and choose an output folder.</i>");
    outer->addWidget(m_statusArea);

    // ── Buttons ───────────────────────────────────────────────────────────────
    auto* btns = new QHBoxLayout();
    btns->addStretch(1);
    m_btnExport = new QPushButton("Export Package", this);
    m_btnExport->setDefault(true);
    auto* btnCancel = new QPushButton("Cancel", this);
    btns->addWidget(m_btnExport);
    btns->addWidget(btnCancel);
    outer->addLayout(btns);

    connect(btnBrowseOut,    &QPushButton::clicked, this, &ExportModDialog::onBrowseOutputDir);
    connect(btnBrowseIcon,   &QPushButton::clicked, this, &ExportModDialog::onBrowseIcon);
    connect(btnClearIcon,    &QPushButton::clicked, this, &ExportModDialog::onClearIcon);
    connect(m_btnAddPreview, &QPushButton::clicked, this, &ExportModDialog::onAddPreview);
    connect(m_btnRemPreview, &QPushButton::clicked, this, &ExportModDialog::onRemovePreview);
    connect(m_btnExport,     &QPushButton::clicked, this, &ExportModDialog::onExport);
    connect(btnCancel,       &QPushButton::clicked, this, &QDialog::reject);

    connect(m_previewList, &QListWidget::currentRowChanged, this, [this](int row) {
        m_btnRemPreview->setEnabled(row >= 0);
    });
}

ModExportSpec ExportModDialog::spec() const {
    ModExportSpec s;
    s.modId        = m_modIdEdit->text().trimmed();
    s.name         = m_nameEdit->text().trimmed();
    s.version      = m_versionEdit->text().trimmed();
    s.author       = m_authorEdit->text().trimmed();
    s.description  = m_descEdit->text().trimmed();
    s.category     = m_categoryEdit->text().trimmed();
    s.targetGameIds << m_gameId;
    s.platforms    << "PS3";

    s.iconSourcePath = m_iconPathEdit->text().trimmed();

    for (int i = 0; i < m_previewList->count(); ++i)
        s.previewSourcePaths << m_previewList->item(i)->data(Qt::UserRole).toString();

    return s;
}

QString ExportModDialog::outputDir() const {
    return m_outputDirEdit->text().trimmed();
}

void ExportModDialog::setIconPath(const QString& absPath) {
    m_iconPathEdit->setText(absPath);
    if (absPath.isEmpty()) {
        m_iconPreview->setPixmap(QPixmap());
        m_iconPreview->setText("None");
        return;
    }
    const QPixmap px(absPath);
    if (px.isNull()) {
        m_iconPreview->setPixmap(QPixmap());
        m_iconPreview->setText("?");
    } else {
        m_iconPreview->setPixmap(
            px.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_iconPreview->setText(QString());
    }
}

void ExportModDialog::onBrowseOutputDir() {
    const QString path = QFileDialog::getExistingDirectory(
        this,
        "Select Output Folder",
        m_outputDirEdit->text().isEmpty()
            ? QDir::homePath()
            : m_outputDirEdit->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!path.isEmpty())
        m_outputDirEdit->setText(QDir::toNativeSeparators(path));
}

void ExportModDialog::onBrowseIcon() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Select Icon Image",
        QString(),
        "Images (*.png *.jpg *.jpeg *.webp);;All Files (*)");
    if (path.isEmpty()) return;

    QString err;
    if (!ModAssetValidator::validateImageFile(path, &err)) {
        updateStatus({err});
        return;
    }
    setIconPath(path);
}

void ExportModDialog::onClearIcon() {
    setIconPath(QString());
}

void ExportModDialog::onAddPreview() {
    const QStringList paths = QFileDialog::getOpenFileNames(
        this,
        "Select Preview Image(s)",
        QString(),
        "Images (*.png *.jpg *.jpeg *.webp);;All Files (*)");

    QStringList errors;
    for (const QString& path : paths) {
        QString err;
        if (!ModAssetValidator::validateImageFile(path, &err)) {
            errors << err;
            continue;
        }
        const QString label = QString("preview_%1 \u2014 %2")
                                  .arg(m_previewList->count() + 1)
                                  .arg(QFileInfo(path).fileName());
        auto* item = new QListWidgetItem(label, m_previewList);
        item->setData(Qt::UserRole, path);
    }
    if (!errors.isEmpty())
        updateStatus(errors);
}

void ExportModDialog::onRemovePreview() {
    const int row = m_previewList->currentRow();
    if (row < 0) return;
    delete m_previewList->takeItem(row);
    // Renumber visible labels
    for (int i = 0; i < m_previewList->count(); ++i) {
        auto* item = m_previewList->item(i);
        const QString src = item->data(Qt::UserRole).toString();
        item->setText(QString("preview_%1 \u2014 %2")
                          .arg(i + 1)
                          .arg(QFileInfo(src).fileName()));
    }
    m_btnRemPreview->setEnabled(m_previewList->currentRow() >= 0);
}

void ExportModDialog::onExport() {
    QStringList errors;
    if (m_modIdEdit->text().trimmed().isEmpty())
        errors << "Mod ID is required.";
    if (m_nameEdit->text().trimmed().isEmpty())
        errors << "Mod name is required.";
    if (m_versionEdit->text().trimmed().isEmpty())
        errors << "Version is required.";
    if (m_outputDirEdit->text().trimmed().isEmpty())
        errors << "Output folder is required.";
    else if (!QDir(m_outputDirEdit->text().trimmed()).exists())
        errors << QString("Output folder does not exist: %1")
                      .arg(m_outputDirEdit->text().trimmed());

    if (!errors.isEmpty()) {
        updateStatus(errors);
        return;
    }

    accept();
}

void ExportModDialog::updateStatus(const QStringList& errors) {
    if (errors.isEmpty()) {
        m_statusArea->setHtml(
            "<p style='color:#27ae60;'><b>Ready to export.</b></p>");
        return;
    }
    QString html = "<p style='color:#c0392b;'><b>Please fix the following:</b></p><ul>";
    for (const auto& e : errors)
        html += QString("<li style='color:#c0392b;'>%1</li>").arg(e.toHtmlEscaped());
    html += "</ul>";
    m_statusArea->setHtml(html);
}

} // namespace gf::gui
