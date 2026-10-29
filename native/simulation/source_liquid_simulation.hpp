#pragma once
#include "content/content_registry.hpp"
#include "core/world_state.hpp"
#include <span>

namespace pixel {
// Local ordinary-fluid update, within GameSession's single fixed step.
// Source accumulator arithmetic is separated from the legacy pressure model.
class SourceLiquidSimulation final {
public:
    static void step(WorldState&, const ContentRegistry&, std::span<const CellPos>,
        std::span<const std::uint8_t> attached, std::span<std::uint8_t> updated,
        std::span<std::uint8_t> transported);
};
}
