#pragma once

#include "content/content_registry.hpp"
#include "core/world_state.hpp"
#include "core/physical_units.hpp"
#include "gameplay/stone_rules.hpp"
#include "simulation/rigid_simulation.hpp"

#include <optional>
#include <string>

namespace pixel {

enum class SessionState { idle, running, paused, ended };
[[nodiscard]] const char* state_name(SessionState state) noexcept;

struct SessionResult {
    bool ok;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return ok; }
};

struct ChangeBatch {
    std::uint64_t sequence = 0;
    bool full_refresh = false;
    bool drops_changed = false;
    std::vector<ChunkPos> dirty_chunks;
    bool bodies_changed=false;
};

// The only owner of mutable world state. Host and UI consume copied views.
class GameSession final {
public:
    static constexpr std::uint32_t ticks_per_second = physical::ticks_per_second;
    static constexpr double fixed_delta_seconds = 1.0 / ticks_per_second;

    explicit GameSession(std::shared_ptr<const ContentRegistry> content,std::unique_ptr<RigidSolver> solver={});
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    [[nodiscard]] SessionResult create_empty_world(std::uint64_t seed);
    [[nodiscard]] SessionResult create_stone_wall_world(std::uint64_t seed);
    [[nodiscard]] SessionResult create_material_lab(std::uint64_t seed, bool liquids_only=false);
    [[nodiscard]] SessionResult submit(WorldCommand command);
    [[nodiscard]] SessionResult restore_world(const WorldSnapshot& snapshot);
    [[nodiscard]] std::optional<WorldSnapshot> capture_snapshot() const;
    [[nodiscard]] const WorldState* world() const noexcept { return world_ ? &*world_ : nullptr; }
    [[nodiscard]] const std::vector<CommandResult>& command_results() const noexcept { return results_; }
    [[nodiscard]] ChangeBatch take_changes();
    [[nodiscard]] SessionResult pause();
    [[nodiscard]] SessionResult resume();
    void end() noexcept;
    // Exactly one fixed step, never a render delta or a wall-clock accumulator.
    [[nodiscard]] bool advance_fixed_step();
    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] std::optional<WorldView> published_world() const noexcept;
    [[nodiscard]] const ContentRegistry& content() const noexcept { return *content_; }

private:
    std::shared_ptr<const ContentRegistry> content_;
    std::optional<WorldState> world_;
    SessionState state_ = SessionState::idle;
    std::vector<WorldCommand> commands_;
    std::vector<CommandResult> results_;
    ChangeBatch changes_;
    std::uint64_t publication_ = 0;
    std::unique_ptr<RigidSolver> rigid_solver_;
    void publish_full_world();
};

} // namespace pixel
