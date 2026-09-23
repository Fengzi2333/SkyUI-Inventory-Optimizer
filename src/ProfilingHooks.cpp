#include "ProfilingHooks.h"

#include "ProcessEntryReplica.h"

#include <RE/F/FxDelegate.h>
#include <RE/G/GFxMovieView.h>
#include <RE/G/GFxValue.h>
#include <RE/I/InventoryMenu.h>
#include <RE/M/MenuOpenCloseEvent.h>
#include <RE/U/UI.h>

namespace plugin {
#ifdef TRACY_ENABLE
    /*
     * -------------------------------------------------------------------------
     * Phase 4 / step S-1 helpers: READ-ONLY inspection of the inventory movie's
     * ActionScript object graph.
     *
     * Design: docs/phase4-design.md sections 4.3 (path discovery) and 8.1.
     *
     * The safety property is structural, not a matter of discipline: nothing in
     * this namespace may call SetMember / SetVariable / Invoke. Everything goes
     * through GetVariable / GetMember / HasMember / VisitMembers, which only
     * read. A failed probe therefore degrades to "nothing logged", never to
     * "menu broken" -- which is what lets S-1 run on a live save.
     * -------------------------------------------------------------------------
     */
    namespace s1 {
        // Entry objects are numerous (one per item), so an unbounded dump of the
        // root's members would both take long and bury the answer we are after.
        constexpr std::size_t kMaxMembersPerObject = 32;
        // Belt-and-braces: the probe is one-shot anyway, but a cap makes an
        // unexpectedly huge object graph cost a truncated log instead of a stall.
        constexpr std::size_t kMaxLogLines = 80;

        inline std::size_t g_logLines = 0;

        inline bool canLog() {
            if (g_logLines >= kMaxLogLines) {
                return false;
            }
            ++g_logLines;
            return true;
        }

        inline const char* typeName(const RE::GFxValue& a_val) {
            if (a_val.IsString()) return "string";
            if (a_val.IsNumber()) return "number";
            if (a_val.IsBool()) return "bool";
            if (a_val.IsArray()) return "array";
            if (a_val.IsObject()) return "object";
            if (a_val.IsNull()) return "null";
            if (a_val.IsUndefined()) return "undefined";
            return "other";
        }

        // One-line description: the type, plus whatever makes the value
        // identifiable at a glance. Deliberately no recursion -- a nested dump
        // would be unbounded.
        inline std::string describe(const RE::GFxValue& a_val) {
            if (a_val.IsArray()) {
                return "array[" + std::to_string(a_val.GetArraySize()) + "]";
            }
            if (a_val.IsString()) {
                const char* s = a_val.GetString();
                return std::string("string \"") + (s ? s : "") + "\"";
            }
            if (a_val.IsNumber()) {
                return "number " + std::to_string(a_val.GetNumber());
            }
            return typeName(a_val);
        }

        /*
         * The object phase 4 actually needs: a list owning both the entry array
         * and the data-processor array.
         *
         * The two members must be probed DIFFERENTLY, and mixing them up yields a
         * silent false negative:
         *   - `_dataProcessors` is a plain var   (BasicList.as:41)  -> HasMember
         *   - `entryList`       is a GETTER      (BSList.as:15)     -> GetMember,
         *     because HasMember does not report accessor-only properties
         *     reliably.
         */
        inline bool looksLikeInventoryList(const RE::GFxValue& a_val) {
            if (!a_val.IsObject() || !a_val.HasMember("_dataProcessors")) {
                return false;
            }
            RE::GFxValue entries;
            return a_val.GetMember("entryList", &entries) && entries.IsArray();
        }

        /*
         * Walks a dotted member path from an already-resolved object.
         *
         * Needed because the engine hands us the menu clip as a GFxValue
         * (InventoryMenu::RUNTIME_DATA::root, documented as "_level0.Menu_mc"),
         * so the graph phase 4 cares about hangs off that handle and no absolute
         * GetVariable("_root...") string has to be guessed. Returns false at the
         * first missing segment, so a wrong guess just means "this candidate did
         * not match" and the caller moves on -- there is nothing to unwind.
         */
        inline bool resolvePath(const RE::GFxValue& a_start, const char* a_path, RE::GFxValue& a_out) {
            if (!a_start.IsObject() || !a_path) {
                return false;
            }
            RE::GFxValue current = a_start;
            const char* segment = a_path;
            for (;;) {
                const char* dot = std::strchr(segment, '.');
                const std::string name(segment, dot ? static_cast<std::size_t>(dot - segment) : std::strlen(segment));
                if (name.empty()) {
                    return false;
                }
                RE::GFxValue next;
                if (!current.GetMember(name.c_str(), &next)) {
                    // A missing plain member and an accessor that refuses to run
                    // both land here; either way this candidate is not the one.
                    return false;
                }
                current = next;
                if (!dot) {
                    break;
                }
                segment = dot + 1;
            }
            a_out = current;
            return true;
        }

        // Enumerates an object's members. This is the fallback that keeps a failed
        // path guess diagnosable from the log alone: if none of the candidate
        // paths match, the member list still shows where `inventoryLists` lives.
        inline void dumpMembers(const RE::GFxValue& a_obj, const char* a_prefix) {
            if (!a_obj.IsObject()) {
                return;
            }
            std::size_t seen = 0;
            a_obj.VisitMembers([&](const char* a_name, const RE::GFxValue& a_val) {
                if (seen >= kMaxMembersPerObject || !canLog()) {
                    return;
                }
                ++seen;
                logger::info("    {}.{} = {}", a_prefix, a_name ? a_name : "?", describe(a_val));
            });
        }

        /*
         * Reports which `_dataProcessors` slot is an ItemcardDataExtender
         * subclass, i.e. the object phase 4 would hot-swap. That class is
         * identified by carrying the three members the design targets:
         *   ItemcardDataExtender.as:40  processList
         *   ItemcardDataExtender.as:22  _requestItemInfo
         *   ItemcardDataExtender.as:63  processEntry
         *
         * WHY ALL THREE. The first landing of this probe keyed on
         * processList + processEntry alone and mislabelled two of the three
         * slots as the target; the second real-machine run (docs/phase4-design.md
         * section 10.4) showed why that is too loose:
         *
         *   [0] InventoryDataSetter   (InventoryMenu.as:74) all three members
         *   [1] InventoryIconSetter   (InventoryMenu.as:75) no _requestItemInfo
         *   [2] PropertyDataExtender  (InventoryMenu.as:76) no _requestItemInfo
         *
         * [1] and [2] are IListProcessor implementations in their own right, so
         * they legitimately carry processList/processEntry while having nothing
         * to do with the item-card round trip. `_requestItemInfo` is what makes
         * slot 0 the unique answer -- and the log confirmed it, rather than the
         * expectation (InventoryMenu.as:74 registers InventoryDataSetter first)
         * deciding it.
         */
        inline void dumpProcessors(const RE::GFxValue& a_list) {
            RE::GFxValue processors;
            if (!a_list.GetMember("_dataProcessors", &processors) || !processors.IsArray()) {
                logger::warn("S-1: `_dataProcessors` is not an array");
                return;
            }

            const auto count = processors.GetArraySize();
            logger::info("S-1: _dataProcessors = array[{}]", count);
            for (std::uint32_t i = 0; i < count && canLog(); ++i) {
                RE::GFxValue proc;
                if (!processors.GetElement(i, &proc) || !proc.IsObject()) {
                    continue;
                }
                const bool hasProcessList = proc.HasMember("processList");
                const bool hasProcessEntry = proc.HasMember("processEntry");
                const bool hasRequestInfo = proc.HasMember("_requestItemInfo");
                // All three, not just the two list-lifecycle members: slots [1] and
                // [2] carry processList/processEntry while being unrelated to the
                // item-card round trip (see the header comment above).
                const bool isTarget = hasProcessList && hasProcessEntry && hasRequestInfo;
                logger::info("      [{}] processList={} processEntry={} _requestItemInfo={}{}", i, hasProcessList ? "yes" : "no",
                             hasProcessEntry ? "yes" : "no", hasRequestInfo ? "yes" : "no",
                             isTarget ? "   <== ItemcardDataExtender: PHASE 4 TARGET" : "");
            }
        }
    }  // namespace s1

    /*
     * -------------------------------------------------------------------------
     * Phase 4 / step S0a (probe 1) and S0b (probes 2 and 3): pass-through
     * instrumentation of the three members skyui actually drives on
     * `_dataProcessors[0]`.
     *
     * Design: docs/phase4-design.md sections 4.2 (probe set and the nesting
     * equations), 8.1 (step table), 8.2 (revert contract) and 10.5 (the S0a
     * real-machine verdict); coordinates in section 10.4 (5).
     *
     * Scope, deliberately narrow: WRAP AND FORWARD ONLY. Nothing is cached and
     * nothing is optimized, so what this produces is a baseline, not a result.
     * With all three probes in place one round of the list decomposes as
     *
     *     AS::processList                 once per round  -> S3 + S1+S2 + S4
     *       +- AS::_requestItemInfo       once per item   -> S1 + S2, and
     *       |    +- RequestItemCardInfo   (target 2.1)       contains that trip
     *       +- AS::processEntry           once per item   -> S4
     *
     *     S1 + S2 = AS::_requestItemInfo - (the RequestItemCardInfo nested in it)
     *     S4      = AS::processEntry
     *     S3      = AS::processList - Sum(AS::_requestItemInfo) - Sum(AS::processEntry)
     *
     * and probe 1 additionally settles hypothesis H2: a GFxValue produced by
     * GFxMovie::CreateFunction has to be callable from ActionScript through
     * `apply`, because that is how skyui calls that very member
     * (ItemcardDataExtender.as:55). If it could not be, phases 4a and 4b would
     * both be unreachable and the plan would fall back to 3a'. H2 is now SETTLED
     * -- measured on a real machine, not inferred (section 10.5 (6)).
     *
     * ALL THREE PROBES GO IN TOGETHER, and that is a correctness requirement
     * rather than a convenience. S3 is a DIFFERENCE of three totals, so a partial
     * install does not yield a smaller answer, it yields an uninterpretable one:
     * a missing processEntry zone would silently add S4 to S3. The install is
     * therefore all-or-nothing, which is also what lets section 10.5 (3) keep its
     * promise -- either every probe is in place, or the processor is byte-for-byte
     * what it was.
     *
     * WHAT EACH PROBE MAY AND MAY NOT PROMISE. Probe 1 is validated end to end on
     * a real machine (section 10.5). Probes 2 and 3 reuse the same
     * CreateFunction + Invoke("call", ...) mechanism and introduce no new API --
     * they only wrap two members that are reached as ordinary AS2 member calls
     * rather than through `apply`:
     *
     *     BasicList.as:276             _dataProcessors[i].processList(this)
     *     ItemcardDataExtender.as:56   processEntry(e, _itemInfo)   ==  this.processEntry(...)
     *
     * but they have NOT been measured yet. Hence the per-probe first-call dump:
     * probe 1's `apply` target is the LIST, while these two are called ON the
     * processor, so `thisPtr` differs between them and is confirmed from the log
     * instead of assumed.
     *
     * The consistency check that keeps the trace trustworthy is the one from
     * section 4.2: the full-path RequestItemCardInfo count must still equal the
     * AS::processEntry count. This is a RELATIVE criterion -- the two counts must
     * be strictly equal within one capture -- and not a hardcoded number: absolute
     * counts move with the save file and with any scrolling or hovering, which is
     * why such a capture has to be a clean open/close run.
     *
     * Unlike s1 above, this namespace DOES write into the movie -- that is the
     * point -- which is why every step below is individually guarded and why a
     * failure leaves the target untouched.
     * -------------------------------------------------------------------------
     */
    namespace s0 {
        /*
         * The members to wrap, in the order they are wrapped. Two properties of
         * this table are load-bearing:
         *
         *   - `_requestItemInfo` comes first. It is the DISCRIMINATOR that makes
         *     slot 0 the unique target: processList / processEntry are generic
         *     IListProcessor contract members and match all three slots, while
         *     `_requestItemInfo` matches only InventoryDataSetter (section 10.4
         *     (2)). resolveTargetProcessor still tests it before anything is
         *     written.
         *
         *   - install() walks this table in a loop, so "all three or none" is a
         *     property of the code's shape rather than of three parallel blocks
         *     that could drift apart.
         *
         * The Tracy zone names deliberately do NOT live here: a zone name has to
         * be a compile-time literal at its SSE_ZONE site (Profiling.h:32-35), so
         * it belongs to the handler class, not to a runtime table.
         */
        enum Slot : std::size_t {
            kSlotRequestItemInfo = 0,
            kSlotProcessList,
            kSlotProcessEntry,
            kSlotCount
        };

