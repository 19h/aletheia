#pragma once

#include <string>

namespace aletheia {

/// Self-contained GNU C11 support preamble used by portable-C translation
/// units. Helpers implement only semantics whose width is explicit; unresolved
/// IR values trap through __aletheia_undefined_u64().
std::string portable_c_runtime_preamble();

} // namespace aletheia
