#pragma once

#include "Profiling.h"

namespace plugin {
    /*
     * Phase 1 instrumentation (Tracy) hook set.
     *
     * Design: docs/tracy-integration-plan.md section 4.
     * Everything here is compiled out unless TRACY_ENABLE is defined, so a
     * release build (-DENABLE_TRACY=OFF) carries no hooks and no Tracy calls.
     */
    class ProfilingHooks {
        public:
            // Called during SKSEPlugin_Load -> Hooks::install(): installs the
            // entry hooks. Safe at load time (single threaded, before the game
            // main loop starts).
            static void install();

            // Called after kDataLoaded, once RE::UI exists: registers the
            // MenuOpenCloseEvent sink used to slice the timeline into logical
            // "InventoryMenu" frames.
            static void installTimelineMarkers();

            // Phase 2 / target 2.1: hooks the engine's "RequestItemCardInfo" GFx
            // delegate, the function skyui calls once per inventory item. See
            // RequestItemCardInfoHook below for why this cannot be done at load
            // time and has to wait for the first InventoryMenu open. Idempotent
            // (no-ops after the first successful install), so the menu event
            // sink can call it on every open.
            static void installRequestItemCardInfoHook();

            // Phase 2 / target 2.5: hooks the GFx delegate boundary itself --
            // FxDelegate::Callback, the single entry point every AS -> C++ delegate
            // call passes through. Its zone is filtered to kItemCardMethodName, so
            // it measures the boundary cost of exactly the calls target 2.1
            // measures the body of:
            //     FxDelegate::Callback[RequestItemCardInfo] - RequestItemCardInfo
            //                                   = boundary marshalling cost
            // Unlike 2.1 this needs no menu instance (the vtable has an address
            // library entry), so it is installed during Hooks::install(). See
            // FxDelegateCallbackHook below. Idempotent.
            static void installFxDelegateCallbackHook();

            // ------------------------------------------------------------------
            // Phase 4 / step S-1: READ-ONLY ActionScript graph probe.
            // Full design: docs/phase4-design.md sections 4.3 (path discovery)
            // and 8.1 (step table).
            //
            // Why this must come before any optimization: target 2.5 proved the
            // engine-side GFx boundary is ~0, which leaves the remaining 47 % of
            // the skyui surcharge as one undivided black box. It can only be one
            // (or a mix) of four things, and they imply completely different
            // plans:
            //   S1+S2  ExternalInterface marshalling   -> phase 4a is enough
            //   S3     the processList loop itself     -> phase 4b-i
            //   S4     processEntry's AS body          -> phase 4b-ii (~855 lines)
            //   S5     the list UI rebuild             -> phase 4 is the WRONG plan
            // Guessing between them is exactly the mistake phase 2 was run to
            // avoid, so S-1 measures instead.
            //
            // MEASURED (2026-09-20, capture #11b; design section 10.7). The box is
            // now open, and S4 won:
            //   S1+S2  ~0 engine-side (target 2.5); 7.2 % of a round AS-side
            //                                            -> phase 4a covers it
            //   S3     NOT MEASURABLE -- the probe only ever saw empty spins
            //                                            -> dropped; 4b-i SKIPPED
            //   S4     processEntry = 303.3 ms/round, 37.7 %  -> phase 4b-ii
            //   S5     <= 8 % of a round                 -> phase 4 stays the plan
            // Route: 4a first (443.4 + 57.9 ~= 501 ms/round = 62.3 %), then 4b-ii.
            //
            // This step writes NOTHING into the movie: it only calls GetVariable /
            // GetMember / HasMember / VisitMembers and logs what it finds. There
            // is no SetVariable, no SetMember and no Invoke anywhere in it, so a
            // wrong guess costs a log line and nothing else (design section 8.2).
            // It also tells us which index of `_dataProcessors` is the
            // InventoryDataSetter, which the later steps need.
            //
            // WHERE it runs was corrected by the first live run (2026-09-20; see
            // design section 10.2). The first version probed on InventoryMenu
            // *close* and produced no output at all: IMenu::uiMovie -- and the
            // movie graph with it -- is already released by the time that event
            // arrives, so the probe returned early on every close without ever
            // latching. It now runs on the FIRST RequestItemCardInfo call, which
            // is the first moment the movie is guaranteed loaded, InitExtensions
            // has registered the data processors and entryList has been filled.
            //
            // `a_handler` is the FxDelegateHandler of that delegate call, which
            // for this delegate is the RE::InventoryMenu itself (IMenu derives
            // from FxDelegateHandler). Taking it from the caller avoids a second
            // lookup through RE::UI and, more importantly, avoids depending on the
            // menu still being registered -- the caller already holds the live
            // object. A null handler falls back to RE::UI.
            //
            // Idempotent: runs at most once.
            static void installAsPathProbe(RE::FxDelegateHandler* a_handler = nullptr);

