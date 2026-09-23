#include "ProcessEntryReplica.h"

#include <RE/G/GFxMovie.h>
#include <RE/G/GFxValue.h>

/*
 * Everything below is the 4b-ii replica, compiled only while
 * SSE_REPLICATE_PROCESS_ENTRY is 1 (see the switch's comment in the header). With
 * it at 0 this file is an empty translation unit, which is what makes section
 * 8.2's revert contract a property of the build rather than of the code's shape.
 */
#if SSE_REPLICATE_PROCESS_ENTRY

/*
 * =============================================================================
 * The C++ replica of `InventoryDataSetter.processEntry`
 *
 * BASELINE: SkyUI-Community **v6.11** -- the build the game actually runs. The
 * source of truth is the tree under `extern/`
 * (source/actionscript/ItemMenus/InventoryDataSetter.as, 1304 lines: a 73-line
 * `processEntry` at :7-79 plus 20 private helpers at :80-1303). This file was
 * first transcribed from `schlangster/skyui` master (2015, "SkyUI 5.1"), which
 * turned out to be a different generation of the same class -- see
 * docs/skyui-version-divergence.md for how that was established and for the
 * re-transcription record. Every line reference in the comments below is a v6.11
 * line number.
 *
 * READING THIS FILE. Every function below is a transcription of one AS function
 * and carries the AS line numbers in its comment, so the two can be diffed side
 * by side. The transcription is deliberately literal rather than table-driven:
 * the ~450 `case` labels rewritten as data would be a second implementation of
 * the same thing, and the failure mode of a data table -- one wrong entry, one
 * silently different display string on one branch -- is exactly what a
 * line-by-line transcription makes reviewable. For the same reason the AS-side
 * constants (Form.TYPE_*, Armor.PARTMASK_*, Material.*, ...) are written as
 * literals with their AS names in a trailing comment rather than as ~290 named
 * C++ constants: the reviewer needs to see `0x0139C0` and `BASEID_DAEDRICARROW`
 * on the same line, and the id tables were extracted from Form.as with a script
 * rather than typed (docs/skyui-version-divergence.md section 7).
 *
 * WHAT IS *NOT* TRANSCRIBED, and why (each was checked against the AS source
 * rather than assumed):
 *
 *   - `processList` (ItemcardDataExtender.as:40-58). S3, out of scope for
 *     4b-ii, and capture #11b could not measure it anyway.
 *   - `fixSKSEExtendedObject` (:65-119). It runs in `processList` on the way in,
 *     so by the time `processEntry` is called the entry is already fixed up --
 *     which is why this file can read `weaponType` and `flags` directly.
 *   - `this` / `_selectedIndex`: `processEntry` never touches the processor.
 *     4a's block comment records the same conclusion for the sibling method.
 *
 * THE THREE AS2 SEMANTIC TRAPS this file exists to get right. All three are
 * places where "close enough in C++" is a different answer, and all three were
 * settled from the ES3 rules AS2 implements rather than by picking whichever
 * reading looked plausible:
 *
 *   1. `!=` is NOT `!(== 0)`. `undefined != ""` and `null != ""` are both TRUE
 *      (ES3 11.9.3: null/undefined compare equal to nothing but each other, and
 *      the String/Number coercion steps never run). So an entry whose `effects`
 *      member is ABSENT reads as "enchanted" in the original AS body, and
 *      `as2NotEqualEmptyString` reproduces that instead of quietly fixing it.
 *
 *   2. `switch` uses STRICT equality, not `==`. `switch (undefined)` therefore
 *      matches no case at all, while `case 0:` matches only the Number 0.
 *      Comparing with `==` would make `null` and `0` the same branch.
 *
 *   3. `null` and `undefined` read the same on an object (`x.foo` is `undefined`
 *      either way), so an absent member and an explicit `undefined` must not be
 *      distinguished. `readMember` reports both as kUndefined on purpose.
 * =============================================================================
 */
namespace plugin::pe {
    namespace {
        /*
         * Armor.PARTMASK_PRECEDENCE (Armor.as:43-74), in the array's own order.
         *
         * The values are held as the NUMBERS the AS array literal holds, not as
         * 32-bit masks: `0x80000000` is 2147483648 in AS2, because Numbers are
         * doubles, and `processArmorPartMask` stores that element straight back
         * into `mainPartMask` and then `switch`es on it in that positive form.
         * `partMaskToInt32` is the one place where the int32 view IS the right
         * one -- and it is applied to `partMask`, never to these.
         *
         * The order is load-bearing: the first entry that matches wins, so this
         * is not a set.
         */
        constexpr double kPartMaskPrecedence[] = {
            0x00000004,  // Armor.PARTMASK_BODY
            0x00000002,  // Armor.PARTMASK_HAIR
            0x00000008,  // Armor.PARTMASK_HANDS
            0x00000010,  // Armor.PARTMASK_FOREARMS
            0x00000080,  // Armor.PARTMASK_FEET
            0x00000100,  // Armor.PARTMASK_CALVES
            0x00000200,  // Armor.PARTMASK_SHIELD
            0x00000020,  // Armor.PARTMASK_AMULET
            0x00000040,  // Armor.PARTMASK_RING
            0x00000800,  // Armor.PARTMASK_LONGHAIR
            0x00002000,  // Armor.PARTMASK_EARS
            0x00000001,  // Armor.PARTMASK_HEAD
            0x00001000,  // Armor.PARTMASK_CIRCLET
            0x00000400,  // Armor.PARTMASK_TAIL
            0x00004000,  // Armor.PARTMASK_UNNAMED14
            0x00008000,  // Armor.PARTMASK_UNNAMED15
            0x00010000,  // Armor.PARTMASK_CLOAK    -- named UNNAMED16 before v6.11
            0x00020000,  // Armor.PARTMASK_BACKPACK -- named UNNAMED17 before v6.11
            0x00040000,  // Armor.PARTMASK_UNNAMED18
            0x00080000,  // Armor.PARTMASK_UNNAMED19
            0x00100000,  // Armor.PARTMASK_DECAPITATEHEAD
            0x00200000,  // Armor.PARTMASK_DECAPITATE
            0x00400000,  // Armor.PARTMASK_UNNAMED22
            0x00800000,  // Armor.PARTMASK_UNNAMED23
            0x01000000,  // Armor.PARTMASK_UNNAMED24
            0x02000000,  // Armor.PARTMASK_UNNAMED25
            0x04000000,  // Armor.PARTMASK_UNNAMED26
            0x08000000,  // Armor.PARTMASK_UNNAMED27
            0x10000000,  // Armor.PARTMASK_UNNAMED28
            0x20000000,  // Armor.PARTMASK_UNNAMED29
            0x40000000,  // Armor.PARTMASK_UNNAMED30
            0x80000000,  // Armor.PARTMASK_FX01
        };

        /* ---------------------------------------------------------------------
         * AS2 primitives.
         * ------------------------------------------------------------------- */

        /*
         * The runtime type of one member read. Collapsing "absent" into
         * `kUndefined` is deliberate and is not a simplification: reading a member
         * that does not exist yields `undefined` in AS2, so the AS body cannot tell
         * the two apart either (trap 3 in the file header).
         */
        enum class ValKind : std::uint8_t {
            kUndefined,
            kNull,
            kBoolean,
            kNumber,
            kString,
            kObject,
            kOther,  // stringW, display object, anything the replica does not model
        };

        const char* valKindName(ValKind a_kind) {
            switch (a_kind) {
                case ValKind::kUndefined: return "undefined";
                case ValKind::kNull: return "null";
                case ValKind::kBoolean: return "bool";
                case ValKind::kNumber: return "number";
                case ValKind::kString: return "string";
                case ValKind::kObject: return "object";
                default: return "unmodelled";
            }
        }

        ValKind valKindOf(const RE::GFxValue& a_value) {
            if (a_value.IsNumber()) return ValKind::kNumber;
            if (a_value.IsBool()) return ValKind::kBoolean;
            if (a_value.IsString()) return ValKind::kString;
            if (a_value.IsNull()) return ValKind::kNull;
            if (a_value.IsUndefined()) return ValKind::kUndefined;
            if (a_value.IsObject()) return ValKind::kObject;
            return ValKind::kOther;
        }

        /*
         * `Math.round`. ES3's definition is "if two values are equally close, the
         * LARGER wins", i.e. `floor(x + 0.5)`, which is why `Math.round(-0.5)` is
         * `-0` and not `-1`. `std::round` rounds half AWAY FROM ZERO and would
         * differ on every negative input; `std::llround` also returns an integer
         * type, which would truncate the `0.01` resolution the AS relies on.
         */
        double as2Round(double a_value) { return std::floor(a_value + 0.5); }

        /*
         * ES3's ToInt32, which is what AS2's `&` operator applies to both operands.
         * Written out rather than cast because the interesting inputs are the ones
         * a cast gets wrong: NaN becomes 0 (never INT_MIN), and values outside the
         * int32 range wrap modulo 2^32 the way the spec says rather than being
         * undefined behaviour.
         */
        std::int32_t as2ToInt32(double a_value) {
            if (!std::isfinite(a_value)) {
                return 0;
            }
            double wrapped = std::fmod(std::trunc(a_value), 4294967296.0);
            if (wrapped < 0.0) {
                wrapped += 4294967296.0;
            }
            if (wrapped >= 2147483648.0) {
                wrapped -= 4294967296.0;
            }
            return static_cast<std::int32_t>(wrapped);
        }

        // The int32 view of a Number, for the places AS2's `&` uses it:
        // `partMask & mask` (InventoryDataSetter.as:422) and
        // `(flags & flag) != 0` (:555, :576, :675, :685).
        std::uint32_t as2Bits(double a_value) { return static_cast<std::uint32_t>(as2ToInt32(a_value)); }

        /*
         * `v > 0` in AS2.
         *
         * Only kNumber, kBoolean, kUndefined and kNull can reach this function:
         * `gather` rejects every other kind for every member it feeds in, and a
         * rejection forwards that item rather than guessing. That is why the
         * `default` arm is an honest `false` instead of a string-parsing branch
         * that could never run and could never be tested.
         */
        bool as2GreaterThanZero(const RE::GFxValue& a_value, ValKind a_kind) {
            switch (a_kind) {
                case ValKind::kNumber:
                    return a_value.GetNumber() > 0.0;
                case ValKind::kBoolean:
                    return a_value.GetBool();
                default:
                    // undefined and null both go through ToNumber to NaN / 0, and
                    // neither is greater than 0.
                    return false;
            }
        }

        /*
         * `v == true` in AS2, i.e. ES3 11.9.3 step 4: a Boolean operand is
         * converted to a Number and the comparison restarts. `1 == true` is true,
         * `2 == true` is false, and `undefined == true` is false because the
         * restarted comparison is Number-vs-Undefined, which has no rule.
         */
        bool as2EqualsTrue(const RE::GFxValue& a_value, ValKind a_kind) {
            switch (a_kind) {
                case ValKind::kBoolean:
                    return a_value.GetBool();
                case ValKind::kNumber:
                    return a_value.GetNumber() == 1.0;
                default:
                    return false;
            }
        }

        /*
         * `v != ""` in AS2, which is NOT `!(v == 0)` -- trap 1 in the file header.
         *
         * The two arms that matter are kNumber and kBoolean, where `""` coerces to
         * 0 and the comparison DOES run, versus kNull / kUndefined, where ES3
         * finds no applicable rule and the answer is "not equal". Writing this as
         * a numeric comparison would silently turn an ABSENT `effects` member from
         * `isEnchanted = true` into `isEnchanted = false`.
         *
         * kObject is the one kind the replica declines: `==` against an object
         * would run ToPrimitive on it, and modelling a path the engine has never
         * produced is not worth the risk. `a_understood` carries that refusal out
         * so the caller can forward the item instead of answering it.
         */
        bool as2NotEqualEmptyString(const RE::GFxValue& a_value, ValKind a_kind, bool& a_understood) {
            a_understood = true;
            switch (a_kind) {
                case ValKind::kString: {
                    const char* text = a_value.GetString();
                    return text && *text != '\0';
                }
                case ValKind::kNumber:
                    return a_value.GetNumber() != 0.0;
                case ValKind::kBoolean:
                    return a_value.GetBool();
                case ValKind::kNull:
                case ValKind::kUndefined:
                    return true;
                default:
                    a_understood = false;
                    return false;
            }
        }

        /*
         * `a_keywords["Name"] != undefined` -- the test every keyword branch of the
         * AS body uses (InventoryDataSetter.as:135, :153, :294, :372, :565, :782,
         * :791, :802, ...).
         *
         * Note what it is NOT: it is not `HasMember`. A keyword that IS present
         * with the value `null` fails this test in AS2, because `null == undefined`
         * and the operand order does not change that. Both halves are therefore
         * required, and using HasMember alone would enter a branch the original AS
         * body never takes.
         */
        bool keywordsHas(const RE::GFxValue& a_keywords, const char* a_name) {
            RE::GFxValue value;
            if (!a_keywords.GetMember(a_name, &value)) {
                return false;
            }
            return !value.IsUndefined() && !value.IsNull();
        }

        /* ---------------------------------------------------------------------
         * Reading the entry / itemInfo objects.
         * ------------------------------------------------------------------- */

        /*
         * One member read out of an entry object: the value, and the type it
         * actually had.
         *
         * Why the Value and not just a C++ number: every operator the AS body uses
         * is type-dependent (trap 1/2 in the file header), so the type travels with
         * the value all the way to the operator instead of being flattened at the
         * read. A `double`-only context would have to re-derive "was this present"
         * from a sentinel, which is the classic way an absent member becomes a 0.
         */
        struct Slot {
            RE::GFxValue value;
            ValKind      kind{ ValKind::kUndefined };

            [[nodiscard]] bool isNumber() const { return kind == ValKind::kNumber; }
            [[nodiscard]] bool isAbsent() const { return kind == ValKind::kUndefined || kind == ValKind::kNull; }
            [[nodiscard]] bool isObject() const { return kind == ValKind::kObject; }
            [[nodiscard]] double number() const { return value.GetNumber(); }
        };

        /*
         * Reads one member. Returns false for an absent member, but fills the slot
         * with `undefined` and kUndefined rather than leaving it blank -- an absent
         * member IS `undefined` to the AS body (trap 3), so the caller does not have
         * to special-case it and cannot accidentally treat "no such member" as 0.
         */
        bool readSlot(const RE::GFxValue& a_object, const char* a_name, Slot& a_out) {
            if (!a_object.GetMember(a_name, &a_out.value)) {
                a_out.value.SetUndefined();
                a_out.kind = ValKind::kUndefined;
                return false;
            }
            a_out.kind = valKindOf(a_out.value);
            return true;
        }

        // ToInt32 of a member `gather` has already certified as number-or-absent.
        // The absent arm is not a fallback: ES3 converts `undefined` to NaN and
        // ToInt32 turns NaN into 0, which is exactly what the AS bitwise operators
        // do with a missing member.
        std::int32_t slotToInt32(const Slot& a_slot) { return as2ToInt32(a_slot.isNumber() ? a_slot.number() : 0.0); }

        constexpr bool isNumberOrAbsent(ValKind a_kind) {
            return a_kind == ValKind::kNumber || a_kind == ValKind::kUndefined || a_kind == ValKind::kNull;
        }
        constexpr bool isObjectOrAbsent(ValKind a_kind) {
            return a_kind == ValKind::kObject || a_kind == ValKind::kUndefined || a_kind == ValKind::kNull;
        }
        // `stolen` / `poisoned`: the engine has only ever produced a bool here
        // (capture #11b), and a Number compares the same for both uses, so both are
        // admitted rather than making a Number forward the item.
        constexpr bool isFlagOrAbsent(ValKind a_kind) { return a_kind == ValKind::kBoolean || isNumberOrAbsent(a_kind); }
        // `effects`: a String is the observed type, and the `!= ""` test is the one
        // place a scalar would still be well defined (see as2NotEqualEmptyString).
        constexpr bool isStringOrScalar(ValKind a_kind) {
            return a_kind == ValKind::kString || isFlagOrAbsent(a_kind);
        }

        /*
         * Every member name `gather` certifies, so that a decline can be reported
         * once per NAME per session. 6440 items x 6440 log lines would otherwise be
         * all that is left of the log -- the same courtesy 4a's `g_fieldTypeWarned`
         * extends, and for the same reason: the finding is "the type assumption is
         * wrong", and one line already says it.
         */
        inline constexpr const char* kCertifiedMembers[] = {
            "formId",       "equipState", "formType", "duration",   "magnitude", "partMask",
            "keywords",     "weaponType", "bookType", "flags",      "gemSize",   "soulSize",
            "useSound",     "actorValue", "type",     "value",      "weight",    "stolen",
            "effects",      "armor",      "damage",   "poisoned",   "weightClass", "mainPartMask",
        };
        constexpr std::size_t kCertifiedMemberCount = std::size(kCertifiedMembers);

        inline std::array<bool, kCertifiedMemberCount> g_declineReported{};
        inline std::uint32_t                           g_declines = 0;

        void decline(const char* a_member, ValKind a_kind) {
            ++g_declines;
            for (std::size_t i = 0; i < kCertifiedMemberCount; ++i) {
                if (std::strcmp(kCertifiedMembers[i], a_member) != 0) {
                    continue;
                }
                if (g_declineReported[i]) {
                    return;
                }
                g_declineReported[i] = true;
                break;
            }
            logger::warn(
                "4b-ii: `{}` on this entry is a `{}`, a type the replica does not reason about; this item forwards to the "
                "untouched AS body (reported once per member per session)",
                a_member, valKindName(a_kind));
        }

        /*
         * Everything one `processEntry` call reads, plus the members the AS body
         * reads back after writing them.
         *
         * The reads are hoisted out of the branch walk on purpose, and that is a
         * deliberate difference from the AS body: `a_entryObject.keywords` is a
         * member lookup at each of the ~30 places `processMaterialKeywords` mentions
         * it, while nothing in `processEntry` ever reassigns it. Hoisting is
         * therefore behaviour-preserving and removes ~30 GFx member lookups per
         * matching item, which comes straight off S4's ~256 ms/round.
         *
         * The ones that CANNOT be hoisted are the ones the AS body writes and then
         * reads: `weightClass` (:118/:136/:139 -> :508), `mainPartMask`
         * (:423 -> :428/:431/:511) and `infoValue` (:32 -> :663). Those start from
         * the entry's own value and are tracked forward, and `out` is what the AS
         * body would have left on the object.
         */
        struct Ctx {
            WriteSet& out;

            // ---- read from `a_entryObject` --------------------------------
            Slot formId;
            Slot equipState;
            Slot formType;
            Slot duration;
            Slot magnitude;
            Slot partMask;
            Slot keywords;
            Slot weaponType;
            Slot bookType;
            Slot flags;
            Slot gemSize;
            Slot soulSize;
            Slot actorValue;
            Slot weightClass;    // :117, :122 and :508; written at :118/:136/:139
            Slot mainPartMask;   // :428, :431, :511; written at :423
            // `a_entryObject.useSound.formId` (:680), read as one member of the
            // nested object because that is how the AS reaches it.
            Slot useSoundFormId;

