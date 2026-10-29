#pragma once
#include "core/world_state.hpp"
#include <string>

namespace pixel {
enum class CommandKind { dig, pick_up, repair, paint, erase, heat, cool, loosen, push, blast, electrify, spawn_body, detach_body, source_warm, source_cold, environment_normal, environment_cold, environment_hot };
struct WorldCommand {
    CommandKind kind = CommandKind::dig;
    CellPos target;
    int radius = 4;
    std::string material_id;
};
struct CommandResult {
    CommandKind kind;
    bool ok;
    std::uint64_t amount;
    std::string message;
};
class StoneRules final {
public:
    [[nodiscard]] static CommandResult apply(WorldState& world, const WorldCommand& command);
};
} // namespace pixel
