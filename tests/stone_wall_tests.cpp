#include "session/game_session.hpp"
#include "persistence/world_file.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace pixel;
namespace {
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
std::shared_ptr<const ContentRegistry> content() { return ContentRegistry::build({{"material.stone", "Stone", 8}}).registry; }
void conserved(const GameSession& session) {
    const auto view = *session.published_world();
    const auto snapshot = *session.capture_snapshot();
    std::uint64_t actual_world = 0, actual_drops = 0;
    for (auto cell : snapshot.cells) actual_world += (cell == 1);
    for (auto drop : snapshot.drops) actual_drops += drop.amount;
    require(actual_world == view.world_stone && actual_drops == view.dropped_stone, "published counts match actual state");
    require(actual_world + actual_drops + snapshot.held_stone == snapshot.initial_stone, "stone conservation");
}
CommandResult execute(GameSession& session, WorldCommand command) {
    require(bool(session.submit(command)), "command submission");
    require(session.advance_fixed_step(), "fixed step");
    conserved(session);
    return session.command_results().back();
}
bool same_except_tick(WorldSnapshot a, const WorldSnapshot& b) { a.tick = b.tick; return a == b; }
void coordinates() {
    require(cell_to_chunk({-1,-65}) == ChunkPos{-1,-2}, "negative chunk floor");
    require(cell_in_chunk({-1,-65}) == CellPos{63,63}, "negative local coordinate");
    require(cell_to_chunk({-64,64}) == ChunkPos{-1,1}, "exact boundaries");
    require(building_cell_origin({-1,-9}) == CellPos{-8,-16}, "negative building grid");
    WorldState empty(7, {{-67,-65}, 130,130});
    require(empty.material_at({-67,-65}) == Material::air && empty.material_at({62,64}) == Material::air, "unaligned finite bounds");
    require(!empty.material_at({-68,-65}) && !empty.material_at({63,64}), "outside distinct from air");
    require(!empty.material_at({std::numeric_limits<int>::max(),0}), "extreme queries are bounded");
    auto wall = make_stone_wall_scenario(1);
    require(wall.view().world_stone == 7168, "authored wall count");
    for (auto p : {CellPos{-1,32}, CellPos{0,32}, CellPos{63,64}, CellPos{64,64}})
        require(wall.material_at(p) == Material::stone, "wall spans chunk borders");
    require(wall.material_at({-25,16}) == Material::air, "wall edge");
}
void transactions() {
    GameSession s(content());
    require(bool(s.create_stone_wall_world(77)), "create scenario");
    static_cast<void>(s.take_changes());
    auto before = *s.capture_snapshot();
    require(bool(s.submit({CommandKind::dig,{64,64},4})), "queue cross-chunk dig");
    require(*s.capture_snapshot() == before, "queue does not mutate world before fixed step");
    require(s.advance_fixed_step(), "apply cross-chunk dig");
    const auto removed = s.command_results().back().amount;
    require(removed == 49, "radius-four local circle removes actual cells");
    auto changes = s.take_changes();
    require(changes.dirty_chunks.size() == 4 && !changes.full_refresh, "cross-border dirty chunks");
    require(s.world()->drops().size() == 4, "one aggregate per affected chunk");
    for (auto p : {CellPos{63,63}, CellPos{64,63}, CellPos{63,64}, CellPos{64,64}})
        require(s.world()->material_at(p) == Material::air, "cross-border material query after dig");
    require(execute(s, {CommandKind::dig,{64,64},4}).amount == 0, "repeated dig gives no reward");
    require(s.take_changes().dirty_chunks.empty(), "unchanged world does not publish dirty textures");
    before = *s.capture_snapshot();
    require(!execute(s, {CommandKind::repair,{63,63}}).ok, "repair before pickup fails");
    require(same_except_tick(before, *s.capture_snapshot()), "insufficient repair has no partial changes");
    require(execute(s, {CommandKind::pick_up,{}}).amount == removed, "pickup transfers all drops");
    require(s.world()->drops().empty(), "pickup removes aggregate drops");
    require(execute(s, {CommandKind::repair,{63,63}}).amount > 0, "repair northwest grid");
    require(s.world()->material_at({63,63}) == Material::stone && s.world()->material_at({64,64}) == Material::air, "only selected grid repaired");
    before = *s.capture_snapshot();
    require(!execute(s, {CommandKind::repair,{-60,4}}).ok, "64-unit new grid cannot be partially filled with remaining stone");
    require(same_except_tick(before, *s.capture_snapshot()), "failed repair preserves shape, drops and container");
    require(!execute(s, {CommandKind::dig,{9999,4}}).ok, "out-of-world command rejected");
    require(!execute(s, {CommandKind::dig,{0,40},9}).ok, "unbounded brush rejected");
    for (auto p : {CellPos{64,63}, CellPos{63,64}, CellPos{64,64}}) require(execute(s, {CommandKind::repair,p}).ok, "repair other grids");
    require(s.published_world()->world_stone == 7168 && s.published_world()->held_stone == 0, "full repair spends exactly excavated quantity");
    // Accumulate changes across several unconsumed physics steps.
    static_cast<void>(s.take_changes());
    execute(s, {CommandKind::dig,{-1,36},4});
    execute(s, {CommandKind::dig,{64,36},4});
    require(s.take_changes().dirty_chunks.size() == 3, "changes survive missed render frames and coalesce");
    require(bool(s.pause()) && !s.submit({CommandKind::pick_up,{}}), "paused commands rejected");
    before = *s.capture_snapshot();
    require(!s.advance_fixed_step() && *s.capture_snapshot() == before, "paused shape and quantities stable");
    require(bool(s.resume()), "resume");
    for (int i = 0; i < 120; ++i) {
        const CellPos p{-20 + i % 100, 20 + i % 50};
        execute(s, {CommandKind::dig,p,2});
        if (i % 3 == 0) execute(s, {CommandKind::pick_up,{}});
        before = *s.capture_snapshot();
        const auto result = execute(s, {CommandKind::repair,p});
        if (!result.ok) require(same_except_tick(before, *s.capture_snapshot()), "transaction stress failure atomicity");
    }
    require(bool(s.submit({CommandKind::dig,{0,32}})), "pending command before end");
    s.end();
    require(bool(s.create_empty_world(8)), "create empty after scenario");
    require(s.advance_fixed_step() && s.published_world()->world_stone == 0 && s.published_world()->held_stone == 0, "empty world retains no commands/materials/resources");
}
struct TestDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("PixelSandbox-stage-b-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TestDirectory() { std::filesystem::create_directories(path); }
    ~TestDirectory() {
        std::error_code error;
        const auto parent = std::filesystem::weakly_canonical(path.parent_path(), error);
        if (error) return;
        const auto expected = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(), error);
        if (!error && parent == expected && path.filename().string().starts_with("PixelSandbox-stage-b-"))
            std::filesystem::remove_all(path, error);
    }
};
void persistence_test() {
    TestDirectory directory;
    const auto path = directory.path / std::filesystem::path(u8"石墙.psw");
    GameSession s(content());
    require(bool(s.create_stone_wall_world(0xfedcba9876543210ULL)), "create saved scenario");
    execute(s, {CommandKind::dig,{-1,36},4});
    execute(s, {CommandKind::pick_up,{}});
    execute(s, {CommandKind::repair,{-1,36}});
    execute(s, {CommandKind::dig,{64,64},2}); // Non-empty drops as well as held resource.
    const auto saved = *s.capture_snapshot();
    const auto written = persistence::save_world_file(path, saved);
    require(written.ok && written.elapsed_ms >= 0, "save: " + written.error);
    const auto loaded = persistence::load_world_file(path);
    require(loaded.snapshot && *loaded.snapshot == saved, "binary roundtrip: " + loaded.error);
    s.end();
    require(bool(s.restore_world(*loaded.snapshot)), "restore after returning to menu");
    require(s.state() == SessionState::paused && *s.capture_snapshot() == saved, "load preserves exact shape, counts, seed and tick");
    auto bad = saved;
    bad.material_ids[0] = "material.missing";
    require(!s.restore_world(bad) && *s.capture_snapshot() == saved, "missing content does not replace active world");
    require(persistence::save_world_file(directory.path / "missing.psw", bad).ok, "write structurally valid missing-content fixture");
    const auto missing_content = persistence::load_world_file(directory.path / "missing.psw");
    require(missing_content.snapshot && !s.restore_world(*missing_content.snapshot), "file content references validated before commit");
    bad = saved; ++bad.held_stone;
    require(!s.restore_world(bad) && !persistence::save_world_file(path, bad).ok, "invalid conservation rejected on load and save");
    bad = saved; bad.cells[0] = 99;
    require(!s.restore_world(bad), "invalid material index rejected");
    bad = saved; bad.building_edge = 9;
    require(!s.restore_world(bad), "unsupported grid rejected");
    bad = saved; bad.drops.push_back(bad.drops.front());
    require(!s.restore_world(bad), "duplicate aggregated drops rejected");
    // A blocked temporary path forces an actual write failure; old save survives.
    auto blocked = path; blocked += ".tmp";
    std::filesystem::create_directory(blocked);
    require(!persistence::save_world_file(path, saved).ok, "temporary write failure surfaced");
    require(persistence::load_world_file(path).snapshot == saved, "old save remains usable after failed write");
    std::filesystem::remove(blocked);
    std::ifstream input(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    input.close();
    auto write_bytes = [&](const auto& data) { std::ofstream out(path, std::ios::binary | std::ios::trunc); out.write(data.data(), static_cast<std::streamsize>(data.size())); };
    auto corrupt = bytes; corrupt.back() ^= 1; write_bytes(corrupt);
    auto failure = persistence::load_world_file(path);
    require(!failure.snapshot && failure.error.find("checksum") != std::string::npos && *s.capture_snapshot() == saved, "corruption rejected without altering live world");
    corrupt = bytes; corrupt[8] = 99; write_bytes(corrupt);
    failure = persistence::load_world_file(path);
    require(!failure.snapshot && failure.error.find("version") != std::string::npos, "unknown format version diagnostic");
    corrupt.resize(12); write_bytes(corrupt);
    require(!persistence::load_world_file(path).snapshot && *s.capture_snapshot() == saved, "truncated file leaves world intact");
    require(!persistence::load_world_file(directory.path / "absent.psw").snapshot, "missing file error");
    require(bool(s.resume()) && s.advance_fixed_step() && s.published_world()->tick == saved.tick + 1, "loaded world resumes at next saved tick");
}
} // namespace
int main(int argc, char** argv) {
    try {
        const std::string name = argc == 2 ? argv[1] : "";
        if (name == "coordinates") coordinates();
        else if (name == "transactions") transactions();
        else if (name == "persistence") persistence_test();
        else throw std::runtime_error("Expected coordinates, transactions or persistence.");
        std::cout << "PASS " << name << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
