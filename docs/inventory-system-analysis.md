# 天际 (Skyrim SE/AE) 背包系统拆解与性能分析

> 本文档记录 Skyrim 运行时背包系统（`InventoryChanges` / `InventoryEntryData` / `ExtraDataList` 链路）的完整拆解结果与性能瓶颈分析，作为**后续优化路线决策**的依据。
> **⚠️ 2026-09-19 实测已把方向从「哈希索引」修正为「定位 + 缓存」**（遍历 < 1 %），详见 §5.2 / §7.3 与 [tracy-capture-log.md](./tracy-capture-log.md) §7。
>
> - 目标项目：`SSEMod`（SKSE 插件 / CommonLibSSE-NG 模板）
> - 分析依据：`extern/CommonLibSSE-NG` 中的引擎结构定义与函数实现
> - 运行时依赖：SKSE + Address Library for SKSE Plugins

---

## 1. 概述

### 1.1 问题定义

游戏内物品条目（item entry）数量增大后，出现以下可感知卡顿：

- 打开背包 / 容器 / 交易界面时有明显延迟（数秒级）
- 频繁增删物品（拾取、转移、丢弃）时掉帧
- 脚本（Papyrus / SKSE 插件）批量查询物品数量时卡顿

### 1.2 结论摘要

> ⚠️ **2026-09-19 实测修正（详见 [tracy-capture-log.md](./tracy-capture-log.md) §7）：「数据结构缺陷是卡顿根因」的假设已被 Tracy 实测否定。**
>
> 五组对照实验（vanilla / +UI Extensions / +skyui / 组合 / +Infinity UI）显示：
> - **遍历仅占打开背包耗时的 < 1 %**（vanilla ≈ 0.4 %），`GetValue` 严格线性（`≈ 3 × n`），无 `n × m` 放大；
> - vanilla 打开「极大量」背包仅 **~0.8 s**；数秒卡顿是 **skyui** 在菜单打开后对全部物品做一次确定性遍历（**27211 次 `GetValue`/次**）时**伴随的 Papyrus / GFx / 排序操作**造成的（稳态 ~2.2 s）；skyui 的发行物是 **esp + bsa（无 DLL）**，引擎桥接靠**开源的 SKSE**，源码查证已把它定位为「对全部 27211 件物品**逐项**发起 `RequestItemCardInfo` **GFx 委托往返**」（≈ 55 µs/次，见 §7.4）；
> - **UI Extensions 经单独测试确认零影响**（`GetValue` 与 wall time 均与 vanilla 一致）；Infinity UI 仅首轮贡献 +2.1 s 初始化；
> - → **§7 的「阶段 2a 哈希索引」收益 ≈ 0，不应优先实施。**

> **原结论（已被实测修正）：根因是数据结构缺陷，而非 UI / Scaleform 渲染问题。**

背包运行时数据使用**单链表（`BSSimpleList`）+ 零索引**的结构：

- `InventoryChanges::entryList` —— 单链表，所有按物品查找/计数操作都是 **O(n)** 线性遍历
- `InventoryEntryData::extraLists` —— 第二层单链表，每个物品的属性查询都要再遍历一次
- UI 打开时对每条目发起多次属性查询 → 总复杂度退化到 **O(n × m)**

> ⚠️ **实测修正（2026-09-19）**：原先的判断「卡顿源于数据结构、前端 mod 只能治标」**已被否定** —— 实测卡顿正是 **skyui**（前端 mod）造成的，而数据结构遍历只占 < 1 %。
> 因此结论**反转**：真正可行的方向是**给 skyui 这类前端 mod 提供旁路缓存**（或先定位其耗时层再对症优化），**而非旁挂哈希索引**。见 §7.3；skyui 架构（esp + bsa、无 DLL、UI 层 MIT 开源）与缓存的可实施位置（**SKSE 桥接层**）见 §7.4。

### 1.3 关键术语

| 术语 | 含义 |
|------|------|
| base form | 物品的静态定义（`TESBoundObject` 及其派生，如 `TESObjectWEAP`），来自 ESP，只读 |
| container / 容器 | `TESObjectREFR` 上挂载的背包数据，分为静态定义与运行时增量两部分 |
| entry / 条目 | 运行时背包中的一条记录（`InventoryEntryData`），绑定一个 base form 与增量数量 |
| instance / 实例 | 同一条目下具有独立附加数据的具体物品（`ExtraDataList`），如「附了不同魔的 3 把匕首」 |

---

## 2. 三层数据结构

背包不是单一结构，而是 **「静态定义 + 运行时增量 + 每条目附加数据」三层叠加**：

```
┌───────────────────────────────────────────────────────────┐
│ TESObjectREFR            (实例：玩家 / 箱子 / NPC)          │
│   └─ ExtraDataList extraList                // 偏移 0x70   │
│        └─ ExtraContainerChanges  (类型 0x15)               │
│             └─ InventoryChanges* changes    ← 运行时背包入口│
└───────────────────────────────────────────────────────────┘
```

`TESObjectREFR::GetInventoryChanges()` 就是通过 `extraList` 上的 `ExtraContainerChanges` 拿到 `InventoryChanges`，详见 §3.4。

### 2.1 第 1 层：静态容器 `TESContainer`（数组，性能正常）

这一层是 **定义层**，表示箱子/NPC 模板「本来就有」的物品，来自 ESP 定义，存的是**数组**：

```cpp
// extern/CommonLibSSE-NG/include/RE/T/TESContainer.h
struct ContainerObject {                    // sizeof == 0x18
    std::int32_t        count;      // 00 - CNTO~
    std::uint32_t       pad04;      // 04
    TESBoundObject*     obj;        // 08 - ~CNTO
    ContainerItemExtra* itemExtra;  // 10 - COED（所有者）
};

class TESContainer : public BaseFormComponent {   // sizeof == 0x18
    ContainerObject** containerObjects;     // 08 - 数组
    std::uint32_t     numContainerObjects;  // 10
    bool              allowStolenItems;     // 14 - new in 1.6.1130
};
```

访问方式为下标遍历（`ForEachContainerObject`），复杂度 **O(n) 但常数极小**，且不是瓶颈所在。

> 注意：`TESContainer::AddObjectToContainer` / `RemoveObjectFromContainer` 是 CommonLib 侧对数组的增删实现（删除时整体 `CopyObjectList` 重建数组）。这一层**不是**本文关注的性能病灶。

### 2.2 第 2 层：运行时容器 `InventoryChanges`（单链表，核心病灶）

这一层是 **实例增量层**，表示玩家实际拾取/丢弃产生的变化，会随存档持久化：