        inline constexpr const char* kProbeMembers[kSlotCount] = {
            "_requestItemInfo",
            "processList",
            "processEntry",
        };

        // Kept under its own name because it is read in a second place as well:
        // resolveTargetProcessor uses it as the discriminator test.
        constexpr const char* kProbeMember = kProbeMembers[kSlotRequestItemInfo];

        // Measured rather than assumed: slot 0 is InventoryDataSetter, registered
        // first by InventoryMenu.as:74, and the second real-machine run confirmed
        // it is the only slot carrying `_requestItemInfo`.
        constexpr std::uint32_t kTargetProcessorSlot = 0;

        // A parameter dump longer than this is a layout surprise, not data worth
        // thousands of log lines.
        constexpr std::uint32_t kMaxArgsLogged = 8;

        // The only legitimate reason for install() to fail is "the list is not
        // complete yet", which resolves within the first few items of a round. An
        // unbounded retry would both flood the log and turn a genuine resolution
        // failure into thousands of wasted GFx calls -- i.e. exactly the kind of
        // measurement pollution this probe exists to avoid.
        constexpr std::uint32_t kMaxInstallAttempts = 4;

        /*
         * Armed by the InventoryMenu "opening" event (requestAsProbeInstall) and
         * consumed by the first RequestItemCardInfo call of that round.
         *
         * This split is what keeps the probes off the hot path: the flag is tested
         * once per item (32242 times in capture #11), while the GetMember /
         * SetMember graph work runs once per round (5 times). Re-arming per open is
         * required, not merely convenient -- the list object and every processor
         * inside it are rebuilt on each open, so a round-1 replacement no longer
         * exists in round 2.
         */
        inline bool g_installPending = false;
        inline std::uint32_t g_installAttempts = 0;

        // One first-call report per probe, and one per process rather than one per
        // round: the shape of the arguments is a property of the call site, which
        // cannot change between rounds, and 5 rounds x 3 probes of dumps would bury
        // everything else in the log.
        inline std::array<bool, kSlotCount> g_probeReported{};

        inline bool claimFirstReport(std::size_t a_slot) {
            if (g_probeReported[a_slot]) {
                return false;
            }
            g_probeReported[a_slot] = true;
            return true;
        }

        /*
         * One-shot dump of what ActionScript actually handed a probe.
         *
         * For probe 1 this is the H2 evidence, and simultaneously the first
         * real-machine check of the "three hosts" reading in section 5.1: the AS2
         * statement
         *
         *     _requestItemInfo.apply(a_list, [this, i])   ItemcardDataExtender.as:55
         *
         * has to arrive here as
         *
         *     thisPtr = a_list           (TabularList -- the host of _selectedIndex)
         *     args    = [processor, i]   (the call site's `this`, then the index)
         *
         * If `thisPtr` were the processor instead, the whole `_selectedIndex`
         * analysis would be misattributed -- which is exactly the mistake the old
         * pseudocode made (section 5.1, warning block).
         *
         * The same distinction is why probes 2 and 3 get a dump at all. Their call
         * sites pass no `apply` thisArg, so `thisPtr` must come back as the
         * PROCESSOR and `args` must NOT carry it -- the mirror image of probe 1.
         * Whether that holds is a measurement, not a reading: `thisPtr` decides
         * which object's state each zone's cost is attributed to.
         */
        inline void reportFirstCall(std::size_t a_slot, const char* a_callSite, const char* a_expectation,
                                    const RE::GFxFunctionHandler::Params& a_params) {
            logger::info("=== phase 4 / S0: `{}` probe received its first call ===", kProbeMembers[a_slot]);
            logger::info("S0: skyui's call site is `{}`; expected -> {}", a_callSite, a_expectation);
            if (a_params.thisPtr) {
                logger::info("S0: thisPtr  = {}", s1::describe(*a_params.thisPtr));
            } else {
                logger::info("S0: thisPtr  = null (no thisArg reached the probe)");
            }
            logger::info("S0: argCount = {}", a_params.argCount);
            for (std::uint32_t i = 0; a_params.args && i < a_params.argCount && i < kMaxArgsLogged; ++i) {
                logger::info("      args[{}]  = {}", i, s1::describe(a_params.args[i]));
            }
        }

        /*
         * The forwarding itself, shared by all three probes.
         *
         * Forward verbatim -- the three semantics section 4.2 lists. `call` is
         * Function.prototype.call and argsWithThisRef is exactly
         * [thisPtr, args...], so this single Invoke reproduces the `this` binding,
         * the argument list, and (through retVal) the return value. Nothing else in
         * a probe may touch the movie: no caching, no state.
         *
         * Why one mechanism covers all three members, `apply` or not: SetMember
         * installs the probe as an OWN property of the processor, and AS2 property
         * lookup consults own properties BEFORE the prototype chain. `processList`
         * and `processEntry` are prototype methods, so the probe shadows them for
         * that one instance -- exactly the scope we want, and the reason the whole
         * probe is per-processor instead of per-class.
         *
         * Keep the three call sites in mind while reading the handlers below:
         * probe 1's thisPtr is the LIST, probes 2 and 3's is the PROCESSOR.
         *
         * `a_original` is a non-const reference because GFxValue::Invoke is a
         * non-const member (GFxValue.h). Nothing in the probe mutates it: the
         * reference is simply what the API requires.
         */
        inline void forwardVerbatim(RE::GFxValue& a_original, RE::GFxFunctionHandler::Params& a_params) {
            a_original.Invoke("call", a_params.retVal, a_params.argsWithThisRef,
                              static_cast<RE::UPInt>(a_params.argCount) + 1);
        }

        /*
         * The C++ bodies that stand in for the three members. One small class per
         * member rather than a template: a Tracy zone name has to be a compile-time
         * literal at its SSE_ZONE site (Profiling.h:32-35), and three classes say
         * that more directly than a string-literal non-type template parameter.
         *
         * Two properties of every one of them are load-bearing:
         *
         *   - It holds a GFxValue COPY of the original member, not a raw pointer.
         *     GFxValue is reference counted (GFxValue.h:288-300), so a pointer
         *     cache would dangle the moment its movie is torn down. The copy is
         *     also the revert path section 8.2 requires: a later S1 can restore it
         *     verbatim.
         *
         *   - It is deliberately never Release()d, and a fresh handler is
         *     allocated per install. GRefCountImplCore starts _refCount at 1
         *     (GRefCountImplCore.h:18), i.e. the `new` already denotes one owning
         *     reference, and Scaleform's CreateFunction contract adds its own.
         *     Releasing ours is correct only if that contract holds; NOT releasing
         *     is the choice that is safe under either assumption, and the worst
         *     outcome (one ~48 byte object per probe per round stays alive for the
         *     lifetime of a profiling build) is not worth risking a use-after-free
         *     over. Sharing one handler across rounds would cap that at one object
         *     per probe and was rejected for a different reason: it would expose
         *     `_original` as mutable state to the previous round's function object.
         */

        // Zone `AS::_requestItemInfo` = S1 + S2 for one item, with the target 2.1
        // RequestItemCardInfo round trip nested inside it.
        class RequestItemInfoHandler final : public RE::GFxFunctionHandler {
            public:
                explicit RequestItemInfoHandler(const RE::GFxValue& a_original) :
                    _original(a_original) {}

                void Call(Params& a_params) override {
                    SSE_ZONE("AS::_requestItemInfo");
                    if (claimFirstReport(kSlotRequestItemInfo)) {
                        reportFirstCall(kSlotRequestItemInfo, "_requestItemInfo.apply(a_list, [this, i])",
                                        "thisPtr = list, args = [processor, index], argCount = 2", a_params);
                    }
                    forwardVerbatim(_original, a_params);
                }

            private:
                RE::GFxValue _original;
        };

        // Zone `AS::processList` = the whole per-round loop, S3 included. Nesting is
        // the entire point: S3 exists only as processList minus the two per-item
        // sums, so this zone on its own is not a result.
        class ProcessListHandler final : public RE::GFxFunctionHandler {
            public:
                explicit ProcessListHandler(const RE::GFxValue& a_original) :
                    _original(a_original) {}

                void Call(Params& a_params) override {
                    SSE_ZONE("AS::processList");
                    if (claimFirstReport(kSlotProcessList)) {
                        reportFirstCall(kSlotProcessList, "_dataProcessors[i].processList(this)",
                                        "thisPtr = processor, args = [list], argCount = 1", a_params);
                    }
                    forwardVerbatim(_original, a_params);
                }

            private:
                RE::GFxValue _original;
        };

        /* =====================================================================
         * Phase 4b / pre-4b-ii: the `processEntry` OUTLIER probe.
         *
         * The reading this exists to settle. The S0b baseline (capture #11b,
         * design section 10.7) carries a 14.59 ms `processEntry` outlier against
         * a 47.1 us mean -- 310x -- and the first reading of it was "some
         * formType branch is very expensive on a few items". Re-reading that
         * trace, and then capture #12, says otherwise:
         *
         *   - the >5 ms events are EXACTLY TWO PER ROUND, in every round;
         *   - they sit at roughly the same two positions every round, one near
         *     the start (~#35-50) and one near the end (~#6232-6242 of 6441),
         *     but they DRIFT by ~15 items within one session;
         *   - their cost DECREASES across rounds (#11b: 14.6 -> 9.6 ms);
         *   - they survive into cache mode (#12: 5.1-9.5 ms), where no card is
         *     allocated at all -- so they are driven by the entry objects and the
         *     list rebuild, not by the card path.
         *
         * A data-dependent branch is the one reading that fits none of those: the
         * same item would be expensive at the same index, at a stable cost. The
         * shape instead matches a deterministic allocation threshold tripping an
         * ActionScript heap mark-sweep, i.e. a PAUSE rather than code cost.
         *
         * Why it is worth a rebuild before 4b-ii: the two readings imply different
         * plans. If formType VARIES across the outliers, a slice of
         * S4 = 303.3 ms/round is a pause that C++-izing processEntry cannot
         * remove (only fewer AS2 allocations can shrink it), so 4b-ii's ceiling is
         * below 303.3. If formType is CONSTANT, it really is one expensive branch,
         * 4b-ii does remove it, and that branch is where its prototype should
         * start. This probe does not decide the plan, it prices it.
         *
         * COST, stated because it runs once per item: two
         * `std::chrono::steady_clock::now()` calls plus a compare, i.e. ~40-60 ns
         * per item, ~0.3 ms per 6441-item round against a ~256 ms/round
         * `processEntry` total -- +0.1 %, an order of magnitude below the 3-6 %
         * cross-session spread already documented in section 10.8 (10). The member
         * reads happen ONLY when the threshold is hit (~2 per round), so nothing
         * is read out of the movie on the steady path.
         *
         * `g_processEntrySeq` is the round-local call ordinal: the trace's own
         * item number, made explicit. reportAndResetRoundStats resets it, and that
         * runs once per round on the round's first RequestItemCardInfo, i.e.
         * BEFORE its first `processEntry`, so it reads index+1 within a round. That
         * it is a proxy rather than the real `_selectedIndex` is deliberate:
         * `_selectedIndex` lives on the LIST while this probe's `thisPtr` is the
         * PROCESSOR (section 10.7 (1)), so the list would have to be re-resolved
         * just to log a number the call ordering already implies.
         * ===================================================================== */
        /*
         * The switch for both diagnostics, and it follows section 8.2 rule 3's
         * pattern exactly as `kAnswerFromCache` does: they are COMPILE-TIME optional,
         * so a capture that does not need them -- or a later revert -- compiles back
         * into the capture #12 build instead of leaving a permanently slower one
         * behind. A runtime toggle would not do: the point of `if constexpr` here is
         * that `false` removes the two clock reads and the histogram increments from
         * the per-item paths entirely, rather than turning them into a branch.
         *
         * ON for capture #13, because a capture taken with it off collects neither
         * number -- this build is the one that gets deployed, so ON is the state that
         * has to be in the artifact. Flipping it to false reproduces capture #12.
         */
        constexpr bool kExtraDiagnostics = true;

        constexpr std::int64_t  kProcessEntryOutlierMs = 5;
        constexpr std::uint32_t kMaxProcessEntryOutliersLogged = 20;

