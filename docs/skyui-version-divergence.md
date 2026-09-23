# SkyUI 版本差异勘查：运行 SWF = SkyUI-Community v6.11（**不是** 5.2 SE）

> **结论先行**
>
> 1. `extern/SkyUI_最新版bsa解包/` 里 SWF 的真实版本是 **SkyUI-Community v6.11**（`SKYUI_VERSION_STRING = "6.11 SE"`，
>    27 个脚本文件同戳），上游是 <https://github.com/doodlum/SkyUI-Community>（tag 列表 `v6.0 … v6.11`）。
> 2. `extern/SkyUISrc_github旧版/` 是 **2015-05-17** 的 `schlangster/skyui` master（SkyUI 5.1 源码树，`Material` 有 **44** 个常量）。
> 3. 因此 `ProcessEntryReplica.cpp` 的转写基线（5.1）与运行时（6.11）**不是同一个东西**。差异不是
>    `Material.FALMER` 一个常量，而是 **系统性的**：Material 常量表、`processMaterialKeywords` 分支、
>    `processAmmoBaseId` 结构、`processEntry` 前导与调用序列、`Translator` 字面量表全部变过。
> 4. 已经观测到的那一条 `replica 18/FALMER vs AS 8/DRAGONBONE`（`materialDisplay` 都是 `$Falmer`）
>    与本文 §3 的 `FALMER: 18 → 8` **完全对应**——它是这批差异里唯一被既有采样命中的一条。
>
> 本文只做**事实登记与差异量化**，不含修复方案的实施（见 §6 的三个候选范围）。

---

## 1. 勘查方法与可复现产物

| 步骤 | 命令 / 工具 | 产物 |
| --- | --- | --- |
| 反编译运行版 SWF | JPEXS Free Flash Decompiler 26.3.0：`java -jar ffdec-cli.jar -export script <out> <swf>` | `build/ffdec/out_all/<swf 名>/scripts/...`（46 个 SWF 全部导出） |
| 单文件反编译 | 同上，仅 `interface/skyui/inventorylists.swf` | `build/ffdec/out_inv/` |
| 权威源码 | `git clone --depth 1 --branch v6.11 https://github.com/doodlum/SkyUI-Community.git` | `extern/SkyUICommunity_v6.11_权威源/` |
| 差分源 | 直接取 v6.11 tag 的 3 个文件 | `build/skyui-community/v6.11/{Material,Form,InventoryDataSetter}.as` |

`interface/skyui/inventorylists.swf`（55,453 B，`CWS`/zlib/version 15）**不含** `skyui.defines.Material`、
`Form`、`InventoryDataSetter`：它只有列表 UI（`skyui.components.list.*`、`skyui.filter.*`、`skyui.util.*`）。
真正跑数据逻辑的是 **`interface/inventorymenu.swf`**（`InventoryDataSetter extends ItemcardDataExtender`）。
`bartermenu.swf` / `containermenu.swf` / `giftmenu.swf` / `craftingmenu.swf` / `magicmenu.swf` /
`favoritesmenu.swf` / `map.swf` / `itemcard.swf` / `activeeffects.swf` / `widgetloader.swf` 同样带 `skyui.*` 类；
`bookmenu` / `hudmenu` / `quest_journal` / `messagebox` / `sharedcomponents` / `startmenu` 等是原版文件（无 `skyui.`）。

### 1.1 运行版身份（决定性证据）

```
out_all/bartermenu/scripts/__Packages/BarterMenu.as:16-18
    static var SKYUI_VERSION_MAJOR = 6;
    static var SKYUI_VERSION_MINOR = 11;
    static var SKYUI_VERSION_STRING = "6.11 SE";
```

同一戳出现在 `BottomBar.as` / `InventoryLists.as` / `ConfigPanel.as` 等共 **27** 处；只有 `mx.transitions.easing.None.as`
仍是 `5.2 SE`（`5`/`2` + `" SE"`）、`Strong.as` 仍是 `5.1`——属于未同步的旧戳，不代表运行版本。

