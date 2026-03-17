#pragma once
#include "UpdateChecker.hpp"
#include <QDialog>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QProgressDialog;

namespace gf::gui::update {

// Modal dialog shown when a new version is available.
// Displays version info + release notes and offers "Update Now" / "Later".
class UpdateDialog : public QDialog {
    Q_OBJECT
public:
    explicit UpdateDialog(const ReleaseInfo& info, QWidget* parent = nullptr);

signals:
    // Emitted when the user clicks "Update Now".
    void updateRequested(const ReleaseInfo& info);

private slots:
    void onUpdateNow();
    void onLater();

private:
    ReleaseInfo m_info;
    QPushButton* m_btnUpdate = nullptr;
    QPushButton* m_btnLater  = nullptr;
};

// Simple "you are up to date" dialog.
class UpToDateDialog : public QDialog {
    Q_OBJECT
public:
    explicit UpToDateDialog(QWidget* parent = nullptr);
};

} // namespace gf::gui::update
