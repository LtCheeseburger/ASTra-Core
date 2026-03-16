\
#pragma once
#include <QDialog>

#include "GameLibrary.hpp"

class QLineEdit;
class QLabel;
class QPushButton;

namespace gf::gui {

class GameLibrary;

class AddGameDialog final : public QDialog {
  Q_OBJECT
public:
  explicit AddGameDialog(GameLibrary* lib, QWidget* parent = nullptr);

  bool wasAccepted() const { return m_accepted; }
  GameEntry result() const { return m_result; }

private slots:
  void onBrowse();
  void onAccept();

private:
  QString pickRootFolder();
  bool detectPs3Game(const QString& root, GameEntry* outGame, QString* outErr) const;
  bool detectPs4Game(const QString& root, GameEntry* outGame, QString* outErr) const;
  bool detectPsVitaGame(const QString& root, GameEntry* outGame, QString* outErr) const;
  bool detectPspGame(const QString& root, GameEntry* outGame, QString* outErr) const;
  bool detectXbox360Game(const QString& root, GameEntry* outGame, QString* outErr) const;

  GameLibrary* m_lib = nullptr;
  bool m_accepted = false;
  GameEntry m_result;

  QLabel* m_hint = nullptr;
  QLineEdit* m_rootPath = nullptr;
  QPushButton* m_browse = nullptr;
};

} // namespace gf::gui