```cpp
// extern/CommonLibSSE-NG/include/RE/I/InventoryChanges.h
class InventoryChanges {                          // sizeof == 0x20
    BSSimpleList<InventoryEntryData*>* entryList; // 00  ⚠️ 单链表，性能问题根源
    TESObjectREFR*                     owner;     // 08
    float                              totalWeight;// 10
    float                              armorWeight;// 14
    bool                               changed;    // 18
    // 19/1A/1B/1C 为未知填充
};
```

`BSSimpleList`（`RE/B/BSTList.h`）是 Bethesda 的**侵入式单链表**：

```cpp
template <class T>
class BSSimpleList {
    struct Node {
        value_type           item;  // 00
        stl::observer<Node*> next;  // 08
    };
    // ...
};
```

**没有数组、没有哈希表、没有 size 缓存**——每次查找都是从头 `next` 到尾。

### 2.3 第 3 层：条目 `InventoryEntryData` + 附加数据 `ExtraDataList`（第二层链表）

```cpp
// extern/CommonLibSSE-NG/include/RE/I/InventoryEntryData.h
class InventoryEntryData {                                  // sizeof == 0x18
    TESBoundObject*               object;       // 00 - 物品 base form
    BSSimpleList<ExtraDataList*>* extraLists;   // 08 ⚠️ 又一个单链表
    std::int32_t                  countDelta;   // 10 - 相对静态容器的数量增量
    std::uint32_t                 pad14;        // 14
};
```

`extraLists` 表示**同一 base form 下每个具有独立附加数据的实例**。例如背包里有 3 把附魔不同的铁匕首，就是 **1 个 `InventoryEntryData`，其 `extraLists` 上挂 3 个 `ExtraDataList`**。

每个 `ExtraDataList` 内部：

```cpp
// extern/CommonLibSSE-NG/include/RE/E/ExtraDataList.h
class BaseExtraList {
    struct PresenceBitfield {                 // sizeof == 0x18
        std::uint8_t bits[0x18];              // 24 字节位图，标记「存在哪些类型」
        bool HasType(std::uint32_t a_type) const;   // O(1) 位运算
        void MarkType(std::uint32_t a_type, bool a_cleared);
    };
    BSExtraData*      data     = nullptr;     // 00 - BSExtraData 单链表头
    PresenceBitfield* presence = nullptr;     // 08
};                                            // sizeof == 0x10

class ExtraDataList {
    BaseExtraList           _extraData;  // 00
    mutable BSReadWriteLock _lock;       // 10（AE 1.6.629+ 偏移 18）
};
```

`ExtraDataList` 内保存的附加数据决定了物品的「实例身份」，常见类型：

| 类型 | 含义 |
|------|------|
| `ExtraEnchantment` / `ExtraCharge` | 附魔与充能 |
| `ExtraPoison` | 涂毒 |
| `ExtraHotkey` | 收藏（favorite） |
| `ExtraWorn` / `ExtraWornLeft` | 装备状态（右手/左手） |
| `ExtraOwner` | 所有者（偷窃判定） |
| `ExtraLeveledItem` | 来自等级列表 |
| `ExtraCount` | 实例数量 |
| `ExtraTextDisplayData` | 自定义显示名 |

> **重要性能细节**：`HasType<T>()` 走 presence 位图，是 **O(1)**；但 `GetByType<T>()` 需要遍历 `data` 链表才能拿到指针，是 **O(k)**。而 `extraLists` 本身是链表，所以「遍历所有实例找某属性」= **O(实例数 × k)**。

---
## 3. 操作层拆解

### 3.1 读（查询）路径 —— 全部线性遍历

| 函数 | 实现来源 | 复杂度 | 说明 |
|------|----------|--------|------|
| `InventoryChanges::GetItemCount(TESBoundObject*)` | 引擎函数（reloc `15868/16047`） | O(n) | 遍历 `entryList` 累加 `countDelta`，返回 **`int16_t`** |
| `InventoryChanges::GetInventoryWeight()` | 引擎函数（reloc `15883/16123`） | O(n) | 遍历 `entryList` 累加重量 |
| `InventoryChanges::GetArmorInSlot(int32_t)` | 引擎函数（reloc `15873/16113`） | O(n) | 找指定槽位护甲 |
| `InventoryChanges::GetWornMask()` | 引擎函数（reloc `15806/16044`） | O(n) | 计算已装备槽位掩码 |
| `InventoryChanges::VisitInventory(IItemChangeVisitor&)` | 引擎函数（reloc `15855/16095`） | O(n) | 遍历 `entryList` 并回调 visitor |
| `InventoryChanges::VisitWornItems(IItemChangeVisitor&)` | 引擎函数（reloc `15856/16096`） | O(n) | 只访问已装备条目 |
| `TESObjectREFR::GetInventory(filter, noInit)` | **CommonLib 自实现** | O(n log n) | 遍历 `entryList` + `containerObjects`，结果写入 `std::map` |
| `TESObjectREFR::GetInventoryCounts(...)` | 调 `GetInventory` | O(n log n) | 再转换一次 map |
| `InventoryEntryData::GetValue()` | 引擎函数（reloc `15757/15995`） | O(m) | 遍历 `extraLists` |
| `InventoryEntryData::GetDisplayName()` | **CommonLib 自实现** | O(m) | 遍历 `extraLists` 取 `ExtraTextDisplayData` |
| `InventoryEntryData::GetEnchantment()` | 引擎函数（reloc `15788/16026`） | O(m) | 遍历 `extraLists` |
| `InventoryEntryData::IsWorn()/IsFavorited()/IsEnchanted()/IsPoisoned()/IsLeveled()` | **CommonLib 自实现** | O(m) | 遍历 `extraLists` + `HasType<T>()` |
| `InventoryEntryData::GetOwner()/GetSoulLevel()/IsQuestObject()` | **CommonLib 自实现** | O(m) | 遍历 `extraLists` |

`TESObjectREFR::GetInventory` 的引擎原生等价实现在 CommonLib 中也可见（简化为数组/链表两段遍历）：

```cpp
InventoryItemMap TESObjectREFR::GetInventory(std::function<bool(TESBoundObject&)> a_filter, bool a_noInit)
{
    InventoryItemMap results;                       // std::map（红黑树，O(log n) 插入）

    auto invChanges = GetInventoryChanges(a_noInit);
    if (invChanges && invChanges->entryList) {
        for (auto& entry : *invChanges->entryList) {          // ← 第 1 层链表 O(n)
            if (entry && entry->object && a_filter(*entry->object)) {
                results.emplace(entry->object,
                    std::make_pair(entry->countDelta,
                                   std::make_unique<InventoryEntryData>(*entry)));
                // ⚠️ 这里对每个 entry 做了一次「深拷贝」（拷贝构造会 new 一份 extraLists 链表）
            }
        }
    }

    auto container = GetContainer();                          // 静态定义层
    if (container) {
        // ... 合并 containerObjects，遇到已存在的条目则 count 相加 ...
    }
    return results;
}
```