        inline std::uint32_t g_processEntrySeq = 0;
        inline std::uint32_t g_processEntryOutliersLogged = 0;

        /*
         * Numeric member read that reports absence instead of inventing a 0 --
         * `formType` / `formId` / `baseId` are all absent on an object that is not
         * the entry this probe expects, and a printed 0 would be a plausible-looking
         * lie about exactly the question being asked.
         */
        inline bool readNumberMember(const RE::GFxValue& a_object, const char* a_name, double& a_out) {
            RE::GFxValue value;
            if (!a_object.GetMember(a_name, &value) || !value.IsNumber()) {
                return false;
            }
            a_out = value.GetNumber();
            return true;
        }

        /*
         * One line per outlier, capped at kMaxProcessEntryOutliersLogged for the
         * whole process: ~2 per round is 10 lines in a 5-round capture, and the cap
         * is what keeps a threshold that turns out to be wrong (e.g. this machine is
         * simply slower) from burying the log.
         *
         * All three numbers are logged as doubles with -1 for "member absent", so
         * the format string stays a plain `{}` list and no `{:.3f}`-style specifier
         * has to be introduced -- this codebase uses none anywhere, and fmt version
         * differences are not worth tripping over for a diagnostic line.
         *
         * `baseId` is written BY processEntry (`a_entryObject.formId & 0xFFFFFF`,
         * InventoryDataSetter.as:26), so reading it here -- after the forward --
         * turns the line into the item's identity rather than just its type.
         */
        inline void logProcessEntryOutlier(const RE::GFxFunctionHandler::Params& a_params, double a_elapsedMs) {
            if (++g_processEntryOutliersLogged > kMaxProcessEntryOutliersLogged) {
                return;
            }

            // args[0] is the entry object -- S0b measured thisPtr = processor,
            // args = [entry, itemInfo], argCount = 2 (design section 10.7 (1)).
            const RE::GFxValue* entry =
                (a_params.args && a_params.argCount > 0 && a_params.args[0].IsObject()) ? &a_params.args[0] : nullptr;

            double     formType = 0.0;
            double     formId = 0.0;
            double     baseId = 0.0;
            const bool haveFormType = entry && readNumberMember(*entry, "formType", formType);
            const bool haveFormId = entry && readNumberMember(*entry, "formId", formId);
            const bool haveBaseId = entry && readNumberMember(*entry, "baseId", baseId);

            logger::warn(
                "4b/outlier: processEntry #{} of this round took {} ms -- formType {}, formId {}, baseId {} (-1 = member "
                "absent; varying formType = heap pause, constant formType = expensive branch)",
                g_processEntrySeq, a_elapsedMs, haveFormType ? formType : -1.0, haveFormId ? formId : -1.0,
                haveBaseId ? baseId : -1.0);
        }

        /*
         * =====================================================================
         * Phase 4b / step S4: the `processEntry` REPLACEMENT.
         *
         * Design: docs/phase4-design.md section 6 (4b-ii). Implementation record:
         * section 10.9.
         *
         * WHY THIS IS NOT A CACHE, unlike slot 0's replacement. `processEntry` is a
         * pure function of the entry object and the engine's card, and both are handed
         * to it fresh on every call -- so there is nothing to remember and no
         * staleness contract to keep. What the replica removes is the AS2 interpreter:
         * the same member reads and the same assignments, executed by compiled code
         * (src/ProcessEntryReplica.cpp, a line-by-line transcription of
         * InventoryDataSetter.as:24-878).
         *
         * WHAT IT CANNOT REMOVE. `Translator.translate` asks the GAME's localization
         * system for a string, so the 103 literals are resolved once per process by
         * calling the AS function and cached; see the block comment on the table in
         * ProcessEntryReplica.cpp. If that resolution fails, the replica declines every
         * item and this stays a pure forwarder -- the worst case is "no change".
         *
         * ROUND 1 PAYS IN FULL, and the hero number is round-over-round: the first
         * round still runs the AS body for the first `kValidationSample` items (the
         * shadow sample), and S4's verdict is the comparison against capture #11b's
         * ~256 ms/round, not a per-round absolute. Same accounting as 4a.
         *
         * The switch: `SSE_REPLICATE_PROCESS_ENTRY` (defined in
         * ProcessEntryReplica.h, and the ONLY place it is defined). It is a
         * preprocessor macro rather than a `constexpr bool` like
         * `kAnswerFromCache` because this step adds a translation unit: with it at 0
         * that file compiles to nothing and everything below that names `pe::` is
         * gone, so the revert artifact is the previous build rather than a build that
         * merely behaves like it.
         *
         * `kReplicateProcessEntry` mirrors the macro for the log lines and for
         * readability; the code that must not compile out is guarded by the macro
         * itself, not by this constant.
         *
         * REVERT (section 8.2 rule 1): set the macro to 0 and rebuild. Nothing else
         * has to change -- the wrapper, the zone name and the section 4.2 count
         * relation are shared with the pure-forwarder build.
         * =====================================================================
         */
        constexpr bool kReplicateProcessEntry = SSE_REPLICATE_PROCESS_ENTRY != 0;

        // Zone `AS::processEntry` = S4 for one item (~855 lines of AS in the real
        // InventoryDataSetter), and the anchor of the section 4.2 self-check: its
        // count must equal the full-path RequestItemCardInfo count.
        class ProcessEntryHandler final : public RE::GFxFunctionHandler {
            public:
                explicit ProcessEntryHandler(const RE::GFxValue& a_original) :
                    _original(a_original) {}

                void Call(Params& a_params) override {
                    SSE_ZONE("AS::processEntry");
                    if (claimFirstReport(kSlotProcessEntry)) {
                        reportFirstCall(kSlotProcessEntry, "processEntry(e, _itemInfo)  [this.processEntry]",
                                        "thisPtr = processor, args = [entry, itemInfo], argCount = 2", a_params);
                    }
                    // Phase 4b-ii: the zone, the section 4.2 count relation and the
                    // `kExtraDiagnostics` stopwatch are all untouched; only what happens
                    // INSTEAD of the unconditional forward changed. The stopwatch now
                    // measures the C++ path too, which is what lets it price the residual
                    // AS2 heap pause the design expects (~14 ms/round) after the
                    // interpreter is gone -- it is no longer only measuring the branch it
                    // was written to look for.
                    if constexpr (kExtraDiagnostics) {
                        ++g_processEntrySeq;
                        const auto started = std::chrono::steady_clock::now();
                        run(a_params);
                        const double elapsedMs =
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                        if (elapsedMs >= static_cast<double>(kProcessEntryOutlierMs)) {
                            logProcessEntryOutlier(a_params, elapsedMs);
                        }
                    } else {
                        run(a_params);
                    }
                }

            private:
                /*
                 * The four decisions, in the safety order:
                 *
                 *   1. argument shape unexpected -> forward, no replica. A call site we
                 *      have never seen is not one to optimize blindly; S0b measured
                 *      exactly one (thisPtr = processor, args = [entry, itemInfo]).
                 *   2. replica declined          -> forward. Fail-closed: `begin` has
                 *      written nothing, so the AS body sees the entry exactly as the
                 *      previous call left it.
                 *   3. shadow sample still due    -> forward, then diff. The one round
                 *      that proves the transcription is paid for explicitly rather than
                 *      assumed.
                 *   4. otherwise                  -> write the prediction and DON'T
                 *      forward. This is the case the whole step exists for.
                 */
                void run(Params& a_params) {
#if SSE_REPLICATE_PROCESS_ENTRY
                    if (a_params.args && a_params.argCount >= 2 && a_params.args[0].IsObject() &&
                        a_params.args[1].IsObject()) {
                        pe::WriteSet predicted;
                        switch (pe::begin(a_params.movie, a_params.args[0], a_params.args[1], predicted)) {
                            case pe::Stage::kHandled:
                                return;
                            case pe::Stage::kPredictNoWrite:
                                forwardVerbatim(_original, a_params);
                                pe::finishValidation(a_params.movie, a_params.args[0], predicted);
                                return;
                            default:
                                break;
                        }
                    }
#endif
                    forwardVerbatim(_original, a_params);
                }

                RE::GFxValue _original;
        };

        /* =====================================================================
         * Phase 4a / step S1: the `_requestItemInfo` REPLACEMENT.
         *
         * Design: docs/phase4-design.md section 5 (5.1 mechanism, 5.2 cache source,
         * 5.3 expected saving, 5.4 risks), 8.1 row S1 and 8.2 (revert contract).
         * Implementation record: section 10.8.
         *
         * The three classes above forward and measure; this one ANSWERS. Replacing
         * the ActionScript body of `_requestItemInfo` (ItemcardDataExtender.as:22-28)
         *
         *     var oldIndex = this._selectedIndex;                                        // (A)
         *     this._selectedIndex = a_index;                                             // (A)
         *     GameDelegate.call("RequestItemCardInfo", [], a_target, "updateItemInfo");  // (B)
         *     this._selectedIndex = oldIndex;                                            // (C)
         *
         * turns (B) -- the engine round trip and its ExternalInterface marshalling --
         * into a table lookup plus one `SetMember`. That is the whole saving:
         * 443.4 ms/round (the C++ body) + 57.9 ms/round (the AS body, `apply`, the
         * boundary) = 501.3 ms/round = 62.3 % of the 804.6 ms/round measured in
         * capture #11b (design section 10.7 (4)).
         *
         * WARNING: (A) AND (C) ARE DELIBERATELY NOT REPRODUCED, and the pseudocode in
         * section 5.1 is wrong on this point. (A) exists for exactly one reader: the
         * engine, which learns "which item is this card for" from `_selectedIndex`
         * because the delegate arguments are empty (section 4.3). With (B) gone there
         * is no such reader left, and (A)/(C) have an identity net effect anyway --
         * (A) overwrites, (C) restores the same value, with no early exit between
         * them -- so the list ends up in the same state either way. That is what makes
         * skipping them safe rather than merely cheap. `_selectedIndex` is a plain
         * `private var _selectedIndex: Number` (BSList.as:21), so not even the
         * temporary write had a side effect to preserve, and dropping the pair also
         * removes a failure mode section 5.1 carries: if its `GetMember` of the old
         * index failed, its (C) would write an undefined value into the list.
         *
         * The switch: `kAnswerFromCache` is section 8.2 rule 3's per-insertion-point
         * toggle. It is a compile-time constant because this project has no
         * config-file reader yet (dist/Template.ini is still empty) and every capture
         * is a rebuild-and-deploy anyway -- flip it to false and this file compiles
         * back into the exact S0b build that produced capture #11b, which is what
         * keeps that capture reproducible. The runtime half of section 8.2 (rules 1
         * and 4) is per ITEM, not per build: the original function is held as a
         * `GFxValue` copy, and every miss -- and every failure anywhere on the hit
         * path -- forwards to it.
         * ===================================================================== */
        constexpr bool kAnswerFromCache = true;

        /*
         * The eight members `processEntry` reads out of `_itemInfo`, and nothing
         * else. The set is CLOSED, and that is the property which lets eight scalars
         * replace the engine's object instead of approximating it:
         *
         *   written  only by `updateItemInfo`  -- ItemcardDataExtender.as:34-37, a
         *            plain `_itemInfo = a_updateObj`
         *   read     only by `processEntry`    -- ItemcardDataExtender.as:56
         *
         * and the `a_itemInfo.*` reads in InventoryDataSetter -- the only class this
         * processor slot can hold (design section 10.4 (2)) -- are exactly these:
         *
         *     :27 type      :30 stolen      :32 value      :33 weight
         *     :47/:75/:85 effects           :48 armor      :77/:86 damage
         *     :76 poisoned
         *
         * Note what this list is NOT: it is not the union of the card fields of every
         * `ItemcardDataExtender` subclass. MagicDataSetter (:24-106) also reads
         * `spellCost` / `word0..2` / `unlocked0..2` / `timeRemaining` / `castLevel` /
         * `magicSchoolName` / `castTime`, and BarterDataSetter (:23-33) WRITES
         * `a_itemInfo.value`. Neither can reach this cache: both are bound to other
         * menus, and install() resolves the processor from THIS menu's
         * `inventoryLists.itemList` (resolveTargetProcessor). Should a later step key
         * a cache off anything broader, this list is the first thing that has to
         * change.
         */
        inline constexpr const char* kCardFields[] = {
            "type", "value", "weight", "stolen", "effects", "armor", "damage", "poisoned",
        };
        constexpr std::size_t kCardFieldCount = sizeof(kCardFields) / sizeof(kCardFields[0]);

