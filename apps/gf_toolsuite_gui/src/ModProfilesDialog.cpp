#include "ModProfilesDialog.hpp"
#include "ModProfileManager.hpp"
#include "NewProfileDialog.hpp"
#include "ProfileCloneService.hpp"
#include "ProfileWorkspaceBuilder.hpp"
#include "ShellHelper.hpp"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSplitter>
#include <QStyle>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace gf::gui {

// ── Phase 6F: profile icon file helpers ──────────────────────────────────────

// Deterministic icon path within a profile workspace.
static QString profileIconPath(const QString& workspacePath) {
    return QDir(workspacePath).filePath("profile_icon.png");
}

// Returns a null QPixmap if no icon is stored.
static QPixmap loadProfileIcon(const QString& workspacePath) {
    const QString path = profileIconPath(workspacePath);
    if (!QFile::exists(path)) return {};
    QPixmap pm;
    pm.load(path);
    return pm;
}

// Loads srcPath as a QImage, converts to PNG, and writes to the workspace.
// Returns false (with *err set) on load or save failure.
static bool setProfileIcon(const QString& workspacePath,
                            const QString& srcPath,
                            QString*       err)
{
    QImage img(srcPath);
    if (img.isNull() || img.width() < 1 || img.height() < 1) {
        if (err) *err = QString("Could not load image: %1").arg(srcPath);
        return false;
    }
    if (!QDir().mkpath(workspacePath)) {
        if (err) *err = QString("Cannot access workspace: %1").arg(workspacePath);
        return false;
    }
    const QString dest = profileIconPath(workspacePath);
    QFile::remove(dest); // overwrite if exists
    if (!img.save(dest, "PNG")) {
        if (err) *err = "Could not save icon to workspace.";
        return false;
    }
    return true;
}

static void clearProfileIcon(const QString& workspacePath) {
    QFile::remove(profileIconPath(workspacePath));
}

// ── Phase 6F: ProfileListDelegate ────────────────────────────────────────────

// Renders each profile row with:
//   [40×40 icon or initial-letter placeholder] [Name (bold if active)] [badges]
class ProfileListDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return {200, 56};
    }

    void paint(QPainter*                   painter,
               const QStyleOptionViewItem& option,
               const QModelIndex&          index) const override
    {
        // 1. Draw the standard selection/hover background via the style.
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.icon = QIcon();
        opt.text.clear();
        const QWidget* w = opt.widget;
        QStyle* style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, w);

        painter->save();

        const QRect r = option.rect;
        constexpr int kIconSize    = 40;
        constexpr int kLeftMargin  = 8;
        constexpr int kIconTextGap = 8;

        const int iconX = r.left() + kLeftMargin;
        const int iconY = r.top() + (r.height() - kIconSize) / 2;
        const QRect iconRect(iconX, iconY, kIconSize, kIconSize);

        // 2. Draw icon or first-letter placeholder.
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        if (!icon.isNull()) {
            icon.paint(painter, iconRect, Qt::AlignCenter);
        } else {
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(140, 152, 178));
            painter->drawRoundedRect(iconRect, 6, 6);

            const QString name   = index.data(Qt::DisplayRole).toString();
            const QString letter = name.isEmpty() ? QStringLiteral("?") : QString(name.at(0).toUpper());
            QFont lf = option.font;
            lf.setBold(true);
            lf.setPixelSize(18);
            painter->setFont(lf);
            painter->setPen(QColor(255, 255, 255, 210));
            painter->drawText(iconRect, Qt::AlignCenter, letter);
        }

        // 3. Profile name.
        const int textX = iconX + kIconSize + kIconTextGap;
        const int textW = r.right() - textX - kLeftMargin;

        const QString name     = index.data(Qt::DisplayRole).toString();
        const bool    isActive = index.data(Qt::UserRole + 1).toBool();

        QFont nameFont = option.font;
        if (isActive) nameFont.setBold(true);
        painter->setFont(nameFont);

        const QPalette::ColorRole textRole =
            (option.state & QStyle::State_Selected)
            ? QPalette::HighlightedText : QPalette::Text;
        painter->setPen(option.palette.color(textRole));

        const QFontMetrics nameFm(nameFont);
        const QRect nameRect(textX, r.top() + 7, textW, nameFm.height() + 2);
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          nameFm.elidedText(name, Qt::ElideRight, textW));

        // 4. Badges row.
        const bool isBaseline = index.data(Qt::UserRole + 2).toBool();
        const int  blType     = index.data(Qt::UserRole + 3).toInt();

        QFont badgeFont = option.font;
        badgeFont.setPixelSize(10);
        painter->setFont(badgeFont);
        const QFontMetrics badgeFm(badgeFont);

        int badgeX       = textX;
        const int badgeY = nameRect.bottom() + 3;

        auto drawBadge = [&](const QString& text, const QColor& bg) {
            const int bw = badgeFm.horizontalAdvance(text) + 8;
            constexpr int bh = 14;
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(bg);
            painter->drawRoundedRect(badgeX, badgeY, bw, bh, 3, 3);
            painter->setPen(Qt::white);
            painter->drawText(QRect(badgeX, badgeY, bw, bh), Qt::AlignCenter, text);
            badgeX += bw + 3;
        };

        if (isActive)
            drawBadge("Active", QColor(0x3a, 0xaa, 0x88));
        if (isBaseline)
            drawBadge(baselineTypeLabel(static_cast<BaselineType>(blType)),
                      QColor(0x75, 0x80, 0xa0));

        painter->restore();
    }
};

