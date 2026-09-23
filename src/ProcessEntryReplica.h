#pragma once

#include <RE/G/GFxValue.h>

/*
 * The 4b-ii master switch, defined in exactly ONE place.
 *
 * A preprocessor macro rather than a `constexpr bool` -- which is what 4a's
 * `kAnswerFromCache` uses -- because this step adds a whole translation unit: with
 * the replica compiled out, `ProcessEntryReplica.cpp` becomes an EMPTY translation
 * unit and `ProfilingHooks.cpp` contains no reference to `pe::` at all, so the
 * section 8.2 revert claim ("flip the switch, rebuild, get the previous artifact
 * back") is constructive rather than a behaviour argument. A `constexpr` could not
 * do that: the dead branch would still have to name `pe::begin`, so the code would
 * still have to exist and link.
 *
 * REVERT: set this to 0 (or `-DSSE_REPLICATE_PROCESS_ENTRY=0`) and rebuild. Every
 * item takes the untouched ActionScript path again, and the `AS::processEntry` zone
 * measures exactly what capture #11b measured.
 */
#ifndef SSE_REPLICATE_PROCESS_ENTRY
    #define SSE_REPLICATE_PROCESS_ENTRY 1
#endif

namespace RE {
    class GFxMovie;
}

namespace plugin {
    /*
     * =========================================================================
     * Phase 4b / step S4: the C++ replica of `InventoryDataSetter.processEntry`.
     *
     * Design: docs/phase4-design.md section 6 (4b-ii), 8.1 row S4, 8.2 (revert
     * contract). Implementation record: section 10.9.
     *
     * WHAT IS BEING REPLACED. `InventoryDataSetter.as` is 1304 lines in the v6.11
     * tree: a 73-line `processEntry` (:7-79) plus 20 private helpers (:80-1303),
     * 380 `case` labels and 173 `Translator.translate` calls over 103 distinct
     * literals. It is the single largest undivided cost the S0b probe found --
     * S4, ~256 ms/round of the 804.6 ms/round measured in capture #11b.
     *
     * HOW IT IS REPLACED, and why the shape is unusual. This is NOT a cache: the
     * replica recomputes every item from the engine's own entry object on every
     * call, so there is no cross-open staleness to reason about and no
     * invalidation contract to keep (contrast 4a, whose whole risk is "the cache
     * and the real inventory disagree"). What it removes is the AS2 interpreter:
     * the same member reads and the same assignments, executed by compiled code.
     *
     * THE ONE THING C++ CANNOT COMPUTE. `Translator.translate`
     * (extern/.../Common/skyui/util/Translator.as, `translate`) is not a lookup
     * table -- it is `_root.createTextField(...)` plus `_translator.text = a_str`
     * plus reading `.text` back, i.e. it asks the GAME's localization system for
     * the string. There is no C++ equivalent of "what does `$Scroll` localize
     * to", so the 103 literals are resolved ONCE per process by calling the AS
     * function, and the results are held in the translation table. On the hot
     * path a display string is therefore one table index, which is strictly
     * cheaper than the AS body's own call. If the resolution fails, the replica
     * declines entirely -- see `prepare`.
     *
     * FAIL-CLOSED, AND WHY THE OUTPUT IS BUILT BEFORE ANYTHING IS WRITTEN.
     * Rule 4 of section 8.2 is per ITEM here: any member whose runtime type the
     * replica cannot reason about makes that ONE call fall back to the untouched
     * AS body. For that fallback to be safe it has to be invisible -- the AS body
     * must see the entry exactly as the previous call left it. So `predict` reads
     * everything and writes nothing, and only a complete prediction reaches
     * `apply`. A call that bails out halfway through the reads therefore leaves
     * the movie untouched by construction rather than by discipline.
     *
     * The 20-odd members written per item are collected into `WriteSet` first for
     * the same reason, and that is also what makes the shadow comparison below
     * possible at all: the replica's output exists as data before it exists as
     * movie state.
     * =========================================================================
     */
    namespace pe {
        /*
         * The 103 distinct `Translator.translate` literals of
         * InventoryDataSetter.as, in order of first appearance, EXTRACTED from the
         * AS source with a script rather than typed -- a hand-copied table is
         * exactly the kind of artifact where one wrong character produces a
         * plausible-looking wrong display string on one branch only. The two
         * `static_assert`s below pin both the order and the count, so a future
         * edit to the .as file cannot silently desynchronise them.
         *
         * v6.11 RE-EXTRACTION. The table was re-extracted from the SkyUI-Community
         * v6.11 tree (the clone under `extern/`, path
         * source/actionscript/ItemMenus/InventoryDataSetter.as) after the running
         * SWF was identified as that build -- see docs/skyui-version-divergence.md
         * and section 10.10 of the phase 4 design. The previous 102-entry table
         * came from the 2015 SkyUI 5.1 tree, which is a different generation of
         * the file: 20 literals no longer exist and 21 are new, so this is a
         * re-extraction rather than an edit of the old list.
         */
        enum class T : std::size_t {
            kScroll,
            kIngredient,
            kTorch,
            kOther,
            kLight,
            kHeavy,
            kClothing,
            kJewelry,
            kDaedric,
            kDragon,
            kDwarven,
            kEbony,
            kElven,
            kGlass,
            kHide,
            kStormcloak,
            kImperial,
            kIron,
            kLeather,
            kOrcish,
            kSteel,
            kSilver,
            kFalmer,
            kBonemold,
            kChitin,
            kNordic,
            kStalhrim,
            kOrdinator,
            kAmber,
            kMadness,
            kWood,
            kWeapon,
            kMelee,
            kFishingRod,
            kSword,
            kDagger,
            kWarAxe,
            kMace,
            kGreatsword,
            kBattleaxe,
            kWarhammer,
            kBow,
            kStaff,
            kCrossbow,
            kPickaxe,
            kWoodAxe,
            kHead,
            kBody,
            kHands,
            kForearms,
            kAmulet,
            kRing,
            kFeet,
            kCalves,
            kShield,
            kCirclet,
            kEars,
            kTail,
            kClothingCloak,
            kBackpack,
            kBook,
            kNote,
            kRecipe,
            kSpellTome,
            kArrow,
            kBolt,
            kKey,
            kPotion,
            kFood,
            kDrink,
            kPoison,
            kHealth,
            kMagicka,
            kStamina,
            kSoulGem,
            kSoulTomato,
            kMisc,
            kToy,
            kHousePart,
            kArtifact,
            kGem,
            kTool,
            kRemains,
            kIngot,
            kFirewood,
            kClutter,
            kClaw,
            kLockpick,
            kGold,
            kStrips,
            kNetchLeather,
            kBrokenWeapon,
            kDwarvenScrap,
            kInstrument,
            kBugJar,
            kMap,
            kOre,
            kHorseTack,
            kBuildingMaterial,
            kScrollSpider,
            kPetGear,
            kAyleidCrystal,
            kElderScroll,
            kCount,
            /*
             * NOT a translation key: the one display string the AS body writes as a
             * RAW literal instead of going through `Translator.translate`. It sits
             * after `kCount` on purpose, so `kTranslationCount` (== `kCount`) and both
             * `static_assert`s above are untouched, and the string's managed copy is
             * cached separately rather than as a table entry -- there is no literal to
             * resolve, so a keyed entry would be the wrong shape. See `putLiteral`.
             */
            kRawDefaultPotion
        };

