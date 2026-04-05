#include "NewProfileDialog.hpp"
#include "ModProfileManager.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace gf::gui {

NewProfileDialog::NewProfileDialog(ModProfileManager* mgr,
                                   const QString&     gameId,
                                   const QString&     sourceContentPath,
                                   QWidget*           parent)
    : QDialog(parent)
{
    setWindowTitle("New Mod Profile");
    setMinimumWidth(400);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 12);
    outer->setSpacing(10);

    // ── Name ────────────────────────────────────────────────────────────────
    auto* form = new QFormLayout();
    form->setSpacing(8);
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText("e.g. My Roster Mod");
    form->addRow("Name:", m_nameEdit);
    outer->addLayout(form);

    // ── Source ──────────────────────────────────────────────────────────────
    outer->addWidget(new QLabel("Start from:", this));

    m_sourceCombo = new QComboBox(this);
    outer->addWidget(m_sourceCombo);

    m_sourceHint = new QLabel(this);
    m_sourceHint->setWordWrap(true);
    m_sourceHint->setStyleSheet("color: #666; font-size: 11px;");
    outer->addWidget(m_sourceHint);

    // Populate combo — baselines first (recommended), then other profiles, then blank.
    const QVector<ModProfile> profiles = mgr->profilesForGame(gameId);

    int firstBaselineIndex = -1;

    for (const auto& p : profiles) {
        if (!p.isBaseline) continue;
        if (firstBaselineIndex < 0)
            firstBaselineIndex = m_sourceCombo->count();
        m_sourceCombo->addItem(
            QString("\u2605 %1  [%2]").arg(p.name, baselineTypeLabel(p.baselineType)));
        m_sourceIds << p.id;
    }

    for (const auto& p : profiles) {
        if (p.isBaseline) continue;
        m_sourceCombo->addItem(QString("Clone: %1").arg(p.name));
        m_sourceIds << p.id;
    }

    // Blank workspace — always available
    m_sourceCombo->addItem("\u25a1  Blank workspace");
    m_sourceIds << QString();

    // Default: first baseline if any, otherwise blank workspace (last item).
    const int defaultIndex =
        (firstBaselineIndex >= 0) ? firstBaselineIndex
                                  : (m_sourceCombo->count() - 1);
    m_sourceCombo->setCurrentIndex(defaultIndex);
    updateSourceHint(defaultIndex);

    // ── Game content path (Phase 7) ─────────────────────────────────────────
    auto* copySection = new QVBoxLayout();
    copySection->setSpacing(4);

    auto* copyLabel = new QLabel("Game content directory (for profile copy):", this);
    copySection->addWidget(copyLabel);

    auto* pathRow = new QHBoxLayout();
    pathRow->setSpacing(4);
    m_srcPathEdit = new QLineEdit(this);
    m_srcPathEdit->setPlaceholderText("Select the game\u2019s AST/content folder\u2026");
    m_srcPathEdit->setText(sourceContentPath);
    pathRow->addWidget(m_srcPathEdit, 1);

    auto* btnBrowse = new QPushButton("Browse\u2026", this);
    btnBrowse->setFixedWidth(80);
    pathRow->addWidget(btnBrowse);
    copySection->addLayout(pathRow);

    auto* pathHint = new QLabel(
        "When set, ASTra copies these files into the profile workspace so you\n"
        "always edit the profile copy \u2014 the original game is never modified.",
        this);
    pathHint->setWordWrap(true);
    pathHint->setStyleSheet("color: #666; font-size: 11px;");
    copySection->addWidget(pathHint);

    m_chkCopyNow = new QCheckBox("Copy game files into workspace now", this);
    m_chkCopyNow->setEnabled(!m_srcPathEdit->text().trimmed().isEmpty());
    m_chkCopyNow->setChecked(!sourceContentPath.isEmpty());
    copySection->addWidget(m_chkCopyNow);

    outer->addLayout(copySection);

    // ── Set active ──────────────────────────────────────────────────────────
    m_chkSetActive = new QCheckBox("Set as active profile after creation", this);
    outer->addWidget(m_chkSetActive);

    // ── Buttons ─────────────────────────────────────────────────────────────
    auto* btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_btnOk = btns->button(QDialogButtonBox::Ok);
    m_btnOk->setEnabled(false); // enabled only when name is non-empty
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(btns);

    // ── Connections ─────────────────────────────────────────────────────────
    connect(m_nameEdit, &QLineEdit::textChanged,
            this, &NewProfileDialog::onNameChanged);
    connect(m_sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) { updateSourceHint(idx); });
    connect(btnBrowse, &QPushButton::clicked,
            this, &NewProfileDialog::onBrowseContentPath);
    connect(m_srcPathEdit, &QLineEdit::textChanged,
            this, &NewProfileDialog::onContentPathChanged);

    m_nameEdit->setFocus();
}

NewProfileSpec NewProfileDialog::spec() const {
    const int idx = m_sourceCombo->currentIndex();
    const QString srcId =
        (idx >= 0 && idx < m_sourceIds.size()) ? m_sourceIds.at(idx) : QString{};

    NewProfileSpec s;
    s.name              = m_nameEdit->text().trimmed();
    s.sourceProfileId   = srcId;
    s.sourceContentPath = m_srcPathEdit->text().trimmed();
    s.copyContentNow    = m_chkCopyNow->isChecked() && !s.sourceContentPath.isEmpty();
    s.setActive         = m_chkSetActive->isChecked();
    return s;
}

void NewProfileDialog::onNameChanged() {
    m_btnOk->setEnabled(!m_nameEdit->text().trimmed().isEmpty());
}

void NewProfileDialog::onBrowseContentPath() {
    const QString start = m_srcPathEdit->text().trimmed();
    const QString dir = QFileDialog::getExistingDirectory(
        this,
        "Select Game Content Directory",
        start.isEmpty() ? QString{} : start,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty())
        m_srcPathEdit->setText(dir);
}

void NewProfileDialog::onContentPathChanged() {
    const bool hasPath = !m_srcPathEdit->text().trimmed().isEmpty();
    m_chkCopyNow->setEnabled(hasPath);
    if (!hasPath)
        m_chkCopyNow->setChecked(false);
}

void NewProfileDialog::updateSourceHint(int idx) {
    if (idx < 0 || idx >= m_sourceIds.size()) return;

    const QString& id = m_sourceIds.at(idx);
    if (id.isEmpty()) {
        m_sourceHint->setText(
            "Creates a new empty workspace. "
            "Mods can be installed after selecting a runtime and baseline.");
    } else {
        // Peek at first char of combo text to distinguish baseline vs clone.
        const QString itemText = m_sourceCombo->itemText(idx);
        if (itemText.startsWith(QChar(0x2605))) { // ★
            m_sourceHint->setText(
                "Copies the baseline\u2019s captured content into the new workspace. "
                "The new profile starts with all captured game files and is ready to use.");
        } else {
            m_sourceHint->setText(
                "Copies the selected profile\u2019s overlay content and installed mods "
                "into the new workspace. The source profile is not modified.");
        }
    }
}

} // namespace gf::gui