// ── ModProfilesDialog ─────────────────────────────────────────────────────────

ModProfilesDialog::ModProfilesDialog(ModProfileManager* mgr,
                                     const QString&     gameId,
                                     const QString&     gameDisplayName,
                                     const QString&     sourceContentPath,
                                     QWidget*           parent)
    : QDialog(parent)
    , m_mgr(mgr)
    , m_gameId(gameId)
    , m_gameDisplayName(gameDisplayName)
    , m_sourceContentPath(sourceContentPath)
{
    setWindowTitle(QString("Mod Profiles \u2014 %1").arg(gameDisplayName));
    setMinimumSize(620, 460);
    resize(780, 540);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 8);
    mainLayout->setSpacing(8);

    // ── Splitter: list left, details right ──────────────────────────────────
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setHandleWidth(4);

    // ── Left: profile list ───────────────────────────────────────────────────
    auto* listGroup  = new QGroupBox("Profiles", splitter);
    auto* listLayout = new QVBoxLayout(listGroup);
    listLayout->setContentsMargins(6, 8, 6, 6);

    m_list = new QListWidget(listGroup);
    m_list->setAlternatingRowColors(false);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    // Phase 6F: use richer delegate
    m_list->setItemDelegate(new ProfileListDelegate(m_list));
    m_list->setIconSize({40, 40});
    listLayout->addWidget(m_list);
    splitter->addWidget(listGroup);

    // ── Right: details pane ──────────────────────────────────────────────────
    auto* detailGroup  = new QGroupBox("Details", splitter);
    auto* detailLayout = new QVBoxLayout(detailGroup);
    detailLayout->setContentsMargins(6, 8, 6, 6);
    detailLayout->setSpacing(6);

    // Phase 6F: icon preview + text browser side by side
    auto* iconAndTextRow = new QHBoxLayout();
    iconAndTextRow->setSpacing(8);

    m_iconLabel = new QLabel(detailGroup);
    m_iconLabel->setFixedSize(64, 64);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    m_iconLabel->setStyleSheet(
        "border: 1px solid #c0c0c0;"
        "border-radius: 6px;"
        "background: #f0f1f4;"
        "color: #aaa;"
        "font-size: 22px;");
    m_iconLabel->setText("\u2606"); // hollow star placeholder
    iconAndTextRow->addWidget(m_iconLabel, 0, Qt::AlignTop);

    m_detailsPane = new QTextBrowser(detailGroup);
    m_detailsPane->setOpenLinks(false);
    m_detailsPane->setReadOnly(true);
    m_detailsPane->setHtml("<p style='color:gray;'>(No profile selected)</p>");
    iconAndTextRow->addWidget(m_detailsPane, 1);

    detailLayout->addLayout(iconAndTextRow, 1);

    // Phase 5D: Open Location / Copy Path; Phase 6F: Set Icon / Clear Icon
    auto* detailBtnRow = new QHBoxLayout();
    detailBtnRow->setSpacing(6);

    m_btnOpenLocation = new QPushButton("Open Location", detailGroup);
    m_btnOpenLocation->setToolTip("Open the workspace folder in the system file manager");
    m_btnOpenLocation->setEnabled(false);

    m_btnCopyPath = new QPushButton("Copy Path", detailGroup);
    m_btnCopyPath->setToolTip("Copy the workspace folder path to the clipboard");
    m_btnCopyPath->setEnabled(false);

    m_btnSetIcon = new QPushButton("Set Icon\u2026", detailGroup);
    m_btnSetIcon->setToolTip("Choose a custom icon image for this profile");
    m_btnSetIcon->setEnabled(false);

    m_btnClearIcon = new QPushButton("Clear Icon", detailGroup);
    m_btnClearIcon->setToolTip("Remove the custom icon from this profile");
    m_btnClearIcon->setEnabled(false);

    // Phase 7: build/rebuild the game_copy/ for the selected profile.
    m_btnBuildCopy = new QPushButton("Build Copy\u2026", detailGroup);
    m_btnBuildCopy->setToolTip(
        "Copy game files into this profile\u2019s workspace so you edit the profile "
        "copy instead of the original game directory");
    m_btnBuildCopy->setEnabled(false);

    detailBtnRow->addWidget(m_btnOpenLocation);
    detailBtnRow->addWidget(m_btnCopyPath);
    detailBtnRow->addWidget(m_btnBuildCopy);
    detailBtnRow->addStretch(1);
    detailBtnRow->addWidget(m_btnSetIcon);
    detailBtnRow->addWidget(m_btnClearIcon);
    detailLayout->addLayout(detailBtnRow);

    splitter->addWidget(detailGroup);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    mainLayout->addWidget(splitter, 1);

    // ── Action row 1: CRUD + activation ─────────────────────────────────────
    auto* actRow = new QHBoxLayout;
    actRow->setSpacing(6);

    m_btnCreate    = new QPushButton("New\u2026",    this);
    m_btnClone     = new QPushButton("Clone\u2026",  this);
    m_btnClone->setToolTip("Duplicate the selected profile and its workspace into a new profile");
    m_btnClone->setEnabled(false);
    m_btnRename    = new QPushButton("Rename\u2026", this);
    m_btnDelete    = new QPushButton("Delete\u2026", this);

    actRow->addWidget(m_btnCreate);
    actRow->addWidget(m_btnClone);
    actRow->addWidget(m_btnRename);
    actRow->addWidget(m_btnDelete);
    actRow->addStretch();

    m_btnSetActive = new QPushButton("Set Active", this);
    m_btnSetActive->setToolTip("Mark this profile as the active profile for the current game");

    m_btnSetActiveClose = new QPushButton("Activate & Close", this);
    m_btnSetActiveClose->setToolTip("Set this profile active and close the dialog");
    m_btnSetActiveClose->setEnabled(false);

    m_btnDeactivate = new QPushButton("Deactivate", this);
    m_btnDeactivate->setToolTip("Clear the active profile selection for the current game");

    actRow->addWidget(m_btnSetActive);
    actRow->addWidget(m_btnSetActiveClose);
    actRow->addWidget(m_btnDeactivate);

    mainLayout->addLayout(actRow);

    // ── Close button ─────────────────────────────────────────────────────────
    auto* closeBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(closeBox, &QDialogButtonBox::rejected, this, &QDialog::accept);
    mainLayout->addWidget(closeBox);

    // ── Connections ──────────────────────────────────────────────────────────
    connect(m_list, &QListWidget::currentRowChanged,
            this, &ModProfilesDialog::onSelectionChanged);
    connect(m_list, &QListWidget::customContextMenuRequested,
            this, &ModProfilesDialog::onContextMenu);
    // Phase 6F: double-click activates
    connect(m_list, &QListWidget::itemDoubleClicked,
            this, &ModProfilesDialog::onItemDoubleClicked);

    connect(m_btnCreate,         &QPushButton::clicked, this, &ModProfilesDialog::onCreate);
    connect(m_btnClone,          &QPushButton::clicked, this, &ModProfilesDialog::onClone);
    connect(m_btnRename,         &QPushButton::clicked, this, &ModProfilesDialog::onRename);
    connect(m_btnDelete,         &QPushButton::clicked, this, &ModProfilesDialog::onDelete);
    connect(m_btnSetActive,      &QPushButton::clicked, this, &ModProfilesDialog::onSetActive);
    connect(m_btnSetActiveClose, &QPushButton::clicked, this, &ModProfilesDialog::onSetActiveAndClose);
    connect(m_btnDeactivate,     &QPushButton::clicked, this, &ModProfilesDialog::onDeactivate);
    connect(m_btnOpenLocation,   &QPushButton::clicked, this, &ModProfilesDialog::onOpenLocation);
    connect(m_btnCopyPath,       &QPushButton::clicked, this, &ModProfilesDialog::onCopyPath);
    connect(m_btnSetIcon,        &QPushButton::clicked, this, &ModProfilesDialog::onSetIcon);
    connect(m_btnClearIcon,      &QPushButton::clicked, this, &ModProfilesDialog::onClearIcon);
    connect(m_btnBuildCopy,      &QPushButton::clicked, this, &ModProfilesDialog::onBuildCopy);

    // Refresh when the manager's data changes
    connect(m_mgr, &ModProfileManager::profilesChanged,
            this, [this](const QString& gid) {
                if (gid == m_gameId) rebuildList();
            });
    connect(m_mgr, &ModProfileManager::activeProfileChanged,
            this, [this](const QString& gid, const QString&) {
                if (gid == m_gameId) rebuildList();
            });

    rebuildList();
}

