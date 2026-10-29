#pragma once
#include "content/content_registry.hpp"

namespace pixel {
struct ReactionTableResult {
    std::vector<ReactionDefinition> reactions;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
// Five required fields, up to three numeric conditions, optional third input/output pair,
// followed by named region options. probability=... enables source sampling,
// global label binding, direction/fast/required flags and source blob geometry.
[[nodiscard]] ReactionTableResult parse_reaction_table(std::string_view text);
}