---

## 2. 事实基线一：`skyui.defines.Material` 常量表

反编译 `out_all/inventorymenu/scripts/__Packages/skyui/defines/Material.as` 与
`extern/SkyUICommunity_v6.11_权威源/source/actionscript/Common/skyui/defines/Material.as` **逐字一致**。
与旧源码（`extern/SkyUISrc_github旧版/src/Common/skyui/defines/Material.as`）对比如下。

| 名称 | 运行版 (v6.11) | 旧源码 (5.1) | 备注 |
| --- | ---: | ---: | --- |
| AMBER | 0 | — | CC `Saints & Seducers` 琥珀 |
| BONEMOLD | 1 | 2 | |
| CHITIN | 2 | 4 | |
| DAEDRIC | 3 | 6 | |
| **DRAGON** | **4** | — | 由 DRAGONPLATE/DRAGONSCALE/DRAGONBONE **三合一** |
| DWARVEN | 5 | 14 | |
| EBONY | 6 | 15 | |
| ELVEN | 7 | 16 | 旧 ELVENGILDED(17) 被并入 |
| **FALMER** | **8** | **18** | ← **已观测到的唯一 mismatch** |
| GLASS | 9 | 22 | |
| HIDE | 10 | 23 | 旧 SCALED(35) 被并入 |
| IMPERIAL | 11 | 25 | 旧 IMPERIALSTUDDED(26)/STUDDED(41) 被并入 |
| IRON | 12 | 27 | 旧 IRONBANDED(28) 被并入 |
| LEATHER | 13 | 29 | |
| MADNESS | 14 | — | CC `Saints & Seducers` 疯狂 |
| NORDIC | 15 | 33 | |
| ORCISH | 16 | 34 | |
| ORDINATOR | 17 | — | CC `Ghosts of the Tribunal` 审判者 |
| SILVER | 18 | 36 | |
| STALHRIM | 19 | 37 | |
| STEEL | 20 | 38 | 旧 STEELPLATE(39) 被并入；旧 DRAUGR(11)/DRAUGRHONED(12) 也归此 |
| STORMCLOAK | 21 | 40 | |
| WOOD | 22 | 43 | |
| **常量总数** | **23** | **44** | |

旧源码里的 `AETHERIUM(0)`、`ARTIFACT(1)`、`BROTHERHOOD(3)`、`CLOTHING(5)`、`DAWNGUARD(7)`、`DEATHBRAND(13)`、
`FUR(21)`、`HUNTER(24)`、`MAGIC(30)`、`MORAGTONG(31)`、`NIGHTINGALE(32)`、`VAMPIRE(42)` 在运行版里**没有对应物**。

`Form.FORMID_FALMERARROW` 两版一致：`0x38341`（= `BASEID_FALMERARROW`，运行版 `Form.as:110`，与复刻 `case 0x038341` 吻合）。

---

## 3. 事实基线二：`InventoryDataSetter.as` 的三方差异

按「运行版 SWF 反编译 ↔ 复刻源码」和「旧源码 ↔ 运行版 SOURCE」两条线各查一遍，结论一致：
复刻 = 旧源码（5.1）逐行转写；运行版 = v6.11。**复刻里每一条 `// :NNN` 行号注释指的都是旧源码的行号**
（例：`materialKeywords` 的 `// :147/:148/:150` = 旧源码 147/148/150 行）。

判定依据（机器提取，非目视）：

* `translate("…")` 字面量：旧源码 **102** 个（与 `ProcessEntryReplica.h` 的 `kTranslationKeys` 数量、
  及其 `static_assert(kTranslationCount == 102)` 完全一致 → 提取方法自校验通过）；v6.11 **103** 个。
* `material = Material.X` 赋值序列（按出现顺序）：v6.11 = **40** 条，复刻 = **51** 条（38 + 13）。
* `this.processX(a_entryObject)` 调用序列：v6.11 = **22** 条，复刻 = **19** 条。

### 3.1 `processEntry` 前导（v6.11 `:9-16`）

