#pragma once

#include <QObject>
#include <QString>

// Simple Qt-side log bus so backend logging (spdlog, etc.) can stream into the GUI.
// Intentionally tiny: the view enforces a max line count.
class LogBus final : public QObject {
  Q_OBJECT

public:
  static LogBus& instance() {
    static LogBus bus;
    return bus;
  }

  // Thread-safe emission (queued if called cross-thread).
  void emitLine(const QString& line);

signals:
  void lineEmitted(const QString& line);

private:
  explicit LogBus(QObject* parent = nullptr) : QObject(parent) {}
};