> **隐藏成本**：上面对每个 entry 调用 `std::make_unique<InventoryEntryData>(*entry)`，触发 `InventoryEntryData` 的拷贝构造，**为每个条目重新分配一条 `extraLists` 链表**。在 n 很大时，这是一次大量的堆分配。

### 3.2 写（增删）路径 —— 分散且无索引

| 函数 | 说明 |
|------|------|
| `InventoryChanges::AddEntryData(InventoryEntryData*)` | **CommonLib 自实现**：`entryList->push_front(a_entry); changed = true;` |
| `TESObjectREFR::AddObjectToContainer(...)` | 虚函数槽 `5A`，加物品的真实入口之一 |
| `TESObjectREFR::AddWornItem(...)` | 虚函数槽 `57`，加物品并直接装备 |
| `TESObjectREFR::RemoveItem(...)` | 虚函数槽 `56`，删物品（返回掉落引用 `ObjectRefHandle`） |
| `TESObjectREFR::set*` / `Actor` 覆写 | `Actor::RemoveItem` 为其 override（`Actor.h:300`） |
| `InventoryChanges::RemoveAllItems(...)` | 引擎函数（reloc `15878/16118`） |
| `TESContainer::AddObjectToContainer` / `RemoveObjectFromContainer` | 对静态数组的增删（删除时整体 copy 重建，O(n)） |

> **关键证据 —— 写入口是分散、内联的**：`InventoryChanges::AddEntryData` 在 CommonLib 中并非 `REL::Relocation` 转发，而是**自己实现的 `push_front`**。这说明引擎真正「创建/插入条目」的逻辑**内联在多个函数的内部**（`AddObjectToContainer`、`InitFromContainerExtra`、`GenerateLeveledListChanges` 等），并不存在一个统一的、可安全 hook 的公开写入口。
>
> → 这直接决定了「哈希索引」的落地策略：**不能依赖 hook 全部写入口来维护索引**，否则漏掉一处就会产生悬挂指针 / 崩溃 / 坏档。低风险方案应采用 **脏标记 + 懒重建**（见 §7）。
>
> ⚠️ **2026-09-19 实测**：该「哈希索引」方案已废弃（遍历 < 1 %，见 §7.3）；但本段结论对新的「背包快照」缓存（§7.3 阶段 3b）**同样适用** —— 快照同样依赖「脏标记 + 失效」，而非 hook 全部写入口。

### 3.3 事件 / 通知路径（写操作的副作用）

| 事件 / 函数 | 位置 | 说明 |
|-------------|------|------|
| `InventoryChanges::SendContainerChangedEvent(itemExtraList, fromRefr, item, count)` | 引擎函数（reloc `15909/16149`） | 发送 `TESContainerChangedEvent` |
| `TESContainerChangedEvent` | `RE/T/TESContainerChangedEvent.h` | `oldContainer / newContainer / baseObj / itemCount / reference / uniqueID` |
| `Inventory::Event` | `RE/I/Inventory.h` | `objRefr / entryData / newCount / prevCount` + `BSTEventSource` |
| `BGSAddToPlayerInventoryEvent` | `RE/B/BGSAddToPlayerInventoryEvent.h` | 「物品进入玩家背包」事件（含拾取类型） |
| `ActorInventoryEvent` | `RE/A/ActorInventoryEvent.h` | 装备变化事件（`INVENTORY_EVENT`），被 `BoundItemEffect` / `EnhanceWeaponEffect` 等监听 |
| Papyrus `OnItemAdded` / `OnItemRemoved` | `SkyrimVM` 内 `InventoryEventFilterMap` | 脚本事件分发（受大量 mod 注册，是 UI 刷新的放大器） |

### 3.4 初始化路径

```cpp
// extern/CommonLibSSE-NG/src/RE/T/TESObjectREFR.cpp
InventoryChanges* TESObjectREFR::GetInventoryChanges(bool a_noInit)
{
    if (!extraList.HasType<ExtraContainerChanges>()) {   // presence 位图，O(1)
        if (a_noInit) {
            return nullptr;                              // 「只读不改」模式
        }
        if (!InitInventoryIfRequired()) {
            ForceInitInventoryChanges();                 // 惰性初始化：从 TESContainer 展开
        }
    }
    auto xContChanges = extraList.GetByType<ExtraContainerChanges>();
    return xContChanges ? xContChanges->changes : nullptr;
}
```

与初始化相关的其它函数：

| 函数 | 引擎 reloc | 说明 |
|------|-----------|------|
| `InventoryChanges::InitFromContainerExtra()` | `15890/16130` | 从 `ExtraContainerChanges` 初始化 |
| `InventoryChanges::InitLeveledItems()` | `15889/16129` | 展开等级列表物品 |
| `InventoryChanges::InitOutfitItems(outfit, npcLevel)` | `15833/16072` | 按 NPC Outfit 初始化 |
| `InventoryChanges::InitScripts()` | `15829/16068` | 初始化脚本附着 |
| `InventoryChanges::GenerateLeveledListChanges()` | `15829/16068` | 生成等级列表变更 |
| `InventoryChanges::Ctor/Dtor` | `15812/16050`、`15813/16051` | 构造 / 析构 |

---

## 4. 完整数据流

### 4.1 打开背包（`InventoryMenu`）——性能爆炸点

```
打开背包 / 容器 / 交易界面
  │
  ├─ 引擎取得 refr->GetInventoryChanges()            ← 必要时惰性初始化
  │
  ├─ 遍历 InventoryChanges.entryList                  ← 【O(n)】第 1 层单链表
  │    │   n = 对象上的条目数（玩家可达数千）
  │    │
  │    └─ for each entry:
  │         ├─ entry->GetDisplayName()   ← 遍历 entry->extraLists  【O(m)】
  │         ├─ entry->GetValue()         ← 遍历 extraLists           【O(m)】
  │         ├─ entry->GetWeight()        ← 取 object->GetWeight()
  │         ├─ entry->IsWorn()           ← 遍历 extraLists           【O(m)】
  │         ├─ entry->IsFavorited()      ← 遍历 extraLists           【O(m)】
  │         ├─ entry->IsEnchanted()      ← 遍历 extraLists           【O(m)】
  │         ├─ entry->GetEnchantment()   ← 遍历 extraLists           【O(m)】
  │         └─ ...（列表排序 / 分类 / 生成 ItemCard）
  │
  └─ Scaleform 端逐项回调 C++ 获取文本/数值（跨 FFI，额外开销）

总复杂度 ≈ O(n × m)
    n = 条目数
    m = 每条目发起的属性查询次数（含 extraLists 遍历）
```

