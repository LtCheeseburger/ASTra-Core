#include "UpdaterLauncher.hpp"

#include <gf/core/log.hpp>

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QProgressDialog>
#include <QMessageBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QCoreApplication>
#include <QTemporaryDir>

// QuaZip / zlib for zip extraction (use Qt's own miniz wrapper available in Qt 6).
// We rely on QProcess + the system `tar` / PowerShell on Windows as a fallback
// so we have no extra library dependency.
#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace gf::gui::update {

// ── helpers ──────────────────────────────────────────────────────────────────

static QString tempUpdateDir() {
    return QDir::tempPath() + QStringLiteral("/astra_update");
}

// ── UpdaterLauncher ───────────────────────────────────────────────────────────

UpdaterLauncher::UpdaterLauncher(QWidget* parentWidget, QObject* parent)
    : QObject(parent)
    , m_parentWidget(parentWidget)
    , m_nam(new QNetworkAccessManager(this))
{}

void UpdaterLauncher::startUpdate(const ReleaseInfo& info) {
    m_info = info;

    if (info.downloadUrl.isEmpty()) {
        emit downloadFailed(tr("No download URL found for this release."));
        return;
    }

    // Prepare destination directory.
    m_destDir = tempUpdateDir();
    QDir().mkpath(m_destDir);

    m_assetPath = m_destDir + QStringLiteral("/") + info.assetName;

    gf::core::Log::get()->info("[Updater] Download started → {}",
                               m_assetPath.toStdString());

    // Progress dialog
    m_progress = new QProgressDialog(
        tr("Downloading ASTra %1…").arg(info.tagName),
        tr("Cancel"),
        0, 100,
        m_parentWidget);
    m_progress->setWindowTitle(tr("Downloading Update"));
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(0);
    m_progress->setValue(0);

    // Start download
    QNetworkRequest req{QUrl(info.downloadUrl)};
    req.setRawHeader("User-Agent", "ASTra-Updater/1.0");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam->get(req);

    connect(m_reply, &QNetworkReply::downloadProgress,
            this,    &UpdaterLauncher::onDownloadProgress);
    connect(m_reply, &QNetworkReply::finished,
            this,    &UpdaterLauncher::onDownloadFinished);
    connect(m_reply, &QNetworkReply::errorOccurred,
            this,    &UpdaterLauncher::onDownloadError);
    connect(m_progress, &QProgressDialog::canceled, this, [this]() {
        if (m_reply) m_reply->abort();
    });
}

void UpdaterLauncher::onDownloadProgress(qint64 received, qint64 total) {
    if (!m_progress) return;
    if (total > 0)
        m_progress->setValue(static_cast<int>(received * 100 / total));
}

void UpdaterLauncher::onDownloadFinished() {
    if (!m_reply) return;
    m_reply->deleteLater();

    if (m_progress) {
        m_progress->setValue(100);
        m_progress->close();
        m_progress->deleteLater();
        m_progress = nullptr;
    }

    if (m_reply->error() != QNetworkReply::NoError) {
        // Handled by onDownloadError already.
        return;
    }

    // Write file.
    QFile out(m_assetPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString msg = tr("Failed to write update file: %1").arg(m_assetPath);
        gf::core::Log::get()->error("[Updater] {}", msg.toStdString());
        emit downloadFailed(msg);
        return;
    }
    out.write(m_reply->readAll());
    out.close();

    gf::core::Log::get()->info("[Updater] Update downloaded successfully → {}",
                               m_assetPath.toStdString());

    // Extract if it's a zip.
    const QString extractDir = m_destDir + QStringLiteral("/extracted");
    QDir().mkpath(extractDir);

    if (m_assetPath.endsWith(".zip", Qt::CaseInsensitive)) {
        if (!extractZip(m_assetPath, extractDir)) {
            emit downloadFailed(tr("Failed to extract update archive."));
            return;
        }
    } else {
        // If it's already an .exe installer we can run it directly.
        QFile::copy(m_assetPath, extractDir + QStringLiteral("/") + QFileInfo(m_assetPath).fileName());
    }

    gf::core::Log::get()->info("[Updater] Extraction complete");
    launchUpdater(extractDir);
}

void UpdaterLauncher::onDownloadError(QNetworkReply::NetworkError /*code*/) {
    if (!m_reply) return;
    const QString msg = m_reply->errorString();
    gf::core::Log::get()->error("[Updater] Download error: {}", msg.toStdString());

    if (m_progress) {
        m_progress->close();
        m_progress->deleteLater();
        m_progress = nullptr;
    }

    emit downloadFailed(msg);
}

bool UpdaterLauncher::extractZip(const QString& zipPath, const QString& destDir) {
#ifdef Q_OS_WIN
    // Use PowerShell's Expand-Archive (available on Windows 8+).
    // We run it synchronously with a generous timeout.
    const QString cmd = QStringLiteral(
        "powershell -NoProfile -Command \"Expand-Archive -Path '%1' -DestinationPath '%2' -Force\"")
        .arg(zipPath, destDir);

    gf::core::Log::get()->info("[Updater] Extracting via PowerShell: {}",
                               cmd.toStdString());

    QProcess ps;
    ps.start("powershell", {
        "-NoProfile", "-Command",
        QStringLiteral("Expand-Archive -Path '%1' -DestinationPath '%2' -Force")
            .arg(zipPath, destDir)
    });
    if (!ps.waitForFinished(120'000)) {
        gf::core::Log::get()->error("[Updater] PowerShell extract timed out");
        return false;
    }
    return ps.exitCode() == 0;
#else
    // Unix: use system unzip or Python
    QProcess proc;
    proc.start("unzip", {"-o", zipPath, "-d", destDir});
    if (!proc.waitForFinished(120'000))
        return false;
    return proc.exitCode() == 0;
#endif
}

void UpdaterLauncher::launchUpdater(const QString& extractDir) {
    // Locate astra_updater.exe next to the running executable.
    const QString appDir   = QCoreApplication::applicationDirPath();
    const QString updaterExe = appDir + QStringLiteral("/astra_updater.exe");

    if (!QFile::exists(updaterExe)) {
        const QString msg = tr("Updater helper not found: %1").arg(updaterExe);
        gf::core::Log::get()->error("[Updater] {}", msg.toStdString());
        emit downloadFailed(msg);
        return;
    }

    // Sanity: ensure extracted directory is not empty.
    if (QDir(extractDir).isEmpty()) {
        const QString msg = tr("Extracted update directory is empty.");
        gf::core::Log::get()->error("[Updater] {}", msg.toStdString());
        emit downloadFailed(msg);
        return;
    }

    gf::core::Log::get()->info("[Updater] Launching updater helper: {}",
                               updaterExe.toStdString());

    // Arguments: <pid> <update-source-dir> <app-install-dir>
    const QStringList args = {
        QString::number(QCoreApplication::applicationPid()),
        extractDir,
        appDir
    };

    // Detach: the helper must outlive this process.
    QProcess::startDetached(updaterExe, args);

    emit updateReadyToInstall();
}

} // namespace gf::gui::update
