#pragma once
#include "UpdateChecker.hpp"
#include <QObject>
#include <QString>
#include <QNetworkReply>   // full type needed by MOC for NetworkError slot

class QWidget;
class QProgressDialog;
class QNetworkAccessManager;

namespace gf::gui::update {

// UpdaterLauncher is responsible for:
//   1. Downloading the release asset to %TEMP%/astra_update/
//   2. Showing a QProgressDialog during the download
//   3. Extracting the downloaded archive
//   4. Launching astra_updater.exe and then requesting ASTra to quit
//
// Signals:
//   updateReadyToInstall  – emitted just before QProcess launches the helper
//   downloadFailed        – emitted when any step fails (human-readable reason)
class UpdaterLauncher : public QObject {
    Q_OBJECT
public:
    explicit UpdaterLauncher(QWidget* parentWidget, QObject* parent = nullptr);

    // Begin the download → extract → launch pipeline.
    void startUpdate(const ReleaseInfo& info);

signals:
    // Emitted after the updater process is launched; the main app should quit.
    void updateReadyToInstall();
    // Emitted on failure.
    void downloadFailed(const QString& reason);

private slots:
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadFinished();
    void onDownloadError(QNetworkReply::NetworkError code);

private:
    bool extractZip(const QString& zipPath, const QString& destDir);
    void launchUpdater(const QString& updateDir);

    QWidget*                m_parentWidget = nullptr;
    QNetworkAccessManager*  m_nam          = nullptr;
    QNetworkReply*          m_reply        = nullptr;
    QProgressDialog*        m_progress     = nullptr;
    ReleaseInfo             m_info;
    QString                 m_destDir;
    QString                 m_assetPath;
};

} // namespace gf::gui::update