### 4.2 增 / 删物品

```
AddObjectToContainer / RemoveItem / 脚本 AddItem …
  │
  ├─ 修改 InventoryChanges（插入 / 移除 / 更新 InventoryEntryData::countDelta）
  │     ⚠️ 写入点分散在内联逻辑中（见 §3.2）
  │
  ├─ InventoryChanges::SendContainerChangedEvent()
  │     └─ 广播 TESContainerChangedEvent
  │           ├─ → Papyrus OnItemAdded / OnItemRemoved（SkyrimVM 分发）
  │           ├─ → 各处 BSTEventSink（含大量第三方 mod）
  │           └─ → UI 标记背包「脏」，触发下一次全量刷新
  │
  └─ 背包 UI 下一次刷新再次执行 §4.1 的全套遍历
```

> **放大效应**：一次批量操作（如 `RemoveAllItems`、开箱取全部）会触发**多次**事件 → **多次**全量 UI 刷新 → §4.1 的 O(n × m) 被执行多遍。

---

## 5. 性能瓶颈精确定位

### 5.1 复杂度总表

| # | 瓶颈 | 根因 | 复杂度 | 严重度 |
|---|------|------|--------|--------|
| 1 | `entryList` 是单链表 | `BSSimpleList` 无索引，查找/计数/遍历全线性 | O(n) | ★★★ 核心 |
| 2 | `extraLists` 是单链表 | 每个属性查询都从头遍历 | O(m) | ★★★ 核心 |
| 3 | 无任何哈希/索引 | 相同 base form 无法 O(1) 命中 | — | ★★★ 核心 |
| 4 | UI 刷新时的查询组合 | 每条目多次属性查询 → n × m | O(n × m) | ★★★ 放大 |
| 5 | `GetInventory` 结果用 `std::map` | 红黑树插入 + 每 entry 深拷贝 | O(n log n) + 堆分配 | ★☆ 次要 |
| 6 | `GetByType<T>` 需遍历 `data` 链表 | 位图只能判断「有无」，取指针仍要遍历 | O(k) | ★☆ 次要 |
| 7 | 增删触发全量 UI 刷新 | 无脏区合并 / 节流机制 | — | ★★ 放大 |

### 5.2 关键定量解释

> ⚠️ **2026-09-19 实测：本节推导的 `n × m` 放大与「打开背包时遍历」在实际运行中均未出现。**
>
> | 实测项 | 结果 |
> |--------|------|
> | 遍历占「极大量」打开背包 wall time | **< 1 %**（vanilla ≈ 0.4 %） |
> | `GetValue` 随 `n` 的增长 | **严格线性**（`≈ 3 × n`），无 `n × m` |
> | vanilla 打开极大量背包 | **~0.8 s**（0→2000 物品持平 ~0.85–1.1 s） |
> | 数秒卡顿来源 | **skyui** 打开后全量遍历（27211 次 `GetValue`/次）伴随的 Papyrus / GFx 等操作 |
>
> 即：**链表遍历在实测中并非「打开背包卡几秒」的原因**。详见 [tracy-capture-log.md](./tracy-capture-log.md) §3 / §6 / §7。

设背包条目数 `n`（大型整合包玩家常见 500 ~ 3000），每条目平均属性查询次数 `m`（≈ 10 ~ 30，随 mod 增加）：

- 单次 `GetItemCount`：`n` 次指针跳转 → 对 n = 2000 约 2000 次随机内存访问
- 打开背包：`n × m` ≈ 2000 × 20 = **40,000 次链表节点访问 + 每项的 `extraLists` 遍历**
- 若再叠加 3 个查询物品数量的 mod 脚本（各自 `GetItemCount`）→ 再乘 3

这解释了「物品一多就卡几秒」的观感：**即使单次遍历很快，链表随机访问带来的 cache miss 也会让 n 较大时的时间快速上升。**

### 5.3 与静态层的对比

| | 静态定义 `TESContainer` | 运行时 `InventoryChanges` |
|--|------------------------|---------------------------|
| 存储 | `ContainerObject**` **数组** | `BSSimpleList` **单链表** |
| 按对象查找 | 下标/顺序遍历，cache 友好 | 从头遍历链表，cache 不友好 |
| 是否有索引 | 否，但连续内存 | 否，且节点分散 |

→ 说明 **Bethesda 自己也知道数组更快**（静态层用数组），只是运行时层受存档格式与历史包袱限制沿用了链表。

---

## 6. RelocationID 速查表

拆解过程中确认到的引擎函数地址（`RELOCATION_ID(SE, AE)`），供后续写 hook / 调用时参考。均取自 `extern/CommonLibSSE-NG/src/RE/**` 与 `include/RE/Offsets.h`。

### 6.1 `InventoryChanges`

| 函数 | SE | AE |
|------|----|----|
| `Ctor(TESObjectREFR*)` | 15812 | 16050 |
| `Dtor()` | 15813 | 16051 |
| `GetWornMask()` | 15806 | 16044 |
| `GetItemCount(TESBoundObject*)` | **15868** | **16047** |
| `GetArmorInSlot(int32_t)` | 15873 | 16113 |
| `GetInventoryWeight()` | 15883 | 16123 |
| `VisitInventory(IItemChangeVisitor&)` | **15855** | **16095** |
| `VisitWornItems(IItemChangeVisitor&)` | 15856 | 16096 |
| `InitFromContainerExtra()` | 15890 | 16130 |
| `InitLeveledItems()` | 15889 | 16129 |
| `InitOutfitItems(BGSOutfit*, uint16_t)` | 15833 | 16072 |
| `InitScripts()` | 15829 | 16068 |
| `GenerateLeveledListChanges()` | 15829 | 16068 |
| `RemoveAllItems(...)` | 15878 | 16118 |
| `SetFavorite(...)` | 15858 | 16098 |
| `RemoveFavorite(...)` | 15859 | 16099 |
| `GetNextUniqueID()` | 15908 | 16148 |
| `SendContainerChangedEvent(...)` | **15909** | **16149** |
| `SetUniqueID(...)` | 15907 | 16149 |
| `TransferItemUID(...)` | 15909 | 16149 |