        inline constexpr const char* kTranslationKeys[] = {
            "$Scroll",         "$Ingredient",  "$Torch",          "$Other",         "$Light",          "$Heavy",
            "$Clothing",       "$Jewelry",     "$Daedric",        "$Dragon",        "$Dwarven",        "$Ebony",
            "$Elven",          "$Glass",       "$Hide",           "$Stormcloak",    "$Imperial",       "$Iron",
            "$Leather",        "$Orcish",      "$Steel",          "$Silver",        "$Falmer",         "$Bonemold",
            "$Chitin",         "$Nordic",      "$Stalhrim",       "$Ordinator",     "$Amber",          "$Madness",
            "$Wood",           "$Weapon",      "$Melee",          "$FishingRod",    "$Sword",          "$Dagger",
            "$War Axe",        "$Mace",        "$Greatsword",     "$Battleaxe",     "$Warhammer",      "$Bow",
            "$Staff",          "$Crossbow",    "$Pickaxe",        "$Wood Axe",      "$Head",           "$Body",
            "$Hands",          "$Forearms",    "$Amulet",         "$Ring",          "$Feet",           "$Calves",
            "$Shield",         "$Circlet",     "$Ears",           "$Tail",          "$ClothingCloak",  "$Backpack",
            "$Book",           "$Note",        "$Recipe",         "$Spell Tome",    "$Arrow",          "$Bolt",
            "$Key",            "$Potion",      "$Food",           "$Drink",         "$Poison",         "$Health",
            "$Magicka",        "$Stamina",     "$Soul Gem",       "$SoulTomato",    "$Misc",           "$Toy",
            "$House Part",     "$Artifact",    "$Gem",            "$Tool",          "$Remains",        "$Ingot",
            "$Firewood",       "$Clutter",     "$Claw",           "$Lockpick",      "$Gold",           "$Strips",
            "$NetchLeather",   "$BrokenWeapon", "$DwarvenScrap",  "$Instrument",    "$BugJar",         "$Map",
            "$Ore",            "$HorseTack",   "$BuildingMaterial", "$ScrollSpider", "$PetGear",        "$AyleidCrystal",
            "$ElderScroll",
        };

