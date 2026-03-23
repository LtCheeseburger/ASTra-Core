#include "ModMetadataEditorDialog.hpp"
#include "ModAssetValidator.hpp"

#include <gf/core/log.hpp>
#include <gf/core/safe_write.hpp>

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <filesystem>

namespace gf::gui {

static constexpr qint64 kMaxManifestBytes = 64LL * 1024LL;

ModMetadataEditorDialog::ModMetadataEditorDialog(const QString& packageDir,
                                                  QWidget*       parent)
    : QDialog(parent)
    , m_packageDir(QDir::cleanPath(QDir(packageDir).absolutePath()))
{
    setWindowTitle("Edit Mod Metadata");
    setMinimumWidth(520);
    setAttribute(Qt::WA_DeleteOnClose);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(8);

    auto* hdr = new QLabel(
        QString("Editing package: <b>%1</b>").arg(
            QDir::toNativeSeparators(m_packageDir).toHtmlEscaped()),
        this);
    hdr->setWordWrap(true);
    outer->addWidget(hdr);

    // ── Metadata form ─────────────────────────────────────────────────────────
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 4, 0, 4);
    form->setSpacing(6);

    m_modIdEdit = new QLineEdit(this);
    m_modIdEdit->setMaxLength(64);
    form->addRow("Mod ID *:", m_modIdEdit);

    m_nameEdit = new QLineEdit(this);
    form->addRow("Name *:", m_nameEdit);

    m_versionEdit = new QLineEdit(this);
    form->addRow("Version *:", m_versionEdit);

    m_authorEdit = new QLineEdit(this);
    form->addRow("Author:", m_authorEdit);

    m_descEdit = new QLineEdit(this);
    form->addRow("Description:", m_descEdit);

    m_categoryEdit = new QLineEdit(this);
    form->addRow("Category:", m_categoryEdit);

    m_tagsEdit = new QLineEdit(this);
    m_tagsEdit->setPlaceholderText("Comma-separated, e.g. retro, uniforms");
    form->addRow("Tags:", m_tagsEdit);

    m_notesEdit = new QLineEdit(this);
    form->addRow("Notes:", m_notesEdit);

    outer->addLayout(form);

    // ── Icon ──────────────────────────────────────────────────────────────────
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

    // ── Preview images ────────────────────────────────────────────────────────
    auto* prevGroup = new QGroupBox("Preview Images (optional)", this);
    auto* prevLayout = new QVBoxLayout(prevGroup);
    prevLayout->setSpacing(6);

    m_previewList = new QListWidget(prevGroup);
    m_previewList->setFixedHeight(80);
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

    // ── Status label ─────────────────────────────────────────────────────────
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    outer->addWidget(m_statusLabel);

    // ── Buttons ───────────────────────────────────────────────────────────────
    auto* btns = new QHBoxLayout();
    btns->addStretch(1);
    m_btnSave = new QPushButton("Save Changes", this);
    m_btnSave->setDefault(true);
    auto* btnClose = new QPushButton("Cancel", this);
    btns->addWidget(m_btnSave);
    btns->addWidget(btnClose);
    outer->addLayout(btns);

    connect(btnBrowseIcon,   &QPushButton::clicked, this, &ModMetadataEditorDialog::onBrowseIcon);
    connect(btnClearIcon,    &QPushButton::clicked, this, &ModMetadataEditorDialog::onClearIcon);
    connect(m_btnAddPreview, &QPushButton::clicked, this, &ModMetadataEditorDialog::onAddPreview);
    connect(m_btnRemPreview, &QPushButton::clicked, this, &ModMetadataEditorDialog::onRemovePreview);
    connect(m_btnSave,       &QPushButton::clicked, this, &ModMetadataEditorDialog::onSave);
    connect(btnClose,        &QPushButton::clicked, this, &QDialog::reject);

    connect(m_previewList, &QListWidget::currentRowChanged, this, [this](int row) {
        m_btnRemPreview->setEnabled(row >= 0);
    });

    if (!loadManifest()) {
        m_btnSave->setEnabled(false);
    }
}

// ── private ───────────────────────────────────────────────────────────────────

