#pragma once

#include <mutex>
#include <spdlog/sinks/base_sink.h>

namespace gf::gui {

// Forwards spdlog output into LogBus (Qt GUI).
class QtSpdlogSink final : public spdlog::sinks::base_sink<std::mutex> {
protected:
  void sink_it_(const spdlog::details::log_msg& msg) override;
  void flush_() override {}
};

// Keep naming compatible with the rest of the codebase.
using QtSpdlogSink_mt = QtSpdlogSink;

} // namespace gf::gui