        constexpr std::size_t kTranslationCount = static_cast<std::size_t>(T::kCount);
        static_assert(std::size(kTranslationKeys) == kTranslationCount,
                      "kTranslationKeys must have exactly one key per T value.");
        static_assert(kTranslationCount == 103, "InventoryDataSetter.as changed; re-extract the 103 literals.");

        // The one display string the AS body writes as a RAW literal rather than
        // through `Translator.translate`: `iconLabel` (`InventoryDataSetter.as:970`).
        // Deliberately NOT a member of `kTranslationKeys` -- there is nothing to
        // translate, so it has no key, and its managed copy is cached separately. See
        // `WriteSet::putLiteral` and `T::kRawDefaultPotion`.
        inline constexpr const char* kRawDefaultPotionText = "default_potion";

        // The resolved display string for one literal. Only meaningful after
        // `prepare` has returned true: the table is filled all-or-nothing, so a
        // half-resolved table is never handed out.
        const char* translation(T a_key);

        // True once every literal has been resolved against the movie.
        bool ready();

        /*
         * One member write the replica has decided on but has not performed yet.
         *
         * `name` and `text` are BORROWED and must outlive the WriteSet: `name` is
         * always a string literal, and `text` is either a translation-table entry
         * or a literal, never a temporary. That is what keeps the hot path free of
         * allocation -- the only thing `apply` allocates is the managed AS string
         * itself, which the AS body allocates too.
         */
        struct Write {
            enum class Kind : std::uint8_t {
                kUndefined,
                kNull,
                kBoolean,
                kNumber,
                kString,
            };

            const char* name{ nullptr };
            Kind        kind{ Kind::kUndefined };
            bool        boolean{ false };
            double      number{ 0.0 };
            // For kString: the resolved display text, kept ONLY for the shadow
            // comparison's `strcmp` and for the mismatch log line. It is not what
            // gets written -- see `key`.
            const char* text{ nullptr };
            /*
             * For kString: which of the cached managed strings to write.
             *
             * Why a key rather than `text` itself: the write has to hand the movie a
             * string it OWNS (see `apply`), and a managed `GFxValue` is bound to the
             * movie's lifetime. Creating one per item -- which is what the AS body
             * effectively does, because `Translator.translate` returns a fresh
             * managed string on every call -- costs one movie-heap allocation per
             * display string per item. Caching the 103 of them once per movie turns
             * that into an array index, which is the whole point of this field.
             *
             * `kCount` is the "not a string write" sentinel; the non-string arms of
             * `apply` never read it. The value just past it, `kRawDefaultPotion`,
             * selects the second cache: the one member the AS body writes as a raw
             * literal rather than through `Translator.translate` -- see `T` and
             * `putLiteral`.
             */
            T key{ T::kCount };
        };

        /*
         * How many members one `processEntry` call can write. Measured, not
         * guessed: the widest branch is TYPE_ARMOR, whose 7 common writes (baseId,
         * type, isEquipped, isStolen, infoValue, infoWeight, infoValueWeight) are
         * followed by isEnchanted, infoArmor, weightClassDisplay, weightClass,
         * subType, subTypeDisplay, mainPartMask, material, materialDisplay and two
         * repeats of members already in the set -- 18 distinct, and `put` collapses
         * repeats. 32 leaves room for a third pass without making reallocation an
         * option anywhere.
         */
        inline constexpr std::size_t kMaxWrites = 32;

        struct WriteSet {
            Write       writes[kMaxWrites];
            std::size_t count{ 0 };

            void reset() { count = 0; }

            /*
             * Records one assignment. Writing the same member twice REPLACES the
             * earlier record instead of appending, because that is what the AS
             * body's sequential assignments amount to -- the last value is the only
             * one anything downstream can observe. The order of first appearance is
             * therefore also the replica's execution order, which is what makes a
             * mismatch log line readable.
             */
            void put(const char* a_name, Write::Kind a_kind, bool a_boolean, double a_number, const char* a_text,
                     T a_key);

