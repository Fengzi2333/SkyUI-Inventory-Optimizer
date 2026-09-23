# Tracy 采集记录（阶段 1 / 阶段 2 实测归档）

> 本文档按时间顺序归档每一次 Tracy 采集的**原始条件、实测数据与结论**：阶段 1（#1–#6）为 [tracy-integration-plan.md](./tracy-integration-plan.md) §5 的验收判据（Q1–Q7）提供证据；**阶段 2（#7–#10）**为 **§4.6** 的「skyui 增量归属层定位」提供证据（最终裁定见 §4.6.4）。
> **阶段 3**（无新增采集）。**阶段 4 的步骤验收**：#11a（S0a）→ [phase4-design.md](./phase4-design.md) **§10.5**；#11b（S0b）→ **§10.7**；#12 / #13（S1 = 4a，及 4b-ii 的前置诊断）→ **§10.8**；**#14（S4 = 4b-ii）→ §10.9**；**#15（Tier 1 的测量前置）/ #16（`gather` 分支化的性能实测）/ #17（v6.11 重转写首测）→ §10.10**。
>
> 采集的操作序列见 tracy-integration-plan.md §4.4；本文只回答「这一次采到了什么、能说明什么、还缺什么」。

| 项 | 值 |
|----|-----|
| 分析工具 | `extern/TracyProfiler/release/tracy-csvexport.exe`（0.14.1 / `30997d5`，与 GUI `tracy-profiler.exe` 同源；**无需自行编译**） |
| 导出模式 | **默认**=聚合统计（`name,total_ns,total_perc,counts,mean_ns,min_ns,max_ns,std_ns`）；**`-m`**=消息时间戳（`MessageName,total_ns`） |
| | **`-u`**=逐事件（额外含 `ns_since_start,exec_time_ns,thread,value`）；**`-f <子串>`**=按 zone 名 **子串** 过滤（⚠️ **不是前缀**：源码 `csvexport.cpp:396 is_substring()`；默认大小写**不敏感**，加 `-c` 才敏感）→ 故 `-f RequestItemCardInfo` 会**同时**命中 `RequestItemCardInfo` 与 `FxDelegate::Callback[RequestItemCardInfo]`，需再按 `name` 分组 |
| 工具链要点 | `.tracy` 是**二进制**，必须先经 `tracy-csvexport` 转 CSV 才能脚本化读取（否则只能用 GUI 手工点） |
| ⚠️ 聚合模式陷阱 | 默认导出的 `total_perc` 分母是**整个 session 总时长**，**不是**「背包打开时长」→ **Q1 占比不能直接看 `total_perc`**，必须用 `-m` 的时间戳自己切区间再算 |

---

## 采集 #1 —— 2026-09-19 10:23（⚠️ 仅 #1 生效，数据不完整）

### 1.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_19_10_23.tracy` |
| 文件大小 | 448,572,699 B（≈427.8 MB） |
| 文件有效性 | 文件头 magic = `74 72`（ASCII `"tr"`）→ 有效 trace |
| 场景设计 | 背包规模 **0 / 50 / 500 / 2000 / 极大量**，各连续开关物品栏 3 次（共 5 × 3 = 15 次） |
| 实际开关次数 | **21 次**（`InventoryMenu opened/closed` 各 21 条消息）→ 与设计不一致，**按时间大间隔切分为 5 段**，不能假设固定轮数 |
| **实际加载的 DLL** | `C:\Mod Organizer 2\mods\ModTest\SKSE\Plugins\Template.dll` = **714,240 B @ 2026-09-19 09:29:34** |
| 该 DLL 含有的 hook | **仅 #1 `GetItemCount`** |
| 当时已编译但**未部署**的 DLL | `build/bin/RelWithDebInfo/Template.dll` = **721,408 B @ 2026-09-19 09:57:58**（含 #1–#8 共 **7** 个 zone） |

> ⚠️ **本轮数据的关键限制**：采集发生在 **10:23**，而含 #2–#8 的新 DLL 于 **09:57** 才编译完成，且**从未复制到 MO2 的 mod 目录**。因此本轮 trace 中**只有 #1 一个 zone**（Tracy 的 source location 是 zone **首次执行时**才注册；未触发的 zone 不会出现在 trace 里，聚合导出也不会列出）。

### 1.2 一句话结论

本轮**不能**回答 Q1/Q2（缺 #2 `VisitInventory` / #3 `GetInventoryWeight` / #5 `GetValue`），但产出了两类**仍然有效**的数据：

1. **21 次开关的 wall time** —— 可量化「背包规模 → 耗时」的增长与抖动（§1.3）；
2. **`GetItemCount` 的调用分布** —— 意外揭示它**不是**打开背包的开销（§1.4、§1.5）。

同时暴露并定位了一个**部署环节的坑**（§1.6），以及沉淀了可直接复用的**分析工具链**（§1.7）。

---

### 1.3 有效数据 A：背包开关 wall time

**切分依据**：`InventoryMenu opened/closed` 的 21 对时间戳，取相邻「closed → 下一次 opened」间隔中 **> 10 s** 的 gap 作为场景边界 → 恰好得到 **5 段（3 / 3 / 4 / 5 / 6 次）**，与 5 个设计场景一一对应。

| 段 | 场景 | 区间序号 | 各次 wall time (ms) | 时段（session 内） | 中位数 (ms) | 最大 (ms) |
|----|------|---------|---------------------|-------------------|-------------|-----------|
| A | 0 物品 | 1–3 | 952 / 694 / 659 | t ≈ 971.5–975.0 s | 694 | 952 |
| B | 50 物品 | 4–6 | 781 / 697 / 638 | t ≈ 1132.7–1136.3 s | 697 | 781 |
| C | 500 物品 | 7–10 | 1160 / 950 / 688 / 765 | t ≈ 1174.4–1303.0 s | 857.5 | 1160 |
| D | 2000 物品 | 11–15 | 1552 / **7052** / 2199 / 2294 / 4257 | t ≈ 1386.8–1431.8 s | 2294 | 7052 |
| E | 极大量 | 16–21 | 1496 / 1290 / 1345 / **5322 / 3860 / 3751** | t ≈ 1459.9–1635.5 s | 2623.5 | 5322 |

**读法与观察**：

- **A / B（0、50 物品）**：0.64–0.95 s，且**逐次下降**（首轮 952 / 781 最高，后续滑到 659 / 638）→ 典型的 **cache 冷启动**（对应 tracy-integration-plan.md §4.5「测量有效性检查」：应取稳定后的轮次作结论，**并单独记录首轮值**，因为玩家感知到的正是首轮）。
- **C（500 物品）**：0.69–1.16 s，开始出现可见抖动。
- **D（2000 物品）**：1.55–7.05 s，**单次尖峰 7.05 s**（区间 12），中位 2.29 s。
- **E（极大量）**：1.29–5.32 s，中位 2.62 s，其中 **3 次 > 3.7 s**。

**量化对比**：A/B 的组内极差 **< 0.3 s**；D/E 的组内极差 **> 4 s**。即 **`n` 增大不只是抬高中位数，更是把「抖动/尖峰」放大了十几倍**——这正是链表遍历（指针追逐 → cache miss → 偶发 GC/分配）的指纹，与 [inventory-system-analysis.md](./inventory-system-analysis.md) §5.2 的假设方向一致。

> ⚠️ **口径提醒**：wall time 是 `opened → closed` 的**完整菜单打开时长**，包含 UI 布局、贴图/字型、脚本、音频等一切开销，**不等于** `entryList` 遍历耗时。真正的 Q1 占比需要 #2/#3 的 total 来做分子。

### 1.4 有效数据 B：`GetItemCount` 调用分布

**全局统计**（聚合导出）：`counts = 3343`，`total = 16.7 ms`，`mean ≈ 5.0 µs`，`max = 160 µs`。

**按 §1.3 的区间切分**：

| 段 | 场景 | 区间内调用次数 | 明细（区间号: 次数 [线程]） |
|----|------|---------------|------------------------------|
| A | 0 物品 | **0** | — |
| B | 50 物品 | **0** | — |
| C | 500 物品 | 1 | `7`: 1 [T1] |
| D | 2000 物品 | 4 | `11`: 1 [T1]；`13`: 1 [T1]；`14`: 1 [T1]；`15`: 1 [T1] |
| E | 极大量 | 279 | `16`: 1 [T1, 33 µs]；`19`: 140 [T1=120, **T45=20**]；`20`: 72 [T1=52, **T49=20**]；`21`: 66 [T1=46, **T49=20**] |
| **区间内合计** | — | **284** | 占全部调用的 **8.5 %** |
| **区间外**（背包未打开 / 后台） | — | **3059** | 占 **91.5 %** |

> 线程号 `T1` = 主线程；`T45` / `T49` = 任务线程（**多线程**现象，与 Q4 相关但样本太少）。

### 1.5 关键发现：`GetItemCount` 不是「打开背包」的开销

**91.5 % 的 `GetItemCount` 调用发生在背包未打开的时段**；而 21 个「打开背包」区间里，前 18 个（0 / 50 / 500 / 2000 全部）几乎为零。

推论：

1. `GetItemCount` 主要由**脚本 / 后台逻辑**驱动（物品变更回调、周期性刷新之类），**不是**背包 UI 渲染路径的必需调用。
2. 所以即便 `GetItemCount` 是 O(n) 链表遍历，它在「打开背包卡顿」里的占比**很可能很小** → **Q1（遍历占比）的答案不能靠 #1 单独得出**，必须用 #2 `VisitInventory` / #3 `GetInventoryWeight` / #5 `GetValue` 的 total。
3. E 段（极大量）最后 3 次开关突然出现 66–140 次调用，且**伴随任务线程（T45 / T49）** → 提示超大背包下**确有额外逻辑被触发**（疑似某 mod 的库存统计），值得在采集 #2 中重点观察。

> 这也回过头印证了 tracy-integration-plan.md §4.2 把 #2 / #3 标为 ★★★ 的理由：它们才是**打开背包路径**的热点，#1 不是。

### 1.6 失败原因复盘：新 DLL 未部署

| 环节 | 事实 |
|------|------|
| 编译 | `2026-09-19 09:57:58` 产出 `build/bin/RelWithDebInfo/Template.dll` = **721,408 B**，含 #1–#8 共 **7** 个 zone（7 个 zone 字符串已确认进入 DLL） |
| 部署 | ❗ **未执行** —— `C:\Mod Organizer 2\mods\ModTest\SKSE\Plugins\Template.dll` 仍是 `09:29:34` 的 **714,240 B** 旧版本 |
| 采集 | `10:23` 启动游戏 → 加载旧 DLL → trace 中只有 #1 |

**为什么会漏**：

1. 项目**没有自动部署步骤**：tracy-integration-plan.md §4.4 只写了「安装/复制到游戏 `Data/SKSE/Plugins/`」，**没有脚本**；
2. 本机游戏**不是**直接用 `D:\Steam\steamapps\common\Skyrim Special Edition`，而是经 **MO2（`base_directory = C:/Mod Organizer 2`）的 VFS** 加载 → 复制目标应是 **`C:\Mod Organizer 2\mods\ModTest\SKSE\Plugins\`**，而不是游戏目录下的 `Data\SKSE\Plugins`（该目录**根本不存在**，可直接作为「走 MO2」的判据）；
3. 缺少「采集前的版本校验」习惯 → 采完一整轮才发现。

**对策（流程性，不改代码）**：

- 编译后先比对 **DLL 字节数**（旧 `714,240` / 新 `721,408`）或 `LastWriteTime`，再复制；
- 复制**之后**重新 `Get-Item` 确认目标路径时间戳已更新；
- 采集后**第一时间**跑一次聚合导出：zone 行数应为 **7**（7 个 `Inventory*` 名字）；若只出现 1 行，立即停手排查，别浪费场景轮次。

### 1.7 分析工具链（已沉淀，可复用）

```powershell
$tool  = "extern\TracyProfiler\release\tracy-csvexport.exe"
$trace = "TracyLog\log_2026_09_19_10_23.tracy"

# ① 聚合统计：各 zone 的 count / total / mean / max
#    —— 先跑这个，确认插桩是否真的生效（应有 7 个 Inventory* 行）
& $tool $trace

# ② 消息时间戳：取 InventoryMenu opened/closed 的 ns_since_start，用于切区间
& $tool -m $trace

# ③ 逐事件（必须按 zone 名分片，避免百万行）
& $tool -u -f "InventoryChanges::"    $trace > "$env:TEMP\ev_inv.csv"
& $tool -u -f "InventoryEntryData::"  $trace > "$env:TEMP\ev_entry.csv"
```

| 模式 | 输出列 |
|------|--------|
| 默认（聚合） | `name, src_file, src_line, total_ns, total_perc, counts, mean_ns, min_ns, max_ns, std_ns` |
| `-m`（消息） | `MessageName, total_ns` |
| `-u`（逐事件） | `name, src_file, src_line, ns_since_start, exec_time_ns, thread, value` |

**逐事件流的用法**：第 4 列 `ns_since_start` 落区间、第 5 列 `exec_time_ns` 累加耗时、第 6 列 `thread` 画线程泳道。

> ⚠️ `-u` 全量导出在「极大量」场景下会有几十万 ~ 百万行（`GetValue` / `GetItemCount`），**务必 `-f` 分片**，否则单文件过大、PowerShell 处理吃紧。
> ℹ️ `.tracy` 体积（427 MB）**主要来自 CPU 采样数据**，zone 事件只占很小一部分——所以「文件很大」不代表「zone 很多」。

### 1.8 采集 #2 的前提（checklist）

| # | 动作 | 通过标准 |
|---|------|---------|
| 1 | 复制 `build/bin/RelWithDebInfo/Template.dll`（721,408 B）→ `C:\Mod Organizer 2\mods\ModTest\SKSE\Plugins\Template.dll` | 目标文件字节数 = **721,408** |
| 2 | 复核目标文件 `LastWriteTime` | 晚于 `2026-09-19 09:57:58` |
| 3 | 按同样 5 场景（0 / 50 / 500 / 2000 / 极大量）× 3 次采集 | — |
| 4 | **先**跑聚合导出，确认出现 **7 个** zone 行 | 7 行 |
| 5 | **再**做区间切分（`-m`）+ 逐事件分片（`-u -f`） | 见 §1.7 |

### 1.9 本轮数据对验收判据（plan §5）的贡献

| 问题 | 本轮能否回答 | 说明 |
|------|-------------|------|
| Q1 遍历占比 | ❌ 不能 | 缺 #2 / #3 的 total |
| Q2 `n × m` 放大 | ❌ 不能 | 缺 #5 `GetValue` 的 count |
| Q3 多次全量刷新 | ❌ 不能 | 缺 #4（且 #4 有意未插桩，plan §4.2） |
| Q4 卡顿线程 | ⚠️ 弱旁证 | `GetItemCount` 出现 `T1` + `T45`/`T49`，但样本仅 284 次 |
| Q5 遍历 vs 事件风暴 | ❌ 不能 | 缺 #1 total 对 #4 count |
| Q6 规模-耗时关系 | ✅ **初步可答** | §1.3 的 wall time 曲线 |
| Q7 `std::map` 拷贝 | ❌ 不能 | 无 `GetInventory` 调用点 |

> **净结论**：本轮仅 **Q6 给出初步数据**；Q1–Q5、Q7 必须等采集 #2（部署含 #1–#8 的 DLL）。

---

## 采集 #2 —— 2026-09-19 12:42（7 靶点全部生效；5 场景 × 3 次）

> 首次使用含 #1–#8 的 DLL。本轮加载了**完整 mod 集**，作为后续无 mod 对照（采集 #3）的基线。

### 2.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_19_12_42.tracy` |
| 文件大小 | 186,768,236 B（≈178 MB） |
| 场景设计 | 新存档 0 / 50 / 500 / 2000 物品 + 游戏后期存档「极大量」，各开关 **3** 次 |
| 实际开关次数 | **15 次**（5 × 3，与设计一致） |
| 加载的 DLL | `build/bin/RelWithDebInfo/Template.dll` = **721,408 B**（含 #1–#8） |
| **生效的 zone** | **5 个**：#1 `GetItemCount`、#2 `VisitInventory`、#3 `GetInventoryWeight`、#5 `GetValue`、#7 `GetWornMask` |
| **零事件 zone** | #6 `GetArmorInSlot`、#8 `GetEnchantment`（二者本就不在「打开背包」路径上） |

### 2.2 wall time（opened → closed）

| 场景 | 3 轮 wall (ms) | 中位 |
|------|---------------|------|
| 0 物品 | 1091 / 1133 / 884 | 1091 |
| 50 物品 | 2055 / 1097 / 938 | 1097 |
| 500 物品 | 873 / 847 / 812 | 847 |
| 2000 物品 | 1108 / 947 / 1168 | 1108 |
| **极大量** | **4452 / 3744 / 4128** | **4128** |

- 0 → 2000 物品基本持平（~0.85–1.1 s，受「打开菜单」固定开销主导）；
- **「极大量」跳到 ~4 s**，是唯一显著增长段；
- 50 物品首轮 2055 ms 为冷启动异常值。

### 2.3 决定性发现：遍历函数「几乎不在打开背包时调用」