// ── Private helpers ───────────────────────────────────────────────────────────

void ModProfilesDialog::rebuildList() {
    // Preserve current selection by id
    const QString prevId = [this]() -> QString {
        const auto opt = selectedProfile();
        return opt ? opt->id : QString{};
    }();

    m_list->blockSignals(true);
    m_list->clear();

    const QVector<ModProfile> profiles = m_mgr->profilesForGame(m_gameId);
    const auto                activeId = m_mgr->activeProfileId(m_gameId);

    for (const auto& p : profiles) {
        const bool isActive = activeId && *activeId == p.id;

        // Plain name only — the delegate renders badges visually.
        auto* item = new QListWidgetItem(p.name, m_list);
        item->setData(Qt::UserRole,     p.id);
        item->setData(Qt::UserRole + 1, isActive);
        item->setData(Qt::UserRole + 2, p.isBaseline);
        item->setData(Qt::UserRole + 3, static_cast<int>(p.baselineType));

        // Phase 6F: load icon for this profile.
        const QPixmap pm = loadProfileIcon(p.workspacePath);
        if (!pm.isNull())
            item->setIcon(QIcon(pm));
    }

    m_list->blockSignals(false);

    // Restore selection
    bool restored = false;
    if (!prevId.isEmpty()) {
        for (int i = 0; i < m_list->count(); ++i) {
            if (m_list->item(i)->data(Qt::UserRole).toString() == prevId) {
                m_list->setCurrentRow(i);
                restored = true;
                break;
            }
        }
    }
    if (!restored && m_list->count() > 0)
        m_list->setCurrentRow(0);

    onSelectionChanged();
}