| 成员 | 运行版 v6.11 | 旧源码 5.1 / 复刻 |
| --- | --- | --- |
| `baseId` | `formId & 0xFFFFFF` | 同 |
| **`eslId`** | **`formId & 0xFFF`（v6.11 `:10`）** | **无** |
| `type` / `isEquipped` / `isStolen` / `infoValue` / `infoWeight` / `infoValueWeight` | 同 | 同 |

复刻的 `prologue()`（`ProcessEntryReplica.cpp:658`）**不预测 `eslId`**。因为影子比对只检查「复刻预测过的成员」，
这条缺失不会报 mismatch，属于**静默覆盖缺口**（不是通过）。

### 3.2 `processEntry` 调用序列：v6.11 多 3 个函数

`processScrollBaseId`（`:23`）、`processBookBaseId`（`:36`）、`processPotionBaseId`（`:70`）。
复刻的 `body()`（`:1482`）在 `case 23/27/46` 里都没有这三个调用，且 `.cpp` 里**没有这三个函数的定义**。

### 3.3 `processMaterialKeywords`：38 臂 → 24 臂，且**顺序变了**

| | 运行版 v6.11（`:115-244`，24 臂） | 旧源码 / 复刻（38 臂） |
| --- | --- | --- |
| 顺序 | DAEDRIC, DRAGON, DWARVEN, EBONY, ELVEN, GLASS, HIDE, STORMCLOAK, IMPERIAL, IRON, LEATHER, ORCISH, STEEL, SILVER, FALMER, BONEMOLD, CHITIN, NORDIC, STALHRIM, FALMER, ORDINATOR, AMBER, MADNESS, WOOD | DAEDRIC, DRAGONPLATE, DRAGONSCALE, DWARVEN, EBONY, ELVEN, ELVENGILDED, GLASS, HIDE, IMPERIAL, IMPERIALSTUDDED, IRON, IRONBANDED, VAMPIRE, LEATHER, ORCISH, SCALED, STEEL, STEELPLATE, STORMCLOAK, STUDDED, DAWNGUARD, FALMERHARDENED, HUNTER, AETHERIUM, DRAGONBONE, BONEMOLD, CHITIN, MORAGTONG, NORDIC, STALHRIM, DEATHBRAND, DRAUGR, DRAUGRHONED, FALMER, FALMERHONED, SILVER, WOOD |
| 结构差异 | `ArmorMaterialStormcloak` 臂在 `ArmorMaterialImperial*` **之前**；`ArmorMaterialImperialStudded`/`ArmorMaterialStudded` 并入 IMPERIAL 臂 | Stormcloak 在 Imperial **之后**；Studded 各自独立成臂 |
| 新关键字 | `ccBGSSSE025_ArmorMaterialDark`/`Golden`（并入 DAEDRIC）、`ccASVSSE001_ArmorOrdinator*`、`ccBGSSSE025_ArmorMaterialAmber*`/`Madness*` | — |
| 消失的分支 | — | VAMPIRE / DAWNGUARD / FALMERHARDENED / HUNTER / AETHERIUM / DEATHBRAND / DRAUGR / DRAUGRHONED / FALMERHONED / MORAGTONG / SCALED / STEELPLATE / IRONBANDED / ELVENGILDED / STUDDED / IMPERIALSTUDDED（改组或删除） |
| **Stalhrim 内嵌覆盖** | **已删除**（v6.11 `:214-218` 只有 Stalhrim 臂，无 `DLC2dunHaknirArmor` 内嵌） | 复刻 `:844-849` 有嵌套 `DLC2dunHaknirArmor` → `kDeathbrand(13)` |

### 3.4 `processWeaponBaseId`：结构变 + **新增 2 处 material 写入**

