#include "godot_host/material_definition_resource.hpp"
#include "godot_host/session_node.hpp"
#include "godot_host/sandbox_app.hpp"
#include "godot_host/world_canvas.hpp"
#include <godot_cpp/godot.hpp>

using namespace godot;
using namespace pixel::host;

void initialize_pixel_module(ModuleInitializationLevel level) {
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
    GDREGISTER_CLASS(SandboxMaterialDefinition);
    GDREGISTER_CLASS(SandboxSessionNode);
    GDREGISTER_CLASS(SandboxApp);
    GDREGISTER_CLASS(SandboxWorldCanvas);
}
void uninitialize_pixel_module(ModuleInitializationLevel /*level*/) {}

extern "C" {
GDExtensionBool GDE_EXPORT pixel_sandbox_init(GDExtensionInterfaceGetProcAddress get_proc_address,
        GDExtensionClassLibraryPtr library, GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize_pixel_module);
    init.register_terminator(uninitialize_pixel_module);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
}