void ModProfilesDialog::onSelectionChanged() {
    const auto opt = selectedProfile();
    if (opt) {
        const auto activeId = m_mgr->activeProfileId(m_gameId);
        const bool isActive = activeId && *activeId == opt->id;

        const QString activeBadge =
            isActive
            ? QStringLiteral(" <span style='background:#3aaa88;color:white;"
                             "border-radius:3px;padding:1px 5px;"
                             "font-size:11px;'>Active</span>")
            : QString();

        const QString baselineBadge =
            opt->isBaseline
            ? QString(" <span style='background:#7580a0;color:white;"
                      "border-radius:3px;padding:1px 5px;"
                      "font-size:11px;'>%1</span>")
                  .arg(baselineTypeLabel(opt->baselineType).toHtmlEscaped())
            : QString();

        const QString nativePath =
            QDir::toNativeSeparators(opt->workspacePath).toHtmlEscaped();

        QString html;
        html += QString("<p style='margin:0 0 4px 0;'>"
                        "<b>%1</b>%2%3</p>")
                    .arg(opt->name.toHtmlEscaped(), activeBadge, baselineBadge);

        html += QString("<p style='color:#888;margin:0 0 6px 0;font-size:11px;'>"
                        "Created: %1 &nbsp;\u2502&nbsp; Updated: %2</p>")
                    .arg(opt->createdAt.toHtmlEscaped(),
                         opt->updatedAt.toHtmlEscaped());

        if (!opt->description.isEmpty()) {
            html += QString("<p style='margin:0 0 6px 0;'>%1</p>")
                        .arg(opt->description.toHtmlEscaped());
        }

        html += QStringLiteral("<hr style='margin:4px 0;'/>");

        html += QString("<p style='margin:0 0 2px 0;'><b>Workspace</b></p>"
                        "<p style='font-family:monospace;font-size:11px;"
                        "color:#555;word-break:break-all;margin:0;'>%1</p>")
                    .arg(nativePath);

        m_detailsPane->setHtml(html);
    } else {
        m_detailsPane->setHtml(
            "<p style='color:gray;'>(No profile selected)</p>");
    }
    updateDetailsIcon();
    updateButtons();
}