            // ------------------------------------------------------------------
            // Phase 4 / steps S0a (probe 1) and S0b (probes 2 and 3):
            // WRAP-AND-FORWARD probes on the three members skyui drives on
            // `_dataProcessors[0]` -- `_requestItemInfo`, `processList`,
            // `processEntry`.
            // Full design: docs/phase4-design.md sections 4.2 (probe set and the
            // nesting equations), 8.1 (step table), 8.2 (revert contract) and 10.5
            // (the S0a real-machine verdict, hypothesis H2 confirmed).
            //
            // WHY THIS TARGET FIRST. Section 4.2 lists three probes; probe 1 is the
            // one that settles hypothesis H2, and H2 is a gate rather than a
            // measurement. `_requestItemInfo` is the only one of the three that
            // skyui invokes through `apply` (ItemcardDataExtender.as:55), so if a
            // `CreateFunction` object cannot serve as an `apply` target, that fails
            // here and nowhere else -- and if it fails, phases 4a and 4b are both
            // unreachable and the plan falls back to 3a' (section 7.1). It is also
            // an ordinary INSTANCE member, which makes it the safest of the three to
            // overwrite: replacing it cannot affect any other object, unlike the two
            // per-processor contract members.
            //
            // Each probe is a pure forwarder -- no cache, no optimization, no
            // behaviour change. Each has to reproduce three semantics exactly
            // (section 4.2): the `this` binding, the arguments and the return value.
            // All three go through one `Invoke("call", ...)`, because
            // `Params::argsWithThisRef` is literally [thisPtr, args...] and `call`
            // is Function.prototype.call. Note that the three call sites do NOT
            // agree on `thisPtr`: probe 1's is the LIST (`apply(a_list, [this, i])`),
            // probes 2 and 3's is the PROCESSOR (`_dataProcessors[i].processList(this)`
            // and the bare `processEntry(...)` inside it). Each probe dumps its own
            // on first call rather than assuming.
            //
            // ALL THREE GO IN TOGETHER. S3 is a difference of three totals, so a
            // partial install yields an uninterpretable number rather than a smaller
            // one; the install is all-or-nothing and rolls back on failure.
            //
            // Outcome (2026-09-20, capture #11b): all three did install, and S3
            // then turned out to be UNMEASURABLE rather than small -- the
            // `processList` probe only ever fires on the empty spin that follows
            // the real loop, because the first real loop is already running when
            // the install point fires. Installing all three together is exactly
            // what made that a trustworthy NEGATIVE result instead of a
            // plausible-looking wrong number (design section 10.7(3)).
            //
            // Failure safety (section 8.2): every step either succeeds or returns
            // without patching anything, so a wrong path guess costs log lines,
            // never a broken menu.
            //
            // Idempotent per menu open. Re-arming every open is REQUIRED, not
            // merely convenient: the list and every processor instance inside it
            // are rebuilt per open, so a replacement made in round 1 is gone by
            // round 2.
            static void requestAsProbeInstall();