        // The member a hit has to leave set on `a_params.args[0]`: it is what
        // `updateItemInfo` writes (`:36`) and what `processEntry(e, _itemInfo)` reads
        // (`:56`), so it is the one piece of processor state a hit is responsible for.
        constexpr const char* kItemInfoMember = "_itemInfo";

        /*
         * One field, stored so it can be rebuilt EXACTLY -- type included, not just
         * value.
         *
         * Why the type and not a plain C++ value: `processEntry` consumes these
         * through AS2 coercion (`a_itemInfo.value > 0`, `== true`, `!= ""`), and
         * coercion depends on the runtime type. Storing everything as a double would
         * silently turn `stolen` from `true` into `1` and a missing member into `0`;
         * those happen to compare the same for `==` and `>`, but not for a `!= ""`
         * test, and "happens to match" is not the standard this project measures
         * against. Storing the `GFxValue` type instead makes the rebuild a
         * re-creation rather than a re-interpretation.
         */
        struct CardField {
                enum class Kind : std::uint8_t {
                    kAbsent,  // no such member on the engine's object
                    kUndefined,
                    kNull,
                    kBoolean,
                    kNumber,
                    kString,
                };

                Kind        kind{ Kind::kAbsent };
                bool        boolean{ false };
                double      number{ 0.0 };
                std::string text;
        };

        // One item card = the eight fields above. `cacheable` stays false until a
        // capture has run to completion, so a half-copied card -- or one whose field
        // type this cache cannot reproduce -- is never served.
        struct Card {
                bool      cacheable{ false };
                CardField fields[kCardFieldCount];
        };

        /*
         * Why a DEEP COPY and not the `GFxValue` of the engine's object: a stored AS
         * object reference points into the movie's heap, and the movie is released
         * when the menu closes -- the lesson S-1 learned the hard way (design section
         * 10.2: by the time MenuOpenCloseEvent says "closed", IMenu::uiMovie and the
         * whole object graph behind it are already gone). A cross-open cache made of
         * `GFxValue`s would therefore be a use-after-free waiting for the second open.
         * Copying the eight fields out and re-creating them later is the only form
         * that survives, which is also why the container below holds values and not
         * references.
         *
         * The cache itself: index -> card. The index is the very number the AS body
         * would have written into `a_list._selectedIndex` for the engine to read
         * (section 5.1's three-host table, where `a_index` is the loop counter `i`),
         * so it is the engine's own key rather than an invention of ours.
         *
         * Cross-open on purpose. "Same open only" was the first cut in section 5.4,
         * and the measurement says otherwise: the build cost is paid once per OPEN
         * (6441 items) while the round trip is paid once per ITEM, so deferring the
         * build to the next open is exactly what buys the 501 ms. A cache that died
         * with the menu would save nothing at all.
         *
         * No invalidation in this step -- the trade-off, its accepted blast radius and
         * what 4a-2 has to add are in section 10.8. `std::vector` is the container
         * because invalidation is then one `clear()` away.
         *
         * Size: capture #11b's list is 6441 entries, a `Card` is 8 * sizeof(CardField)
         * and a `CardField` is a std::string plus 16 bytes -- well under 10 MB for a
         * full list, against 501 ms/round of savings.
         */
        inline std::vector<Card> g_cards;

        // Per-round accounting, logged and reset once per round by
        // reportAndResetRoundStats(). `g_cacheUncacheable` counts the items whose card
        // capture refused to certify (see captureField) and `g_serveFailures` the hits
        // whose rebuild failed -- both are per-ITEM degradations to the full round trip
        // rather than global ones, which is why they are counted separately.
        inline std::uint32_t g_cacheHits = 0;
        inline std::uint32_t g_cacheMisses = 0;
        inline std::uint32_t g_cacheUncacheable = 0;
        inline std::uint32_t g_serveFailures = 0;

        // One "this field's type cannot be reproduced" warning per FIELD NAME for the
        // whole session, not per item: 6440 items x 6440 lines would otherwise be all
        // that is left of the log. `g_itemInfoMissingWarned` is the same courtesy for the
        // "`_itemInfo` is not an object" case, which is not about any one field.
        inline std::array<bool, kCardFieldCount> g_fieldTypeWarned{};
        inline bool                              g_itemInfoMissingWarned = false;

        /* =====================================================================
         * Phase 4b / pre-4b-ii: the eight fields' actual `Kind` DISTRIBUTION.
         *
         * What capture #12 proved, and what it could not. It proved the cache is
         * FAITHFUL: 10 cards x 8 fields compared field by field, 0 differs, with
         * `fieldMatches` testing the Kind AND the exact value (strcmp for strings,
         * `==` with no epsilon for numbers), and `uncacheable` / `serve failures`
         * at 0 for the whole session. What it cannot say is which Kind each field
         * actually took, because a Tracy zone records a duration and nothing about
         * the values inside it.
         *
         * The branch that matters is `kAbsent`. `applyField` deliberately does NOT
         * write an absent member back -- that is what reproduces an absent member,
         * since AS2 reads a missing one as `undefined`, exactly like the engine's
         * object does -- so `kAbsent` is a live, load-bearing path in the cache,
         * and it has never been observed to fire. Whether the engine omits a member
         * or writes `undefined` / `0` / `""` decides whether that path is exercised
         * at all, and InventoryDataSetter.as cannot answer it: the card is built by
         * the engine, not by ActionScript.
         *
         * So it is counted instead. A per-field x per-Kind table, filled once per
         * certified card and printed ONCE per process, answers it definitively:
         * `absent=0` on every field means the branch was never taken, and then it
         * is documented as "unexercised on this data" rather than silently assumed
         * to be dead. The counters are 8 increments per item (51 528 per round) on
         * the capture path only, and that path runs in round 1 and never again in a
         * steady-state capture.
         * ===================================================================== */
        // Sized from the enum's own last value, not from a second constant that
        // could drift away from it. Kind is contiguous and ordered, so the value
        // doubles as the array index -- see kindIndex.
        inline constexpr std::size_t kKindCount = static_cast<std::size_t>(CardField::Kind::kString) + 1;
        static_assert(kKindCount == 6, "CardField::Kind gained a value; extend kindName() to match.");

        inline std::array<std::array<std::uint32_t, kKindCount>, kCardFieldCount> g_fieldKindCounts{};
        inline std::uint32_t                                                      g_capturedCards = 0;
        inline bool                                                               g_fieldKindCountsLogged = false;

        /*
         * H12a's cheap falsifier, and it rides along on the same capture because it
         * reads the same already-copied data. H12a is the leading explanation for
         * `AS::processEntry`'s unexplained -44.9 ms/round once the cache was
         * answering (design section 10.8 (10), tracy-capture-log.md section 12.4
         * point 4): the engine's own card hands `processEntry` an UNMANAGED
         * `const char*` (that is `GFxValue::SetString`, GFxValue.h:347), while
         * `serveCard` has to recreate the field with `GFxMovie::CreateString`
         * (GFxMovie.h:52) -- one managed-string allocation per hit, 6440 times a
         * round.
         *
         * If `effects` is EMPTY on essentially every item, that story cannot carry
         * tens of milliseconds a round: there would be no bytes to allocate. That
         * does NOT close H12a -- GFx may still allocate a string header, and this
         * counts the CAPTURE side rather than the SERVE side -- which is exactly how
         * it is reported below: a magnitude check, not a verdict.
         *
         * `effects` is the field that matters because it is the one string field
         * `processEntry` reads, and it reads it three times (:47 / :75 / :85).
         */
        inline std::uint32_t g_effectsEmpty = 0;
        inline std::uint32_t g_effectsNonEmpty = 0;
        inline std::uint64_t g_effectsTotalLen = 0;
        inline std::size_t   g_effectsMaxLen = 0;

        /*
         * Index of a field, by NAME. `effects` above is the only field any
         * diagnostic here needs by identity, and a hardcoded `4` would silently
         * point at `armor` the first time kCardFields is reordered -- which is
         * exactly the kind of quiet wrong answer this file's other tables exist to
         * prevent. kCardFieldCount doubles as the "not found" sentinel; callers
         * must test for it.
         */
        inline std::size_t indexOfCardField(const char* a_name) {
            for (std::size_t i = 0; i < kCardFieldCount; ++i) {
                if (std::strcmp(kCardFields[i], a_name) == 0) {
                    return i;
                }
            }
            return kCardFieldCount;
        }

        constexpr std::size_t kindIndex(CardField::Kind a_kind) {
            return static_cast<std::size_t>(a_kind);
        }

        // `default` rather than a trailing return, matching describeCardField below:
        // an out-of-range Kind then prints `?` instead of being undefined behaviour.
        inline const char* kindName(CardField::Kind a_kind) {
            switch (a_kind) {
                case CardField::Kind::kAbsent:
                    return "absent";
                case CardField::Kind::kUndefined:
                    return "undefined";
                case CardField::Kind::kNull:
                    return "null";
                case CardField::Kind::kBoolean:
                    return "bool";
                case CardField::Kind::kNumber:
                    return "number";
                case CardField::Kind::kString:
                    return "string";
                default:
                    return "?";
            }
        }

        inline void logFieldKindHistogram() {
            if (g_fieldKindCountsLogged || g_capturedCards == 0) {
                return;
            }
            g_fieldKindCountsLogged = true;
            logger::info(
                "4a/4b: card field `Kind` histogram over {} certified card(s) -- `absent` counts the engine OMITTING a "
                "member, the one kind applyField reproduces by writing nothing",
                g_capturedCards);
            for (std::size_t i = 0; i < kCardFieldCount; ++i) {
                logger::info("4a/4b:   {}: {}={} {}={} {}={} {}={} {}={} {}={}", kCardFields[i],
                             kindName(CardField::Kind::kAbsent), g_fieldKindCounts[i][kindIndex(CardField::Kind::kAbsent)],
                             kindName(CardField::Kind::kUndefined), g_fieldKindCounts[i][kindIndex(CardField::Kind::kUndefined)],
                             kindName(CardField::Kind::kNull), g_fieldKindCounts[i][kindIndex(CardField::Kind::kNull)],
                             kindName(CardField::Kind::kBoolean), g_fieldKindCounts[i][kindIndex(CardField::Kind::kBoolean)],
                             kindName(CardField::Kind::kNumber), g_fieldKindCounts[i][kindIndex(CardField::Kind::kNumber)],
                             kindName(CardField::Kind::kString), g_fieldKindCounts[i][kindIndex(CardField::Kind::kString)]);
            }
            logger::info(
                "4a/4b: `effects` string lengths: empty {} / non-empty {} / total {} chars / max {} (H12a magnitude "
                "check: an all-empty column cannot pay for tens of ms/round of managed-string allocation -- falsifier, "
                "not a verdict)",
                g_effectsEmpty, g_effectsNonEmpty, g_effectsTotalLen, g_effectsMaxLen);
        }

        /*
         * `s1::typeName` has no `kStringW` case -- that namespace only ever printed
         * types it could read -- while a `kStringW` field is precisely the case this
         * cache refuses (see captureField), so it has to be nameable in the log.
         */
        inline const char* describeType(const RE::GFxValue& a_val) {
            if (a_val.IsStringW()) {
                return "stringW";
            }
            return s1::typeName(a_val);
        }

        /*
         * Deep-copies ONE field, preserving its `GFxValue` type rather than reducing it
         * to a C++ type (see CardField).
         *
         * Returns false for a type this cache cannot reproduce, `kStringW` included.
         * That answer is fail-closed rather than best-effort on purpose: the caller then
         * marks the whole card uncacheable, so that one item keeps taking the untouched
         * round trip and the log names the field, instead of the UI quietly showing a
         * wrong number. No `kStringW` field has ever been observed -- the eight fields
         * come out of the engine as kString / kNumber / kBoolean (capture #11b, design
         * section 10.7) -- which is exactly why assuming is unnecessary.
         *
         * An ABSENT member is stored as kAbsent and never written back, because that
         * reproduces itself: reading an absent member yields `undefined` in AS2, which
         * is what the engine's object gives `processEntry` too.
         */
        inline bool captureField(const RE::GFxValue& a_itemInfo, std::size_t a_field, CardField& a_out) {
            RE::GFxValue value;
            if (!a_itemInfo.GetMember(kCardFields[a_field], &value)) {
                a_out.kind = CardField::Kind::kAbsent;
                return true;
            }
            if (value.IsUndefined()) {
                a_out.kind = CardField::Kind::kUndefined;
                return true;
            }
            if (value.IsNull()) {
                a_out.kind = CardField::Kind::kNull;
                return true;
            }
            if (value.IsBool()) {
                a_out.kind = CardField::Kind::kBoolean;
                a_out.boolean = value.GetBool();
                return true;
            }
            if (value.IsNumber()) {
                a_out.kind = CardField::Kind::kNumber;
                a_out.number = value.GetNumber();
                return true;
            }
            if (value.IsString()) {
                const char* text = value.GetString();
                a_out.kind = CardField::Kind::kString;
                a_out.text = text ? text : "";
                return true;
            }
            if (!g_fieldTypeWarned[a_field]) {
                g_fieldTypeWarned[a_field] = true;
                logger::warn(
                    "4a: `_itemInfo.{}` is a `{}`, which this cache cannot reproduce exactly; the whole card stays "
                    "uncacheable and that item keeps taking the full round trip",
                    kCardFields[a_field], describeType(value));
            }
            return false;
        }

