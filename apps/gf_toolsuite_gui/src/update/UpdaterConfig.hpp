#pragma once

// ── ASTra auto-updater configuration ─────────────────────────────────────────
// Adjust ASTRA_GITHUB_OWNER and ASTRA_GITHUB_REPO to match the GitHub
// repository where releases are published.

#define ASTRA_GITHUB_OWNER "LtCheeseburger"
#define ASTRA_GITHUB_REPO  "ASTra-Core"

// Current application version.  This is compared against the GitHub release
// tag to determine whether an update is available.
// Format must be compatible with UpdateChecker::isNewer():  vMAJOR.MINOR.PATCH
// with an optional pre-release suffix such as "-beta" or "-rc1".
//
// In a CMake build this constant is generated from cmake/gf_version.cmake
// via the configured version.hpp; the fallback literal here is used in
// environments where that generated header is not yet on the include path.
// ASTRA_CURRENT_VERSION_QSTRING produces a QString regardless of whether the
// generated version.hpp is available.  kVersionString is a constexpr const
// char*, so we use QString::fromUtf8() for that branch; in the fallback branch
// we can use QStringLiteral because the argument is a string literal.
#if __has_include(<gf_core/version.hpp>)
#  include <gf_core/version.hpp>
#  define ASTRA_CURRENT_VERSION_QSTRING QString::fromUtf8(gf::core::kVersionString)
#else
#  define ASTRA_CURRENT_VERSION_QSTRING QStringLiteral("0.9.9")
#endif