            // ---- read from `a_itemInfo` -----------------------------------
            Slot infoType;       // :27
            Slot infoValueSrc;   // :32, :35
            Slot infoWeightSrc;  // :33, :35
            Slot infoArmorSrc;   // :48
            Slot infoDamageSrc;  // :77, :86
            Slot infoStolen;     // :30
            Slot infoPoisoned;   // :76
            Slot effects;        // :47, :75, :85

            // ---- written by the prologue, read back later -----------------
            double baseId{ 0.0 };   // :26, read at :54/:71/:81/:90
            // v6.11 :10. `eslId = formId & 0xFFF` -- the low 12 bits of the same
            // formId, kept because `processMiscBaseId` switches on it (:1113) for the
            // 0xFE plugin slot, where a Creation Club plugin's records arrive with an
            // ESL-flagged form id. It is NOT a narrower view of `baseId`: for a 0xFE
            // formId the two masks overlap but neither is a prefix of the other's
            // meaning, so both are tracked.
            double eslId{ 0.0 };
            Slot   infoValueState;  // :32, read at :663 (processKeyType)
        };
        /*
         * `a_entryObject.useSound.formId` (:680) -- two-step, and only Form.TYPE_POTION
         * ever reaches it, so it lives here rather than in the common set.
         *
         * The outer member is certified as object-or-absent and the inner one is read
         * only if there IS an outer object. That mirrors the AS, which can only reach
         * `.formId` when `useSound` exists; when it is absent the inner value stays
         * `undefined`, which makes the `== FORMID_ITMPotionUse` test false -- the same
         * answer the AS produces.
         */
        bool useSoundFormId(const RE::GFxValue& a_entry, Ctx& a_ctx) {
            Slot useSound;
            readSlot(a_entry, "useSound", useSound);
            if (!isObjectOrAbsent(useSound.kind)) {
                decline("useSound", useSound.kind);
                return false;
            }
            if (!useSound.isObject()) {
                a_ctx.useSoundFormId.value.SetUndefined();
                a_ctx.useSoundFormId.kind = ValKind::kUndefined;
                return true;
            }
            readSlot(useSound.value, "formId", a_ctx.useSoundFormId);
            if (!isNumberOrAbsent(a_ctx.useSoundFormId.kind)) {
                decline("useSound", a_ctx.useSoundFormId.kind);
                return false;
            }
            return true;
        }

        /*
         * Reads and certifies everything `processEntry` will look at, for BOTH objects,
         * before a single write is decided.
         *
         * Returns false when any member has a type the replica does not reason about.
         * Nothing has been written at that point and nothing will be: the caller forwards
         * the call, and the AS body sees an entry that this function only ever read
         * (fail-closed, per the file header).
         *
         * BRANCH-SELECTIVE since the post-#15 measurement. It used to read one fixed set
         * of 25 members for EVERY item, which was defensible as "certification is a
         * property of the object"; capture #15's per-item distribution priced that choice
         * at ~0.12 us per GFx property operation, i.e. ~18 ms/round spent on reads that
         * every item pays for -- while the members a given branch actually needs are 7 (a
         * key) to 13 (armour). Reading only those saves 12-18 reads per item, ~9-14
         * ms/round.
         *
         * What is given up is the old slogan: a decline now means "the engine sent
         * something new FOR THIS BRANCH" rather than "for this object". The failure
         * semantics are unchanged (still fail-closed, still before any write), and the
         * stratified shadow sample covers every branch, so a member a branch forgot to
         * read surfaces as a mismatch instead of as a silently wrong value.
         *
         * THE NEEDS TABLE -- this comment IS the specification the switch below
         * implements, and the two are meant to be reviewed side by side:
         *
         *   every item : formId equipState formType            (entry)
         *                type stolen value weight               (card)
         *   SCROLL   23 : + duration magnitude
         *   ARMOR    26 : + keywords weightClass partMask mainPartMask | armor effects
         *   BOOK     27 : + bookType flags keywords
         *   MISC     32 : + keywords
         *   WEAPON   41 : + weaponType keywords | effects damage poisoned
         *   AMMO     42 : + flags keywords | effects damage
         *   POTION   46 : + flags actorValue duration magnitude useSound.formId
         *   SOULGEM  52 : + gemSize soulSize
         *   INGREDIENT 30 / LIGHT 31 / KEY 45 / everything else : the common set only
         *
         * Note what is NOT common: `flags` is read only for BOOK / AMMO / POTION, and
         * `effects` only for ARMOR / WEAPON / AMMO, even though both are members every
         * item HAS.
         */
        bool gather(const RE::GFxValue& a_entry, const RE::GFxValue& a_item_info, Ctx& a_ctx) {
            const auto want = [](const char* a_member, const Slot& a_slot, bool a_ok) {
                if (a_ok) {
                    return true;
                }
                decline(a_member, a_slot.kind);
                return false;
            };

            bool ok = true;

            // ---- the common set: everything the prologue (:26-35) reads, plus the
            // `formType` the branch selection itself needs ----------------
            readSlot(a_entry, "formId", a_ctx.formId);
            ok = want("formId", a_ctx.formId, isNumberOrAbsent(a_ctx.formId.kind)) && ok;
            readSlot(a_entry, "equipState", a_ctx.equipState);
            ok = want("equipState", a_ctx.equipState, isNumberOrAbsent(a_ctx.equipState.kind)) && ok;
            readSlot(a_entry, "formType", a_ctx.formType);
            ok = want("formType", a_ctx.formType, isNumberOrAbsent(a_ctx.formType.kind)) && ok;
            readSlot(a_item_info, "type", a_ctx.infoType);
            ok = want("type", a_ctx.infoType, isNumberOrAbsent(a_ctx.infoType.kind)) && ok;
            readSlot(a_item_info, "stolen", a_ctx.infoStolen);
            ok = want("stolen", a_ctx.infoStolen, isFlagOrAbsent(a_ctx.infoStolen.kind)) && ok;
            readSlot(a_item_info, "value", a_ctx.infoValueSrc);
            ok = want("value", a_ctx.infoValueSrc, isNumberOrAbsent(a_ctx.infoValueSrc.kind)) && ok;
            readSlot(a_item_info, "weight", a_ctx.infoWeightSrc);
            ok = want("weight", a_ctx.infoWeightSrc, isNumberOrAbsent(a_ctx.infoWeightSrc.kind)) && ok;

            // ---- the chosen branch's extras --------------------------------
            // Free function names are passed rather than wrapped in lambdas, so the
            // accept predicate is visible in the switch and the needs table above reads
            // line for line against it.
            const auto fromEntry = [&](const char* a_name, Slot& a_slot, bool (*a_accept)(ValKind)) {
                readSlot(a_entry, a_name, a_slot);
                return want(a_name, a_slot, a_accept(a_slot.kind));
            };
            const auto fromCard = [&](const char* a_name, Slot& a_slot, bool (*a_accept)(ValKind)) {
                readSlot(a_item_info, a_name, a_slot);
                return want(a_name, a_slot, a_accept(a_slot.kind));
            };

            if (a_ctx.formType.isNumber()) {
                switch (as2ToInt32(a_ctx.formType.number())) {
                    case 23:  // Form.TYPE_SCROLLITEM (:38-44)
                        ok = fromEntry("duration", a_ctx.duration, isNumberOrAbsent) && ok;
                        ok = fromEntry("magnitude", a_ctx.magnitude, isNumberOrAbsent) && ok;
                        break;
                    case 26:  // Form.TYPE_ARMOR (:46-55)
                        ok = fromEntry("keywords", a_ctx.keywords, isObjectOrAbsent) && ok;
                        ok = fromEntry("weightClass", a_ctx.weightClass, isNumberOrAbsent) && ok;
                        ok = fromEntry("partMask", a_ctx.partMask, isNumberOrAbsent) && ok;
                        ok = fromEntry("mainPartMask", a_ctx.mainPartMask, isNumberOrAbsent) && ok;
                        ok = fromCard("armor", a_ctx.infoArmorSrc, isNumberOrAbsent) && ok;
                        ok = fromCard("effects", a_ctx.effects, isStringOrScalar) && ok;
                        break;
                    case 27:  // Form.TYPE_BOOK (:57-59)
                        ok = fromEntry("bookType", a_ctx.bookType, isNumberOrAbsent) && ok;
                        ok = fromEntry("flags", a_ctx.flags, isNumberOrAbsent) && ok;
                        ok = fromEntry("keywords", a_ctx.keywords, isObjectOrAbsent) && ok;
                        break;
                    case 32:  // Form.TYPE_MISC (:69-72)
                        ok = fromEntry("keywords", a_ctx.keywords, isObjectOrAbsent) && ok;
                        break;
                    case 41:  // Form.TYPE_WEAPON (:74-82)
                        ok = fromEntry("weaponType", a_ctx.weaponType, isNumberOrAbsent) && ok;
                        ok = fromEntry("keywords", a_ctx.keywords, isObjectOrAbsent) && ok;
                        ok = fromCard("effects", a_ctx.effects, isStringOrScalar) && ok;
                        ok = fromCard("damage", a_ctx.infoDamageSrc, isNumberOrAbsent) && ok;
                        ok = fromCard("poisoned", a_ctx.infoPoisoned, isFlagOrAbsent) && ok;
                        break;
                    case 42:  // Form.TYPE_AMMO (:84-91)
                        ok = fromEntry("flags", a_ctx.flags, isNumberOrAbsent) && ok;
                        ok = fromEntry("keywords", a_ctx.keywords, isObjectOrAbsent) && ok;
                        ok = fromCard("effects", a_ctx.effects, isStringOrScalar) && ok;
                        ok = fromCard("damage", a_ctx.infoDamageSrc, isNumberOrAbsent) && ok;
                        break;
                    case 46:  // Form.TYPE_POTION (:97-102)
                        ok = fromEntry("flags", a_ctx.flags, isNumberOrAbsent) && ok;
                        ok = fromEntry("actorValue", a_ctx.actorValue, isNumberOrAbsent) && ok;
                        ok = fromEntry("duration", a_ctx.duration, isNumberOrAbsent) && ok;
                        ok = fromEntry("magnitude", a_ctx.magnitude, isNumberOrAbsent) && ok;
                        ok = useSoundFormId(a_entry, a_ctx) && ok;
                        break;
                    case 52:  // Form.TYPE_SOULGEM (:104-108)
                        ok = fromEntry("gemSize", a_ctx.gemSize, isNumberOrAbsent) && ok;
                        ok = fromEntry("soulSize", a_ctx.soulSize, isNumberOrAbsent) && ok;
                        break;
                    default:
                        // Form.TYPE_INGREDIENT (30) and Form.TYPE_LIGHT (31) write only a
                        // display string, and Form.TYPE_KEY (45) writes one plus its own
                        // `infoValue` -- which the prologue already read. Every other form
                        // type writes nothing beyond the prologue. None of them needs a
                        // member of its own, so there is nothing to read here.
                        break;
                }
            }

            return ok;
        }

        /* ---------------------------------------------------------------------
         * Shared write helpers.
         * ------------------------------------------------------------------- */

        // `a_entryObject.material = <Material.X>` + `materialDisplay = translate(...)`
        void material(Ctx& c, double a_material, T a_display) {
            c.out.putNumber("material", a_material);
            c.out.putTranslation("materialDisplay", a_display);
        }

        /*
         * `a_entryObject.weightClass = <Armor.WEIGHT_*>` plus the display string that
         * always travels with it in a PAIR -- :136-137, :139-140, :522-523, :530-531,
         * :540-541. The fifth weight-class write in the AS, :118's `weightClass =
         * null` fold, is NOT a pair and is therefore written out longhand in
         * processArmorClass rather than routed through here.
         *
         * The member is tracked on the context as well as written, because
         * processArmorOther (:508) reads this exact member back.
         */
        void weightClass(Ctx& c, double a_value, T a_display) {
            c.out.putNumber("weightClass", a_value);
            c.weightClass.value.SetNumber(a_value);
            c.weightClass.kind = ValKind::kNumber;
            c.out.putTranslation("weightClassDisplay", a_display);
        }

        // Forward declaration: `processArmorPartMask` and `processArmorBaseId` are
        // transcribed before the helper's definition, in the order the AS file
        // declares its functions. Defined with `material` below.
        void subType(Ctx& c, double a_value, T a_display);

        /*
         * InventoryDataSetter.as:9-16 -- the prologue, and the only part of
         * `processEntry` every item goes through:
         *
         *   :9   baseId  = formId & 0xFFFFFF
         *   :10  eslId   = formId & 0xFFF
         *   :11  type    = a_itemInfo.type
         *   :12  isEquipped = (equipState > 0)
         *   :13  isStolen   = (stolen == true)
         *   :14  infoValue  = (value  <= 0) ? null : round(value  * 100) / 100
         *   :15  infoWeight = (weight <= 0) ? null : round(weight * 100) / 100
         *   :16  infoValueWeight = !(weight > 0 && value > 0) ? null : round(value / weight)
         *
         * `type` is assigned RAW -- no rounding, no coercion -- so the write carries
         * whatever kind the card had. `infoValue` is the one write the AS body reads
         * back (:663), so it is parked on the context as well as written: the
         * rounding can turn a small positive value into 0, and `processKeyType`
         * then turns that 0 into null. That is a real, reachable difference
         * (value 0.001 -> infoValue 0 -> null, not 0), which is why the state is
         * tracked rather than recomputed.
         *
         * POLARITY, :14/:15. v6.11 states the test the other way round from the 2015
         * tree (`value <= 0 ? null : round` instead of `value > 0 ? round : null`).
         * The two agree for every signed number and for `null` (ToNumber(null) is
         * 0), and they differ exactly on a NaN-ish operand -- `undefined`, or a
         * value the engine did not send -- where `NaN <= 0` is FALSE and `NaN > 0`
         * is also FALSE. So v6.11 ROUNDS it and writes NaN, while 5.1 wrote null.
         * `infoValueWeight` (:16) kept the old polarity, which is why the two tests
         * in this function do not look alike; that asymmetry is the source's.
         */
        void prologue(Ctx& c) {
            const std::uint32_t rawFormId = static_cast<std::uint32_t>(slotToInt32(c.formId));

            c.baseId = static_cast<double>(rawFormId & 0x00FFFFFFu);  // :9
            c.out.putNumber("baseId", c.baseId);

            c.eslId = static_cast<double>(rawFormId & 0x00000FFFu);  // :10
            c.out.putNumber("eslId", c.eslId);

            switch (c.infoType.kind) {
                case ValKind::kNumber:
                    c.out.putNumber("type", c.infoType.number());
                    break;
                case ValKind::kNull:
                    c.out.putNull("type");
                    break;
                default:
                    c.out.putUndefined("type");
                    break;
            }

            c.out.putBoolean("isEquipped", as2GreaterThanZero(c.equipState.value, c.equipState.kind));
            c.out.putBoolean("isStolen", as2EqualsTrue(c.infoStolen.value, c.infoStolen.kind));

            const bool valuePositive = as2GreaterThanZero(c.infoValueSrc.value, c.infoValueSrc.kind);
            const bool weightPositive = as2GreaterThanZero(c.infoWeightSrc.value, c.infoWeightSrc.kind);

            if (valuePositive) {
                const double rounded = as2Round(c.infoValueSrc.number() * 100.0) / 100.0;
                c.out.putNumber("infoValue", rounded);
                c.infoValueState.value.SetNumber(rounded);
                c.infoValueState.kind = ValKind::kNumber;
            } else {
                c.out.putNull("infoValue");
                c.infoValueState.value.SetNull();
                c.infoValueState.kind = ValKind::kNull;
            }

            if (weightPositive) {
                c.out.putNumber("infoWeight", as2Round(c.infoWeightSrc.number() * 100.0) / 100.0);
            } else {
                c.out.putNull("infoWeight");
            }

            if (weightPositive && valuePositive) {
                c.out.putNumber("infoValueWeight", as2Round(c.infoValueSrc.number() / c.infoWeightSrc.number()));
            } else {
                c.out.putNull("infoValueWeight");
            }
        }

        /*
         * processArmorClass (InventoryDataSetter.as:115-143).
         *
         * `switch` is STRICT (trap 2), so the three arms are: the Number 0
         * ("$Light"), the Number 1 ("$Heavy"), and `default` -- which is everything
         * else including `null` and `undefined`. The `default` arm is where the
         * SkyUI-invented WEIGHT_CLOTHING / WEIGHT_JEWELRY come from.
         *
         * :117 first folds WEIGHT_NONE (2) to `null`, so a "no class" armour falls
         * through to the keyword test instead of being reported as `$Other`.
         */
        void armorClass(Ctx& c) {
            // :117-118. Written WITHOUT a display string, because the AS arm does not
            // write one -- :120 is what sets "$Other", and it runs next for every
            // item that reaches this helper. Using the weightClass() helper here would
            // add a write the original never performs.
            if (c.weightClass.kind == ValKind::kNumber && c.weightClass.number() == 2.0) {  // Armor.WEIGHT_NONE
                c.out.putNull("weightClass");
                c.weightClass.value.SetNull();
                c.weightClass.kind = ValKind::kNull;
            }

            // :120 -- the default display, written BEFORE the switch, so the two named
            // arms below overwrite it and the default arm may or may not.
            c.out.putTranslation("weightClassDisplay", T::kOther);

            // :122-142 switch (weightClass) -- STRICT, so null and undefined fall to
            // `default` rather than matching case 0.
            if (c.weightClass.kind == ValKind::kNumber && c.weightClass.number() == 0.0) {  // :123 WEIGHT_LIGHT
                c.out.putTranslation("weightClassDisplay", T::kLight);
                return;
            }
            if (c.weightClass.kind == ValKind::kNumber && c.weightClass.number() == 1.0) {  // :127 WEIGHT_HEAVY
                c.out.putTranslation("weightClassDisplay", T::kHeavy);
                return;
            }

            // :131 default:
            if (c.keywords.isAbsent()) {  // :132
                return;
            }
            if (keywordsHas(c.keywords.value, "VendorItemClothing")) {  // :135
                weightClass(c, 3.0, T::kClothing);                      // Armor.WEIGHT_CLOTHING
            } else if (keywordsHas(c.keywords.value, "VendorItemJewelry")) {  // :138
                weightClass(c, 4.0, T::kJewelry);                             // Armor.WEIGHT_JEWELRY
            }
        }