        /*
         * The inverse of captureField, and the reason a card can be rebuilt at all.
         *
         * Note which setter is NOT used: `GFxValue::SetString(const char*)` stores a
         * BARE POINTER (GFxValue.cpp:774) -- the caller has to keep that memory alive
         * for as long as the value is used, which a `std::string` inside a cache entry
         * cannot promise forever. `GFxMovie::CreateString` (GFxMovie.h:52, vfunc 0B) is
         * the managed form: "Creates strings that are managed by ActionScript runtime",
         * i.e. it copies into the movie's own heap, which is what a member of an AS
         * object has to own. Every other kind is a value type and needs no allocation.
         *
         * `a_scratch` is the caller's reusable `GFxValue`; ChangeType releases the
         * previous managed value (GFxValue.cpp:1005), so re-using one scratch object
         * across the eight fields is safe rather than leaky.
         */
        inline bool applyField(RE::GFxMovie* a_movie, RE::GFxValue& a_card, std::size_t a_field, const CardField& a_in,
                               RE::GFxValue& a_scratch) {
            switch (a_in.kind) {
                case CardField::Kind::kAbsent:
                    // Leave the member out entirely -- see captureField.
                    return true;
                case CardField::Kind::kUndefined:
                    a_scratch.SetUndefined();
                    break;
                case CardField::Kind::kNull:
                    a_scratch.SetNull();
                    break;
                case CardField::Kind::kBoolean:
                    a_scratch.SetBoolean(a_in.boolean);
                    break;
                case CardField::Kind::kNumber:
                    a_scratch.SetNumber(a_in.number);
                    break;
                case CardField::Kind::kString:
                    a_movie->CreateString(&a_scratch, a_in.text.c_str());
                    if (!a_scratch.IsString()) {
                        return false;
                    }
                    break;
            }
            return a_card.SetMember(kCardFields[a_field], a_scratch);
        }

        // A card is servable exactly when a capture has certified it.
        inline const Card* findCard(std::size_t a_index) {
            if (a_index >= g_cards.size()) {
                return nullptr;
            }
            const Card& card = g_cards[a_index];
            return card.cacheable ? &card : nullptr;
        }

        /*
         * The cache-building half of a miss: deep-copy the engine's card into
         * `g_cards[a_index]`.
         *
         * Called immediately after the original body has run, which is the one moment
         * the engine's card is reachable as an AS object -- `updateItemInfo` puts it in
         * `_itemInfo` (ItemcardDataExtender.as:34-37) and nothing else reads it before
         * `processEntry` consumes it. This is source "B prime" of section 5.2: no
         * `Respond` payload has to be intercepted (that was hypothesis H7 and 4a option
         * A), because the AS side already stored the engine's object for us -- so the
         * cache holds exactly what the engine produced, not a reconstruction of it.
         *
         * `cacheable` is cleared FIRST and only set after all eight fields have copied:
         * a re-capture that fails halfway must not leave the previous certificate
         * standing over a half-overwritten card.
         */
        inline void captureCard(RE::GFxValue& a_processor, std::size_t a_index) {
            RE::GFxValue itemInfo;
            if (!a_processor.GetMember(kItemInfoMember, &itemInfo) || !itemInfo.IsObject()) {
                ++g_cacheUncacheable;
                if (!g_itemInfoMissingWarned) {
                    // One line per process, not per item: 6440 of these would be the
                    // whole log, and the finding is "the field set assumption is wrong",
                    // which one line already says.
                    g_itemInfoMissingWarned = true;
                    logger::warn("4a: `_itemInfo` is not an object after the round trip; cards stay uncacheable");
                }
                return;
            }

            if (a_index >= g_cards.size()) {
                g_cards.resize(a_index + 1);
            }
            Card& card = g_cards[a_index];
            card.cacheable = false;

            for (std::size_t i = 0; i < kCardFieldCount; ++i) {
                if (!captureField(itemInfo, i, card.fields[i])) {
                    // captureField has already logged which field and which type.
                    ++g_cacheUncacheable;
                    return;
                }
            }
            card.cacheable = true;

            // Counted only for a COMPLETE card, so every field's histogram sums to
            // the same number -- a half-copied card would otherwise contribute to
            // the earlier fields and not the later ones, which is exactly the
            // ambiguity the table exists to remove. See the block comment on it.
            if constexpr (kExtraDiagnostics) {
                ++g_capturedCards;
                for (std::size_t i = 0; i < kCardFieldCount; ++i) {
                    ++g_fieldKindCounts[i][kindIndex(card.fields[i].kind)];
                }

                // H12a's magnitude check -- see the block comment on these counters.
                const std::size_t effectsField = indexOfCardField("effects");
                if (effectsField < kCardFieldCount) {
                    const CardField& effects = card.fields[effectsField];
                    if (effects.kind == CardField::Kind::kString) {
                        if (effects.text.empty()) {
                            ++g_effectsEmpty;
                        } else {
                            ++g_effectsNonEmpty;
                        }
                        g_effectsTotalLen += effects.text.size();
                        g_effectsMaxLen = std::max(g_effectsMaxLen, effects.text.size());
                    }
                }
            }
        }

        /*
         * A cache hit, end to end: build an AS object, fill the eight fields, and make
         * it the processor's `_itemInfo` -- which is precisely the state the AS body
         * would have left behind, since `updateItemInfo` is nothing but
         * `_itemInfo = a_updateObj` (ItemcardDataExtender.as:34-37). What is NOT here
         * is the (A)/(C) `_selectedIndex` pair and the engine round trip (B); see the
         * block comment above kAnswerFromCache for why omitting (A)/(C) is an identity
         * transform once (B) is gone.
         *
         * Returns false the moment any step fails, so the caller can fall back to the
         * untouched round trip instead of leaving a half-built card in the movie. That
         * fallback needs no rollback: the engine rewrites `_itemInfo` wholesale, so
         * whatever applyField managed to write is overwritten and never observable.
         *
         * Lifetime of the local `card`: `SetMember` gives the receiving object its own
         * reference, so this local's destructor at the end of the call is correct rather
         * than premature. The S0b install is the measured precedent for exactly this
         * pattern -- it `SetMember`s three `CreateFunction` locals that die with
         * `install()`, and the probes were still firing 6440 times a round in capture
         * #11b, i.e. a whole round after those locals went out of scope.
         */
        inline bool serveCard(RE::GFxFunctionHandler::Params& a_params, RE::GFxValue& a_processor, const Card& a_card) {
            if (!a_params.movie) {
                return false;
            }
            RE::GFxValue card;
            a_params.movie->CreateObject(&card);
            if (!card.IsObject()) {
                return false;
            }

            RE::GFxValue scratch;
            for (std::size_t i = 0; i < kCardFieldCount; ++i) {
                if (!applyField(a_params.movie, card, i, a_card.fields[i], scratch)) {
                    return false;
                }
            }
            return a_processor.SetMember(kItemInfoMember, card);
        }

        /* =====================================================================
         * The validation sample -- section 8.1's "UI values identical to stock
         * skyui, 10 items spot-checked", turned from a screen-read into a
         * measurement.
         *
         * Why it is worth 10 forwarded items: a cache hit is the only place in this
         * whole step where a wrong answer is POSSIBLE (the hit path is new code; the
         * miss path is the original body). Section 5.4 rates the risk as "the cache
         * and the real inventory disagree", and the way it could happen is that the
         * engine's card for index i is not a pure function of i -- a cross-open cache
         * assumes it is. Forwarding the first 10 hits and diffing the two cards
         * answers exactly that, on real data, per FIELD, and leaves a log line per
         * item instead of a subjective impression.
         *
         * Cost, bounded and visible: 10 extra engine round trips in round 2, so round
         * 2's RequestItemCardInfo count reads 1 + 10 = 11 instead of 1. That is still
         * three orders of magnitude below round 1's 6441, so the timing verdict is
         * untouched -- but it does mean the number to expect in capture #12 is 11, and
         * 1 (not 0) in the rounds after it. See section 10.8.
         * ===================================================================== */
        constexpr std::uint32_t kValidateSampleSize = 10;
        inline std::uint32_t     g_validateRemaining = kValidateSampleSize;
        inline std::uint32_t     g_validateCompared = 0;
        inline std::uint32_t     g_validateMismatches = 0;

        // Does the freshly captured value equal the cached field?
        inline bool fieldMatches(const CardField& a_cached, const RE::GFxValue& a_fresh) {
            switch (a_cached.kind) {
                case CardField::Kind::kUndefined:
                    return a_fresh.IsUndefined();
                case CardField::Kind::kNull:
                    return a_fresh.IsNull();
                case CardField::Kind::kBoolean:
                    return a_fresh.IsBool() && a_fresh.GetBool() == a_cached.boolean;
                case CardField::Kind::kNumber:
                    // Exact, not epsilon: both sides come from the same engine call on
                    // the same form, so any difference at all is the finding -- an
                    // epsilon would hide a small one.
                    return a_fresh.IsNumber() && a_fresh.GetNumber() == a_cached.number;
                case CardField::Kind::kString: {
                    if (!a_fresh.IsString()) {
                        return false;
                    }
                    const char* text = a_fresh.GetString();
                    return std::strcmp(text ? text : "", a_cached.text.c_str()) == 0;
                }
                case CardField::Kind::kAbsent:
                default:
                    // GetMember succeeded, so an absent cached field cannot match.
                    return false;
            }
        }

        // Cached side of a mismatch report, in the same vocabulary the fresh side uses
        // (s1::describe), so the two halves of a warning line read as a pair.
        inline std::string describeCardField(const CardField& a_field) {
            switch (a_field.kind) {
                case CardField::Kind::kAbsent:
                    return "absent";
                case CardField::Kind::kUndefined:
                    return "undefined";
                case CardField::Kind::kNull:
                    return "null";
                case CardField::Kind::kBoolean:
                    return a_field.boolean ? "bool true" : "bool false";
                case CardField::Kind::kNumber:
                    return "number " + std::to_string(a_field.number);
                case CardField::Kind::kString:
                    return "string \"" + a_field.text + "\"";
                default:
                    return "?";
            }
        }

        /*
         * One validation item: the round trip has just run (the caller forwarded), so
         * `_itemInfo` holds the engine's own card again -- compare it, field by field,
         * against the copy this cache was about to serve from.
         *
         * Both sides are read with the same code path (GetMember + a type check), so the
         * diff cannot be an artifact of how the two are described. A mismatch here is
         * not a formatting problem, it is the cross-open assumption itself failing, and
         * the log names the index and the field so it can be tracked to an item.
         */
        inline void compareAgainstCache(RE::GFxValue& a_processor, std::size_t a_index, const Card& a_cached) {
            ++g_validateCompared;
            RE::GFxValue itemInfo;
            if (!a_processor.GetMember(kItemInfoMember, &itemInfo) || !itemInfo.IsObject()) {
                ++g_validateMismatches;
                logger::warn("4a/validate: index {}: `_itemInfo` is not an object after the round trip", a_index);
                return;
            }

            RE::GFxValue value;
            std::size_t  mismatched = 0;
            for (std::size_t i = 0; i < kCardFieldCount; ++i) {
                const bool fresh = itemInfo.GetMember(kCardFields[i], &value);
                const bool same = fresh ? fieldMatches(a_cached.fields[i], value) : (a_cached.fields[i].kind == CardField::Kind::kAbsent);
                if (same) {
                    continue;
                }
                ++mismatched;
                logger::warn("4a/validate: index {}: `{}` differs -- cached {}, engine {}", a_index, kCardFields[i],
                             describeCardField(a_cached.fields[i]), fresh ? s1::describe(value) : "absent (fresh)");
            }

            if (mismatched == 0) {
                logger::info("4a/validate: index {}: all {} card fields identical to the engine's own card", a_index, kCardFieldCount);
            } else {
                g_validateMismatches += static_cast<std::uint32_t>(mismatched);
            }
        }