| zone | 总次数 | 落在 opened→closed 区间内 | 占比 |
|------|-------|---------------------------|------|
| `VisitInventory` (#2) | 6852 | **0** | **0 %** |
| `GetInventoryWeight` (#3) | 975 | **0** | **0 %** |
| `GetItemCount` (#1) | 2766 | 319 | 11.5 % |
| `GetWornMask` (#7) | 90 | 3 | 3.3 % |
| `GetValue` (#5) | 76899 | 59487 | 77.4 % |

- `VisitInventory` 6852 次**几乎全部**集中在 940–990 s —— 那是「2000 物品新存档 → 后期存档」的**加载切换期**，不是打开背包；
- `GetInventoryWeight` 100 % 在打开背包之外；
- 唯一在打开背包区间内大量调用的是 `GetValue`。

### 2.4 `GetValue` 随 `n` 线性增长（每次打开）

| 场景 | `GetValue` count | sum (µs) |
|------|-----------------|---------|
| 0 物品 | 1 | 2 |
| 50 物品 | ~154 | ~50 |
| 500 物品 | ~1435 | ~500 |
| 2000 物品 | ~5821 | ~2000 |
| 极大量 | ~12417 | ~7000 |

比值稳定在 **≈ 3 × n** → **线性，无 `n × m` 二次放大**。

### 2.5 遍历占 wall time 的比例

「极大量」（wall ≈ 4452 ms）：`GetValue` ~7.0 ms + `GetItemCount` ~0.8 ms + 其余 ~0.25 ms ≈ **8 ms → 占比 ≈ 0.2 %**。

### 2.6 Q1–Q7 裁定（本轮）

| 问题 | 结论 |
|------|------|
| Q1 遍历占比 | **≈ 0.2 %** |
| Q2 `n × m` 放大 | 原判据失效（`GetItemCount` 与 `GetValue` 不同源）；实测 `GetValue ≈ 3×n`，**线性** |
| Q3 多次全量刷新 | 缺 #4，不能答；旁证：打开背包期间 `VisitInventory` 0 次 |
| Q4 卡顿线程 | `GetValue` 100 %（T1 主线程）；少量 `GetItemCount` 来自任务线程 |
| Q5 遍历 vs 事件风暴 | 缺 #4；遍历仅 0.2 %，**必非主导** |
| Q6 规模-耗时 | 0→2000 持平，极大量 +3 s |
| Q7 `std::map` 拷贝 | 无调用点，不能答 |

> ⚠️ 本轮加载完整 mod 集，**无法区分**「引擎开销」与「mod 开销」→ 必须做无 mod 对照（采集 #3）。

---

## 采集 #3 —— 2026-09-19 14:19（★ 无 mod 对照；新存档「极大量」× 3）

> **阶段 1 的转折点。** 首个无 mod 数据，直接推翻「遍历是瓶颈」的假设，并把矛头指向 mod。

### 3.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_19_14_19.tracy` |
| 文件大小 | 50,829,297 B（≈ 50.8 MB） |
| 场景 | 全新存档 + 「极大量」物品，开关 **3** 次 |
| mod 集 | **无**（仅 SKSE + Address Library + 本插件） |

### 3.2 wall time（opened → closed）

| 轮次 | wall (ms) |
|------|-----------|
| 1 | 1028 |
| 2 | 664 |
| 3 | 751 |

对比采集 #2 的「极大量」**4452 / 3744 / 4128 ms** → **无 mod 时卡顿几乎消失（~0.7 s vs ~4 s）**。

### 3.3 决定性发现

| zone | 总次数 | 区间内 | 说明 |
|------|-------|--------|------|
| `GetValue` (#5) | 35652 | **0** | 全部发生在 opened **之前**的预处理，每次打开前爆发 **11884 次** |
| `GetItemCount` (#1) | 611 | 13–19/次 | 实际是 **~18 次/秒的周期性后台任务**，与打开背包无因果 |
| `GetInventoryWeight` (#3) | 737 | **0** | — |
| `VisitInventory` (#2) / `GetWornMask` (#7) | 0 | — | vanilla 打开背包不触发 |

- `GetItemCount` 全局 mean 从采集 #2 的 4.6 µs 涨到 **224.8 µs** → 证明它确实是 **O(n) 链表遍历**（这次真的在遍历极大量 entryList），但**调用频次低、总占比小**。
- 三次 `GetValue` 爆发各 **11884 次**（完全一致）→ vanilla 的「打开前物品价值预处理」是确定性的。

### 3.4 遍历占 wall time 的比例

「极大量」单轮（wall ≈ 800 ms）：`GetItemCount` ~3 ms + `GetValue` 0 + `GetInventoryWeight` 0 ≈ **3 ms → ≈ 0.4 %**。

### 3.5 结论

1. **4 秒卡顿不是引擎 / 遍历造成的** —— vanilla 打开极大量背包只要 ~0.8 s；
2. **遍历从来不是瓶颈**（< 1 %）→ 文档 §5.2「遍历导致卡顿」的假设**被实测否定**；
3. 卡顿必须由 mod 解释 → 进入 mod 二分（采集 #4）。

---

## 采集 #4 —— 2026-09-19 14:39（+ skyui + UI Extensions + Infinity UI）

> ⚠️ 本轮实际加载了 **3 个** UI mod（skyui、UI Extensions、**Infinity UI**）——最初误记为 2 个，由采集 #5 / #6 补齐。

### 4.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_19_14_39.tracy` |
| 文件大小 | 44,943,057 B（≈ 44.9 MB） |
| 场景 | 全新存档 + 「极大量」物品，开关 **3** 次 |
| mod 集 | skyui + UI Extensions + Infinity UI |

### 4.2 wall time（opened → closed）

| 轮次 | wall (ms) |
|------|-----------|
| 1 | 5078 |
| 2 | 2313 |
| 3 | 2718 |

→ 相对采集 #3（vanilla ~0.8 s）**卡顿回归**：稳态 ~2.5 s，首轮 5.1 s（含初始化）。

### 4.3 关键发现：`GetValue` 被精确拆成两段

`GetValue` 总次数 117285，**正好等于**：

```
117285 = 35652（vanilla 打开前预处理）  +  81633（mod 打开期间额外遍历）
              ↑ 与采集 #3 完全一致              ↑ 27211 次/次 × 3
```

| 维度 | 采集 #3（vanilla） | 采集 #4（+3 mod） |
|------|-------------------|------------------|
| `GetValue` 区间内/次 | **0** | **27211**（三次一模一样） |
| `GetValue` 打开前预处理 | 11884 次/次 | 11884 次/次（不变） |
| `GetValue` 区间内总耗时 | 0 | ~11.6 ms/次 |

→ 某个 mod 在「打开背包」期间对全部物品做了一次**确定性遍历**（27211 次 `GetValue`），这是 vanilla 没有的行为。

### 4.4 注意：卡顿不在 `GetValue` 里

`GetValue` 区间内仅 ~11.6 ms，而 wall time 涨了 1.5–4.3 s → **卡顿在上述 mod 遍历物品时伴随的其它操作**（Papyrus / GFx / 排序 / 分配）；`GetValue` 只是「正在遍历」的**计数器**。

---

## 采集 #5 —— 2026-09-19 14:52（− Infinity UI，即 skyui + UI Extensions）

> 用于剥离 Infinity UI 的影响。

### 5.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_19_14_52.tracy` |
| 文件大小 | 43,926,703 B（≈ 43.9 MB） |
| mod 集 | skyui + UI Extensions（**移除 Infinity UI**） |

### 5.2 wall time 对比

| 轮次 | 采集 #4（含 Infinity UI） | 采集 #5（无 Infinity UI） | 差值 |
|------|--------------------------|--------------------------|------|
| 1 | 5078 ms | **2960 ms** | **−2118 ms** |
| 2 | 2313 ms | **2206 ms** | −107 ms |
| 3 | 2718 ms | **2298 ms** | −420 ms |

### 5.3 关键发现：`GetValue` 完全没变

移除 Infinity UI 后 `GetValue` 仍是 **117285**（区间内 **27211 × 3**）→ 那 27211 次/次的「打开后全量遍历」**与 Infinity UI 无关**。

### 5.4 结论

- **Infinity UI 只在首轮贡献 ~2.1 s 初始化**，稳态轮（2 / 3）几乎无影响；
- 稳态 ~2.3 s 卡顿来自 **skyui / UI Extensions**。

---

## 采集 #6 —— 2026-09-19 15:04 / 15:07（skyui 与 UI Extensions 单独测试）

> **决定性实验。** 把两个 mod 分开测，直接锁定元凶。

### 6.1 采集条件

| trace 文件 | 大小 | mod 集 |
|-----------|------|--------|
| `clean_save_many_items_uiExtensions_only.tracy` | 34,342,156 B（≈ 34.3 MB） | **仅 UI Extensions** |
| `clean_save_many_items_skyui_only.tracy` | 37,240,603 B（≈ 37.2 MB） | **仅 skyui** |

场景均为全新存档 + 「极大量」物品 × **3** 次。

### 6.2 结果对比

| 实验 | `GetValue` 总次数 | 区间内 `GetValue`/次 | wall time（3 轮） |
|------|------------------|---------------------|-------------------|
| vanilla（#3） | 35652 | **0** | 1028 / 664 / 751 ms |
| **仅 UI Extensions** | **35652** | **0** | **1063 / 758 / 1097 ms** |
| **仅 skyui** | **117285** | **27211** | **2236 / 2133 / 4004 ms** |
| skyui + UI Ext（#5） | 117285 | 27211 | 2960 / 2206 / 2298 ms |
| skyui + UI Ext + Infinity UI（#4） | 117285 | 27211 | 5078 / 2313 / 2718 ms |

### 6.3 结论

1. **UI Extensions 完全无辜**：
   - `GetValue` 35652 次（与 vanilla 一字不差），区间内 **0 次**——根本没有「打开背包后遍历」；
   - wall time 1063 / 758 / 1097 ms ≈ vanilla 的 1028 / 664 / 751 ms，**差异在噪声范围内**。
2. **skyui 是唯一元凶**：
   - `GetValue` 117285 次，区间内 **27211 × 3** 次，三次一模一样；
   - wall time 稳定 ~2.2 s（vs vanilla ~0.7 s），额外 **+~1.5 s**。
3. `GetValue` 区间内仅 ~11.5 ms → skyui 的 ~1.5 s 卡顿在其「遍历 27211 次物品」时**伴随的其它操作**里（Papyrus / GFx / 排序 / 分配）。
4. 第 3 轮 4004 ms 为波动 / GC 异常（`GetValue` 仍是 27211 次、sum 仅 ~11 ms）。

---

## 阶段 1 汇总结论（Q1–Q7 终裁）

> 汇总采集 #1–#6，替代 §1.9 的初判。

### 7.1 五组对照矩阵

| 实验 | wall time（极大量，3 轮） | `GetValue` 总数 | 区间内 `GetValue`/次 | 裁定 |
|------|--------------------------|----------------|---------------------|------|
| vanilla | 1028 / 664 / 751 | 35652 | 0 | 基线 |
| + UI Extensions | 1063 / 758 / 1097 | 35652 | 0 | **无影响** |
| **+ skyui** | **2236 / 2133 / 4004** | **117285** | **27211** | **唯一主因** |
| + skyui + UI Ext | 2960 / 2206 / 2298 | 117285 | 27211 | 叠加无额外影响 |
| + skyui + UI Ext + Infinity UI | 5078 / 2313 / 2718 | 117285 | 27211 | 仅首轮 +2.1 s 初始化 |

### 7.2 Q1–Q7 终裁

| 问题 | 终裁 |
|------|------|
| **Q1 遍历占比** | **< 1 %**（vanilla ≈ 0.4 %；有 mod ≈ 0.4 %）→ **阶段 2a 哈希索引收益 ≈ 0，不应优先做** |
| Q2 `n × m` 放大 | **不存在**：`GetValue ≈ 3 × n`，严格线性 |
| Q3 多次全量刷新 | 未直接观测（#4 zone 未落地）；间接证据：卡顿主体在非遍历路径 |
| Q4 卡顿线程 | 主线程（T1 / T11 / T15，随启动顺序变化）；`GetValue` 与该 wall time 同线程 |
| Q5 遍历 vs 事件风暴 | 均非主导；主导是 **skyui 的打开后全量遍历伴随操作** |
| Q6 规模-耗时 | 0→2000 物品持平（~0.85–1.1 s）；**「极大量」跳变由 skyui 造成** |
| **Q7 `std::map` 拷贝** | 未直接观测（`GetInventory` 无插桩点），仍为**首号待验证嫌疑** |

### 7.3 根因修正（推翻 §5.2 / §1.2 原假设）

- 原假设「遍历（`entryList` 链表 + `extraLists`）导致卡顿」→ **实测否定**（占比 < 1 %）；
- 实测根因：**skyui 在 `InventoryMenu` 打开后对全部物品做一次确定性遍历（27211 次 `GetValue`）**，真正耗时在其伴随的 Papyrus / GFx / 排序操作；
- 影响：**阶段 2a（哈希索引）收益几乎为零**；优化重心应转向「减少 skyui 打开后遍历的伴随开销」或「给 skyui 提供旁路缓存」。

### 7.4 下一步（定位 skyui ~1.5 s 的具体去向）

按嫌疑排序插桩（均为无 reloc 风险的调用点）：

1. **`RequestItemCardInfo` 的 C++ 委托实现** —— **已由源码定位（§7.6 F）**：skyui 的 `ItemcardDataExtender.processList()` 对**全部 27211 个条目各调一次** `GameDelegate.call("RequestItemCardInfo", [], this, "updateItemInfo")`（GFx → C++ 往返，C++ 侧构建完整 item card）；由 1.5 s ÷ 27211 反推 **≈ 55 µs / 次**，而其中的 `GetValue` 仅 ~400 ns → **1.5 s 的主体就是这 27211 次往返**。**待实测确认**（调用次数 + 单次耗时）；
2. **`TESObjectREFR::GetInventory`** —— Q7 靶点，`std::map` 深拷贝；
3. **GFx / UI 对象创建与条目标签填充** —— skyui 的 item card 与排序。

### 7.5 符号解析限制（采样 `unknown` 根因）

- GUI Sampling 面板中「极大量」那 4 秒的占位**几乎全部显示 `unknown`**；
- 根因：采样帧落在 `SkyrimSE.exe` 引擎代码，Bethesda **不发布 PDB** → Tracy 的 dbghelp 解析失败（连模块名都拿不到）；
- 这是「瓶颈在引擎 / mod 代码、不在 7 个插桩函数内」的**排除性证据**；
- 两条攻坚路线：① 自定义 addr2line（内部用 Address Library 把 `SkyrimSE.exe+偏移` 映射函数名）+ `tracy-update.exe -r -a` 离线重解析现有 trace（无需重采）；② 定向插桩（推荐，快）。

---

### 7.6 skyui 架构查证（2026-09-19 补充）—— 推翻「闭源 DLL」，并把 1.5 s 定位到 GFx 委托往返

> ⚠️ 本轮修正了 §7.3 中「skyui 的 C++ DLL 闭源」的说法 —— **skyui 没有 DLL**。
> ⚠️ **二次修正（同日）**：下面 **D 段的「Papyrus VM 边界」假设已被 AS 源码证伪** —— 真正的跨界层是 **GFx 委托往返**（`RequestItemCardInfo`）。修正后的完整证据链见 **F 段**。

**A. 发行物与源码一一对应（本地）**

| 本地路径 | 内容 | 性质 |
|----------|------|------|
| `extern/SkyUI/interface/*.swf` | 30 个 ActionScript 编译产物 | skyui 发行物（BSA 提取） |
| `extern/SkyUI/scripts/*.pex` | **12 个** Papyrus 脚本 | skyui 发行物（BSA 提取） |
| `extern/SkyUISrc/dist/Data/Scripts/Source/*.psc` | 与上列 12 个 `.pex` **一一对应** | GitHub `schlangster/skyui`（MIT，停更） |
| `extern/SkyUISrc/src/ItemMenus/*.as` | `InventoryMenu.as` / `InventoryLists.as` / `InventoryDataSetter.as` … | skyui 的 ActionScript 源码 |
| `extern/SkyUIUnofficialSDK/src/**/*.fla`·`.as` | **vanilla（原版）UI 的反编译**（VERSION `1.8.151.0`） | 供对照 vanilla 为何只 ~0.8 s |

**B. 关键否定**：12 个 `.pex` 中**没有** `SKI_PlayerInventory` / `SKI_ContainerMenu` / `SKI_ItemCard` → **物品遍历逻辑不在 Papyrus 脚本层**，此前「反编译 `SKI_*.pex` 找循环」的前置失效。

**C. ~~数据流~~（Papyrus 路径 —— 已证伪，保留以记录推理过程）**

```
~~SkyUI.swf --逐项--> GFx skse.InvokeNumber --> SKSE Papyrus VM --> Form.GetGoldValue --> GetValue~~
（❌ 此路径已被 F 段证伪：skyui 的 AS 层不走 Papyrus VM）
```

**D. ~~首要假设~~（已证伪，保留以记录推理过程）**

> ⚠️ **该假设已被 AS 源码证伪**（见 **F 段**）：`ItemMenus/*.as` 中无任何 Papyrus 调用，因此「27211 次跨 Papyrus VM 边界开销」不成立。正确的跨界层是 **GFx 委托 `RequestItemCardInfo`**（每物品一次）——验证点相应改为 §4.6 靶点 **2.1**（该委托的 C++ 实现）。

**E. 佐证**（`extern/SkyUISrc/src/ItemMenus/InventoryDataSetter.as:24-35`）：`processEntry()` 消费 `a_itemInfo.value` / `.weight` / `.armor` / `.damage` / `.effects`（**纯客户端**算术与 `Translator.translate`），**不直接调引擎** → 证实这些字段是**逐项请回来的**（见 F），而不是 `entryList` 里本来就有的。

**F. 二次修正：真正的跨界层是 GFx，不是 Papyrus VM**（AS 源码证据链）

| # | 位置 | 代码 | 含义 |
|---|------|------|------|
| 1 | `ItemMenus/ItemMenu.as:87-88` | `skse.ExtendData(true); skse.ForceContainerCategorization(true);` | 打开时由 **SKSE 的 GFx 扩展**一次性把全量物品的扩展数据灌入 `entryList` |
| 2 | `ItemMenus/InventoryMenu.as:74-76` | `itemList.addDataProcessor(new InventoryDataSetter())` + `InventoryIconSetter` + `PropertyDataExtender` | skyui 向列表注册 **3 个数据处理器** |
| 3 | `Common/skyui/components/list/BasicList.as:275-276` | `for (…) _dataProcessors[i].processList(this);` | 每次 `InvalidateData()` → 跑全部处理器 |
| 4 | `ItemMenus/ItemcardDataExtender.as:44-57` | `for (var i = 0; i < entryList.length; i++) { … _requestItemInfo.apply(a_list, [this, i]); … }` | **对全部条目（27211 个）逐个处理**，每件一次 ↓ |
| 5 | `ItemMenus/ItemcardDataExtender.as:26` | `GameDelegate.call("RequestItemCardInfo", [], a_target, "updateItemInfo")` | **每物品一次 GFx → C++ 往返 + 回调**（C++ 侧构建完整 item card，内含 `InventoryEntryData::GetValue`） |

**vanilla 对照**（`extern/SkyUIUnofficialSDK/src/common/ItemMenu.as:150-168`）：`RequestItemCardInfo` 只在 `onItemHighlightChange`（高亮）与 `onShowItemsList`（打开）里各调 **1 次**；且 vanilla 反编译源码中**无** `processList` / `addDataProcessor` / `ExtendData`（grep 零命中）→ **调用次数差 3–4 个数量级**，这正是 **0.8 s vs 2.2 s** 的机制差异。

**Grep 证伪**：`ItemMenus/*.as` 中**无** `InvokeNumber` / `InvokeInt` / `ExternalInterface` / `GetGoldValue` —— **skyui 的 UI 层根本不经过 Papyrus VM**。

**H. 结论（修正后）**

| 问题 | 结论 |
|------|------|
| 遍历在哪一层 | **ActionScript**（`ItemcardDataExtender.processList()`）；不在 Papyrus 脚本层，也不在引擎的 `entryList` 遍历里（后者实测 < 1 %） |
| 每物品的跨界调用是什么 | `GameDelegate.call("RequestItemCardInfo", …)` = **GFx（AS ↔ C++）委托往返** + `FxResponse` 回调，**不是 Papyrus VM** |
| 为什么恰好 27211 次 | 列表装载的是全部实例化物品（`entryList.length` = 物品数），`processList` 对**未被过滤的每一件**各处理 1 次 |
| 为什么 vanilla 只 ~0.8 s | vanilla 无 `processList` / `addDataProcessor` / `ExtendData`；`RequestItemCardInfo` 只在高亮 / 打开时各 1 次（**差 3–4 个数量级**） |
| 1.5 s 的量级是否自洽 | 1.5 s ÷ 27211 ≈ **55 µs / 次**，而其中的 `GetValue` 仅 ~0.4 µs → 成本在**往返编组 + item card 构建**，与「遍历只占 < 1 %」的实测一致 |
| 下一步（阶段 2 实测） | hook **`RequestItemCardInfo` 的 C++ 实现**并包 zone，核对「次数 ≈ 27211 × 轮数、单次 ≈ 55 µs」→ 决定阶段 3a′（降单次成本）还是阶段 4（批量通道 + 改 SWF） |

---

## 阶段 2 采集（定位 skyui 增量的归属层）

> 阶段 1（§7）已把卡顿根因锁定为 **skyui**，并把「伴随操作」定位到 GFx 委托 `RequestItemCardInfo`（每物品一次）。阶段 2 的任务是**实测**它：调用次数是否 = 物品数、单次是否 ≈ 55 µs、以及它的 **C++ 实现**能否解释 skyui 的全部增量。本节归档阶段 2 的两次采集与裁定。

### 采集 #7 —— 2026-09-19 21:27（skyui + 靶点 2.1 首次生效；3 轮）

#### 采集 #7.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_19_21_27.tracy` |
| 文件大小 | 37.7 MB |
| 文件有效性 | 文件头 magic = `74 72`（ASCII `"tr"`）→ 有效 trace |
| 场景 | 「极大物品」存档（qasmoke 全部箱子搬空；极少数道具为复数堆叠），连续开关物品栏 **3 次** |
| **实际加载的 DLL** | `C:\Mod Organizer 2\mods\ModTest\SKSE\Plugins\Template.dll` = **731,136 B @ 2026-09-19 18:13:53**，与 `build\bin\RelWithDebInfo\Template.dll` **一致** → 确认本 trace 来自**含靶点 2.1 的新构建**（§1.6 的部署坑未复发） |
| 该 DLL 含有的 hook | #1 `GetItemCount`、#2 `VisitInventory`、#3 `GetInventoryWeight`、#5 `GetValue`、#6 `GetArmorInSlot`、#7 `GetWornMask`、#8 `GetEnchantment` + **靶点 2.1 `RequestItemCardInfo`** |
| 物品数（**推断**） | **≈ 8466**（依据：每轮 `RequestItemCardInfo` 恰好 8466 次，见采集 #7.3） |

**靶点 2.1 是否真的装上了？** 是。两条互相独立的判据：

1. 本轮**首个** `RequestItemCardInfo` 事件的时间戳比首个 `InventoryMenu opened` 只晚 **50 ms**（`1368703174492` vs `1368653073593` ns），且 3 轮**各 8466 次**（不是「首轮漏采、后续补齐」的形态）；
2. 若查表失败，zone 只会有个位数次（vanilla 行为，见采集 #8）——实测 25398 次，相差 3 个数量级。

> ✅ 这说明 [tracy-integration-plan.md](./tracy-integration-plan.md) §4.6.1 把安装挂在 `opening` 事件里的做法**足够早**：`opening` 的派发早于 skyui 的 `processList()`，因此**首轮即被完整捕获**。§4.6.1 中「首轮可能漏采」的保守口径在本轮没有兑现（更好的结果）。

#### 采集 #7.2 逐轮 wall time（opened → closed）

| 轮 | wall time (ms) |
|----|----------------|
| 1 | 2429 |
| 2 | 2141 |
| 3 | 2056 |

口径同 §1.3：这是**完整菜单打开时长**（含 UI 布局、贴图、字型、脚本、音频），**不等于**遍历耗时。

#### 采集 #7.3 靶点 2.1 命中数据

**聚合导出**（`tracy-csvexport <trace>`；本 trace 中被触发的 zone 全部列出）：

| zone | counts | total | mean |
|------|--------|-------|------|
| **`RequestItemCardInfo`** | **25398** | **1791.2 ms** | **70.5 µs** |
| `InventoryEntryData::GetValue` | 117285 | 45.6 ms | 0.39 µs |
| `InventoryChanges::GetInventoryWeight` | 371 | 3.2 ms | 8.7 µs |
| `InventoryChanges::GetItemCount` | 4 | ~0 | — |

（`RequestItemCardInfo` 的 `min/max/std` = 7.46 µs / 649.3 µs / 41.1 µs。）

**逐事件分轮**（`tracy-csvexport -u -f RequestItemCardInfo`，按 `opened` 时间戳切片）：

| 轮 | count | total (ms) | mean (µs) | min (µs) | max (µs) |
|----|-------|-----------|-----------|----------|----------|
| 1 | **8466** | 590.9 | 69.8 | 7.69 | 608.9 |
| 2 | **8466** | 619.0 | 73.1 | 7.58 | 649.3 |
| 3 | **8466** | 581.3 | 68.7 | 7.46 | 200.6 |
| 合计 | 25398 | 1791.2 | 70.5 | — | — |

**三点读法**：

1. **调用次数与物品数严格相等**：每轮恰好 8466 次，与 §7.6 F 段「`processList` 对每个条目各调 1 次」的预测完全吻合。（§4.6 / §7.4 中写的「27211」是**另一存档**的规模；成立的是「次数 = 物品数」这一**关系**，绝对值随存档变化。）
2. **单次 70.5 µs**，比此前由 1.5 s ÷ 27211 反推的 55 µs **略高** → 1.5 s 的估计偏保守（按 70 µs 线性外推，27211 条目 ≈ **1.9 s**）。
3. **它是引擎侧唯一的大头**：1791 ms 是次大 zone（`GetValue` 总耗时 45.6 ms）的 **39 倍**；`GetItemCount`（4 次）、`GetInventoryWeight`（3.2 ms）可忽略。

**`RequestItemCardInfo` 内部构成**（以 `GetValue` 为内标）：每次 `RequestItemCardInfo` 平均伴随 117285 ÷ 25398 ≈ **4.6 次** `GetValue`，合计仅 4.6 × 0.39 ≈ **1.8 µs** —— 占单次的 **2.5 %**。即单次 70 µs 中 **~68 µs 不在属性查询上**，而在 item card 构建 + `GFxValue` 组装 + `Respond` 编组。

#### 采集 #7.4 本轮结论

| 问题 | 本轮实测 |
|------|----------|
| 靶点 2.1 是否命中 | ✅ 命中，且**首轮即生效**（3 轮各 8466 次） |
| 调用次数 = 物品数 × 轮数？ | ✅ 严格成立（8466 × 3 = 25398） |
| 单次耗时符合量级预测？ | ✅ 70.5 µs（预测 ≈ 55 µs，同量级） |
| 是否引擎侧最大热点？ | ✅ 是（1791 ms，是次大 zone 的 39 倍） |
| 它能否**解释** skyui 的全部增量？ | ❓ 需 vanilla 对照 → 见**采集 #8** |

---

### 采集 #8 —— 2026-09-19 21:43（同存档 vanilla 对照）

#### 采集 #8.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_19_21_27_vanilla对照.tracy` |
| 文件大小 | 44,223,045 B（≈ 42.2 MB） |
| 文件有效性 | 文件头 magic = `74 72` → 有效 trace |
| 场景 | **与采集 #7 完全相同的存档**（qasmoke 全部箱子搬空），连续开关物品栏 3 次 |
| 变量 | **移除全部 mod（含 skyui）**；仍保留探测用 DLL（否则没有 Tracy 客户端，采不到数据） |
| 价值 | 与采集 #7 构成**同存档、单变量**对照——唯一的差别就是「skyui 在不在」 |

#### 采集 #8.2 数据

**逐轮 wall time**：

| 轮 | wall time (ms) |
|----|----------------|
| 1 | 3497（首轮冷启动，明显偏高，与 §1.3 的「首轮最高」一致） |
| 2 | **959** |
| 3 | **1088** |

**聚合导出**：

| zone | counts | total | mean |
|------|--------|-------|------|
| `InventoryChanges::GetItemCount` | 240 | 10.5 ms | 43.9 µs |
| `InventoryEntryData::GetValue` | 35652 | 13.8 ms | 0.39 µs |
| `InventoryChanges::GetInventoryWeight` | 230 | 3.3 ms | 14.1 µs |
| **`RequestItemCardInfo`** | **0（zone 未出现）** | — | — |

> ⚠️ **vanilla 下 `RequestItemCardInfo` 为 0 次**，这同时解释两件事：
> (a) 该委托**只在 skyui 里被逐项调用**（§7.6 F 段的预测得到独立验证）；
> (b) vanilla 的 **`InventoryMenu` 根本不调它**——vanilla 中它属于 **`ItemMenu`（单物品菜单）**，只在「高亮 / 打开」各 1 次（`extern/SkyUIUnofficialSDK/src/common/ItemMenu.as:150-168`），而本轮只开关背包、未高亮物品。
> zone 从不执行 → Tracy 不注册 source location → 聚合导出也不会列出（机制与 §1.1 一致）。

#### 采集 #8.3 同存档对照（决定性）

| 指标 | vanilla（#8） | skyui（#7） | **skyui 增量** |
|------|--------------|-------------|----------------|
| 稳定 wall time（轮 2/3） | **~1.0 s**（959 / 1088） | **~2.1 s**（2141 / 2056） | **≈ +1.1 s** |
| `RequestItemCardInfo` | **0 次** | 8466 次/轮 @ 70.5 µs | **+0.6 s/轮（≈ 55 %）** |
| `GetValue` | 11884 次/轮 | 39095 次/轮 | +27211 次/轮（**仅 +11 ms**） |
| `GetItemCount` / `GetInventoryWeight` | 240 / 230 次（13.8 ms） | 4 / 371 次（3.2 ms） | 可忽略 |

**两条决定性结论**：

1. **`RequestItemCardInfo` 确实是 skyui 独有的、与物品数同阶的开销**：vanilla **0 次** vs skyui **8466 次/轮**。与 §7.6 F 段的机制判断完全一致（本轮 vanilla 的 ~1.0 s 也说明「背包打开的固定底盘」约 1 s，与阶段 1 的 vanilla 基线 ~0.85–1.1 s 吻合）。
2. **但它只能解释 skyui 增量的一半**：skyui 的额外 ~1.1 s 中，`RequestItemCardInfo` 的 **C++ 实现**占 ~0.6 s（**≈ 55 %**）；**其余 ~0.5 s（≈ 45 %）落在未插桩处**——最可能是 **GFx 边界编组/往返**（靶点 2.5）与 **ActionScript 层**（skyui 的 `processList` 循环本身 + 8466 次 `updateItemInfo` 回调 + 列表 UI 更新；均在 SWF 内部，Tracy 看不到）。

> ⚠️ **口径修正**：§7.4 / §7.6 H 段此前把 ~1.5 s **整体**归因于「全量物品 × GFx 委托往返」。本对照证伪了「整体」二字——委托的 **C++ 实现**只占增量的一半，另一半在**边界编组与 AS 层**。§7.1 表中「+ skyui 2236 / 2133 / 4004 ms」那 1.1–1.4 s 同样应读作**两段之和**，而非单一靶点的耗时。

#### 采集 #8.4 对阶段 3 分支的直接影响

| 结论 | 含义 |
|------|------|
| 阶段 **3a′**（hook `RequestItemCardInfo` 降低单次成本）的**天花板 = 0.6 / 1.1 ≈ 55 %** | 即使把该委托的 C++ 实现降到 0 成本，**最多也只能拿回一半**；且单次 70 µs 里 `GetValue` 只占 1.8 µs（2.5 %），真正成本在 item card 构建 + `GFxValue` 组装 + `Respond` 编组——要压低它就得改引擎侧的构建逻辑，**收益有限、风险不低** |
| 剩下 ~0.5 s（≈ 45 %）**3a′ 完全碰不到** | 它落在 GFx 边界 / AS 层。要么先用**靶点 2.5**（`FxDelegate::Callback` 通用分发，按 `a_methodName` 过滤）把「边界编组」与「AS 层」再拆开，要么直接走**阶段 4** |
| **阶段 4（批量通道 + 改 SWF 消费方式）仍是根治方向** | 只有「一次性灌入 + AS 不再逐项请求」能同时消掉这两段；`skse.ExtendData(true)` 通道（§7.6 F 段第 1 行）**已存在**，缺的是把 `value` / `weight` / `damage` / `armor` / `effects` 也并入该通道，并改 SWF 的消费方式（`InventoryDataSetter.processEntry()` 已证实这些字段目前是「逐项请回来」的，见 §7.6 E） |

### 采集 #9 —— 2026-09-19 23:08（skyui + **靶点 2.5 首次生效**；3 轮）

#### 采集 #9.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_19_23_08_skyui.tracy` |
| 文件大小 | 36,255,068 B（≈ 36.3 MB）；写入时间 23:10:44 |
| 文件有效性 | 文件头 magic = `74 72`（ASCII `"tr"`）→ 有效 trace |
| 场景 | 「极大量物品」存档（重新填充），连续开关物品栏 **3 次** |
| **实际加载的 DLL** | `C:\Mod Organizer 2\mods\ModTest\SKSE\Plugins\Template.dll` = **735,232 B**，与 `build\bin\RelWithDebInfo\Template.dll` 一致 → 确认本 trace 来自**含靶点 2.1 + 2.5 的新构建**（部署坑未复发） |
| 该 DLL 含有的 hook | #1 `GetItemCount`、#2 `VisitInventory`、#3 `GetInventoryWeight`、#5 `GetValue`、#6 `GetArmorInSlot`、#7 `GetWornMask`、#8 `GetEnchantment` + **靶点 2.1 `RequestItemCardInfo`** + **靶点 2.5 `FxDelegate::Callback`（vtable slot 1）** |
| 物品数 | **6441**（依据：每轮 `RequestItemCardInfo` 恰好 6441 次、3 轮合计 19323 = 6441 × 3 **精确整除**，见 §9.3） |

**靶点 2.5 是否真的生效？** 是，三条独立判据：

1. `FxDelegate::Callback[RequestItemCardInfo]` zone **在 trace 中出现**且 3 轮各 6441 次——若活实例 vtable 校验失败，代码会打 `error` 并**不 hook**，zone 将一次都不出现（[tracy-integration-plan.md](./tracy-integration-plan.md) §4.6.3 校验 #1）；
2. 它**严格嵌套**在 2.1 之外（`Callback[…]` ⊃ `RequestItemCardInfo`，次数相同、total 略大）；
3. 与 vanilla（采集 #10）对照——**0 次**，差 3 个数量级。

> ✅ 这说明 [tracy-integration-plan.md](./tracy-integration-plan.md) §4.6.3 的 vtable 校验与槽位推导（slot 1）**在真机上成立**，且「先读槽 → 赋 `orig` → 再写槽」的竞态修复有效（未崩溃、未丢事件）。

#### 采集 #9.2 逐轮 wall time（opened → closed）

| 轮 | `opened` (ns) | `closed` (ns) | wall time (ms) |
|----|---------------|---------------|----------------|
| 1 | 79920818259 | 82537063519 | 2616.2（首轮冷启动，偏高） |
| 2 | 85287700980 | 87070602912 | **1782.9** |
| 3 | 90396424772 | 92053596887 | **1657.2** |

口径同 §1.3：完整菜单打开时长。稳定轮（2/3）中位 ≈ **1720 ms**。

#### 采集 #9.3 靶点 2.1 / 2.5 命中数据（决定性）

**聚合导出**（`tracy-csvexport <trace>`，本 trace 被触发的 zone 全部列出）：

| zone | counts | total | mean | min | max | std |
|------|--------|-------|------|-----|-----|-----|
| **`FxDelegate::Callback[RequestItemCardInfo]`**（2.5） | **19323** | **1246.72 ms** | **64.52 µs** | 7.313 µs | 303.478 µs | 34.34 µs |
| **`RequestItemCardInfo`**（2.1） | **19323** | **1243.45 ms** | **64.35 µs** | 7.253 µs | 303.318 µs | 34.30 µs |
| `InventoryEntryData::GetValue` | 62835 | 26.52 ms | 0.422 µs | 150 ns | 19.386 µs | 0.685 µs |
| `InventoryChanges::GetItemCount` | 289 | 25.32 ms | 87.6 µs | 80 ns | 281.9 µs | 29.8 µs |
| `InventoryChanges::GetInventoryWeight` | 299 | 1.70 ms | 5.68 µs | 10 ns | 586.9 µs | 53.7 µs |

**逐事件分轮**（`tracy-csvexport -u -f RequestItemCardInfo`，按 `opened` 时间戳切片；⚠️ `-f` 是**子串**匹配，故一次导出同时含两个 zone，再按 `name` 分组）：

| 轮 | `Callback[…]` count | `Callback[…]` total (ms) | `Callback[…]` mean (µs) | `RequestItemCardInfo` total (ms) | `RequestItemCardInfo` mean (µs) | **边界差 (ms)** |
|----|--------------------|--------------------------|-------------------------|----------------------------------|---------------------------------|-----------------|
| 1 | **6441** | 416.9 | 64.7 | 415.7 | 64.5 | **1.2** |
| 2 | **6441** | 416.8 | 64.7 | 415.8 | 64.6 | **1.0** |
| 3 | **6441** | 413.0 | 64.1 | 411.9 | 63.9 | **1.1** |
| 合计 | 19323 | 1246.7 | 64.5 | 1243.5 | 64.4 | 3.3 |

**四点读法**：

1. **调用次数 = 物品数 × 轮数**：每轮 **6441** 次，3 轮 **19323 = 6441 × 3**，与 §7.6 F 段「`processList` 对每个未过滤条目各调 1 次」再次吻合（本次存档规模 6441，非采集 #7 的 8466）。
2. **单次 64.5 µs**，与采集 #7（70.5 µs）同量级（略低，说明耗时有 ±10 % 的存档/物品构成相关波动）。
3. **边界成本 ≈ 0（本轮最关键的新信息）**：`Callback[…] − RequestItemCardInfo` 仅 **1.0–1.2 ms/轮**（≈ 0.17 µs/次），占 skyui 增量 **< 0.2 %**。
4. **`GetValue` 内标**：每次 `RequestItemCardInfo` 平均伴随 62835 ÷ 19323 ≈ **3.25 次** `GetValue`，合计仅 3.25 × 0.422 ≈ **1.4 µs** → 占单次 64.5 µs 的 **≈ 2 %**。**即 ~98 % 不在属性查询上**（与采集 #7 的 2.5 % 同口径）。

> ⚠️ **方法论文正（对 [tracy-integration-plan.md](./tracy-integration-plan.md) §4.6.3 判读表的修正）**：`FxDelegate::Callback[…]` zone 只包住**进入引擎 `FxDelegate::Callback` 之后**到返回——即 `callbacks.GetAlt()` 查表 + `FxDelegateArgs` 构造 + `cbDef->callback(params)`。而 **AS 参数 → `GFxValue[]` 的真正编组发生在 Scaleform 内部、进入 `Callback` 之前**，**Tracy 测量不到**。因此本轮的「边界 ≈ 0」严格只证明**引擎侧分发开销 ≈ 0**；Scaleform 侧的编组成本仍混在下方「AS 层残差」里，**无法用 Tracy 进一步拆分**。

#### 采集 #9.4 本轮结论（与采集 #10 对照后）

见下方**采集 #10.3** 的**同存档对照表**——本轮结论与阶段 2 的最终裁定写在 **采集 #10.4** 与 [tracy-integration-plan.md](./tracy-integration-plan.md) **§4.6.4**。

### 采集 #10 —— 2026-09-19 23:08（**同存档 vanilla 对照**，与 #9 配对）

#### 采集 #10.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_19_23_08_vanilla.tracy` |
| 文件大小 | 39,423,928 B（≈ 39.4 MB）；写入时间 23:08:43 |
| 场景 | **与采集 #9 完全相同的存档**，连续开关物品栏 3 次 |
| 变量 | **移除全部 mod（含 skyui）**；仍保留探测用 DLL（否则没有 Tracy 客户端，采不到数据） |
| 价值 | 与采集 #9 构成**同存档、单变量**对照——唯一差别就是「skyui 在不在」 |

#### 采集 #10.2 数据

**逐轮 wall time**：

| 轮 | `opened` (ns) | `closed` (ns) | wall time (ms) |
|----|---------------|---------------|----------------|
| 1 | 2835778395932 | 2837004030190 | 1225.6（首轮冷启动，偏高） |
| 2 | 2838814027204 | 2839572908794 | **758.9** |
| 3 | 2841103970296 | 2842226820192 | **1122.8** |

稳定轮（2/3）中位 ≈ **941 ms**（与阶段 1 的 vanilla 基线 ~0.85–1.1 s 吻合）。

**聚合导出**：

| zone | counts | total | mean |
|------|--------|-------|------|
| `InventoryChanges::GetItemCount` | 295 | 20.65 ms | 70.0 µs |
| `InventoryEntryData::GetValue` | 27825 | 10.17 ms | 0.365 µs |
| `InventoryChanges::GetInventoryWeight` | 284 | 1.44 ms | 5.06 µs |
| **`RequestItemCardInfo`（2.1）** | **0（zone 未出现）** | — | — |
| **`FxDelegate::Callback[RequestItemCardInfo]`（2.5）** | **0（zone 未出现）** | — | — |

> **两个 zone 在 vanilla 下都是 0 次**（与采集 #8 一致）：再次独立验证「`RequestItemCardInfo` 只被 skyui 逐项调用」——vanilla 中它属于 `ItemMenu`（单物品菜单），只在「高亮 / 打开」各 1 次，而本轮只开关背包、未高亮物品。zone 从不执行 → Tracy 不注册 source location → 聚合导出也不列出。

#### 采集 #10.3 同存档对照（**阶段 2 决定性**）

取稳定轮（第 2/3 轮）中位：

| 指标 | vanilla（#10） | skyui（#9） | **增量** |
|------|----------------|-------------|----------|
| wall time（轮 2/3） | **941 ms**（758.9 / 1122.8） | **1720 ms**（1782.9 / 1657.2） | **≈ +779 ms** |
| `RequestItemCardInfo`（2.1，C++ 实现） | 0 次 | 6441 次/轮 @ 64.4 µs → **≈ 414 ms/轮** | **≈ +414 ms（≈ 53 %）** |
| `FxDelegate::Callback[…]`（2.5，边界） | 0 次 | 6441 次/轮 @ 64.5 µs → **≈ 415 ms/轮** | 含上项 |
| **边界差（2.5 − 2.1，引擎侧分发）** | — | **1.0–1.2 ms/轮** | **≈ +1 ms（< 0.2 %）** |
| `GetValue` | 9275 次/轮（10.17 ms/3轮） | 20945 次/轮（26.52 ms/3轮） | +11670 次/轮（**+5.5 ms/轮，< 1 %**） |
| `GetItemCount` / `GetInventoryWeight` | 295 / 284 次 | 289 / 299 次 | 可忽略 |

**增量归属（779 ms）**：

| 段 | 每轮 | 占比 | 证据 |
|----|------|------|------|
| `RequestItemCardInfo` **C++ 实现** | ≈ 414 ms | **≈ 53 %** | 2.1 zone |
| **GFx 边界（引擎侧分发）** | ≈ 1 ms | **≈ 0.1 %** | 2.5 − 2.1 |
| **AS 层 + Scaleform 编组**（Tracy 视野外） | ≈ 364 ms | **≈ 47 %** | 残差 |

#### 采集 #10.4 本阶段最终裁定（**推翻「边界可拿 45 %」的假设**）

1. **靶点 2.5 的结论是「边界 ≈ 0」，不是「边界占一半」**：引擎侧分发（`GetAlt` + `FxDelegateArgs`）实测 **0.17 µs/次**，占 skyui 增量 **< 0.2 %**。→ 采集 #8 中「剩余 ~45 % 落在 **GFx 边界 / AS 层**」的二分法，现在**收窄为「几乎全在 AS 层」**；**引擎侧边界优化无利可图**。
2. **残留 47 % 的边界（Tracy 可测范围之外）**：`Callback[…]` zone 的起点在**进入引擎 `FxDelegate::Callback` 之后**，而「AS 参数 → `GFxValue[]` 的编组」发生在 Scaleform 内部、进入 `Callback` **之前** → **Tracy 测不到**。因此那 364 ms 是「Scaleform 内部编组 + AS 层 `processList` 循环 + 6441 次 `updateItemInfo` 回调 + 列表 UI 更新」的**合体**，**无法用 Tracy 再拆**（除非上符号级采样，见 §7.5 的符号解析限制）。
3. **阶段 3 / 4 分支的最终裁定**：
   | 方向 | 判决 |
   |------|------|
   | **3a′**（降低 `RequestItemCardInfo` 单次成本） | 天花板 **≈ 53 %**；且其内部 `GetValue` 仅占 **≈ 2 %**，只能改引擎的 item card 构建 — **收益有限、风险高** |
   | **GFx 边界优化**（原以为可拿 45 %） | ❌ **已被 2.5 证伪**：引擎侧边界 ≈ 0 |
   | **阶段 4**（`skse.ExtendData(true)` 批量通道 + 改 SWF 消费方式） | ✅ **唯一能拿到剩余 ≈ 47 % 的方向**（且顺带消掉 3a′ 的 53 %，因为逐项 `RequestItemCardInfo` 往返本身就不再需要） |
4. **物品数口径**：本存档实测 **6441** 件（`RequestItemCardInfo` 3 轮各 6441 次精确整除、vanilla `GetValue` 9275 次/轮 ÷ 1.44 ≈ 6441 双路自洽）。若与人工计数（如 6567）不一致，应以**每轮委托次数**为准（它直接等于 `entryList` 中**未被 skyui 过滤**的条目数）。

### 采集 #11a —— 2026-09-20 15:26–15:29（**S0a 转发探针首次生效**；5 轮 + 中途滚动）

#### 采集 #11a.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_20_15_29.tracy` |
| 文件大小 | 61,058,287 B（≈ 61.1 MB）；写入时间 15:29:28 |
| 文件有效性 | 文件头 magic = `74 72 fd 50`（ASCII `tr`）→ 有效 trace |
| 场景 | 「极大量物品」存档（**另行加入 10 个堆叠物品**，故 `entryList` 仍为 6441）；**5 轮**开背包，且**中途滚动 / 高亮过列表**（这一条是本次的关键变量，见 §11a.4） |
| **实际加载的 DLL** | `build\bin\RelWithDebInfo\Template.dll` = **762 880 B**（构建时间 13:28:24），与 [phase4-design.md](./phase4-design.md) §10.5(5) 的 `ENABLE_TRACY=ON` 值**逐字节一致** → 确认本 trace 来自**含 S0a 探针**的构建 |
| 该 DLL 含有的 hook | 靶点 #1 / #2 / #3 / #5 / #6 / #7 / #8 + **靶点 2.1 `RequestItemCardInfo`** + **靶点 2.5 `FxDelegate::Callback`** + **S0a：`_dataProcessors[0]._requestItemInfo` 转发探针** |
| 物品数 | **6441**（全量路径每轮恰好 6441 次，5 轮一致；见 §11a.3） |
| 轮数 | **5**（由日志中 `S0a: … = C++ probe (pure forwarder…)` 出现 **5 次**确认，**不是推断**） |

#### 采集 #11a.2 聚合数据（`tracy-csvexport <trace>`）

| zone | counts | total | mean | min | max | std |
|------|--------|-------|------|-----|-----|-----|
| **`AS::_requestItemInfo`（S0a 探针）** | **32201** | **2506.73 ms** | **77.85 µs** | 14.367 µs | 423.06 µs | 39.17 µs |
| **`RequestItemCardInfo`（2.1）** | **32242** | **2239.00 ms** | **69.44 µs** | 7.594 µs | 1726.21 µs | 49.71 µs |
| **`FxDelegate::Callback[RequestItemCardInfo]`（2.5）** | **32242** | **2246.21 ms** | **69.67 µs** | 7.694 µs | 1728.00 µs | 49.82 µs |
| `InventoryEntryData::GetValue` | 114073 | 49.996 ms | 0.438 µs | 150 ns | 23.865 µs | 0.720 µs |
| `InventoryChanges::GetItemCount` | 725 | 69.55 ms | 95.9 µs | 70 ns | 482.4 µs | 30.7 µs |
| `InventoryChanges::GetInventoryWeight` | 330 | 2.653 ms | 8.04 µs | 10 ns | 496.0 µs | 55.8 µs |

**嵌套关系成立**：`AS::_requestItemInfo` ⊃ `RequestItemCardInfo`（探针在**外层**，zone 包住整次转发），两者单次差 **8.40 µs**（77.85 − 69.44）= `_requestItemInfo` 自身 AS 体 + `apply` + 探针转发的成本 —— 即 **S1+S2 的轮级口径下限**（S1–S5 的细分留给 S0b）。

**2.5 − 2.1 边界仍 ≈ 0**：**0.23 µs/次**（69.67 − 69.44），与采集 #9 的 0.17 µs/次同量级 → 「引擎侧分发开销 ≈ 0」的结论在 S0a 探针存在下**未被改变**（回归检查通过）。

#### 采集 #11a.3 count 对账（**差 41 = 5 + 36**，S0a 的核心验收）

| 量 | 值 | 构成 |
|----|----|------|
| `RequestItemCardInfo` | **32242** | 全量 **32206**（`6441×4 + 6442×1`）+ **36** 次悬停 / 事件路径 |
| `AS::_requestItemInfo` | **32201** | 全量 **32200**（`6440×5`）+ **1**（滚动新增条目，探针已装 → 计入） |
| **差** | **41** | **5**（全量路径：每轮第 1 次走旧函数）+ **36**（悬停路径：不经探针） |

逐簇明细（`tracy-csvexport -u` 逐事件，按 > 1 s 间隔分簇；时间为 ms，相对 trace 起点）：

| 簇 | 时间（ms） | `RequestItemCardInfo` | `AS::_requestItemInfo` | 判定 |
|----|-----------|----------------------|------------------------|------|
| 轮 1 | 87126→87975 | 6441 | 6440 | 全量，漏 1（探针在该次调用**内部**安装） |
| 轮 2 | 91515→92365 | 6441 | 6440 | 同上 |
| 轮 3 | 94701→95548 | 6441 | 6440 | 同上 |
| 轮 4 | 98561→99383 | 6441 | 6440 | 同上 |
| 悬停 a–e | 103773 / 105616 / 114770 / 118453 / 120907 | 2 / 19 / 9 / 2 / 2 | 0 / 0 / 0 / **1** / 0 | **`onItemHighlightChange` 路径**，不经探针；仅 118453 那 1 次是「滚动新增条目」走了全量 |
| 轮 5 | 126145→127324 | 6442 | 6440 | 全量 6441（漏 1）+ **1 次悬停**（`opening` 后立即高亮某件） |
| 悬停 f | 131222→131604 | 2 | 0 | 同上 |

> **回归检查**：全量路径的 `RequestItemCardInfo` 每轮仍是 **6441 次**、单次 69.44 µs，与采集 #9 的 6441 次 @ 64.35 µs 同量级（略高，属存档 / 物品构成相关波动，与 §10.4 记录的 ±10 % 同口径）→ 探针**未破坏既有测量**。

#### 采集 #11a.4 结论与新增发现

1. **S0a 通过**：`CreateFunction` 的产物**能被 AS2 的 `apply` 正常调用**，转发**语义无损**（判据表与对账见 [phase4-design.md](./phase4-design.md) **§10.5(6)**）→ **H2 成立**，S0b 可以继续；
2. **新增架构发现：`RequestItemCardInfo` 有两条独立触发路径**（本次最重要的副产品）—— 除**全量路径**（`ItemcardDataExtender.as:26`，经 `_requestItemInfo`）外，还有**悬停 / 事件路径**（`ItemMenu.as:357` `onItemHighlightChange`；`InventoryMenu.as:223` `onQuantityMenuSelect` / `:250` `onItemCardSubMenuAction`）**直接调委托、不经过任何 processor 成员**。这条路径**不在** S0a/S0b 的插入点覆盖内（全量路径占 99.9 %，悬停路径占 0.1 %）—— 记入 [phase4-design.md](./phase4-design.md) §4.2「双路径」；
3. **count 自检方法修正（务必沿用）**：判据「`_requestItemInfo` count == `RequestItemCardInfo` count − 轮数」**只在纯 opening、不滚动、不高亮时成立**；本采集因中途滚动，差额变为 **41**。区分「悬停路径」与「探针丢调用」**必须**用 `-u`（逐事件）+ 按时间分簇：前者表现为**零散小簇**（2 / 19 / 9 / 2 / 2），后者会表现为**某一轮整批缺失**；
4. **采集行为建议**：若要最干净地验收 count，应只做「开背包 → 关闭 → 再开」的纯 opening 循环、**不碰鼠标**。

#### 采集 #11a.5 后续：采集 #11b 的准备（S0b 已落地，2026-09-20 同日）

> 本节**不是新采集**，而是把「#11a 记录的产物指纹已被覆盖」这件事留档，避免下一轮拿错 DLL 或照旧前缀 grep 日志。

| 项 | 值 |
|----|-----|
| 代码变更 | **S0b 已实现并通过构建验证**：在同一安装点补上 `processList` + `processEntry` 两个转发探针（**三者原子装入**），设计与构建数据见 [phase4-design.md](./phase4-design.md) **§10.6** |
| **新 DLL 指纹** | `build\bin\RelWithDebInfo\Template.dll` = **767 488 B**（`ENABLE_TRACY=ON`）—— 取代 §11a.1 记录的 **762 880 B**（+4 608 B = 两个 handler 类 + 3 个 zone 名字符串） |
| 新增 zone | `AS::processList`（预期 count = 轮数 − 1）、`AS::processEntry`（预期 count **严格等于**全量路径 `RequestItemCardInfo` count） |
| **日志前缀已变** | 探针日志前缀由 `S0a:` 统一为 **`S0:`**，安装日志改为一次列出三个成员。照 §11a 的字符串 grep 会**找不到**，新字符串见 §10.6(4) |
| **采集卫生（硬要求）** | #11b 必须**不滚动、不悬停**：`AS::processEntry` 的 count 自检是**严格相等**式，悬停路径（`onItemHighlightChange`）会污染它（§11a.4 结论 3 / 4） |
| 验收判据 | 8 条，见 §10.6(7) |
| **结果** | ✅ **已完成**：2026-09-20 18:34–18:35 采集（见下方 **#11b**）；DLL 指纹与本节预测**逐字节一致**（767 488 B / `ENABLE_TRACY=ON`） |

### 采集 #11b —— 2026-09-20 18:34–18:35（**S0b 三探针首次生效**；5 轮纯 opening）

#### 采集 #11b.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_20_18_35.tracy` |
| 文件大小 | 51,457,139 B（≈ 51.5 MB）；写入时间 18:35:50 |
| 场景 | 「极大量物品」存档，**5 轮**开 / 关背包，**全程不滚动、不悬停**（满足 §11a.5 的硬要求） |
| **实际加载的 DLL** | `build\bin\RelWithDebInfo\Template.dll` = **767 488 B**，与 [phase4-design.md](./phase4-design.md) §10.6(6) 的 `ENABLE_TRACY=ON` 值**逐字节一致** → 确认本 trace 来自**含 S0b 三探针**的构建 |
| 该 DLL 含有的 hook | 靶点 #1 / #2 / #3 / #5 / #6 / #7 / #8 + 靶点 2.1 `RequestItemCardInfo` + 靶点 2.5 `FxDelegate::Callback` + **S0b：`_dataProcessors[0]` 的 `_requestItemInfo` / `processList` / `processEntry` 三个转发探针（原子装入）** |
| 物品数 | **6441**（每轮全量路径恰好 6441 次，5 轮一致；§11b.3） |
| 轮数 | **5**（由日志 `S0: \`_dataProcessors[0]\` wrapped: …` 出现 **5 次**确认 —— 18:34:15.873 / 19.663 / 23.609 / 27.373 / 30.923，**不是推断**） |

#### 采集 #11b.2 聚合数据（`tracy-csvexport <trace>`）

| zone | counts | total | mean | min | max | std |
|------|--------|-------|------|-----|-----|-----|
| **`AS::_requestItemInfo`（S0a 探针）** | **32200** | **2505.89 ms** | **77.82 µs** | 14.147 µs | 613.76 µs | 37.95 µs |
| **`RequestItemCardInfo`（2.1）** | **32205** | **2216.77 ms** | **68.83 µs** | 7.595 µs | 602.43 µs | 37.27 µs |
| **`FxDelegate::Callback[RequestItemCardInfo]`（2.5）** | **32205** | **2224.72 ms** | **69.08 µs** | 7.685 µs | 772.23 µs | 37.51 µs |
| **`AS::processEntry`（S0b 探针）** | **32205** | **1516.41 ms** | **47.09 µs** | 8.717 µs | **14.59 ms** | 190.89 µs |
| **`AS::processList`（S0b 探针）** | **4** | **14.34 ms** | **3.585 ms** | 3.365 ms | 3.754 ms | 163.2 µs |
| `InventoryEntryData::GetValue` | 104725 | 47.69 ms | 0.455 µs | 150 ns | 85.14 µs | 0.836 µs |
| `InventoryChanges::GetItemCount` | 390 | 37.37 ms | 95.8 µs | 90 ns | 298.8 µs | 26.7 µs |
| `InventoryChanges::GetInventoryWeight` | 327 | 2.44 ms | 7.47 µs | 10 ns | 577.4 µs | 57.1 µs |

**嵌套关系成立**：`AS::_requestItemInfo` ⊃ `RequestItemCardInfo`（探针在**外层**）—— 逐事件核对：**32200 个 `RequestItemCardInfo` 落在某个 `_requestItemInfo` 窗口内，仅 5 个在外**（正是每轮第 1 项，见 §11b.3）。单次差 **8.99 µs**（77.82 − 68.83）= `_requestItemInfo` 自身 AS 体 + `apply` + 探针转发。

**2.5 − 2.1 边界仍 ≈ 0**：**0.25 µs/次**（69.08 − 68.83），与采集 #9 的 0.17 µs/次、#11a 的 0.23 µs/次同量级 → 回归检查通过。

**单线程（H4 成立）**：四个 zone（`_requestItemInfo` / `RequestItemCardInfo` / `processEntry` / `Callback[…]`）的 `thread` 列**全部 = 10**，无跨线程事件 → AS 与 C++ zone 可直接相减。

#### 采集 #11b.3 count 对账（判据 1–6，全中）

逐事件分簇（`tracy-csvexport -u`，按 > 1 s 间隔分簇；时间为 trace 起点后的 ms）：

| 轮 | `RequestItemCardInfo`（全量路径） | `AS::_requestItemInfo` | `AS::processEntry` | 全量窗口（首→末，span） |
|----|-------------------------------|------------------------|--------------------|------------------------|
| 1 | 6441 | 6440 | 6441 | 632778.0 → 633650.0（872.0 ms） |
| 2 | 6441 | 6440 | 6441 | 636567.8 → 637420.5（852.7 ms） |
| 3 | 6441 | 6440 | 6441 | 640513.5 → 641353.4（839.9 ms） |
| 4 | 6441 | 6440 | 6441 | 644277.8 → 645135.4（857.6 ms） |
| 5 | 6441 | 6440 | 6441 | 647827.8 → 648654.1（826.3 ms） |
| **合计** | **32205** | **32200** | **32205** | — |

| # | 判据 | 实测 | 结论 |
|---|------|------|------|
| 1 | 三个 first-call 日志各一次 | 3 条（`processEntry` 18:34:15.873 / `_requestItemInfo` 18:34:15.874 / `processList` 18:34:20.677） | ✅ |
| 2 | 安装日志每轮一次 | **5** 次 | ✅ |
| 3 | `thisPtr` 语义分叉被实测确认 | `_requestItemInfo` → list；`processList` / `processEntry` → processor；`argCount` = 2 / 1 / 2 | ✅ |
| 4 | `AS::processEntry` count == 全量路径 count（**严格相等**） | **32205 == 32205**，且**逐轮** 6441 == 6441 | ✅ |
| 5 | `AS::processList` count == 轮数 − 1 | **4 == 5 − 1**（数值 ✅，但**成因与预期不同** → 见 §11b.4） | ⚠️ |
| 6 | `AS::_requestItemInfo` count == 全量路径 count − 轮数 | **32200 == 32205 − 5**，且**逐轮** 6440 == 6441 − 1 | ✅ |
| 7 | 卡片数值与 skyui 原版一致 | 探针为纯转发（零缓存、零改写），路径与 S0a 同构 | ✅（推定，同 §10.5(6)） |
| 8 | 由嵌套解出 S1–S5 | S3 **不可解 → 剔除** | ⚠️ |

> **判据 4 的机制再次自检通过**：`processEntry` 的 first-call（18:34:15.873）**早于** `_requestItemInfo` 的 first-call（18:34:15.874）1 ms —— 与 §4.2 拆分表「`:55` 的往返返回后才调 `:56` 的 `processEntry`」一致：第 1 项的 `_requestItemInfo` 走旧函数（无 zone），而同项的 `processEntry` 已被覆盖（有 zone）→ 每轮 `processEntry` = 6441（**含第 1 项**）、`_requestItemInfo` = 6440。

#### 采集 #11b.4 决定性发现：`AS::processList` 探针**测到的是空转**，S3 剔除

`AS::processList` 的 4 个 zone 的**精确窗口**与**窗口内子事件数**：

| # | 起点（ns） | 时长 | 窗口内 `_requestItemInfo` | 窗口内 `processEntry` | 窗口内 `RequestItemCardInfo` |
|---|------------|------|---------------------------|----------------------|------------------------------|
| 1 | 637581917789 | 3.639 ms | **0** | **0** | **0** |
| 2 | 641508425015 | 3.365 ms | **0** | **0** | **0** |
| 3 | 645282129775 | 3.754 ms | **0** | **0** | **0** |
| 4 | 648801488901 | 3.583 ms | **0** | **0** | **0** |

这 4 次调用都紧跟在各轮全量窗口**之后**（第 2 轮窗口 `636567.8 → 637420.5`，spin 在 `637582.0`，即窗口**起点**后 **1.014 s**、**结束**后 **0.16 s**），且**不含任何逐项工作** → 它们是**空转** `processList`（`skyui_itemDataProcessed` 已全为 `true`，6441 项全部走 `continue`）；3.6 ms 即「纯 AS 循环 + 过滤判断」的耗时。

**每轮的全量 `processList`（6441 项 × 逐项往返 + `processEntry`，约 0.83–0.87 s）从未被本探针覆盖** —— 探针安装在第 1 项 `RequestItemCardInfo` 的 hook 内（§4.2 拆分表），而那时第 1 次（全量）`processList` **已经在执行中**。

> ⚠️ **因此 `S3 = processList − Σ(_requestItemInfo) − Σ(processEntry)` 这个方程不成立**（实测左项 14.34 ms，右项两个减数之和 4022 ms，符号为负）—— **S3 无法由本探针解出，予以剔除**（[phase4-design.md](./phase4-design.md) §4.2 已就地修正）。
>
> 剔除**不影响结论**：空转 3.6 ms 已给出「纯 AS 循环 6441 项」的量级下界；全量循环本体（含 `fixSKSEExtendedObject`）即使放大一个数量级仍在数十 ms 级，与下面 S4 的 **303 ms/轮**相差悬殊。

#### 采集 #11b.5 S1–S5 分解（每轮 6441 项；S3 剔除）

| 段 | 每轮 | 占比 | 证据 | 谁能消掉 |
|----|------|------|------|---------|
| `RequestItemCardInfo` **C++ 实现** | **443.4 ms** | **55.1 %** | 2.1 zone：32205 × 68.83 µs ÷ 5 | **4a** |
| `_requestItemInfo` 体 + `apply` + 转发 | **57.9 ms** | **7.2 %** | `AS::_requestItemInfo` − `RequestItemCardInfo` = 8.99 µs × 6440 | **4a** |
| **S4 = `processEntry`（855 行 AS）** | **303.3 ms** | **37.7 %** | `AS::processEntry`：32205 × 47.09 µs ÷ 5 | **4b-ii** |
| ~~S3 = `processList` 全量循环本体~~ | ~~不可测~~ | **剔除** | 见 §11b.4 | — |
| S5（`invalidate` + `UpdateList`） | 未测 | — | 在 `processList` 之外（[phase4-design.md](./phase4-design.md) §6.3），本次未插桩 | P4 |
| **合计** | **≈ 804.6 ms/轮** | **100 %** | 对照：全量窗口实测 826–872 ms/轮（差额含 S5 等） | — |

**方向裁决**：

| 方向 | 判决 | 理由 |
|------|------|------|
| **4a**（消往返，P1） | ✅ **必做** | 一次拿到 443.4 + 57.9 ≈ **501 ms/轮（62.3 %）**，且零逻辑复刻 |
| **4b-i**（只换 `processList`） | ❌ **跳过** | 唯一增量 S3 已剔除（量级 ≤ 数十 ms）→ 收益 < 2.5 %，不值得引入「复刻循环过滤语义」的风险 |
| **4b-ii**（循环 + `processEntry` 都 C++ 化） | ✅ **保留（第二批）** | 唯一能拿到 **S4 = 303.3 ms/轮（37.7 %）** 的路径；代价 = 复刻 `InventoryDataSetter.as:24-878`（≈ 855 行） |

**假设裁定**：**H5 判否** —— S1+S2 **不是**主导（其引擎侧已被靶点 2.5 证伪为 ≈ 0，AS 侧体仅 7.2 %）；**S4 才主导**（37.7 %）。**H9 沿用 #11a 判定为假**（`processEntry` 每轮 6441 次，见判据 4）；**H4 成立**（单线程，见 §11b.2）。

#### 采集 #11b.6 附带发现

1. **`AS::processEntry` 存在离群值**：max = **14.59 ms**（mean 47.1 µs 的 310 倍；std 190.9 µs）—— 某个 `formType` 分支（`InventoryDataSetter.as:37-101` 的 armor / damage / effects / poisoned 之一）在个别物品上极贵。对总账影响 < 0.5 %（32205 次中仅少数几次），但属「单件拖慢」隐患，建议在 4b-ii 前定位；
2. **每轮各一次「空转 `processList`」**（§11b.4），发生在全量之后约 1.0 s。本次**不深究其触发源**（候选：`BasicList.requestInvalidate` → `commitInvalidate` 的延迟失效链，`BasicList.as:208-250`），仅记录它**不参与** S1–S5 分解。**注意本版无 `opened` / `closed` zone**，故本轮没有 wall time，只有「全量窗口 span」；
3. **回归检查**：单次 `RequestItemCardInfo` 68.83 µs、`GetValue` 104725 次 —— 与 #9（64.35 µs / 62835 次）、#11a（69.44 µs / 114073 次）同量级 → 探针**未破坏**既有测量。

---

### 采集 #12（阶段 4 / 步骤 S1 = 4a）—— **已运行**（2026-09-21 12:18–12:19；判据 1–6 全中，判据 6 机制命中、量级优于预测）

> ✅ **本节已填实测。** 预测值来自 [phase4-design.md](./phase4-design.md) **§10.8(9)**，按约定**未回改**：`#12.2` / `#12.3` 每行同时保留「预测」与「实测」，偏离逐条解释（见 §12.4 第 2/3 点）。
>
> 本步与前面所有采集的**性质不同**：S0/S0b 是**只转发**的探针（不可能改变 UI），而 S1 是**第一个真正改变行为**的步骤 —— 命中缓存时不再问引擎。因此本轮的判据不只是「更快」，还必须同时证明「**数值一样**」（§12.3 判据 3，本轮 **10/10 通过**）。

#### 采集 #12.0 运行前 checklist

| # | 检查项 | 值 / 做法 |
|---|--------|-----------|
| 1 | **DLL 指纹** | `build\bin\RelWithDebInfo\Template.dll` = **780,288 B**；与 #11b 的 767,488 B **不同**（这是区分两个构建最直接的办法）。⚠️ 本行原写的 `77FE31B2…47162` 是**登记时的占位值**，与 §10.8(8) 的正式指纹不符 —— **以 §12.1 的实测 `44BEDCFF…F1D5` 为准**（构建产物与部署文件两处 `Get-FileHash` 均已核对一致） |
| 2 | 部署 | 覆盖 `C:\Mod Organizer 2\mods\ModTest\SKSE\Plugins\` 下的 `Template.dll` **和** `Template.pdb` |
| 3 | 场景 | 与 #11b **完全一致**：同一「极大量物品」存档、**5 轮**纯开 / 关背包、**全程不滚动、不悬停**（满足 §11a.5 的硬要求） |
| 4 | 关键约束 | **全程不拾取、不丢弃、不使用、不买卖** —— 本步的缓存**不做失效**（§10.8(7)），清单必须保持静态，否则卡片数值可能显示上一轮的数字 |
| 5 | 启动前确认 | Tracy 客户端已连接（`TRACY_ON_DEMAND=ON`，不连接就不采集） |
| 6 | 日志必备 | 开跑前记录 `D:\Documents\My Games\Skyrim Special Edition\SKSE\Template.log` 的当前大小，跑完取增量（本轮的**自证**在日志里，不在 Tracy 里） |

#### 采集 #12.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_21_12_19.tracy` |
| 文件大小 / 写入时间 | **45,613,432 B**（≈ 45.6 MB）；写入时间 **2026-09-21 12:19:13** |
| 实际加载的 DLL | `build\bin\RelWithDebInfo\Template.dll` = **780,288 B**，时间戳 2026-09-21 11:12:03，SHA-256 `44BEDCFF…F1D5`；与 §10.8 登记的 4a 指纹**逐字节一致**，且与 #11b 的 767,488 B **不同** → 确认本 trace 来自**含 4a 的构建** |
| 该 DLL 含有的 hook | 靶点 #1 / #2 / #3 / #5 / #6 / #7 / #8 + 靶点 2.1 / 2.5 + **S0b 的 slot 1/2 纯转发探针** + **S1：slot 0 的 `_requestItemInfo` 缓存替换体（`kAnswerFromCache = true`）** |
| 物品数 / 轮数 | **6441**（每轮 `AS::processEntry` 6441 次、第 1 轮全量路径 `RequestItemCardInfo` 6441 次，5 轮一致）/ **5**（轮数由日志 `4a: \`_dataProcessors[0]\` wrapped: … = CACHE …` 出现次数确认，同 #11b 的做法） |
| 日志增量 | `Template.log` 中 `4a` 相关 **25 行**（逐行清单与行数对账见 §12.2 末）：5× `= CACHE` 安装 + 1× `cache holds 0 / 0` + 4× `cache holds 6441 / 6440 certified` + 4× `previous round -- cache hits …` + 1× `validation compared 10 … 0 mismatch(es)` + 10× `validate: index 1..10 … identical` |
| 5 轮窗口（trace 时间轴） | 每轮起点 **3179.7 / 3182.9 / 3185.6 / 3188.2 / 3191.1 s**（轮间隔 ≈ 2.9 s），与日志 5 次安装时间戳一一对应；游戏 12:18:47.014 退出 |


#### 采集 #12.2 聚合数据（`tracy-csvexport <trace>`）

预测值来自 §10.8(9)，**与原登记并存、未回改**：

| zone | counts | total | mean | 对比 #11b |
|------|--------|-------|------|-----------|
| `RequestItemCardInfo`（2.1） | **6455**（预测 6455 ✅） | **418.54 ms**（预测 ≈ 445 ms） | **64.84 µs**（min 7.243 / max 338.81 / std 35.53） | #11b：32205 / 2216.77 ms / 68.83 µs |
| `FxDelegate::Callback[RequestItemCardInfo]`（2.5） | **6455**（须 **= 上一行** ✅） | **421.27 ms** | **65.26 µs**（min 7.344 / max 478.32） | #11b：32205 / 2224.72 ms / 69.08 µs |
| `AS::_requestItemInfo`（本步换成缓存替换体） | **32200**（预测 32200 ✅） | **567.61 ms**（#11b 2505.89 ms） | **17.63 µs** 全域均值；**逐轮 76.71 / 3.04 / 2.79 / 2.79 / 2.80 µs**（预测命中的 **5–15 µs** → 实测命中 **2.80 µs，优于预测下限**）；min 1.843 / max 1509.06 / std 35.12 | #11b：32200 / 2505.89 ms / 77.82 µs |
| `AS::processEntry`（S0b 探针） | **32205**（须 **= 32205** ✅） | **1319.35 ms** | **40.97 µs**（min 8.045 / max 9546.73 / std 132.56） | #11b：32205 / 1516.41 ms / 47.09 µs |
| `AS::processList`（S0b 探针） | **4**（预期仍为 4 ✅） | **10.60 ms** | **2.649 ms** | #11b：4 / 14.34 ms / 3.585 ms |
| `InventoryEntryData::GetValue`（回归检查） | **58061** | **20.38 ms** | **0.350 µs** | #11b：104725 / 47.69 ms / 0.455 µs |
| `InventoryChanges::GetItemCount` | **274** | 20.13 ms | 73.45 µs | #11b：390 / 37.37 ms |
| `InventoryChanges::GetInventoryWeight` | **318** | 1.70 ms | 5.35 µs | #11b：327 / 2.44 ms |

**嵌套关系成立，且与 #11b 完全镜像**：`AS::_requestItemInfo` ⊃ `RequestItemCardInfo`（探针在**外层**）—— 双指针逐事件核对（32200 个区间 × 6455 个事件）：**6450 内 / 5 外**，且 5 个「在外」的时刻 = **3179732.4 / 3182904.5 / 3185579.4 / 3188178.2 / 3191066.1 ms**，**恰好等于 5 轮的起簇时刻** → 就是每轮第 0 项（它走引擎原生函数）。#11b 是 32200 内 / **5 外** —— 同一个 5。

**2.5 − 2.1 边界**：**0.42 µs/次**（65.26 − 64.84），与 #9 的 0.17、#11a 的 0.23、#11b 的 0.25 同量级（亚微秒）→ 回归检查通过。⚠️ 含义已变：本轮 96 % 的 2.5/2.1 事件集中在第 1 轮**两条混合路径**上（外层探针路径 + 处理器内部 `forwardVerbatim` 的路径），不再是 #11b 的纯探针路径。

**单线程（H4 成立）**：五个 zone（`_requestItemInfo` / `RequestItemCardInfo` / `Callback[…]` / `processEntry` / `processList`）的 `thread` 列**全部 = 7**，无跨线程事件 → AS 与 C++ zone 可直接相减。⚠️ 线程编号**不能跨运行比**：同一条主线程在 #11b 是 10、在 #12 是 7。

**两个需要裁定的信号**（裁定见 §12.4 第 4/6 点）：

| 信号 | #11b | #12 | 初判 |
|------|------|-----|------|
| 主线程 `InventoryEntryData::GetValue` | **11670 次/轮**（5 轮 = 58350） | 第 1 轮 **11670**，第 2–5 轮 **13 / 1 / 1 / 1**（= 11686） | **无回归**：工作线程计数 **46375 = 46375 逐字相同**；主线程下降量 **46664** 精确等于「4 轮 × 11670 − 16」→ 只是少做了工作 |
| `AS::processEntry` 单次 | 46.75 µs（轮 2–5 均） | **39.78 µs**（轮 2–5 均，−14.9 %） | **方向有利、归因未完成**：count 未变、探针与 AS 体未改、8 个输入字段已验证一致 → 只能是间接效应 |

**本轮新增的自证日志**（`Template.log`，`grep '4a'`）—— 这是**不必打开 Tracy** 就能判定的部分：

```text
4a: `_dataProcessors[0]` wrapped: `_requestItemInfo` = CACHE (answered in C++), `processList` / `processEntry` = pass-through probes   <- 每轮 1 次 → 共 5 次
4a: cache holds 0 card slot(s), 0 of them certified                                  <- 第 1 轮（还没建缓存）
4a: cache holds 6441 card slot(s), 6440 of them certified                            <- 第 2-5 轮各一次
4a: previous round -- cache hits 0, full round trips 6440, uncacheable cards 0, serve failures 0      <- 第 1 轮的账
4a/validate: index 1: … ; index 10: all 8 card fields identical to the engine's own card   <- 共 10 条（index 1..10，逐条一行）
4a: previous round -- validation compared 10 cards field by field, 0 mismatch(es)     <- 1 条（第 2 轮的账）
4a: previous round -- cache hits 6430, full round trips 0, uncacheable cards 0, serve failures 0      <- 第 2 轮的账
4a: previous round -- cache hits 6440, full round trips 0, uncacheable cards 0, serve failures 0      <- 第 3、4 轮的账（各一条）
```

（**实测原文摘录**；5 组安装时间戳 = 12:18:29.364 / 32.536 / 35.211 / 37.809 / 40.697。**行数对账**：安装 5 + `cache holds` 5 + `previous round -- cache hits` **4** + `validation compared` 1 + `identical` 10 = **25**，与 `Select-String '4a' Template.log` 的 **25 行严格相等**。10 条 `identical` 全部落在 12:18:32.536/.537 —— 即第 2 轮头 10 项。第 5 轮的账无下一轮可打印，属正常。**本轮 `uncacheable cards` / `serve failures` 恒为 0** → §10.8(6) 的失败安全路径**一次都没触发**。）

#### 采集 #12.3 count 对账（判据 1–6，全中）

逐事件分簇（`tracy-csvexport -u`，按 > 1 s 间隔分簇；时间为 trace 起点后的 ms）。**预测列保留自 §10.8(9)，未回改**：

| 轮 | `RequestItemCardInfo` 预测 | 实测 | `AS::_requestItemInfo` 预测 | 实测 | `AS::processEntry` 预测 | 实测 |
|----|---------------------------|------|----------------------------|------|------------------------|------|
| 1 | 6441 | **6441** ✅ | 6440 | **6440** ✅ | 6441 | **6441** ✅ |
| 2 | **11**（index 0 + 10 次校验） | **11** ✅ | 6440 | **6440** ✅ | 6441 | **6441** ✅ |
| 3 | **1** | **1** ✅ | 6440 | **6440** ✅ | 6441 | **6441** ✅ |
| 4 | **1** | **1** ✅ | 6440 | **6440** ✅ | 6441 | **6441** ✅ |
| 5 | **1** | **1** ✅ | 6440 | **6440** ✅ | 6441 | **6441** ✅ |
| 合计 | **6455** | **6455** ✅ | **32200** | **32200** ✅ | **32205** | **32205** ✅ |

逐轮窗口（`AS::processEntry` 首→末；#11b 同口径由本轮重跑得到，见 §12.4 第 1 点）：

| 轮 | 分簇起点（ms） | `FxDelegate::Callback[…]` | 窗口 span #12 | 同口径 #11b | 逐轮差 |
|----|----------------|---------------------------|---------------|-------------|--------|
| 1 | 3179732.4 | **6441** ✅ | **832.7 ms**（建缓存，全价） | 872.0 ms | −39.3 ms |
| 2 | 3182904.5 | **11** ✅ | **323.7 ms** | 852.8 ms | **−529.1 ms** |
| 3 | 3185579.4 | **1** ✅ | **308.2 ms** | 839.9 ms | **−531.7 ms** |
| 4 | 3188178.2 | **1** ✅ | **309.2 ms** | 857.6 ms | **−548.4 ms** |
| 5 | 3191066.1 | **1** ✅ | **309.1 ms** | 826.4 ms | **−517.3 ms** |

| # | 判据 | 通过条件 | 实测 | 结论 |
|---|------|----------|------|------|
| 1 | 轮数 | `4a: … = CACHE …` 出现 **5** 次 | **5**（12:18:29.364 / 32.536 / 35.211 / 37.809 / 40.697） | ✅ |
| 2 | 缓存规模 | `cache holds 6441 card slot(s), 6440 of them certified`（**6441 − 6440 = item 0** 未入缓存，是机制而非缺陷） | 该行 **4 次** + 第 1 轮 `cache holds 0 / 0` 1 次 | ✅ |
| 3 | **数值一致（本轮的硬判据）** | `4a/validate: … identical …` **10 条**，且 **0** 条 `differs`、**0** 条 `rebuilding card … failed` | **10 identical / 0 differs / 0 failed**；`uncacheable cards 0`、`serve failures 0` **全程为 0** | ✅ |
| 4 | S4 基线未坏 | `AS::processEntry` = **32205**，逐轮 6441 | **32205**，逐轮 **6441** | ✅ |
| 5 | 引擎侧计数同意 | `RequestItemCardInfo` 与 `FxDelegate::Callback[RequestItemCardInfo]` **逐轮相等**，且逐轮符合上表 | 逐轮 **6441=6441 / 11=11 / 1=1 / 1=1 / 1=1**，合计 **6455 = 6455** | ✅ |
| 6 | 时间收益 | 每轮 `RequestItemCardInfo` 总量从 ≈ 443.4 ms → **≈ 1 ms 级**；单次 `AS::_requestItemInfo` 从 77.82 µs → **5–15 µs 量级**；每轮节省落在 **405–469 ms（50–58 %）** | 每轮 `RequestItemCardInfo` = **418.07 / 0.38 / 0.03 / 0.03 / 0.03 ms**；单次 `_requestItemInfo` 命中 **2.80 µs**；窗口级节省（轮 2–5 均）**−531.6 ms（−63.0 %）** | ✅ **机制命中、量级优于预测**（逐条裁定见 §12.4 第 2/3 点） |

**判据 3 的「对照严格性」核查**（这条判据要算数，必须先证明对照本身够狠 —— 三点都在本轮核实）：

1. **比的是「类型 + 精确值」**：`fieldMatches`（`src/ProfilingHooks.cpp:899`）对 `kNumber` 用 `==`（**无 epsilon**，注释写明是刻意的）、`kString` 用 `strcmp`、`kBoolean` 比精确布尔、`kAbsent` 要求新卡的 `GetMember` **失败**；两侧走同一条读取路径，所以 `identical` 不是描述方式造出来的；
2. **读取集边界已证**：`InventoryDataSetter.as` 全文件只读 **8 个不同字段**（`value`×4 / `weight`×4 / `damage`×4 / `effects`×3 / `armor`×2 / `type` / `stolen` / `poisoned`），与 `kCardFields`（`src/ProfilingHooks.cpp:571`）**逐字相同，没有第 9 个字段**；
3. **该对象只有两个访问点**：`ItemcardDataExtender.as:36`（`updateItemInfo` 写）与 `:56`（`processEntry` 读）—— 即缓存的全部 blast radius。`ItemCard.itemInfo`（`ItemCard.as:172`）是**另一个对象**、由**另一条** GameDelegate 路径写入，本步未触碰。

> 判据 6 的**预测区间**已写在 [phase4-design.md](./phase4-design.md) §10.8(9)：命中路径不是零成本（每次命中 `CreateObject` ×1 + `CreateString` ×1~2 + `SetMember` ×9），所以预期**低于** §5.3 的乐观值 501 ms。**低于 405 ms 是需要解释的信号**（先看单次 `AS::_requestItemInfo`），不是「效果更好/更差」的模糊判断。
>
> ⚠️ **实测落在该区间的上方**：未校正读数 −531.6 ms 超上限 **+62.6 ms**，单次命中 2.80 µs 低于下限 5 µs。两条都指向「命中比估计更便宜」，**已按预登记要求逐条解释**（§12.4 第 2 点），并给出受限的不确定性模型（§12.4 第 3 点）—— 其中一种模型下读数回到区间内。

#### 采集 #12.4 结论

**1）4a 实测收益**（稳态 = 轮 2–5 均值；#11b 侧用**同口径**的轮 2–5 均值，由本轮重跑 `tracy-csvexport -u` 得到 —— #11b 的聚合表原先只有 5 轮总量，没有 span）

| 指标 | #11b | #12 | Δ |
|------|------|-----|---|
| UIList 全量窗口（`AS::processEntry` 首→末 span） | 844.2 ms | **312.6 ms** | **−531.6 ms（−63.0 %）** |
| `AS::_requestItemInfo` 总量/轮 | 498.04 ms | **18.41 ms** | −479.6 ms（−96.3 %） |
| `AS::_requestItemInfo` 单次 | 77.33 µs | **2.80 µs** | −96.4 % |
| `RequestItemCardInfo` 总量/轮 | 443.35 ms | **0.028 ms** | −99.99 % |
| `AS::processEntry` 总量/轮 | 301.13 ms | 256.21 ms | −44.9 ms（−14.9 %，见第 4 点） |
| 主线程 `GetValue` 次/轮 | 11670 | ≈ **4** | −99.97 % |
| 5 轮窗口合计 | 4248.7 ms | **2082.9 ms**（1 轮全价 + 4 轮命中） | −2165.8 ms（−51.0 %） |

**第 1 轮（建缓存）没有变慢**：窗口 832.7 ms vs #11b 均值 849.7 ms（−2.0 %）；`AS::_requestItemInfo` 493.99 ms vs 501.18 ms（−1.4 %）→ **八字段深拷贝 + 认证的开销在测量噪声以下**（< ≈ 1.2 µs/次）。

**2）与 §10.8(9) 预测的偏差及原因**（预测未回改）

| 项 | 预测 | 实测 | 裁定 |
|----|------|------|------|
| 命中路径单次 `AS::_requestItemInfo` | 5–15 µs | **2.80 µs** | **优于预测下限**：每次命中 ≈ 2.80 µs 覆盖「AS→C++ 派发 + `CreateObject` ×1 + `CreateString` ×1 + `SetMember` ×9」≈ 11 次桥/GFx 调用 ⇒ **单次 GFx 操作 ≲ 0.25 µs**，是 §10.8(9) 估计值（0.5–1 µs）的 **1/2 ~ 1/4** |
| 每轮节省（窗口级） | 405–469 ms（50–58 %） | **−531.6 ms（−63.0 %）**，超上限 **+62.6 ms** | 两条量化原因：① 命中成本高估 ⇒ 少算 6440 × 2.2 µs ≈ **14 ms/轮**；② `AS::processEntry` 附带变快 **−44.9 ms/轮**（**未被预测到**，见第 4 点）。14 + 45 ≈ 59 ms ≈ 超出的 62.6 ms（余量在噪声内） |
| 每轮 `RequestItemCardInfo` 总量 | ≈ 1 ms 级 | **0.028 ms** | ✅ 比预测更低（第 2 轮 11 次 = 1 + 10 次校验；第 3–5 轮各 1 次 = 每轮第 0 项） |

**3）跨会话绝对时间的不确定性**（必须先摊开，否则第 2 点的偏差无法裁定）

同一份 trace 里有四个**做同样工作**的对照组：

| 对照 | 「同工作」的证据 | #12 / #11b |
|------|------------------|-----------|
| 第 1 轮单次 `RequestItemCardInfo`（C++ 引擎侧） | 第 1 轮**无缓存命中**，全价路径 | 0.943 |
| 第 1 轮主线程 `GetValue`（C++ 引擎侧） | **11670 = 11670**，次数逐字相同 | 1.000（次数）/ 0.959（单次） |
| 第 1 轮单次 `AS::processEntry`（AS 侧） | 引擎自产卡片，与 #11b 同价 | 0.971 |
| `AS::processList` 空转（AS 侧，纯 AS 循环基准） | 4 次 × 6441 次迭代 `continue`，**无子事件** | **0.739** ⚠️ |

前三个一致给出「**#12 的机器比 #11b 快 3–6 %**」；第四个（纯 AS 循环）给出 **26 %**，这一条**解释不了**。于是同一份数据有两种读数：

- 按 C++ 侧对照（≈ 0.95）折算 → 节省 ≈ **−502 ms（−59 %）**，略高于预测上限（+33 ms，可由第 2 点 ①② 解释）；
- 按纯 AS 循环对照（0.739）折算 → 节省 ≈ **−421 ms（−50 %）**，**落在预测区间 405–469 ms 内**；
- 表内 **−531.6 ms** 是**未校正**读数 —— 玩家实际感受到的就是这个数。

**裁定**：判据 6 **通过** —— 机制级预测（「命中 ≪ 往返」「命中单次 5–15 µs」）被证实且**优于**预测；但「节省数值落在 405–469 ms」这一条**不予判为精确命中**（跨会话不确定性 ≥ 5 %，AS 侧可能更大，已覆盖区间下沿；实测上沿超出）。这是**预测偏低**，不是失败，**未触发任何回退条件**（判据 3 无 `differs`）。

**4）`AS::processEntry` −14.9 %：方向有利，但归因未完成（记为待查项，不阻塞）**

事实：count 未变（32205，判据 4 ✅）；该探针与其 AS 体**本步未改**；它消费的就是那 8 个字段，而 8 个字段已验证逐字段一致（判据 3）→ 差异只能来自**间接效应**：

| 假设 | 机制 | 判别实验 |
|------|------|----------|
| **H12a**（领先） | 引擎卡片里的字符串成员是**未托管的裸 `const char*`**（`GFxValue::SetString` 的语义，`GFxValue.h:347`），而 `serveCard` 用 `GFxMovie::CreateString`（"managed by ActionScript runtime"，`GFxMovie.h:52`）→ AS 侧每次读该成员要多一次转换/分配；`processEntry` 每轮读 `effects` **3 次**（`:47/:75/:85`） | 在 `serveCard` 里把 `effects` 改用 `GFxValue::SetString`（指向 `g_cards` 内 session 寿命的 `std::string`，不会悬垂）→ 若命中轮 `processEntry` 均值回升到 ≈ 45 µs 则 H12a 成立 |
| **H12b** | 卡片对象的**内部形状 / 原型链**不同（引擎的可能挂在类原型上） | 上面的实验**无效**即为 H12b（该实验只动字符串表示） |
| **H12c** | 少掉 6440 次 `GameDelegate.call` + 建卡 ⇒ AS 堆压力 / GC 更小 | 上面实验无效，且需另设对照 |

**H12a 即使成立也不改变任何判据结论**（`CreateString` 是正确的所有权模型，命中路径的「便宜」是附带收益，**不应**改成 `SetString`），且 **4b-ii 不依赖这一条**（那一步会把 `processEntry` 也搬进 C++）→ **待查项，不阻塞**。

**5）人工 UI 抽查（§8.1 的原始要求）**

判据 3 是它的自动化替代（10 件 × 8 字段 × **类型 + 精确值**），本轮 **10/10 通过**；**人工目视抽查仍建议补做一次**（开背包，目视若干件的重量 / 价值 / 护甲 / 伤害 / 「附魔」「毒」标记），成本几分钟，用于交叉两个独立通道。⚠️ 抽查必须遵守 §12.0 的约束：**不拾取、不丢弃、不使用、不买卖**（本版缓存不做失效）。

**6）是否引入新的离群值 / 回归（对照 #11b）**

- ✅ **无新增量级跳变**：单次最大 `AS::_requestItemInfo` 1509.06 µs（第 1 轮，一次），#11b 为 613.76 µs；`AS::processEntry` max **9.547 ms** vs #11b 14.59 ms（更小）；命中轮的 `_requestItemInfo` max 仅 **16.6–17.8 µs**（第 2 轮 190.67 µs 来自 10 次校验对照的全价往返，属预期）；
- ✅ `InventoryEntryData::GetValue` 主线程 58350 → **11686**（**只随被消掉的建卡路径下降**；工作线程 **46375 逐字不变**）；
- ✅ `AS::processList` 仍为 **4 次** → 「空转、S3 不可测」的 §6.4 结论不变；
- ⚪ `InventoryChanges::GetItemCount` 390 → 274、`GetInventoryWeight` 327 → 318：**无轮次结构**（按 > 1 s 分簇后落在任意时点），绝对量 37.37 → 20.13 ms / 2.44 → 1.70 ms → 判为**会话抖动**（与 4a 无关）；
- ⚪ `AS::processEntry` −14.9 %：见第 4 点（待查项）。

**7）下一步**

- ✅ **判据 1–6 全中、判据 3 无 `differs`、`serve failures` / `uncacheable cards` 恒为 0** → **不回退**（`kAnswerFromCache` 保持 `true`），**进入 4b-ii**（消 S4 ≈ 303.3 ms/轮 ≈ 37.7 %，见 §6.4 / §8.1）；
- 📌 待查项（不阻塞）：H12a / H12b / H12c 的判别实验（第 4 点）—— 建议与采集 #13 合并做；
- 📌 可选：在 4a-2（缓存失效，§10.8(7)）里补一条八字段 `Kind` 分布的一次性直方图 —— 本轮只证明「10 件 × 8 字段全等」，**不知道各字段实际落在哪个 `Kind`**，因此 `kAbsent` 分支在真机上是否被执行过仍无从判断。


### 采集 #13 ——（阶段 4 / 4b-ii 前置）**已运行**（2026-09-21 13:24–13:25，5 轮开 / 关背包；判据 1 / 2 / 3 / 4 / 6 **通过**，判据 4 裁定 **H-GC 成立**，判据 5 预登记**口径缺陷**但读数在噪声内 —— 见 §13.5–§13.7）

> 📌 **本节已按「预测 / 实测并存、不回改预测」原则回填完成**（2026-09-21 13:24–13:25）。判据与裁决表的**预登记版**在 [phase4-design.md](./phase4-design.md) **§10.8(11)**，其**实测裁定与三处口径修正**在 **§10.8(12)**；本节结构 = 运行清单（§13.0）+ 采集条件（§13.1）+ **预登记判据与推理留档（§13.2–§13.4，均未回改）** + 实测数据与对账（§13.5–§13.7）。
>
> **本步性质**：#12 是**第一个改变行为**的步骤（命中缓存时不再问引擎），#13 相反 —— 它是**三条只写日志、不改行为**的诊断，合并进**一次改动、一次采集**。因此它的第一组判据（§10.8(11) 判据 1/2/5）是「证明它**没有**改变任何东西」，第二组（判据 3/4/6）才是新信息。

#### 采集 #13.0 运行前 checklist

| # | 检查项 | 值 / 做法 |
|---|--------|-----------|
| 1 | **DLL 指纹** | `build\bin\RelWithDebInfo\Template.dll` = **786,432 B**（≠ #12 的 **780,288 B**，**+6,144 B**）。⚠️ SHA-256 会随任何重编变化，**采集时必须用 `Get-FileHash -Algorithm SHA256` 实测**并与部署目录那份比对（见 §10.8(11) 与 §10.8(8) 的警告） |
| 2 | 部署 | 覆盖 `C:\Mod Organizer 2\mods\ModTest\SKSE\Plugins\` 下的 `Template.dll` **和** `Template.pdb`（部署后两处 SHA-256 必须一致） |
| 3 | 场景 | 与 #11b / #12 **完全一致**：同一「极大量物品」存档、**5 轮**纯开 / 关背包、**全程不滚动、不悬停** |
| 4 | 关键约束 | **全程不拾取、不丢弃、不使用、不买卖**（本版缓存仍不做失效 —— 4a-2，§10.8(7)） |
| 5 | 启动前确认 | Tracy 客户端已连接（`TRACY_ON_DEMAND=ON`，不连接就不采集） |
| 6 | 日志必备 | 开跑前记录 `D:\Documents\My Games\Skyrim Special Edition\SKSE\Template.log` 当前大小，跑完取增量 —— **#13 的全部新信息都在日志里，不在 Tracy 里**（Tracy 只用于判据 2/5 的时间与计数对账） |
| 7 | 开关状态 | `src/ProfilingHooks.cpp` 的 **`kExtraDiagnostics = true`**（`false` 会得到 **780,288 B** 的 #12 同尺寸构建，三条字符串全部不存在 → 诊断不会输出） |

#### 采集 #13.1 采集条件（实测回填）

| 项 | 值 |
|----|-----|
| trace 文件 | `D:\Documents\SSEMod\TracyLog\log_2026_09_21_13_25.tracy` |
| 文件大小 / 写入时间 | **40,040,827 B** / **2026-09-21 13:25:40** |
| 实际加载的 DLL | **786,432 B** / 时间戳 **2026-09-21 13:11:02** / SHA-256 **`AED34D6D644B08573BEEEE5F64085AD69603158222D180084B5D4648ED864B60`**（部署目录 `C:\Mod Organizer 2\mods\ModTest\SKSE\Plugins\Template.dll` 实测同值，`Template.pdb` 同步部署） |
| 物品数 / 轮数 | **6441** / **5** ✅（`entryList = array[6441]`；`AS::processEntry` 实测 **32205** = 6441 × 5） |
| 日志增量 | 见 §13.5 第 1 点（共 **119 行 / 13,068 B**）：5× `4a: cache holds … = CACHE` 安装 + 4× `previous round -- cache hits` + 1× `validation compared` + 10 行 `4a/validate:` + `Kind` 直方图 **1 + 8 = 9 行** + 1× `effects` 长度行 + **9 行** `4b/outlier:` |
| 5 轮窗口（trace 时间轴） | R1 **13:24:49.313 → 13:24:50.140**（span **826.5 ms**）/ R2 **13:24:52.447 → 13:24:52.776**（**329.7 ms**）/ R3 **13:24:54.964 → 13:24:55.277**（**313.2 ms**）/ R4 **13:24:57.429 → 13:24:57.741**（**312.7 ms**）/ R5 **13:25:00.423 → 13:25:00.735**（**311.8 ms**）。锚点 = 日志 `[13:24:49.323] 4b/outlier: processEntry #50` 对应的那条 trace 事件；逐轮以 **> 1 s 间隔**分簇（§13.5 第 3 点）。**R2–R5 的起点/终点与日志的 4 条安装行逐毫秒吻合**（如 R2 起点 = 日志 `[13:24:52.447]`），锚定可信 |

#### 采集 #13.2 预登记判据

**判据 1–6 与裁决表完整版见 [phase4-design.md](./phase4-design.md) §10.8(11)**（判据 1 DLL 指纹 → 2 结构自证未破 → 3 ② 直方图自洽 → 4 离群值归因 → 5 成本在噪声内 → 6 ③ 分母一致）。本节只留三条最容易读错的地方：

1. **判据 2 优先于一切**：`AS::processEntry` 仍须 **32205**、逐轮 6441，命中轮 `AS::_requestItemInfo` 单次仍 ≈ **2.8 µs**，`uncacheable cards` / `serve failures` **恒 0**，无 `differs`。三条诊断的设计意图就是「只加日志」，**判据 2 不成立则 3/4/6 的读数没有意义**。
2. **判据 4 的「0 条」不是好消息**：若一行 `4b/outlier:` 都不出现，结论是「5 ms 阈值对本机偏高 → 诊断未定」，**必须调阈值重采**，不能写成「离群值已消失」（#11b / #12 都存在 > 5 ms 事件，本机不出现反而说明阈值口径要复核）。
3. **判据 5 必须用轮内对照**：轮 2–5 的 `AS::processEntry` 每轮总量对照**同一份 trace 里**第 1 轮（全价、走引擎卡片），**禁止**拿跨会话绝对值比 —— §12.4 第 3 点已经量出跨会话离散 3–6 %（纯 AS 基准甚至 26 %），任何绝对值比较都无法裁定 +0.1 % 这个量级。

> 📌 **实测已回填（2026-09-21 13:24–13:25）**：逐条裁定见 **§13.6**，聚合数据与逐轮分簇见 **§13.5**，结论与下一步见 **§13.7**。**本节三条预登记提醒、以及 §13.3 的推理留档，均未回改** —— §13.6 记录了其中两处被实测**修掉**的地方（判据 5 的口径、§13.3 第 3 条「幅度递减」）。

#### 采集 #13.3 零采集阶段的推理留档（三条诊断的由来）

原来对 14.59 ms 离群值的读法是「某个 `formType` 分支在某些物品上很贵」。**只读 #11b 与 #12 两份 trace（各 32205 条 `AS::processEntry`）**、按轮分簇后，四条事实否掉了它：

| 观察 | #11b | #12 |
|------|------|-----|
| > 5 ms 事件**每轮恰好 2 条** | 5 轮各 2 条 | 5 轮各 2 条 |
| 位置（轮内序号，共 6441） | #11b 前一条 49–50 附近、后一条 6240 附近 | #12 前一条 35–50、后一条 6232–6242 |
| 幅度**逐轮递减** | 14.6 ms（首轮）→ 9.6 ms | 5.1–9.5 ms |
| **命中轮是否仍有** | —（无缓存） | **有**，且这些轮**一次卡片分配都没有** |

**漂移**是决定性的一条：同一份清单、同一次会话，`formType` 若固定则位置不可能挪动 15 项。四条合起来指向 **AS2 堆的确定性分配阈值触发 mark-sweep（暂停）**，而不是代码开销 —— 这也顺带解释了「幅度递减」（首轮建卡堆压力最大，之后堆逐渐整理完毕）。

**这一步因此只是给暂停定价**：303.3 ms/轮 的 S4 里有多少是 C++ 化**消不掉**的。判据 4 的裁决表把两种结果分开处理（H-GC 成立 → 暂停量必须写进 4b-ii 的验收判据；H-branch 成立 → 4b-ii 直接消掉并从该分支起做原型）。

#### 采集 #13.4 与 #12 遗留项的关系

| #12 遗留项 | #13 是否覆盖 |
|------------|--------------|
| §12.4 第 4 点的 H12a / H12b / H12c 归因 | **部分**：③ 是 H12a 的**低价证伪器**（`effects` 若几乎全空，则「每轮 6440 次托管字符串分配」站不住），但**不是闭环** —— 真正的判别实验仍是把 `serveCard` 的 `effects` 改用 `GFxValue::SetString`（那会**改变行为**，必须单独一轮、单独回退，不属于「只加日志」的 #13） |
| §12.4 第 7 点的「可选：补一条八字段 `Kind` 分布直方图」 | ✅ **本步吸收并升级**：原提议的「只记 `Kind`」扩成 `Kind` 直方图 + `effects` 长度两条，且合并进同一次改动 —— 该「可选」项**本节取代，不再单独排期** |
| §12.4 第 5 点的人工目视抽查 | ⚪ **不属本步**：仍建议单独补做一次（成本几分钟，用于交叉两个独立通道），必须遵守 §12.0 的约束（不拾取 / 不丢弃 / 不使用 / 不买卖） |
| 4a-2（缓存失效，§10.8(7)） | ⚪ **不属本步**：仍是对外可用的唯一硬阻塞（靶点 #4 的 AE 重定位需先反汇编），与 #13 无依赖关系 |

#### 采集 #13.5 实测：聚合数据（`tracy-csvexport <trace>`）与逐轮分簇

**1）日志增量（六组关键行）** —— #13 的 `Template.log` 增量共 **119 行 / 13,068 B**（首行 13:22:49.953 → 末行 13:25:06 `Game quitting`）。除下列六组读数外，增量里只有**每次启动都会出现的固定行**（hook 安装 12 行 + 生命周期 11 行 + `S0` / `S-1` 探针自证 + 5×「`_dataProcessors[0]` wrapped」安装行），**没有第二条新类别**。

| 组 | 日志行 | 实测读数 |
|----|--------|----------|
| 安装 | `4a: cache holds <n> card slot(s), <m> of them certified` | R1 安装（13:24:49.314）**0 / 0**；R2–R5 各 **6441 / 6440** |
| 计数 | `4a: previous round -- cache hits <h>, full round trips <t>, uncacheable cards <u>, serve failures <f>` | 打印 R1：**hits 0 / trips 6440 / 0 / 0**；打印 R2：**6430 / 0 / 0 / 0**；打印 R3、R4：**6440 / 0 / 0 / 0** |
| 校验 | `4a: previous round -- validation compared <n> cards field by field, <m> mismatch(es)` | 打印 R1：**10 / 0**（另有 10 行 `4a/validate: index 1..10: all 8 card fields identical to the engine's own card`，**无** `differs`） |
| ②③（一次性，**R2 安装时 13:24:52.447**） | 标题行 = `4a/4b: card field ... histogram over <n> certified card(s)` + **8 行**分字段桶计数（`4a/4b:   <字段>: absent=… undefined=… null=… bool=… number=… string=…`） | **6440** certified；8 行逐字见 §13.6「判据 3」表 |
| | ``4a/4b: `effects` string lengths: empty … / non-empty … / total … / max …`` | **1591 / 4849 / 584112 / 445** |
| ①离群值 | `4b/outlier: processEntry #<seq> of this round took <ms> ms -- formType <n>, formId <n>, baseId <n>`（行尾自带判读提示：`-1 = member absent; varying formType = heap pause, constant formType = expensive branch`） | **9 行**（位置 + 幅度见 §13.6「判据 4」表） |

**2）trace 侧聚合（整条 trace，`tracy-csvexport <trace>` 无过滤）**

| zone | count | total | mean | min | max |
|------|-------|-------|------|-----|-----|
| `AS::processEntry` | **32205** | **1327.26 ms** | 41.21 µs | 8.16 µs | **10.268 ms** |
| `AS::_requestItemInfo` | **32200** | 568.04 ms | 17.64 µs | 1.86 µs | 1.070 ms |
| `RequestItemCardInfo` | **6455** | 419.64 ms | 65.01 µs | 7.48 µs | 0.520 ms |
| `FxDelegate::Callback[RequestItemCardInfo]` | **6455** | 422.29 ms | 65.42 µs | 7.59 µs | 0.520 ms |
| `AS::processList` | **4** | 12.61 ms | 3.15 ms | 2.55 ms | 4.54 ms |
| `InventoryEntryData::GetValue` | 58061 | 20.96 ms | 0.36 µs | 0.14 µs | 26.4 µs |
| `InventoryChanges::GetItemCount` | 277 | 22.01 ms | 79.45 µs | 0.08 µs | 272.6 µs |
| `InventoryChanges::GetInventoryWeight` | 316 | 1.81 ms | 5.71 µs | 0.01 µs | 345.2 µs |

> **与 #12 的对照**：`AS::processEntry` 5 轮合计 **1327.26 ms**（#12 = **1319.35 ms**，§10.8(10)）→ **+7.91 ms = +0.60 %**；`AS::_requestItemInfo` = **32200**（同 #12）；`AS::processList` = **4**（同 #12 → §6.4「S3 不可测」的结论**不变**）；`AS::processEntry` 全部事件 `thread = 14`（#12 为 `thread = 10`，这只是 Tracy 的线程编号分配顺序差异，**同一线程**这一点不变，H4 仍成立）。

**3）逐轮分簇（`-u` 逐事件导出，按 > 1 s 间隔切分；每轮 count 恒 6441）**

| 轮 | `processEntry` 总量 | 单次均值 | 轮内最大 | `_requestItemInfo` 单次 | `RequestItemCardInfo` 次数 | ≥ 5 ms 事件（轮内序号 = 幅度） |
|----|---------------------|----------|----------|--------------------------|----------------------------|--------------------------------|
| 1 | **289.11 ms** | 44.89 µs | 8.99 ms | 76.63 µs（全价） | **6441** | #50 = 5.90；#6242 = 8.99 |
| 2 | **269.15 ms** | 41.79 µs | 8.53 ms | 3.154 µs | **11** | #50 = 7.92；#6242 = 8.53 |
| 3 | **256.80 ms** | 39.87 µs | 8.89 ms | **2.831 µs** | **1** | **#6241 = 8.89（仅 1 条）** |
| 4 | **256.45 ms** | 39.81 µs | 10.27 ms | **2.814 µs** | **1** | #50 = 5.54；#6242 = 10.27 |
| 5 | **255.75 ms** | 39.71 µs | 8.81 ms | **2.781 µs** | **1** | #50 = 6.45；#6242 = 8.81 |

**4）诊断自证：探针读数与 Tracy 读数逐条一致**

9 条离群值的**位置逐条相同**、**幅度逐条吻合** —— 探针（C++ `steady_clock`，`src/ProfilingHooks.cpp:624-627`）：5.8293 / 8.9422 / 7.8632 / 8.4855 / 8.8479 / 5.4847 / 10.2311 / 6.4058 / 8.7738 ms；Tracy 对应：5.90 / 8.99 / 7.92 / 8.53 / 8.89 / 5.54 / 10.27 / 6.45 / 8.81 ms。Tracy 读数**系统性高 +0.04–0.07 ms**，原因见 §13.6 判据 5 末尾的注（探针只计 `forwardVerbatim`，Tracy 的 zone 还含 `logProcessEntryOutlier` 那次文件写入）。

#### 采集 #13.6 count 对账（判据 1–6）

| # | 判据 | 预登记通过条件 | 实测 | 裁定 |
|---|------|----------------|------|------|
| 1 | DLL 指纹 | **786,432 B** | **786,432 B** / 13:11:02 / `AED34D6D…4B60` | ✅ |
| 2 | 结构自证未破 | `processEntry` **32205**（逐轮 6441）；`RequestItemCardInfo` 逐轮 **6441 / 11 / 1 / 1 / 1**；命中轮 `_requestItemInfo` 单次 ≈ **2.8 µs**；`uncacheable` / `serve failures` 恒 0；无 `differs` | **32205**（6441 × 5）；**6441 / 11 / 1 / 1 / 1**（合 **6455**）；**2.831 / 2.814 / 2.781 µs**；`0 / 0`；**无 `differs`**（10 identical） | ✅ **逐字命中** |
| 3 | ② 自洽 | 1 次标题行 + 8 行分字段行；每行六桶之和 = **6441** | 1 + 8 行；每行六桶之和 = **6440** | ✅（**分母口径**见下） |
| 4 | 离群值归因 | 每轮 2 条（±1），5 轮 ≈ **10 条** | **9 条**：R1 / R2 / R4 / R5 各 2 条、**R3 仅 1 条** | ✅ **H-GC 成立** |
| 5 | 成本在噪声内 | 轮 2–5 每轮总量 vs 同一 trace 轮 1 ≤ 1 % | 见下（**预登记口径不可判**） | ⚠️ **口径缺陷**，读数在噪声内 |
| 6 | ③ 的分母一致 | `empty + non-empty = 6441` | **1591 + 4849 = 6440** | ✅（**分母口径**见下） |

**判据 3 / 6 的分母口径偏差**：② 与 ③ 的分母是**认证卡数 6440**，不是条目数 6441 —— 差 1 = 每轮第 0 项（它跨过 `processEntry` 的时刻 `_requestItemInfo` 尚未被替换 ⇒ 不入缓存），与 §10.8(10) 的 `cache holds 6441 / 6440 certified` 是**同一个环测项**。预登记处把下标 0 算进去了，属**口径笔误**（判据表未回改，此处登记）。

**判据 3 的八字段 `Kind` 分布**（分母 6440，每行六桶之和 = 6440 ✅）：

| 字段 | absent | undefined | null | bool | number | string |
|------|--------|-----------|------|------|--------|--------|
| type / value / weight | 0 | 0 | 0 | 0 | **6440** | 0 |
| stolen | 0 | 0 | 0 | **6440** | 0 | 0 |
| effects | 0 | 0 | 0 | 0 | 0 | **6440** |
| armor | **4015** | 0 | 0 | 0 | 2425 | 0 |
| damage | **4055** | 0 | 0 | 0 | 2385 | 0 |
| poisoned | **4069** | 0 | 0 | **2371** | 0 | 0 |

- **`kAbsent` 被大量覆盖**：`armor` + `damage` + `poisoned` = **12,139 次缺失成员**，且同轮 `validation 10 / 0` ⇒「**不写回 = 复现缺失成员**」的 `applyField::kAbsent` 分支**实测正确**（从设计推理升级为实测覆盖）；
- 「某字段 `undefined` / `null` ≥ 1」这一行**未出现**（全 0）→ 该约束本轮**未被触发**（不可据此删除，只是没被覆盖到）；
- `effects` **恒为 `string`** ⇒ 字符串比较语义（`!= ""`）**不可降级为 bool**。

**判据 4 的读数（离群值归因）**：

| 轮 | 早离群值 | 晚离群值 |
|----|----------|----------|
| 1 | #50，`formType 27` / `formId 110588` / `baseId 110588`，**5.90 ms** | #6242，`formType 26` / 883293，**8.99 ms** |
| 2 | #50，27 / 110588，**7.92 ms** | #6242，26 / 883293，**8.53 ms** |
| 3 | **无**（< 5 ms 阈值） | **#6241**，26 / **883281**，**8.89 ms** |
| 4 | #50，27 / 110588，**5.54 ms** | #6242，26 / 883293，**10.27 ms** |
| 5 | #50，27 / 110588，**6.45 ms** | #6242，26 / 883293，**8.81 ms** |

> `formType` 的数值语义按 `RE::FormType`（`extern/CommonLibSSE-NG/include/RE/F/FormTypes.h:138`，0 基枚举）读：**26 = `Armor`（ARMO）、27 = `Book`（BOOK）**。两处离群值分别落在一本书与一件护甲上，与其在清单中的位置（#50 靠前、#6242 靠后）一致。

**裁定：H-GC 成立（离群值 = AS2 堆的 mark-sweep 暂停），H-branch（昂贵分支）不成立。** 三条证据都只与「暂停」相容：

1. **`baseId` 随位置漂移**：轮 3 的晚离群值落在 **#6241 / `baseId` 883281**，另外 4 轮是 **#6242 / 883293**。清单是静态的（判据 2 的 10/10 全等 + 命中稳定共同证明），所以下标 6242 每轮就是 883293 —— **轮 3 的暂停落在另一个物品上**。数据相关分支做不到「换个物品、同一个暂停」。
2. **早离群值在轮 3 整条消失**：同一本书（110588）、同一位置（#50），轮 3 跌破 5 ms。固定分支不会「有时 5.8 ms、有时 < 5 ms」。
3. **幅度逐轮乱跳**：5.54 → 10.27 ms（#12 同形：5.12 → 9.55 ms）。

⚠️ **预登记裁决表的第 2 行（「`formType` 恒定 → H-branch 成立」）口径过粗，被本轮实测证伪**：本轮 `formType` **恰好恒定**（27 / 26），若只按那一行判会得到 H-branch。原因是 `formType` 太粗（26 = ARMO 覆盖数千件）而**漂移只有 ±1 项**（相邻项同类型）⇒「`formType` 恒定」与「物品已被换掉」可以同时成立。**破平局的是 `baseId`**（超出预登记计划才加的字段）：`883293 → 883281` 的漂移才是不依赖具体物品的直接证据。正确判别列 = **「`formType` + `baseId` 是否随位置漂移 + 幅度是否波动 / 是否偶发低于阈值」**。

**同时被本轮修掉的还有 §13.3 四条事实里的第 3 条**：

| §13.3 的四条 | #13 是否复现 |
|--------------|--------------|
| > 5 ms 事件**每轮恰好 2 条** | ⚪ **每轮 1–2 条**（R3 仅 1 条）。#12 同样有一轮（R4）只有 1 条 → 判据 4 的「2 条（±1）」容差**是必要的** |
| 位置**近似固定但逐轮漂移** | ✅ 复现（#13 是 #50 / #6242，R3 为 #6241；**#11b 是 #49–#50 / #6240–#6242、#12 是 #35→#50 / #6232→#6242**）—— 三份 trace 的漂移区间互相重叠，**跨会话可复现** |
| 幅度**逐轮递减** | ❌ **未复现**（#13：8.99 → 8.53 → 8.89 → **10.27** → 8.81；#12：8.49 → 9.55 → 7.38 → 8.42 → 9.10）→ 那只是 **#11b 首轮建卡堆压力最大**造成的单次现象，**不能算作 H-GC 的证据** |
| **缓存命中轮仍然存在** | ✅ 复现（R2–R5 全为命中轮 —— 合计 **25750** 次命中 —— 仍有 8.53 / 8.89 / 10.27 / 8.81 ms） |

第 3 条被修掉**不削弱 H-GC**：它本来只是**辅助**证据；而 `baseId` 漂移 + 早离群值消失（**直接**证据）、位置漂移 + 命中轮仍在（**复现**证据）都指向同一个结论。

**判据 5（成本在噪声内）—— 预登记口径不可判，改用两条可达成对照**

| 对照 | 读数 | 判定 |
|------|------|------|
| **轮 3 / 4 / 5 内部离散**（同工作负载、纯命中、无校验项） | **256.80 / 256.45 / 255.75 ms**，极差 **0.41 %** | ✅ ≤ 1 % |
| 轮 2 vs 轮 3 | 269.15 vs 256.80 = **+4.81 %** | ⚠️ **结构性、非探针成本**：轮 2 含 10 次校验全价往返 + 建缓存后的堆状态；**#12 同形 +4.91 %**（264.97 vs 252.57） |
| #13 vs #12 逐轮（除探针外同构建） | R1 **−1.83 %** / R2 +1.58 % / R3 +1.67 % / R4 +1.20 % / R5 +0.74 %；合计 **+0.60 %**（1327.26 vs 1319.35 ms） | ⚠️ **符号在轮间翻转** ⇒ 差异由**跨会话离散（3–6 %，§12.4 第 3 点）+ GC 暂停落点**主导，**分辨不了 0.1 % 量级** |

- **预登记写的「轮 2–5 每轮总量 vs 同一 trace 第 1 轮 ≤ 1 %」本身不成立**：第 1 轮是**全价轮**（走引擎卡片），轮 2–5 是**命中轮**，两者工作负载不同 —— 实测轮 2–5 比轮 1 低 **6.9–11.5 %**，正是 §10.8(10) 已记录的 H12a / H12c 效应（−14.9 %）。**参照只能是命中轮之间的内部离散**（跨会话同轮被 3–6 % 离散排除）。
- **探针成本理论值**：每项 2× `steady_clock::now()` + 1 次比较 ≈ 55 ns ⇒ 6441 × 55 ns ≈ **0.35 ms/轮 ≈ 0.14 %**（与 §10.8(11) 预估的 +0.1 % 一致）。
- **裁定：通过（按可达成口径）** —— 轮内离散 **0.41 %**；预登记口径登记为**缺陷**，不是失败。归因结论不受影响（GC 暂停 ≈ 14 ms/轮 与 0.14 % 的探针成本差两个数量级）。

> **探针读数与 Tracy 读数之间 +0.04–0.07 ms 系统偏移的解释**：探针只包住 `forwardVerbatim(_original, a_params)`（`src/ProfilingHooks.cpp:625-627`），而 Tracy 的 `AS::processEntry` zone（`:610`）**还包含阈值命中后的 `logProcessEntryOutlier(...)` 文件写入**。即 Tracy 读数 =「AS 体 + 日志 I/O」，探针读数 =「只 AS 体」。9 次 × ≈ 55 µs ≈ **0.5 ms / 5 轮 = 0.1 ms/轮 ≈ 0.04 %**，对判据 2 / 5 无影响；但**引用离群值幅度时应以探针读数为准**（它不含日志成本）。

**判据 6 / ③ 的读数**：`empty **1591** / non-empty **4849** / total **584,112** chars / max **445**`。

- 分量自洽：**1591 + 4849 = 6440** ✅（分母同上）；
- 语义：**75 % 的卡有非空 `effects`**，非空项平均 ≈ **120 字符**、单条最长 **445 字符**，全轮合计 **≈ 584 KB** 字符串数据；
- 按 §10.8(11) 对 ③ 的措辞，这落在「**普遍非空且平均长度可观**」分支 ⇒ **③ 既未证伪也未证实 H12a**；H12a / H12b / H12c 的判别实验**仍然开放**（闭环仍是那条会改变行为的 `SetString` 替换实验，须单独一轮），**不阻塞 4b-ii**。

#### 采集 #13.7 结论与下一步

1. **判据 1 / 2 / 3 / 4 / 6 通过；判据 5 按可达成口径通过，其预登记口径登记为缺陷。** 三条诊断**只加日志、未改动行为**（判据 2 逐字命中），因此 #13 不改动任何既有回退契约（`kAnswerFromCache` 保持 `true`，本轮**未触发回退**）。
2. **判据 4 裁定 H-GC 成立** —— 离群值是 **AS2 堆的 mark-sweep 暂停**，不是 `formType` 分支开销。**这直接决定 4b-ii 的上界**：`S4 ≈ 303.3 ms/轮（37.7 %）` 里 **≈ 14 ms/轮 是 C++ 化不能保证消掉的暂停**（#13 = **14.26 ms/轮**、#12 = 13.76 ms/轮、#11b 全价语料 = 21.40 ms/轮；均为 ≥ 5 ms 事件合计）→ 扣除后可消上界 ≈ **289 ms/轮 ≈ 36 %**。但**不能反向承诺它一定残留**：C++ 化会改变 AS2 分配画像（`Translator.translate` / 字符串拼接从 AS 侧移走），暂停**可能缩小 / 不变 / 移位** ⇒ **必须在 B1 原型上实测**，并写进 4b-ii 的验收判据（「轮内仍有毫秒级尖峰 ≠ 没做干净」）。
3. **`formType` 不能单独当判别器**（裁决表第 2 行的口径缺陷，见 §13.6）：4b-ii 及以后任何「离群值归因」都应同时记录 `baseId`，并用 **位置是否漂移 + 物品是否随之改变 + 幅度是否波动** 三条一起判。
4. **`kAbsent` 实测覆盖成立**（**12,139** 次）⇒ 4b-ii 的 C++ `processEntry` 必须保留「成员不存在 ≠ 值是 `0` / `""`」的语义，以及 `effects` 的字符串比较语义。
5. **H12a / H12b / H12c 仍开放**（③ 不是闭环）；**人工目视抽查**（§8.1 原始要求）仍建议补做一次，且必须遵守 §12.0 的约束（**不拾取 / 不丢弃 / 不使用 / 不买卖**）。
6. **未回退、也未拆除诊断**：`kExtraDiagnostics = true` 的这份构建（**786,432 B**）已实测完毕；要回到与 #12 尺寸相同的构建，只需把该常量置 `false`（**780,288 B**，三条字符串全部不存在，已实测）。下一次采集是否继续保留 `true` 由 4b-ii 的需求决定 —— 三条诊断都是一次性读数，保留的代价是每项 ≈ 55 ns（≈ **0.14 %**）。

### 采集 #14 ——（阶段 4 / 步骤 S4 = 4b-ii）**已运行**（2026-09-21 20:22–20:23，5 轮开 / 关背包；判据 1 / 2 / 3 / 4 / 6 **通过**，判据 5 **未达承诺** —— 见 §14.5–§14.7）

> ⚠️ **本节基于 5.1 基线**（日志原文「`102` literals / `all 102 display`」）；运行版已确认为 SkyUI-Community **v6.11**，2026-09-22 按 v6.11 重转写并经真机验收 **4 轮 / 0 mismatch / 103 literals** —— 见 [phase4-design.md](./phase4-design.md) §10.10 与 [skyui-version-divergence.md](./skyui-version-divergence.md) §6.2。

> 📌 **本步性质**：这是**第二次改变行为**的步骤（第一次是 #12 的 4a）。`processEntry` 的 AS 体（`InventoryDataSetter.as:24-878`，879 行 / 125 `case` / 132 次 `Translator.translate`）被一份 **C++ 逐行复刻**顶替，仍然挂在 S0a/S0b 的同一个 `install()` 上。因此它的判据分两组：
>
> - **第一组（判据 1/2/3/6）证明「替换是忠实的、且失败路径没被走到」** —— 结构计数关系、复刻承接率、影子逐成员对照、回退契约；
> - **第二组（判据 4/5）才是新信息** —— 尖峰归因（老问题）与耗时（本步的主指标）。
>
> 预登记判据与推理留档在 [phase4-design.md](./phase4-design.md) **§9 / §6.4 / §10.8(11)(12)**（其中「可消上界 ≈ 289 ms/轮」写在 **§10.8(12) 第 6 点**）；本节的裁定、以及与原预测的偏差，见 §14.6 / §14.7 与 §10.9「真机验收」。

#### 采集 #14.0 运行前 checklist

| # | 检查项 | 值 / 做法 |
|---|--------|-----------|
| 1 | **DLL 指纹** | `build\bin\RelWithDebInfo\Template.dll` = **829,952 B**（≠ #13 的 **786,432 B**，**+43,520 B** = 复刻的静态体积；回退构建实测 **786,432 B**、符号表无 `pe::`）。⚠️ SHA-256 随任何重编变化，**采集时必须实测**并与部署目录那份比对 |
| 2 | 部署 | 覆盖 MO2 下 `SKSE\Plugins\` 的 `Template.dll` **和** `Template.pdb`（部署后两处 SHA-256 必须一致） |
| 3 | 场景 | 与 #11b / #12 / #13 **完全一致**：同一「极大量物品」存档、**5 轮**纯开 / 关背包、**全程不滚动、不悬停** |
| 4 | 关键约束 | **全程不拾取、不丢弃、不使用、不买卖**（缓存仍不做失效 —— 4a-2） |
| 5 | 启动前确认 | Tracy 客户端已连接（`TRACY_ON_DEMAND=ON`，不连接就不采集） |
| 6 | 日志必备 | 开跑前记录 `Template.log` 当前大小，跑完取增量 —— **判据 1/2/3 的全部证据都在日志里**；Tracy 只用于判据 4/5 的计数与耗时对账 |
| 7 | 开关状态 | `SSE_REPLICATE_PROCESS_ENTRY = 1`（`0` 会得到 **786,432 B** 的回退构建，`4b-ii:` 前缀的日志一行都不会出现） |
| 8 | **本次新增关注点** | `4b-ii: translation spike resolved -- route …` 必须出现**且只出现一次** —— 它同时是「复刻是否真的启用」和「spike 命中哪条路」的唯一证据 |

#### 采集 #14.1 采集条件（实测回填）

| 项 | 值 |
|----|-----|
| trace 文件 | `D:\Documents\SSEMod\TracyLog\log_2026_09_21_20_23.tracy` |
| 文件大小 / 写入时间 | **39,818,005 B** / **2026-09-21 20:23:39** |
| 实际加载的 DLL | **829,952 B** / 时间戳 **2026-09-21 20:06:58** / SHA-256 **`E3654CD3AC885B40DFE4A858A8BF23F7AAE301D450B9A6F20BC4E89DB57D564E`**（部署目录 `C:\Mod Organizer 2\mods\ModTest\SKSE\Plugins\Template.dll` 与本机构建产物实测同值，`Template.pdb` 同步部署） |
| 物品数 / 轮数 | **6441** / **5** ✅（`entryList = array[6441]`；`AS::processEntry` 实测 **32,205** = 6441 × 5，`AS::_requestItemInfo` **32,200** = 32,205 − 5） |
| 日志增量 | 见 §14.5 第 1 点（共 **8 组 × 5 轮** 的 `4b-ii:` 行）：1× spike 路由行 + 1× `all 102 display strings resolved` + 1× 安装横幅（`4a/4b:`）+ 每轮 1× `previous round -- processEntry calls …` + 每轮 1× `previous round -- shadow comparison covered …` + 每轮 10 行 `4b-ii/validate:` + **10 行** `4b/outlier:` |
| 5 轮窗口（trace 时间轴，`InventoryMenu opened → closed`，由 `-m` 消息时间戳算出） | R1 **1225.34 ms** / R2 **1054.12 ms** / R3 **865.99 ms** / R4 **847.47 ms** / R5 **896.93 ms**。⚠️ **本轮窗口比 #12/#13（826.5 / 329.7 / 313.2 / 312.7 / 311.8 ms）长 2.5–2.9×，但这不是回归** —— 见 §14.6 第 3 点：**同一份对照里所有 zone 都下降、无一个上升**，长出来的部分是**用户关闭菜单的反应时间** |


#### 采集 #14.2 预登记判据（未回改）

**判据 1–8 与裁决表的完整预登记版见 [phase4-design.md](./phase4-design.md) §10.9 开头与 §8.1 的 S2′ 行**，本节只留三条最容易读错的地方：

1. **判据 2 优先于一切**：`AS::processEntry` 仍须 **32,205**（逐轮 6441），影子对照必须 **0 differs**。复刻是新代码，**任何计数关系或对照失败都先判「复刻不忠实」**，耗时读得再漂亮也作废。
2. **`forwarded` / `declines` / `apply failures` 三个计数必须全 0** —— 它们同时覆盖「fail-closed 有没有被走到」和「写入有没有中途失败」。**非 0 不是 bug，而是复刻覆盖面不足**：`declines` 非 0 说明 `gather` 遇到了没建模的成员类型（日志会点名是哪个成员），`apply failures` 非 0 说明 `CreateString`/`SetMember` 失败过。两者都必须查，不能只看「能跑」。
3. **判据 4 的「尖峰」要按幅度与位置一起读**：本轮开始 `processEntry` 已由 C++ 承担，所以**尖峰不再可能是「某个分支很贵」**（那已被搬走）—— 剩下的唯一解释是 AS2 堆暂停。**必须核对「位置漂移 + `formId` 随之改变 + 幅度波动」三条同时成立**，否则不能写成 H-GC（#13 的裁决表第 2 行就是只凭 `formType` 判而留下的口径缺陷）。

#### 采集 #14.3 零采集阶段的预期（留档，未回改）

1. 复刻**忠实**：判据 1/2/3 应全中，`forwarded`/`declines`/`apply failures` 全 0。
2. `AS::processEntry` 应降到 **≈ 14 ms/轮**（= 只剩 AS2 堆暂停）：依据是 §10.8(12) 第 6 点的「可消上界 ≈ 289 ms/轮」。
3. 尖峰数量与幅度应与 #13 同形（**每轮 2 个、合计 ≈ 14 ms/轮**），且**可能缩小 / 不变 / 移位**（§10.8(12) 第 6 点已预登记「不能反向承诺它一定残留」）。
4. 窗口跨度（`InventoryMenu opened → closed`）应比 #12/#13 短约 200 ms/轮（复刻消掉的部分）。

> ⚠️ 上列第 2 条被实测**证伪**（实际 **80.45 ms/轮**），第 4 条也被实测**反向**（窗口反而变长，因为那是用户时间）。**两条都保留在此处不回改** —— §14.6 与 §10.9「真机验收」记录了偏差与修正后的成本模型。

#### 采集 #14.4 日志原文（关键行，`Template.log` 20:23:05–20:23:17）

```
[20:23:05.999] 4b-ii: translation spike resolved -- route _global.skyui.util.Translator + Invoke(translate), 102 literals to resolve
[20:23:05.999] 4b-ii: all 102 display strings resolved; `processEntry` now runs in C++
[20:23:05.999] 4a/4b: `_dataProcessors[0]` wrapped: `_requestItemInfo` = CACHE (answered in C++), `processList` = pass-through probe, `processEntry` = 4b-ii replica (answered in C++)
[20:23:06.000] 4b-ii/validate: formType 27 formId 109797: all 10 predicted member(s) identical to the AS body's own result
…（本轮共 10 行，formType 27 ×4 / 32 ×4 / 45 ×1 / 26 ×1，成员数 10/10/10/10/9/9/8/9/9/15）
[20:23:06.007] 4b/outlier: processEntry #35 of this round took 5.2844 ms -- formType 27, formId 984087, baseId 984087
[20:23:06.582] 4b/outlier: processEntry #6230 of this round took 8.6755 ms -- formType 26, formId 883320, baseId 883320
[20:23:08.794] 4b-ii: previous round -- processEntry calls 6441, replicated in C++ 6431, shadow-validated (forwarded on purpose) 10, forwarded 0, declines 0, apply failures 0
[20:23:08.794] 4b-ii: previous round -- shadow comparison covered 10 item(s) / 99 member(s), 0 mismatch(es)
…（第 2–5 轮同形，共 10 个 outlier、50 行 validate、5 组 previous round）
```

**读法**：5 组 `previous round` 行**逐字相同**，只有 outlier 的序号与 `formId` 在漂移。`forwarded 0 / declines 0 / apply failures 0` ⇒ **fail-closed 一次都没被走到**，即 `gather` 的 22 个成员全部落在已建模的类型集合内，且 6441 次 `SetMember`/`CreateString` 无一失败。

#### 采集 #14.5 Tracy 数据（聚合 + 与 #13 的逐 zone 对照）

**`AS::processEntry` 聚合（`tracy-csvexport` 默认模式）**

| 采集 | total_ns | counts | mean_ns | min_ns | max_ns |
|------|----------|--------|---------|--------|--------|
| 09-20 18:35（#11b，S0b） | 1,516,406,195 | 32,205 | 47,086 | 8,717 | 14,592,507 |
| 09-21 13:25（#13，4a） | 1,327,259,222 | 32,205 | 41,212 | 8,156 | 10,267,564 |
| **09-21 20:23（#14，4a+4b-ii）** | **402,264,854** | **32,205** | **12,490** | **4,028** | **8,715,442** |

**逐 zone 对照（#14 vs #13；同构建除 4b-ii 外一致）**

| zone | #14 总 ms | #13 总 ms | Δ ms | count #14 / #13 |
|------|-----------|-----------|------|------------------|
| `AS::processEntry` | **402.26** | **1327.26** | **−924.99** | 32,205 / 32,205 |
| `AS::_requestItemInfo` | 536.58 | 568.04 | −31.46 | 32,200 / 32,200 |
| `RequestItemCardInfo` | 400.90 | 419.64 | −18.74 | 6,455 / 6,455 |
| `FxDelegate::Callback[RequestItemCardInfo]` | 404.18 | 422.29 | −18.11 | 6,455 / 6,455 |
| `InventoryEntryData::GetValue` | 19.84 | 20.96 | −1.12 | 58,061 / 58,061 |
| `InventoryChanges::GetInventoryWeight` | 1.68 | 1.81 | −0.12 | 286 / 316 |
| `InventoryChanges::GetItemCount` | 18.86 | 22.01 | −3.14 | 258 / 277 |
| `AS::processList` | 10.29 | 12.61 | −2.32 | 4 / 4 |

**判据 1（结构自证）分量核对**

| zone | counts | 期望 | 判定 |
|------|--------|------|------|
| `AS::processEntry` | **32,205** | = 全量 `RequestItemCardInfo` 路径 = 6441 × 5 | ✅ |
| `AS::_requestItemInfo` | **32,200** | = 32,205 − 5（每轮第 1 次走旧函数对象） | ✅ |
| `RequestItemCardInfo` | 6,455 | 5（每轮第 1 项）+ 6,450（**悬停 / 事件路径**，不经探针） | ✅ 与 #11b / #13 同形 |
| `AS::processList` | 4 | 轮数 − 1（安装发生在它自己的循环体内） | ✅ |

#### 采集 #14.6 判据逐条裁定

| # | 判据 | 实测 | 判定 |
|---|------|------|------|
| 1 | 结构自证未被破坏 | `AS::processEntry` **32,205** == 全量 `RequestItemCardInfo` 路径；`AS::_requestItemInfo` **32,200 = 32,205 − 5**；`AS::processList` **4** | ✅ |
| 2 | 复刻忠实 + 失败路径未被走到 | 逐轮 `replicated 6431 / validated 10 / forwarded 0 / declines 0 / apply failures 0`（5 轮全同）；影子对照 **50 件 / 495 成员 / 0 mismatch** | ✅ |
| 3 | 影子对照（§8.1「UI 数值与 skyui 原版逐项一致」的机器化） | 每轮 10 件、成员数 8–15，逐成员 `Kind` + 精确值全等；覆盖 formType **27 / 32 / 45 / 26** 四条不同分支路径 | ✅ |
| 4 | 毫秒级尖峰归因 | **每轮恰好 2 个** ≥ 5 ms 事件，序号 **#35 / #49 / #50 / #49 / #49** 与 **#6230 / #6239 / #6240 / #6239 / #6239**（**位置漂移**）；`formType` 恒 27 / 26 但 `formId` **逐轮改变**（984087→110551→110588→110551→110551、883320→883238→883317→883238→883238）；幅度 **13.96 / 13.84 / 15.40 / 13.25 / 15.03 ms**（**波动**）。三条同时成立 ⇒ **AS2 堆 mark-sweep 暂停**。平均 **14.30 ms/轮**（#13 = 14.26、#12 = 13.76） | ✅ **H-GC 再次成立，且 C++ 化未使其缩小或移位** |
| 5 | 耗时达到承诺 | `AS::processEntry` **265.45 → 80.45 ms/轮**（相对 S0b：303.28 → 80.45）。**未达到「≈ 14 ms/轮」的预测**，也**低于「消 230–270 ms」承诺的下沿**（实测消 222.83 ms，相对 S0b） | ⚠️ **未达标**（结论与修正见 §14.7） |
| 6 | 回退契约未被破坏 | 开关为编译期宏（`SSE_REPLICATE_PROCESS_ENTRY`），关掉时 `ProcessEntryReplica.cpp` 编成空 TU、符号表零 `pe::`（**786,432 B**）；本轮**未触发任何回退路径** | ✅ |

**#14.6.x 两处必须写下来的口径**

1. **窗口跨度本轮变长，且这不是回归。** R1–R5 = **1225.34 / 1054.12 / 865.99 / 847.47 / 896.93 ms**，比 #12/#13 的 **826.5 / 329.7 / 313.2 / 312.7 / 311.8 ms** 长 2.5–2.9 倍。但 §14.5 的逐 zone 对照显示：**每一个 zone 都下降、没有一个上升**，且 `AS::processEntry` / `AS::_requestItemInfo` / `RequestItemCardInfo` 的 count 逐项相等 ⇒ 工作负载相同、总工作量减少。结论：**长出来的那部分是用户关闭菜单的反应时间**。`InventoryMenu opened → closed` 是**含用户行为的量**，跨会话比较它必须先把用户时间扣掉（或改用同一 trace 内的 zone 总量）。这与 §12.4 第 3 点「跨会话离散 3–6 %，纯 AS 基准甚至 26 %」是同一类陷阱的**反面**。
2. **`kExtraDiagnostics` 的停止计时现在包住了 C++ 路径**，因此 `4b/outlier:` 的读数含义变了：不再是「AS 体很贵」，而是「**这一次 handler 调用**很贵」。本轮它抓到的两个事件幅度（5.3–8.7 ms）与 #13 的（5.2–8.9 ms）同量级，而 `AS::processEntry` 的**均值**从 41.2 µs 降到 12.5 µs ⇒ **尖峰与均值已经解耦**，这正是「毫秒级尖峰 ≠ 没做干净」的实测形态。

#### 采集 #14.7 结论与下一步

1. **判据 1 / 2 / 3 / 4 / 6 通过；判据 5 未达标。** 复刻是**忠实**的：32,205 计数关系成立、5 轮 50 件 / 495 成员影子对照 **0 differs**、`forwarded` / `declines` / `apply failures` **全 0**（fail-closed 一次都没被走到）。本轮**未触发任何回退路径**。
2. **判据 4：H-GC 再次成立，而且这次是「加强版」** —— `processEntry` 已搬到 C++，尖峰**不可能**再是分支开销，唯一解释就是 AS2 堆暂停。三条共变（位置漂移 / `formId` 随之改变 / 幅度波动）同时成立，平均 **14.30 ms/轮**，与 #13（14.26）几乎相同 ⇒ **暂停由 entry 对象与列表重建驱动，不由 `processEntry` 自身的分配驱动**（把 `Translator.translate` 与全部字符串拼接搬走**并没有**缩小它）。§10.8(12) 第 6 点预登记的「可能缩小 / 不变 / 移位，不能反向承诺」中，实测落在「**不变**」。
3. **判据 5 的偏差与修正后的成本模型**：`AS::processEntry` **265.45 → 80.45 ms/轮**（消 **185.0 ms/轮，−69.7 %**；相对 S0b 消 222.83，= 可消上界 289 的 **77 %**）。**原预测「可消到只剩 ≈ 14 ms 堆暂停」被证伪**，原因是它把 289 ms 整体当成「解释器开销」，而其中 **≈ 66 ms 其实是 `GetMember` / `SetMember` / `CreateString` 的 GFx 属性访问成本** —— AS 原版也要碰同样这些成员，**任何忠实复刻都绕不开**。修正后的分解：`80.45 = 14.30（暂停）+ 66.15（复刻自身 GFx 访问 = 10.3 µs/件）`。
4. **下一步只能从「少碰成员」入手**（不是「再搬解释器」），两条候选，收益目前均为**估计值**，按本项目惯例应**先测再改**：
   - ① **显示字符串 managed 化**：`prepare` 时把 102 个译文字符串建成 `GFxValue` 并在 `apply` 复用，消掉每件 ~5–15 次 `CreateString`（**这一项是赢过 AS 原版**：AS 每次 `translate` 都新分配一个字符串）；它同时直击 H12a，是可闭环的一条；
   - ② **`gather` 分支化**：只读当前 `formType` 需要的那组成员，把读量砍半；代价是放宽 §10.9「decline 是对象的性质」为「decline 是分支的性质」。
   - 建议的测量顺序：先用 `tracy-csvexport -u` 把 80.45 ms **按轮分簇**，再把单件 12.49 µs 拆成 read / write / `CreateString` 三块（三块各自可估：read ≈ 24 次、write ≈ 10–18 次、`CreateString` ≈ 5–15 次），据此决定做 ① 还是 ①+②。
5. **口径警告（沿用 #13 的结论并强化）**：`InventoryMenu opened → closed` 窗口跨度**不能跨会话比较**（含用户反应时间），本轮就是反例；**判据 5 必须用同一 trace 内的 zone 总量**。
6. **人工目视抽查**仍建议补做一次（本步改变了显示字符串的**来源**：从 AS 侧 `Translator.translate` 改为 C++ 查表 + `CreateString`；机器对照已 0 differs，目视是独立证据）。必须遵守 §12.0 的约束（**不拾取 / 不丢弃 / 不使用 / 不买卖**）。

### 采集 #15 ——（阶段 4 / 4b-ii Tier 1 的**测量前置**）**已运行**（2026-09-21 23:52；不设验收判据，产出是「否决原计划、改做 branch-selective」）

> **本节的性质**：项目里第一次**先测量再改**的采集（执行 §14.7 第 4 点建议的顺序）。它不产生新的验收判据，而是**换掉了下一步**，故单独成节。
> **证据来源**：结论取自成果提交 **`422452d`** 的说明与 `src/ProcessEntryReplica.cpp:489-501` 的引用注释（均为已提交事实）。**逐事件 CSV 明细尚未回填** —— 见 §15.5。

#### 采集 #15.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_21_23_52.tracy`，**40 856 876 B** / 写入 **2026-09-21 23:52:59**（**推定**：早于成果提交 `422452d`（2026-09-22 00:06）**14 分钟**，与「先采样 → 后改码 → 再提交」的因果一致；文件解析确认待 `tracy-csvexport`，见 §15.5） |
| 实际加载的 DLL | 与 §14 同一 ON 产物（**829 952 B**）：本轮测的是**现状的构成**，不是新构建 |
| 开关状态 | `SSE_REPLICATE_PROCESS_ENTRY = 1` |
| 目的 | 把 `AS::processEntry` 的单件 **12.49 µs** 拆成 read / write / `CreateString` 三块，为「做 ① 还是 ①+②」提供依据（§14.7 第 4 点） |

#### 采集 #15.2 测得的核心数字

| 量 | 实测 | 含义 |
|----|------|------|
| 单件耗时分布 | **4–20 µs、平滑、无昂贵分支** | 分支本身不是成本，成本是**每次 GFx 属性操作** |
| 单次 GFx 属性操作（`GetMember` / `SetMember`） | **≈ 0.12 µs** | 由上面的分布反推的价格，本步全部推论的基础 |
| `gather` 的固定 **25 成员**读 | **≈ 18 ms/轮** | **每件都付**，与物品形状无关 |
| 某分支实际需要的成员数 | **7（key）– 13（armour）** | 相比固定 25，**多读 12–18 个** |

#### 采集 #15.3 结论：原计划被否决，换成 branch-selective

1. **采纳**：`gather` **分支化** —— 只读当前 `formType` 那条分支需要的成员。收益 **≈ 9–14 ms/轮**。
2. **否决**：**keyword batching**（原计划的下一步）—— 它只值 **≈ 4–5 ms**，且可能**打不过** 39 次直接 `GetMember`，收益不足以支付复杂度。
3. **代价与补偿**：`gather` 的「decline 是**对象**的性质」放宽为「decline 是**分支**的性质」。因为「覆盖全分支」不再由固定读取集保证，影子采样加了 **stratified 通道**（每轮每个 `formType` 分支转发 1 件），叠加在原有的**定位前 10 件**之上；每轮日志报出**已覆盖分支数**，使盲区**可见**而非隐含。

#### 采集 #15.4 去向

- 落地产物：提交 **`422452d`**（`gather` = 「common 集 + 按 `formType` 的 extras 集」，每件少读 12–18 次；`useSound.formId` 移入独立两步 helper，因为只有 POTION 到达它）。
- 该次提交时的构建：**ON 833 024 B / OFF 786 432 B**，均 **0 warning / 0 error** ⇒ 回退开关仍成立。
- 对验收判据：**不新增判据**。改变的是「下一步做什么」，不是验收标准。

#### 采集 #15.5 未回填项（如实登记）

> **2026-09-23 状态更新**：本节第 3 条登记的两份「未登记采集」**均已补齐** —— 分别为 **采集 #16**（`log_2026_09_22_11_29.tracy`，性质查明 = **`gather` 分支化后的首次采样**）与 **采集 #17**（`log_2026_09_22_23_32.tracy`，v6.11 重转写首测）。**第 1 条仍缺。**

1. **逐事件 CSV 明细**：三块拆分（read / write / `CreateString`）**尚未从 trace 导出**（**聚合**侧已在 **采集 #16** 回填）。工具：`extern/TracyProfiler/release/tracy-csvexport.exe`（**本机已有现成二进制**，0.14.1 / `30997d5`，与 GUI 同源，**无需自行编译**），用法 `extract [OPTION...] <trace file>`，`-f` 过滤 zone 名（**子串**匹配）、`-u` 逐事件、`-t` 截尾均值，或 Tracy GUI。
2. **采集文件确认**：`log_2026_09_21_23_52.tracy` 是**推定**的 #15（判据见 §15.1）—— 该推定已在 **采集 #16** 获得**独立佐证**：其 `AS::processEntry` **mean = 11,730 ns / min = 3,857 ns**，与 #14（**12,490 ns** / **4,028 ns**）同属「分支化前」形态，而与 #16（**10,535 ns** / **2,204 ns**）清晰分野。
3. **相邻的两次未登记采集**（**均已登记，见下方 #16 / #17**）：
   - `TracyLog/log_2026_09_22_11_29.tracy`（09-22 11:29，**41 434 552 B**）：**性质已查明** —— 它是 `gather` 分支化（提交 `422452d`）后的**首次采样**，现为 **采集 #16**，并作为该次优化的性能验收依据；
   - `TracyLog/log_2026_09_22_23_32.tracy`（09-22 23:32，**36 703 919 B**）= **v6.11 重转写首测**，其结果已登记在 `phase4-design.md` §10.10 与 `skyui-version-divergence.md` §6.2，**本文件已补齐对应章节**（**采集 #17**）。

### 采集 #16 —— 2026-09-22 11:29（**`gather` 分支化后首次采样**；补登记 §15.5 登记的「性质待定」那次）

> **本节的性质**：§15 是分支化的**测量前置**（先测后改，产出「否决原计划、改做 branch-selective」）；本节是**改后的实测回填** —— 两节合起来才是「预测 → 改动 → 实测」的完整闭环。本节同时补上 §15.5 第 3 条登记的第一份未登记采集，并给出**该次优化的性能验收结论**。

#### 采集 #16.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_22_11_29.tracy`，**41,434,552 B** / 写入 **2026-09-22 11:29:33** |
| 实际加载的 DLL | **推定为提交 `422452d` 的 ON 产物 = 833,024 B**（该字节数见 §15.4；本次运行**未留下** `Template.log`，故只能推定）。**推定依据三条**：① 时间落在 `422452d`（09-22 00:06:52 提交）之后、v6.11 重转写首测（09-22 23:32）之前；② 该 trace 里 `AS::processEntry` 的 source line = **`:657`**（复刻后的探针位置，与 #14 / #15 同），排除复刻前构建；③ **`min_ns` = 2,204**，远低于分支化前的 **3,857**（#15），又接近 v6.11 首测的 **2,304** —— `min` 是「最便宜那件」的成本，只有分支化（把 key 分支由固定 25 读压到 7 读）才能把它压低四成 |
| 物品数 / 轮数 | **6441** / **5** ✅（`AS::processEntry` = **32,205** = 6441 × 5，与 #14 / #15 / #17 **完全相等**） |
| 场景 | 同 #11b–#15：同一「极大量物品」存档、5 轮纯开 / 关背包、**不滚动不悬停**（`RequestItemCardInfo` 6,455 次与 #14 / #15 **全等**，佐证场景一致） |
| 开关状态 | `SSE_REPLICATE_PROCESS_ENTRY = 1` |

#### 采集 #16.2 四份 trace 同口径对照（**本节正文**）

全部经 `tracy-csvexport` 默认聚合模式；四份 trace 的 **counts 全等**（`processEntry` 32,205 / `RequestItemCardInfo` 6,455 / `_requestItemInfo` 32,200 / `processList` 4），故构成**同存档、单变量**对照：

| zone（mean） | #14 (09-21 20:23) | #15 (09-21 23:52) | **#16 (09-22 11:29)** | #17 (09-22 23:32) |
| --- | --- | --- | --- | --- |
| 构建 | 829,952 B | 829,952 B | **833,024 B（推定）** | 838,656 B |
| 相对上一份的增量 | — | — | **+ 字符串 managed 化 + `gather` 分支化** | + v6.11 重转写 |
| `AS::processEntry` | 12,490 ns | 11,730 ns | **10,535 ns** | 10,748 ns |
| ↳ `min` | 4,028 ns | 3,857 ns | **2,204 ns** | 2,304 ns |
| ↳ 折合 ms/轮 | 80.45 | 75.55 | **67.86** | 69.23 |
| `RequestItemCardInfo`（**对照，与复刻无关**） | 62,107 ns | 65,286 ns | 63,457 ns | 62,015 ns |
| `AS::_requestItemInfo`（对照） | 16,663 ns | 17,287 ns | 16,974 ns | 16,518 ns |

**先定噪声底，再读收益。** `RequestItemCardInfo` 是**引擎侧**的取卡往返（`src/ProfilingHooks.h:529` 处的 hook），C++ 复刻的改动**不可能**影响它，因此它的逐份波动就是本轮的**噪声底**：

- **同一构建内**（#14 → #15，均为 829,952 B）：**+5.1 %**（62,107 → 65,286）
- **跨构建**（#15 → #16 → #17）：**−2.8 %** / **−2.3 %**

即运行间噪声量级 **±3–5 %**。于是：

| 量 | 值 |
|---|---|
| 分支化**前**基线（829,952 B，两次采样） | **12,490 / 11,730 ns** → 均值 **12,110 ns**（≈ 80.45 / 75.55 ms/轮） |
| 分支化**后**（#16） | **10,535 ns**（≈ **67.86 ms/轮**） |
| **降幅** | **−1,575 ns/件（−13.0 %）→ −10.1 ms/轮** |
| 噪声底 | ±3–5 % ⇒ 降幅为噪声的 **2.6–4.3 倍** |
| §15.3 预登记预测 | **−9 ~ −14 ms/轮**（即 −1.4 ~ −2.2 µs/件） |

#### 采集 #16.3 结论

1. **判据 4（性能）兑现**：实测 **−1.575 µs/件 = −10.1 ms/轮**，落在 §15.3 预登记的 **9–14 ms/轮** 区间内（**偏下限**），且为噪声底的 2.6–4.3 倍 ⇒ 收益**真实、可辨**。
2. **`min_ns` 是分支化的独立指纹**：**3,857 → 2,204 ns（−42.9 %）**。`min` 是「最便宜那件」的成本，而分支化恰好把最便宜的分支（key，7 个成员）的读量从固定 25 降到 7 —— **`mean` 与 `min` 两条互不依赖的证据同向**。
3. **基线取值必须显式说明（本次验收最容易读错的一处）**：同构建的 #14 / #15 两次采样本身相差 **6.1 %**（12,490 vs 11,730），**大于**它们各自与 #16 的部分差值。故本节一律取**两次采样的均值 12,110 ns** 作基线、**不取单次** —— 若取 11,730 得 −10.2 %，取 12,490 得 −15.7 %，两者相差 5.5 个百分点且**全部落在噪声量级内**，单次基线不足以支撑结论。
4. **归因口径（重要，勿过度归功）**：#16 的构建**同时**含 **Tier 1 (1)**（显示字符串 managed 化，`18c0e28`）与 **Tier 1 (2)**（`gather` 分支化，`422452d`）—— 两个提交**相邻**，中间**没有**单独采样。因此 **−10.1 ms/轮 是两项的合并收益，不是分支化单项**。按 §15.2 的单价反推，字符串 managed 化只是其中的小头，但这**是推断、不是实测**；要分离单项需再加一轮带开关的 A/B，本项目**选择不做**（理由见 §16.4 第 1 点）。
5. **v6.11 重转写的代价被单独量出**（#16 → #17）：`AS::processEntry` **10,535 → 10,748 ns（+2.0 %）**，与「v6.11 的 body 更大（1304 行 vs 879 行）且每件多读 1 个成员（`eslId`）」一致 —— 即**基线换代本身是增重的**，而分支化的收益是在这个更重的基线上仍然成立。

#### 采集 #16.4 未做与不做（如实登记）

1. **不做编译期 A/B**：要分离「分支化」与「字符串 managed 化」的单项收益，需临时加开关恢复固定 25 成员读、编译两份、各采一轮。**本项目选择不做** —— 收益的**机制**（每件少读 12–18 次）与**单价**（§15.2 的 ≈0.12 µs/op）都已有独立实测，A/B 只能重新分配一个**已经落在预测区间内**的数字，却要付出一次有风险的临时代码改动。**这是取舍，不是遗漏。**
2. **逐事件 CSV 仍缺**：本节全部数字来自**聚合**模式；§15.5 第 1 条登记的「read / write / `CreateString` 三块拆分」**依然缺失**，需 `tracy-csvexport -u` 的 32,205 行逐事件导出（或 GUI），本轮未做。
3. **`#17`（v6.11 首测）** 的性能数字已同步回填至 [phase4-design.md](./phase4-design.md) §10.10 与 [skyui-version-divergence.md](./skyui-version-divergence.md) §6.2（两处此前均标注「需用 Tracy GUI 另行回填」）。

### 采集 #17 —— 2026-09-22 23:32（**v6.11 重转写首测**；补登记 §15.5 登记的缺失章节）

> **本节性质 = 补登记 + 回填。** 本轮**判据与日志原文**已完整登记在 [phase4-design.md](./phase4-design.md) **§10.10** 与 [skyui-version-divergence.md](./skyui-version-divergence.md) **§6.2**，本节**不重复**它们，只做两件只有本文件做得了的事：① 补齐 §15.5 第 3 条登记的「本文件尚缺对应章节」；② 回填上述两处都标了「需用 Tracy GUI 打开后回填」的 **zone 级耗时**。

#### 采集 #17.1 采集条件

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_22_23_32.tracy`，**36,703,919 B** / 写入 **2026-09-22 23:32:30** |
| 实际加载的 DLL | **838,656 B / 时间戳 2026-09-22 21:42:44 / SHA-256 `C7D0A369EA6DE5E73507F78A3D655F90E5BFBC26C47A793A51A4898E5BC44106`（2026-09-23 补测，该文件自采集后未变动；`Template.pdb` 19,460,096 B 同步部署）**（部署目录 `C:\Mod Organizer 2\mods\ModTest\SKSE\Plugins\Template.dll` 实测）。⚠️ **必须区分**：本项目 `build/bin/RelWithDebInfo/Template.dll` 现为 **839,680 B / 2026-09-23 00:26:52**，那是 `iconLabel` 第二个缓存落地后的产物，**尚未部署**（见 §17.3） |
| 物品数 / 轮数 | **6441** / **5**（`AS::processEntry` = **32,205**）。⚠️ 日志里 `previous round` 汇总行只有 **4 组**，因为该行由**下一轮**的首个 `RequestItemCardInfo` 触发、末轮没有「下一轮」，故**行数 = 轮数 − 1**；§10.10 / §6.2 写的「4 轮」即指此，**并不代表只跑了 4 轮** |
| 开关状态 | `SSE_REPLICATE_PROCESS_ENTRY = 1` |
| 判据结果 | **全过**（原文见 §10.10 / §6.2）：`0 mismatch` / `11/11 formType branch(es) exercised` / `forwarded 0` / `declines 0` / `apply failures 0`；旧 5.1 基线里 Falmer 箭的 `material 18 vs 8` 差异行**消失** |

#### 采集 #17.2 zone 级耗时（回填 §10.10 / §6.2 的两处「待回填」）

`tracy-csvexport` 默认聚合，与 #14–#16 同口径：

| zone | counts | total_ns | mean_ns | 折合 |
| --- | --- | --- | --- | --- |
| `AS::processEntry` | 32,205 | 346,142,273 | **10,748** | **69.23 ms/轮** |
| `RequestItemCardInfo`（引擎侧对照） | 6,455 | 400,312,233 | 62,015 | — |
| `AS::_requestItemInfo` | 32,200 | 531,892,075 | 16,518 | — |
| `AS::processList` | 4 | 10,175,765 | 2,543,941 | 4 次全为空转（同 #11b 结论） |

#### 采集 #17.3 与 #16 的关系，以及 `iconLabel` 为什么仍待验

两份 trace 的 **counts 全等、场景相同**，唯一变量是**未提交工作区里的 v6.11 重转写**：

| 量 | #16（5.1 基线） | #17（v6.11） | Δ |
| --- | --- | --- | --- |
| `AS::processEntry` mean | 10,535 ns | 10,748 ns | **+213 ns（+2.0 %）** |
| `AS::processEntry` min | 2,204 ns | 2,304 ns | +100 ns（+4.5 %） |

即 **v6.11 的忠实性是用「+2.0 % 的单件成本」换来的**（原因：body 由 879 行增至 1304 行、每件多读 1 个 `eslId`）。

⚠️ **`iconLabel`（c2）不在本轮验收范围内，且本轮无法代替它**：本轮部署的 DLL 是 **838,656 B**（v6.11 重转写，**不含** `iconLabel` 第二个缓存），而含 `iconLabel` 的 **839,680 B** 产物**尚未部署** ⇒ 「`iconLabel` 预测 identical 且不破 `0 mismatch`」**必须重新部署 839,680 B 后再采一轮**才能验收。在那之前，`iconLabel` 的状态仍是 **已实现、未真机复验**（与 [phase4-design.md](./phase4-design.md) §10.10「仍开放」第 1 条、[skyui-version-divergence.md](./skyui-version-divergence.md) §6.1 验收状态第 3 条一致）。
>
> **2026-09-23 部署状态更新（为 `iconLabel` 验收就绪）**：含 `iconLabel` 的 **839,680 B** 产物**已部署**至 `C:\Mod Organizer 2\mods\ModTest\SKSE\Plugins\`，两处 SHA-256 实测一致 = `296D04A619C1105A93936F0CBD7A1117BCEFE89794420F288694FB90F50805F8`（`Template.pdb` 同步部署，两处同为 `D1C7386D3128679088A5900E18CBAA3B683060627D025B399E4A33237E9CC210`）。覆盖前已把本条 trace 所用的 **838,656 B** 存为 `TracyLog/Template.dll.838656_v611-rewrite_20260922-2142.bak`（SHA-256 `C7D0A369EA6DE5E73507F78A3D655F90E5BFBC26C47A793A51A4898E5BC44106`），以保住 #17 的可重现性。

## 文档维护约定

- 每次采集新增一节 `## 采集 #N —— <日期 时间>`，沿用 §1.1（条件）→ §1.3（wall time）→ §1.4（zone 分布）→ §1.9（对 Q1–Q7 的贡献）的结构；
- **采集条件里的「实际加载的 DLL」必须写出字节数 + 时间戳**（本轮教训，§1.6）；
- 若某轮插桩集不全，**在标题里显式标注**（如「⚠️ 仅 #1 生效」），避免后续误用；
- **采集编号跨阶段连续递增**（阶段 1 = #1–#6，阶段 2 从 **#7** 起）；子节标题带「采集 #N.M」前缀（如 `#### 采集 #7.3`），以便与阶段 1 汇总章节里的 `### 7.x` 在层级和编号上区分开，避免读错节。