bool ModMetadataEditorDialog::loadManifest() {
    const QString manifestPath =
        QDir(m_packageDir).filePath("astra_mod.json");
    QFile f(manifestPath);
    if (!f.open(QIODevice::ReadOnly)) {
        setStatus(QString("Cannot open astra_mod.json: %1").arg(f.errorString()), true);
        return false;
    }
    const QByteArray raw = f.read(kMaxManifestBytes + 1);
    f.close();

    if (raw.size() > kMaxManifestBytes) {
        setStatus("astra_mod.json exceeds 64 KiB — refusing to edit.", true);
        return false;
    }

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
    if (doc.isNull()) {
        setStatus(QString("JSON parse error: %1").arg(pe.errorString()), true);
        return false;
    }

    const QJsonObject root = doc.object();

    m_modIdEdit->setText(root.value("mod_id").toString());
    m_nameEdit->setText(root.value("name").toString());
    m_versionEdit->setText(root.value("version").toString());
    m_authorEdit->setText(root.value("author").toString());
    m_descEdit->setText(root.value("description").toString());
    m_categoryEdit->setText(root.value("category").toString());
    m_notesEdit->setText(root.value("notes").toString());

    QStringList tagsList;
    for (const auto& v : root.value("tags").toArray())
        tagsList << v.toString();
    m_tagsEdit->setText(tagsList.join(", "));

    // Icon: resolve relative path to absolute using packageDir
    const QString iconRel = root.value("icon").toString().trimmed();
    if (!iconRel.isEmpty()) {
        const QString iconAbs = QDir(m_packageDir).filePath(iconRel);
        if (QFileInfo::exists(iconAbs))
            updateIconPreview(iconAbs);
        else
            setStatus(QString("Icon file not found: %1").arg(iconRel), false);
    }

    // Previews
    for (const auto& v : root.value("previews").toArray()) {
        const QString relPath = v.toString().trimmed();
        if (relPath.isEmpty()) continue;
        const QString absPath = QDir(m_packageDir).filePath(relPath);
        auto* item = new QListWidgetItem(m_previewList);
        item->setData(Qt::UserRole, absPath);
    }
    rebuildPreviewLabels();

    return true;
}

bool ModMetadataEditorDialog::saveManifest() {
    // ── Validate required fields ──────────────────────────────────────────────
    const QString modId   = m_modIdEdit->text().trimmed();
    const QString name    = m_nameEdit->text().trimmed();
    const QString version = m_versionEdit->text().trimmed();

    if (modId.isEmpty()   || name.isEmpty() || version.isEmpty()) {
        setStatus("Mod ID, Name, and Version are required.", true);
        return false;
    }

    // ── Copy icon into package if it's an outside source ─────────────────────
    QString iconRel;
    const QString iconSrc = m_iconPathEdit->text().trimmed();
    if (!iconSrc.isEmpty()) {
        const QString iconDest = QDir(m_packageDir).filePath("icon.png");
        if (QDir::cleanPath(iconSrc) != QDir::cleanPath(iconDest)) {
            if (!QFile::exists(iconDest) || QFile::remove(iconDest)) {
                if (!QFile::copy(iconSrc, iconDest)) {
                    setStatus(QString("Failed to copy icon into package: %1")
                                  .arg(QFileInfo(iconSrc).fileName()), true);
                    return false;
                }
            }
        }
        iconRel = "icon.png";
    }

    // ── Copy preview images into package ─────────────────────────────────────
    QStringList previewRels;
    for (int i = 0; i < m_previewList->count(); ++i) {
        const QString srcAbs = m_previewList->item(i)->data(Qt::UserRole).toString();
        const QString ext    = QFileInfo(srcAbs).suffix().toLower();
        const QString relName = QString("preview_%1.%2").arg(i + 1).arg(ext);
        const QString destAbs = QDir(m_packageDir).filePath(relName);
        if (QDir::cleanPath(srcAbs) != QDir::cleanPath(destAbs)) {
            if (QFileInfo::exists(destAbs) && !QFile::remove(destAbs)) {
                setStatus(QString("Cannot overwrite existing preview: %1").arg(relName), true);
                return false;
            }
            if (!QFile::copy(srcAbs, destAbs)) {
                setStatus(QString("Failed to copy preview %1 ('%2') into package.")
                              .arg(i + 1).arg(QFileInfo(srcAbs).fileName()), true);
                return false;
            }
        }
        previewRels << relName;
    }

    // ── Re-read existing manifest to preserve unedited fields ────────────────
    const QString manifestPath = QDir(m_packageDir).filePath("astra_mod.json");
    QFile rf(manifestPath);
    QJsonObject root;
    if (rf.open(QIODevice::ReadOnly)) {
        QJsonParseError pe;
        const auto doc = QJsonDocument::fromJson(rf.read(kMaxManifestBytes), &pe);
        rf.close();
        if (!doc.isNull()) root = doc.object();
    }

    // ── Write updated fields ──────────────────────────────────────────────────
    root["mod_id"]  = modId;
    root["name"]    = name;
    root["version"] = version;

    auto setOrRemove = [&](const char* key, const QString& val) {
        if (val.isEmpty()) root.remove(key);
        else               root[key] = val;
    };
    setOrRemove("author",      m_authorEdit->text().trimmed());
    setOrRemove("description", m_descEdit->text().trimmed());
    setOrRemove("category",    m_categoryEdit->text().trimmed());
    setOrRemove("notes",       m_notesEdit->text().trimmed());

    // Tags: split comma-separated string
    const QString tagsRaw = m_tagsEdit->text().trimmed();
    if (tagsRaw.isEmpty()) {
        root.remove("tags");
    } else {
        QJsonArray tagsArr;
        for (const QString& t : tagsRaw.split(',', Qt::SkipEmptyParts))
            if (!t.trimmed().isEmpty()) tagsArr.append(t.trimmed());
        root["tags"] = tagsArr;
    }

    // Icon / previews
    if (iconRel.isEmpty()) root.remove("icon");
    else                   root["icon"] = iconRel;

    if (previewRels.isEmpty()) {
        root.remove("previews");
    } else {
        QJsonArray arr;
        for (const QString& p : previewRels) arr.append(p);
        root["previews"] = arr;
    }

    // ── Write manifest ────────────────────────────────────────────────────────
    const QString json =
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));

    gf::core::SafeWriteOptions opt;
    opt.make_backup = false;
    opt.max_bytes   = 64ull * 1024ull;

    const auto res = gf::core::safe_write_text(
        std::filesystem::path(manifestPath.toStdString()),
        json.toStdString(),
        opt);

    if (!res.ok) {
        setStatus(QString("Failed to write astra_mod.json: %1")
                      .arg(QString::fromStdString(res.message)), true);
        return false;
    }

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModMetadataEditorDialog: manifest saved",
                      (modId + " \u2014 " + m_packageDir).toStdString());
    return true;
}