void ModProfilesDialog::updateDetailsIcon() {
    if (!m_iconLabel) return;
    const auto opt = selectedProfile();
    if (!opt) {
        m_iconLabel->setPixmap({});
        m_iconLabel->setText("\u2606");
        return;
    }
    const QPixmap pm = loadProfileIcon(opt->workspacePath);
    if (pm.isNull()) {
        m_iconLabel->setText("\u2606");
    } else {
        m_iconLabel->setPixmap(
            pm.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

void ModProfilesDialog::updateButtons() {
    const auto opt    = selectedProfile();
    const bool hasSel = opt.has_value();

    const bool isActive = hasSel && [&] {
        const auto aid = m_mgr->activeProfileId(m_gameId);
        return aid && *aid == opt->id;
    }();

    const bool hasIcon = hasSel &&
        QFile::exists(profileIconPath(opt->workspacePath));

    m_btnClone->setEnabled(hasSel);
    m_btnRename->setEnabled(hasSel);
    // Prevent deleting the active profile — user must deactivate first.
    m_btnDelete->setEnabled(hasSel && !isActive);
    m_btnSetActive->setEnabled(hasSel && !isActive);
    m_btnSetActiveClose->setEnabled(hasSel);
    m_btnDeactivate->setEnabled(isActive);
    m_btnOpenLocation->setEnabled(hasSel);
    m_btnCopyPath->setEnabled(hasSel);
    m_btnSetIcon->setEnabled(hasSel);
    m_btnClearIcon->setEnabled(hasIcon);
    // Phase 7: allow building game_copy/ whenever a profile is selected.
    m_btnBuildCopy->setEnabled(hasSel);
}

std::optional<ModProfile> ModProfilesDialog::selectedProfile() const {
    const QListWidgetItem* item = m_list->currentItem();
    if (!item) return std::nullopt;
    return m_mgr->findById(item->data(Qt::UserRole).toString());
}

// ── Slot implementations ──────────────────────────────────────────────────────

void ModProfilesDialog::onItemDoubleClicked(QListWidgetItem* item) {
    if (!item) return;
    m_list->setCurrentItem(item);

    const auto opt = selectedProfile();
    if (!opt) return;

    // No-op if already active.
    const auto activeId = m_mgr->activeProfileId(m_gameId);
    if (activeId && *activeId == opt->id) return;

    onSetActive();
}

void ModProfilesDialog::onCreate() {
    NewProfileDialog dlg(m_mgr, m_gameId, m_sourceContentPath, this);
    if (dlg.exec() != QDialog::Accepted) return;

    const NewProfileSpec spec = dlg.spec();

    QString newProfileId;

    if (spec.sourceProfileId.isEmpty()) {
        // Blank workspace — original behavior.
        ModProfile created;
        QString    err;
        if (!m_mgr->createProfile(m_gameId, spec.name, {}, &created, &err,
                                   m_gameDisplayName)) {
            QMessageBox::warning(this, "Create Profile", err);
            return;
        }
        newProfileId = created.id;

        // Phase 7: persist sourcePath on the newly created profile.
        if (!spec.sourceContentPath.isEmpty()) {
            created.sourcePath = spec.sourceContentPath;
            m_mgr->updateProfile(created, nullptr);
        }
    } else {
        // Copy from source (baseline or existing profile) via ProfileCloneService.
        const auto srcOpt = m_mgr->findById(spec.sourceProfileId);
        if (!srcOpt) {
            QMessageBox::warning(this, "Create Profile",
                                 "Source profile could not be found.");
            return;
        }
        const ProfileCloneResult result =
            ProfileCloneService::clone(*srcOpt, spec.name, m_gameDisplayName,
                                       *m_mgr, this);
        if (!result.warnings.isEmpty()) {
            QMessageBox::warning(this, "Create Profile \u2014 Warnings",
                                 result.warnings.join('\n'));
        }
        if (!result.success) {
            QMessageBox::critical(this, "Create Profile Failed", result.message);
            return;
        }
        newProfileId = result.newProfileId;

        // Phase 7: persist sourcePath on the clone (inherit from source or use spec).
        const QString inheritedSrc = !spec.sourceContentPath.isEmpty()
                                         ? spec.sourceContentPath
                                         : srcOpt->sourcePath;
        if (!inheritedSrc.isEmpty()) {
            if (auto cloned = m_mgr->findById(newProfileId)) {
                cloned->sourcePath = inheritedSrc;
                m_mgr->updateProfile(*cloned, nullptr);
            }
        }
    }

    // Phase 7: copy game files into game_copy/ if the user requested it.
    if (spec.copyContentNow && !newProfileId.isEmpty() && !spec.sourceContentPath.isEmpty()) {
        const auto profileOpt = m_mgr->findById(newProfileId);
        if (profileOpt) {
            const ProfileWorkspaceBuildResult copyResult =
                ProfileWorkspaceBuilder::buildGameCopy(*profileOpt,
                                                       spec.sourceContentPath, this);
            if (!copyResult.warnings.isEmpty()) {
                QMessageBox::warning(this, "Game Copy \u2014 Warnings",
                                     copyResult.warnings.join('\n'));
            }
            if (!copyResult.success) {
                QMessageBox::warning(this, "Game Copy",
                    QString("Could not build game copy: %1\n\n"
                            "You can try again via the \"Build Copy\u2026\" button.")
                        .arg(copyResult.message));
            }
        }
    }

    // Optionally activate the new profile immediately.
    if (spec.setActive && !newProfileId.isEmpty()) {
        QString err;
        m_mgr->setActiveProfile(m_gameId, newProfileId, &err);
        // Non-fatal if activation fails; the list will reflect the current state.
    }
    // rebuildList() triggered via profilesChanged signal from createProfile() / clone()
}

void ModProfilesDialog::onClone() {
    const auto opt = selectedProfile();
    if (!opt) return;

    bool ok = false;
    const QString newName = QInputDialog::getText(
        this,
        "Clone Profile",
        QString("Clone \u201c%1\u201d\n\nNew profile name:").arg(opt->name),
        QLineEdit::Normal,
        opt->name + " (copy)",
        &ok).trimmed();
    if (!ok || newName.isEmpty()) return;

    const ProfileCloneResult result =
        ProfileCloneService::clone(*opt, newName, m_gameDisplayName, *m_mgr, this);

    if (!result.warnings.isEmpty()) {
        QMessageBox::warning(this, "Clone Profile \u2014 Warnings",
                             result.warnings.join('\n'));
    }
    if (!result.success) {
        QMessageBox::critical(this, "Clone Failed", result.message);
    }
}

void ModProfilesDialog::onRename() {
    const auto opt = selectedProfile();
    if (!opt) return;

    bool ok = false;
    const QString newName = QInputDialog::getText(
        this, "Rename Profile", "New name:",
        QLineEdit::Normal, opt->name, &ok).trimmed();
    if (!ok || newName.isEmpty()) return;

    QString err;
    if (!m_mgr->renameProfile(opt->id, newName, &err)) {
        QMessageBox::warning(this, "Rename Profile", err);
    }
}

void ModProfilesDialog::onDelete() {
    const auto opt = selectedProfile();
    if (!opt) return;

    const auto confirm = QMessageBox::question(
        this, "Delete Profile",
        QString("Delete profile \"%1\"?\n\n"
                "The workspace files on disk will not be touched.\n"
                "This cannot be undone.").arg(opt->name),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);
    if (confirm != QMessageBox::Yes) return;

    const auto wsConfirm = QMessageBox::question(
        this, "Delete Workspace Files",
        QString("Also permanently delete the workspace directory?\n\n"
                "%1\n\n"
                "Warning: all files in the workspace will be lost.")
            .arg(QDir::toNativeSeparators(opt->workspacePath)),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    QString err;
    if (!m_mgr->deleteProfile(opt->id, wsConfirm == QMessageBox::Yes, &err)) {
        QMessageBox::warning(this, "Delete Profile", err);
    }
}

void ModProfilesDialog::onSetActive() {
    const auto opt = selectedProfile();
    if (!opt) return;

    QString err;
    if (!m_mgr->setActiveProfile(m_gameId, opt->id, &err)) {
        QMessageBox::warning(this, "Set Active Profile", err);
    }
}

void ModProfilesDialog::onSetActiveAndClose() {
    const auto opt = selectedProfile();
    if (!opt) return;

    // If already active, just close.
    const auto activeId = m_mgr->activeProfileId(m_gameId);
    if (activeId && *activeId == opt->id) {
        close();
        return;
    }

    QString err;
    if (!m_mgr->setActiveProfile(m_gameId, opt->id, &err)) {
        QMessageBox::warning(this, "Set Active Profile", err);
        return; // don't close on failure
    }
    close();
}

void ModProfilesDialog::onDeactivate() {
    const auto opt = selectedProfile();
    if (!opt) return;
    m_mgr->clearActiveProfile(m_gameId);
}

void ModProfilesDialog::onSetIcon() {
    const auto opt = selectedProfile();
    if (!opt) return;

    const QString srcPath = QFileDialog::getOpenFileName(
        this, "Select Profile Icon", {},
        "Images (*.png *.jpg *.jpeg)");
    if (srcPath.isEmpty()) return;

    QString err;
    if (!setProfileIcon(opt->workspacePath, srcPath, &err)) {
        QMessageBox::warning(this, "Set Profile Icon", err);
        return;
    }
    rebuildList(); // refreshes icon in list item and details pane
}

void ModProfilesDialog::onClearIcon() {
    const auto opt = selectedProfile();
    if (!opt) return;
    clearProfileIcon(opt->workspacePath);
    rebuildList();
}

void ModProfilesDialog::onOpenLocation() {
    const auto opt = selectedProfile();
    if (!opt) return;
    ShellHelper::openFolder(opt->workspacePath);
}

void ModProfilesDialog::onCopyPath() {
    const auto opt = selectedProfile();
    if (!opt) return;
    ShellHelper::copyToClipboard(QDir::toNativeSeparators(opt->workspacePath));
}

void ModProfilesDialog::onBuildCopy() {
    const auto opt = selectedProfile();
    if (!opt) return;

    // Determine the source path: prefer the profile's recorded sourcePath,
    // fall back to the dialog's sourceContentPath, then ask the user.
    QString srcPath = !opt->sourcePath.isEmpty()
                          ? opt->sourcePath
                          : m_sourceContentPath;

    if (srcPath.isEmpty()) {
        srcPath = QFileDialog::getExistingDirectory(
            this,
            QString("Select Game Content Directory for \u201c%1\u201d").arg(opt->name),
            {},
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (srcPath.isEmpty()) return;
    }

    const ProfileWorkspaceBuildResult result =
        ProfileWorkspaceBuilder::buildGameCopy(*opt, srcPath, this);

    if (!result.warnings.isEmpty()) {
        QMessageBox::warning(this, "Build Copy \u2014 Warnings",
                             result.warnings.join('\n'));
    }

    if (result.success) {
        // If the profile didn't have a recorded sourcePath, persist the one used.
        if (opt->sourcePath.isEmpty()) {
            ModProfile updated = *opt;
            updated.sourcePath = srcPath;
            m_mgr->updateProfile(updated, nullptr);
        }
        QMessageBox::information(
            this, "Build Copy",
            QString("Game copy ready: %1 file(s) in profile workspace.")
                .arg(result.filesCopied));
    } else {
        QMessageBox::critical(this, "Build Copy Failed", result.message);
    }
}

void ModProfilesDialog::onContextMenu(const QPoint& pos) {
    const QListWidgetItem* item = m_list->itemAt(pos);
    if (!item) return;

    m_list->setCurrentItem(const_cast<QListWidgetItem*>(item));

    const auto opt = m_mgr->findById(item->data(Qt::UserRole).toString());
    if (!opt) return;

    const auto activeId = m_mgr->activeProfileId(m_gameId);
    const bool isActive = activeId && *activeId == opt->id;
    const bool hasIcon  = QFile::exists(profileIconPath(opt->workspacePath));

    QMenu menu(this);

    QAction* actSetActive = menu.addAction("Set Active");
    actSetActive->setEnabled(!isActive);

    QAction* actSetActiveClose = menu.addAction("Activate & Close");
    actSetActiveClose->setEnabled(!isActive);

    QAction* actDeactivate = menu.addAction("Deactivate");
    actDeactivate->setEnabled(isActive);

    menu.addSeparator();

    QAction* actRename = menu.addAction("Rename\u2026");

    menu.addSeparator();

    QAction* actSetIcon   = menu.addAction("Set Icon\u2026");
    QAction* actClearIcon = menu.addAction("Clear Icon");
    actClearIcon->setEnabled(hasIcon);

    menu.addSeparator();

    QAction* actOpenLocation = menu.addAction("Open Profile Location");
    QAction* actCopyPath     = menu.addAction("Copy Workspace Path");

    menu.addSeparator();

    QAction* actDelete = menu.addAction("Delete\u2026");
    actDelete->setEnabled(!isActive);

    QAction* chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if      (chosen == actSetActive)      onSetActive();
    else if (chosen == actSetActiveClose) onSetActiveAndClose();
    else if (chosen == actDeactivate)     onDeactivate();
    else if (chosen == actRename)         onRename();
    else if (chosen == actSetIcon)        onSetIcon();
    else if (chosen == actClearIcon)      onClearIcon();
    else if (chosen == actDelete)         onDelete();
    else if (chosen == actOpenLocation)   ShellHelper::openFolder(opt->workspacePath);
    else if (chosen == actCopyPath)
        ShellHelper::copyToClipboard(QDir::toNativeSeparators(opt->workspacePath));
}

} // namespace gf::gui
