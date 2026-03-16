#include "QtSpdlogSink.hpp"

#include "LogBus.hpp"

namespace gf::gui {

void QtSpdlogSink::sink_it_(const spdlog::details::log_msg& msg) {
  // Use spdlog's formatter if present.
  spdlog::memory_buf_t formatted;
  base_sink<std::mutex>::formatter_->format(msg, formatted);

  // Convert to QString (UTF-8).
  const auto str = std::string(formatted.data(), formatted.size());
  LogBus::instance().emitLine(QString::fromUtf8(str.c_str(), static_cast<int>(str.size())));
}

} // namespace gf::gui