v6.11（`:315-362`）是 `switch (formId >>> 24)`：`case 0x00` 内层 `switch (baseId)`（pickaxe / wood axe /
**LONGBOW+HUNTINGBOW+DRAVINSBOW → `Material.WOOD`** / FORSWORN* 空臂），`case 0x04` 内层 `switch (formId)`
（**DLC2PICKAXE1/2/3 → TYPE_PICKAXE + `Material.STEEL`**）。复刻（`:1063-1077`）只有 5 个 baseId 的扁平 switch，
**完全没有这 2 处 material 写入**。

### 3.5 `processAmmoBaseId`：扁平 `baseId` → 嵌套 `formId >>> 24`

v6.11（`:554-641`）外层 `switch (formId >>> 24)`：`case 0x00` 按 `baseId`、`case 0x02`/`case 0x04` 按 `formId`。
逐项行为差异（**同一条箭，两版给出的 material / materialDisplay / subTypeDisplay 不同**）：

| 物品 | 运行版 v6.11 | 旧源码 / 复刻 | 差异 |
| --- | --- | --- | --- |
| FALMERARROW `0x038341` | `Material.FALMER` = **8** | `Material.FALMER` = **18** | ← 已观测到 |
| DRAUGRARROW `0x034182` | 并入 STEEL 组 = **20** | 独立臂 `Material.DRAUGR` = **11** | material + display(`$Steel` / `$Draugr`) |
| DUNGEIRMUNDSIGDISARROWSILLUSION `0x0E738A` | 并入 **STEEL** 组 = 20 | 并入 **IRON** 组 = 12 | material + display |
| FORSWORNARROW `0x0CEE9E` | **无臂**（保留 `processMaterialKeywords` 的结果） | `Material.HIDE` = 23 + `"$Forsworn"` | 复刻多写 |
| DLC2RIEKLINGSPEARTHROWN `0x017720` | `Material.WOOD` = 22，**不再改写 `subTypeDisplay`** | 额外写 `subTypeDisplay = "$Spear"` | 复刻多写 |
| TESTDLC1BOLT `0x00590C` | `case 0x02` → IRON | 扁平 switch → IRON | 结果同，机制不同 |
| DLC2DWARVENBALLISTABOLT `0x0339A1` | `case 0x04` → DWARVEN | 扁平 switch → DWARVEN | 结果同，机制不同 |
| 其余（DAEDRIC / EBONY / GLASS / ELVEN / DWARVEN / ORCISH / NORDHERO / STEEL / IRONARROW …） | 同 | 同 | 仅常量重编号 |

### 3.6 `Translator` 字面量表：`+21 / −20`

```
新增 (21): $Dragon $Ordinator $Amber $Madness $FishingRod $ClothingCloak $Backpack $SoulTomato
           $NetchLeather $BrokenWeapon $DwarvenScrap $Instrument $BugJar $Map $Ore $HorseTack
           $BuildingMaterial $ScrollSpider $PetGear $AyleidCrystal $ElderScroll
消失 (20): $Dragonplate $Dragonscale $Elven Gilded $Studded $Iron Banded $Vampire $Scaled
           $Steel Plate $Dawnguard $Falmer Hardened $Hunter $Aetherium $Dragonbone $Morag Tong
           $Deathbrand $Draugr $Draugr Honed $Falmer Honed $Forsworn $Spear
```

其中只有 `$Dragon`、`$Ordinator`、`$Amber`、`$Madness` 属于 material 路径；其余 17 个新增属于 `processMiscType` /
`processMiscBaseId` / `processSoulGemBaseId` / `processBookBaseId` / `processScrollBaseId` / `processPotionBaseId`
（复刻只实现到 5.1 的子类型集合，且缺 §3.2 的三个函数）。

---

## 4. 对 `ProcessEntryReplica.*` 的影响清单（精确到现状行号）