        /*
         * Per-round accounting. Called from install(), which runs exactly once per round
         * (on the first RequestItemCardInfo of the round -- see the block comment on the
         * arm/consume pair above), so the numbers printed here are the numbers of the
         * PREVIOUS round and are then reset for the next one.
         *
         * That placement is what makes capture #12 self-evidencing: round 1 prints `cache
         * hits 0, full round trips 6440`, rounds 2..5 print `cache hits 6440, full round
         * trips 0`, and nobody has to read the Tracy timeline to see which. (Zero misses
         * after round 1 is expected, not suspicious: index 0 runs the original AS body in
         * every round -- see the ordering block comment on the handler below -- so no
         * handler call ever asks for an index the cache was not given.)
         */
        inline void reportAndResetRoundStats(std::size_t a_cacheSize) {
            if (g_cacheHits || g_cacheMisses || g_cacheUncacheable || g_serveFailures) {
                logger::info(
                    "4a: previous round -- cache hits {}, full round trips {}, uncacheable cards {}, serve failures {}",
                    g_cacheHits, g_cacheMisses, g_cacheUncacheable, g_serveFailures);
                if (g_validateCompared) {
                    logger::info("4a: previous round -- validation compared {} cards field by field, {} mismatch(es)",
                                 g_validateCompared, g_validateMismatches);
                }
            }
            logger::info("4a: cache holds {} card slot(s), {} of them certified", g_cards.size(), a_cacheSize);
            // One-shot, and this is the right moment: round 1 is the only round that
            // captures -- rounds 2..5 serve hits and forward 10 validation items --
            // and this call happens at the START of round 2, i.e. when round 1's
            // captures are complete. That placement is also what keeps a re-capture
            // from double counting, rather than a flag on the table itself.
            if constexpr (kExtraDiagnostics) {
                logFieldKindHistogram();
            }
            g_cacheHits = 0;
            g_cacheMisses = 0;
            g_cacheUncacheable = 0;
            g_serveFailures = 0;
            g_validateCompared = 0;
            g_validateMismatches = 0;
            if constexpr (kExtraDiagnostics) {
                // Round-local ordinal of the processEntry outlier probe, reset here
                // because this runs exactly once per round and always before that
                // round's first `processEntry` -- see the block comment on the probe.
                g_processEntrySeq = 0;
            }
        }

        // Counting the servable cards is O(n) and runs once per round, so it is not worth
        // maintaining as a counter that would have to be kept honest on resize.
        inline std::size_t countCertifiedCards() {
            std::size_t count = 0;
            for (const Card& card : g_cards) {
                if (card.cacheable) {
                    ++count;
                }
            }
            return count;
        }

        /*
         * The replacement itself. Same zone name as the S0b probe on purpose: in cache
         * mode the zone no longer CONTAINS the engine round trip, so a capture shows the
         * same zone with its nested `RequestItemCardInfo` child mostly gone -- which is
         * the finding, visible at a glance, rather than a name change the reader has to
         * translate. It also keeps the section 4.2 consistency check usable in both
         * modes, since this handler's call count is still "6441 items minus the round's
         * first item" (see below).
         *
         * The order of the four decisions below is the safety order:
         *
         *   1. argument shape wrong        -> forward, no cache. A call site we have never
         *                                     seen (S0b measured exactly one: thisPtr =
         *                                     a_list, args = [processor, i], argCount = 2)
         *                                     is not a call site to optimize blindly.
         *   2. no usable card              -> forward, then capture. The miss path, and
         *                                     the only path that runs in round 1.
         *   3. validation sample still due -> forward + diff, so the ONE round that proves
         *                                     the cross-open assumption is paid for
         *                                     explicitly instead of being skipped.
         *   4. otherwise                   -> rebuild from cache and DON'T forward.
         *
         * Round 1 is all misses by construction, and `install()` is the reason: the
         * probes go in on the round's first RequestItemCardInfo, which happens INSIDE
         * that item's `_requestItemInfo`, so item 0 keeps running the original AS body.
         * That is why every round shows exactly one full round trip (index 0, which is
         * therefore never cached) -- the same 1-per-round that capture #11b's
         * `AS::_requestItemInfo` count = full-count minus rounds already showed.
         */
        class RequestItemInfoCacheHandler final : public RE::GFxFunctionHandler {
            public:
                explicit RequestItemInfoCacheHandler(const RE::GFxValue& a_original) :
                    _original(a_original) {}

                void Call(Params& a_params) override {
                    SSE_ZONE("AS::_requestItemInfo");
                    if (claimFirstReport(kSlotRequestItemInfo)) {
                        reportFirstCall(kSlotRequestItemInfo, "_requestItemInfo.apply(a_list, [this, i])",
                                        "thisPtr = list, args = [processor, index], argCount = 2", a_params);
                    }

                    RE::GFxValue* processor = nullptr;
                    std::size_t   index = 0;
                    if (!resolveCall(a_params, processor, index)) {
                        forwardVerbatim(_original, a_params);
                        return;
                    }

                    const Card* card = findCard(index);
                    if (!card) {
                        ++g_cacheMisses;
                        forwardVerbatim(_original, a_params);
                        captureCard(*processor, index);
                        return;
                    }

                    if (g_validateRemaining > 0) {
                        --g_validateRemaining;
                        forwardVerbatim(_original, a_params);
                        compareAgainstCache(*processor, index, *card);
                        return;
                    }

                    if (serveCard(a_params, *processor, *card)) {
                        ++g_cacheHits;
                        return;
                    }

                    // The hit path failed somewhere. Fall back to the round trip we were
                    // trying to avoid -- counted as a miss, because that is what the item
                    // cost, and reported once per process so a systematic failure cannot
                    // hide behind the 6440 lines of a "successful" round.
                    ++g_cacheMisses;
                    if (g_serveFailures++ == 0) {
                        logger::warn("4a: rebuilding card {} failed; falling back to the full round trip from here on", index);
                    }
                    forwardVerbatim(_original, a_params);
                }

            private:
                /*
                 * The one place the three hosts of section 5.1 are distinguished, and the
                 * only place a call can be declined. `a_params.args[0]` is the processor
                 * (`a_target`, the holder of `_itemInfo`) and `args[1]` the index -- NOT
                 * `thisPtr`, which is the LIST. S0b confirmed both on a real machine
                 * (design section 10.7 (1) item 3); this re-checks them per call because
                 * a wrong answer here would corrupt the movie rather than merely mis-time
                 * it.
                 */
                static bool resolveCall(const Params& a_params, RE::GFxValue*& a_processor, std::size_t& a_index) {
                    if (!a_params.args || a_params.argCount < 2 || !a_params.args[0].IsObject() || !a_params.args[1].IsNumber()) {
                        return false;
                    }
                    const double number = a_params.args[1].GetNumber();
                    if (number < 0.0) {
                        return false;
                    }
                    a_processor = &a_params.args[0];
                    a_index = static_cast<std::size_t>(number);
                    return true;
                }

                RE::GFxValue _original;
        };

        /*
         * Creates the replacement function object for one slot. A non-object
         * GFxValue means failure, which is the only way a failed CreateFunction can
         * be noticed at all: the API returns void (GFxMovie.h:56).
         *
         * `a_movie` is GFxMovie, not GFxMovieView: CreateFunction lives on the base
         * class (GFxMovie.h:56, vfunc 0F), and IMenu::uiMovie (GPtr<GFxMovieView>,
         * IMenu.h:106) is-a GFxMovie, so the caller passes it through unchanged.
         */
        inline RE::GFxValue makeProbe(std::size_t a_slot, RE::GFxMovie* a_movie, const RE::GFxValue& a_original) {
            RE::GFxValue probe;
            switch (a_slot) {
                case kSlotRequestItemInfo:
                    // Phase 4a: the one slot whose replacement ANSWERS rather than
                    // forwards. `kAnswerFromCache = false` compiles the S0b probe build
                    // back in verbatim (see the block comment above kAnswerFromCache).
                    // The slot below is gated the same way but by its own switch:
                    // `processList` stays a pass-through probe in both modes, because
                    // S3 was never measurable (capture #11b) and 4b-ii replaced
                    // `processEntry`, not the loop around it.
                    if constexpr (kAnswerFromCache) {
                        a_movie->CreateFunction(&probe, new RequestItemInfoCacheHandler(a_original), nullptr);
                    } else {
                        a_movie->CreateFunction(&probe, new RequestItemInfoHandler(a_original), nullptr);
                    }
                    break;
                case kSlotProcessList:
                    a_movie->CreateFunction(&probe, new ProcessListHandler(a_original), nullptr);
                    break;
                case kSlotProcessEntry:
                    // Phase 4b-ii: the replica. Same wrapper, same zone and the same
                    // forwarded fallback as the S0b probe -- only the answering path is
                    // new, and `kReplicateProcessEntry = false` compiles the S0b probe
                    // back in verbatim.
                    a_movie->CreateFunction(&probe, new ProcessEntryHandler(a_original), nullptr);
                    break;
                default:
                    break;
            }
            return probe;
        }

        /*
         * Re-walks the coordinate S-1 pinned down (section 10.4 (5)) from the live
         * menu: it RESOLVES the path instead of trusting a cached GFxValue. That is
         * not defensiveness for its own sake -- the list and every processor inside
         * it are recreated on each open, so any cache would be precisely the thing
         * that breaks on round 2.
         *
         * Returns false at the first step that does not resolve; the caller then
         * leaves the movie untouched (section 8.2, rule 4).
         */
        inline bool resolveTargetProcessor(RE::InventoryMenu* a_menu, RE::GFxValue& a_out) {
            if (!a_menu) {
                return false;
            }

            // Same entry point s1 uses, and for the same reason: the clip handle is
            // live (measured -- section 10.4 (4), H1), while uiMovie is only a
            // fallback source for it.
            RE::GFxValue menuRoot = a_menu->GetRuntimeData().root;
            if (!menuRoot.IsObject() && a_menu->uiMovie) {
                a_menu->uiMovie->GetVariable(&menuRoot, "_level0.Menu_mc");
            }
            if (!menuRoot.IsObject()) {
                return false;
            }

            RE::GFxValue list;
            if (!s1::resolvePath(menuRoot, "inventoryLists.itemList", list) || !s1::looksLikeInventoryList(list)) {
                return false;
            }

            RE::GFxValue processors;
            if (!list.GetMember("_dataProcessors", &processors) || !processors.IsArray()) {
                return false;
            }

            RE::GFxValue processor;
            if (!processors.GetElement(kTargetProcessorSlot, &processor) || !processor.IsObject()) {
                return false;
            }

            // The discriminator s1::dumpProcessors applies, re-checked on the slot
            // we are about to modify rather than assumed from its index:
            // processList / processEntry alone would also match InventoryIconSetter
            // and PropertyDataExtender (section 10.4 (2)).
            if (!processor.HasMember(kProbeMember)) {
                return false;
            }

            a_out = processor;
            return true;
        }

        /*
         * Writes the revert path of section 8.2 back, one member at a time. Only
         * ever called from the rollback below, which is why a failure here is
         * reported as an error rather than handled further: there is no second
         * fallback, and the honest outcome is a loud log line.
         */
        inline void restoreMembers(RE::GFxValue& a_processor, const RE::GFxValue (&a_originals)[kSlotCount],
                                   std::size_t a_count) {
            for (std::size_t j = 0; j < a_count; ++j) {
                if (!a_processor.SetMember(kProbeMembers[j], a_originals[j])) {
                    logger::error("S0: rollback of `{}` failed; processor left partially patched", kProbeMembers[j]);
                }
            }
        }

