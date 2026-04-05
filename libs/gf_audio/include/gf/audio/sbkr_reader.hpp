#pragma once
#include "sbkr_types.hpp"
#include <optional>
#include <string>

namespace gf::audio {

class SbkrReader {
public:
    static std::optional<SbkrFile> read(const std::string& path,
                                        std::string*       errorOut = nullptr);
};

} // namespace gf::audio