| 复刻位置 | 现状 | 运行版 v6.11 对应处 |
| --- | --- | --- |
| `ProcessEntryReplica.cpp:658` `prologue()` | 写 baseId / type / isEquipped / isStolen / infoValue / infoWeight / infoValueWeight | v6.11 `:9-16`：**多 `eslId`** |
| `:771-863` `materialKeywords()` | 38 臂（5.1） | v6.11 `:115-244`：24 臂、顺序不同、无嵌套 Deathbrand |
| `:989-995` `armorBaseId()` | `if (baseId == …)`，只认 2 个 baseId（wreath / vampire-lord armor） | v6.11 `:487-515`：`formId >>> 24`；`default` 臂新增 `BASEID_CC025ADVDSGSRING` |
| `:1063-1077` `weaponBaseId()` | 5 个 baseId，只写 `subType`/`subTypeDisplay` | v6.11 `:315-362`：**新增 LONGBOW 组 → WOOD、DLC2 pickaxe 组 → STEEL** |
| `:1109-1168` `ammoBaseId()` | 扁平 `switch (as2ToInt32(baseId))`，13 个 material 写入 | v6.11 `:554-641`：嵌套 `formId >>> 24`，14 个 material 写入 |
| `:1482-1560` `body()` | `case 23/27/46` 缺 `scrollBaseId`/`bookBaseId`/`potionBaseId` | v6.11 `:23/:36/:70` |
| `ProcessEntryReplica.h:86-190` `enum class T` | 102 项，按 5.1 首现顺序 | v6.11 = 103 项、顺序不同 |
| `ProcessEntryReplica.h:192-215` `kTranslationKeys` + `static_assert(… == 102)` | 按 5.1 抽表 | 需按 v6.11 重抽（103 项） |

**未受影响**（两版已核对一致）：`as2GreaterThanZero` / `as2EqualsTrue` / `keywordsHas` 语义、`processArmorClass`、
`processArmorPartMask`、`processArmorOther`、`processAmmoType`（`AMMOFLAG_NONBOLT`）、`Weapon.ANIM_*` → `TYPE_*`
的 12 组映射、`Form.TYPE_*` 的 11 个值、`FORMID_FALMERARROW`。

---

## 5. 与「已验证通过」的关系（为什么 50 件只报了 1 处）

影子比对的判据是「复刻**预测过**的成员与 AS 实际写值是否相等」（`compareAgainstOriginal`），因此：

1. §3.1 的 `eslId` 缺失 → 不报，**静默漏检**。
2. §3.6 的 17 个新字面量所属函数（misc / soulgem / book / scroll / potion 的 baseId 臂）只在**特定 CC/DLC 物品**上触发；
   既有 50 件采样没命中 → 不报。
3. §3.3–3.5 里被命中的只有 FALMERARROW（`material` 8 vs 18，而 `materialDisplay` 恰好都是 `$Falmer`）→ 只报这 1 条。

所以「10/11 分支全等」是**真**的，但它只证明「被采样的路径上一致」，**不证明转写基线正确**。换一份没有 Falmer 箭的存档，
这批差异会继续静默通过。

---

## 6. 修复范围（三选一，**尚未实施**）

> **2026-09-22 更新：已按方案 B 实施。** 见 §6.1。

| 方案 | 内容 | 代价 | 风险 |
| --- | --- | --- | --- |
| **A. 只修 material 模型** | 按 v6.11 重写 `materialKeywords()`（24 臂）、`ammoBaseId()`（嵌套 `formId >>> 24`）、补齐 `weaponBaseId()` 的 2 处 material、更新 `Material` 常量表、字面量 `-20/+4` | 中（约 250–350 行） | 字面量表不再等于「全文件首现顺序」，`static_assert(102)` 与文档口径要改；`eslId` 与 §3.2 三个函数仍缺 |
| **B. 按 v6.11 全面重转写 `InventoryDataSetter.as`** | `prologue`（含 `eslId`）+ 全部 22 个 `processX` + 103 项字面量表 + 相关 `Item`/`Weapon`/`Armor`/`Form` 常量 | 大（复刻整体返工） | 一次清掉「基线错源」；需重做 §8.1 逐项对照与 §12 采样验收 |
| **C. 维持 5.1 基线，只把已命中处补丁化** | 保留现有转写，只把 FALMERARROW 的 `18.0` 改成 `8.0` | 小 | **不解决问题**：表内其余常量、38→24 臂结构、`eslId` 仍错，后续采样会继续爆 |

