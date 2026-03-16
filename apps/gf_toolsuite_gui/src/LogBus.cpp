#include "LogBus.hpp"

#include <QMetaObject>
#include <QThread>

void LogBus::emitLine(const QString& line) {
  // Ensure signal emission happens on the LogBus' thread to keep Qt happy.
  if (QThread::currentThread() == thread()) {
    emit lineEmitted(line);
    return;
  }

  // Queue into the object's thread.
  QMetaObject::invokeMethod(
      this,
      [this, line]() { emit lineEmitted(line); },
      Qt::QueuedConnection);
}
