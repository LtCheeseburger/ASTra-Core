#pragma once

#include <string_view>

namespace gf::core {

// Feature gates are used to keep beta/editor functionality behind explicit
// switches until the planned milestone (v0.6.0).
//
// IMPORTANT: In v0.5.x, all beta/editor features should remain disabled by
// default.
enum class Feature {
  AstEditor,
  RsfConfigViewer,
  TexturePreview,
  AptEditor,
  DbEditor,
  AptLiveEditing,
};

// Returns true if the feature is currently enabled.
bool feature_enabled(Feature f);

// Enable all gated features when running in developer/beta mode.
// In v0.5.x this is only enabled via explicit opt-in (e.g. --enable-beta).
void set_beta_enabled(bool enabled);

// Human-readable key (stable) for UI/logging/config.
std::string_view feature_key(Feature f);

} // namespace gf::core