        /*
         * processMaterialKeywords (InventoryDataSetter.as:115-244).
         *
         * A first-match-wins chain of `keywords["X"] != undefined` tests, 24 arms,
         * transcribed in the AS's own order because that order IS the behaviour.
         *
         * THE ARMS ARE NOT A RENAME OF THE 2015 LIST. v6.11 folds the per-variant
         * materials into their family arm -- DRAGONPLATE + DRAGONSCALE + DRAGONBONE
         * are one DRAGON arm, IRONBANDED joins IRON, SCALED joins HIDE, STEELPLATE +
         * DRAUGR + DRAUGRHONED join STEEL, ELVENGILDED joins ELVEN, STUDDED +
         * IMPERIALSTUDDED join IMPERIAL, FALMERHARDENED joins FALMER -- and it
         * AUTHORS THE STORMCLOAK ARM BEFORE THE IMPERIAL ONE. The move is
         * observable: an armour carrying both a Stormcloak and an Imperial keyword
         * resolves to STORMCLOAK here and to IMPERIAL in the 2015 file.
         *
         * Three arms read keywords the 2015 file never mentioned: the Creation Club
         * dark/golden (Saints & Seducers) Daedric variants, the Tribunal Ordinator
         * set, and Amber/Madness. Six old materials have no successor at all
         * (VAMPIRE, DAWNGUARD, HUNTER, AETHERIUM, DEATHBRAND, MORAGTONG); the items
         * that used to reach them now fall through to the family arm owning their
         * base keyword, or to `material = null`.
         *
         * The NESTED override is gone: the 2015 file made Stalhrim armour carrying
         * `DLC2dunHaknirArmor` report DEATHBRAND from inside the Stalhrim arm;
         * v6.11 has no Deathbrand material, so Stalhrim is a plain arm. Transcribing
         * it flat is what the source says.
         *
         * The keyword slot is read once for the whole chain: nothing in
         * `processEntry` reassigns `a_entryObject.keywords`, so the ~60 member
         * lookups the AS body performs here collapse to one, and the answers cannot
         * differ.
         */
        void materialKeywords(Ctx& c) {
            c.out.putNull("material");                           // :117
            c.out.putTranslation("materialDisplay", T::kOther);  // :118

            if (c.keywords.isAbsent()) {  // :119
                return;
            }

            const RE::GFxValue& kw = c.keywords.value;
            const auto          has = [&kw](const char* a_name) { return keywordsHas(kw, a_name); };

            if (has("ArmorMaterialDaedric") || has("WeapMaterialDaedric") ||
                has("ccBGSSSE025_ArmorMaterialDark") || has("ccBGSSSE025_WeapMaterialDark") ||
                has("ccBGSSSE025_ArmorMaterialGolden") ||
                has("ccBGSSSE025_WeapMaterialGolden")) {   // :124
                material(c, 3.0, T::kDaedric);                 // Material.DAEDRIC
            } else if (has("ArmorMaterialDragonplate") || has("ArmorMaterialDragonscale") ||
                       has("DLC1WeapMaterialDragonbone")) {  // :129
                material(c, 4.0, T::kDragon);                  // Material.DRAGON
            } else if (has("ArmorMaterialDwarven") || has("WeapMaterialDwarven")) {  // :134
                material(c, 5.0, T::kDwarven);                 // Material.DWARVEN
            } else if (has("ArmorMaterialEbony") || has("WeapMaterialEbony")) {  // :139
                material(c, 6.0, T::kEbony);                   // Material.EBONY
            } else if (has("ArmorMaterialElven") || has("WeapMaterialElven") ||
                       has("ArmorMaterialElvenGilded")) {  // :144
                material(c, 7.0, T::kElven);                   // Material.ELVEN
            } else if (has("ArmorMaterialGlass") || has("WeapMaterialGlass")) {  // :149
                material(c, 9.0, T::kGlass);                   // Material.GLASS
            } else if (has("ArmorMaterialHide") || has("ArmorMaterialScaled")) {  // :154
                material(c, 10.0, T::kHide);                   // Material.HIDE
            } else if (has("ArmorMaterialStormcloak") ||
                       has("ArmorMaterialBearStormcloak")) {  // :159, BEFORE Imperial
                material(c, 21.0, T::kStormcloak);             // Material.STORMCLOAK
            } else if (has("ArmorMaterialImperialHeavy") ||
                       has("ArmorMaterialImperialLight") || has("WeapMaterialImperial") ||
                       has("ArmorMaterialImperialStudded") ||
                       has("ArmorMaterialStudded")) {  // :164
                material(c, 11.0, T::kImperial);               // Material.IMPERIAL
            } else if (has("ArmorMaterialIron") || has("WeapMaterialIron") ||
                       has("ArmorMaterialIronBanded")) {  // :169
                material(c, 12.0, T::kIron);                   // Material.IRON
            } else if (has("ArmorMaterialLeather")) {  // :174
                material(c, 13.0, T::kLeather);                // Material.LEATHER
            } else if (has("ArmorMaterialOrcish") || has("WeapMaterialOrcish") ||
                       has("ccBGSSSE055_ArmorMaterialOrcishLight")) {  // :179
                material(c, 16.0, T::kOrcish);                 // Material.ORCISH
            } else if (has("ArmorMaterialSteel") || has("WeapMaterialSteel") ||
                       has("ArmorMaterialSteelPlate") || has("WeapMaterialDraugr") ||
                       has("WeapMaterialDraugrHoned")) {  // :184
                material(c, 20.0, T::kSteel);                  // Material.STEEL
            } else if (has("WeapMaterialSilver")) {  // :189
                material(c, 18.0, T::kSilver);                 // Material.SILVER
            } else if (has("ArmorMaterialFalmer") ||
                       has("DLC1ArmorMaterialFalmerHardened") ||
                       has("DLC1ArmorMaterielFalmerHeavy") ||
                       has("DLC1ArmorMaterielFalmerHeavyOriginal")) {  // :194
                material(c, 8.0, T::kFalmer);                  // Material.FALMER
            } else if (has("DLC2ArmorMaterialBonemoldHeavy") ||
                       has("DLC2ArmorMaterialBonemoldLight")) {  // :199
                material(c, 1.0, T::kBonemold);                // Material.BONEMOLD
            } else if (has("DLC2ArmorMaterialChitinHeavy") ||
                       has("DLC2ArmorMaterialChitinLight") ||
                       has("DLC2ArmorMaterialMoragTong")) {  // :204
                material(c, 2.0, T::kChitin);                  // Material.CHITIN
            } else if (has("DLC2ArmorMaterialNordicHeavy") ||
                       has("DLC2ArmorMaterialNordicLight") ||
                       has("DLC2WeaponMaterialNordic")) {  // :209
                material(c, 15.0, T::kNordic);                 // Material.NORDIC
            } else if (has("DLC2ArmorMaterialStalhrimHeavy") ||
                       has("DLC2ArmorMaterialStalhrimLight") ||
                       has("DLC2WeaponMaterialStalhrim")) {  // :214
                material(c, 19.0, T::kStalhrim);               // Material.STALHRIM
            } else if (has("WeapMaterialFalmer") ||
                       has("WeapMaterialFalmerHoned")) {  // :219, a second FALMER arm
                material(c, 8.0, T::kFalmer);                 // Material.FALMER
            } else if (has("ccASVSSE001_ArmorOrdinator") ||
                       has("ccASVSSE001_ArmorOrdinatorIndoril")) {  // :224
                material(c, 17.0, T::kOrdinator);              // Material.ORDINATOR
            } else if (has("ccBGSSSE025_ArmorMaterialAmber") ||
                       has("ccBGSSSE025_WeapMaterialAmber")) {  // :229
                material(c, 0.0, T::kAmber);                   // Material.AMBER
            } else if (has("ccBGSSSE025_ArmorMaterialMadness") ||
                       has("ccBGSSSE025_WeapMaterialMadness")) {  // :234
                material(c, 14.0, T::kMadness);                // Material.MADNESS
            } else if (has("WeapMaterialWood")) {  // :239
                material(c, 22.0, T::kWood);                   // Material.WOOD
            }
        }

        /*
         * processArmorPartMask (InventoryDataSetter.as:415-504).
         *
         * :417 bails out on a missing `partMask`, and :428 bails out again if the
         * precedence loop found nothing -- the second test reads the member the loop
         * just wrote, which is why `mainPartMask` is a tracked slot rather than a
         * local.
         *
         * `partMask & PRECEDENCE[i]` is an AS2 `&`, so both sides go through
         * ToInt32: 0x80000000 (2147483648 as a Number) becomes the negative int32
         * 0x80000000, and a non-zero result is truthy either way. The `subType` the
         * `default` arm writes is the mask itself, in its positive Number form --
         * transcribing it as an int32 would put a negative number in the movie.
         */
        void armorPartMask(Ctx& c) {
            if (c.partMask.isAbsent()) {  // :417
                return;
            }

            const std::uint32_t partMask = as2Bits(c.partMask.number());
            for (const double mask : kPartMaskPrecedence) {  // :421
                if ((partMask & as2Bits(mask)) == 0) {
                    continue;
                }
                c.mainPartMask.value.SetNumber(mask);
                c.mainPartMask.kind = ValKind::kNumber;
                c.out.putNumber("mainPartMask", mask);  // :423
                break;
            }

            if (c.mainPartMask.isAbsent()) {  // :428
                return;
            }

            // :431-503, strict switch. Every named arm also rewrites subTypeDisplay;
            // the `default` arm (:500-502) rewrites only subType.
            const double mask = c.mainPartMask.number();
            if (mask == 0x00000001) {  // Armor.PARTMASK_HEAD
                subType(c, 0.0, T::kHead);  // Armor.EQUIP_HEAD
            } else if (mask == 0x00000002) {  // PARTMASK_HAIR
                subType(c, 1.0, T::kHead);
            } else if (mask == 0x00000800) {  // PARTMASK_LONGHAIR
                subType(c, 2.0, T::kHead);
            } else if (mask == 0x00000004) {  // PARTMASK_BODY
                subType(c, 3.0, T::kBody);
            } else if (mask == 0x00000008) {  // PARTMASK_HANDS
                subType(c, 5.0, T::kHands);
            } else if (mask == 0x00000010) {  // PARTMASK_FOREARMS
                subType(c, 4.0, T::kForearms);
            } else if (mask == 0x00000020) {  // PARTMASK_AMULET
                subType(c, 10.0, T::kAmulet);
            } else if (mask == 0x00000040) {  // PARTMASK_RING
                subType(c, 12.0, T::kRing);
            } else if (mask == 0x00000080) {  // PARTMASK_FEET
                subType(c, 8.0, T::kFeet);
            } else if (mask == 0x00000100) {  // PARTMASK_CALVES
                subType(c, 7.0, T::kCalves);
            } else if (mask == 0x00000200) {  // PARTMASK_SHIELD
                subType(c, 6.0, T::kShield);
            } else if (mask == 0x00001000) {  // PARTMASK_CIRCLET
                subType(c, 9.0, T::kCirclet);
            } else if (mask == 0x00002000) {  // PARTMASK_EARS
                subType(c, 11.0, T::kEars);
            } else if (mask == 0x00000400) {  // PARTMASK_TAIL
                subType(c, 13.0, T::kTail);
            } else if (mask == 0x00010000) {      // PARTMASK_CLOAK, :441
                subType(c, 15.0, T::kClothingCloak);  // Armor.EQUIP_CLOAK
            } else if (mask == 0x00020000) {      // PARTMASK_BACKPACK, :445
                subType(c, 14.0, T::kBackpack);       // Armor.EQUIP_BACKPACK
            } else {
                c.out.putNumber("subType", mask);  // :450 -- no display write
            }
        }

        /*
         * processArmorOther (InventoryDataSetter.as:506-534).
         *
         * :508 is `if (a_entryObject.weightClass != null) return;` -- i.e. it runs
         * only when the class is null OR wholly absent, which is why
         * `Slot::isAbsent()` (null or undefined) is the right predicate and a
         * comparison against a C++ sentinel would not be.
         *
         * No `subTypeDisplay` and no `mainPartMask` write here: this is the one
         * helper that only ever touches the weight class.
         */
        void armorOther(Ctx& c) {
            if (!c.weightClass.isAbsent()) {  // :508
                return;
            }

            if (c.mainPartMask.isAbsent()) {  // switch (undefined) matches nothing
                return;
            }

            // The case labels are the int32 view purely so the compiler can use a
            // jump table; the comparison is still exact, because every one of them
            // is a small positive integer that survives ToInt32 unchanged.
            switch (as2ToInt32(c.mainPartMask.number())) {
                case 0x00000001:  // PARTMASK_HEAD
                case 0x00000002:  // PARTMASK_HAIR
                case 0x00000800:  // PARTMASK_LONGHAIR
                case 0x00000004:  // PARTMASK_BODY
                case 0x00000008:  // PARTMASK_HANDS
                case 0x00000010:  // PARTMASK_FOREARMS
                case 0x00000080:  // PARTMASK_FEET
                case 0x00000100:  // PARTMASK_CALVES
                case 0x00000200:  // PARTMASK_SHIELD
                case 0x00000400:  // PARTMASK_TAIL
                case 0x00010000:  // PARTMASK_CLOAK, :472
                case 0x00020000:  // PARTMASK_BACKPACK, :473
                    weightClass(c, 3.0, T::kClothing);  // :474 WEIGHT_CLOTHING
                    break;
                case 0x00000020:  // PARTMASK_AMULET
                case 0x00000040:  // PARTMASK_RING
                case 0x00001000:  // PARTMASK_CIRCLET
                case 0x00002000:  // PARTMASK_EARS
                    weightClass(c, 4.0, T::kJewelry);  // :481 WEIGHT_JEWELRY
                    break;
                default:
                    break;
            }
        }

        /*
         * processArmorBaseId (InventoryDataSetter.as:487-515). Three BASEIDs that need
         * a classification the armour keywords cannot give them: the wedding wreath
         * (jewellery), the Vampire Lord armour (a body piece with no useful partMask),
         * and -- NEW in v6.11 -- the Creation Club `cc025` ring.
         *
         * The shape changed with them: v6.11 is a nested `formId >>> 24` switch with an
         * explicit `default` arm, and the third case lives in that `default` rather
         * than in `case 0x00`, because the CC ring arrives in an ESL-flagged plugin
         * whose slot is 0xFE. That arm matches on `baseId` alone, not on the formId.
         */
        void armorBaseId(Ctx& c) {
            const bool   formIdIsNumber = c.formId.isNumber();
            const double formId         = formIdIsNumber ? c.formId.number() : 0.0;

            switch (static_cast<std::uint32_t>(slotToInt32(c.formId)) >> 24) {  // :489
                case 0x00:  // :491
                    if (c.baseId == 0x8895A) {            // Form.FORMID_CLOTHESWEDDINGWREATH
                        weightClass(c, 4.0, T::kJewelry);  // :494 Armor.WEIGHT_JEWELRY
                    }
                    return;  // :497

                case 0x02:  // :498
                    if (formIdIsNumber &&
                        formId == 0x02011A84) {       // FORMID_DLC1CLOTHESVAMPIRELORDARMOR
                        subType(c, 3.0, T::kBody);     // :501 Armor.EQUIP_BODY
                    }
                    return;  // :504

                default:  // :505
                    if (c.baseId == 0x183E63) {           // Form.BASEID_CC025ADVDSGSRING
                        weightClass(c, 4.0, T::kJewelry);  // :508
                        subType(c, 12.0, T::kRing);        // :510 Armor.EQUIP_RING
                    }
                    return;  // :513
            }
        }

        // `a_entryObject.subType = <value>` + `subTypeDisplay = translate(...)`.
        // The pair travels together in every helper except three (`default:` in
        // processArmorPartMask, the resist cases in processPotionType, and
        // processSoulGemType), which write `subType` alone and are spelled out
        // separately for that reason.
        void subType(Ctx& c, double a_value, T a_display) {
            c.out.putNumber("subType", a_value);
            c.out.putTranslation("subTypeDisplay", a_display);
        }

        /*
         * processWeaponType (InventoryDataSetter.as:325-396).
         *
         * The AS `switch` is strict, so the 20 `Weapon.ANIM_*` values map onto 12
         * `Weapon.TYPE_*` sub types in pairs, and a `weaponType` the engine sends
         * that is not one of them leaves the two prologue writes in place
         * (`subType = null`, `subTypeDisplay = "$Weapon"`).
         *
         * One arm nests: two-handed axes (:372) become "$Warhammer" when the entry
         * carries `WeapTypeWarhammer`, because SkyUI splits the battleaxe animation
         * into two catalogue entries. The keyword slot is tested only when it is not
         * `undefined` -- the AS's own guard, kept rather than relying on
         * `keywordsHas` returning false for a missing object.
         */
        void weaponSubType(Ctx& c) {
            c.out.putNull("subType");                                 // :327
            c.out.putTranslation("subTypeDisplay", T::kWeapon);  // :328

            if (!c.weaponType.isNumber()) {  // switch (undefined) matches nothing
                return;
            }

            const double anim = c.weaponType.number();
            if (anim == 0.0 || anim == 10.0) {  // ANIM_HANDTOHANDMELEE, ANIM_H2H
                subType(c, 0.0, T::kMelee);     // Weapon.TYPE_MELEE
            } else if (anim == 1.0 || anim == 11.0) {  // ANIM_ONEHANDSWORD, ANIM_1HS
                // :258 -- v6.11 nests a Creation Club test inside the one-handed-sword
                // arm: a fishing pole animates as a sword but is not one, and the arm
                // RETURNS instead of writing the sword pair, so the sword values are
                // never written for it. `keywords` is guarded exactly as the AS guards
                // it (:258 tests `keywords != undefined` first).
                if (!c.keywords.isAbsent() && keywordsHas(c.keywords.value, "ccBGSSSE001_FishingPoleKW")) {
                    subType(c, 13.0, T::kFishingRod);  // Weapon.TYPE_FISHINGROD
                    return;
                }
                subType(c, 1.0, T::kSword);  // TYPE_SWORD
            } else if (anim == 2.0 || anim == 12.0) {  // ANIM_ONEHANDDAGGER, ANIM_1HD
                subType(c, 2.0, T::kDagger);           // TYPE_DAGGER
            } else if (anim == 3.0 || anim == 13.0) {  // ANIM_ONEHANDAXE, ANIM_1HA
                subType(c, 3.0, T::kWarAxe);           // TYPE_WARAXE
            } else if (anim == 4.0 || anim == 14.0) {  // ANIM_ONEHANDMACE, ANIM_1HM
                subType(c, 4.0, T::kMace);             // TYPE_MACE
            } else if (anim == 5.0 || anim == 15.0) {  // ANIM_TWOHANDSWORD, ANIM_2HS
                subType(c, 5.0, T::kGreatsword);       // TYPE_GREATSWORD
            } else if (anim == 6.0 || anim == 16.0) {  // ANIM_TWOHANDAXE, ANIM_2HA
                subType(c, 6.0, T::kBattleaxe);        // TYPE_BATTLEAXE
                if (!c.keywords.isAbsent() && keywordsHas(c.keywords.value, "WeapTypeWarhammer")) {  // :372
                    subType(c, 7.0, T::kWarhammer);    // TYPE_WARHAMMER
                }
            } else if (anim == 7.0 || anim == 17.0) {  // ANIM_BOW, ANIM_BOW2
                subType(c, 8.0, T::kBow);              // TYPE_BOW
            } else if (anim == 8.0 || anim == 18.0) {  // ANIM_STAFF, ANIM_STAFF2
                subType(c, 10.0, T::kStaff);           // TYPE_STAFF
            } else if (anim == 9.0 || anim == 19.0) {  // ANIM_CROSSBOW, ANIM_CBOW
                subType(c, 9.0, T::kCrossbow);         // TYPE_CROSSBOW
            }
        }

