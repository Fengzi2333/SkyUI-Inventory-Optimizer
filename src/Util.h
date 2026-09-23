#pragma once

struct RuntimeUtil {
    static bool isWine() {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (GetProcAddress(ntdll, "wine_get_version")) {
            return true;
        }

        return false;
    }
};

struct Hooking {
        template <class T, size_t size = 5>
        static void writeCall() {
            SKSE::AllocTrampoline(64);
            auto& trampoline = SKSE::GetTrampoline();
            uintptr_t addrCall = T::srcFunc.address() + T::srcFuncOffset;
            T::orig = trampoline.write_call<size>(addrCall, T::hook);
            if constexpr (requires { T::logName; }) {
                if constexpr (requires { T::srcFunc.id(); }) {
                    SKSE::log::info("{} hook installed at address 0x{:X} (id {} + 0x{:X})", T::logName, T::srcFunc.offset(),
                                    T::srcFunc.id(), T::srcFuncOffset);
                } else {
                    SKSE::log::info("{} hook installed at address 0x{:X} + 0x{:X}", T::logName, T::srcFunc.offset(), T::srcFuncOffset);
                }
            }
        }

        template <class T, size_t size = 5>
        static void writeCall(uintptr_t srcFuncAddress, int64_t srcFuncOffset) {
            SKSE::AllocTrampoline(64);
            auto& trampoline = SKSE::GetTrampoline();
            uintptr_t addrCall = srcFuncAddress + srcFuncOffset;
            T::orig = trampoline.write_call<size>(addrCall, T::hook);
            if constexpr (requires { T::logName; }) {
                if constexpr (requires { T::srcFunc.id(); }) {
                    SKSE::log::info("{} hook installed at address 0x{:X} (id {} + 0x{:X})", T::logName, T::srcFunc.offset(),
                                    T::srcFunc.id(), T::srcFuncOffset);
                } else {
                    SKSE::log::info("{} hook installed at address 0x{:X} + 0x{:X}", T::logName, srcFuncAddress, T::srcFuncOffset);
                }
            }
        }

        template <class T, size_t size = 5>
        static void writeBranch() {
            SKSE::AllocTrampoline(64);
            auto& trampoline = SKSE::GetTrampoline();
            uintptr_t addrCall = T::srcFunc.address();
            T::orig = reinterpret_cast<T::FuncType>(trampoline.write_branch<size>(addrCall, T::hook));
            if constexpr (requires { T::logName; }) {
                if constexpr (requires { T::srcFunc.id(); }) {
                    SKSE::log::info("{} hook installed at address 0x{:X} (id {})", T::logName, T::srcFunc.offset(), T::srcFunc.id());
                } else {
                    SKSE::log::info("{} hook installed at address 0x{:X}", T::logName, T::srcFunc.offset());
                }
            }
        }

        template <class T>
        static void writeDetour() {
#if DETOURS_LIBRARY
            uintptr_t addrCall = T::srcFunc.address();
            T::orig = reinterpret_cast<T::FuncType>(addrCall);
            if (DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR &&
                DetourAttach(&reinterpret_cast<PVOID&>(T::orig), T::hook) == NO_ERROR && DetourTransactionCommit() == NO_ERROR) {
                if constexpr (requires { T::logName; }) {
                    if constexpr (requires { T::srcFunc.id(); }) {
                        SKSE::log::info("{} hook installed at address 0x{:X} (id {})", T::logName, T::srcFunc.offset(), T::srcFunc.id());
                    } else {
                        SKSE::log::info("{} hook installed at address 0x{:X}", T::logName, T::srcFunc.offset());
                    }
                }
            } else {
                SKSE::log::error("Failed to install hook");
            }
            static_assert(BUILDOPTIONS.detoursFound, "DETOURS FOUND");
#else
            static_assert(!BUILDOPTIONS.detoursFound, "DETOURS NOT FOUND");
#endif
        }

        /*
         * Same contract as writeDetour<T>(), but for a target whose address is
         * only known at *runtime* -- one that has no REL::RelocationID at all
         * because it is reached through a table instead of through the address
         * library.
         *
         * The motivating case is the engine's GFx delegate callbacks: the
         * function implementing e.g. "RequestItemCardInfo" is only reachable via
         *   IMenu::fxDelegate (IMenu.h:113)
         *     -> FxDelegate::callbacks (FxDelegate.h:50, GHash<GString, CallbackDefn>)
         *       -> CallbackDefn::callback (FxDelegate.h:24)
         * Resolving it at runtime deletes the (SE, AE) pair entirely, so such a
         * hook cannot go stale when the game is updated.
         *
         * T must expose the same members as for writeDetour<T>: FuncType, orig,
         * hook, and optionally logName.
         *
         * Returns true only when the whole Detours transaction committed, so the
         * caller can leave its "installed" flag unset and retry on a later
         * opportunity instead of silently profiling nothing.
         */
        template <class T>
        static bool writeDetourAt(uintptr_t a_targetAddress) {
#if DETOURS_LIBRARY
            static_assert(BUILDOPTIONS.detoursFound, "DETOURS FOUND");
            T::orig = reinterpret_cast<T::FuncType>(a_targetAddress);
            if (DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR &&
                DetourAttach(&reinterpret_cast<PVOID&>(T::orig), T::hook) == NO_ERROR && DetourTransactionCommit() == NO_ERROR) {
                if constexpr (requires { T::logName; }) {
                    SKSE::log::info("{} hook installed at runtime address 0x{:X} (SkyrimSE.exe + 0x{:X})", T::logName, a_targetAddress,
                                    a_targetAddress - REL::Module::get().base());
                }
                return true;
            }
            if constexpr (requires { T::logName; }) {
                SKSE::log::error("{}: Detours failed to attach at runtime address 0x{:X}", T::logName, a_targetAddress);
            }
            return false;
#else
            static_assert(!BUILDOPTIONS.detoursFound, "DETOURS NOT FOUND");
            if constexpr (requires { T::logName; }) {
                SKSE::log::warn("{}: detours unavailable, cannot hook runtime address 0x{:X}", T::logName, a_targetAddress);
            }
            return false;
#endif
        }
};