            // Hot-path half of the pair above: called from
            // RequestItemCardInfoHook::hook on EVERY RequestItemCardInfo (32242 of
            // them in capture #11), so it stays a single flag read until
            // `requestAsProbeInstall` has armed it -- all the GFxValue graph work
            // happens once per round, not once per item.
            static void maybeInstallAsProbe(RE::FxDelegateHandler* a_handler);

            // ------------------------------------------------------------------
            // Phase 4a / step S1: the `_requestItemInfo` replacement itself --
            // the first step that CHANGES behaviour instead of measuring it.
            // Full design: docs/phase4-design.md section 5 (5.1 mechanism, 5.2
            // cache source, 5.3 expected saving, 5.4 risks), 8.1 row S1, 8.2
            // (revert contract). Implementation record: section 10.8.
            //
            // It rides on the S0a/S0b install above rather than adding a fourth
            // insertion point: `requestAsProbeInstall` / `maybeInstallAsProbe`
            // still arm and resolve the same three members on
            // `_dataProcessors[0]`, and only slot 0's replacement differs -- it
            // answers cache hits from C++ instead of forwarding. Slots 1 and 2
            // (`processList`, `processEntry`) stay pass-through probes on
            // purpose: capture #12 has to be comparable with capture #11b, and
            // phase 4b-ii is what consumes S4.
            //
            // TARGET: 443.4 ms/round (the engine `RequestItemCardInfo` body) +
            // 57.9 ms/round (the AS body, `apply`, the boundary) = 501.3 ms/round,
            // i.e. 62.3 % of the 804.6 ms/round measured in capture #11b
            // (section 10.7 (4)). Round 1 still pays full price -- it builds the
            // cache -- so the verdict of section 8.1 row S1 is a round-over-round
            // comparison, not a per-round absolute.
            //
            // SWITCH: `s0::kAnswerFromCache` (a compile-time constant, see its
            // block comment in ProfilingHooks.cpp for why). With it false this
            // file builds the exact S0b capture #11b binary; with it true, slot 0
            // answers and the per-round accounting
            // (`reportAndResetRoundStats`) prints hits/misses once per round
            // instead of leaving the answer to the Tracy timeline.
            //
            // REVERT (section 8.2 rules 1 and 4, at runtime and per item): the
            // original function is held as a `GFxValue` copy in the handler, and
            // every miss -- and every failure on the hit path -- forwards to it,
            // so the AS body is never more than one call away and a wrong cache
            // entry degrades to a full round trip rather than a wrong card.

            // Phase 4 / step S-1, second half: dumps the FxDelegate argument
            // layout for the first RequestItemCardInfo call of the session, which
            // validates or refutes the offset table in design section 4.3 (it is
            // a source-code inference there, not a measurement). Called from
            // RequestItemCardInfoHook::hook; prints once, then no-ops.
            static void probeDelegateArgs(const RE::FxDelegateArgs& a_params);

        private:
            /*
             * The one GFx delegate method phase 2 is about: skyui calls it once
             * per inventory item (section 7.6 F of
             * docs/inventory-system-analysis.md). Shared by target 2.1, whose zone
             * wraps the callback body, and target 2.5, which filters the shared
             * dispatcher on this very string -- one definition, so the two sides
             * can never drift apart.
             */
            static constexpr const char* kItemCardMethodName = "RequestItemCardInfo";