### 需要先定的一个问题

`docs/phase4-design.md` 与 `docs/inventory-system-analysis.md` 一直把基线写作 **skyui 5.2**
（例：phase4-design.md:534「换 SWF 版本（非 skyui 5.2）时本风险回归」、:1545「UI 数值与 skyui 原版逐项一致」）。
现在需要先回答：**本项目的对照对象是「本机实际安装的 UI = SkyUI-Community v6.11」，还是「某个指定版本的 stock SkyUI」？**
这个答案决定上表选 A / B / C，也决定 `docs/` 里那批「51 组 material」「102 字面量」指标要不要整体重算。

---

## 6.1 实施记录（方案 B，2026-09-22）

`ProcessEntryReplica.h` / `ProcessEntryReplica.cpp` 已按 v6.11 权威源整体重转写。逐项：

| 项 | 改动 |
| --- | --- |
| `ProcessEntryReplica.h` | `T` 枚举 102 → **103**，按 v6.11 首现顺序重抽；`kTranslationKeys` 同步；`static_assert(kTranslationCount == 103)` |
| `Ctx` | 新增 `double eslId`（`processMiscBaseId` 的 `0xFE` 臂读它） |
| `prologue()` | 新增 `eslId = formId & 0xFFF` 写入；`infoValue`/`infoWeight` 的判据改为 v6.11 的 `<= 0 ? null : round` 极性（NaN 情形与 5.1 不同，已在注释中登记） |
| `materialKeywords()` | 38 臂 → **24 臂**，v6.11 顺序，Stormcloak 提到 Imperial 之前，新增 CC dark/golden、Ordinator、Amber、Madness，删除嵌套 Deathbrand |
| `weaponSubType()` | 新增 `ccBGSSSE001_FishingPoleKW` → `TYPE_FISHINGROD`(13) 内嵌早返回 |
| `weaponBaseId()` | 改成 `formId >>> 24` 双层开关；新增 LONGBOW 组 → WOOD、DLC2 pickaxe 组 → STEEL |
| `armorPartMask()` / `armorOther()` | 新增 `PARTMASK_CLOAK`(0x10000) → `EQUIP_CLOAK`(15)+`$ClothingCloak`、`PARTMASK_BACKPACK`(0x20000) → `EQUIP_BACKPACK`(14)+`$Backpack`（掩码值原本就对，只是旧源码把它叫 UNNAMED16/17） |
| `armorBaseId()` | 改成 `formId >>> 24` 双层开关；`default` 臂新增 `BASEID_CC025ADVDSGSRING` |
| `ammoBaseId()` | 改成 `formId >>> 24` 双层开关；DRAUGRARROW / DUNGEIRMUND… 移入 STEEL；删除 FORSWORNARROW 臂与 Riekling spear 的 `$Spear` 覆盖 |
| `soulGemBaseId()` | 新增 SOULTOMATO 臂（`0x804` / `0x137F40` → `Item.SOULGEM_SOULTOMATO`(7) + `$SoulTomato`） |
| **新增** `bookBaseId()` | v6.11 `:1190-1255`（地图 / 上古卷轴 / CC 钓鱼图） |
| **新增** `scrollBaseId()` | v6.11 `:1256-1290`（DLC2 蜘蛛卷轴 ×21） |
| **新增** `potionBaseId()` | v6.11 `:1291-1303`（CC 艾雷德水晶药水）；两条 Heartland 标签是**死代码**（常量无值 = `undefined`），已在注释中说明为何不转写 |
| `miscBaseId()` | 6 臂 → **7 个插件槽分支 / 189 个 id**（含 `0xFE` 走 `eslId`） |
| `body()` | `case 23/27/46` 补上 `scrollBaseId` / `bookBaseId` / `potionBaseId` 调用；行号引用全部更新为 v6.11 |

**机器验证（非目视）**