        /*
         * The patch itself: all three probes go in, or none does.
         *
         * In phase 4a mode the set is unchanged in SHAPE -- three members, one snapshot
         * each, all-or-nothing -- and only slot 0's function differs: it answers instead
         * of forwarding (RequestItemInfoCacheHandler). The all-or-nothing rule still
         * applies to it, and for the reason above plus one more: a slot-0 install that
         * failed while slots 1/2 went in would leave a round with S4 measured and the
         * round trip not, i.e. a capture that cannot be compared against #11b.
         *
         * S3 is a difference of three totals, so a partial install is not a smaller
         * measurement but an uninterpretable one (see the block comment above). The
         * three phases below are ordered so that only the last one can leave a
         * partial state at all:
         *
         *   1. snapshot all three originals     -- nothing written yet
         *   2. build all three function objects -- nothing written yet
         *   3. write all three                  -- rolled back on failure
         *
         * So a failure in phase 1 or 2 leaves the processor UNTOUCHED by
         * construction, and a failure in phase 3 is undone from the snapshots.
         *
         * Returns false if nothing was patched, so the caller can keep the flag
         * armed and retry on the next item instead of waiting for the next open.
         */
        inline bool install(RE::InventoryMenu* a_menu) {
            RE::GFxValue processor;
            if (!resolveTargetProcessor(a_menu, processor)) {
                return false;
            }

            auto* movie = a_menu->uiMovie.get();
            if (!movie) {
                return false;
            }

            // Phase 1: snapshot the originals BEFORE the first write. They are both
            // the forward target of every probe call and the revert path of section
            // 8.2.
            RE::GFxValue originals[kSlotCount];
            for (std::size_t i = 0; i < kSlotCount; ++i) {
                if (!processor.GetMember(kProbeMembers[i], &originals[i])) {
                    logger::warn("S0: `{}` not readable on _dataProcessors[{}]; nothing patched (all-or-nothing)",
                                 kProbeMembers[i], kTargetProcessorSlot);
                    return false;
                }
            }

            // Phase 2: build the replacements. Still before the first write.
            RE::GFxValue probes[kSlotCount];
            for (std::size_t i = 0; i < kSlotCount; ++i) {
                probes[i] = makeProbe(i, movie, originals[i]);
                if (!probes[i].IsObject()) {
                    logger::warn("S0: CreateFunction produced no function object for `{}`; nothing patched",
                                 kProbeMembers[i]);
                    return false;
                }
            }

            // Phase 3: the writes. A failure here is the only way to end up half
            // patched, so it is rolled back.
            for (std::size_t i = 0; i < kSlotCount; ++i) {
                if (processor.SetMember(kProbeMembers[i], probes[i])) {
                    continue;
                }
                logger::warn("S0: SetMember(`{}`) failed; rolling back {} member(s)", kProbeMembers[i], i);
                restoreMembers(processor, originals, i);
                return false;
            }

            if constexpr (kAnswerFromCache) {
                // Phase 4a. This function runs once per round, on the round's first
                // item, so the counters just printed belong to the round that ended --
                // see reportAndResetRoundStats.
                reportAndResetRoundStats(countCertifiedCards());
            }
#if SSE_REPLICATE_PROCESS_ENTRY
            // Warm the replica's translation table HERE rather than on the hot path.
            // This runs once per round, outside the `AS::processEntry` zone, which
            // matters twice: the ~100 `Translator.translate` calls that build the table
            // would otherwise land inside the first item's processEntry measurement and
            // show up as a ~30 ms outlier on item #1 of round 1, and the per-item cost
            // afterwards is one flag read. `prepare` is idempotent and bounded, so a
            // failure here costs log lines and nothing else.
            pe::prepare(movie);

            // Phase 4b-ii, same placement and same reason as 4a's report: the replica's
            // counters are per-round and are final at exactly this moment. This is also
            // what re-arms the shadow sample.
            pe::reportAndResetRoundStats();
#endif

            if constexpr (kAnswerFromCache && kReplicateProcessEntry) {
                logger::info(
                    "4a/4b: `_dataProcessors[{}]` wrapped: `{}` = CACHE (answered in C++), `{}` = pass-through probe, "
                    "`{}` = 4b-ii replica (answered in C++)",
                    kTargetProcessorSlot, kProbeMembers[kSlotRequestItemInfo], kProbeMembers[kSlotProcessList],
                    kProbeMembers[kSlotProcessEntry]);
            } else if constexpr (kAnswerFromCache) {
                logger::info(
                    "4a: `_dataProcessors[{}]` wrapped: `{}` = CACHE (answered in C++), `{}` / `{}` = pass-through "
                    "probes",
                    kTargetProcessorSlot, kProbeMembers[kSlotRequestItemInfo], kProbeMembers[kSlotProcessList],
                    kProbeMembers[kSlotProcessEntry]);
            } else {
                logger::info(
                    "S0: `_dataProcessors[{}]` wrapped: `{}` / `{}` / `{}` = C++ probes (pure forwarders: no cache, no "
                    "optimization)",
                    kTargetProcessorSlot, kProbeMembers[kSlotRequestItemInfo], kProbeMembers[kSlotProcessList],
                    kProbeMembers[kSlotProcessEntry]);
            }
            return true;
        }

    }  // namespace s0

    namespace {
        /*
         * Timeline marker sink (plan section 4.3).
         *
         * A rendered frame cannot be observed without hooking DXGI Present,
         * which this project does not do. Instead we slice the Tracy timeline
         * whenever the inventory menu opens or closes: that interval is exactly
         * the operation whose cost question 1 of the acceptance criteria asks
         * about. Only low-frequency events are touched -- never call this kind
         * of logging from a hot path.
         */
        class MenuOpenCloseListener final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
            public:
                RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                                      RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                    if (a_event && a_event->menuName == "InventoryMenu") {
                        // Target 2.1. The FxDelegate callback table only exists
                        // once the menu has loaded its movie, so the earliest
                        // possible resolution point is the first open.
                        // Both branches try, not just `opening`: the call is
                        // idempotent, and if `opening` were to fire before the
                        // table is populated then `closing` picks it up, so the
                        // next open is instrumented instead of us losing two
                        // rounds to the menu lifecycle.
                        ProfilingHooks::installRequestItemCardInfoHook();
                        // Target 2.5. Same trigger and same reason: it needs the
                        // menu's FxDelegate instance to validate the vtable it is
                        // about to patch (see installFxDelegateCallbackHook).
                        ProfilingHooks::installFxDelegateCallbackHook();

                        if (a_event->opening) {
                            // Phase 4 / steps S0a + S0b: arm the three probes
                            // (`_requestItemInfo` / `processList` / `processEntry`)
                            // for the round that is about to build its list.
                            // Deliberately only ARMS: resolving the graph happens
                            // on the first RequestItemCardInfo of the round, which
                            // is the first moment the rebuilt list is guaranteed to
                            // exist (and where installAsPathProbe already proved
                            // that it does).
                            ProfilingHooks::requestAsProbeInstall();
                            SSE_MSG_L("InventoryMenu opened");
                        } else {
                            SSE_MSG_L("InventoryMenu closed");
                            // NOTE: phase 4 / S-1 must NOT probe here. This is
                            // where the first version hooked in, and it logged
                            // nothing: IMenu::uiMovie (and the movie object graph
                            // with it) is already released by the time this event
                            // arrives, so the probe returned early every single
                            // time without latching. The corrected trigger lives
                            // in RequestItemCardInfoHook::hook -- see design
                            // section 10.2.
                        }
                        SSE_FRAME("InventoryMenu");
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }
        };

        MenuOpenCloseListener g_menuOpenCloseListener;
    }  // namespace
#endif

    void ProfilingHooks::install() {
#ifdef TRACY_ENABLE
        logger::info("Tracy instrumentation enabled; installing profiling hooks");

        // Target #1: InventoryChanges::GetItemCount -- the highest-frequency
        // aggregate query, and the reference point for the n x m ratio of
        // acceptance question 2.
        GetItemCountHook::install();

        // Targets #2..8: the remaining read-path entry points. Every (SE, AE)
        // relocation here comes from the CommonLibSSE-NG source, see
        // ProfilingHooks.h for the per-target signature notes.
        //   #2 VisitInventory      -- O(n) enumeration, question 1
        //   #3 GetInventoryWeight  -- O(n) total-weight refresh, question 1
        //   #5 GetValue            -- inner extraLists walk, question 2
        //   #6 GetArmorInSlot      -- O(n) slot lookup
        //   #7 GetWornMask         -- O(n) worn-state query
        //   #8 GetEnchantment      -- inner extraLists walk (second sample)
        //
        // Deliberately absent: #4 SendContainerChangedEvent (its AE relocation is
        // annotated incorrectly upstream -- Offsets.h:339-341 -- and must be
        // disassembled before it is safe to hook) and #9/#10 (write side, lowest
        // priority).
        //
        // Also NOT installed here: target 2.5 (installFxDelegateCallbackHook).
        // Like 2.1 it waits for the InventoryMenu's FxDelegate instance, because
        // that live instance is what validates the vtable it patches -- see
        // MenuOpenCloseListener.
        VisitInventoryHook::install();
        GetInventoryWeightHook::install();
        GetValueHook::install();
        GetArmorInSlotHook::install();
        GetWornMaskHook::install();
        GetEnchantmentHook::install();
#else
        logger::info("Tracy instrumentation disabled (build with -DENABLE_TRACY=ON to profile)");
#endif
    }

    void ProfilingHooks::installTimelineMarkers() {
#ifdef TRACY_ENABLE
        static bool installed = false;
        if (installed) {
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            logger::warn("RE::UI unavailable; Tracy timeline markers not installed");
            return;
        }

        ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuOpenCloseListener);
        installed = true;
        logger::info("Tracy timeline markers installed (InventoryMenu open/close)");
#endif
    }

    void ProfilingHooks::installRequestItemCardInfoHook() {
#ifdef TRACY_ENABLE
        static bool installed = false;
        if (installed) {
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            logger::warn("{}: RE::UI unavailable", RequestItemCardInfoHook::logName);
            return;
        }

        auto menu = ui->GetMenu<RE::InventoryMenu>();
        if (!menu) {
            logger::warn("{}: InventoryMenu not instantiated yet", RequestItemCardInfoHook::logName);
            return;
        }

        // Offset 0x28 on IMenu, filled in when the menu loads its movie.
        if (!menu->fxDelegate) {
            logger::warn("{}: InventoryMenu has no FxDelegate yet", RequestItemCardInfoHook::logName);
            return;
        }

        // Kept as a `const char*` (not a bare string literal) on purpose: this is
        // the exact form FxDelegate::Callback uses to index the very same table
        // (extern/CommonLibSSE-NG/src/RE/F/FxDelegate.cpp:19), and it selects the
        // alt-key overload of GHash::GetAlt. The single definition lives on
        // ProfilingHooks so target 2.5 filters on the identical string.
        static constexpr const char* kMethodName = kItemCardMethodName;
        auto* defn = menu->fxDelegate->callbacks.GetAlt(kMethodName);
        if (!defn || !defn->callback) {
            logger::warn("{}: not registered in the InventoryMenu FxDelegate table", RequestItemCardInfoHook::logName);
            return;
        }

        const auto target = reinterpret_cast<uintptr_t>(defn->callback);
        logger::info("{} resolved: 0x{:X} (SkyrimSE.exe + 0x{:X})", RequestItemCardInfoHook::logName, target,
                     target - REL::Module::get().base());

        if constexpr (BUILDOPTIONS.detoursFound) {
            // Only latch the flag on a successful attach: if Detours refuses (e.g.
            // another mod already trampled the prologue) the next InventoryMenu
            // open retries instead of silently profiling nothing.
            installed = Hooking::writeDetourAt<RequestItemCardInfoHook>(target);
        } else {
            logger::warn("{}: detours unavailable; target 2.1 not hooked", RequestItemCardInfoHook::logName);
        }
#endif
    }

    void ProfilingHooks::installFxDelegateCallbackHook() {
#ifdef TRACY_ENABLE
        static bool installed = false;
        if (installed) {
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return;
        }

        auto menu = ui->GetMenu<RE::InventoryMenu>();
        if (!menu || !menu->fxDelegate) {
            // Not created yet: FxDelegate belongs to the loaded movie. Leave the
            // flag clear so the next menu event retries (measured behaviour of
            // 2.1: `opening` already fires early enough to cover the first open).
            return;
        }

        // Two independent ways to name the same vtable:
        //   - the live instance, i.e. what Scaleform will actually dispatch through
        //   - VTABLE_FxDelegate, i.e. the address library entry for the vtable we
        //     are about to patch
        // Requiring them to be equal validates the address library entry and the
        // slot number below in one step. VTABLE_FxDelegate resolves to the vtable
        // itself, not to a pointer to it (Relocation.h:395 indexes it as
        // `address() + 8 * idx` without dereferencing).
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_FxDelegate[0] };
        const auto instanceVtable = reinterpret_cast<std::uintptr_t>(*reinterpret_cast<void**>(menu->fxDelegate.get()));
        if (vtbl.address() == 0 || vtbl.address() != instanceVtable) {
            // A build/layout problem, not a timing race: retrying cannot help, and
            // patching the wrong slot would corrupt the game in ways no trace
            // could explain. Report and stop.
            logger::error("{}: vtable mismatch -- instance 0x{:X} vs VTABLE_FxDelegate 0x{:X}; target 2.5 not hooked",
                          FxDelegateCallbackHook::logName, instanceVtable, vtbl.address());
            installed = true;
            return;
        }

        // Capture the slot's previous contents BEFORE overwriting it, and note
        // that this order is load-bearing rather than stylistic: the vtable is
        // shared by every menu, so from the instant the slot is patched, a
        // delegate call from ANY menu enters this hook and dereferences `orig`.
        // Filling `orig` after the write would leave a window in which it is
        // still null and the game crashes. Read it the same way write_vfunc()
        // does internally (Relocation.h:395): `address() + 8 * idx`, no
        // dereference of the vtable pointer itself.
        const auto slotAddress = vtbl.address() + sizeof(void*) * FxDelegateCallbackHook::kCallbackVtableSlot;
        const auto previous = *reinterpret_cast<std::uintptr_t*>(slotAddress);
        FxDelegateCallbackHook::orig = reinterpret_cast<FxDelegateCallbackHook::FuncType>(previous);

        vtbl.write_vfunc(FxDelegateCallbackHook::kCallbackVtableSlot, FxDelegateCallbackHook::hook);

        logger::info("{} hook installed at vtable 0x{:X} slot {} (was 0x{:X}, SkyrimSE.exe + 0x{:X})", FxDelegateCallbackHook::logName,
                     vtbl.address(), FxDelegateCallbackHook::kCallbackVtableSlot, previous, previous - REL::Module::get().base());
        installed = true;