            /*
             * Target #1: InventoryChanges::GetItemCount(TESBoundObject*)
             *   SE 15868 / AE 16047 (see inventory-system-analysis.md section 6)
             *   Highest-frequency aggregate query: O(n) linked-list walk.
             *
             * The hook signature is copied 1:1 from
             *   extern/CommonLibSSE-NG/include/RE/I/InventoryChanges.h:56
             *     std::int16_t GetItemCount(RE::TESBoundObject* a_obj);
             * Do NOT "simplify" it: a member function takes `this` as the
             * implicit first argument (rcx on x64), and the narrow std::int16_t
             * return type means the upper 48 bits of rax are garbage -- widening
             * it to int32_t silently yields wrong values.
             */
            struct GetItemCountHook {
                    static std::int16_t hook(RE::InventoryChanges* a_this, RE::TESBoundObject* a_obj) {
                        SSE_ZONE("InventoryChanges::GetItemCount");
                        return orig(a_this, a_obj);
                    }

                    static inline std::string logName = "GetItemCount";
                    using FuncType = decltype(&hook);
                    static inline FuncType orig;
                    static inline REL::RelocationID srcFunc = REL::RelocationID{15868, 16047};  // (SE, AE)

                    static void install() {
                        // Detours is preferred: it chains automatically with
                        // other mods hooking the same entry. Without detours we
                        // fall back to a SKSE trampoline entry hook (see
                        // Hooking::writeBranch), which measures the same thing
                        // but is not chainable.
                        if constexpr (BUILDOPTIONS.detoursFound) {
                            Hooking::writeDetour<GetItemCountHook>();
                        } else {
                            logger::warn("{}: detours unavailable, using SKSE trampoline entry hook", logName);
                            Hooking::writeBranch<GetItemCountHook>();
                        }
                    }
            };

            /*
             * Targets #2..#8: phase-1 entry hooks for the read paths asked about
             * by the acceptance criteria (inventory-system-analysis.md section 5).
             * Every relocation ID below was confirmed against the CommonLibSSE-NG
             * source under extern/CommonLibSSE-NG/src/RE/I/*.cpp rather than taken
             * on faith from the upstream annotation table in section 6.1.
             *
             * Each follows the same shape -- declare the hook, copy the
             * signature verbatim from CommonLib, forward the return value, set
             * (SE, AE) relocations, and call Hooking::writeDetour / writeBranch
             * from install():
             */

            /*
             * Target #2: InventoryChanges::VisitInventory(IItemChangeVisitor&)
             *   SE 15855 / AE 16095 (inventory-system-analysis.md section 6.1)
             *   The enumeration entry point shared by the inventory UI and by
             *   other mods: one O(n) walk over entryList per call. Question 1 of
             *   the acceptance criteria ("how much of the operation is the walk")
             *   is answered by comparing this hook's total time with the
             *   InventoryMenu open/close interval from the timeline markers.
             *
             * Signature copied 1:1 from
             *   extern/CommonLibSSE-NG/include/RE/I/InventoryChanges.h:57
             *     void VisitInventory(IItemChangeVisitor& visitor);
             * The visitor is an explicit reference argument that follows `this`
             * (rcx = this, rdx = &visitor), and IItemChangeVisitor is a *nested*
             * type of InventoryChanges (see InventoryChanges.h:18), so it must be
             * spelled RE::InventoryChanges::IItemChangeVisitor here.
             */
            struct VisitInventoryHook {
                    static void hook(RE::InventoryChanges* a_this, RE::InventoryChanges::IItemChangeVisitor& a_visitor) {
                        SSE_ZONE("InventoryChanges::VisitInventory");
                        orig(a_this, a_visitor);
                    }

                    static inline std::string logName = "VisitInventory";
                    using FuncType = decltype(&hook);
                    static inline FuncType orig;
                    static inline REL::RelocationID srcFunc = REL::RelocationID{15855, 16095};  // (SE, AE)

                    static void install() {
                        // Detours is preferred: it chains automatically with
                        // other mods hooking the same entry. Without detours we
                        // fall back to a SKSE trampoline entry hook (see
                        // Hooking::writeBranch), which measures the same thing
                        // but is not chainable.
                        if constexpr (BUILDOPTIONS.detoursFound) {
                            Hooking::writeDetour<VisitInventoryHook>();
                        } else {
                            logger::warn("{}: detours unavailable, using SKSE trampoline entry hook", logName);
                            Hooking::writeBranch<VisitInventoryHook>();
                        }
                    }
            };