> ⚠️ 上游标注存在可疑之处，若需 hook 请先自行反汇编核对：
> - `InitScripts` 与 `GenerateLeveledListChanges` 标注为**同一 reloc（15829/16068）**；
> - `SetUniqueID` / `SendContainerChangedEvent` / `TransferItemUID` 三者的 **AE 值均为 16149**（但 SE 分别为 15907 / 15909 / 15909）。

### 6.2 `InventoryEntryData`

| 函数 | SE | AE |
|------|----|----|
| `GetValue()` | **15757** | **15995** |
| `GetEnchantment()` | **15788** | **16026** |
| `PoisonObject(...)` | 15786 | 16024 |
| `IsOwnedBy_Impl(...)` | 15782 | 16020 |

### 6.3 装备与事件源（`include/RE/Offsets.h`）

| 函数 | SE | AE |
|------|----|----|
| `ActorEquipManager::EquipObject(...)` | 37938 | 38894 |
| `ActorEquipManager::UnequipObject(...)` | 37945 | 38901 |
| `ActorEquipManager::Singleton` | 514494 | 400636 |
| `Inventory::GetEventSource()` | 15980 | 16225 |

### 6.4 常量与成员偏移

| 项 | 值 |
|----|-----|
| `ExtraDataType::kContainerChanges` | `0x15` |
| `TESObjectREFR::extraList` 偏移 | `0x70` |
| `InventoryChanges::entryList` 偏移 | `0x00` |
| `InventoryEntryData::extraLists` 偏移 | `0x08` |
| `InventoryEntryData::countDelta` 偏移 | `0x10` |
| `ExtraDataList::_extraData` 偏移 | `0x00` |
| `ExtraDataList::_lock` 偏移 | `0x10`（AE 1.6.629+ 为 `0x18`） |

---

## 7. 对优化方案的启示

### 7.1 结论：保留引擎链表为真源（加速方向已修正）

> ⚠️ **2026-09-19 修正**：本节「旁挂哈希索引」的**加速方案已被实测否定**（遍历占比 < 1 %，收益 ≈ 0，见 §7.3）；但「**引擎链表是唯一真源、不可替换**」这一前提判断**仍然成立**，且同样适用于新的「缓存」路线（缓存是只读旁路，不改变真源）。

`InventoryChanges` 是引擎的**真源数据**，被存档 / 装备 / Papyrus / UI / AI 等十余个子系统直接按内存字段读取，**无法被接管替换**（重写背包 + 独立序列化方案已论证不可行：双真源 + 引用重建 + 坏档风险过高）。

因此正确路线是：

```
引擎链表（entryList / extraLists）  ← 真源，保持不变，继续由引擎读
        ▲
        │ 旁挂（只读旁路，不改变真源）
        ▼
加速层：~~哈希索引~~ → 改为「缓存 skyui 高频查询」  ← 只加速「读」，方案见 §7.3
```

### 7.2 由拆解得出的具体判断

| 拆解结论 | 对方案的影响 |
|----------|--------------|
| `entryList`、`extraLists` 是仅有的两处核心病灶 | 索引设计聚焦这两处即可，无需动 `TESContainer` |
| **写入口分散且内联**（§3.2，`AddEntryData` 非引擎函数） | ❗ 不能依赖 hook 全部写入口维护索引 → **低风险版必须用「脏标记 + 懒重建」** |
| 读路径函数签名清晰、有 reloc（§6） | 读侧 hook / 包装点明确，易于加装索引旁路 |
| `ExtraDataList` 已有 presence 位图（`HasType` O(1)） | 属性「有无」判断已够快，优化应聚焦 `GetByType`（取指针）与 `extraLists` 遍历 |
| `GetInventory` 对每 entry 做深拷贝（§3.1） | 该函数本身可作为独立优化点（避免无谓拷贝） |
| 增删触发全量 UI 刷新（§4.2） | UI 刷新节流是独立收益项，可与索引层解耦实施 |
| 物品本体数据必须由引擎原生存档管理 | 索引只在**内存**中重建，**不写入 co-save** |

### 7.3 建议的分阶段路线（供后续决策）

> ⚠️ **2026-09-19 实测判决（详见 [tracy-capture-log.md](./tracy-capture-log.md) §7）**：阶段 1 已完成（Tracy 集成 + 6 轮采集）。
> 实测推翻了「哈希索引」路线的前提——**遍历仅占打开背包耗时的 < 1 %**，数秒卡顿的根因是 **skyui** 打开后的全量遍历伴随开销（`GetValue` 27211 次/次仅 ~11.5 ms，卡顿在其伴随的 **GFx 委托往返 / 排序 / 分配**里，见 §7.4）。
> **架构查证（§7.4）修正**：skyui **不是 DLL，而是 esp + bsa**（`interface/*.swf` + `scripts/*.pex`），引擎桥接由**开源的 SKSE** 承担 —— 「skyui 闭源」不成立；其 12 个 Papyrus 脚本里**没有**物品遍历逻辑（遍历在 ActionScript 内），且 AS 层**也不经 Papyrus VM** → **那 ~1.5 s 已由源码定位为 27211 次 `RequestItemCardInfo` 的 GFx 委托往返**（§7.4）。故路线改为「**降低该委托的单次成本 / 提供批量数据通道**」，而非「改造引擎数据结构」或「Papyrus 层 memoization」。

| 阶段 | 内容 | 风险 | 状态 / 目标 |
|------|------|------|------------|
| **阶段 1** | Profiling 诊断：hook §6 读路径靶点，统计调用次数 / 单次耗时 / 规模关系 | 极低（只测量） | ✅ **已完成**（2026-09-19，6 轮）→ 遍历 < 1 %，否定哈希索引路线 |
| **阶段 2** | **用实测确认那 ~1.5 s 的归属**：hook `RequestItemCardInfo` 的 C++ 委托实现（**首要**）+ `TESObjectREFR::GetInventory` + `InventoryEntryData::GetDisplayName`·`GetEnchantment`·`GetWeight`；反向确认 Papyrus VM 调用 ≈ 0 | 低（只测量） | ⬜ **待做** → 量化「全量物品 × GFx 往返」 |
| **阶段 3a′** | **降单次成本**（已定位：1.5 s = 27211 × `RequestItemCardInfo` GFx 往返，见 §7.4）：hook 引擎 `InventoryMenu` 的该委托实现，只构建 skyui 真正需要的字段 / 复用 item card 构建结果；`InventoryMenu opened` 建缓存，`SendContainerChangedEvent` / `closed` 失效 | 中（失效不当会显示陈旧数据） | ⬜ 视阶段 2 实测 |
| **阶段 3b** | **缓存（若 1.5 s 在整包枚举层）**：背包快照 + 脏标记，首次查询一次性构建 `{entry → 属性}`，后续 O(1) 命中 | 中 | ⬜ 视阶段 2 |
| **阶段 4** | 备选（若 1.5 s 在 GFx / ActionScript 层，引擎侧缓存不可达） | — | ⬜ 接受现状 / 减少可见 n / 换 UI 组合 |