        /*
         * processWeaponBaseId (InventoryDataSetter.as:315-362).
         *
         * Runs AFTER processWeaponType and overrides its answer for the five forms
         * whose animation does not say what they are: three pickaxes and two wood
         * axes, all of which animate as one-handed axes.
         *
         * NOT the 2015 shape any more: v6.11 nests a `formId >>> 24` switch around
         * it, keys `case 0x04` on the FULL formId for the Dragonborn pickaxes, and
         * adds two material writes.
         */
        /*
         * processWeaponBaseId (InventoryDataSetter.as:315-362).
         *
         * Runs AFTER processWeaponType and overrides its answer for the forms whose
         * animation does not say what they are. Same two-level shape as
         * processAmmoBaseId: `case 0x00` keys on `baseId`, `case 0x04` on the full
         * `formId` (the Dragonborn pickaxes).
         *
         * v6.11 added TWO material writes here, and they are the reason this function
         * can no longer be described as "subType only":
         *
         *   - :333-337 -- LONGBOW / HUNTINGBOW / DRAVINSBOW report `Material.WOOD`.
         *     They are the only weapons the keyword chain gets wrong, because a plain
         *     hunting bow carries no `WeapMaterialWood` keyword at all.
         *   - :349-356 -- the Dragonborn pickaxes get `Material.STEEL` alongside their
         *     `TYPE_PICKAXE`, matching the material their tempering recipe uses.
         *
         * The FORSWORN* arm (:339-343) is empty apart from its `break` -- it exists to
         * stop the `default` from being taken, which for a `switch` changes nothing,
         * so transcribing it as an arm that does nothing is faithful either way; it is
         * kept here so the label list can be diffed against the source.
         */
        void weaponBaseId(Ctx& c) {
            const bool   formIdIsNumber = c.formId.isNumber();
            const double formId         = formIdIsNumber ? c.formId.number() : 0.0;

            switch (static_cast<std::uint32_t>(slotToInt32(c.formId)) >> 24) {  // :317
                case 0x00:                             // :319
                    switch (as2ToInt32(c.baseId)) {    // :320
                        case 0x0E3C16:  // FORMID_WEAPPICKAXE
                        case 0x06A707:  // FORMID_SSDROCKSPLINTERPICKAXE
                        case 0x1019D4:  // FORMID_DUNVOLUNRUUDPICKAXE
                            subType(c, 11.0, T::kPickaxe);  // Weapon.TYPE_PICKAXE
                            break;
                        case 0x02F2F4:  // FORMID_AXE01
                        case 0x0AE086:  // FORMID_DUNHALTEDSTREAMPOACHERSAXE
                            subType(c, 12.0, T::kWoodAxe);  // TYPE_WOODAXE
                            break;
                        case 0x3B562:  // FORMID_LONGBOW, :333 -- new in v6.11
                        case 0x13985:  // FORMID_HUNTINGBOW
                        case 0x6B9AD:  // FORMID_DRAVINSBOW
                            material(c, 22.0, T::kWood);  // Material.WOOD
                            break;
                        case 0xCC829:  // FORMID_FORSWORNAXE, :339 -- deliberately empty
                        case 0xCEE9B:  // FORMID_FORSWORNBOW
                        case 0xFA2C1:  // FORMID_FORSWORNSTAFF
                        case 0xCADE9:  // FORMID_FORSWORNSWORD
                            break;
                        default:
                            break;
                    }
                    return;  // :345

                case 0x04:  // :346
                    if (formIdIsNumber) {
                        if (formId == 0x040179C9 ||   // FORMID_DLC2PICKAXE1
                            formId == 0x040206F2 ||   // FORMID_DLC2PICKAXE2
                            formId == 0x040398E6) {   // FORMID_DLC2PICKAXE3
                            subType(c, 11.0, T::kPickaxe);  // Weapon.TYPE_PICKAXE
                            material(c, 20.0, T::kSteel);   // Material.STEEL, :354
                        }
                    }
                    return;  // :358

                default:  // :359
                    return;
            }
        }

        /*
         * processAmmoType (InventoryDataSetter.as:574-583).
         *
         * Note the polarity, because it reads backwards: the flag is
         * `Weapon.AMMOFLAG_NONBOLT` (:44), so a SET bit means the projectile is an
         * ARROW and a clear bit means a BOLT. It is a plain int32 test, so a missing
         * `flags` reads as 0 and lands on the BOLT side -- which is what the AS does
         * too, via ToInt32(undefined) == 0.
         */
        void ammoType(Ctx& c) {
            if ((as2Bits(c.flags.isNumber() ? c.flags.number() : 0.0) & 0x0004u) != 0) {  // Weapon.AMMOFLAG_NONBOLT
                subType(c, 0.0, T::kArrow);  // Weapon.AMMO_ARROW
            } else {
                subType(c, 1.0, T::kBolt);   // Weapon.AMMO_BOLT
            }
        }

        /*
         * processAmmoBaseId (InventoryDataSetter.as:554-641).
         *
         * A TWO-LEVEL switch: the outer one is `formId >>> 24`, the plugin index, so
         * the arms can never collide across plugin slots even when two records share
         * the same low 24 bits. `case 0x00` then switches on `baseId` (the low 24
         * bits -- for the master file that IS the formId); `case 0x02` and `case 0x04`
         * switch on the FULL `formId`, because the Dawnguard and Dragonborn records
         * they name are identified by plugin index as well as by id.
         *
         * That is the structural difference from the 2015 file, which had a single
         * flat `switch (baseId)` and therefore could not tell, say, `0x00590C` in
         * Dawnguard from the same low bits in another plugin. v6.11 also dropped
         * three arms outright:
         *
         *   - FORSWORNARROW has no arm at all, so a Forsworn arrow keeps whatever
         *     `processMaterialKeywords` left (`material = null`, "$Other") instead of
         *     being relabelled `Material.HIDE` + "$Forsworn".
         *   - The thrown Riekling spear keeps `Material.WOOD` + "$Wood" but no longer
         *     overwrites `subTypeDisplay` with "$Spear", so it stays the "$Bolt" or
         *     "$Arrow" that processAmmoType wrote.
         *   - DRAUGRARROW and DUNGEIRMUNDSIGDISARROWSILLUSION moved into the STEEL
         *     group: `Material.DRAUGR` no longer exists, so both now report "Steel".
         *
         * It runs AFTER processMaterialKeywords, so for ammunition this function --
         * not the keyword chain -- decides the material: an arrow carries none of the
         * keywords `processMaterialKeywords` looks for.
         */
        void ammoBaseId(Ctx& c) {
            // `switch (a_entryObject.formId)` reads the member once, as a raw Number.
            // Keeping both the type guard and the value is what makes the 0x02/0x04
            // arms strict comparisons instead of ToInt32 ones: a formId that is NOT a
            // number must match nothing, and `formId == 0x020098A1` in C++ would
            // otherwise be decided by whatever `number()` returned for a non-number.
            const bool   formIdIsNumber = c.formId.isNumber();
            const double formId         = formIdIsNumber ? c.formId.number() : 0.0;

            // `switch (formId >>> 24)` -- ToInt32 then an UNSIGNED shift, so a
            // 0xFE-prefixed Creation Club form id lands on 0xFE rather than -2.
            switch (static_cast<std::uint32_t>(slotToInt32(c.formId)) >> 24) {  // :556
                case 0x00:  // :558
                    switch (as2ToInt32(c.baseId)) {  // :559
                        case 0x0139C0:  // Form.FORMID_DAEDRICARROW
                            material(c, 3.0, T::kDaedric);  // Material.DAEDRIC
                            break;
                        case 0x0139BF:  // FORMID_EBONYARROW
                            material(c, 6.0, T::kEbony);  // Material.EBONY
                            break;
                        case 0x0139BE:  // FORMID_GLASSARROW
                            material(c, 9.0, T::kGlass);  // Material.GLASS
                            break;
                        case 0x0139BD:  // FORMID_ELVENARROW
                            material(c, 7.0, T::kElven);  // Material.ELVEN
                            break;
                        case 0x0139BC:  // FORMID_DWARVENARROW
                        case 0x07B932:  // FORMID_DWARVENSPHEREARROW
                        case 0x07B935:  // FORMID_DWARVENSPHEREBOLT01
                        case 0x10EC8C:  // FORMID_DWARVENSPHEREBOLT02
                            material(c, 5.0, T::kDwarven);  // Material.DWARVEN
                            break;
                        case 0x0139BB:  // FORMID_ORCISHARROW
                            material(c, 16.0, T::kOrcish);  // Material.ORCISH
                            break;
                        case 0x0EAFDF:  // FORMID_NORDHEROARROW
                            material(c, 15.0, T::kNordic);  // Material.NORDIC
                            break;
                        case 0x038341:  // FORMID_FALMERARROW
                            material(c, 8.0, T::kFalmer);  // Material.FALMER
                            break;
                        case 0x01397F:  // FORMID_STEELARROW
                        case 0x105EE7:  // FORMID_MQ101STEELARROW
                        case 0x034182:  // FORMID_DRAUGRARROW -- was its own DRAUGR arm
                        case 0x0E738A:  // FORMID_DUNGEIRMUNDSIGDISARROWSILLUSION -- was IRON
                            material(c, 20.0, T::kSteel);  // Material.STEEL
                            break;
                        case 0x01397D:  // FORMID_IRONARROW
                        case 0x020DDF:  // FORMID_CWARROW
                        case 0x020F02:  // FORMID_CWARROWSHORT
                        case 0x0236DD:  // FORMID_TRAPDART
                        case 0x0CAB52:  // FORMID_DUNARCHERPRATICEARROW
                        case 0x10E2DE:  // FORMID_FOLLOWERIRONARROW
                            material(c, 12.0, T::kIron);  // Material.IRON
                            break;
                        default:
                            break;
                    }
                    return;  // :612 -- the ONLY arm that has a default; 0x02/0x04 do not

                case 0x02:  // :613
                    // `switch (a_entryObject.formId)` -- a strict comparison against
                    // the Number, so the test is done on the raw formId rather than on
                    // its low 24 bits. `formIdIsNumber` is false only for an absent
                    // formId, where the AS `switch` matches no case either.
                    if (formIdIsNumber) {
                        if (formId == 0x020098A1 ||   // FORMID_DLC1ELVENARROWBLESSED
                            formId == 0x020098A0) {   // FORMID_DLC1ELVENARROWBLOOD
                            material(c, 7.0, T::kElven);  // Material.ELVEN, one arm
                        } else if (formId == 0x0200590C) {  // FORMID_TESTDLC1BOLT
                            material(c, 12.0, T::kIron);    // Material.IRON
                        }
                    }
                    return;

                case 0x04:  // :626
                    if (formIdIsNumber) {
                        if (formId == 0x040339A1) {  // FORMID_DLC2DWARVENBALLISTABOLT
                            material(c, 5.0, T::kDwarven);  // Material.DWARVEN
                        } else if (formId == 0x04017720) {  // FORMID_DLC2RIEKLINGSPEARTHROWN
                            // Material.WOOD, and NOTHING else -- v6.11 dropped the
                            // `subTypeDisplay = "$Spear"` the 2015 file wrote here.
                            material(c, 22.0, T::kWood);
                        }
                    }
                    return;

                default:  // :638
                    return;
            }
        }

        /*
         * processBookType (InventoryDataSetter.as:550-572).
         *
         * `subType = Item.OTHER` is `subType = undefined` -- `Item.as:3` declares
         * `OTHER: Number = undefined` -- so this arm writes `undefined`, not a
         * number, and any later arm that fires overwrites it. `isRead` comes from
         * `Item.BOOKFLAG_READ` (0x08) and is written before both the note test and
         * the keyword chain, so it is always present by the time the AS returns.
         *
         * The note test and the keyword chain are NOT an if/else: a note can also be
         * a recipe, and the original lets the keyword chain have the last word.
         */
        void bookType(Ctx& c) {
            c.out.putUndefined("subType");                             // :552 Item.OTHER
            c.out.putTranslation("subTypeDisplay", T::kBook);  // :553

            c.out.putBoolean("isRead", (as2Bits(c.flags.isNumber() ? c.flags.number() : 0.0) & 0x08u) != 0);  // :555

            if (c.bookType.isNumber() && c.bookType.number() == 0xFF) {  // :557 Item.BOOKTYPE_NOTE
                subType(c, 1.0, T::kNote);                               // Item.BOOK_NOTE
            }

            if (c.keywords.isAbsent()) {  // :562
                return;
            }

            if (keywordsHas(c.keywords.value, "VendorItemRecipe")) {  // :565
                subType(c, 2.0, T::kRecipe);                          // Item.BOOK_RECIPE
            } else if (keywordsHas(c.keywords.value, "VendorItemSpellTome")) {  // :568
                subType(c, 0.0, T::kSpellTome);                                 // Item.BOOK_SPELLTOME
            }
        }

        /*
         * processKeyType (InventoryDataSetter.as:659-668).
         *
         * The AS body duplicates its own test -- `if (infoValue <= 0) infoValue =
         * null;` twice, verbatim -- and the second pass can only re-null a null,
         * because the first pass already replaced anything <= 0. It is therefore
         * transcribed as ONE conditional write, and the reader is told why rather
         * than being left to wonder whether the second was forgotten.
         *
         * `infoValue` here is the value the PROLOGUE wrote, not the card's: keys are
         * the one branch that reads its own output back (:663 reads what :32 wrote).
         * The case where that matters is a positive value small enough to round to
         * 0.00 -- :32 writes the Number 0, :663 turns it into null, and writing `0`
         * would show a 0-value key in the UI.
         */
        void keyType(Ctx& c) {
            c.out.putTranslation("subTypeDisplay", T::kKey);  // :661

            // :663. `null <= 0` is true (ToNumber(null) == 0), so an already-null
            // infoValue re-enters the body; that is why the test is `not a positive
            // number` rather than `is a number that is <= 0`.
            if (c.infoValueState.kind != ValKind::kNumber || c.infoValueState.number() <= 0.0) {
                c.out.putNull("infoValue");
                c.infoValueState.value.SetNull();
                c.infoValueState.kind = ValKind::kNull;
            }
        }

        /*
         * processPotionType (InventoryDataSetter.as:670-742).
         *
         * The FOOD / POISON / actorValue split is a three-way branch, and only the
         * FOOD arm nests: since SKSE 1.6.6 a "food" item whose use sound is the
         * potion one is re-classified as a DRINK (:680), which is the distinction
         * between "restores on use" and "consumed as a meal".
         *
         * The actorValue switch is strict, like every other `switch` here, and three
         * of its twelve arms write `subType` WITHOUT a display string (:729, :733,
         * :737 -- the three resist potions). That is intended SkyUI behaviour: a
         * resist potion keeps the "$Potion" that :673 wrote. It looks like an
         * omission and is not, so it is flagged here rather than "fixed".
         */
        void potionType(Ctx& c) {
            subType(c, 12.0, T::kPotion);  // :672-673 Item.POTION_POTION

            const std::uint32_t flags = as2Bits(c.flags.isNumber() ? c.flags.number() : 0.0);

            if ((flags & 0x00002u) != 0) {   // :675 Item.ALCHFLAG_FOOD
                subType(c, 14.0, T::kFood);  // Item.POTION_FOOD
                if (c.useSoundFormId.isNumber() && c.useSoundFormId.number() == 0x0B6435) {  // :680
                    subType(c, 13.0, T::kDrink);                                             // POTION_DRINK
                }
            } else if ((flags & 0x20000u) != 0) {  // :685 Item.ALCHFLAG_POISON
                subType(c, 15.0, T::kPoison);      // Item.POTION_POISON
            } else if (c.actorValue.isNumber()) {  // :689 switch (actorValue)
                const double av = c.actorValue.number();
                if (av == 24.0) {                    // Actor.AV_HEALTH
                    subType(c, 0.0, T::kHealth);     // Item.POTION_HEALTH
                } else if (av == 25.0) {             // AV_MAGICKA
                    subType(c, 3.0, T::kMagicka);    // Item.POTION_MAGICKA
                } else if (av == 26.0) {             // AV_STAMINA
                    subType(c, 6.0, T::kStamina);    // Item.POTION_STAMINA
                } else if (av == 27.0) {             // AV_HEALRATE
                    subType(c, 1.0, T::kHealth);     // Item.POTION_HEALRATE
                } else if (av == 28.0) {             // AV_MAGICKARATE
                    subType(c, 4.0, T::kMagicka);    // Item.POTION_MAGICKARATE
                } else if (av == 29.0) {             // AV_STAMINARATE
                    subType(c, 7.0, T::kStamina);    // Item.POTION_STAMINARATE
                } else if (av == 155.0) {            // AV_HEALRATEMULT
                    subType(c, 2.0, T::kHealth);     // Item.POTION_HEALRATEMULT
                } else if (av == 156.0) {            // AV_MAGICKARATEMULT
                    subType(c, 5.0, T::kMagicka);    // Item.POTION_MAGICKARATEMULT
                } else if (av == 157.0) {            // AV_STAMINARATEMULT
                    subType(c, 8.0, T::kStamina);    // Item.POTION_STAMINARATEMULT
                } else if (av == 41.0) {                             // AV_FIRERESIST
                    c.out.putNumber("subType", 9.0);                 // Item.POTION_FIRERESIST
                } else if (av == 42.0) {                             // AV_ELECTRICRESIST
                    c.out.putNumber("subType", 10.0);                // Item.POTION_ELECTRICRESIST
                } else if (av == 43.0) {                             // AV_FROSTRESIST
                    c.out.putNumber("subType", 11.0);                // Item.POTION_FROSTRESIST
                }
            }
        }

        /*
         * processSoulGemType / Status / BaseId (InventoryDataSetter.as:744-772).
         *
         * Three passes over the same item, IN THIS ORDER, and the third overwrites
         * the first: Azura's Star and the Black Star report `subType = 6`
         * (SOULGEM_AZURA) whatever size they hold, while keeping the "$Soul Gem"
         * display. Splitting them into one function would be tidier and wrong.
         *
         * `status` is the only member these three write that the UI reads as a
         * straight enum, and `Item.SOULGEM_NONE` (0) is a strict comparison against
         * the Number 0 (:756) -- a size that is merely ABSENT is caught by the
         * `== undefined` half of the same test, which is loose and therefore also
         * catches `null`.
         */
        void soulGemType(Ctx& c) {
            c.out.putUndefined("subType");                                // :746 Item.OTHER
            c.out.putTranslation("subTypeDisplay", T::kSoulGem);  // :747

            // :750 -- "Ignores soulgems that have a size of None": GemSize != None
            if (c.gemSize.isNumber() && c.gemSize.number() != 0.0) {  // Item.SOULGEM_NONE
                c.out.putNumber("subType", c.gemSize.number());
            }
        }

