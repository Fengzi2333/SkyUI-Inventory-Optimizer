#include "Hooks.h"
#include "ProfilingHooks.h"
namespace plugin {
    void Hooks::install() {
        QuitGameHook::install();
        QuitGameDetoursHook::install();
        ProfilingHooks::install();
    }

    void Hooks::quitGame() {
        logger::info("Game quitting");
    }
}  // namespace plugin