* `material` 写入序列：复刻 **40** 处 ↔ v6.11 **40** 处，`(值, display)` 逐项比对 **完全一致**：
  `3/kDaedric 4/kDragon 5/kDwarven 6/kEbony 7/kElven 9/kGlass 10/kHide 21/kStormcloak 11/kImperial
   12/kIron 13/kLeather 16/kOrcish 20/kSteel 18/kSilver 8/kFalmer 1/kBonemold 2/kChitin 15/kNordic
   19/kStalhrim 8/kFalmer 17/kOrdinator 0/kAmber 14/kMadness 22/kWood 22/kWood 20/kSteel 3/kDaedric
   6/kEbony 9/kGlass 7/kElven 5/kDwarven 16/kOrcish 15/kNordic 8/kFalmer 20/kSteel 12/kIron 7/kElven
   12/kIron 5/kDwarven 22/kWood`
* 枚举引用：`T::` 使用 103 个，全部存在于枚举；枚举中无未使用项。
* 编译：`cmake --build --preset relwithdebinfo` → **BUILD_EXIT=0**，0 error / 0 warning（`/W4 /WX`）；产物 `build/bin/RelWithDebInfo/Template.dll` = **838,656 B**（`iconLabel` 第二个缓存落地后为 **839,680 B**，见 §6.1 验收状态第 3 条）。

**验收状态（2026-09-22 更新）**

1. ✅ **游戏内复采已通过**（2026-09-22 23:32，实际 **4 轮**）：`0 mismatch` / `11/11 branch exercised`，
   且旧日志里 `material` 那一行已消失（Falmer 箭自身 identical）。原始日志见 §6.2。
2. ✅ **OFF 构建尺寸核对已通过（2026-09-23）**：`SSE_REPLICATE_PROCESS_ENTRY=0` 重建 = **786,432 B**
   （§8.2 回退契约），且二进制内**零复刻字符串**（`4b-ii` / `default_potion` / `iconLabel` /
   `subTypeDisplay` 全 0）；改回 `1` 重建回 **839,680 B**。
3. ✅ **`iconLabel` 已预测（2026-09-23）**：v6.11 `processMiscBaseId` 的 `MISCARTIFACT` 臂多写一个成员
   `a_entryObject.iconLabel = "default_potion"`（`:970`）。这是全文件唯一用**裸字符串字面量**而非
   `Translator.translate` 写出的成员，而复刻的 `WriteSet` 按「翻译键」写字符串（每串一个缓存 managed
   `GFxValue`）。已按「第二个缓存」落地（不是再加一条表项）：`T::kRawDefaultPotion` 排在 `kCount` 之后
   使 `kTranslationCount` 仍为 103、单值缓存 `g_rawLiteralValue`、`WriteSet::putLiteral`、`apply` 按 key
   分派。影子比对现在能看到该成员。⚠️ **真机复验待做**（本改动改变了预测集）—— 2026-09-23：含本改动的 **839,680 B** 产物**已部署**至 MO2（两处 SHA-256 实测一致 = `296D04A6…805F8`），只等一次新采集；基于 838,656 B 的 **采集 #17 不能代替它**（见 [tracy-capture-log.md](./tracy-capture-log.md) #17.3）。
4. ✅ **旧文档指标已重算**：`docs/phase4-design.md` 里基于 5.1 的三处口径（「51 组 material」
   「102 字面量」「§8.1 逐项对照」）已在新增的 §10.10 登记为 **40 组 / 103 字面量**，
   §10.9 标题处亦标注该节数字属 **5.1 基线历史值**。2026-09-22 又补齐其余基线引用：
   `inventory-system-analysis.md` §7.4 加勘误 + §8.2 新增 v6.11 权威源行，`phase4-design.md`
   §3.1 / §7.3 / 附录 B 三处加基线注，`tracy-capture-log.md` §14 加横幅。

### 6.2 真机验收（2026-09-22 23:32，v6.11 重转写首测）