#endif
    }

    // The parameter is only touched on the TRACY_ENABLE path below; marking it
    // [[maybe_unused]] keeps a profiling-off build (-DENABLE_TRACY=OFF) clean
    // under /W4, where an unreferenced parameter is C4100.
    void ProfilingHooks::probeDelegateArgs([[maybe_unused]] const RE::FxDelegateArgs& a_params) {
#ifdef TRACY_ENABLE
        static bool done = false;
        if (done) {
            return;
        }
        done = true;

        /*
         * MEASURED RESULT (first live run, 2026-09-20 11:12): argCount = 0.
         *
         * Design section 4.3 inferred args = [uid, processor, index] from the AS
         * source. That inference is REFUTED, and the reason is visible in
         * GameDelegate.call's own signature:
         *   ItemcardDataExtender.as:26  GameDelegate.call("RequestItemCardInfo", [], a_target, "updateItemInfo")
         *   GameDelegate.as:24          params.unshift(methodName, _loc1)  ->  params = ["RequestItemCardInfo", uid]
         *   GameDelegate.as:25          ExternalInterface.call.apply(null, params)
         *   FxDelegate.cpp:21           FxDelegateArgs(a_args[0], handler, movie, &a_args[1], a_argCount - 1)
         *
         * call()'s SECOND argument is the parameter array, and it is EMPTY. The
         * processor and the callback name are call()'s 3rd/4th arguments -- the
         * scope/callback pair -- which never crosses ExternalInterface at all:
         * they go into GameDelegate's responseHash for the AS side's
         * receiveResponse to consume.
         *
         * So the delegate arguments are empty by construction, and the index
         * reaches the engine by a completely different route: _requestItemInfo
         * writes it to `this._selectedIndex` (ItemcardDataExtender.as:25)
         * immediately before the call. That field -- not the delegate arguments --
         * is what the later phase 4 steps must reproduce. See design section 4.3
         * (corrected) and 10.2.
         *
         * The loop below is kept even though it currently prints nothing: it is
         * the assertion that keeps the refutation honest, because if a future UI
         * build does pass arguments, this is where it would show up.
         */
        const auto count = a_params.GetArgCount();
        logger::info("S-1: RequestItemCardInfo argCount = {}", count);
        for (std::uint32_t i = 0; i < count; ++i) {
            logger::info("      args[{}] = {}", i, s1::describe(a_params[i]));
        }
#endif
    }

    // The handler is only touched on the TRACY_ENABLE path below; marking it
    // [[maybe_unused]] keeps a profiling-off build (-DENABLE_TRACY=OFF) clean
    // under /W4, where an unreferenced parameter is C4100. Same treatment as
    // probeDelegateArgs above.
    void ProfilingHooks::installAsPathProbe([[maybe_unused]] RE::FxDelegateHandler* a_handler) {
#ifdef TRACY_ENABLE
        static bool installed = false;
        if (installed) {
            return;
        }

        /*
         * Menu resolution. The handler the delegate call handed us is preferred:
         * RequestItemCardInfo is registered by RE::InventoryMenu, which IS an
         * FxDelegateHandler (IMenu derives from it), so this is the live menu
         * object -- no global lookup, and no dependency on the menu still being
         * registered in RE::UI. The RE::UI path stays as the fallback for any
         * future call site that has no arguments to hand.
         */
        auto* inventoryMenu = a_handler ? static_cast<RE::InventoryMenu*>(a_handler) : nullptr;
        if (!inventoryMenu) {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return;
            }
            auto menu = ui->GetMenu<RE::InventoryMenu>();
            if (!menu) {
                return;
            }
            inventoryMenu = menu.get();
        }

        /*
         * Entry point of the graph: NOT `_root`, and not uiMovie either.
         *
         * InventoryMenu::RUNTIME_DATA::root is a cached display-object handle,
         * documented in CommonLibSSE-NG as "_level0.Menu_mc" -- the menu clip
         * itself, which is where skyui's ItemMenu instance lives (ItemMenu
         * extends MovieClip, ItemMenu.as:15). Two things follow:
         *
         *   - `_root` sits one level ABOVE Menu_mc, which is why the first
         *     version's `_root.inventoryLists.itemList` candidate could never have
         *     matched even before the timing bug was found.
         *   - No absolute path has to be guessed at all: the candidates below are
         *     walked with GetMember from this handle (s1::resolvePath).
         *
         * uiMovie is deliberately only a fallback source for the handle, never a
         * gate: on menu close it is already released, which is precisely how the
         * first version ended up logging nothing at all.
         */
        RE::GFxValue menuRoot = inventoryMenu->GetRuntimeData().root;
        if (!menuRoot.IsObject() && inventoryMenu->uiMovie) {
            inventoryMenu->uiMovie->GetVariable(&menuRoot, "_level0.Menu_mc");
        }
        if (!menuRoot.IsObject()) {
            // Neither a clip handle nor a movie: a lifecycle race. Not latched --
            // the menu can be reopened, and the next RequestItemCardInfo retries.
            return;
        }

        // Past this point the graph is complete by construction (the list is
        // literally being processed right now), so a failure below is a real
        // answer rather than a timing artefact. Latch it and report once.
        installed = true;

        logger::info("=== phase 4 / S-1: ActionScript graph probe (read-only) ===");
        logger::info("S-1: menu clip = InventoryMenu::RUNTIME_DATA::root ({})", s1::describe(menuRoot));

        // (1) What the clip actually carries. ItemMenu declares `inventoryLists`
        // (ItemMenu.as:42), so it is EXPECTED to be there -- and "expected" is
        // exactly what this step replaces with a measurement. This dump is also
        // what keeps a failed path guess diagnosable from the log alone.
        s1::dumpMembers(menuRoot, "_level0.Menu_mc");

        // (2) Candidate paths, most likely first (design section 4.3), as
        // RELATIVE member paths from the menu clip.
        //
        // Two traversal styles are tried and the winning one is logged, because
        // they can disagree: resolvePath walks GetMember from the cached
        // RUNTIME_DATA handle, while GetVariable parses an absolute display path
        // through the movie. If only the latter works, the cached handle is stale
        // and the later phase 4 steps must use absolute paths instead.
        static constexpr const char* kCandidates[] = {
            "inventoryLists.itemList",  // ItemMenu.as:42 -> InventoryLists.itemList
            "itemList",                 // ItemMenu.as:201 (a `get itemList()`)
        };

        RE::GFxValue list;
        const char* found = nullptr;
        const char* how = nullptr;
        for (const auto* path : kCandidates) {
            RE::GFxValue candidate;
            if (s1::resolvePath(menuRoot, path, candidate) && s1::looksLikeInventoryList(candidate)) {
                list = candidate;
                found = path;
                how = "GetMember from the cached clip handle";
                break;
            }
            // Absolute fallback for the same candidate, so that a stale handle is
            // distinguishable from a wrong path.
            const std::string absolute = std::string("_level0.Menu_mc.") + path;
            if (inventoryMenu->uiMovie && inventoryMenu->uiMovie->GetVariable(&candidate, absolute.c_str()) &&
                s1::looksLikeInventoryList(candidate)) {
                list = candidate;
                found = path;
                how = "GetVariable on the absolute path (cached handle is stale)";
                break;
            }
        }
        if (!found) {
            logger::warn("S-1: no candidate path resolved to an inventory list -- read the member dump above");
            return;
        }
        logger::info("S-1: inventory list found at `{}` via {}", found, how);

        // (3) The two arrays phase 4 needs, and which processor slot is its target.
        RE::GFxValue entries;
        if (list.GetMember("entryList", &entries) && entries.IsArray()) {
            logger::info("S-1: entryList = array[{}]  (compare against the per-round RequestItemCardInfo count)",
                         entries.GetArraySize());
        }
        s1::dumpProcessors(list);
        logger::info("=== phase 4 / S-1: probe done ({} log lines) ===", s1::g_logLines);
#endif
    }

    void ProfilingHooks::requestAsProbeInstall() {
#ifdef TRACY_ENABLE
        // Runs on the menu-open event, so it must NOT touch the graph: uiMovie and
        // the list are rebuilt from here on, and installAsPathProbe already
        // established WHERE the graph is reachable. The event's only job is to mark
        // "the movie built from now on has not been patched yet". Re-arming an
        // already armed probe is a no-op by construction, which is what makes the
        // event safe to fire more than once.
        s0::g_installPending = true;
        s0::g_installAttempts = 0;
#endif
    }

    // The handler is only touched on the TRACY_ENABLE path below; marking it
    // [[maybe_unused]] keeps a profiling-off build (-DENABLE_TRACY=OFF) clean
    // under /W4, where an unreferenced parameter is C4100. Same treatment as
    // probeDelegateArgs / installAsPathProbe above.
    void ProfilingHooks::maybeInstallAsProbe([[maybe_unused]] RE::FxDelegateHandler* a_handler) {
#ifdef TRACY_ENABLE
        // Hot path: one bool read per item once the probe is in place. Everything
        // after this guard runs at most kMaxInstallAttempts times per round.
        if (!s0::g_installPending) {
            return;
        }
        if (++s0::g_installAttempts > s0::kMaxInstallAttempts) {
            s0::g_installPending = false;
            logger::warn("S0: `_dataProcessors[{}]` unreachable after {} attempts; all 3 members left as ActionScript "
                         "for this round",
                         s0::kTargetProcessorSlot, s0::kMaxInstallAttempts);
            return;
        }

        // Same menu resolution as installAsPathProbe, for the same reason: the
        // handler the delegate call handed us IS the live RE::InventoryMenu
        // (IMenu derives from FxDelegateHandler), which avoids a second lookup and
        // a dependency on the menu still being registered. RE::UI stays as the
        // fallback for any future call site with no arguments to hand.
        auto* inventoryMenu = a_handler ? static_cast<RE::InventoryMenu*>(a_handler) : nullptr;
        if (!inventoryMenu) {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return;
            }
            auto menu = ui->GetMenu<RE::InventoryMenu>();
            if (!menu) {
                return;
            }
            inventoryMenu = menu.get();
        }

        if (s0::install(inventoryMenu)) {
            // Patched: disarm until the next menu open rebuilds the list.
            s0::g_installPending = false;
            return;
        }

        // Left armed on purpose: the list can legitimately still be filling up on
        // the very first item, so the next call retries. The attempt cap above is
        // what bounds the cost of being wrong.
        logger::info("S0: target not reachable yet (attempt {} of {}); retrying on the next item", s0::g_installAttempts,
                     s0::kMaxInstallAttempts);
#endif
    }
}  // namespace plugin