> ❌ **已废弃**：原「阶段 2a / 2b 哈希索引」（实测遍历占比 < 1 %，收益 ≈ 0）、原「阶段 3 UI 刷新节流」（无证据表明其为主导）。
>
> **阶段 2 的验收判据**：把 skyui 那 ~1.5 s 归属到「**GFx 委托往返** / 引擎枚举 / AS 侧计算」三者之一 —— 这直接决定阶段 3 走 **3a′** 还是 3b（或阶段 4）。
> 当前**首要结论**（§7.4，源码级）：skyui 的 `ItemcardDataExtender.processList()` 对全部 27211 个条目**各调一次** `GameDelegate.call("RequestItemCardInfo", …)`（GFx → C++ 往返，C++ 侧构建完整 item card）；而 `GetValue` 只是一次 item card 构建里 ~400 ns 的一小步。由 1.5 s ÷ 27211 反推 **≈ 55 µs / 次**。**验证手段**：hook 该委托的 C++ 实现并包 zone，核对「次数 ≈ 27211 × 轮数、单次 ≈ 55 µs」。
>
> **阶段 1 的落地方案**（Tracy 集成、编译开关、hook 模板、插桩靶点表、验收判据）单独成文：[tracy-integration-plan.md](./tracy-integration-plan.md)；**阶段 2 的插桩靶点已补录至该文 §4.6**。代码侧已落地（`CMakeLists.txt` 的 `ENABLE_TRACY` 开关、`src/Profiling.h`、`src/ProfilingHooks.h/.cpp`、`src/Util.h` 的 `writeBranch`），落地记录与偏差说明见该文 **附录 A / 附录 B**。
>
> ✅ **阶段 2 已闭环**（2026-09-19，采集 #7–#10）：上表「阶段 2」的判据已全部回答 —— skyui 增量 **+779 ms** = C++ 实现 **414 ms（53 %）** + GFx **引擎侧**边界 **1 ms（0.1 %）** + **AS 层 364 ms（47 %，Tracy 视野外）**。
> → **3b 依据不成立**（属性查询合计 < 2 %）；**3a′ 天花板 53 %**（可做但有限）；上表「阶段 4」一行原写的「接受现状 / 减少可见 n / 换 UI 组合」**已被推翻**，改为「**批量数据通道 / 改消费方式**」的根治路线（唯一能同时触到 53 % 与 47 % 的方向）。
>
> 📄 **阶段 4 的落地方案**：[phase4-design.md](./phase4-design.md) —— 给出**不改 SKSE、不编译 SWF** 的运行时 AS 热替换路径（`GFxMovie::CreateFunction` + `SetVariable`），并明确其**第一步是测量**：把 47 % 拆成「往返编组 / 循环本体 / `processEntry` / 列表 UI」四段，据此决定最终方案。

### 7.4 SkyUI 架构查证与 1.5 s 归属定位（2026-09-19 补充）

> 本节记录对 skyui 发行物与源码的两轮查证结论。本地材料：`extern/SkyUI/`（BSA 提取物）、`extern/SkyUISrc/`（MIT 源码，停更十余年）、`extern/SkyUIUnofficialSDK/`（vanilla UI 反编译，用于对照）。

> ⚠️ **2026-09-22 勘误**：本节源码引用基于 5.1 的 `extern/SkyUISrc/`；运行版 SWF 实为 **SkyUI-Community v6.11**（持续维护的 fork，非「停更十余年」），逐字取证见 [skyui-version-divergence.md](./skyui-version-divergence.md)。

**① skyui 的组成（推翻「闭源 DLL」说法）**

| 部分 | 内容 | 开源 |
|------|------|------|
| 发行物 | `SkyUI_SE.esp` + `SkyUI_SE.bsa`（**无 DLL**） | — |
| BSA 内 `interface/` | 30 个 `.swf`（ActionScript 编译产物） | ✅ MIT |
| BSA 内 `scripts/` | **12 个 `.pex`**（widget / config / favorites / quest 类） | ✅ MIT（与 `extern/SkyUISrc/dist/Data/Scripts/Source/` 一一对应） |
| 引擎桥接 | **SKSE**（`skse_1_5_97.dll`，开源） | ✅ |

**关键否定**：`scripts/` 中**没有** `SKI_PlayerInventory.psc` / `SKI_ContainerMenu.psc` / `SKI_ItemCard.psc` —— skyui 的物品遍历逻辑**根本不在 Papyrus 脚本层**，故「反编译 `SKI_*.pex` 找遍历循环」这条前置**失效**。

**② 打开背包的真实数据流**（skyui 无 DLL → 引擎交互走 **GFx 委托 / SKSE 的 GFx 扩展**；**不经过 Papyrus VM**）

```
SkyUI.swf (ActionScript)
   ├─ 打开时一次：skse.ExtendData(true) ──► SKSE 的 GFx 扩展：把全量物品的扩展数据灌入 entryList
   └─ 每件物品一次：GameDelegate.call("RequestItemCardInfo", [], this, "updateItemInfo")
                        │
                        ▼
        GFx 委托（FxDelegate / Scaleform Invoke）  ← 跨 AS VM ↔ C++ 边界（参数编组）
                        ▼
        引擎 InventoryMenu 的 item card 构建（SkyrimSE.exe 内，无 PDB）
                        ▼
        InventoryEntryData::GetValue / GetWeight / GetDisplayName …（实测 GetValue ~400 ns）
                        ▼
        FxResponse 回调 updateItemInfo(obj) ──► 再跨一次边界，把嵌套 Object 交给 AS
```

**③ 逐物品跨界调用的源码级证据链（skyui 侧）**

> ⚠️ **2026-09-19 二次修正**：初次查证把跨界层归为 **Papyrus VM**（`skse.InvokeNumber` → `Form.GetGoldValue`）。读完 skyui 的 AS 源码后**该假设被证伪** —— `ItemMenus/*.as` 中**没有任何** Papyrus 调用（grep 无 `InvokeNumber` / `InvokeInt` / `ExternalInterface` / `GetGoldValue`），真正的逐物品跨界是 **GFx（ActionScript ↔ C++）委托调用**。