        void soulGemStatus(Ctx& c) {
            if (c.gemSize.isAbsent() || c.soulSize.isAbsent() ||
                (c.soulSize.isNumber() && c.soulSize.number() == 0.0)) {  // :756
                c.out.putNumber("status", 0.0);                           // Item.SOULGEMSTATUS_EMPTY
            } else if (c.soulSize.number() >= c.gemSize.number()) {       // :758
                c.out.putNumber("status", 2.0);                           // Item.SOULGEMSTATUS_FULL
            } else {
                c.out.putNumber("status", 1.0);  // Item.SOULGEMSTATUS_PARTIAL
            }
        }

        void soulGemBaseId(Ctx& c) {
            switch (as2ToInt32(c.baseId)) {
                case 0x63B29:  // Form.FORMID_DA01SOULGEMBLACKSTAR, :754
                case 0x63B27:  // Form.FORMID_DA01SOULGEMAZURASSTAR
                    // No display write: the arm overwrites only `subType`, so the
                    // "$Soul Gem" that processSoulGemType wrote survives.
                    c.out.putNumber("subType", 6.0);  // Item.SOULGEM_AZURA
                    return;
                case 0x804:  // Form.BASEID_CC025SOULTOMATO1, :758 -- new in v6.11
                case 0x137F40:  // Form.BASEID_CC025SOULTOMATO2
                    c.out.putNumber("subType", 7.0);                        // SOULGEM_SOULTOMATO
                    c.out.putTranslation("subTypeDisplay", T::kSoulTomato);  // :761
                    return;
                default:  // :763
                    return;
            }
        }

        /*
         * processMiscType (InventoryDataSetter.as:774-834).
         *
         * The catch-all branch of the whole class: `$Misc` is what an item with no
         * better classification gets, and the ten arms below are the SkyUI-invented
         * sub categories the vendor keyword set can support. The house-building arm
         * (:791) has SEVEN alternative keywords and is the widest single test in the
         * file.
         */
        void miscType(Ctx& c) {
            c.out.putUndefined("subType");                            // :776 Item.OTHER
            c.out.putTranslation("subTypeDisplay", T::kMisc);  // :777

            if (c.keywords.isAbsent()) {  // :779
                return;
            }

            const RE::GFxValue& kw = c.keywords.value;
            const auto          has = [&kw](const char* a_name) { return keywordsHas(kw, a_name); };

            if (has("BYOHAdoptionClothesKeyword")) {  // :782
                subType(c, 9.0, T::kClothing);        // Item.MISC_CHILDRENSCLOTHES
            } else if (has("BYOHAdoptionToyKeyword")) {  // :786
                subType(c, 10.0, T::kToy);               // Item.MISC_TOY
            } else if (has("BYOHHouseCraftingCategoryWeaponRacks") || has("BYOHHouseCraftingCategoryShelf") ||
                       has("BYOHHouseCraftingCategoryFurniture") || has("BYOHHouseCraftingCategoryExterior") ||
                       has("BYOHHouseCraftingCategoryContainers") || has("BYOHHouseCraftingCategoryBuilding") ||
                       has("BYOHHouseCraftingCategorySmithing")) {  // :791
                subType(c, 18.0, T::kHousePart);                    // Item.MISC_HOUSEPART
            } else if (has("VendorItemDaedricArtifact")) {  // :802
                subType(c, 2.0, T::kArtifact);              // Item.MISC_ARTIFACT
            } else if (has("VendorItemGem")) {  // :806
                subType(c, 0.0, T::kGem);       // Item.MISC_GEM
            } else if (has("VendorItemAnimalHide")) {  // :810
                subType(c, 5.0, T::kHide);             // Item.MISC_HIDE
            } else if (has("VendorItemTool")) {  // :814
                subType(c, 8.0, T::kTool);       // Item.MISC_TOOL
            } else if (has("VendorItemAnimalPart")) {  // :818
                subType(c, 6.0, T::kRemains);          // Item.MISC_REMAINS
            } else if (has("VendorItemOreIngot")) {  // :822
                subType(c, 7.0, T::kIngot);          // Item.MISC_INGOT
            } else if (has("VendorItemClutter")) {  // :826
                subType(c, 19.0, T::kClutter);      // Item.MISC_CLUTTER
            } else if (has("VendorItemFirewood")) {  // :830
                subType(c, 11.0, T::kFirewood);      // Item.MISC_FIREWOOD
            }
        }

        /*
         * processMiscBaseId (InventoryDataSetter.as:831-1189 in the v6.11 tree).
         *
         * The widest function in the class by far, and the one that grew the most
         * between the two generations: the 2015 file had a FLAT switch on `baseId`
         * with six arms, while v6.11 has a `formId >>> 24` switch over SEVEN arms
         * (0x00, 0x01, 0x02, 0x03, 0x04, 0xFE, default) and keys them on `baseId`, on
         * the full `formId`, or -- in the 0xFE arm -- on `eslId`.
         *
         * THAT LAST ARM IS WHY `eslId` EXISTS. A Creation Club plugin is ESL-flagged:
         * its records live at plugin slot 0xFE and the form id's low 12 bits are the
         * ESL id, not a load-order-independent baseId. So 0xFE is the one place the
         * 12-bit view is the right key, and the two views are not substitutes.
         *
         * Three of the 2015 arms changed shape rather than growing:
         *
         *   - The gem arm absorbed GEM1..GEM4, which the old file left to the keyword
         *     chain, and the wedge of Creation Club gems arrives in two more arms.
         *   - MISC_HIDE and MISC_TOOL LOST their baseId arms; a hide or a tool is now
         *     classified by `processMiscType` alone, so those arms do not exist here.
         *   - The dragon-claw arm gained CORALDRAGONCLAW, and the leather arm gained
         *     CHITIN1 -> MISC_NETCHLEATHER (Solstheim chitin plate counts as leather).
         *
         * THE ONE RAW LITERAL: the MISCARTIFACT arm (:966-971) writes a THIRD member,
         * `a_entryObject.iconLabel = "default_potion"`. `iconLabel` is the only member
         * in this file written with a RAW string literal instead of through
         * `Translator.translate`, so it has no translation key and cannot come out of
         * the keyed cache; it is served by a second, single-value cache
         * (`g_rawLiteralValue`) and recorded with `putLiteral`, which is also what
         * makes the shadow comparison able to see it.
         */
        void miscBaseId(Ctx& c) {
            const bool   formIdIsNumber = c.formId.isNumber();
            const double formId         = formIdIsNumber ? c.formId.number() : 0.0;

            switch (static_cast<std::uint32_t>(slotToInt32(c.formId)) >> 24) {  // :833
                case 0x00:                           // :835
                    switch (as2ToInt32(c.baseId)) {  // :836
                        case 0x6851E:  // Form.FORMID_GEMAMETHYSTFLAWLESS
                        case 0x1994F:  // FORMID_GEM1
                        case 0x59654:  // FORMID_GEM2
                        case 0x9DFBB:  // FORMID_GEM3
                        case 0x9F7A6:  // FORMID_GEM4
                            subType(c, 0.0, T::kGem);  // :843 Item.MISC_GEM
                            break;
                        case 0x4B56C:  // Form.FORMID_RUBYDRAGONCLAW
                        case 0xAB7BB:  // FORMID_IVORYDRAGONCLAW
                        case 0x7C260:  // FORMID_GLASSCLAW
                        case 0x5AF48:  // FORMID_EBONYCLAW
                        case 0xED417:  // FORMID_EMERALDDRAGONCLAW
                        case 0xAB375:  // FORMID_DIAMONDCLAW
                        case 0x8CDFA:  // FORMID_IRONCLAW
                        case 0xB634C:  // FORMID_CORALDRAGONCLAW
                        case 0x999E7:  // FORMID_E3GOLDENCLAW
                        case 0x663D7:  // FORMID_SAPPHIREDRAGONCLAW
                        case 0x39647:  // FORMID_MS13GOLDENCLAW
                            // display FIRST, sub type second -- :857-858, the source's order
                            c.out.putTranslation("subTypeDisplay", T::kClaw);
                            c.out.putNumber("subType", 1.0);  // Item.MISC_DRAGONCLAW
                            break;
                        case 0x75868:  // Form.FORMID_REMAINS1
                        case 0xF6767:  // FORMID_REMAINS2
                        case 0xAADB6:  // FORMID_REMAINS3
                        case 0xAADB7:  // FORMID_REMAINS4
                        case 0x4286C:  // FORMID_REMAINS5
                            subType(c, 6.0, T::kRemains);  // :865 MISC_REMAINS
                            break;
                        case 0xA:  // Form.FORMID_LOCKPICK
                            subType(c, 20.0, T::kLockpick);  // :869 MISC_LOCKPICK
                            break;
                        case 0xF:  // Form.FORMID_GOLD001
                            subType(c, 21.0, T::kGold);  // :873 MISC_GOLD
                            break;
                        case 0xDB5D2:                                         // Form.FORMID_LEATHER01
                            c.out.putTranslation("subTypeDisplay", T::kLeather);  // :877
                            c.out.putNumber("subType", 3.0);                   // MISC_LEATHER
                            break;
                        case 0x800E4:                                         // Form.FORMID_LEATHERSTRIPS
                            c.out.putTranslation("subTypeDisplay", T::kStrips);  // :881
                            c.out.putNumber("subType", 4.0);                  // MISC_LEATHERSTRIPS
                            break;
                        case 0x3AD57:  // Form.FORMID_CHITIN1, :884
                            subType(c, 23.0, T::kNetchLeather);  // Item.MISC_NETCHLEATHER
                            break;
                        case 0xE72AA:  // Form.FORMID_BROKENWEAPON1, :888
                        case 0xE72AC:  // FORMID_BROKENWEAPON2
                        case 0xE72B0:  // FORMID_BROKENWEAPON3
                        case 0xE72AE:  // FORMID_BROKENWEAPON4
                        case 0xE72A6:  // FORMID_BROKENWEAPON5
                        case 0xE72A8:  // FORMID_BROKENWEAPON6
                        case 0xE72A0:  // FORMID_BROKENWEAPON7
                        case 0xE729E:  // FORMID_BROKENWEAPON8
                        case 0xE729A:  // FORMID_BROKENWEAPON9
                        case 0xE729C:  // FORMID_BROKENWEAPON10
                        case 0xE7296:  // FORMID_BROKENWEAPON11
                        case 0xE7298:  // FORMID_BROKENWEAPON12
                        case 0xE72A4:  // FORMID_BROKENWEAPON13
                        case 0xE72A2:  // FORMID_BROKENWEAPON14
                        case 0x64283:  // FORMID_BROKENWEAPON15
                        case 0x64285:  // FORMID_BROKENWEAPON16
                        case 0x64287:  // FORMID_BROKENWEAPON17
                        case 0x64289:  // FORMID_BROKENWEAPON18
                        case 0x6428B:  // FORMID_BROKENWEAPON19
                        case 0x6428E:  // FORMID_BROKENWEAPON20
                        case 0x64290:  // FORMID_BROKENWEAPON21
                        case 0x64292:  // FORMID_BROKENWEAPON22
                        case 0x64294:  // FORMID_BROKENWEAPON23
                        case 0x64296:  // FORMID_BROKENWEAPON24
                        case 0x64298:  // FORMID_BROKENWEAPON25
                        case 0x6E806:  // FORMID_BROKENWEAPON26
                        case 0xDB351:  // FORMID_BROKENWEAPON27
                        case 0x240D3:  // FORMID_BROKENWEAPON28
                        case 0x240D4:  // FORMID_BROKENWEAPON29
                        case 0x240D5:  // FORMID_BROKENWEAPON30
                        case 0x240D6:  // FORMID_BROKENWEAPON31
                            subType(c, 26.0, T::kBrokenWeapon);  // :919 MISC_BROKENWEAPON
                            break;
                        case 0xC886C:  // Form.FORMID_DWARVENSCRAP1, :922
                        case 0xC8878:  // FORMID_DWARVENSCRAP2
                        case 0xC8864:  // FORMID_DWARVENSCRAP3
                        case 0xC8872:  // FORMID_DWARVENSCRAP4
                        case 0xC8866:  // FORMID_DWARVENSCRAP5
                        case 0xC8874:  // FORMID_DWARVENSCRAP6
                        case 0xC886A:  // FORMID_DWARVENSCRAP7
                        case 0xAEBF1:  // FORMID_DWARVENSCRAP8
                        case 0xC8861:  // FORMID_DWARVENSCRAP9
                        case 0xC8868:  // FORMID_DWARVENSCRAP10
                        case 0xC886E:  // FORMID_DWARVENSCRAP11
                        case 0xC8870:  // FORMID_DWARVENSCRAP12
                            subType(c, 27.0, T::kDwarvenScrap);  // :934 MISC_DWARVENSCRAP
                            break;
                        case 0xDABA9:  // Form.FORMID_INSTRUMENT1, :937
                        case 0xDABA7:  // FORMID_INSTRUMENT2
                        case 0x105177:  // FORMID_INSTRUMENT3
                        case 0x3292F:  // FORMID_INSTRUMENT4
                        case 0x200BA:  // FORMID_INSTRUMENT5
                        case 0xDABAB:  // FORMID_INSTRUMENT6
                        case 0x200B6:  // FORMID_INSTRUMENT7
                        case 0x105109:  // FORMID_INSTRUMENT8
                        case 0xE77BB:  // FORMID_INSTRUMENT9
                            subType(c, 28.0, T::kInstrument);  // :946 MISC_INSTRUMENT
                            break;
                        case 0xB08C7:  // Form.FORMID_BUGJAR1, :949
                        case 0xFBC3A:  // FORMID_BUGJAR2
                        case 0xFBC3B:  // FORMID_BUGJAR3
                        case 0xFBC3C:  // FORMID_BUGJAR4
                        case 0xFBC3D:  // FORMID_BUGJAR5
                            subType(c, 29.0, T::kBugJar);  // :954 MISC_BUGJAR
                            break;
                        case 0x60CC2:  // Form.FORMID_MISCMAP1, :957
                        case 0xBBCD5:  // FORMID_MISCMAP2
                            subType(c, 32.0, T::kMap);  // :959 MISC_MAP
                            break;
                        case 0x28AD7:  // Form.FORMID_MISCAZURASSTAR, :962
                            subType(c, 2.0, T::kArtifact);  // Item.MISC_ARTIFACT
                            break;
                        case 0x2C259:  // Form.FORMID_MISCARTIFACT1, :966
                        case 0x2C25A:  // FORMID_MISCARTIFACT2
                            subType(c, 2.0, T::kArtifact);  // :968 MISC_ARTIFACT
                            // :970 -- the one RAW literal in the file rather than a
                            // translated one, so it goes through the second cache; see
                            // `putLiteral` and `T::kRawDefaultPotion`.
                            c.out.putLiteral("iconLabel", kRawDefaultPotionText);
                            break;
                        case 0xC4F2E:  // Form.FORMID_MISCPOTION, :972
                            subType(c, 33.0, T::kPotion);  // MISC_POTION
                            break;
                        case 0x2BAAB:  // Form.FORMID_MISCPOISON, :976
                            subType(c, 34.0, T::kPoison);  // MISC_POISON
                            break;
                        case 0x457AB:  // Form.FORMID_MISCSCROLL1, :980
                        case 0xDC530:  // FORMID_MISCSCROLL2
                        case 0xDC52E:  // FORMID_MISCSCROLL3
                            subType(c, 35.0, T::kScroll);  // :983 MISC_SCROLL
                            break;
                        case 0xF1491:  // Form.FORMID_MISCBOOK1, :986
                        case 0xCE70B:  // FORMID_MISCBOOK2
                        case 0xE4897:  // FORMID_MISCBOOK3
                        case 0xE3CB7:  // FORMID_MISCBOOK4
                            subType(c, 36.0, T::kBook);  // :990 MISC_BOOK
                            break;
                        case 0x1CB34:  // Form.FORMID_MISCRING1, :993
                        case 0xDA732:  // FORMID_MISCRING2
                        case 0xDA733:  // FORMID_MISCRING3
                        case 0xDA734:  // FORMID_MISCRING4
                        case 0xDA735:  // FORMID_MISCRING5
                            subType(c, 37.0, T::kRing);  // :998 MISC_RING
                            break;
                        case 0x5ACDB:  // Form.FORMID_ORE1, :1001
                        case 0x5ACDC:  // FORMID_ORE2
                        case 0x5ACDE:  // FORMID_ORE3
                        case 0x71CF3:  // FORMID_ORE4
                        case 0x5ACE1:  // FORMID_ORE5
                        case 0x5ACE0:  // FORMID_ORE6
                        case 0x5ACDD:  // FORMID_ORE7
                        case 0x5ACE2:  // FORMID_ORE8
                        case 0x5B2DF:  // FORMID_ORE9
                        case 0x5ACDF:  // FORMID_ORE10
                            subType(c, 31.0, T::kOre);  // :1011 MISC_ORE
                            break;
                    }
                    return;  // :1014

                case 0x01:  // :1015 -- Update.esm horse tack
                    if (formIdIsNumber &&
                        (formId == 0x010030C9 ||   // FORMID_UPDATEHORSETACK1
                         formId == 0x010030CA)) {  // FORMID_UPDATEHORSETACK2
                        subType(c, 25.0, T::kHorseTack);  // :1020 Item.MISC_HORSETACK
                    }
                    return;  // :1023

                case 0x02:  // :1024
                    if (formIdIsNumber) {
                        if (formId == 0x02012F97 ||   // FORMID_DLC1GEM1
                            formId == 0x02012FC3 ||   // FORMID_DLC1GEM2
                            formId == 0x02019ABB ||   // FORMID_DLC1GEM3
                            formId == 0x02019ABC ||   // FORMID_DLC1GEM4
                            formId == 0x02019ABD) {   // FORMID_DLC1GEM5
                            subType(c, 0.0, T::kGem);  // :1032 MISC_GEM
                        } else if (formId == 0x02002993 ||   // FORMID_DLC1REMAINS1
                                   formId == 0x02002994 ||   // FORMID_DLC1REMAINS2
                                   formId == 0x02011CF7 ||   // FORMID_DLC1REMAINS3
                                   formId == 0x02005704 ||   // FORMID_DLC1REMAINS4
                                   formId == 0x02005705 ||   // FORMID_DLC1REMAINS5
                                   formId == 0x02005706 ||   // FORMID_DLC1REMAINS6
                                   formId == 0x02005707) {   // FORMID_DLC1REMAINS7
                            subType(c, 6.0, T::kRemains);  // :1042 MISC_REMAINS
                        } else if (formId == 0x020195AA) {       // FORMID_DLC1CHITIN1
                            subType(c, 23.0, T::kNetchLeather);  // :1046 MISC_NETCHLEATHER
                        }
                    }
                    return;  // :1049

                case 0x03:  // :1050 -- Hearthfire house parts
                    if (formIdIsNumber &&
                        (formId == 0x03003043 ||   // FORMID_HFHOUSEPART1
                         formId == 0x03003035 ||   // FORMID_HFHOUSEPART2
                         formId == 0x03005A69 ||   // FORMID_HFHOUSEPART3
                         formId == 0x03003011 ||   // FORMID_HFHOUSEPART4
                         formId == 0x0300303F ||   // FORMID_HFHOUSEPART5
                         formId == 0x03003012 ||   // FORMID_HFHOUSEPART6
                         formId == 0x0300300E ||   // FORMID_HFHOUSEPART7
                         formId == 0x0300300F ||   // FORMID_HFHOUSEPART8
                         formId == 0x0300306C ||   // FORMID_HFHOUSEPART9
                         formId == 0x03005A68)) {  // FORMID_HFHOUSEPART10
                        // MISC_HOUSEPART, but the display is NOT "$House Part" --
                        // :1064 uses "$BuildingMaterial" for the Hearthfire parts,
                        // while processMiscType's keyword arm uses "$House Part".
                        subType(c, 18.0, T::kBuildingMaterial);  // :1063-1064
                    }
                    return;  // :1066

                case 0x04:  // :1067
                    if (formIdIsNumber) {
                        if (formId == 0x0401CAC0 ||   // FORMID_DLC2DRAGONCLAW1
                            formId == 0x0401CAC1) {   // FORMID_DLC2DRAGONCLAW2
                            c.out.putTranslation("subTypeDisplay", T::kClaw);  // :1072
                            c.out.putNumber("subType", 1.0);                   // MISC_DRAGONCLAW
                        } else if (formId == 0x0402145A ||   // FORMID_DLC2GEM1
                                   formId == 0x0403166F ||   // FORMID_DLC2GEM2
                                   formId == 0x04031670 ||   // FORMID_DLC2GEM3
                                   formId == 0x04031671 ||   // FORMID_DLC2GEM4
                                   formId == 0x04031672) {   // FORMID_DLC2GEM5
                            subType(c, 0.0, T::kGem);  // :1080 MISC_GEM
                        } else if (formId == 0x0402B04E ||   // FORMID_DLC2CHITIN1
                                   formId == 0x0401CD7C) {   // FORMID_DLC2NETCHLEATHER
                            subType(c, 23.0, T::kNetchLeather);  // :1085
                        } else if (formId == 0x040247F9) {    // FORMID_DLC2TROLLSKULL
                            // MISC_TROLLSKULL, and the display is "$Remains"
                            subType(c, 22.0, T::kRemains);  // :1089-1090
                        } else if (formId == 0x04017719 ||   // FORMID_DLC2SCROLLSPIDERMISC1
                                   formId == 0x0401771F) {   // FORMID_DLC2SCROLLSPIDERMISC2
                            subType(c, 30.0, T::kScrollSpider);  // :1094
                        } else if (formId == 0x0402BAAE) {       // FORMID_DLC2MISCMAP
                            subType(c, 32.0, T::kMap);           // :1098
                        } else if (formId == 0x0401AAD6) {       // FORMID_DLC2INGREDIENT
                            subType(c, 38.0, T::kIngredient);    // :1102
                        } else if (formId == 0x0402B06B ||   // FORMID_DLC2ORE1
                                   formId == 0x04017749 ||   // FORMID_DLC2ORE2
                                   formId == 0x040195A9) {   // FORMID_DLC2ORE3
                            subType(c, 31.0, T::kOre);  // :1108
                        }
                    }
                    return;  // :1111
                case 0xFE:  // :1112 -- ESL-flagged Creation Club plugins
                    // The one place in this file that switches on `eslId` rather than
                    // on `baseId`: at slot 0xFE the low 12 bits ARE the ESL id.
                    switch (as2ToInt32(c.eslId)) {  // :1113
                        case 0x80E:  // Form.ESLID_CC019STAFFREMAINS
                        case 0x80A:  // ESLID_CC036PETWOLFREMAINS
                            subType(c, 6.0, T::kRemains);  // :1117 MISC_REMAINS
                            break;
                        case 0x81A:  // ESLID_CCKRTALTARGOLD
                            subType(c, 21.0, T::kGold);  // :1121 MISC_GOLD
                            break;
                        case 0x851:  // ESLID_CCVSV002PETGEAR
                        case 0x871:  // ESLID_CCVSV002PETAMULET
                            subType(c, 39.0, T::kPetGear);  // :1126 MISC_PETGEAR
                    }
                    return;  // :1129

                default:                           // :1130 -- the CC records keyed by baseId
                    switch (as2ToInt32(c.baseId)) {  // :1131
                        case 0x990ED:  // Form.BASEID_CCALMSIVIGEM1, :1133
                        case 0x990F0:  // BASEID_CCALMSIVIGEM2
                        case 0x990F1:  // BASEID_CCALMSIVIGEM3
                        case 0x990F2:  // BASEID_CCALMSIVIGEM4
                            subType(c, 0.0, T::kGem);  // :1137 MISC_GEM
                            break;
                        case 0x809:    // Form.BASEID_CC067AYLEIDCRYSTAL1, :1140
                        case 0x762B7:  // BASEID_CC067AYLEIDCRYSTAL2
                        case 0x762B8:  // BASEID_CC067AYLEIDCRYSTAL3
                        case 0x762B9:  // BASEID_CC067AYLEIDCRYSTAL4
                        case 0x762BA:  // BASEID_CC067AYLEIDCRYSTAL5
                        case 0xBDB37:  // BASEID_CC067AYLEIDCRYSTAL6
                            subType(c, 24.0, T::kAyleidCrystal);  // :1146 MISC_AYLEIDCRYSTAL
                            break;
                        case 0x804:  // Form.BASEID_CC001DWESCRAP, :1149
                            subType(c, 27.0, T::kDwarvenScrap);  // :1150
                            break;
                        case 0x12DD3A:  // Form.BASEID_CC025BUGJAR1, :1153
                        case 0x12DD3B:  // BASEID_CC025BUGJAR2
                        case 0x12DD3C:  // BASEID_CC025BUGJAR3
                            subType(c, 29.0, T::kBugJar);  // :1156
                            break;
                        case 0x80D:  // Form.BASEID_CC031MISCMAP, :1159
                            subType(c, 32.0, T::kMap);  // :1160
                            break;
                        case 0x82F:  // Form.BASEID_CCALMSIVIPOTION, :1163
                            subType(c, 33.0, T::kPotion);  // :1164
                            break;
                        case 0x8B2:  // Form.BASEID_CC001PUZZLEINGREDIENT1, :1167
                        case 0x8B8:  // BASEID_CC001PUZZLEINGREDIENT2
                        case 0x8B9:  // BASEID_CC001PUZZLEINGREDIENT3
                        case 0x8BA:  // BASEID_CC001PUZZLEINGREDIENT4
                        case 0x8BB:  // BASEID_CC001PUZZLEINGREDIENT5
                        case 0x8BC:  // BASEID_CC001PUZZLEINGREDIENT6
                        case 0x8BD:  // BASEID_CC001PUZZLEINGREDIENT7
                        case 0x8BE:  // BASEID_CC001PUZZLEINGREDIENT8
                        case 0x8BF:  // BASEID_CC001PUZZLEINGREDIENT9
                        case 0x8C3:  // BASEID_CC001PUZZLEINGREDIENT10
                        case 0x8C4:  // BASEID_CC001PUZZLEINGREDIENT11
                        case 0x8C5:  // BASEID_CC001PUZZLEINGREDIENT12
                            subType(c, 38.0, T::kIngredient);  // :1179 MISC_INGREDIENT
                            break;
                        case 0xBC6:  // Form.BASEID_CC025ORE1, :1182
                        case 0xBC9:  // BASEID_CC025ORE2
                            subType(c, 31.0, T::kOre);  // :1184 MISC_ORE
                    }
                    return;  // :1187
            }
        }