            /*
             * Target #3: InventoryChanges::GetInventoryWeight()
             *   SE 15883 / AE 16123 (inventory-system-analysis.md section 6.1)
             *   Called by every weight refresh in the UI; an O(n) walk that also
             *   recomputes totalWeight. Same evidence role as #2.
             *
             * Signature copied 1:1 from
             *   extern/CommonLibSSE-NG/include/RE/I/InventoryChanges.h:43
             *     float GetInventoryWeight();
             * Returns float in xmm0 -- keep the return type exactly float, do not
             * widen it to double (the callee-convention for the two differ).
             */
            struct GetInventoryWeightHook {
                    static float hook(RE::InventoryChanges* a_this) {
                        SSE_ZONE("InventoryChanges::GetInventoryWeight");
                        return orig(a_this);
                    }

                    static inline std::string logName = "GetInventoryWeight";
                    using FuncType = decltype(&hook);
                    static inline FuncType orig;
                    static inline REL::RelocationID srcFunc = REL::RelocationID{15883, 16123};  // (SE, AE)

                    static void install() {
                        if constexpr (BUILDOPTIONS.detoursFound) {
                            Hooking::writeDetour<GetInventoryWeightHook>();
                        } else {
                            logger::warn("{}: detours unavailable, using SKSE trampoline entry hook", logName);
                            Hooking::writeBranch<GetInventoryWeightHook>();
                        }
                    }
            };

            /*
             * Target #5: InventoryEntryData::GetValue() const
             *   SE 15757 / AE 15995 (inventory-system-analysis.md section 6.2)
             *   Representative of the *inner* loop: it walks extraLists, i.e. the
             *   m per-entry extra-data lists. Question 2 ("is the n x m blow-up
             *   real") is answered by #5.count / #1.count -- a ratio well above 1
             *   proves that callers iterate every entry and touch every extra list.
             *
             * Signature copied 1:1 from
             *   extern/CommonLibSSE-NG/include/RE/I/InventoryEntryData.h:44
             *     [[nodiscard]] std::int32_t GetValue() const;
             * The engine function is const-qualified, but `const` only affects the
             * compile-time type of `this`: it is still passed in rcx and the ABI is
             * identical, so the hook deliberately takes a non-const
             * RE::InventoryEntryData* (a cast-free hook signature cannot otherwise
             * be formed from a pointer-to-member-function).
             */
            struct GetValueHook {
                    static std::int32_t hook(RE::InventoryEntryData* a_this) {
                        SSE_ZONE("InventoryEntryData::GetValue");
                        return orig(a_this);
                    }

                    static inline std::string logName = "GetValue";
                    using FuncType = decltype(&hook);
                    static inline FuncType orig;
                    static inline REL::RelocationID srcFunc = REL::RelocationID{15757, 15995};  // (SE, AE)

                    static void install() {
                        if constexpr (BUILDOPTIONS.detoursFound) {
                            Hooking::writeDetour<GetValueHook>();
                        } else {
                            logger::warn("{}: detours unavailable, using SKSE trampoline entry hook", logName);
                            Hooking::writeBranch<GetValueHook>();
                        }
                    }
            };

            /*
             * Target #6: InventoryChanges::GetArmorInSlot(std::int32_t)
             *   SE 15873 / AE 16113 (inventory-system-analysis.md section 6.1)
             *   Armour-slot lookup; the engine implementation walks entryList,
             *   so it is another O(n) consumer on the read path.
             *
             * Signature copied 1:1 from
             *   extern/CommonLibSSE-NG/include/RE/I/InventoryChanges.h:42
             *     TESObjectARMO* GetArmorInSlot(std::int32_t a_slot);
             * Note: CommonLib's own wrapper adds a VR-only branch that bypasses
             * this relocation (src/RE/I/InventoryChanges.cpp:47-61). That branch
             * lives in the *wrapper*, not in the engine function, so it does not
             * affect the SE/AE entry hook installed here.
             */
            struct GetArmorInSlotHook {
                    static RE::TESObjectARMO* hook(RE::InventoryChanges* a_this, std::int32_t a_slot) {
                        SSE_ZONE("InventoryChanges::GetArmorInSlot");
                        return orig(a_this, a_slot);
                    }