采集 `TracyLog/log_2026_09_22_23_32.tracy`；日志 `…/SKSE/Template.log`（游戏 1.5.97，4 轮纯开 / 关背包）。
**这是 v6.11 重转写后第一次真机实测，判据全过。**

| 判据 | 观测值 |
| --- | --- |
| 翻译字面量 | `translation spike resolved … 103 literals to resolve` / `all 103 display strings resolved`（旧 5.1 基线为 **102**） |
| 轮数 | **4 组汇总行**（23:31:46 / :48 / :50 / :52 各一条 `previous round`）—— ⚠️ 该行由**下一轮**的首个 `RequestItemCardInfo` 触发，末轮没有「下一轮」，故**行数 = 轮数 − 1**；**实际跑了 5 轮**（`AS::processEntry` = **32,205** = 6441 × 5，见采集 #17） |
| 每轮调用 | `processEntry calls 6441, replicated in C++ 6424, shadow-validated (forwarded on purpose) 17, forwarded 0, declines 0, apply failures 0` |
| 每轮影子对照 | **`17 item(s) / 190 member(s), 0 mismatch(es), 11/11 formType branch(es) exercised`** |
| **原缺陷（Falmer 箭）** | `formType 42 formId 230209`（= `0x38341`）→ **`all 14 predicted member(s) identical`**；旧日志里的 `material 18 vs 8` 差异行**消失** |
| 每件预测成员数 | 因 `eslId` 补齐而统一 **+1**：scroll 11 / armor 16 / book 11 / ingredient 9 / light 9 / misc 10 / weapon 15 / ammo 14 / key 9 / potion 12 / soulgem 11 |

> `4b/outlier`（formType 27 ≈ 6 ms、formType 26 ≈ 7 ms）仍在，属既有的 AS2 堆暂停，与正确性无关。
>
> **zone 级耗时已回填（2026-09-23，`tracy-csvexport` 聚合）**：`AS::processEntry` = **10,748 ns/件**（32,205 次，合计 **69.23 ms/轮**，`min` 2,304 ns）。对照见 [tracy-capture-log.md](./tracy-capture-log.md) **采集 #16 / #17**：同一 zone 在「5.1 基线 + 分支化」下为 **10,535 ns** —— 即在分支化已生效的同一前提下，**本次 v6.11 换代花了 +2.0 % 的单件成本**（与每件多 1 个 `eslId` 成员一致）；而相对分支化**前**的基线（两采样均值 12,110 ns ≈ 78.0 ms/轮），本条 trace 仍是 **−10.1 ms/轮** 量级的净下降（归因口径与噪声底见 #16.3）。

---

## 7. 复现命令（供第三方复核）

```powershell
# 1) 反编译运行版全部 SWF（需要 JPEXS FFDec，Java >= 8）
java -jar ffdec-cli.jar -export script <out> "<…>\interface\skyui\inventorylists.swf"
java -jar ffdec-cli.jar -export script <out2> "<…>\interface\inventorymenu.swf"

# 2) 权威源码（tag v6.11）
git clone --depth 1 --branch v6.11 https://github.com/doodlum/SkyUI-Community.git

# 3) 版本戳
Select-String -Path <out2>\**\*.as -Pattern 'SKYUI_VERSION_STRING\s*=\s*(.+);'

# 4) 字面量抽取（按首现顺序去重）——旧源码应得 102，v6.11 应得 103
Get-Content <file>.as | ForEach-Object {
    foreach ($m in [regex]::Matches($_, 'translate\("([^"]*)"\)')) { $m.Groups[1].Value }
} | Select-Object -Unique

# 5) material 赋值序列
Get-Content <file>.as | ForEach-Object {
    foreach ($m in [regex]::Matches($_, 'Material\.([A-Z]+)')) { $m.Groups[1].Value }
}
```

本机产物位置：反编译 `build/ffdec/out_all`、`build/ffdec/out_inv`（46 个 SWF）；权威源码
`extern/SkyUICommunity_v6.11_权威源/`（tag v6.11，浅克隆）；FFDec 26.3.0 位于 `build/ffdec/app/`。