        /*
         * `duration` / `magnitude` / `infoArmor` / `infoDamage` are all written by
         * the same AS shape -- `x = (x > 0) ? Math.round(x * 100) / 100 : null` --
         * at :41, :42, :48, :77, :86, :98 and :99. One helper, so the copies cannot
         * drift from each other or from `as2Round`.
         *
         * The source is the entry's own member (`duration`, `magnitude`) or the
         * card's (`infoArmor`, `infoDamage`), read once up front -- identical to
         * reading it at the assignment, because nothing in between writes it.
         */
        void roundOrNull(Ctx& c, const char* a_member, const Slot& a_source) {
            if (as2GreaterThanZero(a_source.value, a_source.kind)) {
                c.out.putNumber(a_member, as2Round(a_source.number() * 100.0) / 100.0);
            } else {
                c.out.putNull(a_member);
            }
        }

        /*
         * `isEnchanted = (a_itemInfo.effects != "")` -- :47, :75, :85, three times
         * on the same card, which is why the card's `effects` is read once.
         *
         * The `understood` flag can only be false for an object-typed `effects`,
         * which `gather` has already declined the item for. It is carried through
         * the operator anyway so that the operator itself has no "close enough"
         * branch, and a future caller that skipped the certification would still get
         * the conservative answer.
         */
        bool isEnchanted(const Ctx& c) {
            bool       understood = false;
            const bool result = as2NotEqualEmptyString(c.effects.value, c.effects.kind, understood);
            return understood && result;
        }

        /*
         * processBookBaseId (InventorySetter.as:1190-1255 in the v6.11 tree).
         *
         * A NEW pass in v6.11 -- the 2015 file had no book baseId function at all, so
         * the replica had no counterpart. It runs AFTER processBookType (:36), which
         * means it can and does overwrite the `subType`/`subTypeDisplay` pair that
         * function just decided: a map is a book whose FILE says "map", and no keyword
         * distinguishes it.
         *
         * `case 0x00` is the one arm that falls off the end of its inner switch without
         * a `break` on the Elder Scrolls arm -- harmless here because the arm is last,
         * but it is why the two arms are written as one if/else in the transcription
         * rather than left implicit.
         *
         * `default:` (:1235) is where the Creation Club fishing book maps live. They
         * arrive in a plugin slot of their own, and the arm keys on `baseId` because the
         * twelve ids are unique across plugins.
         */
        void bookBaseId(Ctx& c) {
            const bool   formIdIsNumber = c.formId.isNumber();
            const double formId         = formIdIsNumber ? c.formId.number() : 0.0;

            switch (static_cast<std::uint32_t>(slotToInt32(c.formId)) >> 24) {  // :1192
                case 0x00:                            // :1194
                    switch (as2ToInt32(c.baseId)) {   // :1195
                        case 0xDDEFB:  // Form.FORMID_BOOKMAP1
                        case 0xEF07A:  // FORMID_BOOKMAP2
                        case 0xF33CD:  // FORMID_BOOKMAP3
                        case 0xF33CE:  // FORMID_BOOKMAP4
                        case 0xF33CF:  // FORMID_BOOKMAP5
                        case 0xF33D0:  // FORMID_BOOKMAP6
                        case 0xF33D1:  // FORMID_BOOKMAP7
                        case 0xF33D2:  // FORMID_BOOKMAP8
                        case 0xF33D3:  // FORMID_BOOKMAP9
                        case 0xF33D4:  // FORMID_BOOKMAP10
                        case 0xF33D5:  // FORMID_BOOKMAP11
                        case 0xF33E0:  // FORMID_BOOKMAP12
                            subType(c, 3.0, T::kMap);  // :1209 Item.BOOK_MAP
                            break;
                        case 0x2D513:  // Form.FORMID_ELDERSCROLL1, :1212
                        case 0x48782:  // FORMID_ELDERSCROLL2
                            subType(c, 4.0, T::kElderScroll);  // :1214 BOOK_ELDERSCROLL
                    }
                    return;  // :1217

                case 0x02:  // :1218
                    if (formIdIsNumber &&
                        (formId == 0x020126DC ||   // FORMID_DLC1ELDERSCROLL1
                         formId == 0x02011A13 ||   // FORMID_DLC1ELDERSCROLL2
                         formId == 0x020118F9)) {  // FORMID_DLC1ELDERSCROLL3
                        subType(c, 4.0, T::kElderScroll);  // :1224
                    }
                    return;  // :1227

                case 0x04:  // :1228
                    if (formIdIsNumber && formId == 0x0401CAF2) {  // FORMID_DLC2BOOKMAP
                        subType(c, 3.0, T::kMap);                  // :1231
                    }
                    return;  // :1234

                default:                           // :1235
                    switch (as2ToInt32(c.baseId)) {  // :1236
                        case 0x70CCA:  // Form.BASEID_CC001FISHBOOKMAP1
                        case 0x70CCB:  // BASEID_CC001FISHBOOKMAP2
                        case 0x70CCC:  // BASEID_CC001FISHBOOKMAP3
                        case 0x70CCD:  // BASEID_CC001FISHBOOKMAP4
                        case 0x70CCE:  // BASEID_CC001FISHBOOKMAP5
                        case 0x70CCF:  // BASEID_CC001FISHBOOKMAP6
                        case 0x70CD0:  // BASEID_CC001FISHBOOKMAP7
                        case 0x70CD1:  // BASEID_CC001FISHBOOKMAP8
                        case 0x70CD2:  // BASEID_CC001FISHBOOKMAP9
                        case 0x70CD3:  // BASEID_CC001FISHBOOKMAP10
                        case 0x70CD4:  // BASEID_CC001FISHBOOKMAP11
                        case 0x70CD5:  // BASEID_CC001FISHBOOKMAP12
                            subType(c, 3.0, T::kMap);  // :1250 Item.BOOK_MAP
                    }
                    return;  // :1253
            }
        }

        /*
         * processScrollBaseId (InventoryDataSetter.as:1256-1290 in the v6.11 tree).
         *
         * A NEW pass in v6.11, called from the TYPE_SCROLLITEM arm (:23). The guard at
         * :1258 is unusual for this file -- an EARLY RETURN on the plugin index rather
         * than a `case`: scrolls only ever get a baseId treatment in Dragonborn, so
         * everything else leaves the arm without reading its own id.
         *
         * The inner `switch (formId)` compares against the FULL formId, and its 21 arms
         * all write the same pair, which is why they collapse to one body. The
         * `default:` that follows carries the `return`; there is no `break`, so an arm
         * that did NOT match falls out of the function exactly like the matched one.
         */
        void scrollBaseId(Ctx& c) {
            if ((static_cast<std::uint32_t>(slotToInt32(c.formId)) >> 24) != 0x04) {  // :1258
                return;
            }

            // `switch (formId)` on the AS side; here on its ToInt32 view, because C++
            // will not switch on a double. Every one of the 21 ids is below 0x80000000,
            // where ToInt32 is the identity, and an id that is not a whole number would
            // have to be cast anyway -- the plugin guard above has already done the
            // comparison that matters.
            switch (slotToInt32(c.formId)) {  // :1262
                case 0x0401445E:  // Form.FORMID_DLC2SCROLLSPIDER1
                case 0x04014480:  // FORMID_DLC2SCROLLSPIDER2
                case 0x04016E1C:  // FORMID_DLC2SCROLLSPIDER3
                case 0x0401707B:  // FORMID_DLC2SCROLLSPIDER4
                case 0x0401952C:  // FORMID_DLC2SCROLLSPIDER5
                case 0x04019534:  // FORMID_DLC2SCROLLSPIDER6
                case 0x0401CAB0:  // FORMID_DLC2SCROLLSPIDER7
                case 0x0401DA03:  // FORMID_DLC2SCROLLSPIDER8
                case 0x040206D3:  // FORMID_DLC2SCROLLSPIDER9
                case 0x040206D9:  // FORMID_DLC2SCROLLSPIDER10
                case 0x040206DB:  // FORMID_DLC2SCROLLSPIDER11
                case 0x0402095F:  // FORMID_DLC2SCROLLSPIDER12
                case 0x04020960:  // FORMID_DLC2SCROLLSPIDER13
                case 0x04020961:  // FORMID_DLC2SCROLLSPIDER14
                case 0x04027490:  // FORMID_DLC2SCROLLSPIDER15
                case 0x0402749D:  // FORMID_DLC2SCROLLSPIDER16
                case 0x040274A5:  // FORMID_DLC2SCROLLSPIDER17
                case 0x0403319E:  // FORMID_DLC2SCROLLSPIDER18
                case 0x0403319F:  // FORMID_DLC2SCROLLSPIDER19
                case 0x040331A0:  // FORMID_DLC2SCROLLSPIDER20
                case 0x040331A1:  // FORMID_DLC2SCROLLSPIDER21
                    subType(c, 0.0, T::kScrollSpider);  // Item.SCROLL_SPIDER, :1285-1286
                default:                                // :1287
                    return;
            }
        }

        /*
         * processPotionBaseId (InventoryDataSetter.as:1291-1303 in the v6.11 tree).
         *
         * A NEW pass in v6.11, called from the TYPE_POTION arm (:70): it relabels the
         * Ayleid-crystal potions that ship with `The Cause` / `Ghosts of the Tribunal`,
         * which look like ordinary potions to the potionType logic above.
         *
         * TWO OF THE THREE CASE LABELS ARE DEAD, and that is the source's doing rather
         * than a transcription shortcut. `BASEID_HEARTLANDAYLEIDCRYSTALPOTION1` and
         * `...2` are declared in Form.as with NO VALUE (the decompiled Form.as shows a
         * bare `static var NAME;`), so at runtime they are `undefined`; the `case`
         * compares `a_entryObject.baseId` -- always a Number, because the prologue just
         * computed `formId & 0xFFFFFF` -- against `undefined` under strict equality,
         * which never holds. They are therefore NOT transcribed as labels, because a
         * C++ `case` needs a value and whichever value we invented would match items
         * the AS body does not match. The arm is written as the single real id plus a
         * note, and `potionBaseId` is the only function in this file whose arm count
         * deliberately differs from the source.
         */
        void potionBaseId(Ctx& c) {
            if (as2ToInt32(c.baseId) == 0x20E802) {  // Form.BASEID_CC067AYLEIDCRYSTALPOTION
                subType(c, 16.0, T::kAyleidCrystal);  // :1298 Item.POTION_AYLEIDCRYSTAL
            }
            // :1300 `default: return;` -- and the two Heartland labels above it, dead.
        }