                    static inline std::string logName = "GetArmorInSlot";
                    using FuncType = decltype(&hook);
                    static inline FuncType orig;
                    static inline REL::RelocationID srcFunc = REL::RelocationID{15873, 16113};  // (SE, AE)

                    static void install() {
                        if constexpr (BUILDOPTIONS.detoursFound) {
                            Hooking::writeDetour<GetArmorInSlotHook>();
                        } else {
                            logger::warn("{}: detours unavailable, using SKSE trampoline entry hook", logName);
                            Hooking::writeBranch<GetArmorInSlotHook>();
                        }
                    }
            };

            /*
             * Target #7: InventoryChanges::GetWornMask()
             *   SE 15806 / AE 16044 (inventory-system-analysis.md section 6.1)
             *   Equipment-state query; the engine implementation walks entryList
             *   looking for worn extras, i.e. O(n) on the same list.
             *
             * Signature copied 1:1 from
             *   extern/CommonLibSSE-NG/include/RE/I/InventoryChanges.h:45
             *     std::uint32_t GetWornMask();
             * Returned in eax as a 32-bit value.
             */
            struct GetWornMaskHook {
                    static std::uint32_t hook(RE::InventoryChanges* a_this) {
                        SSE_ZONE("InventoryChanges::GetWornMask");
                        return orig(a_this);
                    }

                    static inline std::string logName = "GetWornMask";
                    using FuncType = decltype(&hook);
                    static inline FuncType orig;
                    static inline REL::RelocationID srcFunc = REL::RelocationID{15806, 16044};  // (SE, AE)

                    static void install() {
                        if constexpr (BUILDOPTIONS.detoursFound) {
                            Hooking::writeDetour<GetWornMaskHook>();
                        } else {
                            logger::warn("{}: detours unavailable, using SKSE trampoline entry hook", logName);
                            Hooking::writeBranch<GetWornMaskHook>();
                        }
                    }
            };

            /*
             * Target #8: InventoryEntryData::GetEnchantment() const
             *   SE 15788 / AE 16026 (inventory-system-analysis.md section 6.2)
             *   Second representative of the inner extraLists walk (#5 is the
             *   first). Together they show whether the m-loop is dominated by one
             *   accessor or spread over several.
             *
             * Signature copied 1:1 from
             *   extern/CommonLibSSE-NG/include/RE/I/InventoryEntryData.h:38
             *     [[nodiscard]] EnchantmentItem* GetEnchantment() const;
             * Same const-qualification note as #5: the hook takes a non-const
             * pointer because `this` is passed in rcx either way.
             */
            struct GetEnchantmentHook {
                    static RE::EnchantmentItem* hook(RE::InventoryEntryData* a_this) {
                        SSE_ZONE("InventoryEntryData::GetEnchantment");
                        return orig(a_this);
                    }

                    static inline std::string logName = "GetEnchantment";
                    using FuncType = decltype(&hook);
                    static inline FuncType orig;
                    static inline REL::RelocationID srcFunc = REL::RelocationID{15788, 16026};  // (SE, AE)

                    static void install() {
                        if constexpr (BUILDOPTIONS.detoursFound) {
                            Hooking::writeDetour<GetEnchantmentHook>();
                        } else {
                            logger::warn("{}: detours unavailable, using SKSE trampoline entry hook", logName);
                            Hooking::writeBranch<GetEnchantmentHook>();
                        }
                    }
            };