void ModMetadataEditorDialog::updateIconPreview(const QString& absPath) {
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

void ModMetadataEditorDialog::setStatus(const QString& msg, bool isError) {
    m_statusLabel->setText(msg);
    m_statusLabel->setStyleSheet(isError
        ? "color: #c0392b;"
        : "color: #7f8c8d;");
}

void ModMetadataEditorDialog::rebuildPreviewLabels() {
    for (int i = 0; i < m_previewList->count(); ++i) {
        auto* item = m_previewList->item(i);
        const QString src = item->data(Qt::UserRole).toString();
        item->setText(QString("preview_%1 \u2014 %2")
                          .arg(i + 1)
                          .arg(QFileInfo(src).fileName()));
    }
}

void ModMetadataEditorDialog::onSave() {
    setStatus(QString(), false);
    if (saveManifest()) {
        setStatus("Changes saved successfully.", false);
        accept();
    }
}

void ModMetadataEditorDialog::onBrowseIcon() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Select Icon Image",
        QString(),
        "Images (*.png *.jpg *.jpeg *.webp);;All Files (*)");
    if (path.isEmpty()) return;

    QString err;
    if (!ModAssetValidator::validateImageFile(path, &err)) {
        setStatus(err, true);
        return;
    }
    updateIconPreview(path);
    setStatus(QString(), false);
}

void ModMetadataEditorDialog::onClearIcon() {
    updateIconPreview(QString());
}

void ModMetadataEditorDialog::onAddPreview() {
    const QStringList paths = QFileDialog::getOpenFileNames(
        this,
        "Select Preview Image(s)",
        QString(),
        "Images (*.png *.jpg *.jpeg *.webp);;All Files (*)");

    for (const QString& path : paths) {
        QString err;
        if (!ModAssetValidator::validateImageFile(path, &err)) {
            setStatus(err, true);
            continue;
        }
        auto* item = new QListWidgetItem(m_previewList);
        item->setData(Qt::UserRole, path);
    }
    rebuildPreviewLabels();
}

void ModMetadataEditorDialog::onRemovePreview() {
    const int row = m_previewList->currentRow();
    if (row < 0) return;
    delete m_previewList->takeItem(row);
    rebuildPreviewLabels();
    m_btnRemPreview->setEnabled(m_previewList->currentRow() >= 0);
}

} // namespace gf::gui