        /*
         * InventoryDataSetter.as:17-78 -- the `formType` switch, after the prologue.
         *
         * The switch is strict, so an ABSENT `formType` matches no case at all and
         * only the prologue's writes survive. That is worth stating because
         * `formType` is the one member `fixSKSEExtendedObject`
         * (ItemcardDataExtender.as:48) also treats as optional -- and because a
         * `==`-based transcription would send it to whichever case happened to be
         * first after coercion.
         *
         * The order inside each arm is the AS's own, and it is load-bearing in four
         * places: processMaterialKeywords BEFORE processAmmoBaseId (the arrow decides
         * the material), processArmorClass BEFORE processArmorOther (which only runs
         * while the class is still null), processWeaponType BEFORE processWeaponBaseId
         * (the pickaxes override the animation), and processBookType BEFORE
         * processBookBaseId (a map overrides the keyword answer).
         *
         * v6.11 added three CALLS to these arms -- processScrollBaseId (:23),
         * processBookBaseId (:36) and processPotionBaseId (:70). The 2015 file had none
         * of them, so an older replica answers scrolls, books and potions without ever
         * consulting their ids.
         *
         * TYPE_SOULGEM (:72) has NO `break` in the source and falls through into
         * `default: return;`. The two paths are indistinguishable -- `default` writes
         * nothing and this arm is the last before it -- so the transcription keeps the
         * `break`. The same holds for every other type the switch does not name.
         */
        void body(Ctx& c) {
            prologue(c);

            if (!c.formType.isNumber()) {
                return;
            }

            switch (as2ToInt32(c.formType.number())) {
                case 23:  // Form.TYPE_SCROLLITEM, :19-24
                    c.out.putTranslation("subTypeDisplay", T::kScroll);
                    roundOrNull(c, "duration", c.duration);    // :21
                    roundOrNull(c, "magnitude", c.magnitude);  // :22
                    scrollBaseId(c);                           // :23 -- NEW in v6.11
                    break;

                case 26:                                                        // Form.TYPE_ARMOR, :46-55
                    c.out.putBoolean("isEnchanted", isEnchanted(c));            // :47
                    roundOrNull(c, "infoArmor", c.infoArmorSrc);                // :48
                    armorClass(c);                                              // :50
                    armorPartMask(c);                                           // :51
                    materialKeywords(c);                                        // :52
                    armorOther(c);                                              // :53
                    armorBaseId(c);                                             // :54
                    break;

                case 27:           // Form.TYPE_BOOK, :34-37
                    bookType(c);   // :35
                    bookBaseId(c); // :36 -- NEW in v6.11
                    break;

                case 30:  // Form.TYPE_INGREDIENT, :61-63
                    c.out.putTranslation("subTypeDisplay", T::kIngredient);
                    break;

                case 31:  // Form.TYPE_LIGHT, :65-67
                    c.out.putTranslation("subTypeDisplay", T::kTorch);
                    break;

                case 32:           // Form.TYPE_MISC, :69-72
                    miscType(c);   // :70
                    miscBaseId(c); // :71
                    break;

                case 41:  // Form.TYPE_WEAPON, :74-82
                    c.out.putBoolean("isEnchanted", isEnchanted(c));  // :75
                    c.out.putBoolean("isPoisoned",
                                     as2EqualsTrue(c.infoPoisoned.value, c.infoPoisoned.kind));  // :76
                    roundOrNull(c, "infoDamage", c.infoDamageSrc);                               // :77
                    weaponSubType(c);                                                            // :79
                    materialKeywords(c);                                                         // :80
                    weaponBaseId(c);                                                             // :81
                    break;

                case 42:                                          // Form.TYPE_AMMO, :84-91
                    c.out.putBoolean("isEnchanted", isEnchanted(c));  // :85
                    roundOrNull(c, "infoDamage", c.infoDamageSrc);    // :86
                    ammoType(c);                                      // :88
                    materialKeywords(c);                              // :89
                    ammoBaseId(c);                                    // :90
                    break;

                case 45:        // Form.TYPE_KEY, :93-95
                    keyType(c); // :94
                    break;

                case 46:                                       // Form.TYPE_POTION, :66-71
                    roundOrNull(c, "duration", c.duration);    // :67
                    roundOrNull(c, "magnitude", c.magnitude);  // :68
                    potionType(c);                             // :69
                    potionBaseId(c);                           // :70 -- NEW in v6.11
                    break;

                case 52:              // Form.TYPE_SOULGEM, :104-108
                    soulGemType(c);   // :105
                    soulGemStatus(c); // :106
                    soulGemBaseId(c); // :107
                    break;

                default:
                    // Every other form type (spells, enchantments, shouts, ...)
                    // leaves the prologue's writes and nothing else. `processEntry`
                    // is abstract in ItemcardDataExtender, so a form type outside
                    // this `switch` is a legitimate no-op, not an error.
                    break;
            }
        }

        /* =====================================================================
         * The translation table -- decision A of the 4b-ii plan.
         *
         * `Translator.translate` is not a lookup table
         * (extern/.../Common/skyui/util/Translator.as, `translate`): it creates a
         * hidden TextField on `_root` and assigns `_translator.text = a_str`, which
         * is how Scaleform asks the GAME's localization system for the string, then
         * reads `.text` back. There is no C++ equivalent of "what does `$Scroll`
         * localize to", so the ONLY faithful implementation calls the AS function.
         *
         * Doing that once per process and caching the 103 results is what makes the
         * replica cheaper on this axis rather than merely equal: the AS body pays a
         * function call per display string, while the replica pays one array index.
         *
         * FOUR ROUTES, tried in order, because which one exists depends on how
         * skyui's SWFs were compiled and that cannot be determined from the source:
         *
         *   1. `_global.skyui.util.Translator` + `Invoke("translate")`. The path AS2
         *      registers for `class skyui.util.X`, and the class
         *      `InventoryDataSetter.as:1` imports.
         *   2. `GFxMovie::Invoke("_global.skyui.util.Translator.translate")` -- the
         *      same object reached through the movie's own path resolver instead of
         *      through GetVariable plus a member call.
         *   3. `_root._translator`, the TextField routes 1/2 leave behind once
         *      `translate` has run. Nothing is created, nothing is changed.
         *   4. `_root._translator` CREATED the way `translate` creates it, for the
         *      case where the menu has not translated anything yet. This is the only
         *      route with a side effect on the movie, and it is the side effect
         *      `Translator.translate` performs by itself at :14 -- same field name, so
         *      a later skyui call replaces this exact field rather than adding a
         *      second one. It is tried last and logged as such.
         *
         * WHICH ROUTE WON IS THE SPIKE'S RESULT and is logged once, because a route
         * that silently stopped working would otherwise be indistinguishable from a
         * table that never resolved: on failure the replica declines EVERY item, and
         * the log has to say why.
         *
         * FAIL-CLOSED. A single literal that will not resolve leaves
         * `g_translationsReady` false, so the replica becomes a pure forwarder and the
         * AS body answers every item. That is the same state 4b-ii started in, i.e.
         * the worst case of this whole file is "no change", never "a wrong display
         * string".
         * ===================================================================== */
        inline std::array<std::string, kTranslationCount> g_translations;
        inline bool                                        g_translationsReady = false;

        enum class Route : std::uint8_t {
            kNone,
            kClassObject,
            kMovieInvoke,
            kExistingField,
            kCreatedField,
        };
        inline Route g_route = Route::kNone;

        const char* routeName(Route a_route) {
            switch (a_route) {
                case Route::kClassObject: return "_global.skyui.util.Translator + Invoke(translate)";
                case Route::kMovieInvoke: return "GFxMovie::Invoke(_global.skyui.util.Translator.translate)";
                case Route::kExistingField: return "_root._translator (pre-existing TextField)";
                case Route::kCreatedField: return "_root._translator (created like Translator.translate does)";
                default: return "unresolved";
            }
        }

        // `_translator.text = a_key` then read `_translator.text` back. Both halves go
        // through GFxMovie, so the string lives in the movie's own heap for exactly
        // as long as the TextField keeps it -- the bare-pointer hazard of
        // `GFxValue::SetString` never applies here.
        bool translateViaField(RE::GFxMovie* a_movie, const char* a_key, std::string& a_out) {
            if (!a_movie->SetVariable("_root._translator.text", a_key)) {
                return false;
            }
            RE::GFxValue text;
            if (!a_movie->GetVariable(&text, "_root._translator.text") || !text.IsString()) {
                return false;
            }
            const char* value = text.GetString();
            a_out = value ? value : "";
            return true;
        }

        /*
         * Routes 3/4 need `_root._translator` to exist. Route 3 is "something already
         * made it" (Translator.translate does, on its first call, which any skyui menu
         * makes during its own setup); route 4 makes it exactly the way
         * Translator.as:14 does, including `_visible = false` at :15.
         *
         * The `a_allowCreate` gate is what keeps routes 1 and 2 free of side effects:
         * they are tried first, so on a working install this function is never called
         * with creation enabled at all.
         */
        bool ensureTranslatorField(RE::GFxMovie* a_movie, bool a_allowCreate) {
            RE::GFxValue field;
            if (a_movie->GetVariable(&field, "_root._translator") && field.IsObject()) {
                return true;
            }
            if (!a_allowCreate) {
                return false;
            }

            RE::GFxValue depth;
            if (!a_movie->Invoke("_root.getNextHighestDepth", &depth, nullptr, 0) || !depth.IsNumber()) {
                return false;
            }

            RE::GFxValue args[6];
            args[0].SetString("_translator");
            args[1].SetNumber(depth.GetNumber());
            args[2].SetNumber(0.0);
            args[3].SetNumber(0.0);
            args[4].SetNumber(1.0);
            args[5].SetNumber(1.0);

            RE::GFxValue created;
            if (!a_movie->Invoke("_root.createTextField", &created, args, 6) || !created.IsObject()) {
                return false;
            }
            if (created.IsDisplayObject()) {
                // Translator.as:15 is `_translator._visible = false`, i.e. an AS2
                // member assignment, so the member is what is written here too --
                // `GFxValue::SetVisible` does not exist (the display setters live on
                // the nested DisplayInfo class, which is a different mechanism from
                // the `_visible` property the field is hidden with).
                RE::GFxValue hidden;
                hidden.SetBoolean(false);
                created.SetMember("_visible", hidden);
            }
            return true;
        }

        /*
         * One literal through the already-latched route.
         *
         * Routes 1/2 re-resolve the class object on EVERY call rather than caching the
         * `GFxValue` across calls. That is deliberate: a `GFxValue` naming an AS object
         * points into the movie's heap, and the movie is released when the menu closes
         * (design section 10.2 -- the lesson S-1 learned the hard way), so a cached
         * class handle would be a use-after-free the first time a retry runs after a
         * reopen. The cost is 103 GetVariable calls during a one-off prepare; the
         * alternative is a lifetime bug.
         */
        bool translateOne(RE::GFxMovie* a_movie, const char* a_key, std::string& a_out) {
            switch (g_route) {
                case Route::kExistingField:
                case Route::kCreatedField:
                    return translateViaField(a_movie, a_key, a_out);

                case Route::kClassObject: {
                    RE::GFxValue translator;
                    if (!a_movie->GetVariable(&translator, "_global.skyui.util.Translator") || !translator.IsObject()) {
                        return false;
                    }
                    RE::GFxValue arg;
                    arg.SetString(a_key);  // a string literal, alive for the whole call
                    RE::GFxValue out;
                    if (!translator.Invoke("translate", &out, &arg, 1) || !out.IsString()) {
                        return false;
                    }
                    const char* value = out.GetString();
                    a_out = value ? value : "";
                    return true;
                }

                case Route::kMovieInvoke: {
                    RE::GFxValue arg;
                    arg.SetString(a_key);
                    RE::GFxValue out;
                    if (!a_movie->Invoke("_global.skyui.util.Translator.translate", &out, &arg, 1) || !out.IsString()) {
                        return false;
                    }
                    const char* value = out.GetString();
                    a_out = value ? value : "";
                    return true;
                }

                default:
                    return false;
            }
        }

        // Tries the four routes once and latches the first that answers with a
        // string. Latching only the ROUTE (never a handle) is what keeps a retry
        // after a menu reopen safe -- see translateOne.
        Route resolveRoute(RE::GFxMovie* a_movie) {
            std::string probe;

            RE::GFxValue translator;
            if (a_movie->GetVariable(&translator, "_global.skyui.util.Translator") && translator.IsObject()) {
                RE::GFxValue arg;
                arg.SetString(kTranslationKeys[0]);
                RE::GFxValue out;
                if (translator.Invoke("translate", &out, &arg, 1) && out.IsString()) {
                    return Route::kClassObject;
                }
            }

            {
                RE::GFxValue arg;
                arg.SetString(kTranslationKeys[0]);
                RE::GFxValue out;
                if (a_movie->Invoke("_global.skyui.util.Translator.translate", &out, &arg, 1) && out.IsString()) {
                    return Route::kMovieInvoke;
                }
            }

            if (ensureTranslatorField(a_movie, /* a_allowCreate */ false) && translateViaField(a_movie, kTranslationKeys[0], probe)) {
                return Route::kExistingField;
            }

            if (ensureTranslatorField(a_movie, /* a_allowCreate */ true) && translateViaField(a_movie, kTranslationKeys[0], probe)) {
                return Route::kCreatedField;
            }

            return Route::kNone;
        }

        inline std::uint32_t        g_prepareFailures = 0;
        inline std::uint32_t        g_prepareAttempts = 0;
        // Bounded retries, for the same reason `kMaxInstallAttempts` is bounded in
        // ProfilingHooks.cpp: "the table is not resolvable yet" resolves within the
        // first few rounds if it resolves at all, while an unbounded retry would turn a
        // genuine failure into ~100 wasted GFx calls per item for the rest of the
        // session -- i.e. exactly the kind of measurement pollution this whole file
        // exists to avoid.
        inline constexpr std::uint32_t kMaxPrepareAttempts = 4;

        bool ensureTranslations(RE::GFxMovie* a_movie) {
            if (g_translationsReady) {
                return true;
            }
            if (!a_movie || g_prepareAttempts >= kMaxPrepareAttempts) {
                return false;
            }
            ++g_prepareAttempts;

            if (g_route == Route::kNone) {
                g_route = resolveRoute(a_movie);
                if (g_route == Route::kNone) {
                    // Once per session: this is a spike result, not a per-item
                    // condition, and it would otherwise repeat 6440 times a round.
                    if (g_prepareFailures++ == 0) {
                        logger::warn(
                            "4b-ii: the Translator spike found no route to `Translator.translate` (tried the class object, "
                            "the movie's own path resolver and both `_root._translator` forms); `processEntry` stays a pure "
                            "forwarder, so nothing regresses -- but 4b-ii cannot be validated on this build");
                    }
                    return false;
                }
                logger::info("4b-ii: translation spike resolved -- route {}, {} literals to resolve", routeName(g_route),
                             kTranslationCount);
            }

            if ((g_route == Route::kExistingField || g_route == Route::kCreatedField) &&
                !ensureTranslatorField(a_movie, g_route == Route::kCreatedField)) {
                ++g_prepareFailures;
                return false;
            }

            for (std::size_t i = 0; i < kTranslationCount; ++i) {
                std::string text;
                if (!translateOne(a_movie, kTranslationKeys[i], text)) {
                    if (g_prepareFailures++ == 0) {
                        logger::warn(
                            "4b-ii: `{}` did not resolve through {}; the replica declines every item rather than "
                            "substituting a display string of its own",
                            kTranslationKeys[i], routeName(g_route));
                    }
                    return false;
                }
                g_translations[i] = std::move(text);
            }

            g_translationsReady = true;
            logger::info("4b-ii: all {} display strings resolved; `processEntry` now runs in C++", kTranslationCount);
            return true;
        }

        /* =====================================================================
         * The managed-string cache -- Tier 1 (1) of the post-capture-#14 work.
         *
         * `g_translations` above holds the TEXT of the 103 display strings, resolved
         * once per process. What a member assignment needs is not text: a string living
         * inside an AS object has to be a string the MOVIE owns, which is what
         * `GFxMovie::CreateString` produces. `apply` used to call it once per string
         * write -- ~5-15 times per item, 6440 items per round -- which is the same
         * allocation the AS body performed, because `Translator.translate` also returns
         * a freshly created managed string on every call.
         *
         * So the 103 managed strings are created ONCE PER MOVIE instead, and `apply`
         * hands the movie one of these by index. On this axis the two implementations
         * then differ in exactly one way: the AS body allocates per call, the replica
         * allocates per open.
         *
         * LIFETIME, and why this is a LEAKED pointer rather than a value. A managed
         * `GFxValue` names memory inside the movie, so it stops being valid the moment
         * the movie is torn down -- and the movie IS torn down on every menu close
         * (design section 10.2: by the time `MenuOpenCloseEvent` says "closed",
         * `IMenu::uiMovie` and the whole object graph behind it are already gone).
         * Rebuilding the cache for the next movie therefore means replacing values whose
         * `_objectInterface` points into freed memory, and assigning into a
         * `std::array<GFxValue, N>` would run the element's destructor on exactly that
         * stale value; deleting the array at process exit would do the same thing a
         * minute later.
         *
         * Both are avoided the same way the three probe handlers avoid it
         * (ProfilingHooks.cpp: a fresh handler per install, deliberately never
         * `Release`d): a fresh array is allocated per movie and the previous one is left
         * alone. The cost is `103 * sizeof(GFxValue)` ~= 1.6 KB per menu open, against
         * the ~30k-90k movie-heap string allocations per round that this removes.
         * ===================================================================== */
        inline std::array<RE::GFxValue, kTranslationCount>* g_translationValues = nullptr;
        /*
         * The second and last managed-string cache: the ONE display string the AS body
         * writes as a raw literal rather than through `Translator.translate`
         * (`iconLabel`, InventoryDataSetter.as:970). Same lifetime and the same
         * leak-by-design as `g_translationValues` above, and a separate pointer because
         * it is not part of the keyed table -- see `putLiteral`. It is created AFTER
         * the table, so a failure to create it leaves BOTH unpublished and the whole
         * attempt is retried, rather than a half-built cache being used.
         */
        inline RE::GFxValue* g_rawLiteralValue = nullptr;
        // Which movie the cache was built against, compared by POINTER IDENTITY: the
        // movie is a fresh object on every open, so identity is exactly the "this cache
        // is still valid" test.
        inline RE::GFxMovie* g_valuesMovie = nullptr;
        inline std::uint32_t g_valuesFailures = 0;

