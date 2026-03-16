#pragma once

#include <optional>
#include <string>

namespace gf::apt {

// Converts an APT+CONST pair to an XML string (EASE-style).
// Returns std::nullopt on failure; if err != nullptr, a human-friendly reason is stored.
std::optional<std::string> apt_to_xml_string(const std::string& apt_path,
                                            const std::string& const_path,
                                            std::string* err);

} // namespace gf::apt