            void putUndefined(const char* a_name) { put(a_name, Write::Kind::kUndefined, false, 0.0, nullptr, T::kCount); }
            void putNull(const char* a_name) { put(a_name, Write::Kind::kNull, false, 0.0, nullptr, T::kCount); }
            void putBoolean(const char* a_name, bool a_value) {
                put(a_name, Write::Kind::kBoolean, a_value, 0.0, nullptr, T::kCount);
            }
            void putNumber(const char* a_name, double a_value) {
                put(a_name, Write::Kind::kNumber, false, a_value, nullptr, T::kCount);
            }

            /*
             * One TRANSLATED display-string write, by KEY rather than by text. The
             * replica's other string write -- the single raw literal -- goes through
             * `putLiteral` below; there is still deliberately no
             * `putString(name, const char*)` that accepts an arbitrary pointer,
             * because a caller that could pass one would be able to produce a Write
             * `apply` cannot satisfy without allocating, which is the cost this key
             * exists to remove.
             *
             * The text is still recorded, from the same process-lifetime table, so
             * the shadow comparison and the log lines are unaffected by the caching.
             */
            void putTranslation(const char* a_name, T a_key) {
                put(a_name, Write::Kind::kString, false, 0.0, translation(a_key), a_key);
            }

            /*
             * The one display string the AS body does NOT translate: `iconLabel`
             * (`InventoryDataSetter.as:970` writes `iconLabel = "default_potion"`).
             *
             * Same shape as `putTranslation` -- `text` is still recorded, from a
             * process-lifetime pointer, because the shadow comparison `strcmp`s it --
             * but keyed to the single-value raw-literal cache instead of the table.
             * `apply` picks the cache from `key`, never from the text, so a literal
             * cannot arrive through the translation arm and an unresolved translation
             * cannot arrive here.
             */
            void putLiteral(const char* a_name, const char* a_literal) {
                put(a_name, Write::Kind::kString, false, 0.0, a_literal, T::kRawDefaultPotion);
            }

            // What the replica decided for one member, or null if it decided not to
            // touch it. Used by the shadow comparison to report a member the AS body
            // wrote that the replica did not.
            [[nodiscard]] const Write* find(const char* a_name) const;
        };

        /*
         * Resolves the translation table from the live movie. Idempotent, and
         * deliberately NOT one-shot on failure: it is retried on later calls
         * because a route can become available once the menu has settled (the
         * `_translator` TextField route below is exactly such a case), and the cost
         * of a retry is a handful of flag reads on the hot path.
         *
         * Returns false when the table is not (yet) usable, which is the caller's
         * signal to forward the call untouched.
         */
        bool prepare(RE::GFxMovie* a_movie);

        /*
         * What the caller has to do with the call.
         *
         * The three answers are ordered by what has already happened to the movie:
         * `kForwardOnly` and `kPredictNoWrite` have written nothing at all, so
         * forwarding is the original behaviour, verbatim and unobservable.
         * `kHandled` means the replica wrote its prediction and the original must
         * NOT run.
         */
        enum class Stage : std::uint8_t {
            kForwardOnly,     // could not predict; forward
            kPredictNoWrite,  // shadow sample: forward, then call finishValidation
            kHandled,         // answered in C++
        };

        /*
         * The whole decision for one item: resolve, predict, and either write or
         * ask the caller to forward. `a_out` is filled with the prediction on
         * BOTH of the non-forwarding paths, so the shadow comparison can diff it
         * against what the AS body actually wrote.
         *
         * A WriteSet rather than a `std::vector` because this runs 6441 times a
         * round: 32 * 24 bytes on the caller's stack replaces 6441 heap
         * allocations, and the plan's whole point is that the allocation traffic
         * is what a heap pause is made of.
         */
        Stage begin(RE::GFxMovie* a_movie, RE::GFxValue& a_entry, const RE::GFxValue& a_item_info, WriteSet& a_out);

        /*
         * The second half of the shadow sample. Called AFTER the caller has
         * forwarded, so `_itemInfo` and the entry carry the AS body's own output:
         * read each predicted member back and compare it field by field.
         */
        void finishValidation(RE::GFxMovie* a_movie, const RE::GFxValue& a_entry, const WriteSet& a_predicted);

        /*
         * Per-round accounting, printed and reset by the S0a/S0b `install()` --
         * which runs exactly once per round, on the round's first
         * RequestItemCardInfo, i.e. when the previous round's numbers are final.
         * Same placement and same reason as 4a's reportAndResetRoundStats.
         */
        void reportAndResetRoundStats();
    }
}  // namespace plugin
