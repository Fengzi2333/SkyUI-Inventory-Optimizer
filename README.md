# ***SkyUI Inventory Optimizer***

Native SKSE plugin that takes over SkyUI's inventory item-card filling.

SkyUI (v6.11) fills every visible item card from `InventoryDataSetter.as`, a
1304-line ActionScript 2 script whose `processEntry` runs once per visible entry
inside the game's AS2 interpreter. This plugin replaces that hot path with a C++
replica of the same member reads and writes, executed by compiled code instead.

## ***How it works***

- **No cache.** The replica recomputes every item from the engine's own entry
  object on every call, so there is no cross-open staleness and no invalidation
  contract to keep.
- **One-time translation table.** `Translator.translate` is not a lookup table:
  it asks the game's localization system for a string, and C++ has no equivalent
  of "what does `$Scroll` localize to". The literals are therefore resolved once
  per process through the AS function, which makes a display string on the hot
  path one table index.
- **Fail-closed.** A member whose runtime type the replica cannot reason about
  makes that single call fall back to the untouched AS2 body. Nothing is written
  until a complete prediction is ready, so the fallback is unobservable.
- **Master switch.** `SSE_REPLICATE_PROCESS_ENTRY` (default `1`) is defined in
  `src/ProcessEntryReplica.h`. Set it to `0` (configure with
  `-DSSE_REPLICATE_PROCESS_ENTRY=0`) to compile the replica out: every item takes
  the original ActionScript path again.

## ***Runtime requirements***

- [Skyrim Script Extender (SKSE)](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- [SkyUI](https://www.nexusmods.com/skyrimspecialedition/mods/12604) (v6.11)

## ***Build requirements***

- [CMake](https://cmake.org/)
- [vcpkg](https://vcpkg.io/en/)
- [Visual Studio Community 2022](https://visualstudio.microsoft.com/vs/community/)
- [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG)

#### ***CommonLibSSE-NG***

To use CommonLibSSE-NG as a git-submodule instead of overlay-ports, clone it to extern/CommonLibSSE-NG and edit vcpkg.json removing "commonlibsse-ng" and adding its dependencies (like "directxtk").

To use CommonLibSSE-NG as port, edit vcpkg.json adding "commonlibsse-ng".

## ***Building***

In `Developer Command Prompt for VS 2022` or `Developer PowerShell for VS 2022`, run:

~~~
git clone https://github.com/Fengzi2333/SkyUI-Inventory-Optimizer.git
cd SkyUI-Inventory-Optimizer
~~~

then

~~~
.\cmake\build.ps1
~~~

or

~~~
.\cmake\build.ps1 -buildPreset relwithdebinfo
~~~

or

~~~
.\cmake\build.ps1 -buildPreset debug
~~~

or

~~~
cmake -B build -S . --preset default --fresh
cmake --build build --preset release
~~~

Then get the .dll in build/Release, or the .zip (ready to install using mod manager) in build.

## ***Optional profiling***

Set `-DENABLE_TRACY=ON` to link the [Tracy](https://github.com/wolfpld/tracy)
client (expected in `extern/TracyProfiler`) and instrument the item-card hot
path. Source code never includes `<tracy/Tracy.hpp>` directly -- everything goes
through `src/Profiling.h`, which degrades to no-op macros when `TRACY_ENABLE` is
absent, so a profiling build is opt-in and a release build carries no probes.

## ***File local.cmake***

CMake will use a file named local.cmake (project root), in this file you can add something like:

~~~
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND xcopy /y /-I "bin\\$<CONFIG>\\${PROJECT_NAME}.dll" "C:\\games\\Skyrim\\Data\\SKSE\\Plugins\\${PROJECT_NAME}.dll"
    COMMAND xcopy /y /-I "bin\\$<CONFIG>\\${PROJECT_NAME}.pdb" "C:\\games\\Skyrim\\Data\\SKSE\\Plugins\\${PROJECT_NAME}.pdb"
)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "C:\\games\\Skyrim\\skse64_loader.exe" WORKING_DIRECTORY "C:\\games\\Skyrim"
)
~~~


## ***Credits***

Built on the [skse-clibng-template](https://github.com/epinter/skse-clibng-template)
build system.

## ***License***

[CC0 1.0 Universal](LICENSE).