        bool ensureManagedStrings(RE::GFxMovie* a_movie) {
            if (g_valuesMovie == a_movie && g_translationValues && g_rawLiteralValue) {
                return true;
            }
            if (!a_movie || !g_translationsReady || g_valuesFailures >= kMaxPrepareAttempts) {
                return false;
            }

            auto* values = new std::array<RE::GFxValue, kTranslationCount>();
            for (std::size_t i = 0; i < kTranslationCount; ++i) {
                a_movie->CreateString(&(*values)[i], g_translations[i].c_str());
                if (!(*values)[i].IsString()) {
                    if (g_valuesFailures++ == 0) {
                        logger::warn(
                            "4b-ii: could not create the managed copy of `{}`; `processEntry` forwards until this "
                            "succeeds (reported once per session)",
                            kTranslationKeys[i]);
                    }
                    // `values` is deliberately left allocated -- see the LIFETIME note.
                    return false;
                }
            }

            // The one raw literal, cached the same way but created AFTER the table: a
            // failure here leaves both pointers unpublished, so the next call retries
            // instead of using a half-built cache.
            auto* raw = new RE::GFxValue();
            a_movie->CreateString(raw, kRawDefaultPotionText);
            if (!raw->IsString()) {
                if (g_valuesFailures++ == 0) {
                    logger::warn(
                        "4b-ii: could not create the managed copy of `{}`; `processEntry` forwards until this "
                        "succeeds (reported once per session)",
                        kRawDefaultPotionText);
                }
                // `raw` and `values` are deliberately left allocated -- see the
                // LIFETIME note.
                return false;
            }

            // The previous array is deliberately not deleted: its values belong to a
            // movie that no longer exists.
            g_translationValues = values;
            g_rawLiteralValue = raw;
            g_valuesMovie = a_movie;
            logger::info(
                "4b-ii: {} display strings + the `{}` literal cached as managed GFxValue for this menu instance",
                kTranslationCount, kRawDefaultPotionText);
            return true;
        }

        /* =====================================================================
         * Writing a prediction, and the shadow comparison that checks it.
         * ===================================================================== */

        /*
         * Turns the prediction into movie state, in the order the replica recorded
         * it.
         *
         * The string arm is the only allocation, and it uses
         * `GFxMovie::CreateString` rather than `GFxValue::SetString` for exactly the
         * reason 4a's `applyField` does: `SetString` stores a BARE POINTER
         * (GFxValue.cpp:774), while a member of an AS object has to own its string.
         * `CreateString` copies into the movie's heap, which is the same thing the
         * AS body's `Translator.translate` returns.
         *
         * A failure part-way through leaves a partially written entry, and the
         * caller then forwards -- which is still correct, and this is why: the AS
         * body recomputes everything from inputs the replica never writes
         * (`formId`, the card, `keywords`), and the three members it READS BACK
         * (`baseId`, `mainPartMask`, `weightClass`) are ones the replica would have
         * written identically. So a partial write followed by a forward produces the
         * same entry as the forward alone.
         *
         * `a_scratch` is re-used across all writes; ChangeType releases the previous
         * managed value (GFxValue.cpp:1005), so one scratch object for 18 writes is
         * safe rather than leaky.
         */
        bool apply(RE::GFxMovie* a_movie, RE::GFxValue& a_entry, const WriteSet& a_writes) {
            /*
             * The string arm is the one write that cannot go through a plain C++ value:
             * a member of an AS object has to OWN its string, and the only way to hand it
             * one is a managed `GFxValue` out of the movie. Those 103 are created once per
             * movie by `ensureManagedStrings`, so this arm is an index plus a `SetMember`
             * instead of an allocation per item.
             *
             * `a_movie` is therefore no longer read inside the loop, but it is still the
             * cache's owner, and the identity test below is load-bearing rather than
             * defensive: writing a string owned by a dead movie would be a
             * use-after-free, and it is exactly what a stale cache would produce.
             *
             * A missing or mismatched cache is a FAILURE, not a fallback. Allocating
             * here would be precisely the per-item cost this function exists to remove,
             * so the caller forwards the item instead.
             */
            if (!g_translationValues || !g_rawLiteralValue || g_valuesMovie != a_movie) {
                return false;
            }

            RE::GFxValue scratch;
            for (std::size_t i = 0; i < a_writes.count; ++i) {
                const Write& write = a_writes.writes[i];

                if (write.kind == Write::Kind::kString) {
                    // Two caches, picked by KEY: the table for the 103 translated
                    // literals, and the single raw literal the AS body writes for
                    // `iconLabel`. `kRawDefaultPotion` is tested FIRST because it sits
                    // past `kTranslationCount` and would otherwise look like an
                    // out-of-range key. Finding no usable cache is a hard FAILURE, not
                    // a fallback: allocating here is precisely the per-item cost this
                    // exists to remove, so the caller forwards the item instead.
                    const RE::GFxValue* cached = nullptr;
                    if (write.key == T::kRawDefaultPotion) {
                        cached = g_rawLiteralValue;
                    } else if (static_cast<std::size_t>(write.key) < kTranslationCount) {
                        cached = &(*g_translationValues)[static_cast<std::size_t>(write.key)];
                    }
                    if (!cached || !cached->IsString() || !a_entry.SetMember(write.name, *cached)) {
                        return false;
                    }
                    continue;
                }

                switch (write.kind) {
                    case Write::Kind::kUndefined:
                        scratch.SetUndefined();
                        break;
                    case Write::Kind::kNull:
                        scratch.SetNull();
                        break;
                    case Write::Kind::kBoolean:
                        scratch.SetBoolean(write.boolean);
                        break;
                    case Write::Kind::kNumber:
                        scratch.SetNumber(write.number);
                        break;
                    default:
                        return false;
                }
                if (!a_entry.SetMember(write.name, scratch)) {
                    return false;
                }
            }
            return true;
        }

        std::string describeValue(const RE::GFxValue& a_value) {
            if (a_value.IsUndefined()) {
                return "undefined";
            }
            if (a_value.IsNull()) {
                return "null";
            }
            if (a_value.IsBool()) {
                return a_value.GetBool() ? "true" : "false";
            }
            if (a_value.IsNumber()) {
                return std::to_string(a_value.GetNumber());
            }
            if (a_value.IsString()) {
                const char* text = a_value.GetString();
                return std::string("\"") + (text ? text : "") + "\"";
            }
            return "unmodelled";
        }

        std::string describeWrite(const Write& a_write) {
            switch (a_write.kind) {
                case Write::Kind::kUndefined: return "undefined";
                case Write::Kind::kNull: return "null";
                case Write::Kind::kBoolean: return a_write.boolean ? "true" : "false";
                case Write::Kind::kNumber: return std::to_string(a_write.number);
                default: return std::string("\"") + (a_write.text ? a_write.text : "") + "\"";
            }
        }

        /*
         * Does what the AS body actually left on the entry equal what the replica
         * predicted? Kind AND value, compared with the same standard 4a's
         * `fieldMatches` uses: `strcmp` for strings, `==` with no epsilon for
         * numbers, and a type test before the value test in both cases.
         *
         * `kUndefined` accepts an ABSENT member as a match, because AS2 reads a
         * missing member as `undefined` and the AS body itself cannot tell the two
         * apart (trap 3) -- `subType = Item.OTHER` is literally `subType =
         * undefined`, and `DeleteMember` would be an equally faithful spelling of it.
         */
        bool writeMatches(const Write& a_write, bool a_present, const RE::GFxValue& a_actual) {
            switch (a_write.kind) {
                case Write::Kind::kUndefined:
                    return !a_present || a_actual.IsUndefined();
                case Write::Kind::kNull:
                    return a_present && a_actual.IsNull();
                case Write::Kind::kBoolean:
                    return a_present && a_actual.IsBool() && a_actual.GetBool() == a_write.boolean;
                case Write::Kind::kNumber:
                    return a_present && a_actual.IsNumber() && a_actual.GetNumber() == a_write.number;
                case Write::Kind::kString: {
                    if (!a_present || !a_actual.IsString()) {
                        return false;
                    }
                    const char* actual = a_actual.GetString();
                    return std::strcmp(actual ? actual : "", a_write.text ? a_write.text : "") == 0;
                }
                default:
                    return false;
            }
        }

        /*
         * The shadow sample -- section 8.1's "UI values identical to stock skyui",
         * turned from a screen-read into a measurement, exactly as 4a's
         * `kValidateSampleSize` does for the card cache.
         *
         * Why it is worth 10 forwarded items: the replica is new code, and this is the
         * only place in the whole step where a WRONG answer is possible that no build
         * error and no crash would reveal -- a branch transcribed one `case` off
         * produces a perfectly valid-looking entry with the wrong sub type. Forwarding
         * the first 10 items of a round, letting the AS body write its own answer, and
         * then diffing it member by member against the prediction answers that on real
         * data and leaves a log line per item instead of a subjective impression.
         *
         * It also covers the AS2 semantic traps by construction: the comparison would
         * catch `!=` vs `!(== 0)`, `switch` vs `==`, and `null` vs `undefined`, because
         * those all change the value that ends up on the entry.
         *
         * Cost, bounded and visible: 10 items per round take the full AS path, i.e.
         * ~0.4 ms of a ~260 ms round. The counters are reported per round so the
         * sample is never silently skipped.
         */
        inline constexpr std::uint32_t kValidationSample = 10;

        /*
         * The form types the replica branches on, in the order the AS `switch` lists them.
         *
         * WHY THIS TABLE EXISTS: the positional sample is "the first 10 items of the
         * save", and on this save those are four books, four miscs, a key and a piece of
         * armour -- i.e. 4 of these 11 branches. Every other branch would be verified only
         * by the static transcription diff. That was tolerable while `gather` read one
         * fixed member set for every item; it is not tolerable now, because `gather` reads
         * a DIFFERENT set per branch (see its needs table), so an untested branch is
         * exactly where a forgotten read would hide. One shadow item per branch per round
         * costs ~7 additional forwards (~0.3 ms/round) and turns branch coverage from a
         * property of the save into a property of the code.
         */
        inline constexpr std::int32_t kValidatedFormTypes[] = { 23, 26, 27, 30, 31, 32, 41, 42, 45, 46, 52 };
        inline constexpr std::size_t  kValidatedFormTypeCount = std::size(kValidatedFormTypes);
        inline std::array<bool, kValidatedFormTypeCount> g_validatedFormType{};

        inline std::uint32_t g_calls = 0;
        inline std::uint32_t g_replicated = 0;
        inline std::uint32_t g_validated = 0;
        inline std::uint32_t g_forwarded = 0;
        inline std::uint32_t g_applyFailures = 0;
        inline std::uint32_t g_comparedItems = 0;
        inline std::uint32_t g_comparedFields = 0;
        inline std::uint32_t g_mismatches = 0;
        inline std::uint32_t g_validationRemaining = kValidationSample;

        /*
         * One shadow item: the AS body has just run (the caller forwarded), so the
         * entry carries ITS answer -- read each predicted member back and compare.
         *
         * Both sides go through the same kind of read (`GetMember` plus a type test),
         * so a mismatch cannot be an artifact of how the two are described, and the
         * log names the item by formType/formId rather than by an index, because
         * `processEntry`'s arguments are `[entry, itemInfo]` and carry no index at
         * all.
         */
        void compareAgainstOriginal(const RE::GFxValue& a_entry, const WriteSet& a_predicted) {
            ++g_comparedItems;

            double        formType = -1.0;
            double        formId = -1.0;
            RE::GFxValue  probe;
            if (a_entry.GetMember("formType", &probe) && probe.IsNumber()) {
                formType = probe.GetNumber();
            }
            if (a_entry.GetMember("formId", &probe) && probe.IsNumber()) {
                formId = probe.GetNumber();
            }

            std::uint32_t mismatched = 0;
            for (std::size_t i = 0; i < a_predicted.count; ++i) {
                const Write& write = a_predicted.writes[i];
                RE::GFxValue actual;
                const bool   present = a_entry.GetMember(write.name, &actual);
                ++g_comparedFields;
                if (writeMatches(write, present, actual)) {
                    continue;
                }
                ++mismatched;
                logger::warn("4b-ii/validate: formType {} formId {}: `{}` differs -- replica predicted {}, the AS body "
                             "left {}",
                             formType, formId, write.name, describeWrite(write),
                             present ? describeValue(actual) : std::string("absent"));
            }

            if (mismatched == 0) {
                logger::info(
                    "4b-ii/validate: formType {} formId {}: all {} predicted member(s) identical to the AS body's own "
                    "result",
                    formType, formId, a_predicted.count);
            } else {
                g_mismatches += mismatched;
            }
        }

        /*
         * Should this item be shadow-validated? Two independent reasons, and the answer is
         * their union:
         *
         *   positional : the first `kValidationSample` items of the round -- kept
         *                unchanged because captures #14 and #15 were measured with
         *                exactly that sample, and the numbers should stay comparable.
         *
         *   stratified : the first item of each form type the replica branches on, per
         *                round. This is the part that makes coverage a property of the
         *                BRANCH rather than of the save's first ten entries.
         *
         * Both paths MARK the form type, so an item the positional sample already covered
         * is not taken a second time by the stratified pass.
         */
        bool shouldShadowValidate(const Ctx& a_ctx) {
            bool wanted = false;
            if (g_validationRemaining > 0) {
                --g_validationRemaining;
                wanted = true;
            }

            if (a_ctx.formType.isNumber()) {
                const std::int32_t formType = as2ToInt32(a_ctx.formType.number());
                for (std::size_t i = 0; i < kValidatedFormTypeCount; ++i) {
                    if (kValidatedFormTypes[i] != formType) {
                        continue;
                    }
                    if (!g_validatedFormType[i]) {
                        g_validatedFormType[i] = true;
                        wanted = true;
                    }
                    break;
                }
            }

            if (!wanted) {
                return false;
            }
            ++g_validated;
            return true;
        }

        Stage beginImpl(RE::GFxMovie* a_movie, RE::GFxValue& a_entry, const RE::GFxValue& a_item_info, WriteSet& a_out) {
            ++g_calls;

            if (!a_movie || !ensureTranslations(a_movie) || !ensureManagedStrings(a_movie)) {
                ++g_forwarded;
                return Stage::kForwardOnly;
            }

            a_out.reset();
            Ctx ctx{ a_out };
            if (!gather(a_entry, a_item_info, ctx)) {
                ++g_forwarded;
                return Stage::kForwardOnly;
            }
            body(ctx);

            if (shouldShadowValidate(ctx)) {
                return Stage::kPredictNoWrite;
            }

            if (apply(a_movie, a_entry, a_out)) {
                ++g_replicated;
                return Stage::kHandled;
            }

            ++g_applyFailures;
            ++g_forwarded;
            if (g_applyFailures == 1) {
                logger::warn("4b-ii: writing a prediction failed; this item falls back to the AS body (reported once per session)");
            }
            return Stage::kForwardOnly;
        }

    }  // namespace

    /* =========================================================================
     * The public surface (ProcessEntryReplica.h). Everything above lives in an
     * unnamed namespace; these seven definitions are the only names
     * ProfilingHooks.cpp needs.
     * ===================================================================== */

    void WriteSet::put(const char* a_name, Write::Kind a_kind, bool a_boolean, double a_number, const char* a_text,
                       T a_key) {
        for (std::size_t i = 0; i < count; ++i) {
            if (std::strcmp(writes[i].name, a_name) == 0) {
                writes[i] = Write{ a_name, a_kind, a_boolean, a_number, a_text, a_key };
                return;
            }
        }
        if (count < kMaxWrites) {
            writes[count] = Write{ a_name, a_kind, a_boolean, a_number, a_text, a_key };
            ++count;
            return;
        }
        // Unreachable by construction: kMaxWrites is sized from the widest branch.
        // Reported rather than silently dropped, because a dropped write is not a
        // missing log line -- it is a wrong item on screen.
        logger::error("4b-ii: WriteSet overflow on `{}`; a branch grew past {} members", a_name, kMaxWrites);
    }

    const Write* WriteSet::find(const char* a_name) const {
        for (std::size_t i = 0; i < count; ++i) {
            if (std::strcmp(writes[i].name, a_name) == 0) {
                return &writes[i];
            }
        }
        return nullptr;
    }

    const char* translation(T a_key) {
        const std::size_t index = static_cast<std::size_t>(a_key);
        if (index >= kTranslationCount || !g_translationsReady) {
            // Reachable only if a future caller reaches a branch before `prepare`
            // returned true; `begin` cannot, because it declines first.
            return "";
        }
        return g_translations[index].c_str();
    }

    bool ready() { return g_translationsReady; }

    bool prepare(RE::GFxMovie* a_movie) {
        // Two caches, two lifetimes: the TEXT table is resolved once per process, the
        // managed `GFxValue` copies once per movie (see ensureManagedStrings). `install`
        // calls this once per round, so the second half is what re-adopts the cache to
        // the freshly created movie.
        return ensureTranslations(a_movie) && ensureManagedStrings(a_movie);
    }

    Stage begin(RE::GFxMovie* a_movie, RE::GFxValue& a_entry, const RE::GFxValue& a_item_info, WriteSet& a_out) {
        return beginImpl(a_movie, a_entry, a_item_info, a_out);
    }

    void finishValidation(RE::GFxMovie* a_movie, const RE::GFxValue& a_entry, const WriteSet& a_predicted) {
        // The movie is unused on purpose: the comparison reads the ENTRY, which by
        // now carries the AS body's own answer. The parameter is kept so the call
        // site reads symmetrically with `begin`, and so a future check that needs the
        // movie (a second CreateString probe, say) does not change the signature.
        static_cast<void>(a_movie);
        compareAgainstOriginal(a_entry, a_predicted);
    }

    /*
     * Per-round accounting. Called from the S0a/S0b `install()`, which runs once per
     * round on the round's first RequestItemCardInfo -- i.e. when the previous
     * round's numbers are final. Same placement, and the same reason, as 4a's
     * reportAndResetRoundStats: the reader never has to correlate counters with the
     * Tracy timeline to know which round they belong to.
     *
     * This is also where the shadow sample is re-armed, so exactly one round in a
     * capture pays for it -- the same shape 4a uses.
     */
    void reportAndResetRoundStats() {
        if (g_calls) {
            logger::info(
                "4b-ii: previous round -- processEntry calls {}, replicated in C++ {}, shadow-validated (forwarded on "
                "purpose) {}, forwarded {}, declines {}, apply failures {}",
                g_calls, g_replicated, g_validated, g_forwarded, g_declines, g_applyFailures);
            if (g_comparedItems) {
                // The branch count is the point of the stratified sample, so it is
                // reported rather than inferred: a capture that exercises 7/11 branches
                // says so, and a `gather` change that broke an unexercised branch is
                // then a known blind spot instead of an unexamined one.
                std::size_t branches = 0;
                for (const bool seen : g_validatedFormType) {
                    if (seen) {
                        ++branches;
                    }
                }
                logger::info(
                    "4b-ii: previous round -- shadow comparison covered {} item(s) / {} member(s), {} mismatch(es), {}/{} "
                    "formType branch(es) exercised",
                    g_comparedItems, g_comparedFields, g_mismatches, branches, kValidatedFormTypeCount);
            }
        }

        g_calls = 0;
        g_replicated = 0;
        g_validated = 0;
        g_forwarded = 0;
        g_applyFailures = 0;
        g_comparedItems = 0;
        g_comparedFields = 0;
        g_mismatches = 0;
        g_declines = 0;
        g_validationRemaining = kValidationSample;
        g_validatedFormType.fill(false);
    }
}  // namespace plugin::pe

#endif  // SSE_REPLICATE_PROCESS_ENTRY
