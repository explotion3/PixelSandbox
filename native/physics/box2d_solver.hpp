#pragma once
#include "simulation/rigid_simulation.hpp"
#include <memory>
namespace pixel {
[[nodiscard]] std::unique_ptr<RigidSolver> make_box2d_solver();
}