| # | 位置 | 代码 | 含义 |
|---|------|------|------|
| 1 | `ItemMenus/ItemMenu.as:87-88` | `skse.ExtendData(true); skse.ForceContainerCategorization(true);` | 打开菜单时由 **SKSE 的 GFx 扩展**一次性把全量物品的扩展数据灌入 `entryList` |
| 2 | `ItemMenus/InventoryMenu.as:74-76` | `itemList.addDataProcessor(new InventoryDataSetter())` + `InventoryIconSetter` + `PropertyDataExtender` | skyui 向列表注册 **3 个数据处理器** |
| 3 | `Common/skyui/components/list/BasicList.as:275-276` | `for (…) _dataProcessors[i].processList(this);` | 每次列表失效（打开 / 切分类 / 过滤）→ **跑全部处理器** |
| 4 | `ItemMenus/ItemcardDataExtender.as:44-57` | `for (var i = 0; i < entryList.length; i++) { … _requestItemInfo.apply(a_list, [this, i]); … }` | **对全部条目（27211 个）逐个处理**，且每件一次 ↓ |
| 5 | `ItemMenus/ItemcardDataExtender.as:26` | `GameDelegate.call("RequestItemCardInfo", [], a_target, "updateItemInfo")` | **每物品一次 GFx → C++ 往返**；C++ 侧构建完整 item card（含 `InventoryEntryData::GetValue`），再经 `FxResponse` 回调把嵌套 Object 传回 AS |
| 6 | `ItemMenus/InventoryDataSetter.as:24-35` | `a_entryObject.infoValue = a_itemInfo.value …` | `processEntry` 消费上一步返回的 `_itemInfo` —— **这正是它必须逐项请求的原因**（`value/weight/damage/armor/effects` 不在 `entryList` 里） |

**vanilla 对照**（`extern/SkyUIUnofficialSDK/src/common/ItemMenu.as:150-168`）：

```actionscript
function onItemHighlightChange(event) { … GameDelegate.call("RequestItemCardInfo", [], this, "UpdateItemCardInfo"); }  // 仅高亮时
function onShowItemsList(event)      { … GameDelegate.call("RequestItemCardInfo", [], this, "UpdateItemCardInfo"); }  // 仅打开时
```

且 vanilla 反编译源码中**完全没有** `processList` / `addDataProcessor` / `ExtendData`（grep 零命中）→ **vanilla 的 `RequestItemCardInfo` 是事件驱动（个位数次），skyui 是「全量物品 × 每件 1 次」** —— 这就是 0.8 s vs 2.2 s 的机制差异。

**④ 修正后的归因：1.5 s ≈ 27211 次 GFx 委托往返**

| 层 | 单次耗时 | 27211 次合计 | 依据 |
|----|---------|-------------|------|
| `InventoryEntryData::GetValue`（引擎内部，item card 的一小部分） | ~400 ns | **~11.5 ms** | 采集 #6 实测 |
| **`RequestItemCardInfo` 一次完整往返**（GFx 参数编组 + C++ 构建 item card + 回调传值） | **≈ 55 µs**（1.5 s ÷ 27211 反推） | **≈ 1.5 s** | 采集 #6 + 本源码链 |

> ⚠️ **2026-09-19 实测修正（采集 #7 / #8）**：上表两列是**反推值**，实测后应改写为：
>
> | 项 | 反推值 | **实测值** |
> |----|--------|-----------|
> | 单次耗时 | ≈ 55 µs | **70.5 µs**（同存档 8466 物件 × 3 轮，次数**严格 = 物品数 × 1**） |
> | 该项总耗时 / skyui 增量 | ≈ 1.5 s（= 全部） | **≈ 55 %**：同存档单变量对照（vanilla ~1.0 s vs skyui ~2.1 s → 增量 ≈ **+1.1 s**），其中委托 **≈ 0.6 s**，其余 **≈ 0.45 s 在 GFx 边界编组与 AS 层**；**第二轮（采集 #9 / #10，6441 件）复核为 53 %，且经靶点 2.5 实测把「引擎侧 GFx 边界」压缩到 < 0.2 %** → 剩余 **≈ 47 % 几乎全在 AS 层** |
>
> 详见 [tracy-capture-log.md](./tracy-capture-log.md) 采集 **#7 / #8** 与 [tracy-integration-plan.md](./tracy-integration-plan.md) **§4.6.2**（含口径修正：那 1.1 s 是**两段之和**，不是单一靶点的耗时）。

该解释**同时说明**：① `GetValue` 极快但整体慢 1.5 s（成本在**往返与 item card 构建**，不在取值）；② 采样全 `unknown`（`RequestItemCardInfo` 的 C++ 实现在无 PDB 的 `SkyrimSE.exe` / `skse64_*.dll` 内）；③ vanilla 只需 ~0.8 s（**调用次数少 3–4 个数量级**）。

**⑤ 对缓存落点的修正**

- ❌ ~~缓存 `GetValue` 结果~~ → 只省 400 ns；
- ❌ ~~Papyrus 原生函数 memoization（`Form.GetGoldValue` 等）~~ → **该层根本没被调用**（源码证伪）；
- ⚠️ **`RequestItemCardInfo` 的 C++ 实现**（引擎 `InventoryMenu` 的 GFx 委托）—— 一次调用 = 一次完整 item card 构建 + 两次跨边界编组，**单次成本最大**；但**实测它只占 skyui 增量的 ≈ 55 %**（见上表修正），所以把它作为「只读旁路」的落点，**天花板也只有 55 %**，另 ≈ 45 % 在边界 / AS 层 —— 那部分只能靠**批量通道**（下一条）；
  - 🔁 **第二轮修正（采集 #9 / #10，2026-09-19 23:08）**：靶点 2.5 实测证明「**引擎侧 GFx 边界 ≈ 0（< 0.2 %）**」——`Callback[…] − RequestItemCardInfo` 仅 **1.0–1.2 ms/轮**（≈ 0.17 µs/次）。故「另 ≈ 45–47 %」应读作**几乎全在 AS 层**（`processList` 循环 + 6441 次 `updateItemInfo` + 列表 UI 更新 + Scaleform 内部编组），**引擎侧无落点可优化**；
- ✅ **批量数据通道**：`skse.ExtendData(true)` 已一次性灌入全量物品的扩展数据；若把 `value` / `weight` / `damage` / `armor` / `effects` 也一并灌入，skyui 就无需逐项请求 —— **但需改 skyui 的 `processEntry`（= 改 SWF）**，属阶段 4；
- 失效点：`InventoryMenu opened` 建缓存，`SendContainerChangedEvent` / `closed` 失效（§4.6 靶点 2.4）。

---

## 8. 参考资料

### 8.1 CommonLibSSE-NG（本项目 `extern/CommonLibSSE-NG`）

