#include "gf/core/features.hpp"

#include <atomic>
#include <cstdlib>
#include <string>

namespace gf::core {

namespace {
std::atomic_bool g_beta_enabled{false};

bool env_truthy(const char* v) {
  if (!v) return false;
  std::string s(v);
  for (auto& c : s) c = static_cast<char>(::tolower(c));
  return (s == "1" || s == "true" || s == "yes" || s == "on");
}

bool beta_enabled_now() {
  // Explicit override wins.
  if (g_beta_enabled.load()) return true;
  // Env var opt-in for power users / CI.
  return env_truthy(std::getenv("ASTRA_ENABLE_BETA")) || env_truthy(std::getenv("GF_ENABLE_BETA"));
}
} // namespace

void set_beta_enabled(bool enabled) {
  g_beta_enabled.store(enabled);
}

bool feature_enabled(Feature f) {
  if (f == Feature::AstEditor) {
    // AST editor is the only stable editor mode available pre-v0.6.
    return true;
  }

  // v0.5.x foundation phase: editor/beta features remain OFF by default.
  // Only enable via explicit opt-in.
  return beta_enabled_now();
}

std::string_view feature_key(Feature f) {
  switch (f) {
    case Feature::AstEditor: return "ast_editor";
    case Feature::RsfConfigViewer: return "rsf_config_viewer";
    case Feature::TexturePreview: return "texture_preview";
    case Feature::AptEditor: return "apt_editor";
    case Feature::DbEditor: return "db_editor";
    case Feature::AptLiveEditing: return "apt_live_editing";
    default: return "unknown";
  }
}

} // namespace gf::core
