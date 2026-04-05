#include "ApplyPreviewDialog.hpp"
#include "RuntimeContentRoot.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMap>
#include <QPushButton>
#include <QStringList>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace gf::gui {

static constexpr int kMaxPreviewPaths = 6; // representative file paths per group

ApplyPreviewDialog::ApplyPreviewDialog(const ProfileResolvedMap&  resolved,
                                        const RuntimeTargetConfig& runtime,
                                        const QString&             profileName,
                                        QWidget*                   parent)
    : QDialog(parent)
{
    setWindowTitle(QString("Apply Preview \u2014 %1").arg(profileName));
    setMinimumSize(580, 420);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(8);

    auto* hdr = new QLabel(
        QString("<b>Profile:</b> %1<br>"
                "The following files will be written to the live game directories.")
            .arg(profileName.toHtmlEscaped()), this);
    hdr->setWordWrap(true);
    outer->addWidget(hdr);

    m_report = new QTextBrowser(this);
    m_report->setReadOnly(true);
    m_report->setOpenLinks(false);
    outer->addWidget(m_report, 1);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    m_btnApply  = new QPushButton("Apply", this);
    m_btnApply->setDefault(true);
    m_btnCancel = new QPushButton("Cancel", this);
    btnRow->addWidget(m_btnApply);
    btnRow->addWidget(m_btnCancel);
    outer->addLayout(btnRow);

    connect(m_btnApply,  &QPushButton::clicked, this, &QDialog::accept);
    connect(m_btnCancel, &QPushButton::clicked, this, &QDialog::reject);

    buildReport(resolved, runtime);
}

void ApplyPreviewDialog::buildReport(const ProfileResolvedMap&  resolved,
                                      const RuntimeTargetConfig& runtime)
{
    // Group files by destination root
    QMap<QString, QStringList> groups; // destRootPath → list of relPaths

    const QString baseLabel   = QString("Base  \u2192  %1").arg(runtime.astDirPath);
    const QString unknownBase = QStringLiteral("base");

    // Build a label for each configured content root
    QMap<QString, QString> rootLabels; // destRootPath → human label
    rootLabels.insert(QString(), baseLabel); // empty destRootPath = base
    for (const RuntimeContentRoot& cr : runtime.contentRoots) {
        const QString lbl = QString("%1  \u2192  %2")
                                .arg(runtimeContentKindLabel(cr.kind), cr.path);
        rootLabels.insert(cr.path, lbl);
    }

    for (const ResolvedAstFile& entry : resolved.files) {
        const QString key = entry.destRootPath; // empty = base
        groups[key].append(entry.filename);
    }

    QString html;
    html += QStringLiteral("<style>"
        "h3 { margin: 8px 0 4px 0; color: #2c3e50; }"
        "ul { margin: 2px 0 8px 12px; padding: 0; }"
        "li { font-family: monospace; font-size: 11px; }"
        ".more { color: gray; font-style: italic; }"
        "</style>");

    int totalFiles = 0;
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        const QString& root       = it.key();
        const QStringList& paths  = it.value();
        totalFiles += paths.size();

        const QString label = rootLabels.value(root,
            QString("Unknown root  \u2192  %1").arg(root));

        html += QString("<h3>%1 (%2 file%3)</h3><ul>")
                    .arg(label.toHtmlEscaped())
                    .arg(paths.size())
                    .arg(paths.size() == 1 ? "" : "s");

        const int show = qMin(paths.size(), kMaxPreviewPaths);
        for (int i = 0; i < show; ++i)
            html += QString("<li>%1</li>").arg(paths[i].toHtmlEscaped());
        if (paths.size() > kMaxPreviewPaths)
            html += QString("<li class='more'>… and %1 more</li>")
                        .arg(paths.size() - kMaxPreviewPaths);

        html += QStringLiteral("</ul>");
    }

    if (!resolved.warnings.isEmpty()) {
        html += QStringLiteral("<p style='color:#e67e22;'><b>Warnings:</b></p><ul>");
        for (const QString& w : resolved.warnings)
            html += QString("<li style='color:#e67e22;'>%1</li>").arg(w.toHtmlEscaped());
        html += QStringLiteral("</ul>");
    }

    if (totalFiles == 0) {
        html = QStringLiteral(
            "<p style='color:#c0392b;'>No files resolved — nothing to apply.</p>");
        m_btnApply->setEnabled(false);
    }

    m_report->setHtml(html);
}

} // namespace gf::gui