            /*
             * Target 2.1 (phase 2): the engine's "RequestItemCardInfo" GFx
             * delegate -- the function skyui calls once per item from
             * ItemcardDataExtender.processList() (inventory-system-analysis.md
             * section 7.4), i.e. ~27211 times per InventoryMenu open, and the
             * prime suspect for the ~1.5 s skyui-only surcharge.
             *
             * THIS HOOK HAS NO RELOCATION ID, and that is deliberate rather than
             * an omission. The engine registers it as a named callback in the
             * InventoryMenu FxDelegate, so it is reachable at runtime through
             *   IMenu::fxDelegate                   (IMenu.h:113, offset 0x28)
             *     -> FxDelegate::callbacks          (FxDelegate.h:50, offset 0x18,
             *                                         GHash<GString, CallbackDefn>)
             *       -> CallbackDefn::callback       (FxDelegate.h:24)
             * Nothing in the address library exposes that pointer, and resolving
             * it dynamically is strictly better anyway: there is no (SE, AE)
             * pair to keep correct.
             *
             * Signature copied 1:1 from CommonLib:
             *   extern/CommonLibSSE-NG/include/RE/F/FxDelegateHandler.h:16
             *     using CallbackFn = void(const FxDelegateArgs& a_params);
             * Note what is NOT there: no `this`. CallbackFn is a PLAIN function
             * pointer (FxDelegate.cpp:23 calls it as `cbDef->callback(params)`),
             * so the menu instance travels inside the arguments
             * (a_params.GetHandler()) and must not be added as a first
             * parameter here. It is also NOT a pointer-to-member-function.
             */
            struct RequestItemCardInfoHook {
                    static void hook(const RE::FxDelegateArgs& a_params) {
                        // Phase 4 / S-1, both halves, before the zone so their
                        // one-off cost is not charged to the zone they exist to
                        // help interpret. Order matters: the argument dump runs
                        // first so its report cannot be crowded out of the probe's
                        // log budget. Both are read-only and both no-op after the
                        // first call.
                        //
                        // Deliberately NOT on the menu-open/close event: this call
                        // is the first point where the movie is loaded AND the
                        // list is fully built (see installAsPathProbe).
                        probeDelegateArgs(a_params);
                        installAsPathProbe(a_params.GetHandler());
                        // Phase 4 / steps S0a + S0b, third one-shot on this
                        // trigger: keep the pass-through probes on
                        // `_dataProcessors[0]` in place. Placed after the two S-1
                        // probes so their logs cannot be crowded out by this one.
                        // This is a single flag read per item -- the GFxValue graph
                        // work happens once per menu open, armed by
                        // ProfilingHooks::requestAsProbeInstall() from the
                        // InventoryMenu "opening" event.
                        maybeInstallAsProbe(a_params.GetHandler());
                        SSE_ZONE("RequestItemCardInfo");
                        return orig(a_params);
                    }

                    static inline std::string logName = "RequestItemCardInfo";
                    using FuncType = decltype(&hook);
                    // Assigned at install time from the FxDelegate table, not from
                    // a REL::Relocation -- see installRequestItemCardInfoHook().
                    static inline FuncType orig;
            };