| 文件 | 内容 |
|------|------|
| `include/RE/I/InventoryChanges.h` | `InventoryChanges` 定义、`IItemChangeVisitor` |
| `src/RE/I/InventoryChanges.cpp` | 各成员函数实现与 reloc |
| `include/RE/I/InventoryEntryData.h` | `InventoryEntryData` 定义 |
| `src/RE/I/InventoryEntryData.cpp` | 属性查询实现（遍历 `extraLists`） |
| `include/RE/T/TESContainer.h` | `TESContainer` / `ContainerObject` |
| `src/RE/T/TESContainer.cpp` | 静态容器增删实现 |
| `include/RE/T/TESObjectREFR.h` | `extraList`、`GetInventory*`、虚函数槽 `56/57/5A` |
| `src/RE/T/TESObjectREFR.cpp` | `GetInventory` / `GetInventoryCounts` / `GetInventoryChanges` 实现 |
| `include/RE/E/ExtraDataList.h` | `BaseExtraList`、`PresenceBitfield`、`ExtraDataList` |
| `include/RE/E/ExtraContainerChanges.h` | `ExtraContainerChanges`（挂在 `extraList` 上的载体） |
| `include/RE/B/BSTList.h` | `BSSimpleList` 单链表实现 |
| `include/RE/I/Inventory.h` | `Inventory::Event` |
| `include/RE/T/TESContainerChangedEvent.h` | 容器变更事件结构 |
| `include/RE/A/ActorEquipManager.h` / `src/RE/A/ActorEquipManager.cpp` | 装备 / 卸下入口 |
| `include/RE/Offsets.h` | reloc 常量 |

### 8.2 SkyUI / 原版 UI 源码（本项目 `extern/`）

| 路径 | 内容 | 用途 |
|------|------|------|
| `extern/SkyUI/interface/*.swf` | skyui 的 30 个 ActionScript 编译产物（BSA 提取） | 确认 UI 层组成 |
| `extern/SkyUI/scripts/*.pex` | skyui 的 **12 个** Papyrus 脚本（BSA 提取） | **证明遍历逻辑不在脚本层**（无 `SKI_PlayerInventory`） |
| `extern/SkyUISrc/src/ItemMenus/ItemcardDataExtender.as` | `processList()` 循环 + `RequestItemCardInfo` 调用 | **1.5 s 的定位证据**（§7.4 ③ 第 4/5 行） |
| `extern/SkyUISrc/src/Common/skyui/components/list/BasicList.as` | `addDataProcessor` / `InvalidateData` → 跑全部处理器 | 说明处理器何时被触发 |
| `extern/SkyUISrc/src/Common/skyui/util/Translator.as` | `translate()`（纯 AS 侧 `TextField`，不调引擎） | 排除 AS 侧翻译开销 |
| `extern/SkyUIUnofficialSDK/src/common/ItemMenu.as` | **vanilla UI 反编译**：`RequestItemCardInfo` 只在高亮 / 打开时各 1 次 | 对照 vanilla 为何只 ~0.8 s |
| `extern/SkyUICommunity_v6.11_权威源/` | 运行版 SkyUI-Community **v6.11** 权威源码（tag v6.11 浅克隆） | `processEntry` 复刻基线（[phase4-design.md](./phase4-design.md) §10.10） |

### 8.3 范式参考：SSE Engine Fixes

`extern/SSEEngineFixs/src/patches/form_caching.h` 提供了与本问题**同构**的成熟范式：

- 使用 `tbb::concurrent_hash_map` 建立缓存
- hook 写入口（`SetAt` / `RemoveAt` / `ClearData`）以保持缓存一致性
- 结论：Engine Fixes 本身**不含任何 inventory/container 性能补丁**，社区引擎级方案在此为空白 → 本优化属于填补空白。

---

## 附录 A：核心结论速记

> ⚠️ **2026-09-19 实测修正**：第 2、4、6 条被实测改写 —— 遍历并非卡顿主因（< 1 %），根因是 **skyui** 打开背包后的全量遍历伴随开销。**原「哈希索引」最优路线已废弃**，改为「定位 + 缓存」，见 §7.3 与 [tracy-capture-log.md](./tracy-capture-log.md) §7。

1. `InventoryChanges::entryList` 与 `InventoryEntryData::extraLists` **都是单链表**，且没有任何索引 → 所有查询 O(n) / O(m)。（结构描述仍准确）
2. ~~打开背包是 **O(n × m)**：外层遍历条目，内层遍历每条目的附加数据链表。~~ → **实测**：打开背包时遍历仅占 **< 1 %**；`GetValue ≈ 3 × n` **严格线性**，**无 n × m 放大**。
3. `TESContainer`（静态层）用**数组**，性能正常 —— 问题只在运行时层。
4. 写入口**分散且内联**，没有统一可 hook 的入口 → 任何旁路加速层的一致性低风险做法都是**脏标记 + 懒重建 / 失效**（对索引与快照同样适用）。
5. `ExtraDataList` 的类型判断（`HasType`）已是 **O(1)** 位图，但取指针（`GetByType`）仍需遍历。
6. ~~最优路线：保留引擎链表为真源，旁挂内存哈希索引只加速读；索引不落存档。~~ → **修正**：哈希索引已废弃；路线改为「**定位 skyui 那 ~1.5 s → 加只读旁路缓存**」，真源仍为引擎链表、缓存不落存档。
7. **skyui = esp + bsa（无 DLL）**：UI 层（Papyrus + ActionScript）MIT 开源，引擎桥接靠**开源的 SKSE**；12 个 Papyrus 脚本**不含**物品遍历逻辑，遍历在 **ActionScript** 内 —— `ItemcardDataExtender.processList()` 对全部条目**各调一次** `GameDelegate.call("RequestItemCardInfo", …)`（**GFx ↔ C++ 往返**，**不是** Papyrus VM）。**实测（采集 #7 / #8）**：单次 **70.5 µs**、次数**严格 = 物品数 × 1**（8466 × 3 = 25398），但该委托的 **C++ 实现只占 skyui 增量的 ≈ 55 %**，其余 ≈ 45 % 落在 GFx 边界编组与 AS 层 —— 见 [tracy-capture-log.md](./tracy-capture-log.md) **§8.3**（§7.4）。**第二轮实测（采集 #9 / #10，6441 件）进一步裁定**：单次 **64.4 µs**、增量 ≈ **+779 ms**，其中 **C++ 实现 ≈ 53 %**、**引擎侧 GFx 边界仅 < 0.2 %（可忽略）** → 剩余 **≈ 47 % 几乎全在 AS 层（含 Scaleform 内部编组，Tracy 不可见）**。

