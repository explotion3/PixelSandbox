#include "session/game_session.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
std::shared_ptr<const pixel::ContentRegistry> content() {
    auto result = pixel::ContentRegistry::build({{"material.stone", "Stone", 8}});
    require(bool(result), result.error);
    return result.registry;
}
void lifecycle() {
    pixel::GameSession session(content());
    require(session.state() == pixel::SessionState::idle, "initial state");
    require(!session.published_world(), "idle has no world");
    require(!session.advance_fixed_step(), "idle must not tick");
    require(!session.pause() && !session.resume(), "invalid transitions rejected");
    require(bool(session.create_empty_world(123)), "create world");
    require(!session.create_empty_world(999), "cannot overwrite active world");
    require(session.published_world()->seed == 123, "failed create preserves world");
    require(session.advance_fixed_step(), "running advances");
    auto old_view = session.published_world();
    session.end();
    require(session.state() == pixel::SessionState::ended && !session.published_world(), "end releases world");
    require(!session.advance_fixed_step() && !session.pause() && !session.resume(), "ended cannot update");
    session.end();
    require(bool(session.create_empty_world(456)), "recreate after end");
    require(session.published_world()->tick == 0 && session.published_world()->seed == 456, "new world has no old state");
    require(old_view->tick == 1 && old_view->seed == 123, "copied view survives teardown");
    require(bool(session.pause()), "pause before teardown");
    session.end();
    require(!session.published_world(), "paused world teardown");
    for (int i = 0; i < 100; ++i) {
        require(bool(session.create_empty_world(static_cast<std::uint64_t>(i))), "repeated enter");
        require(session.published_world()->tick == 0, "repeated enter resets ticks");
        require(session.advance_fixed_step(), "repeat tick");
        session.end();
    }
    bool rejected_null = false;
    try { pixel::GameSession invalid(nullptr); } catch (const std::invalid_argument&) { rejected_null = true; }
    require(rejected_null, "null content rejected");
}
void fixed_step() {
    pixel::GameSession session(content());
    require(bool(session.create_empty_world(1)), "create");
    for (int i = 0; i < 60; ++i) require(session.advance_fixed_step(), "running tick");
    require(session.published_world()->tick == 60, "one second = 60 steps");
    require(bool(session.pause()), "pause");
    require(!session.pause(), "double pause rejected");
    for (int i = 0; i < 100; ++i) require(!session.advance_fixed_step(), "paused step rejected");
    require(session.published_world()->tick == 60, "paused tick unchanged");
    require(bool(session.resume()) && !session.resume(), "resume once");
    require(session.advance_fixed_step() && session.published_world()->tick == 61, "resume continues");
    session.end();
    for (int i = 0; i < 100; ++i) require(!session.advance_fixed_step(), "ended step rejected");
}
void registry() {
    auto valid = content();
    require(valid->material_count() == 1, "material count");
    require(valid->find_material("material.stone")->hardness == 8, "lookup");
    require(valid->find_material("missing") == nullptr, "missing reference detectable");
    auto empty = pixel::ContentRegistry::build({});
    require(!empty && !empty.error.empty(), "empty content rejected");
    auto missing = pixel::ContentRegistry::build({{"", "Stone", 8}});
    require(!missing && missing.error.find("missing an ID") != std::string::npos, "missing ID diagnostic");
    auto duplicate = pixel::ContentRegistry::build({{"material.stone", "A", 1}, {"material.stone", "B", 2}});
    require(!duplicate && duplicate.error.find("material.stone") != std::string::npos, "duplicate ID diagnostic");
    require(!pixel::ContentRegistry::build({{"bad ID", "Stone", 8}}), "invalid ID rejected");
    require(!pixel::ContentRegistry::build({{"material.stone", "", 8}}), "missing display name rejected");
    require(!pixel::ContentRegistry::build({{"material.stone", "Stone", 0}}), "invalid hardness rejected");
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view name = argc == 2 ? argv[1] : "";
        if (name == "lifecycle") lifecycle();
        else if (name == "fixed_step") fixed_step();
        else if (name == "registry") registry();
        else throw std::runtime_error("Expected lifecycle, fixed_step or registry.");
        std::cout << "PASS " << name << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