            /*
             * Target 2.5 (phase 2): the GFx delegate boundary itself.
             *
             * WHY THIS EXISTS. Capture #8 (docs/tracy-capture-log.md) showed that
             * the C++ body of RequestItemCardInfo -- target 2.1 -- accounts for
             * only ~55 % of the skyui-only surcharge (~0.6 s of ~1.1 s); the
             * remaining ~0.5 s appears in no zone the engine exposes. This hook
             * splits that remainder, because the two zones nest:
             *
             *     FxDelegate::Callback[RequestItemCardInfo]   <- this hook
             *       +- RequestItemCardInfo                    <- target 2.1
             *
             * The difference between the two totals is what the AS <-> C++ boundary
             * itself costs (argument marshalling, the GFxValue array it passes).
             * Whatever is still unaccounted for after that is ActionScript: skyui's
             * processList() loop and its per-open updateItemInfo callbacks, which
             * live inside the SWF and are invisible to Tracy.
             *
             * HOW IT IS REACHED. FxDelegate derives from GFxExternalInterface
             * (FxDelegate.h:14), whose vtable starts
             *     [0] deleting destructor      GRefCountImplCore.h:10
             *     [1] Callback                 GFxExternalInterface.h:22
             * and FxDelegate overrides slot 1 (FxDelegate.h:41). Two consequences
             * matter here:
             *
             *   - the slot NUMBER is a property of the CommonLib class hierarchy
             *     above, not of any game build, so it needs no (SE, AE) pair and
             *     cannot go stale. The vtable ADDRESS does need a pair, and
             *     VTABLE_FxDelegate (Offsets_VTABLE.h:6150) supplies it;
             *   - all instances of a class share one vtable, so patching that slot
             *     covers every menu's delegate at once, with no per-instance
             *     bookkeeping -- and it can be done at load time, unlike 2.1, which
             *     must wait for a menu instance to exist.
             *
             * Patching the slot is deliberately preferred over Detours-attaching
             * the function body: Callback is virtual, so Scaleform can only reach
             * it through a vtable anyway, and this way target 2.5 still works in
             * builds without the detours port (BUILDOPTIONS.detoursFound == false),
             * where installRequestItemCardInfoHook() can only log a warning.
             *
             * FILTERING IS MANDATORY, not an optimization. This entry point carries
             * every delegate call of every menu; an unfiltered zone would be
             * swamped by unrelated traffic and its nesting would make the capture
             * unreadable. `a_methodName` is a NUL-terminated C string owned by
             * Scaleform (not a GString), so a plain strcmp is correct -- and being
             * allocation-free matters, because this runs once per item.
             */
            struct FxDelegateCallbackHook {
                    // FxDelegate overrides GFxExternalInterface's only pure virtual;
                    // see the comment above for why 1 is derived, not guessed.
                    static constexpr std::size_t kCallbackVtableSlot = 1;

                    static void hook(RE::FxDelegate* a_this, RE::GFxMovieView* a_movieView, const char* a_methodName,
                                     const RE::GFxValue* a_args, std::uint32_t a_argCount) {
                        if (a_methodName && std::strcmp(a_methodName, kItemCardMethodName) == 0) {
                            SSE_ZONE("FxDelegate::Callback[RequestItemCardInfo]");
                            orig(a_this, a_movieView, a_methodName, a_args, a_argCount);
                            return;
                        }
                        // Every other menu / method pays one strcmp and nothing else.
                        orig(a_this, a_movieView, a_methodName, a_args, a_argCount);
                    }

                    static inline std::string logName = "FxDelegate::Callback";
                    using FuncType = decltype(&hook);
                    // What the vtable slot held before we replaced it, captured at
                    // install time so this hook chains with whatever was there.
                    static inline FuncType orig;
            };

            /*
             * Still deferred (see section 4.2 of the plan):
             *   #4  InventoryChanges::SendContainerChangedEvent(...)  15909 / 16149 (verify!)
             *   #9  ActorEquipManager::EquipObject(...)               37938 / 38894
             *  #10  ActorEquipManager::UnequipObject(...)             37945 / 38901
             *
             * #4 must NOT be hooked before the AE binary has been disassembled:
             * the AE values of SendContainerChangedEvent / SetUniqueID /
             * TransferItemUID are all annotated 16149 upstream
             * (extern/CommonLibSSE-NG/include/RE/Offsets.h:339-341), and three
             * different functions cannot share a single address.
             * #9/#10 are write-side, are the lowest-priority targets, and add no
             * evidence that acceptance questions 1..7 need.
             */
    };
}  // namespace plugin
