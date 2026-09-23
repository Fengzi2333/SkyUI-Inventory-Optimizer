# 阶段 4 落地设计 —— 运行时 AS 热替换（免编译 SWF）

> 本文档是 [inventory-system-analysis.md](./inventory-system-analysis.md) §7.3 阶段路线中**阶段 4** 的落地方案，依据 [tracy-capture-log.md](./tracy-capture-log.md) 采集 #7–#10 的实测裁定与 [tracy-integration-plan.md](./tracy-integration-plan.md) §4.6.4 的最终结论。
>
> - **前置结论（已实测闭环）**：skyui 的增量 ≈ **+779 ms**（同存档 6441 件），归属 **C++ 实现 414 ms（53 %）** + **GFx 引擎侧边界 1 ms（0.1 %）** + **AS 层 364 ms（47 %，Tracy 视野外）**。
> - **本文档的核心新发现**：那 47 % 并非只能靠「改 SWF + 重编译」触及 —— CommonLibSSE-NG 暴露了 `GFxMovie::CreateFunction` / `GetVariable` / `SetVariable`，**可在运行时热替换 SWF 内的 AS 函数**，从而在**不编译 SWF、不依赖 Scaleform AS 工具链**的前提下同时干预 53 % 与 47 %。
> - **本文档的诚实边界**：~~47 % 的**内部构成仍是黑盒**~~ → ✅ **2026-09-20 已拆开**（采集 #11b，见 **§10.7**）：那 47 % 里 **≈ 84 % 是 `processEntry` 的 AS 体（303.3 ms/轮 = 37.7 %）**，**真正的边界编组 ≈ 0**（靶点 2.5），S1+S2 只在轮级留下 57.9 ms（7.2 %）；每轮总账 **804.6 ms**（对照全量窗口 826–872 ms）。**第一个交付物「测量」（阶段 4-0）已完成**，§5 / §6 的收益数字现已**全部有实测支撑**（§6.4 给出裁决：**4a → 4b-ii，跳过 4b-i**）。

---

## 1. 前置结论回顾（为什么必须走阶段 4）

| 分支 | 天花板 | 裁定 | 依据 |
|------|--------|------|------|
| 阶段 2a（哈希索引） | ≈ 0 | ❌ 遍历实测 < 1 % | 采集 #1–#6 |
| 阶段 3b（属性查询缓存） | < 2 % | ❌ 靶点 2.3 zone 合计 < 2 % | 采集 #7 / #9 |
| GFx 边界优化 | ≈ 0.1 % | ❌ 靶点 2.5 证伪（1.0–1.2 ms/轮） | 采集 #9 |
| 阶段 3a′（降 C++ 单次成本） | **53 %** | ⚠️ 可做但有限、风险高 | `GetValue` 仅占单次 2 %，其余在 item card 构建 |
| **阶段 4（改交互模式）** | **≈ 100 %** | ✅ **唯一根治** | 逐项往返本身不再必要 |

**机制上的根因**（一句话）：skyui 把「每件物品的数据」建模成了 **6441 次 AS→C++ 同步往返**，而 vanilla 只用了个位数次。优化引擎内部的任何一环都无法改变这个**次数量级**。

---

## 2. 新发现：从 C++ 侧运行时干预 AS

### 2.1 已就位的 API（全部来自 `extern/CommonLibSSE-NG`）

| 能力 | 签名 | 位置 |
|------|------|------|
| 拿到 movie | `GFxMovieView* FxDelegateArgs::GetMovie() const` | `RE/F/FxDelegateArgs.h:19` |
| 拿到 movie | `RE::GPtr<GFxMovieView> IMenu::uiMovie` | `RE/I/IMenu.h:106` |
| **创建 AS 函数对象** | `void GFxMovie::CreateFunction(GFxValue*, GFxFunctionHandler*, void* userData = 0)` | `RE/G/GFxMovie.h:56`（vfunc 0F） |
| 创建对象 | `void GFxMovie::CreateObject(GFxValue*, const char* cls = 0, const GFxValue* args = 0, uint32 n = 0)` | `RE/G/GFxMovie.h:54`（0D） |
| 创建数组 | `void GFxMovie::CreateArray(GFxValue*)` | `RE/G/GFxMovie.h:55`（0E） |
| 设变量 | `bool GFxMovie::SetVariable(const char* path, const GFxValue&, SetVarType = kSticky)` | `RE/G/GFxMovie.h:57`（10） |
| **读变量** | `bool GFxMovie::GetVariable(GFxValue* out, const char* path) const` | `RE/G/GFxMovie.h:58`（11） |
| 批量写数组 | `bool GFxMovie::SetVariableArray(SetArrayType, const char* path, uint32 idx, const void* data, uint32 count, SetVarType = kSticky)` | `RE/G/GFxMovie.h:59`（12） |
| 数组尺寸 | `bool GFxMovie::SetVariableArraySize(const char* path, uint32 count, SetVarType = kSticky)` | `RE/G/GFxMovie.h:60`（13） |
| 读数组 | `bool GFxMovie::GetVariableArray(SetArrayType, const char* path, uint32 idx, void* data, uint32 count)` | `RE/G/GFxMovie.h:62`（15） |
| 成员读写 | `GFxValue::GetMember / SetMember / Invoke / PushBack / GetArraySize` | `RE/G/GFxValue.h:359-384` |
| C++ 函数体 | `class GFxFunctionHandler { virtual void Call(Params&) = 0; }` | `RE/G/GFxFunctionHandler.h:11,32` |

`GFxFunctionHandler::Params`（`RE/G/GFxFunctionHandler.h:16-27`，`sizeof == 0x38`）的字段：

```
GFxValue* retVal;            // 00  返回值出口
GFxMovie* movie;             // 08  所属 movie（= GFxMovieView 基类）
GFxValue* thisPtr;           // 10  AS 侧 this（关键：拿到调用者对象）
GFxValue* argsWithThisRef;   // 18
GFxValue* args;              // 20  实参
uint32    argCount;          // 28
void*     userData;          // 30  建函数时传入的任意指针
```

### 2.2 为什么这就够了

三个事实叠加：

1. **我们现在就有 movie**。靶点 2.1 的 hook 是 `void hook(const RE::FxDelegateArgs&)`，`a_params.GetMovie()` 直接返回 `GFxMovieView*`（`src/ProfilingHooks.h:351`）；靶点 2.5 的 hook 里 `a_movieView` 是显式参数（`src/ProfilingHooks.h:415`）。
2. **`GFxMovieView` 继承 `GFxMovie`**，因此上面那张表的全部 API 都能调。
3. **AS2 是动态语言**。`ItemcardDataExtender` 的成员不是 final，实例属性可以在运行时被覆盖 —— `obj.f = newFunc` 在 AS2 里合法，且 `ExternalInterface`/`FxDelegate` 只按名字查找。

> **结论**：`GetVariable(path)` 取出目标对象 → `CreateFunction(&fn, handler)` 造一个 C++ 实现的 AS 函数 → `SetVariable(path, fn)` 覆盖原函数。**全程不需要 SWF 源码、不需要 `gfxexport`、不需要重新打包 SWF。**

### 2.3 与「改 SWF 重编译」路线的对比

| 维度 | 改 SWF + 重编译 | **运行时热替换（本文档）** |
|------|----------------|--------------------------|
| 工具链 | 需 Scaleform AS2 编译器（`gfxexport`，已不随 SSE 发行） | **无**（只用 CommonLibSSE-NG + 现有 MSVC） |
| 交付物 | 替换 `interface/*.swf`（30 个文件之一） | 仍是一个 `.dll` |
| 与其它 mod 的兼容 | 覆盖 SWF → 与任何改同一 SWF 的 mod **硬冲突** | 运行时改函数 → 与 SWF 内容无关，**可共存**（但会被后加载者覆盖，需注意顺序） |
| 可关闭性 | 需还原 SWF | **运行时可开关**（换成 `orig` 或 no-op） |
| 调试成本 | 每次改都要重编译 + 重启游戏 | 改 C++、重编译 DLL 即可 |
| 风险 | 打包错误 → 菜单打不开 | 替换错对象 → 菜单功能异常（同样严重，但**可粒度回退**） |

> 阶段 4 的原设想（[tracy-integration-plan.md](./tracy-integration-plan.md) §4.6.4「`skse.ExtendData(true)` 批量通道 + 改 SWF 消费方式」）**仍然成立**，但本文档给出的是**同一目标、成本更低、可回退**的实现路径。二者关系见 §7.3。

---

## 3. AS 侧调用链（源码级，含三个可插入点）

### 3.1 完整链路

以「打开背包 → 列表失效」为例，skyui 侧的实际执行序列（源码行号均为 `extern/SkyUISrc/src/`，**5.1 基线**；运行版 v6.11 的行号见 §10.10 与 [skyui-version-divergence.md](./skyui-version-divergence.md)）：

```
BasicList.InvalidateData()                                  Common/skyui/components/list/BasicList.as:263
 ├─ for i: _entryList[i].itemIndex = i; .clipIndex = undefined     :270-273
 ├─ for i: _dataProcessors[i].processList(this)                    :275-276
 │    └─ [processor 0] InventoryDataSetter.processList(list)  ←── ItemcardDataExtender.as:40
 │         ├─ var entryList = a_list.entryList                      :42
 │         ├─ for (var i = 0; i < entryList.length; i++)            :44   ★ 6441 次
 │         │    ├─ if (e.skyui_itemDataProcessed || e.filterFlag == 0) continue;   :46-47
 │         │    ├─ e.skyui_itemDataProcessed = true;                 :49
 │         │    ├─ fixSKSEExtendedObject(e)         ← 纯 AS，无引擎调用  :52 / :65-119
 │         │    ├─ _requestItemInfo.apply(a_list, [this, i])         :55   ★★ 往返点
 │         │    │    └─ _requestItemInfo           ItemcardDataExtender.as:22-28
 │         │    │         ├─ this._selectedIndex = a_index;          :25
 │         │    │         ├─ GameDelegate.call("RequestItemCardInfo", [], a_target, "updateItemInfo")   :26
 │         │    │         └─ this._selectedIndex = oldIndex;         :27
 │         │    └─ processEntry(e, _itemInfo)        ← 纯 AS，~880 行  :56 / InventoryDataSetter.as:24-878
 │         └─ (循环结束)
 ├─ listEnumeration.invalidate()                                    :278
 ├─ UpdateList()                                                    :283
 └─ onInvalidate()                                                  :285-286
```

**`GameDelegate.call` 的真实语义**（`CLIK/gfx/io/GameDelegate.as:14-27`）：

```as
static function call(methodName, params, scope, callBack) {
    nextID = ++nextID;  var _loc1 = nextID;
    responseHash[_loc1] = [scope, callBack];
    params.unshift(methodName, _loc1);              // 把 methodName + uid 插到参数最前
    ExternalInterface.call.apply(null, params);     // ★ 进入 Flash/Scaleform 的 ExternalInterface
    delete responseHash[_loc1];                     // ★ 在 call 返回之后才执行 → 证明同步
}
```

两个关键推论：

1. **它是同步的**。`delete responseHash[_loc1]`（`:26`）在 `ExternalInterface.call`（`:25`）之后执行，且 `processEntry(e, _itemInfo)` 在下一行（`:56`）就能读到刚写入的 `_itemInfo`。→ 6441 次循环是**严格串行、阻塞**的，不存在流水化。
2. **它走的是 `ExternalInterface`**，而 Scaleform 把 `ExternalInterface.call` 实现为 **`GFxExternalInterface` → `FxDelegate::Callback`**。所以：

```
AS: ExternalInterface.call(m, uid, ...)          ← 入参编组发生在这里（Tracy zone 之前）
  → [Scaleform 内部]  → FxDelegate::Callback     ← 靶点 2.5 zone 起点
      → RequestItemCardInfo 引擎实现              ← 靶点 2.1 zone
      → FxResponse::Respond(obj)                  ← 返回值编组
  ← [Scaleform 内部]  → AS: GameDelegate.receiveResponse(uid, obj)   GameDelegate.as:29-39
      → responseHash[uid][1].apply(scope, [obj])  → ItemcardDataExtender.updateItemInfo  :34-37
```

**靶点 2.5 实测的 `Callback[…] − RequestItemCardInfo = 0.17 µs/次` 只覆盖了上图中「`FxDelegate::Callback` 内部」那一段**，即纯哈希查找 + 分发。它**不包含** `ExternalInterface` 的两侧编组（图上两端的 `[Scaleform 内部]`），那才是 47 % 的主要候选。

### 3.2 三个可插入点

| # | 插入点 | AS 位置 | 干预后消掉什么 | 需要复刻的 AS 逻辑 | 风险 |
|---|--------|---------|---------------|------------------|------|
| **P1** | `_requestItemInfo`（成员，**函数值**） | `ItemcardDataExtender.as:22-28` | 6441 次 `ExternalInterface` 双向编组 | **0 行**（`processEntry` 仍由 AS 跑） | 低 |
| **P2** | `processEntry`（成员，覆写） | `ItemcardDataExtender.as:63` → `InventoryDataSetter.as:24` | `processEntry` 的 AS 执行成本 | **~855 行**（`InventoryDataSetter.as` 几乎全部） | 高 |
| **P3** | `processList`（成员，覆写） | `ItemcardDataExtender.as:40-58` | P1 + P2 + 循环本身 | 循环 ~15 行 + P2 的 ~855 行 | 高 |

> **插入点优先级：P1 > P3 > P2**。P1 是「改动最小、逻辑复刻为零」的那一刀；P2/P3 需要复刻 `InventoryDataSetter`，只在实测证明 `processEntry` 的 AS 执行成本确实占大头时才值得做。
>
> ⚠️ `processList` / `processEntry` 是 **prototype 上的类成员**，而 `_requestItemInfo` 是**实例成员**（在构造函数里赋值，`ItemcardDataExtender.as:22`）。**实例成员比原型成员更好替换**：`SetVariable` 到具体实例即可，不影响 prototype，也就不影响其它 processor。

---

## 4. 阶段 4-0：AS 侧插桩（**第一个交付物，零优化风险**）

### 4.1 为什么必须先做这一步

47 % = 364 ms/轮 ÷ 6441 ≈ **56.5 µs/件**。这个数字与 C++ 侧的 64.4 µs/件**同量级**，本身就是一个可疑信号：一段简单的解释执行循环 + 若干属性赋值，通常不该比「构建完整 item card」还贵。因此 56.5 µs 里很可能**大部分不是 AS 逻辑，而是 `ExternalInterface` 的编组**。但这个判断目前**没有任何实测支撑**。

> ❌ **2026-09-20 实测：这个猜测是错的**（采集 #11b，见 §10.7(4)）。那 47 % 里 **≈ 84 % 是 `processEntry` 的 AS 体**（303.3 ms ÷ 361.2 ms，361.2 = AS 侧小计 57.9 + 303.3），**真正的边界编组 ≈ 0**（靶点 2.5 实测 0.25 µs/次）；S1+S2 只在轮级留下 **57.9 ms（7.2 %）**。「简单的解释执行循环不该比构建 item card 还贵」这个直觉之所以落空，是因为 skyui 的 `processEntry` 根本不是「简单循环」—— 它是 **855 行**的分支密集代码。

4-0 的唯一目标就是**把这个黑盒拆开**，产出下表：

| 段 | 含义 | 归属 | 4a 能否拿到 | 4b 能否拿到 |
|----|------|------|------------|------------|
| S1 | `ExternalInterface.call` 入参编组 + 进入 Scaleform | 往返 | ✅ | ✅ |
| S2 | `FxResponse::Respond` 编组 + `receiveResponse` + `apply` | 往返 | ✅ | ✅ |
| S3 | `processList` 循环本体（含 `fixSKSEExtendedObject`） | AS | ❌ | ✅ |
| S4 | `processEntry` 的 AS 执行（switch + `Math.round` + `keywords[...]`） | AS | ❌ | ✅ |
| S5 | `listEnumeration.invalidate()` + `UpdateList()`（列表 UI） | AS | ❌ | ⚠️ 部分 |

> ⚠️ **2026-09-20 实测（采集 #11b，§10.7(4)）**：**S3 无法用本探针测出**（`processList` 探针只覆盖到空转调用，见 §4.2 的修正块）→ 已从方程中**剔除**。实测值：**引擎侧 `RequestItemCardInfo` = 443.4 ms/轮（55.1 %）**、**S1+S2（AS 侧体）= 57.9 ms/轮（7.2 %）**、**S4 = 303.3 ms/轮（37.7 %）**、**S5 ≤ 67 ms/轮（< 8 %）**。

**判据**（决定后续路线）：

- 若 **S1+S2 主导**（≥ 60 %）→ **只做 4a（P1）就够**，收益接近全量，且零逻辑复刻；
- 若 **S4 主导** → 4a 收益有限，必须做 4b（P3）并复刻 `InventoryDataSetter`；
- 若 **S5 主导** → 病根在列表 UI 更新，**阶段 4 的方向本身要修正**（应转向列表虚拟化 / 惰性渲染，与 SKSE 通道无关）。这一分支目前**未被排除**，是本设计最大的未知。

### 4.2 探针设计

三个探针都用同一套「包装并替换」机制，探针函数体只调 `SSE_ZONE` 并转发：

| 探针 | 替换目标（相对 processor 实例） | zone 名 | 测出 |
|------|-------------------------------|---------|------|
| `AS:processList` | `processList` | `AS::processList` | ⚠️ **实测（§10.7(3)）只覆盖到「空转」`processList` → 测不出 S3**（原设计意图：S3+S4+往返 = 循环总时间） |
| `AS:requestItemInfo` | `_requestItemInfo` | `AS::_requestItemInfo` | S1+S2（往返） |
| `AS:processEntry` | `processEntry` | `AS::processEntry` | S4 |

由嵌套关系即可解出各段：

```
S3 + S4 + S1 + S2 = AS::processList            （外层，1 次/轮）
S1 + S2           = AS::_requestItemInfo × 6441
S4                = AS::processEntry × 6441
S3                = processList − Σ(_requestItemInfo) − Σ(processEntry)
```

> ⚠️ **2026-09-20 实测修正（采集 #11b，见 §10.7）**：上表**只有前两行成立**。第三行（把 `processList` 当外层）**已在真机上被证伪**：`AS::processList` 探针捕获到的 4 次调用（第 2–5 轮各 1 次，各 3.6 ms）**全为空转** —— 其窗口内**没有任何** `_requestItemInfo` / `processEntry` / `RequestItemCardInfo` 子事件（§10.7(3)），而每轮真正的**全量** `processList`（6441 项、约 0.85 s）**从未被覆盖**（它在探针安装完成之前就已开始 —— 安装发生在第 1 项 `RequestItemCardInfo` 的 hook 内）。因此 **S3 无法由本探针解出，予以剔除**；S1+S2 与 S4 则由前两行**正常解出** —— 完整的 S1–S5 分解见 §10.7(4)。

**一致性自检（复用采集 #9 已验证的手法）**：`AS::processEntry` 的 count **必须等于** `RequestItemCardInfo` 的**全量路径** count。**若两者不等，说明包装改变了行为，数据作废** —— 这是一个内置的假阳性检测器。✅ **2026-09-20 通过（采集 #11b）**：**32205 == 32205**（`32205 = 6441 × 5`，且**逐轮** 6441 == 6441）。注意**绝对数不是判据**（随存档与轮数变化，见下方「⚠️ 绝对数不是判据」）。

> ⚠️ **「全量路径」是本自检的隐含前提，S0a 真机实测已把它变成必须显式写出的条件**（§10.5(6)）：`RequestItemCardInfo` 有**两条独立的 AS 触发路径**（见下方「双路径」），只有**全量路径**会经过 `_requestItemInfo` / `processEntry`；**悬停/事件路径**直接调 `GameDelegate.call("RequestItemCardInfo", …)`，**不经过任何一个探针**。做自检时必须先按时间戳把这部分**扣除**，否则会把它误判成「探针丢调用」。

> **实施拆分（2026-09-20 决定）**：三个探针**不一起上**，拆成 **S0a**（只做 `_requestItemInfo`）与 **S0b**（补 `processList` / `processEntry`）。理由有两层：
>
> 1. `_requestItemInfo` 是三者中**唯一被 `apply` 调用**的成员（`ItemcardDataExtender.as:55`），所以它同时是 §8.3 风险表里「`CreateFunction` 造的函数与 AS2 语义不兼容」（概率**中**）的**判定点**。H2 若为假，另两个探针没有意义（4b 同样依赖替换后的函数能被 AS 正常调用）—— 先做它，可以避免无谓的风险暴露；
> 2. 它是**实例成员**（`ItemcardDataExtender.as:22`，在构造函数里赋值），替换它**不可能**波及其它对象；而 `processList` / `processEntry` 是 `IListProcessor` 的**通用契约成员**，替换的语义面更大。
>
> **count 自检的精确形式随「安装发生在何处」而不同**，三者不能混用同一个公式：
>
> | 探针 | 安装时机 | 预期 count | 差额来源 |
> |------|---------|-----------|---------|
> | `AS::_requestItemInfo`（S0a） | 该轮**第 1 次** `RequestItemCardInfo` 的 hook 内 —— 也就是第 1 项的 `_requestItemInfo` **正在执行时** | **全量 `RequestItemCardInfo` − 轮数 × 1**（**相对式**；3 轮即 `19323 − 3` = 19320） | 被替换的那次调用已经用旧函数对象发起（`apply` 的函数引用在进入函数体之前就已解析），**不计入** zone |
> | `AS::processEntry`（S0b） | 同上（同一处安装点） | **= 全量 `RequestItemCardInfo`**（严格相等） | 无 —— `:56` 的 `processEntry` 是在 `:55` 的往返**返回之后**才被调用，所以**连第 1 项都被覆盖** |
> | `AS::processList`（S0b） | 同上（安装发生在它**自己的循环体内部**） | `轮数 − 1`（5 轮即 4） | ⚠️ **实测证明本行的把握是错的**（§10.7(3)）：**每一轮**的**全量** `processList` 都在安装前已开始 → **全量循环一次都没被覆盖**；`轮数 − 1` 只是**偶然对上**（捕获到的 4 次其实是各轮**之后**的空转调用）。**该探针不可用于 S3。** |
>
> **判据**：差额必须**恒定且等于上表**（**按路径分解后**核对，见下方「双路径」）。`_requestItemInfo` 与全量路径的差额若随轮数漂移，说明探针在**丢调用**——包装改变了行为，数据作废。S0b 的 `processEntry` 则必须与全量路径**严格相等**，这正是本节原有判据能原样保留的原因。
>
> ⚠️ **绝对数不是判据**：全量路径每轮次数 = 存档的 `entryList.length`（过滤后），**随存档变化**；「轮数」也只在该次采集的轮数内成立。写死 `19320` / `19323` 只在「3 轮 + 该存档」下偶然正确 —— S0a 真机实测（5 轮，且中途有滚动）已证明**必须用相对式**，见 **§10.5(6)**。

**双路径：`RequestItemCardInfo` 的 AS 调用点不止一处**（S0a 实测新增，§10.5(6)）—— 这一点决定了**所有 count 自检的口径**：

| 路径 | 调用点 | 触发时机 | 经过探针？ | 量级（S0a 采集：5 轮） |
|------|--------|---------|-----------|----------------------|
| **全量** | `ItemcardDataExtender.as:26`（在 `_requestItemInfo` 内） | 每轮 `opening` 后 `processList`（`:44`）的全量遍历 | ✅ `_requestItemInfo` / `processEntry` | **6441 / 轮**（5 轮 = 32206） |
| **悬停** | `ItemMenu.as:357` `onItemHighlightChange` | 鼠标高亮切换物品 | ❌ 直接 `GameDelegate.call` | **36**（本采集） |
| **事件** | `InventoryMenu.as:223` `onQuantityMenuSelect` | 数量菜单选择 | ❌ | 0（本次未触发） |
| **事件** | `InventoryMenu.as:250` `onItemCardSubMenuAction` | 子菜单关闭 | ❌ | 0（本次未触发） |
| — | `BarterMenu.as:198` | （源码中已注释掉） | ❌ | 0 |

> 因此 `RequestItemCardInfo` 的**总** count 只有在「全程不滚动、不悬停」时才等于全量路径 count。**判据 4/5 必须按路径分解后核对**，不能拿总数直接相减 —— 这是 S0a 实测中最容易踩的坑（逐项对账见 §10.5(6)）。

**包装函数必须保留的三个语义**（否则会破坏功能）：

1. **`this` 绑定**：`Params::thisPtr`（`GFxFunctionHandler.h:20`）必须原样传给原函数 —— `_requestItemInfo` 是用 `apply(a_list, [this, i])` 调用的，所以它里面的 `this` 是 **`a_list`（TabularList）**，`this._selectedIndex` 是**列表**的字段（详见 §5.1「三方宿主」）；
2. **返回值**：`Params::retVal`（`:18`）写出原函数的返回；
3. **参数个数**：`Params::argCount` / `args`（`:22-23`）原样转发。

### 4.3 路径发现策略（**本设计最不确定的实现细节**）

`GetVariable` 需要一条 AS 路径。已知结构（源码级）：

```
InventoryMenu (AS 类, ItemMenus/InventoryMenu.as:15)      ← 绑定在 SWF 的某个 MovieClip 上
 └─ inventoryLists : InventoryLists                        （ItemMenu.as:92 调用 inventoryLists.InitExtensions()）
     └─ itemList : TabularList                            （InventoryMenu.as:73 声明）
         ├─ entryList : Array                             （ItemcardDataExtender.as:42）
         └─ _dataProcessors : Array                        （BasicList.as:275）
             ├─ [0] = InventoryDataSetter 实例            （InventoryMenu.as:74）← 唯一含 _requestItemInfo，phase 4 目标
             ├─ [1] = InventoryIconSetter 实例            （InventoryMenu.as:75）—— 无 _requestItemInfo，非目标
             └─ [2] = PropertyDataExtender 实例          （InventoryMenu.as:76）—— 无 _requestItemInfo，非目标
```

> 三个槽与它们的有无成员是**第二次实测**结果（`_dataProcessors = array[3]`，见 §10.4），不是源码推断：`[1]`/`[2]` 同样是 `IListProcessor` 实现，因此**合法地**带 `processList`/`processEntry`，只有 `_requestItemInfo` 能把目标认出来。

**入口：不用 `_root`，用引擎已缓存的菜单 clip 句柄**（2026-09-20 首跑后修正，见 §10.2）

`RE::InventoryMenu::RUNTIME_DATA::root` 是一个**引擎缓存的 `GFxValue`**，CommonLibSSE-NG 在 `extern/CommonLibSSE-NG/include/RE/I/InventoryMenu.h` 里把它的语义写明了：

```cpp
GFxValue root;  /* 00 - kDisplayObject - "_level0.Menu_mc" */
```

即：它是 `_level0.Menu_mc` 的活句柄 —— skyui 的 `ItemMenu` 实例就挂在这一层（`ItemMenu extends MovieClip`，`ItemMenu.as:15`）。两个直接后果：

1. **`_root` 比 `Menu_mc` 高一层**，所以 `_root.inventoryLists.itemList` 这条候选**从一开始就不可能命中** —— 这与「closing 时机」是两个**独立**的错误。
2. **不需要猜任何绝对路径**：有了句柄就 `GetMember` 逐级走即可。

**多级回退探测**（按顺序尝试，第一个成功的即为路径）：

| # | 探测 | 说明 |
|---|------|------|
| 1 | `RUNTIME_DATA::root` → `GetMember("inventoryLists")` → `GetMember("itemList")` | 相对成员路径遍历，实现在 `s1::resolvePath`（`src/ProfilingHooks.cpp`） |
| 2 | `GetVariable("_level0.Menu_mc.inventoryLists.itemList")` | 同一候选的绝对路径写法。**保留它是为了区分「缓存句柄失效」与「成员路径写错」**，命中方式会打进日志 |
| 3 | `RUNTIME_DATA::root` → `GetMember("itemList")` | 若 clip 的 class 直接暴露这个 getter（`ItemMenu.as:201`） |
| 4 | 成员名扫描 | `GFxValue` 无公开的成员枚举 API，只能用 `VisitMembers` 枚举 + 按候选名逐条试探 |

> **实际命中情况（第二次实测，见 §10.4）**：第 **1** 条命中 —— `inventoryLists.itemList` 经 `GetMember from the cached clip handle` 解析成功；第 2–4 条**从未被触发**。即 `RUNTIME_DATA::root` 句柄**有效**，后续阶段（S0 起）可全程使用 `GetRuntimeData().root` + `GetMember`，无需退回绝对路径。

**第 5 条「用委托参数兜底」已删除 —— 首跑实测证伪。**

首跑日志给出 `S-1: RequestItemCardInfo argCount = 0`，而 `a_args` 为空是**构造使然**，不是意外：

```
ItemcardDataExtender.as:26  GameDelegate.call("RequestItemCardInfo", [], a_target, "updateItemInfo")
                                                        ↑ call() 的第 2 参数 = 空参数数组
GameDelegate.as:24          params.unshift(methodName, _loc1)   ->  params = ["RequestItemCardInfo", uid]
GameDelegate.as:25          ExternalInterface.call.apply(null, params)
FxDelegate.cpp:21           FxDelegateArgs(a_args[0], handler, movie, &a_args[1], a_argCount - 1)   ->  实参个数 = 0
```

`a_target`（processor）与 `"updateItemInfo"`（回调名）是 `call()` 的**第 3、4 参数**，即 scope/callback 对 —— 它们**不经过 `ExternalInterface`**，只进 `GameDelegate` 的 `responseHash`，供 AS 侧 `receiveResponse` 使用。

> 所以 **processor 实例无法从委托参数拿到：路径探测是唯一落点**（H1 从「有兜底」升级为「没有退路」）。
>
> index 走的是另一条路：`_requestItemInfo` 在调用前把它写进 **`this._selectedIndex`**，而这里的 `this` 是 **`a_list`（TabularList）**，因为调用方式是 `apply(a_list, [this, i])`（`ItemcardDataExtender.as:55`）—— 它是**列表**的字段，**不是** processor 自己的字段。这条修正直接改写了 §5.1 的伪代码。

### 4.4 风险与回退

| 风险 | 影响 | 缓解 |
|------|------|------|
| 路径找不到 | 探针不生效（失败安全：不替换任何东西） | 多级回退；每个探针独立、有日志 |
| 包装破坏了 `this` / 返回值 | **菜单功能异常**（卡死 / 列表空） | 探针函数只转发、不改语义；4-0 **只包装不优化**；`SSE_ZONE` 在 `TRACY_ENABLE` 关闭时是空宏（`src/Profiling.h:45`），因此**正式版构建不含探针** |
| 被其它 mod 的 SWF 覆盖 | 路径失效 | 探测失败即静默跳过 |
| 高频 zone 拖慢本体 | 6441 × 3 次/轮 的 Tracy zone 本身有成本 | 用固定名 `ZoneScopedN`（无分配，`Profiling.h:32-35`）；对照**同轮**的 `RequestItemCardInfo` 计数确认未失真 |
| AS 侧 zone 与 C++ zone 的时间线对齐 | 嵌套关系可能不符合预期 | Tracy 按线程区分；AS 执行发生在调用 `Callback` 的**同一个线程**（同步往返，§3.1 推论 1），故嵌套应当成立 —— **这本身就是对「同步」推断的一次验证** |

**回退开关**：所有探针由独立编译开关 / config 控制，默认**仅 4-0 探针 + 靶点 2.1 / 2.5**，且探针在找不到路径时**不替换**。

**4-0 的完成标志**：产出上表 S1–S5 的**实测占比**，并明确 S1+S2 vs S4 vs S5 谁是主导。

---

## 5. 阶段 4a：热替换 `_requestItemInfo`（消往返，P1）

> ⚠️ **前提**：4-0 证明 **S1+S2 是 47 % 的主要构成**。若 S4 主导，本阶段收益有限，见 §6。

### 5.1 原理

`_requestItemInfo`（`ItemcardDataExtender.as:22-28`）只做三件事：

```as
var oldIndex = this._selectedIndex;
this._selectedIndex = a_index;                                        // ① 设"当前项"，供引擎读取
GameDelegate.call("RequestItemCardInfo", [], a_target, "updateItemInfo");  // ② 往返
this._selectedIndex = oldIndex;                                       // ③ 还原
```

替换为 C++ 函数后，**②** 变成「查表 + 一次 `SetMember`」：

```cpp
// 伪代码：形状对齐 FxDelegateCallbackHook / RequestItemCardInfoHook 的现有风格
// 三个 GFxValue 的宿主各不相同，混淆它们是本阶段最容易犯的错（见下方「三方宿主」）。
void RequestItemInfoHandler::Call(RE::GFxFunctionHandler::Params& a_params) {
    RE::GFxValue* list      = a_params.thisPtr;    // = a_list（apply 的 thisArg）→ _selectedIndex 的宿主
    RE::GFxValue* processor = &a_params.args[0];   // = 调用点的 this（InventoryDataSetter 实例）
    const auto    index     = static_cast<std::size_t>(a_params.args[1].GetNumber());

    RE::GFxValue oldIndex;
    list->GetMember("_selectedIndex", &oldIndex);
    list->SetMember("_selectedIndex", RE::GFxValue(static_cast<double>(index)));  // ① 保留

    // ② 用缓存直接写 _itemInfo，不做 ExternalInterface 往返
    processor->SetMember("_itemInfo", g_cardCache[index]);

    list->SetMember("_selectedIndex", oldIndex);   // ③ 保留
}
```

**三方宿主**（首跑推翻 §4.3 旧偏移表之后才看清，务必逐项对齐）：

| AS 里的写法 | 对应谁 | C++ 侧怎么拿 | 用途 |
|------------|--------|-------------|------|
| `this` | `a_list`（TabularList） | `a_params.thisPtr` | `_selectedIndex` 的读 / 设 / 还原（①②③） |
| `a_target` | processor（`InventoryDataSetter` 实例） | `a_params.args[0]` | `updateItemInfo` 写 `_itemInfo`（`ItemcardDataExtender.as:36`）；`processEntry(e, _itemInfo)` 也从这里读（`:56`） |
| `a_index` | 循环变量 `i` | `a_params.args[1]` | 缓存键 |

> ⚠️ 旧伪代码把 `thisPtr` 当 processor 用、并从 `args[0]` 取 index —— **两处都错**。`apply(a_list, [this, i])` 的 thisArg 是 `a_list`，`[this, i]` 才是实参表。
>
> `GFxFunctionHandler::Params` 的字段见 `extern/CommonLibSSE-NG/include/RE/G/GFxFunctionHandler.h:16-27`（`thisPtr` 0x10、`args` 0x20、`argCount` 0x28）。

**关键保留点**：① 与 ③ 的 `_selectedIndex` 操作**必须保留** —— 而现在有实测支撑，不再是「可能被依赖」的猜测：委托参数是**空的**（§4.3），所以 `a_list._selectedIndex` 是 `GameDelegate.call` 之后**引擎唯一能读到"当前在构建哪一项"的地方**。该字段从「可疑的保留点」升级为「数据通路本身」：**删掉它 = 引擎不知道该给哪一项构建 card**。

> ⚠️ **4a 的覆盖面（S0a 实测修正，§10.5(6)）**：替换 `_requestItemInfo` 只能优化**全量路径** —— 每轮 `opening` 的 `processList` 遍历（实测 **6441 次/轮**，占 `RequestItemCardInfo` 总量 **99.9 %**）。用户在背包里**滚动 / 移动鼠标高亮**时，`ItemMenu.as:357` 的 `onItemHighlightChange` 会**直接**调 `GameDelegate.call("RequestItemCardInfo", …)`，**不经过** `_requestItemInfo`（实测该路径 36 次，占 **0.1 %**）。两个直接后果：
>
> - 4a 的收益口径**只覆盖全量路径**，不要写成「消掉所有 item card 往返」；
> - 悬停路径次数极少（36 vs 32206），但它是**滚动瞬间卡顿**的独立来源 —— 若后续实测显示滚动卡顿显著，需针对 `onItemHighlightChange` 单独处理（该插入点**不在**附录 A 的清单内）。

### 5.2 缓存数据的两种来源

| 来源 | 做法 | 代价 | 风险 |
|------|------|------|------|
| **A. 观测并缓存引擎结果** | 在靶点 2.1 的 hook 里，第一轮把 `orig` 的 `Respond` 结果**截获并存档**（用 index 做键），后续轮次直接查表 | 第一轮仍付全价（414 ms），后续轮次 ≈ 0 | 需要能截获 `Respond` 的载荷（`FxResponseArgsBase`，见 §9-3 假设） |
| **B. 复刻引擎构建逻辑** | 参照 `RequestItemCardInfo` 的 C++ 实现自行构建 card | 无 | **极高**：那是引擎内部函数，其字段/格式无公开文档 |

**推荐 A**：一次全价换后续全免，且**不引入任何格式复刻风险**——缓存的就是引擎自己产出的字节。缓存失效点复用 §4.6 靶点 2.4（`SendContainerChangedEvent`）与菜单 `closed`。

**A 的必要条件**（待验证）：能拿到 `orig` 的返回载荷。若 `Respond` 不可截获，退路是**在第一轮直接读回 AS 侧已写好的 `_itemInfo`**（`updateItemInfo` 在 `ItemcardDataExtender.as:34-37` 把 `a_updateObj` 存进 `_itemInfo`）——即第一轮照原样跑，在 `processEntry` 之后由我们的探针把 `_itemInfo` 复制一份到 C++ 缓存。

### 5.3 预期收益

| 项 | 值 | 说明 |
|----|-----|------|
| 消掉 | **S1 + S2** | 6441 次 `ExternalInterface` 双向编组 |
| 消掉 | 6441 次引擎 `RequestItemCardInfo` 本体 | 仅当缓存命中（2 轮及以后） |
| 保留 | S3 + S4 + S5 | AS 侧循环、`processEntry`、列表 UI |
| 首轮 | 仍为全价 | 缓存构建 |

**保守区间**：≥ 53 %（C++ 本体全消）；**乐观**：53 % + (S1+S2 占比)。**这个数在 4-0 出数据前不写死。**

### 5.4 风险

| 风险 | 严重度 | 缓解 |
|------|--------|------|
| AS2 不允许替换实例成员 | 低（AS2 动态） | 4-0 已先行验证（探针就是同一种替换） |
| `this` 语义被改坏 | 中 | 严格保留 ①③；失败即回退到 `orig` |
| 缓存与真实库存不一致（拾取/丢弃后未失效） | **高**（UI 显示错误数字） | 复用靶点 2.4 失效；**首次发布先只做"同一次打开内缓存"**（即打开时构建、关闭时丢弃），把失效复杂度降到零 |
| 与其它改 `ItemcardDataExtender` 的 mod 冲突 | 中 | 若探针发现目标成员已被替换成非原生实现，则**放弃替换**并记录 |

---

## 6. 阶段 4b：热替换 `processList`（消循环，P3）

> ⚠️ **前提**：4-0 证明 **S3 或 S4 显著**（否则 4a 已足够，本阶段的额外复杂度不值得）。**实测已给出答案（见 §6.4）**：**S4 显著（303.3 ms/轮，37.7 %）**、**S3 不可测（剔除）** → **本阶段保留，但只做 4b-ii**。

### 6.1 两个变体（~~**先做 4b-i，不要直接上 4b-ii**~~ → **已被 §6.4 实测裁决推翻：跳过 4b-i，改走 4a → 4b-ii**）

两者都替换 `processList`（`ItemcardDataExtender.as:40-58`），差别在「谁执行 `processEntry`」：

| 变体 | 做法 | 消掉 | 复刻量 | 风险 |
|------|------|------|--------|------|
| **4b-i（保守，推荐先做）** | C++ 侧跑循环（读 `entryList`、判 `skyui_itemDataProcessed` / `filterFlag`、填 `_itemInfo`），**`processEntry` 仍调 AS 原函数**（`GFxValue::Invoke`，`GFxValue.h:361`） | **S1 + S2 + S3** | **0 行**（`fixSKSEExtendedObject` 也可以选择性地用 `Invoke` 调用） | 中 |
| **4b-ii（激进）** | 同上，但 `processEntry` 也用 C++ 复刻 | S1 + S2 + S3 + **S4** | **~855 行**（`InventoryDataSetter.as:24-878`） | **高** |

### 6.2 ~~为什么 4b-i 是更好的第一步~~（**已被 §6.4 推翻：4b-i 的收益 S3 不可测，本小节不再代表当前路线**）

4b-i 相对 4a 的**唯一增量**是 S3（`processList` 的 AS 循环本体 + `fixSKSEExtendedObject`）。而它的代价是：

- 要在 C++ 侧**精确复刻循环的过滤语义** —— `e.skyui_itemDataProcessed || e.filterFlag == 0`（`:46-47`）与 `e.skyui_itemDataProcessed = true`（`:49`）。这**必须**与 AS 侧一致，否则：
  - 漏设 `skyui_itemDataProcessed` → 下一轮会重复处理（性能与语义都可能错）；
  - 错判 `filterFlag` → 该显示/不该显示的物品被跳过。
- 要处理 `a_list` 参数（`processList(a_list)` 的第一个实参是列表对象，`:42` 用 `a_list.entryList`）。

**收益的上限**：S3 ≤ S1+S2+S4+S5 之外的那部分。从量级看，`fixSKSEExtendedObject` 是纯 AS 的 `switch` + 属性读写（`:65-119`），**可能并不便宜**（每件都跑一次，含 `formType` 分发与 `delete`）——但也可能就是几十 ns 量级。**这正是 4-0 要回答的**。

> 结论：**4b 的收益完全取决于 4-0 的 S3 数据**。若 S3 < 10 %，直接放弃 4b，4a 即终点。

### 6.3 S5（列表 UI 更新）**不在 P1/P2/P3 的覆盖范围内**

这一点必须显式记录，否则会产生「做完 4a/4b 却仍有卡顿」的困惑：

`listEnumeration.invalidate()`（`BasicList.as:278`）与 `UpdateList()`（`:283`）**在 `processList` 之外**，属于 `BasicList.InvalidateData` 的后续步骤。若 4-0 显示 S5 主导，则需要**第 4 个插入点**：

| # | 插入点 | 位置 | 后果 |
|---|--------|------|------|
| **P4** | `InvalidateData`（`BasicList`） | `BasicList.as:263-287` | 消掉 S5 / 改为惰性渲染；**但它是所有列表的公共路径**，影响面远大于 processor，风险最高 |

**P4 不在本设计的第一批范围内**。若 4-0 指向 S5，应**重新评估阶段 4 的定位**（那意味着「逐项往返」不是主因，列表 UI 重建才是），并优先考虑阶段 3c（列表虚拟化）而非 SKSE 通道。

### 6.4 实测裁决（2026-09-20，采集 #11b）—— **本节的前提「S3 可测」不成立，改按 S4 裁决**

§6.1 / §6.2 的整条推理**建立在「S3 可被 `processList` 探针测出」之上**。采集 #11b 证明**这个前提不成立**：

| 事实 | 数据 |
|------|------|
| `AS::processList` 探针只捕获到 **4 次**调用（第 2–5 轮各 1 次），各 **3.6 ms**，窗口内**零**子事件 | §10.7(3) |
| 每轮真正的**全量** `processList`（6441 项、约 **0.85 s**）**从未被覆盖**（探针安装在第 1 项 `RequestItemCardInfo` 的 hook 内，那时全量循环已在执行中） | 同上 |
| 因此 `S3 = processList − Σ(_requestItemInfo) − Σ(processEntry)` **符号为负**（14.34 ms − 4022 ms） | 同上 |
| **S4（`processEntry`）= 303.3 ms/轮（37.7 %）**，逐项 47.09 µs × 6441 | §10.7(4) |
| **S1+S2 的引擎侧 ≈ 0**（0.25 µs/次），AS 侧体 57.9 ms/轮（7.2 %） | 同上 |

> ⚠️ **2026-09-21 采集 #13 修正**：上面那 **303.3 ms/轮 不是全部可消** —— 其中 **≈ 14 ms/轮 是 AS2 堆的 mark-sweep 暂停**（C++ 化不能保证消掉，且可能不缩 / 移位）→ 4b-ii 的**可消上界 ≈ 289 ms/轮 ≈ 36 %**。**这条必须写进 4b-ii 的验收判据**（轮内仍有毫秒级尖峰 ≠ 没做干净）。读数与推导见 **§10.8(12) 第 6 点**。

**修正后的裁决**：

| 方向 | 原判据（§6.2） | **实测裁决** |
|------|---------------|-------------|
| **4b-i**（只换 `processList`，消 S1+S2+S3） | 「S3 < 10 % 则放弃 4b」 | ❌ **跳过** —— 唯一增量 S3 **无法测量且量级 ≤ 数十 ms**（空转 3.6 ms 为下界），收益 < 2.5 %，不值得引入「复刻循环过滤语义」的风险 |
| **4b-ii**（循环 + `processEntry` 都 C++ 化，另消 **S4**） | 原列为「激进、最后再考虑」 | ✅ **保留（第二批）** —— 它是唯一能拿到 **S4 = 303.3 ms/轮（37.7 %）** 的路径；复刻量仍是 `InventoryDataSetter.as:24-878`（≈ 855 行），但**收益已有实测支撑**（不再是猜测） |
| **4a** | 由 S1+S2 是否主导决定 | ✅ **必做（第一批）** —— 443.4 ms（55.1 %）+ 57.9 ms（7.2 %）= **501 ms/轮（62.3 %）**，零逻辑复刻 |

> **本节「先做 4b-i」的措辞已被数据推翻**（4b-i 的收益 = S3，而 S3 不可测且极小）；**「4b 的收益完全取决于 S3」同样被推翻** —— 收益取决于 **S4**，而 S4 已实测为 303.3 ms/轮。执行顺序因此改为 **4a → 4b-ii**，**跳过 4b-i**。

---

## 7. 与既有路线的关系

### 7.1 与 3a′ 的组合

3a′（降低 `RequestItemCardInfo` 单次成本）与 4a 的缓存是**同一件事的两面**：

```
3a′  = 缓存引擎 item card（第一轮构建，后续命中）  → 省 53 %
4a   = 缓存 + 把「查询」从 ExternalInterface 换成 SetMember  → 省 53 % + (S1+S2)
```

→ **4a 严格包含 3a′ 的收益**。若 4a 的路径探测可行，**3a′ 应被 4a 取代**；只有在 4a 被证伪（例如无法替换 AS 成员）时，3a′ 才作为**独立降级方案**。

### 7.2 与原「`skse.ExtendData(true)` 通道」设想的关系

原设想（[tracy-integration-plan.md](./tracy-integration-plan.md) §4.6.4）是把 `value/weight/damage/armor/effects` 并入 SKSE 的 `ExtendData` 通道、再改 SWF 的消费方式。本设计的关系：

| 维度 | 原设想（SKSE 通道 + 改 SWF） | 本设计（运行时热替换） |
|------|---------------------------|---------------------|
| 数据来源 | SKSE 已灌入 `entryList` 的扩展字段 | 引擎自己的 item card 产出（缓存） |
| 是否需要改 SKSE | 需要（`ExtendData` 是 SKSE 的） | **不需要** |
| 是否需要改 SWF | **需要 + 需重编译** | **不需要** |
| 是否需要复刻 `processEntry` | **需要**（消费方式变了） | **不需要**（4a / 4b-i） |
| 收益上限 | ≈ 100 % | ≈ 100 %（4b-ii）/ 53 %+（4a） |

→ 本设计**优先**，因为它在**同一收益区间内消除了两个最重的工程依赖**（SKSE 改造、SWF 工具链）。原设想保留为**备选**（若 AS 热替换被证伪）。

### 7.3 为什么 `ExtendData` 本身**不能**直接解决问题

调研结论（`grep` 于 `extern/SkyUISrc/src/**/*.as`，**5.1 基线**）：

- `skse.ExtendData(true)` 只在 `ItemMenu.as:87`、`CraftingMenu.as:169`、`FavoritesMenu.as:226` 出现，**每类菜单每次打开调 1 次**；
- `ItemcardDataExtender.as` / `InventoryDataSetter.as` 里**没有任何 `skse.` 调用**（`processList` 只用 `GameDelegate.call`）。

→ `ExtendData` 已经把「SKSE 认为需要扩展的字段」灌进了 `entryList`（`fixSKSEExtendedObject` 就是在给这些字段改名的证据，`ItemcardDataExtender.as:65-119`）。但 **`value` / `weight` / `damage` / `armor` / `effects` 这 5 个字段不在其中** —— 它们由 `processEntry` 从 `a_itemInfo` 读取（`InventoryDataSetter.as`：`:32` value、`:33`/`:35` weight、`:47`/`:75`/`:85` effects、`:48` armor、`:77`/`:86` damage），而 `a_itemInfo` 正是 `RequestItemCardInfo` 的返回值。

> 这解释了为什么「灌更多字段」是**可行性最直接但工程最重**的路：它需要同时改 SKSE（加字段）和 SWF（改消费）。

---

## 8. 实施顺序、验收判据与回退

### 8.1 分步计划

| 步 | 内容 | 交付物 | 验收判据 | 失败代价 |
|----|------|--------|---------|---------|
| **S-1** | 路径探测 + `a_args` 偏移校验日志 | ✅ **已完成（2026-09-20）** —— 实现见 **§10.1**，首跑与两处修正见 **§10.2**，第二次实测 **6 条验收日志全命中** 见 **§10.4** | 日志中出现一个**可用的 processor 实例路径**（已达成：`inventoryLists.itemList`，目标 `_dataProcessors[0]`） | 无（只读） |
| **S0a** | **仅 `_requestItemInfo`** 一个探针包装并替换，**不优化** —— 先做它，因为它是 H2（`CreateFunction` 的产物能否被 `apply` 调用）的判定点，拆分理由见 §4.2 | ✅ **已完成（2026-09-20 真机，5 轮）** —— **采集 #11a**（见 [tracy-capture-log.md](./tracy-capture-log.md)）：`AS::_requestItemInfo` count = 32201、单次 ≈ 77.8 µs（`RequestItemCardInfo` 单次 69.4 µs，即往返外层残差 ≈ 8.4 µs）；实现见 **§10.5**，实测对账见 **§10.5(6)** | ① `AS::_requestItemInfo` **全量路径** count == 全量 `RequestItemCardInfo` count − 轮数（**相对式**，见 §4.2「双路径」）；② **物品卡片数值与 skyui 原版一致** —— **两条均 ✅** | 无（探针只转发；不命中则不替换） |
| **S0b** | 补上 `processList` + `processEntry` 两个探针，**不优化**；由嵌套关系解出 S1–S5。三者**同一次安装、全成或全不成**（S3 是三个总量之差，部分安装会把 S4 静默计入 S3，产出的不是更小的数而是**不可解释**的数） | ✅ **已完成（2026-09-20 真机，5 轮）** —— 实现见 **§10.6**；**采集 #11b** 判据 1–6 全中（[tracy-capture-log.md](./tracy-capture-log.md) §11b.3）；⚠️ 结论修正：`processList` 探针**测到的是空转**（窗口内零子事件）→ **S3 剔除**，方向改为 **4a → 4b-ii**（§6.4） | ✅ 判据达成：`AS::processEntry` count **严格等于**全量路径 count = **32205 == 32205**（且**逐轮** 6441 == 6441）；**不滚动、不悬停**（已满足） | 无（同上） |
| **S1（=4a）** | 替换 `_requestItemInfo`，首轮构建缓存、后续命中 | ✅ **已完成（2026-09-21 真机，5 轮）** —— 实现见 **§10.8**；**采集 #12 判据 1–6 全中**（[tracy-capture-log.md](./tracy-capture-log.md) §12.3）：命中轮窗口 **844.2 → 312.6 ms（−63.0 %）**、单次 `_requestItemInfo` **77.33 → 2.80 µs**、10 件 × 8 字段**类型 + 精确值全等** | 第 2/3 轮增量**明显低于**第 1 轮 ✅（−529.1 / −531.7 ms）；**且 UI 数值与 skyui 原版逐项一致** → 已自动化为「前 10 次命中逐字段对照」，**10/10 identical / 0 differs** ✅ | 回退到 `orig`（handler 已保存原函数副本）；§8.2 四条逐条对照见 §10.8(6)；本轮**未触发回退** |
| ~~**S2（=4b-i）**~~ | ❌ **已跳过**（§6.4）：S0 显示 **S3 不可测**（≤ 数十 ms）→ 收益 < 2.5 %，不值得复刻循环过滤语义 | — | — | — |
| **S2′（=4b-ii）** | 循环 + `processEntry` 都 C++ 化（复刻 `InventoryDataSetter.as:24-878`，≈ 855 行），消 **S4 = 303.3 ms/轮（37.7 %）** → ⚠️ **#13 修正：可消上界 ≈ 289 ms/轮 ≈ 36 %**（其中 **≈ 14 ms/轮 是 AS2 堆 mark-sweep 暂停**，C++ 化不能保证消掉，见 **§10.8(12) 第 6 点**） | ✅ **已实现（2026-09-21，提交 `e3a01ce`）+ 采集 #14 已运行** —— **判据 1/2/3/4/6 全中**（`32 205 == 32 205`；逐轮 `replicated 6431 / 6441` 且 `forwarded · declines · apply failures` **全 0**；影子对照 **50 件 / 495 成员 0 differs**；尖峰 **14.30 ms/轮** = 堆暂停）。⚠️ **判据 5 未达承诺**：`AS::processEntry` **265.45 → 80.45 ms/轮（−69.7 %）**，相对 S0b 消 **222.83 ms/轮 = 可消上界的 77 %** —— 原预测把「289 ms 解释器开销」里的 **≈ 66 ms GFx 成员访问**（`GetMember`/`SetMember`/`CreateString`，忠实复刻绕不开）误算成可消。读数、zone 逐项对照与修正后的成本模型见 **§10.9「真机验收」** | 同上 + `skyui_itemDataProcessed` 语义未变；卡片数值逐项一致（**机器对照已自动化并通过**）+ ⚠️ **轮内仍可能出现毫秒级尖峰（≠ 没做干净）** —— 实测 **每轮恰好 2 个**、合计 14.30 ms/轮，与 #13 的 14.26 一致 | 回退（`SSE_REPLICATE_PROCESS_ENTRY=0`，**本轮未触发**） |

> **S0 是硬门槛**：没有 S0 的数据，S1/S2 的收益无法预估、失败无法归因。原先「S0（=4-0）三个探针一次上齐」已按 §4.2 拆成 **S0a / S0b**：S0a 只做 `_requestItemInfo`（既是 H2 的判定点，也提供 S1+S2），S0b 补齐后才谈 S1–S5 的分解。实施记录见 **§10.5**（S0a）/ **§10.6**（S0b）。

### 8.2 回退开关设计

所有热替换必须满足**可运行时回退**：

1. **保存原函数**：替换前 `GetMember` 出原值存到 `GFxValue`（不能只存指针 —— `GFxValue` 是引用计数的，见 `GFxValue.h:288-300` 的拷贝/移动构造）。
2. **单一入口**：`install()` 幂等（对齐 `ProfilingHooks::installRequestItemCardInfoHook` 的现有约定，`src/ProfilingHooks.h:28-31`「Idempotent」）。
3. **开关粒度**：每个插入点（P1/P3）各一个独立开关，默认**关闭**，由 config 打开。
4. **不命中即不动**：路径探测失败 → 只写日志，**不替换**任何东西（失败安全）。

### 8.3 风险总表

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| 47 % 里的 S5 主导（列表 UI 重建） | 中 | **阶段 4 方向需修正** | 4-0 优先测量；S5 不在 P1–P3 覆盖内（§6.3） |
| `CreateFunction` 造的 AS 函数与 AS2 语义不兼容（如 `arguments`/`apply`） | 中 | 探针不工作 | `_requestItemInfo` 是**被 `apply` 调用**的（`ItemcardDataExtender.as:55`）→ 必须验证 C++ 函数能否作为 `apply` 的目标；**若不兼容，4a 不可行，退回 3a′** |
| 路径探测失败（SWF 版本差异 / 缓存句柄失效） | **低**（✅ 第二次实测已排除，§10.4） | ~~探针不工作，且没有退路~~ → 实测：缓存句柄**有效**，`inventoryLists.itemList` 一次命中（`via GetMember from the cached clip handle`） | 已落地两路探测（`RUNTIME_DATA::root` 句柄 `GetMember` + 绝对路径 `GetVariable`）+ 命中方式入日志；换 SWF 版本（非 skyui 5.2）时本风险回归，届时读日志的 `via` 字段即可自判 |
| 高频 `SetMember` 自身成为新瓶颈 | 低 | 收益打折 | 4-0 已测出基线；对比 `SetMember` 与 `ExternalInterface.call` 的单次成本 |
| 与其它 mod 的 AS 修改冲突 | 中 | 行为异常 | 替换前检查目标是否已是"非原生"（无法可靠判定 → 记录并说明局限） |
| Tracy zone 在 AS 高频路径的成本 | 低 | 测量失真 | 用 `ZoneScopedN`（无分配）；对比计数自检 |

### 8.4 本设计**不**声称的东西

- ❌ 不声称 4a 能拿到 **100 %**。在 4-0 出数据前，4a 的收益区间是 **[53 %, 53 %+(S1+S2)]**。
- ❌ 不声称 AS 热替换在 Scaleform GFx 上**一定可用**。`CreateFunction` 的存在只证明 API 存在，**不证明** AS2 侧会按预期解释这个函数对象（尤其 `apply` 调用方式，见 §8.3）。
- ❌ 不声称 47 % 里没有列表 UI 成本。相反，S5 分支（§6.3）是**必须先排除**的可能。

---

## 9. 待验证假设清单（按验证优先级）

| # | 假设 | 验证方式 | 若为假 | 影响 |
|---|------|---------|--------|------|
| **H1** | 从 `_level0.Menu_mc`（`RUNTIME_DATA::root` 缓存句柄）可到达 `inventoryLists.itemList` | S-1 日志：`S-1: inventory list found at ... via ...` + 命中方式 | ✅ **第二次实测成立**（§10.4）：`inventoryLists.itemList`，`via GetMember from the cached clip handle` —— 缓存句柄**有效** | **无退路** —— `a_args` 兜底已被首跑证伪（§4.3）→ 本设计不可行 |
| **H2** | `CreateFunction` 产出的函数对象可以被 AS2 正常调用（含作为 `apply` 的目标） | S0a：`_requestItemInfo` 替换后 ① count == `RequestItemCardInfo` count − 轮数 且 ② 卡片数值不变（§4.2 表） | ✅ **成立**（2026-09-20 真机，5 轮）：判据 1–3 + 6 直接命中（探针被 `apply` 调用、`thisPtr`/`argCount`/`args` 形状与 §5.1「三方宿主」一致、卡片数值无损）；判据 4/5 在**按路径分解后**成立（全量路径差额**恰 = 轮数 5**，探针不丢调用）—— 逐项对账见 **§10.5(6)** | ~~4a / 4b 全部不可行~~ **已排除** → **S0b 可以继续**（补齐 `processList` / `processEntry`，由嵌套关系解出 S1–S5） |
| **H3** | ~~`a_args` 的偏移如 §4.3 所推（`[0]=uid`, `[1]=processor`, `[2]=index`）~~ | S-1：打印 `argCount` + 每参数的 `IsNumber/IsObject/IsString` | ❌ **首跑证伪，第二次实测复现**（§10.2 / §10.4）：实测 `argCount = 0`，委托参数**恒为空**（`FxDelegate.cpp:21` 的 `-1` + AS 侧传空参数数组，见 §4.3） | 影响重大：processor 只能靠路径探测拿到（**H1 失去退路**）；index 改走 `a_list._selectedIndex`；`a_args[0]`/`[1]` 的语义由 §5.1「三方宿主」表重新定义 |
| **H4** | 6441 次循环**全部发生在一个线程**上（Tracy 时间线可嵌套） | S0：AS 侧 zone 是否嵌套在调用线程的 `Callback` 之下 | ✅ **成立**（2026-09-20，采集 #11b，§10.7(2)）：四个 zone（`_requestItemInfo` / `RequestItemCardInfo` / `processEntry` / `Callback[…]`）的 `thread` 列**全部 = 10**，无跨线程事件 | AS 与 C++ zone **可直接相减**（无需退化为「轮级」对比） |
| **H5** | 47 % 中 **S1+S2（往返编组）是主导**（≥ 60 %） | S0 的 S1–S5 分解 | ❌ **判否**（2026-09-20，采集 #11b，§10.7(4)）：S1+S2 的**引擎侧 ≈ 0**（0.25 µs/次，靶点 2.5 已证伪）、AS 侧体仅 **7.2 %**；**S4（`processEntry`）才主导 = 37.7 %（303.3 ms/轮）** | 4a 收益定为 **62.3 %**（非「接近全量」）→ **必须做 4b-ii**（≈ 855 行复刻，换 37.7 %） |
| **H6** | 47 % 中 **S5（列表 UI）不是主导** | S0：`processList` 之外的时间占比 | ⚠️ **间接支持（非直测）**：三段（`RequestItemCardInfo` 443.4 + `_requestItemInfo` 体 57.9 + `processEntry` 303.3 = **804.6 ms/轮**）已解释全量窗口 span（826–872 ms/轮）的 **92–97 %**，留给 S5 的空间 ≤ 67 ms/轮（< 8 %）→ S5 **不是**主导。**注**：本版无独立 S5 zone（P4 未插桩，§6.3），该结论由**总账残差**得出 | 无需修正阶段 4 方向（仍走 SKSE 通道） |
| **H7** | `Respond` 的载荷可截获（4a 缓存来源 A 的必要条件） | S1：在第一轮把 `orig` 结果存档，第 2 轮验证数值一致 | 不可截获 | 改用「第一轮后从 AS 侧 `_itemInfo` 读回」的退路（§5.2） |
| **H8** | 引擎的 `RequestItemCardInfo` 实现**依赖** AS 侧 `_selectedIndex`（故 ①③ 必须保留） | 阅读实现或实验：删掉 ①③ 看是否出错 | 不依赖 | 可简化 handler（收益不变，风险更低） |
| **H9** | `processEntry`（`InventoryDataSetter`）**在首轮之后不再被调用**（因 `skyui_itemDataProcessed` 缓存） | 对比第 1 轮与第 2/3 轮的 `AS::processEntry` count | ❌ **已由 S0a 数据判定为假**（§10.5(6) 逐轮明细）：全量路径 `RequestItemCardInfo` **5 轮各 6441 次**，而该路径与 `processEntry` 是同一个非 `continue` 分支里的**相邻两条语句** → `processEntry` **每轮都在被调用**。原因是列表每次 open 都重建（`skse.ExtendData(true)`，`ItemMenu.as:87`），条目对象上的 `skyui_itemDataProcessed` 标记随之消失 | 4b 的收益按「每轮都跑」计算；同时说明缓存机制比预期更弱 |

> **H2 与 H6 是"生死假设"**：H2 为假则本设计整体不可行；H6 为假则问题定义需重写。**在实现任何优化之前，先做 S-1 与 S0。**
>
> **S0b 落地后，H4–H9 的验证方式全部就位**（§10.6）：H5（S1+S2 是否主导）、H6（S5 是否主导）由 S1–S5 的分解回答；H4（是否单线程嵌套）由 AS 侧 zone 是否嵌在调用线程的 `Callback` 之下回答；**H9 已由 S0a 数据先行判定为假**（见上表）。

---

## 10. 实施记录

### 10.1 S-1 首次落地（2026-09-20，**其中两处已被 §10.2 修正**）

> 本节记录**首版**实现与它的构建验证。首跑暴露了两个实现缺陷（触发点 + 路径入口），修正见 **§10.2**；首版的构建数字（750 592 / 611 840 字节）保留在下面作为对照。

| 项 | 内容 |
|----|------|
| 新增声明 | `ProfilingHooks::installAsPathProbe()` 与 `ProfilingHooks::probeDelegateArgs(const RE::FxDelegateArgs&)`（`src/ProfilingHooks.h`）。修正后签名为 `installAsPathProbe(RE::FxDelegateHandler* = nullptr)`，见 §10.2 |
| 新增实现 | `src/ProfilingHooks.cpp`：匿名 `s1` 命名空间（只读工具）+ 两个函数体 |
| 触发点（❌ **已被否**） | `MenuOpenCloseListener::ProcessEvent` 的 **closing** 分支（`src/ProfilingHooks.cpp`）。当时的理由：菜单被用过并关闭时对象图才完整，`opening` 时列表可能仍在构建。**首跑证明此处永不生效** —— 关闭时 `IMenu::uiMovie` 已释放，探针每次提前返回且不 latch；已于 2026-09-20 移到 `RequestItemCardInfoHook::hook` 首次调用（§10.2） |
| 编译开关 | 全部在 `#ifdef TRACY_ENABLE` 内；`probeDelegateArgs` 的参数标了 `[[maybe_unused]]`（避免 `TRACY=OFF` 时的 C4100 撞 `/WX`） |
| 验证 ①（`ENABLE_TRACY=ON`，**首版**） | 构建通过；`Template.dll` **750 592** 字节，含全部 S-1 日志字符串（修正后为 **753 152**，见 §10.2） |
| 验证 ②（`ENABLE_TRACY=OFF`，**首版**） | 构建通过（无警告错误）；`Template.dll` **611 840** 字节，**不含** `phase 4 / S-1` 与 `S-1: RequestItemCardInfo argCount` → §4.4 的「正式版构建不含探针」**经实测成立**（修正后为 **612 352**，见 §10.2） |
| 验证 ③（构建状态复原） | 已把 `ENABLE_TRACY` 改回 `ON` 并重建（`Tracy Profiler enabled`），当前 `build/bin/RelWithDebInfo/Template.dll` 是带探针的版本 |

**S-1 的只读保证（可对照代码审查）**：`s1` 命名空间与两个函数体内**不存在** `SetMember` / `SetVariable` / `Invoke`，只有 `GetVariable` / `GetMember` / `HasMember` / `GetElement` / `GetArraySize` / `VisitMembers`。因此失败安全（§4.4 / §8.2）。

**一处实现上的要点**（写进代码注释了，这里也留档）：`_dataProcessors` 是**普通 var**（`BasicList.as:41`）可以用 `HasMember` 探；而 `entryList` 是 **getter**（`BSList.as:15`），必须用 `GetMember` 探 —— 对 getter 用 `HasMember` 会得到假阴性，进而让「列表对象识别」整体失败。

### 10.2 S-1 首跑结果与两处修正（2026-09-20 11:10–11:13）

**首跑只产出了一行 S-1 日志**：

```
[11:12:26.621] RequestItemCardInfo resolved: 0x7FF7DDE5E050 (SkyrimSE.exe + 0x88E050)
[11:12:26.621] RequestItemCardInfo hook installed at runtime address 0x7FF7DDE5E050
[11:12:26.621] FxDelegate::Callback hook installed at vtable 0x7FF7DED8AE38 slot 1
[11:12:26.665] S-1: RequestItemCardInfo argCount = 0          <-- 唯一一条 S-1 输出
```

开关背包数轮后，既没有 `=== phase 4 / S-1: ActionScript graph probe ===`，也没有任何 `S-1:` 前缀的路径日志。**这不是探针没跑，而是两个独立缺陷各挡掉一半**：

| # | 现象 | 根因 | 修正 |
|---|------|------|------|
| 1 | 路径探测**零输出** | 触发点挂在 `MenuOpenCloseListener` 的 **closing** 分支。菜单关闭时 `IMenu::uiMovie`（`GPtr<GFxMovieView>`，`IMenu.h:106`）**已被释放**，于是 `installAsPathProbe` 每次都在 `!menu->uiMovie` 处提前返回，且 `installed` **未 latch** → 反复重试、反复失败、无日志 | 触发点移到 **`RequestItemCardInfoHook::hook` 的首次调用**（此时 movie 已加载、`InitExtensions` 已注册 processor、`entryList` 已填充）。`uiMovie` 不再作为前置门，只作为句柄的兜底来源 |
| 2 | 路径入口**本来就错** | 入口用的是 `_root`，候选为 `_root.inventoryLists.itemList`。但 `RUNTIME_DATA::root` 的语义是 `"_level0.Menu_mc"`（`InventoryMenu.h`）—— **`_root` 比它高一层**，`inventoryLists` 挂在 `Menu_mc` 上 | 改用 `inventoryMenu->GetRuntimeData().root` 这个**引擎已缓存的 `GFxValue` 句柄**，候选改成**相对成员路径** `inventoryLists.itemList` / `itemList`，由新增的 `s1::resolvePath` 逐段 `GetMember`；绝对路径 `_level0.Menu_mc.*` 作为第二路，**用于区分「句柄失效」与「路径写错」** |

**首跑还证伪了设计里的一个假设（H3）——这是本次最有价值的产出。**

`S-1: RequestItemCardInfo argCount = 0`。设计 §4.3 曾由源码推断 `a_args = [uid, processor, index]`；实测为 **0**，且原因是**结构性的**：

```
ItemcardDataExtender.as:26  GameDelegate.call("RequestItemCardInfo", [], a_target, "updateItemInfo")
                                                        ↑ call() 的第 2 参数是空参数数组
FxDelegate.cpp:21           FxDelegateArgs(a_args[0], handler, movie, &a_args[1], a_argCount - 1)
```

`a_target` / `"updateItemInfo"` 是 `call()` 的第 3、4 参数（scope/callback 对），**不经过 `ExternalInterface`**。后果：

- **processor 实例拿不到 → 路径探测没有退路**（原 §4.3 第 5 条兜底作废，H1 从「有兜底」升级为硬门槛）。
- **index 不走委托参数** —— `_requestItemInfo` 把它写进 `this._selectedIndex`，而这里的 `this` 是 **`a_list`（TabularList）**（`apply(a_list, [this, i])`，`ItemcardDataExtender.as:55`）。§5.1 的伪代码据此重写，并新增「三方宿主」表。

**修正后的代码状态**：

| 项 | 内容 |
|----|------|
| 触发点 | `RequestItemCardInfoHook::hook`：`probeDelegateArgs(a_params)` → `installAsPathProbe(a_params.GetHandler())`，两者都在 `SSE_ZONE` **之外**（一次性开销不计入被测 zone） |
| `installAsPathProbe` 签名 | `static void installAsPathProbe(RE::FxDelegateHandler* a_handler = nullptr)`；非空时直接 `static_cast<RE::InventoryMenu*>`（`IMenu` 派生自 `FxDelegateHandler`）——省掉一次 `RE::UI` 查找，也**不再依赖菜单仍注册在 `RE::UI` 中** |
| 新增工具 | `s1::resolvePath(const GFxValue&, const char* a_dottedPath, GFxValue&)` —— 逐段 `GetMember` 遍历，首段缺失即返回 false |
| `MenuOpenCloseListener` | closing 分支的 `installAsPathProbe()` 调用**已移除**，原处留注释说明为何不能在此探测 |
| `[[maybe_unused]]` | `installAsPathProbe` 的参数同 `probeDelegateArgs` 一样标注，避免 `TRACY=OFF` 时 C4100 撞 `/WX` |
| 验证 ①（`ENABLE_TRACY=ON`） | 构建通过；`Template.dll` **753 152** 字节；含 `phase 4 / S-1: ActionScript graph probe`、`RUNTIME_DATA::root`、`GetMember from the cached clip handle`、`inventoryLists.itemList`、`cached handle is stale` |
| 验证 ②（`ENABLE_TRACY=OFF`） | 构建通过；`Template.dll` **612 352** 字节；`phase 4 / S-1`、`RUNTIME_DATA::root`、`S-1: inventory list found at` **全部不存在** → 「正式版不含探针」再次实测成立 |
| 验证 ③（状态复原） | 已改回 `ENABLE_TRACY=ON` 并重建（`753 152` 字节） |

**修正效果**：第二次实测（**§10.4**）证明两处修正都生效 —— 探针首次产出完整数据，§10.3 的 6 条验收日志**全部命中**。

### 10.3 第二次实测的验收日志（S0 的坐标，硬门槛）——**6 条已全部命中，见 §10.4**

| # | 期望日志 | 说明 |
|---|---------|------|
| 1 | `S-1: RequestItemCardInfo argCount = 0` | 应与首跑一致（§4.3 的证伪可复现） |
| 2 | `S-1: menu clip = InventoryMenu::RUNTIME_DATA::root (object)` | 缓存句柄有效 |
| 3 | `    _level0.Menu_mc.<成员> = ...` | 成员 dump；确认 `inventoryLists` 在哪一层 |
| 4 | ``S-1: inventory list found at `inventoryLists.itemList` via GetMember from the cached clip handle`` | **H1 成立**（✅ 第二次实测已命中，见 §10.4）。若 `via` 显示 "GetVariable on the absolute path (cached handle is stale)"，说明缓存句柄不可用，后续步骤须改用绝对路径 |
| 5 | `      [0] processList=yes processEntry=yes _requestItemInfo=yes   <== ItemcardDataExtender: PHASE 4 TARGET` | 确认 processor 下标 = 0 且三个成员齐备 |
| 6 | `S-1: entryList = array[6441]` | 与每轮 `RequestItemCardInfo` 次数对照 |

**只有拿到 1–6，S0（三个探针包装）才有确定坐标。** —— **已于第二次实测全部拿到，见 §10.4。**

> 若第 5 项的 `processList` 显示 `no`：prototype 成员对 `HasMember` 可能不可见 —— 改用 `GetMember` 探（同 `entryList` 的处理，这是一条已知的实现陷阱，见 §10.1 末段）。**实测未触发该情况：三个成员对 `HasMember` 全部可见。**

### 10.4 S-1 第二次实测：6 条全命中，并暴露探针自身一处判据 bug（2026-09-20 11:35–11:36）

**结论先行：S-1 通过。** §10.2 的两处修正（触发点 + 入口）都生效；**H1 成立**，且命中方式是**缓存句柄 `GetMember`**（不是绝对路径回退）→ `RUNTIME_DATA::root` 句柄**有效**，后续阶段可全程沿用；`entryList = 6441` 与既有采集（`tracy-capture-log.md` 采集 #9）自洽。同时日志暴露了探针自身一处**判据过宽**，不影响结论但会误导读者，已修正（本节末尾）。

**真机日志（S-1 部分全文，27 行）**：

```
[11:36:13.306] RequestItemCardInfo resolved: 0x7FF7DDE5E050 (SkyrimSE.exe + 0x88E050)
[11:36:13.306] RequestItemCardInfo hook installed at runtime address 0x7FF7DDE5E050 (SkyrimSE.exe + 0x88E050)
[11:36:13.306] FxDelegate::Callback hook installed at vtable 0x7FF7DED8AE38 slot 1 (was 0x7FF7DE4A6BD0)
[11:36:13.348] S-1: RequestItemCardInfo argCount = 0
[11:36:13.348] === phase 4 / S-1: ActionScript graph probe (read-only) ===
[11:36:13.348] S-1: menu clip = InventoryMenu::RUNTIME_DATA::root (object)
[11:36:13.348]     _level0.Menu_mc.itemCardFadeHolder = object
[11:36:13.348]     _level0.Menu_mc.inventoryLists = object
[11:36:13.348]     _level0.Menu_mc.bottomBar = object
[11:36:13.348]     _level0.Menu_mc.mouseRotationRect = object
[11:36:13.348]     _level0.Menu_mc.exitMenuRect = object
[11:36:13.348]     _level0.Menu_mc._searchControls = object
[11:36:13.348]     _level0.Menu_mc._bItemCardPositioned = bool
[11:36:13.348]     _level0.Menu_mc.bFadedIn = bool
[11:36:13.348]     _level0.Menu_mc._searchKey = number 57.000000
[11:36:13.348]     _level0.Menu_mc._categoryListIconArt = array[11]
[11:36:13.348]     _level0.Menu_mc._platform = number 0.000000
[11:36:13.348]     _level0.Menu_mc.itemCard = object
[11:36:13.348]     _level0.Menu_mc._switchTabKey = number 56.000000
[11:36:13.348]     _level0.Menu_mc._cancelControls = object
[11:36:13.348]     _level0.Menu_mc.navPanel = object
[11:36:13.348]     _level0.Menu_mc._quantityMinCount = number 6.000000
[11:36:13.348]     _level0.Menu_mc._acceptControls = object
[11:36:13.348]     _level0.Menu_mc._config = object
[11:36:13.348]     _level0.Menu_mc._bPlayBladeSound = undefined
[11:36:13.348]     _level0.Menu_mc._bItemCardFadedIn = bool
[11:36:13.348]     _level0.Menu_mc._switchControls = object
[11:36:13.348]     _level0.Menu_mc.PlayReverse = object
[11:36:13.348]     _level0.Menu_mc.Lock = object
[11:36:13.348]     _level0.Menu_mc.PlayForward = object
[11:36:13.348] S-1: inventory list found at `inventoryLists.itemList` via GetMember from the cached clip handle
[11:36:13.348] S-1: entryList = array[6441]  (compare against the per-round RequestItemCardInfo count)
[11:36:13.348] S-1: _dataProcessors = array[3]
[11:36:13.349]       [0] processList=yes processEntry=yes _requestItemInfo=yes   <== ItemcardDataExtender: PHASE 4 TARGET
[11:36:13.349]       [1] processList=yes processEntry=yes _requestItemInfo=no    <== ItemcardDataExtender: PHASE 4 TARGET
[11:36:13.349]       [2] processList=yes processEntry=yes _requestItemInfo=no    <== ItemcardDataExtender: PHASE 4 TARGET
[11:36:13.349] === phase 4 / S-1: probe done (27 log lines) ===
```

> 日志里的 `[1]` / `[2]` 带上 TARGET 后缀是**错误标注**（判据 bug，见本节「第三个发现」）。修正后只有 `[0]` 带此后缀；此处保留原样以如实存档。

**（1）§10.3 的 6 条验收日志 —— 全部命中**

| # | 期望 | 实测 | 判定 |
|---|------|------|------|
| 1 | `S-1: RequestItemCardInfo argCount = 0` | 完全一致 | ✅ 与首跑一致，§4.3 的 H3 证伪**可复现** |
| 2 | `menu clip = InventoryMenu::RUNTIME_DATA::root (object)` | 完全一致 | ✅ 缓存句柄有效 |
| 3 | `_level0.Menu_mc.<成员> = ...` 成员 dump | 24 行；其中 `inventoryLists = object` | ✅ `inventoryLists` 确实挂在 `Menu_mc` 上，**`_root` 入口是错的**（§4.3 结论被实测确认） |
| 4 | ``inventory list found at `inventoryLists.itemList` via GetMember from the cached clip handle`` | 完全一致（**`via` 是 `GetMember from the cached clip handle`**） | ✅ **H1 成立**，且**缓存句柄有效** → 第 2–4 条回退路径从未被触发，后续阶段可放心用 `GetRuntimeData().root` + `GetMember` |
| 5 | `[0] processList=yes processEntry=yes _requestItemInfo=yes <== PHASE 4 TARGET` | 一致（附带 `[1]`/`[2]` 误标，见 (3)） | ✅ **目标 = `_dataProcessors[0]`**；三个成员对 `HasMember` 全部可见（§10.3 末尾的担忧未发生） |
| 6 | `S-1: entryList = array[6441]` | 完全一致 | ✅ 与 `tracy-capture-log.md` 采集 #9 的 6441 自洽，基数可直接用于 S0 的 count 对照 |

**（2）`_dataProcessors` 三槽身份 —— 与源码逐条对上**

| 槽 | 实测 | 源码出处 | 身份 | phase 4 目标 |
|----|------|---------|------|-------------|
| `[0]` | `processList=yes processEntry=yes _requestItemInfo=yes` | `InventoryMenu.as:74` | `InventoryDataSetter`（`extends ItemcardDataExtender`） | ✅ **是，唯一** |
| `[1]` | `processList=yes processEntry=yes _requestItemInfo=no` | `InventoryMenu.as:75` | `InventoryIconSetter` | ❌ |
| `[2]` | `processList=yes processEntry=yes _requestItemInfo=no` | `InventoryMenu.as:76` | `PropertyDataExtender` | ❌ |

`[1]` / `[2]` 同样是 `IListProcessor` 的合法实现，所以**出现 `processList` / `processEntry` 是正常的** —— 它们只是与物品卡片往返无关。**`_requestItemInfo` 才是唯一的鉴别特征。**

**（3）第三个发现：`dumpProcessors` 的判据过宽（探针自身的 bug，已修正）**

首版判据是：

```cpp
const bool isTarget = hasProcessList && hasProcessEntry;                     // ❌ 太宽
```

于是 `[1]`（`InventoryIconSetter`）与 `[2]`（`PropertyDataExtender`）也被打了 `PHASE 4 TARGET`。修正：

```cpp
const bool isTarget = hasProcessList && hasProcessEntry && hasRequestInfo;   // ✅ 只有目标类才含 _requestItemInfo
```

| 项 | 内容 |
|----|------|
| 位置 | `src/ProfilingHooks.cpp` → `s1::dumpProcessors`（含注释补充与三槽身份留档） |
| 影响面 | **仅日志标注**。真正的目标 `[0]` 在首版日志里已被正确标出，**S-1 的数据可用性不受影响** —— 不需要重做探针，下次构建后顺带核对 `[1]`/`[2]` 已无后缀即可 |
| 对 S0 的影响 | **无**。S0 的目标坐标是 `_dataProcessors[0]`，与判据无关 |
| 修正验证 | ✅ **第三次实测（2026-09-20 11:48）已核对**：`[1]` / `[2]` 不再带 `PHASE 4 TARGET` 后缀，`[0]` 仍被正确标出 —— 修正生效且未引入新偏差。S0a 的 `install()` 复用**同一判据**核对目标（不靠下标假定，见 §10.5 (3)） |

> 这条 bug 值得留档的原因：它是「**判据比事实宽**」的典型。`processList` / `processEntry` 是 `IListProcessor` 的**通用契约成员**，**任何** processor 实现都有，因此**天然没有鉴别力**；鉴别必须落在**只有目标类才有**的成员上（`_requestItemInfo`，`ItemcardDataExtender.as:22`）。

**（4）对假设清单的影响（对应 §9）**

| 假设 | 实测前状态 | 第二次实测后 |
|------|-----------|-------------|
| **H1** | 硬门槛、**无退路** | ✅ **成立** —— `GetMember from the cached clip handle`；且比预期更好：句柄有效，**不需要**绝对路径回退 |
| **H2** | 生死假设 | 🔶 **第一步已验证**：目标对象唯一确定为 `_dataProcessors[0]`；`CreateFunction` 替换本身待 S0 —— **后续进展：S0a 真机实测（2026-09-20，5 轮）已判定为 ✅ 成立，见 §10.5(6)**（本表是 §10.4 当次快照，不代表最终状态） |
| **H3** | ❌ 首跑证伪 | ❌ **复现**（`argCount = 0`）—— 证伪的可复现性得到确认 |
| **H9** | 待验证 | 仍未验证：需要 S0 的第 1 轮 vs 第 2/3 轮 `AS::processEntry` count 对比（`skyui_itemDataProcessed` 缓存） |

**（5）S0 的坐标（S-1 的交付物）**

```
菜单 clip              = InventoryMenu::RUNTIME_DATA::root          -> GetRuntimeData().root（句柄有效）
列表对象               = <clip>.GetMember("inventoryLists").GetMember("itemList")
 ├─ entryList          = array[6441]        （count 对照基线；与采集 #9 自洽）
 └─ _dataProcessors[0] = InventoryDataSetter 实例（唯一含 _requestItemInfo 的槽）
     ├─ processList       ItemcardDataExtender.as:40   -> 4b-i 的替换目标
     ├─ _requestItemInfo  ItemcardDataExtender.as:22   -> 4a 的替换目标
     └─ processEntry      InventoryDataSetter.as:24    -> 4b-ii 的替换目标（先不要动，见 6.1）
```

**S-1 至此结束，S0（三个探针包装）可以开始。** 一致性自检基线：`AS::processEntry` count == **全量路径** `RequestItemCardInfo` count（预期 19323 = 6441 × 3；口径说明见 §4.2「双路径」）。

### 10.5 S0a 落地：`_requestItemInfo` 转发探针（2026-09-20，**代码就绪 → 真机实测通过**）

**范围**：只做 §4.2 拆分中的 **S0a** —— 在 `_dataProcessors[0]` 上用 `GFxMovie::CreateFunction` 造一个 C++ 函数对象、`SetMember` 覆盖 `_requestItemInfo`；函数体**只做两件事**：打一个 `SSE_ZONE("AS::_requestItemInfo")`，再把调用**原样转发**给覆盖前保存下来的原函数。**不含缓存、不含任何优化** —— 这一版的价值全部在「证明转发本身可行且无损」。

#### （1）代码坐标

| 项 | 位置 |
|----|------|
| 声明（含设计理由） | `src/ProfilingHooks.h:88-138` —— `requestAsProbeInstall()` / `maybeInstallAsProbe()` |
| 挂点 1（低频，**只上膛**） | `src/ProfilingHooks.cpp:697-707` —— `MenuOpenCloseListener` 的 `opening` 分支调用 `ProfilingHooks::requestAsProbeInstall()`，**只置一个标志**，不碰对象图 |
| 挂点 2（高频，**只读一个 bool**） | `src/ProfilingHooks.h:452-484`（`maybeInstallAsProbe` 的调用点在 `:474`）—— `RequestItemCardInfoHook::hook` 里 `maybeInstallAsProbe(a_params.GetHandler())`，与 `probeDelegateArgs` / `installAsPathProbe` 并列、互不依赖 |
| 实现 | `src/ProfilingHooks.cpp:198-665` —— `s0` 命名空间（3 个 handler + `resolveTargetProcessor` + `install`）；S0b 后三者各占一段，明细见 §10.6(1) |
| 安装 / 重试 | `src/ProfilingHooks.cpp:1072-1134` |
| 回退（§8.2） | 原函数在覆盖前被 `GetMember` 取出，存进 handler 的 `RE::GFxValue _original` 成员（**值拷贝，不是裸指针** —— `GFxValue` 是引用计数对象，`GFxValue.h:288-300`），S1 可原样还原 |

> 上表行号为 **S0b 落地后**的当前值；S0a 时期分别是 `h:88-128`、`cpp:482-491`、`h:456-464`、`cpp:198-450`、`cpp:856-917`。刷新行号不影响 S0a 的设计与实测结论（§10.5(2)–(6)）；S0b 新增的代码坐标见 §10.6(1)。

**为什么挂点要分成两个。** 安装要做 5–6 次 `GetMember` / `HasMember` / `GetElement`，若直接跑在 19323 次的热路径上，**探针自身的开销会污染它要测的东西**。所以 `opening` 事件只负责上膛（置 `g_installPending`），真正的图操作发生在该轮的**第 1 次** `RequestItemCardInfo` 内 —— 也就是重建后的列表**保证已存在**的时刻（S-1 已实测，§10.4）。热路径上剩下的只有一个 bool 读。

**每轮重新上膛是必需的，不是优化。** 列表对象与其中的 processor 实例**每开一次背包全部重建**，所以第 1 轮做的替换在第 2 轮已经不存在。这也是 S-1 的 `installAsPathProbe` 能做成 one-shot、而本步不能的原因。

#### （2）§4.2 要求的三个语义如何被一次调用同时满足

转发只用一次 `Invoke`：

```cpp
_original.Invoke("call", a_params.retVal, a_params.argsWithThisRef,
                 static_cast<RE::UPInt>(a_params.argCount) + 1);
```

| §4.2 的要求 | 由哪个参数承载 |
|------------|---------------|
| ① `this` 绑定 | `argsWithThisRef` 的**第 0 个元素就是** `thisPtr`，而 `call` 是 `Function.prototype.call`（`fn.call(thisArg, arg0, …)`）→ 原函数看到的 `this` 与替换前完全一致 |
| ② 参数个数与内容 | `argsWithThisRef` 的第 1..n 个元素 = `args[]` 全体；长度即 `argCount + 1` |
| ③ 返回值 | `a_params.retVal` 直接作为 `Invoke` 的 out 参数 |

**`this` 到底是谁**：本调用点是 `_requestItemInfo.apply(a_list, [this, i])`（`ItemcardDataExtender.as:55`），所以 `thisPtr` = **`a_list`（TabularList）**、`args[0]` = processor、`args[1]` = index。这与 §5.1「三方宿主」的读法一致，而探针会把**实测形状**打进日志（判据 3）—— 该表由此拿到第一条真机证据，而不是继续停留在源码推断。

#### （3）失败安全（§8.2 规则 4）：每条失败路径都在第一次写入之前返回

| 步骤 | 失败时的行为 |
|------|------------|
| `GetRuntimeData().root` 不是对象 | 返回 false（**保持上膛**，下一项重试） |
| `inventoryLists.itemList` 解析失败 / 不是列表 | 同上 |
| `_dataProcessors` 不是数组 / `GetElement(0)` 失败 | 同上 |
| `_dataProcessors[0]` **不含** `_requestItemInfo` | 同上 —— **复用 §10.4 的判据核对目标**，不靠下标假定 |
| `uiMovie` 为空 | 同上 |
| `GetMember("_requestItemInfo")` 失败 | 同上 |
| `CreateFunction` 未产出函数对象 | `logger::warn` 并返回 false（**不替换**） |
| `SetMember` 失败 | `logger::warn` 并返回 false（原值未变） |

因此不可能留下「半个探针」：要么全部成功，要么目标对象与替换前**逐字节相同**。

重试有**上限**（`kMaxInstallAttempts = 4`）。唯一合法的失败原因是「列表还没填完」，通常在前几项内就消失；超过上限则本轮放弃并 `warn`。**无上限重试是个陷阱**：真失败时它会把 19323 次调用放大成 19323 × 6 次 GFx 调用 —— 那正是这个探针存在要避免的测量污染。

#### （4）handler 的生命周期：为什么**故意不** `Release()`

`GFxFunctionHandler` 是引用计数对象，`GRefCountImplCore::_refCount` **初始为 1**（`GRefCountImplCore.h:18`），即 `new` 本身已经代表一个持有引用；Scaleform 的 `CreateFunction` 契约**另外**加一个。于是：

- **`Release()`**：只在该契约确实成立时才正确。若不成立，`_refCount` 由 1 → 0 → **立即析构** → 函数对象随即持有悬空 handler → 之后每次调用都崩（而且崩在 19323 次的热路径中间，极难归因）。
- **不 `Release()`**：两种假设下都安全。最坏结果是每轮一个约 48 字节的对象活到进程结束 —— 3 轮共约 144 字节，且只存在于 `TRACY_ENABLE` 构建。

> 取舍写在这里而不是埋在代码里：**一个可能崩溃的探针，比 144 字节的泄漏危险得多。** 代码中的同一说明见 `RequestItemInfoHandler` 所在的三-handler 类注释（`src/ProfilingHooks.cpp:410-435`）。

跨轮**共享**一个 handler 本可把泄漏压到单个对象，但被否掉了：那会让上一轮的函数对象看到被改写的 `_original`（可变状态跨轮可见）。每轮新建 = 每个函数对象一个专属 handler，`_original` 永不变。

#### （5）构建验证（本机，2026-09-20）

| 配置 | 产物 | 警告 / 错误 |
|------|------|-----------|
| `ENABLE_TRACY=ON`（RelWithDebInfo） | `build/bin/RelWithDebInfo/Template.dll` = **762 880 字节** | **0**（`/W4 /WX`） |
| `ENABLE_TRACY=OFF`（RelWithDebInfo） | 同路径 = **612 352 字节** | **0** |

差额 **150 528 字节** = Tracy 客户端 + 探针的全部代码，即 `TRACY_ENABLE` 关闭时探针代码**被完整排除**（无泄漏）。`OFF` 一侧的 **612 352** 与 §10.2 记录的 S-1 基线**逐字节相同** —— 这直接证明 S0a 新增的代码在关闭时被 100 % 排除，一行都没漏进正式版。验证后已把配置恢复为 `ON` 并重新构建（762 880 字节，与上表一致）。

> **配置口径（重要）**：本节数字用 **RelWithDebInfo**（`cmake --build build --preset relwithdebinfo`），与 §10.1–§10.4 及附录 A 的历史字节数**同口径**，且真机排障需要 PDB。首次构建误用了 `release` preset（得 750 080 / 610 304），两个配置的产物**不能直接比较**，故已按 RelWithDebInfo 重测并以此表为准。
>
> ON 相对 S-1 基线 **753 152** 的增量 = **+9 728 字节**，即 S0a 探针（handler 类 + 5 条日志字符串 + 格式化缓冲）的全部体积 —— 与「一个纯转发器 + 一次 `Invoke`」的预期量级一致。

#### （6）验收判据 —— **真机已实测：判据 1–6 全部命中（2026-09-20，5 轮）**

| # | 判据 | 说明 |
|---|------|------|
| 1 | 日志出现 `=== phase 4 / S0a: \`_requestItemInfo\` probe received its first call ===` | ✅ 命中（第 1 轮首次调用即打印） |
| 2 | 日志出现 `S0a: \`_dataProcessors[0]._requestItemInfo\` = C++ probe (pure forwarder…)` | ✅ **5 次**（每轮 `opening` 后安装一次） |
| 3 | `S0a: thisPtr (apply's thisArg) = object`、`argCount = 2`、`args[0] = object`、`args[1] = number` | ✅ 四项全中（`thisPtr` = TabularList、`args[0]` = processor、`args[1]` = 循环下标；详见下方实测结果） |
| 4 | `AS::_requestItemInfo` **不丢调用**（**全量路径** count == 全量 `RequestItemCardInfo` count − 轮数，**相对式**；口径见 §4.2「双路径」） | ✅ **分解后成立**：`32201 = 6440 × 5 + 1`，全量路径差额**恰 = 轮数 5** |
| 5 | `RequestItemCardInfo` **未被破坏**（全量路径每轮 count 不变） | ✅ **分解后成立**：全量路径仍 **6441 / 轮**（5 轮 = `6441×4 + 6442×1` = 32206） |
| 6 | **物品卡片数值与 skyui 原版一致**（重量 / 价值 / 伤害 / 护甲 / 附魔等，目视抽查 ≥ 10 件） | ✅ **数值一致、无错乱** —— 转发**语义无损**（`retVal` + `this` + args 三者都对） |

> 判据 4 的差额**不是容差，而是可解释的确定值**（§4.2 表）：探针安装发生在该轮第 1 次调用的**内部**，那一次已经用旧函数对象发起。差额必须**恰好**等于轮数；若随轮数漂移或明显偏大，说明探针在丢调用 → 数据作废。
>
> ⚠️ **但「差额 = 轮数」只在「全程不滚动、不悬停」的纯 opening 场景成立。** 本次采集用户中途滚动过列表，实测差额是 **41 ≠ 5** —— 逐项分解后证明这 41 **完全可解释**，**不是丢调用**（见下表）。

##### 实测对账（2026-09-20，`TracyLog/log_2026_09_20_15_29.tracy`）

**采集坐标**：5 轮开背包 + 中途滚动若干次；分析工具 `extern\TracyProfiler\release\tracy-csvexport.exe`（`-f` 取汇总、`-u` 取逐事件）。

**判据 1–6 实测结果**

| # | 判据 | 实测 |
|---|------|------|
| 1 | first call 日志 | ✅ 命中（第 1 轮首次调用即打印） |
| 2 | 安装日志（`= C++ probe (pure forwarder…)`） | ✅ **5 次** —— 每轮 `opening` 后重新上膛并安装一次，与「每轮重新上膛」的设计一致（§10.5 引子） |
| 3 | `thisPtr` / `argCount` / `args[0]` / `args[1]` | ✅ 四项全中：`thisPtr = object`、`argCount = 2`、`args[0] = object`、`args[1] = number 1.000000` → §5.1「三方宿主」表拿到**真机证据**（`thisPtr` = TabularList、`args[0]` = processor、`args[1]` = 循环下标） |
| 4 | `AS::_requestItemInfo` **不丢调用** | ✅ **分解后成立**：全量路径差额**恰 = 轮数 5** |
| 5 | `RequestItemCardInfo` **未被破坏** | ✅ **分解后成立**：全量路径仍 **6441 / 轮** |
| 6 | 卡片数值与 skyui 原版一致 | ✅ 实测确认（重量 / 价值 / 伤害 / 护甲 / 附魔等正常显示，无错乱） |

> ⚠️ **日志前缀已随 S0b 变更（2026-09-20，同日）**：上表与本节其余处引用的字符串来自 **S0a 时期的构建**（探针日志前缀 `S0a:`，安装日志只提 `_requestItemInfo`）。S0b 落地后前缀统一为 **`S0:`** —— 安装是三个探针**原子**完成的，单独标 `S0a` / `S0b` 都不准确 —— 安装日志也改为一次列出三者：`S0: \`_dataProcessors[0]\` wrapped: \`_requestItemInfo\` / \`processList\` / \`processEntry\` = C++ probes (pure forwarders: no cache, no optimization)`。**判据 1/2 的语义完全不变**（仍有 first-call 逐项 dump、仍每轮一次安装日志），只是字面前缀变了；本节作为**历史快照**保留原文，变更记录见 **§10.6 (4)**。

**总量对账（差 41 的逐项分解）**

| 量 | 值 | 构成 |
|----|----|------|
| `RequestItemCardInfo` 总 count | **32242** | 全量 `6441×4 + 6442×1` = **32206**，**+ 36** 次悬停/事件路径 |
| `AS::_requestItemInfo` 总 count | **32201** | 全量 `6440×5` = **32200**，**+ 1**（滚动新增的 1 个条目，探针已装 → 计入） |
| **差** | **41** | **5**（全量路径：每轮第 1 次走旧函数）+ **36**（悬停路径：不经探针） |

> `41 = 5 + 36`：其中 **5 是可解释的确定值**（§4.2 表），**36 属于另一条路径**（§4.2「双路径」），与探针**无关**。

**逐轮明细**（`tracy-csvexport -u` 逐事件，按 > 1 s 间隔分簇；时间为 ms，相对 trace 起点）

| 簇 | 时间（ms） | `RequestItemCardInfo` | `AS::_requestItemInfo` | 说明 |
|----|-----------|----------------------|------------------------|------|
| 轮 1 | 87126→87975 | 6441 | 6440 | 全量；漏 1（第 1 次走旧函数） |
| 轮 2 | 91515→92365 | 6441 | 6440 | 同上 |
| 轮 3 | 94701→95548 | 6441 | 6440 | 同上 |
| 轮 4 | 98561→99383 | 6441 | 6440 | 同上 |
| 滚动 a–e | 103773 / 105616 / 114770 / 118453 / 120907 | 2 / 19 / 9 / 2 / 2 | 0 / 0 / 0 / **1** / 0 | 悬停路径（`onItemHighlightChange`）；仅 118453 那 1 次是「滚动新增条目」走了全量 |
| 轮 5 | 126145→127324 | 6442 | 6440 | 全量 6441（漏 1）+ **1 次悬停**（`opening` 后立即高亮某件） |
| 滚动 f | 131222→131604 | 2 | 0 | 悬停路径 |

**单次成本**（本次采集）

| zone | count | mean | 读法 |
|------|-------|------|------|
| `RequestItemCardInfo` | 32242 | **69.4 µs** | 与 §10.4 / 采集 #9 的 64.4 µs 同量级 |
| `FxDelegate::Callback[RequestItemCardInfo]` | 32242 | **69.7 µs** | 边界 ≈ 0.3 µs/次，与采集 #9 一致 |
| `AS::_requestItemInfo` | 32201 | **77.8 µs** | 比 `RequestItemCardInfo` 多 **8.4 µs** = `_requestItemInfo` 自身 AS 开销 + `apply` + 探针转发 —— 即 **S1+S2 的轮级口径下限**（详细分解留给 S0b） |

**结论**：`41 = 5 + 36` 逐项可解释 —— **全量路径差额恒定 = 轮数**（判据 4 ✅）、**全量路径每轮 count 不变**（判据 5 ✅），**探针没有丢调用**。

**两处方法论收获（后续采集必须沿用）**

1. **`-u`（逐事件）+ 按时间分簇是 count 自检的必要手段**：`-f` 的汇总 count 只能给出总数，**无法**把「悬停路径」与「探针丢调用」区分开 —— 两者表现都是「`RequestItemCardInfo` 比 `_requestItemInfo` 多」。只有分簇后才能看出「多出来的是 2 / 19 / 9 / 2 / 2 这种零散小簇」而非「某一轮整批缺失」。
2. **采集期间的行为会污染 count 自检**：滚动/悬停会引入悬停路径调用。若要最干净地验收 count，应**只做「开背包 → 关闭 → 再开」的纯 opening 循环、不碰鼠标**。本次的滚动恰好换来了一个**额外的架构发现**（双路径，§4.2），属意外收获，但不能指望每次都这么走运。

> **复现要点**：`entryList` 仍为 **6441**，说明本次是「加了 10 个**堆叠**物品」的存档（entry 数不变，只是某个 entry 的 count +10），与「全量 6441 次/轮」一致。**轮数 5 由日志中 `= C++ probe` 的出现次数确认，不是推断。**

**H2 判定：✅ 成立**（2026-09-20 真机，5 轮）—— 判据 1–3 与 6 **同时成立** → `CreateFunction` 的产物**可被 AS2 的 `apply` 正常调用**；判据 4/5 在**按路径分解后**成立（全量路径差额恰 = 轮数、全量路径 count 未被改动）→ **探针不丢调用、不破坏既有测量**。**S0b 可以继续**（补齐 `processList` / `processEntry`，由嵌套关系解出 S1–S5）。

> 证伪条件（本次均未触发）：AS 侧抛错、卡片数值错乱、或探针 count 明显偏小 → H2 **证伪** → **退回 3a′**（§7.1），阶段 4a / 4b 均不再尝试。





---

### 10.6 S0b 落地：`processList` + `processEntry` 转发探针（2026-09-20，**代码就绪 + 构建验证 ✅ → 真机实测 ✅，见 §10.7**）

#### （1）代码坐标

| 项 | 位置 |
|----|------|
| 声明 | `src/ProfilingHooks.h:88-138`（`requestAsProbeInstall` / `maybeInstallAsProbe`；含「三个探针原子装入」与「每轮重新上膛」的理由） |
| 成员表与槽位 | `src/ProfilingHooks.cpp:263-297` —— `enum Slot` + `kProbeMembers[3]`，**唯一真源**：`install()` 按表循环，三者不会各自漂移 |
| 一次性报告 | `:333-341`（`g_probeReported[3]` / `claimFirstReport`）+ `:367-378`（`reportFirstCall`，**逐个探针** dump，不再是单一布尔） |
| 转发器 | `:405-408`（`forwardVerbatim`）—— 三者共用同一个 `Invoke("call", …)` |
| 三个 handler | `:439-455`（`AS::_requestItemInfo`）/ `:460-472`（`AS::processList`）/ `:481-493`（`AS::processEntry`） |
| 工厂 | `:508-521`（`makeProbe`，按槽位 `switch` 建函数对象） |
| 路径解析 | `:536-574`（`resolveTargetProcessor`，**未改动**，仍以 `_requestItemInfo` 作判别式） |
| 安装 | `:585-590`（`restoreMembers`）+ `:612-663`（`install`，三阶段 + 回滚） |
| 上膛 / 热路径 | `:1072-1083`（`requestAsProbeInstall`）+ `:1089-1134`（`maybeInstallAsProbe`） |
| 挂点 | `src/ProfilingHooks.h:474` —— 仍是 `RequestItemCardInfoHook::hook` 里的**第三个** one-shot，排在 S-1 两个之后（日志预算优先） |

#### （2）与 S0a 的关键差异：三个调用点的 `this` 不在同一个对象上

三者的 AS 调用形态不同，`Params::thisPtr` 因此**不同** —— 这是 S0b 唯一新增的语义风险点，也是每个探针都保留 first-call dump 的原因（不靠源码推断，靠实测确认）：

| 探针 | AS 调用点 | 调用形态 | `thisPtr` | `argCount` / `args` |
|------|-----------|---------|-----------|---------------------|
| `AS::_requestItemInfo`（S0a） | `ItemcardDataExtender.as:55` | `_requestItemInfo.apply(a_list, [this, i])` | **list**（`TabularList`，`_selectedIndex` 的宿主） | 2 / `[processor, index]` |
| `AS::processList`（S0b） | `BasicList.as:276` | `_dataProcessors[i].processList(this)` | **processor** | 1 / `[list]` |
| `AS::processEntry`（S0b） | `ItemcardDataExtender.as:56` | `processEntry(e, _itemInfo)` —— 即 `this.processEntry(...)`，此处 `this` 就是上一层传入的 processor | **processor** | 2 / `[entry, itemInfo]` |

> **为什么同一个 `Invoke("call", …)` 对三者都成立**：`SetMember` 装的是 processor 的**自有属性**，而 AS2 的属性查找**先查自有、再查原型链**。`processList` / `processEntry` 是 prototype 方法，所以探针只**遮蔽该实例** —— 这正是想要的粒度，也是整个探针按「每 processor 实例」而非按类设计的原因。而 `Params::argsWithThisRef` 恒为 `[thisPtr, args...]`（`GFxFunctionHandler.h:21`），配 `Function.prototype.call` 一次就同时还原了 `this` 绑定、实参列表与返回值。

#### （3）为什么三者必须**原子**装入（而不是逐个尽力而为）

`S3 = AS::processList − Σ(AS::_requestItemInfo) − Σ(AS::processEntry)` 是一个**三总量之差**。因此部分安装的后果**不是「少一个数」，而是「多一个错数」** —— 少了 `processEntry` 区间的读数，S4 会被**静默计入 S3**，而 S3 恰好是决定「4b-i 是否值得做」的那个量（§6.1）。一个看起来合理、实际是 S3+S4 的数字，比缺一个数字危险得多。

所以 `install()` 是**全成或全不成**，且这不只是安全约定，更是**数据有效性的前提**。实现上分三阶段，使只有最后一相可能留下中间态：

| 阶段 | 动作 | 失败后果 |
|------|------|---------|
| 1 | 先把三个原成员 `GetMember` 快照到 `originals[3]` | **尚未写入任何东西** → 目标对象与替换前**逐字节相同** |
| 2 | 用 `makeProbe` 建三个函数对象到 `probes[3]` | 同上 |
| 3 | 逐个 `SetMember` 写入 | 这是唯一可能半装的阶段 → **按快照回滚**（`restoreMembers`，失败则 `logger::error` 明示「已半装」） |

> 这条把 §10.5 (3) 当时的承诺（「要么全部成功，要么目标对象与替换前逐字节相同」）**从单探针推广到三探针**，且推广后依然成立。

#### （4）本次对既有记录的**字面**变更（§10.5 已就地标注）

S0b 改的是**日志前缀与安装日志的写法**，语义未变；但 §10.5(6) 是逐字引用日志的**历史快照**，所以必须显式记录，否则下次照 §10.5 找字符串会找不到：

| 项 | S0a 时期（§10.5(6) 引用） | S0b 之后 |
|----|---------------------------|---------|
| 探针 first-call 日志 | `=== phase 4 / S0a: \`_requestItemInfo\` probe received its first call ===` | `=== phase 4 / S0: \`{成员名}\` probe received its first call ===` —— 前缀统一为 `S0:`，且**按成员名区分**（三个探针各打一次） |
| 安装日志 | `S0a: \`_dataProcessors[0]._requestItemInfo\` = C++ probe (pure forwarder: no cache, no optimization)` | `S0: \`_dataProcessors[0]\` wrapped: \`_requestItemInfo\` / \`processList\` / \`processEntry\` = C++ probes (pure forwarders: no cache, no optimization)` —— **一条日志列全三者**，因为安装是原子的 |
| 重试日志 | `S0a: target not reachable yet (attempt {} of {})` / `S0a: \`_dataProcessors[0].{}\` unreachable after N attempts` | 同样改为 `S0:` 前缀，且不可达日志不再点名单个成员（三个一起放弃） |

> **为什么前缀统一为 `S0:` 而不是 `S0a:` / `S0b:` 分开**：安装是三者**原子**完成的（(3)），那条安装日志既不属于 S0a 也不属于 S0b，标任何一个都会误导。单个探针的身份已经由 first-call 日志里的**成员名**表达，比前缀更精确。

#### （5）失败安全（§8.2 规则 4）—— 逐条路径复核

| 失败点 | 行为 |
|--------|------|
| 路径任一段解析失败（`resolveTargetProcessor`） | 直接返回，**未写入** |
| `uiMovie` 为空 | 直接返回，**未写入** |
| 三个成员中**任一** `GetMember` 失败 | 阶段 1 返回，**未写入**，日志点名是哪一个 |
| 三个函数对象中**任一** `CreateFunction` 返回非 object | 阶段 2 返回，**未写入**，日志点名是哪一个 |
| `SetMember` 中途失败 | **回滚已写入的那些**；回滚本身再失败才 `logger::error` 报「已半装」 |
| 挂载点未找到 | 最多重试 `kMaxInstallAttempts = 4` 次（跨 item），超出则放弃本轮并告警 |

> 与 S0a 的唯一区别是多了「回滚」这一分支 —— 因为「第一次写入之后」不再是返回点。其余路径仍然**全部在第一次写入之前**返回，这一性质未被削弱。

#### （6）构建验证（本机，2026-09-20）

| 配置 | 产物（RelWithDebInfo） | 警告 / 错误 |
|------|----------------------|-----------|
| `ENABLE_TRACY=ON` | `Template.dll` = **767 488 字节** | **0**（`/W4 /WX`） |
| `ENABLE_TRACY=OFF` | 同路径 = **612 352 字节** | **0** |

- **`OFF` 一侧与 §10.5(5) 记录的 S-1 基线 `612 352` 逐字节相同** → S0b 的全部新增代码在 `TRACY_ENABLE` 关闭时被 **100 % 排除**，对正式版**加 0 字节**（这一点比 `ON` 一侧的增量更有意义）。
- `ON` 一侧相对 S0a 的 `762 880` 增量 = **+4 608 字节**（= 两个 handler 类 + 3 个 zone 名字符串 + 日志格式串），量级与「两个纯转发器」相符。
- **字符串核对**（直接读 DLL 字节）：`AS::_requestItemInfo` / `AS::processList` / `AS::processEntry` 在 `ON` 版**全部 FOUND**、在 `OFF` 版**全部 MISSING**。
- 验证后已把配置恢复为 `ON` 并重建（`ENABLE_TRACY:BOOL=ON`，767 488 字节）。

> 中途遇到并修掉的唯一编译错误：`forwardVerbatim` 最初把原函数声明为 `const RE::GFxValue&`，而 `GFxValue::Invoke` 是**非 const 成员**（`GFxValue.h`）→ `error C2663`。改成 `RE::GFxValue&`，并在注释里写明「探针一个字都不改，只是 API 要求非 const」。S0a 时期没暴露这一点，是因为当时的 `_original` 作为非 const 成员、在非 const 的 `Call` 里被调用。

#### （7）判据清单（真机，采集 #11b）—— **实测结果见 §10.7(1)：1–6 全中，第 8 条部分失败**

采集要求与 §4.2「双路径」一致：**纯 opening 的开 / 关循环，全程不滚动、不悬停**（否则悬停路径会污染 count 自检）。

| # | 判据 | 预期 |
|---|------|------|
| 1 | 三个探针的 first-call 日志各出现一次 | 3 条，成员名分别为 `_requestItemInfo` / `processList` / `processEntry` |
| 2 | 安装日志每轮一次 | 共 = 轮数（与 S0a 的 5 次同构） |
| 3 | `thisPtr` 语义分叉被实测确认 | `_requestItemInfo` → list；`processList` / `processEntry` → processor（见 (2) 表） |
| 4 | **`AS::processEntry` count == 全量路径 `RequestItemCardInfo` count**（**严格相等**，相对式） | 相等；不等则说明包装改了行为，**数据作废** |
| 5 | `AS::processList` count == 轮数 − 1 | ⚠️ **数值对上（4 == 5 − 1），但成因不同**：捕获到的是各轮**之后**的空转调用，**全量循环一次都没被覆盖**（§10.7(3)） |
| 6 | `AS::_requestItemInfo` count == 全量路径 count − 轮数 | 同 S0a（§10.5(6) 判据 4） |
| 7 | 卡片数值与 skyui 原版一致 | 同 S0a 判据 6 |
| 8 | **由嵌套解出 S1–S5** | ❌ **部分失败**：S1+S2 与 S4 解出，**S3 不可解 → 剔除**（§10.7(3)(4)）；**4b-i 因此跳过**（§6.4） |

> ⚠️ **已知覆盖缺口（沿用 §10.5(6) 与附录 A 的结论）**：悬停路径（`ItemMenu.as:357` `onItemHighlightChange`）**不经过任何一个探针**。若真机上**滚动**造成的卡顿显著，则 S1–S5 这套分解**不覆盖**它，需要**新增插入点**。（**S0b 数据已出**：采集 #11b 是**零滚动、零悬停**的 5 轮纯 opening，故该缺口本次未显现；悬停路径**仍未插桩** —— 见 §10.7(6)。）

### 10.7 S0b 真机实测（采集 #11b，2026-09-20 18:34–18:35）—— **判据 1–6 全中，但 S3 不可解**

> 原始数据、聚合表与逐轮分簇见 [tracy-capture-log.md](./tracy-capture-log.md) **采集 #11b**。本节只记录结论与对 §4.2 / §6 / §9 的修正。

#### （1）验收结果：判据 1–6 全部命中，第 7 条推定，第 8 条**部分失败**

| # | 判据 | 实测 |
|---|------|------|
| 1 | 三个 first-call 日志各一次 | ✅（`processEntry` 15.873 / `_requestItemInfo` 15.874 / `processList` 20.677） |
| 2 | 安装日志 = 轮数 | ✅ **5** |
| 3 | `thisPtr` 语义分叉 | ✅ `_requestItemInfo` → **list**；`processList` / `processEntry` → **processor**；`argCount` = 2 / 1 / 2 —— 与 §10.6(2) 的表**逐项一致**（不再依赖源码推断） |
| 4 | `AS::processEntry` count == 全量路径 count | ✅ **32205 == 32205**，且**逐轮** 6441 == 6441（**本设计最严格的假阳性检测器通过**） |
| 5 | `AS::processList` count == 轮数 − 1 | ⚠️ 数值 **4 == 5 − 1** 对上，但**成因与预期不同**（见 (3)） |
| 6 | `AS::_requestItemInfo` count == 全量路径 count − 轮数 | ✅ **32200 == 32205 − 5**，且**逐轮** 6440 == 6441 − 1 |
| 7 | 卡片数值与 skyui 原版一致 | ✅（推定 —— 三个探针均为**纯转发**、零缓存零改写，机制与 S0a 同构） |
| 8 | **由嵌套解出 S1–S5** | ❌ **部分失败**：S1+S2 与 S4 解出，**S3 不可解 → 剔除**（见 (3)/(4)） |

**判据 4 的机制再获独立确认**：`processEntry` 的 first-call **早于** `_requestItemInfo` 1 ms（15.873 vs 15.874）—— 与 §4.2 拆分表推导的「`:55` 的往返返回后才调 `:56` 的 `processEntry`」精确一致：每轮 **6441 个 `processEntry`**（含第 1 项）、**6440 个 `_requestItemInfo`**（第 1 项走旧函数）。

#### （2）H4 成立：全部 AS / C++ zone 在**同一线程**

逐事件 `thread` 列核对：`_requestItemInfo` / `RequestItemCardInfo` / `processEntry` / `FxDelegate::Callback[RequestItemCardInfo]` **全部 = 10**，无跨线程事件 → **AS 与 C++ zone 可直接相减**，无需退化为「轮级」对比。

嵌套也逐事件核对通过：**32200 个 `RequestItemCardInfo` 落在某个 `_requestItemInfo` 窗口内，仅 5 个在外**（= 每轮第 1 项）。

#### （3）**核心发现：`processList` 探针测到的是「空转」** —— §4.2 第三行方程作废

`AS::processList` 只有 **4 次**事件（第 2–5 轮各 1 次），各 **3.6 ms**，而**窗口内子事件数全部为 0**（`_requestItemInfo` / `processEntry` / **乃至 `RequestItemCardInfo`** 都是 0）。这 4 次紧跟在各轮全量窗口**之后**（起点后 ≈ 1.01 s / 结束后 ≈ 0.16 s）→ 它们是**空转** `processList`（`skyui_itemDataProcessed` 已全 `true`，6441 项全 `continue`）。

**每轮的全量 `processList`（6441 项、约 0.85 s）从未被覆盖** —— 它是安装点（第 1 项 `RequestItemCardInfo`）所在的**同一次**调用：探针装上时，全量循环**已经在执行中**。因此：

> `S3 = processList − Σ(_requestItemInfo) − Σ(processEntry)` **符号为负**（14.34 ms − 4022 ms）→ §4.2 的第三行**作废**，**S3 剔除**。（§4.2 已就地加了修正块。）

**为什么「原子装入」的设计仍然必要**：正因为 S3 = 三总量之差，若只装了部分探针，S4 会被静默计入 S3，产出「看似合理、实则错」的数。本次三者全装，才能得出「S3 不可解」这个**确定**结论，而不是拿到一个 S3+S4 的假数。

#### （4）S1–S5 分解（每轮 6441 项；S3 剔除）

| 段 | 每轮 | 占比 | 消掉它的阶段 |
|----|------|------|-------------|
| `RequestItemCardInfo` **C++ 实现** | **443.4 ms** | **55.1 %** | 4a |
| `_requestItemInfo` 体 + `apply` + 转发 | **57.9 ms** | **7.2 %** | 4a |
| **S4 = `processEntry`（855 行 AS）** | **303.3 ms** | **37.7 %** | **4b-ii** |
| S3（`processList` 全量循环本体） | **不可测** | — | **剔除** |
| S5（`invalidate` + `UpdateList`） | 未测 | — | P4（§6.3） |
| **合计** | **≈ 804.6 ms/轮** | 100 % | — |

**对 §4.1 开篇疑问的最终回答**：§4.1 曾指出「47 % ÷ 6441 ≈ 56.5 µs/件，与 C++ 侧 64.4 µs/件同量级，可疑 → 大概率是 `ExternalInterface` 编组」。**实测把这个猜测彻底推翻**：那 47 % 里 **≈ 84 % 是 `processEntry`**（303.3 ms ÷ AS 侧小计 361.2 ms，纯 AS，855 行），**真正的边界编组 ≈ 0**（靶点 2.5）。S1+S2 只在轮级留下 **57.9 ms（7.2 %）**。

> **口径提示**：本轮实测的 **AS 侧小计 = 57.9 + 303.3 = 361.2 ms/轮**（对应 §1 里「AS 层 ≈ 364 ms」的旧估计）；**C++ 侧 = 443.4 ms/轮**（旧估计 414 ms）；两者之和 **804.6 ms/轮**，与全量窗口 span 826–872 ms/轮一致。
>
> ⚠️ **2026-09-21 采集 #13 补充（上表数值不改）**：这 **303.3 ms/轮** 里 **≈ 14 ms/轮** 是 **AS2 堆的 mark-sweep 暂停**（#13 逐轮 ≥ 5 ms 事件合计 **14.26 ms/轮**；#12 = 13.76；#11b 全价语料 = 21.40）→ 4b-ii 的**可消上界 ≈ 289 ms/轮 ≈ 36 %**。暂停会不会随 C++ 化缩小 / 移位**未定**，须在 **B1 原型**实测（判据 4 裁定 H-GC，全文见 **§10.8(12) 第 6 点**）。

#### （5）对 §6 / §9 的修正

- **§6 新增 §6.4 实测裁决**：原「先做 4b-i」被推翻（其收益 S3 不可测且 ≤ 数十 ms）；执行顺序改为 **4a → 4b-ii**，**跳过 4b-i**；
- **H4 成立**；**H5 判否**（S1+S2 非主导，S4 主导）；**H6 间接支持**（804.6 ms/轮已解释全量窗口 span 的 92–97 %，S5 ≤ 67 ms/轮、< 8 %）；**H9 沿用 #11a 判假**（`processEntry` 每轮 6441 次）。

#### （6）遗留与后续

| 项 | 说明 |
|----|------|
| 【待查】`processEntry` 离群值 **14.59 ms** | 某个 `formType` 分支在个别物品上极贵（mean 47.1 µs 的 310 倍）；对总账 < 0.5 %，建议 4b-ii 前定位。**定位方法**：`-u` 导出 `AS::processEntry` 的 max 事件时间戳，反查该轮该序号的物品 / 其 `formType` |
| 【待查】每轮一次「空转 `processList`」的**触发源** | 候选：`BasicList.requestInvalidate` → `setInterval(commitInvalidate, 1)`（`BasicList.as:208-250`）或 `BSScaleformManager::InitExtensions`；本次未深究 —— 用户已决定**不纳入分析**（不可控、无稳定语义），**不影响 S1–S5** |
| 【待查】`fixSKSEExtendedObject` 的**实际代价** | 它是 S3 的一部分（`processList` 全量循环体内，S3 已剔除）→ **本探针无法测**。若要测：需在全量循环**之前**安装（即把安装点从第 1 项 `RequestItemCardInfo` 前移到 `MenuOpenCloseEvent` 的 `opening` 分支 —— 但那已验证**太早**，见 §10.2）或在 C++ 侧给它独立 zone |
| 悬停路径未覆盖 | 沿用 §4.2「双路径」/ 附录 A 的缺口（全量路径占 99.9 %） |
| **下一步** | 进入 **4a**（替换 `_requestItemInfo`，首轮建缓存、后续命中），采集 #12 验收 |

### 10.8 S1（=4a）落地：`_requestItemInfo` 热替换（2026-09-21，**已真机实测通过** —— 判据 1–6 全中，见 (10) 与 [tracy-capture-log.md](./tracy-capture-log.md) §12）

> 本节是**实现记录**（SDD 文档 §5 的落地版）。与 §10.5 / §10.6 / §10.7 的差别只有一处：那三节记的是「探针（只转发）」，本节记的是**第一个真正改变行为的步骤** —— 命中缓存时不再走引擎往返。因此本节对**每一处偏离设计与每一处失败关闭**都单独给出理由，而不只是描述代码。
>
> **状态**：代码 ✅ / 构建 ✅ / 部署 ✅ / **真机实测 ✅（采集 #12，2026-09-21 12:18–12:19，判据 1–6 全中 → 结论见 (10)）**。

#### （1）交付物与代码锚点

全部改动都在 `src/ProfilingHooks.cpp` 的 `s0` 命名空间内（+ `src/ProfilingHooks.h` 的说明块）。以下行号为 **2026-09-21 版**：

| 交付物 | 位置 | 作用 |
|--------|------|------|
| `kAnswerFromCache` | `:543` | 4a 的开关（= §8.2 规则 3） |
| `kCardFields[8]` | `:571` | 深拷贝的字段集（**封闭集**，见 (2)） |
| `CardField` / `Card` | `:594` / `:613` | 类型保真的字段存储 + 一张卡 |
| `g_cards` | `:648` | 缓存本体（`std::vector<Card>`，键 = index） |
| `captureField` / `applyField` | `:695` / `:750` | 深拷贝 / 重建，**逐字段保型** |
| `captureCard` / `serveCard` | `:803` / `:854` | 未命中后建缓存 / 命中时写回 `_itemInfo` |
| `compareAgainstCache` + `kValidateSampleSize` | `:957` / `:893` | §8.1「人工抽查 10 件」的**自动化**版本 |
| `reportAndResetRoundStats` | `:999` | 每轮计数日志（本步的**自证**手段） |
| `RequestItemInfoCacheHandler` | `:1059` | 替换体本身 |
| `makeProbe` 的 slot 0 分支 | `:1144` | 按开关选择「替换体」或「S0b 探针」 |
| `install()` 的计数调用 | `:1266` | 每轮一次 `reportAndResetRoundStats` |

（行号属于**该版本文档写就时**的文件；任何一次编辑都会让它漂移，查代码请**按符号名搜索**，不要按行号跳。）

**安装点没有新增**：仍然复用 S0a/S0b 的「`opening` 武装 → 本轮首个 `RequestItemCardInfo` 消费」两步（§10.6），仍然对 `_dataProcessors[0]` 的三个成员**原子装入、全成或全不成**。slot 1/2（`processList` / `processEntry`）**在两种模式下都是纯转发探针** —— 采集 #12 必须能与 #11b 对账（S4 的基线），而消费 S4 的是 4b-ii。

#### （2）与 §5.1 的**唯一**一处有意偏离：(A)/(C) 不再复刻

§5.1 的伪代码把 `_requestItemInfo` 的三行**全部保留**（① 存旧索引 / ①′ 设新索引 / ③ 还原）。实现**没有**保留 ①/③，这是本步唯一一处与设计文本相反的决定，理由必须站得住：

| | 依据 |
|---|------|
| **①/③ 的唯一读者是引擎** | ① 把 `a_index` 写进 `a_list._selectedIndex`，而 §4.3 已实测**委托参数为空**（`argCount = 0`，见 §11b 日志第 24 行），所以 `_selectedIndex` 是引擎获知「这一张卡属于哪一项」的**唯一**通道。命中时 ② **不存在**了 → ①/③ 也就没有读者 |
| **①/③ 的净效果恒为恒等** | ① 覆写、③ 还原同一个值，中间无提前返回（`ItemcardDataExtender.as:22-28` 只有顺序语句）→ 无论走不走，调用结束后 `_selectedIndex` 的值完全一致 |
| **连中间态的副作用都没有** | `_selectedIndex` 是普通私有字段（`BSList.as:21: private var _selectedIndex: Number`），不是 accessor → 「设一下再还原」不留任何痕迹 |
| **不保留反而消掉一个失败模式** | §5.1 的伪代码用 `list->GetMember("_selectedIndex", &oldIndex)` 读旧值；若该读失败，它的 ③ 会把一个 **undefined** 写回列表。不实现就没有这个风险 |
| **cache key 用的是引擎自己的键** | 缓存键 = `a_params.args[1]` = 循环变量 `i`，正是 ① 会写进 `_selectedIndex` 的那个数（§5.1 三方宿主表）。**这一点已被 #11b 日志直接证实**：`_requestItemInfo` 探针的首个调用 `args[1] = number 1.000000`（日志第 70 行）—— 即首轮第 0 项走原生函数、探针从第 1 项开始，与本步「每轮恰好 1 次全价往返」的预测逐项吻合 |

> 结论：命中路径**只做**「查表 → `CreateObject` → 逐字段 `SetMember` → `processor.SetMember("_itemInfo", card)`」，与 AS 体留下的状态（`updateItemInfo` 就是 `_itemInfo = a_updateObj`，`ItemcardDataExtender.as:34-37`）**完全等价**。而**未命中路径**仍然 `forwardVerbatim` → 原生 AS 体一字不改地跑（含 ①①′③）。

#### （3）缓存来源：§5.2 的「来源 B′」，因此 **H7 不必验证**

§5.2 犹豫的是「能否截获 `Respond` 载荷」（= 假设 H7 / 来源 A）。实现选了**来源 B′**（§5.2 末段那行退路），于是 H7 直接**变为无关**：

1. **未命中**：`forwardVerbatim` 让原生 AS 体把 `_itemInfo` 写进 processor，随后 `captureCard` 立刻 `GetMember("_itemInfo")` 深拷贝八字段；
2. **命中**：`serveCard` 把缓存重建为一个新对象再写回。

好处不只是「绕开 H7」：缓存里放的就是**引擎自己产出的对象**（不是我们复刻的），所以本步**不存在格式复刻风险**（§5.2 来源 B 被否掉的那个风险）。

#### （4）类型保真：为什么缓存的是 `GFxValue` 的**类型**而不是 C++ 值

八个字段由 `processEntry` 通过 **AS2 隐式转换**消费：`a_itemInfo.value > 0`、`stolen == true`、`effects != ""`（`InventoryDataSetter.as:27-86`）。转换结果**依类型而定**，所以：

- 把八字段统一存成 `double` 是**语义改动**（`stolen` 从 `true` 变 `1`、缺失成员从 `undefined` 变 `0`），本项目不接受「恰好比得一样」；
- 实现改为存 `CardField::Kind`（`kAbsent` / `kUndefined` / `kNull` / `kBoolean` / `kNumber` / `kString`）+ 对应值，重建时按类型选 setter → 是**再创建**而不是再解释；
- **缺失成员**存 `kAbsent` 且**不写回**：AS2 读不存在的成员得 `undefined`，与引擎对象的表现一致，这就是它自己的复现方式。

**为什么必须是深拷贝，不能存 `GFxValue`**：AS 对象引用的宿主是 movie 堆，而 movie 在菜单关闭时即被释放（§10.2 的 S-1 教训：`closed` 事件到达时 `uiMovie` 已经没了）。跨轮缓存如果存 `GFxValue`，**第二轮就是 use-after-free**。

**失败关闭策略**（比「尽力而为」更符合本项目标准）：

| 情形 | 处理 | 后果 |
|------|------|------|
| 字段类型是本缓存不支持的（`kStringW`、`kObject`…） | `captureField` 返回 false → **整张卡不认证** | 该物品永远走全价往返；日志按**字段名**告警一次（不是 6440 次） |
| `_itemInfo` 在往返后不是对象 | `captureCard` 早退 | 同上 |
| `serveCard` 中任一步失败 | 返回 false → 调用方**改为 `forwardVerbatim`** | 该次退化为全价；日志「一次进程一条」告警 |

> **`SetString` 陷阱（实现时才发现，值得记一笔）**：`GFxValue::SetString(const char*)` **只存裸指针**（`GFxValue.cpp:774`），而 `GFxMovie::CreateString`（`GFxMovie.h:52`，vfunc 0B）的语义是「Creates strings that are managed by ActionScript runtime」——即拷进 movie 自己的堆。写回 AS 对象成员必须用后者；用前者就是让缓存里的 `std::string` 去承担 movie 的生命周期。

#### （5）自证手段：每轮计数日志 + §8.1「人工抽查 10 件」的自动化

§8.1 对 S1 的判据是「**第 2/3 轮增量明显低于第 1 轮**；且 UI 数值与 skyui 原版逐项一致（人工抽查 10 件）」。两条都被做成**可直接读日志的测量**，不再依赖现场印象：

1. **每轮计数** `reportAndResetRoundStats`（在 `install()` 里调用，而 `install()` 每轮恰好一次）→ 打印**上一轮**的 `cache hits / full round trips / uncacheable cards / serve failures` 与缓存规模。于是「第 2 轮起命中」这件事**不必打开 Tracy 也能确认**；
2. **字段级对照** `compareAgainstCache`：**前 10 次命中**照旧跑一次引擎往返，然后把引擎刚产出的卡与缓存里的卡**逐字段**（8 个）对比，逐项差异按 `index` + 字段名落日志。
   - 代价被显式限定：**第 2 轮多 10 次引擎往返** → 第 2 轮的 `RequestItemCardInfo` 读数应为 **1 + 10 = 11**（而不是 1）；其余轮仍为 **1**。相对第 1 轮的 6441 仍低三个数量级，**不影响 §8.1 的时间判据**；
   - 这是对 §5.4 那条「**高**严重度」风险（缓存与真实库存不一致）唯一可能的**直接**检验：命中路径是本步**唯一**新增的、可能给出错值的地方（未命中路径就是原函数）。

#### （6）开关与回退（对照 §8.2 四条）

| §8.2 | 本步做法 |
|------|----------|
| 规则 1「保存原函数」 | ✅ handler 持 `GFxValue` **拷贝**（不是裸指针；`GFxValue` 是引用计数的，`GFxValue.h:288-300`），且从未 `Release()` —— 与 S0b 探针同一约定 |
| 规则 2「单一入口幂等」 | ✅ 仍是 `install()` + `armed/consumed` 两步，幂等性由 S0a/S0b 继承 |
| 规则 3「每个插入点一个开关，默认关闭，由 config 打开」 | ⚠️ **部分偏离**：开关是**编译期常量** `s0::kAnswerFromCache`。理由：本项目**尚无 config 读取器**（`dist/Template.ini` 与 `Template.toml` 都还是空文件），而每次采集本来就要重新构建 + 部署。置 `false` 即编译回**采集 #11b 的同一份 S0b 构建**，这正是「#11b 可复现」的保证 |
| 规则 4「不命中即不动」 | ✅ **逐项**成立：参数形状不符 / 未命中 / 重建失败 → 一律 `forwardVerbatim`，异常路径不产生任何对 movie 的写（`serveCard` 即使写了一半也无害：引擎会整体覆写 `_itemInfo`） |

> 也就是说：**运行时**回退是按**每个物品**生效的（不是整个开关），这比 §8.2 要求的最小可用性更强；弱的是**配置面**（编译期而非运行时），这一条如实记在这里，等 4a-2 / 4b 阶段引入真实 config 后再补齐。

#### （7）已知限制：本步**不做失效**，留给 4a-2

| 项 | 说明 |
|----|------|
| 键 | **index**（= 引擎自己的键），不是物品身份 |
| 成立前提 | 清单在同一会话内**静态** —— 与 §10.7 采集 #11b 完全同样的 setup（纯开关背包、不拾取/丢弃/使用/买卖） |
| 失效症状 | 一旦清单变化，变化处**及其后**的 index 可能对不上 → 卡片数值显示的是**上一轮的**数字（`infoValue` / `infoWeight` / `infoArmor` / `infoDamage` / `isStolen` / `isEnchanted` / `isPoisoned` / `type`）。**不影响物品本身**，也不影响任何写操作 |
| 为什么本步可以不做 | 采集 #12 的前提是「与 #11b 同 setup 的纯 opening」→ 静态清单下风险**不成立**；把失效单独拆出去，才能让 #12 的结论**只**关于「消往返」这一件事 |
| 4a-2 要做的事 | ① 靶点 #4 `InventoryChanges::SendContainerChangedEvent` 失效（其 **AE 重定位上游标错**，必须先反汇编才能 hook，见 `ProfilingHooks.h` 末尾注释块）；② 给 `g_cards` 加一个 `clear()` 或按「物品身份」重建键；③ 重跑 #12 与 #12′（拾取/丢弃场景）对照 |

#### （8）构建与部署（供采集 #12 引用）

| 项 | 值 |
|----|-----|
| 构建 | `cmake --build --preset relwithdebinfo`（`ENABLE_TRACY=ON`），MSVC v143 / `/W4 /WX`：**0 warning** |
| 产物 | `build\bin\RelWithDebInfo\Template.dll` = **780,288 B**，时间戳 2026-09-21 11:12:03；本次构建的 SHA-256 = `44BEDCFFDA6C1F54BF4D67CC78360F2EFFA995A035AEA01BE4786A2DE1F3F1D5` |
| 部署 | `C:\Mod Organizer 2\mods\ModTest\SKSE\Plugins\`（`Template.dll` + `Template.pdb` 同时覆盖；部署后两处 SHA-256 已核对一致） |
| 日志 | `D:\Documents\My Games\Skyrim Special Edition\SKSE\Template.log` |
| 指纹 | 字节数 **780,288 B ≠ #11b 的 767,488 B**（+12,800 B）→ 尺寸即可区分两个构建；要更严格就用上面的 SHA-256（**任何后续重编都会改变它**，采集时以 `Get-FileHash` 实测为准），符合 [tracy-capture-log.md](./tracy-capture-log.md) 的采集约定 |

> ⚠️ 上面这一行是**文档写就时**的构建。此后只要再动过 `src/` 并重编，**SHA-256 必变、尺寸通常不变** —— 采集前请用 `Get-FileHash ... -Algorithm SHA256` 与部署目录里那份核对，别照抄文档里的哈希。

#### （9）采集 #12 的预期读数（**先登记预测，再实测**）

**count 预测**（每轮 6441 项；「index 0」= 每轮第 0 项，因安装发生在该项的 `RequestItemCardInfo` **之内**，故它**恒**走原生函数 —— 与 #11b 判据 6 是同一机制的镜像）：

| 轮 | `RequestItemCardInfo`（2.1） | `AS::_requestItemInfo` | `AS::processEntry`（S4） | 该轮日志（在**下一轮** install 时打印） |
|----|------------------------------|------------------------|--------------------------|------------------------------------------|
| 1 | 6441 | **6440**（建缓存） | 6441 | 命中 0 / 全价 6440 / 缓存 6441 槽 6440 认证 |
| 2 | **11**（= index 0 + 10 次校验对照） | 6440（**10 次校验 + 6430 次命中**） | 6441 | 命中 6430 / 全价 0 / 校验对照 10 / **0 不符** |
| 3 | **1** | 6440（全命中） | 6441 | 命中 6440 / 全价 0 |
| 4 | **1** | 6440（全命中） | 6441 | 命中 6440 / 全价 0 |
| 5 | **1** | 6440（全命中） | 6441 | （第 5 轮的计数无下一轮可打印 —— 停机） |

**判据（验收 = 下列 1–6 全中）**：

1. 日志出现 **5** 次 `4a: \`_dataProcessors[0]\` wrapped: … = CACHE …`（轮数）；
2. 日志出现 `4a: cache holds 6441 card slot(s), 6440 of them certified` **4 次**（第 2–5 轮各一次）—— 第 1 轮 install 时缓存还是空的，所以它打的是 `cache holds 0 card slot(s), 0 of them certified`，**这两组数正是「第 1 轮建缓存、第 2 轮起命中」的自证**；`6441 − 6440 = item 0`（index 0 恒不入缓存），是机制而不是缺陷；
3. `4a/validate: … all 8 card fields identical …` **10 条**，且**没有** `4a/validate: … differs …` 与 `4a: rebuilding card … failed`；
4. `AS::processEntry` count 仍 **32205**，逐轮 6441（S4 基线未被本步破坏 → #12 可与 #11b 对账）；
5. `RequestItemCardInfo` 与 `FxDelegate::Callback[RequestItemCardInfo]` 的 count **逐轮相等**（引擎侧独立计数同意 2.1 的计数），且逐轮读数符合上表；
6. 时间：单次 `AS::_requestItemInfo` 从 **77.82 µs** 掉到「命中路径」量级，`RequestItemCardInfo` 的**每轮总量**从 ≈ **443.4 ms** 掉到 ≈ **1 ms 级**。

**时间预测（可被证伪，故先写下来）**：命中路径**不是零成本** —— 每次命中要 `CreateObject` ×1、`CreateString` ×1~2（`effects` 恒为字符串）、`SetMember` ×9。按 GFx 成员操作 0.5–1 µs 量级估：

```text
每轮节省 = 443.4 ms（引擎 C++ 体）+ 57.9 ms（AS 体 + apply + 边界） - 6440 × 命中成本
         ≈ 501.3 ms - 6440 × (5 ~ 15 µs)  =  405 ~ 469 ms/轮  （50 ~ 58 %）
```

即：**预期落在 40x ms/轮 而不是 §5.3 乐观值 501 ms** —— 差额全部来自命中路径自身的 GFx 调用。若实测显著低于 405 ms，说明命中路径比估计更贵（应去看单次 `AS::_requestItemInfo`），这才是需要解释的信号。

#### （10）采集 #12 实测结果（2026-09-21 12:18–12:19，5 轮开 / 关背包）

完整数据、逐条裁定与不确定性讨论见 [tracy-capture-log.md](./tracy-capture-log.md) §12；本节只留结论，供设计文档内部自洽引用。

**DLL**：`build\bin\RelWithDebInfo\Template.dll` = **780,288 B**，SHA-256 `44BEDCFF…F1D5` —— 与 (8) 登记的 4a 指纹**逐字节一致**。

| 判据 | (9) 的预期 | 实测 | |
|------|-----------|------|---|
| 轮数 | 5 | **5** | ✅ |
| 缓存规模 | `6441 槽 / 6440 认证` | **4 次该行** + 第 1 轮 `0 / 0` | ✅ |
| 数值一致 | 10 identical / 0 differs | **10 / 0**（`uncacheable cards`、`serve failures` **恒为 0**） | ✅ |
| S4 基线 | `AS::processEntry` = 32205，逐轮 6441 | **32205**，逐轮 **6441** | ✅ |
| 引擎侧计数 | `RequestItemCardInfo` == `Callback[…]`，逐轮 6441 / 11 / 1 / 1 / 1 | **逐轮相等，逐轮符合** | ✅ |
| 时间 | 单次 `_requestItemInfo` 5–15 µs；节省 405–469 ms（50–58 %） | 命中单次 **2.80 µs**；窗口 **844.2 → 312.6 ms/轮 = −531.6 ms（−63.0 %）** | ✅ **机制命中、量级优于预测**（偏差已解释） |

**关键读数**（轮 2–5 均值 vs #11b 轮 2–5 均值）：UIList 全量窗口（`AS::processEntry` 首→末 span）**844.2 → 312.6 ms（−63.0 %）**；`AS::_requestItemInfo` **498.04 → 18.41 ms/轮（−96.3 %）**；`RequestItemCardInfo` **443.35 → 0.028 ms/轮**；主线程 `GetValue` **11670 → ≈ 4 次/轮**。**第 1 轮（建缓存）不慢**：832.7 ms vs 849.7 ms，`AS::_requestItemInfo` 493.99 ms vs 501.18 ms → **八字段深拷贝 + 认证的开销在噪声以下**；5 轮窗口合计 **−51.0 %**（首轮全价只付一次，这是设计使然）。

**结构自证**：`RequestItemCardInfo` ⊃ `AS::_requestItemInfo` 的嵌套与 #11b **镜像** —— 双指针逐事件核对 **6450 内 / 恰好 5 外**，且 5 个在外的时刻**恰好等于 5 轮的起簇时刻** → 就是每轮第 0 项（走原生函数）；#11b 是 32200 内 / 5 外。

**预测偏低的两条量化原因**（(9) 的区间**未回改**）：① 命中成本高估 —— 预测 5 µs 起、实测 2.80 µs ⇒ 少算 ≈ **14 ms/轮**；② **未预测到** `AS::processEntry` 附带 **−44.9 ms/轮（−14.9 %）**，归因未完成（待查：卡片字符串表示的托管方式 / 对象形状 / 堆压力），**不改变任何判据结论，不阻塞 4b-ii**。

**跨会话不确定性**：本轮 trace 内的「同工作对照」中，C++ 侧三处一致给出「#12 的机器比 #11b 快 3–6 %」，但纯 AS 循环对照（`AS::processList` 空转）给出 26 % —— 后者解释不了。按 0.95 折算，节省 ≈ **−502 ms（−59 %）**；按 0.739 折算，≈ **−421 ms（−50 %）**，**落回 (9) 的区间内**。因此「节省的**精确数值**」这一条预测**不予判为命中**，而**机制级预测（命中 ≪ 往返、命中单次 ≈ 2.8 µs）被证实且有余量**。

**裁定**：判据 1–6 **全部通过**；判据 3 **无 `differs`** → **不回退**（`kAnswerFromCache` 保持 `true`），**下一步 4b-ii**（消 **S4 ≈ 303.3 ms/轮 ≈ 37.7 %**，见 §6.4 / §8.1）。

#### （11）采集 #13 的预登记：三条一次性诊断（离群值归因 / 八字段 `Kind` 分布 / `effects` 长度）

**为什么要有这一步 —— 零采集阶段已经排除掉另一个读法。** §10.7 的 14.59 ms `AS::processEntry` 离群值最初被读成「某个 `formType` 分支在某些物品上很贵」。把 #11b 与 #12 两份 trace 的 `processEntry` 事件（各 32205 条）重新按轮分簇之后，四条事实否掉了那个读法：

| 观察 | 证据 |
|------|------|
| 每次开背包**恰好 2 条** > 5 ms 事件 | #11b / #12 各 5 轮，逐轮 2 条 |
| 位置**近似固定但逐轮漂移** | 前一条在 #35–50、后一条在 #6232–6242（共 6441），同一会话内漂移 ≈ 15 项 —— 固定物品身份不可能漂移 |
| 幅度**逐轮递减** | #11b 首轮 14.6 ms → 末轮 9.6 ms |
| **缓存命中轮仍然存在** | #12 轮 2–5：5.1–9.5 ms；而这些轮**一次卡片分配都没有**（命中路径只 `CreateObject` + `SetMember`） |

一个数据相关的分支解释不了「同一物品、位置漂移、幅度递减、与卡片路径无关」。能同时解释四条的是 **AS2 堆的确定性分配阈值触发 mark-sweep** —— 那是**暂停**，不是代码开销。

**因此这一步要回答的是定价问题，不是定性问题**：4b-ii 的上界是 **S4 ≈ 303.3 ms/轮**，其中有多少是 C++ 化**消不掉**的暂停？把它测出来，比事后解释一个「没做干净」的残差便宜。**已在 `src/` 落地（一次改动、三条诊断、一次采集合并）**：

| # | 诊断 | 位置 | 触发 / 频率 | 读数 |
|---|------|------|-------------|------|
| ① | `processEntry` 离群值 | `ProcessEntryHandler::Call` | 单次 ≥ **5 ms**（≤ 20 行/进程） | `4b/outlier: processEntry #<轮内序号> of this round took <ms> ms -- formType <n>, formId <n>, baseId <n>` |
| ② | 八字段 `Kind` 直方图 | `captureCard` + `reportAndResetRoundStats` | 每认证 1 张卡记 8 个数；**每进程打一次**（第 2 轮 install 时） | `4a/4b: card field \`Kind\` histogram over 6441 certified card(s)` + 8 行分字段桶计数 |
| ③ | `effects` 字符串长度（H12a 量级检验） | 同 ② | 同 ② | `4a/4b: \`effects\` string lengths: empty … / non-empty … / total … / max …` |

**开关与回退**（§8.2 规则 3 的同一模式，与 `kAnswerFromCache` 并列）：三条诊断全部由 **`kExtraDiagnostics`** 这一个**编译期**常量控制，并且用 `if constexpr` 而非运行期分支 —— 理由是要让 `false` 把两次 `steady_clock::now()` 与直方图自增**从每项路径上完全删除**，而不是把它们变成一个分支。**回退契约已实测**：

| `kExtraDiagnostics` | 产物 | 三条新字符串是否在 DLL 中 |
|----------------------|------|----------------------------|
| `true`（**当前值**，为 #13 部署） | `Template.dll` = **786,432 B**，SHA-256 `AED34D6D…4B60` | **全部存在** |
| `false` | `Template.dll` = **780,288 B** | **全部不存在** ✅ |

> ⚠️ 上表的 SHA-256 只对**写就时的这一份产物**有效：本步实测过一次 —— 同样 `kExtraDiagnostics = true`、同样 **786,432 B**，`--clean-first` 重建一次哈希就从 `B989219E…3D57` 变成 `AED34D6D…4B60`。**尺寸是稳定的指纹，哈希不是**；采集时必须 `Get-FileHash` 实测，与 (8) 那条警告同一口径。

`false` 的 **780,288 B 与 (8) 的 4a 构建逐字节同尺寸** → 「回退到 #12 的构建」只是翻一个常量，不需要删除任何代码，也不需要第二份源码。

**代价（先承认，再判读）**：① 每个 `processEntry` 多两次 `steady_clock::now()` + 一次比较 ≈ 40–60 ns/项 × 6441 ≈ **0.3 ms/轮**，对 ≈ 256 ms/轮的 `processEntry` 总量是 **+0.1 %**（比 (10) 记录的 3–6 % 跨会话离散小一个数量级）；成员读取只在阈值命中时发生（≈ 2 次/轮）。②③ 的自增只在**建缓存那一轮**（第 1 轮）执行。

**采集 #13 的预登记判据**（先登记、后实测，与 (9) 同一约定：本表**不回改**）：

| # | 判据 | 通过条件 | 不通过说明什么 |
|---|------|----------|----------------|
| 1 | DLL 指纹 | **786,432 B**（≠ #12 的 780,288 B，+6,144 B）；SHA-256 以采集时 `Get-FileHash` 实测为准 | 尺寸不符 = 部署的不是本构建，**整轮作废** |
| 2 | 结构自证未破 | `AS::processEntry` count 仍 **32205**（逐轮 6441）；`RequestItemCardInfo` 逐轮 6441 / 11 / 1 / 1 / 1；命中轮 `AS::_requestItemInfo` 单次仍 ≈ 2.8 µs；`uncacheable cards` / `serve failures` 恒 0；无 `differs` | 任一不符 = 三条诊断**改动了行为**（而非只加日志）→ 先查这个，再判读 3/4/5 |
| 3 | ② 自洽 | ② 出现 **1 次**标题行 + **8 行**分字段行；**每行六个桶之和 = 6441** | 和不等于 6441 = 计数漏了（半张卡未被计入 / 重捕获）→ ② 作废 |
| 4 | 离群值归因 | 每轮 **2 条（±1）** `4b/outlier:` 行，5 轮共 ≈ **10 条**；按下面的裁决表分组 | **0 条** = 阈值 5 ms 对本机偏高，诊断**未定**，需调阈值重采（不是「离群值消失」） |
| 5 | 成本在噪声内 | 轮 2–5 的 `AS::processEntry` 每轮总量与**同一 trace 内**第 1 轮（全价、走引擎卡片）对照，差值 ≤ 1 %；**禁止**用跨会话绝对值判 | > 3 % = `steady_clock`×2 的开销超预估 → 判据 4 的读数按 +0.1 % 的量级修正，但归因结论仍成立 |
| 6 | ③ 的分母一致 | `empty + non-empty = 6441` | 不等 = ② 与 ③ 分母不一致，③ 作废 |

**判据 4 的裁决表（本轮的核心：两种结果导向两个不同的 4b-ii 计划）**：

| 观察 | 裁定 | 对 4b-ii 的后果 |
|------|------|------------------|
| 10 条 `formType` **互不相同 / 与序号无稳定对应** | **H-GC 成立**：离群值 = AS2 堆暂停 | 303.3 ms/轮里**含 ≈ 10–30 ms/轮 的暂停**，C++ 化**消不掉**（它来自 AS 侧分配与堆，不来自 `processEntry` 的代码）→ **必须写进 4b-ii 的验收判据**，否则会被误判成「没做干净」 |
| 10 条 `formType` **恒定** | **H-branch 成立**：确是某个昂贵分支 | 4b-ii **直接消掉它**，原型应从该分支先做；H-GC 作废 |
| `formType` 恒定**且 `baseId` 恒定** | 定位到**某个具体基础物品**（`baseId` = base form id，由 `processEntry` 自己写在 `:26`） | 可离线核对该物品为何贵 → 直接作为 4b-ii 的第一个性能测试用例 |

**判据 3 的 `Kind` 列（②）对 C++ 侧 `processEntry` 的硬约束**：

| ② 的读数 | 约束 |
|----------|------|
| `effects` 的 `string` 桶 = 6441（预期） | 字符串比较语义（`!= ""`）必须保留，不能降级成 bool |
| 任何字段 `absent ≥ 1` | **真机确实存在缺失成员** → C++ 侧读缺失成员必须给出 `undefined` 语义（不能补 `0` / `""`）；`applyField` 的 `kAbsent` 分支从「设计推理」升级为「实测覆盖」 |
| 某字段 `undefined` / `null` ≥ 1 | 同上，「成员不存在」与「成员值就是 `undefined`」必须在 C++ 侧被区分开 |

**③ 的证据权重（写清楚，避免被过度引用）**：③ **不是** H12a 的闭环，只是它的**低价证伪器** —— 它回答的是「`effects` 在**捕获侧**是不是几乎全空」：若几乎全空，则「每轮 6440 次托管字符串分配」这个量级**站不住**（没有字节可分配）；若普遍非空且平均长度可观，H12a **仍未被证实**（GFx 是否真的每次分配，只有 (10) 里那条 `SetString` 替换实验能答）。**H12a / H12b / H12c 的判别实验仍然开放，且不阻塞 4b-ii。**

> 📌 **本节的实测结果见 (12)**：判据 1 / 2 / 3 / 4 / 6 **通过**、**判据 4 裁定 H-GC 成立**；但其中 **裁决表第 2 行的口径被实测证伪**（`formType` 太粗，须加 `baseId` 漂移列）、**四条事实里「幅度逐轮递减」未复现**、**判据 3 / 6 的分母应为 6440（认证卡数，非 6441）**、**判据 5 的预登记口径不可判（第 1 轮是全价轮，与命中轮不同工作负载）**。**本节的两张表一律不回改**，全部修正与读数登记在 (12)。

#### （12）采集 #13 实测结果与判据 4 裁定（2026-09-21 13:24–13:25，5 轮开 / 关背包）

**采集记录见 [tracy-capture-log.md](./tracy-capture-log.md) §13.5–§13.7。**（11）的判据表与裁决表**均未回改**；本节登记实测读数、**三处口径偏差**（其中一处是裁决表的口径被证伪），以及它给 4b-ii 划出的新上界。

**1）判据 1–6 逐条裁定**

| # | 判据 | 实测 | 裁定 |
|---|------|------|------|
| 1 | DLL 指纹 | `Template.dll` = **786,432 B** / 时间戳 2026-09-21 13:11:02 / SHA-256 `AED34D6D…4B60`；与部署目录那份同值 | ✅ |
| 2 | 结构自证未破 | `AS::processEntry` **32205**（逐轮 6441 × 5）；`AS::_requestItemInfo` **32200**（= 32205 − 5）；`AS::processList` **4**；`RequestItemCardInfo` 逐轮 **6441 / 11 / 1 / 1 / 1**（合 6455）；命中轮单次 **2.831 / 2.814 / 2.781 µs**；`uncacheable cards` / `serve failures` 恒 0；无 `differs`（10 identical） | ✅ **逐字命中** |
| 3 | ② 自洽 | ② 出现 1 次标题行（`over 6440 certified card(s)`）+ 8 行分字段行；每行六桶之和 = **6440** | ✅（分母口径见第 2 点） |
| 4 | 离群值归因 | 5 轮共 **9 条**（R1/R2/R4/R5 各 2 条、R3 **1 条**）；「每轮 2 条（±1）」满足 | ✅ **H-GC 成立**，见第 3 点 |
| 5 | 成本在噪声内 | 见第 4 点（**预登记口径不可判**，改用轮内自对照） | ⚠️ **口径缺陷**，读数在噪声内 |
| 6 | ③ 的分母一致 | `empty 1591 + non-empty 4849 = 6440` | ✅（同上，分母 6440） |

**2）三处口径偏差（预登记表未回改，在此登记）**

| 位置 | 预登记写的 | 实测 | 说明 |
|------|-----------|------|------|
| 判据 3 | 每行六桶之和 = **6441** | **6440** | ② 的分母是**认证卡数**（6440），不是条目数（6441）。差 1 = 每轮第 0 项（它跨过 `processEntry` 时 `_requestItemInfo` 尚未被替换 ⇒ 不入缓存），与 (10) 的 `cache holds 6441 / 6440 certified` 是**同一个环测项**；预登记处把下标 0 算进去了 |
| 判据 6 | `empty + non-empty = 6441` | **6440** | 同上 |
| 判据 5 | 轮 2–5 每轮总量 vs **同一 trace 内第 1 轮** ≤ 1 % | 轮 2–5 比轮 1 低 **6.9–11.5 %** | **口径本身不成立**：第 1 轮是全价轮（引擎卡片），轮 2–5 是命中轮，**工作负载不同** —— 这个 −12 % 正是 (10) 已记录的 H12a / H12c 效应（#12 同形：294.51 → 264.97 / 252.57 / 253.42 / 253.88 ms）。可达成的参照只有**命中轮之间的内部离散**（跨会话同轮被 §12.4 第 3 点的 3–6 % 离散排除） |

**3）判据 4 的裁定：H-GC 成立 —— 但裁决表第 2 行的口径被本步证伪**

逐轮离群值（Tracy 逐事件分簇；与 C++ 探针的日志读数**逐条同位置、同幅度**，见 §13.5 第 4 点）：

| 轮 | 早离群值 | 晚离群值 |
|----|----------|----------|
| 1 | #50，`formType 27`(BOOK) / `baseId 110588`，5.90 ms | #6242，`formType 26`(ARMO) / 883293，8.99 ms |
| 2 | #50，27 / 110588，7.92 ms | #6242，26 / 883293，8.53 ms |
| 3 | **无**（< 5 ms） | **#6241**，26 / **883281**，8.89 ms |
| 4 | #50，27 / 110588，5.54 ms | #6242，26 / 883293，10.27 ms |
| 5 | #50，27 / 110588，6.45 ms | #6242，26 / 883293，8.81 ms |

（`formType` 按 `RE::FormType` 读，`RE/F/FormTypes.h:138` 为 0 基枚举：**26 = Armor、27 = Book**。）

**裁定 H-GC**，三条证据各自都只与「暂停」相容：

1. **`baseId` 随位置漂移**：轮 3 的晚离群值打在 **#6241 / `baseId` 883281**，另外 4 轮是 **#6242 / 883293**。清单是静态的（判据 2 的 10/10 全等 + 命中稳定共同证明），所以下标 6242 每轮就是 883293 —— **轮 3 的暂停落在了另一个物品上**。数据相关分支做不到「换个物品、同一个暂停」。
2. **早离群值在轮 3 整条消失**：同一本书（110588）、同一位置（#50），轮 3 跌破 5 ms。固定分支不会「有时 5.8 ms、有时 < 5 ms」。
3. **幅度乱跳**：5.54 → 10.27 ms（#12 同形：5.12 → 9.55 ms）。

⚠️ **（11）的裁决表第 2 行「10 条 `formType` 恒定 → H-branch 成立」口径过粗，本步实测把它证伪了 —— 但（11）的结论方向（H-GC）不变。** 本轮 `formType` 恰好**恒定**（27 / 26），若只按那一行判就会得到 H-branch。原因是 `formType` 太粗（26 = ARMO 覆盖数千件）而**漂移只有 ±1 项**（相邻项同类型）⇒「`formType` 恒定」与「物品已被换掉」可以同时成立。**真正破平局的是 `baseId`**（超出预登记计划才加的字段）：`883293 → 883281` 的漂移才是不依赖具体物品的直接证据。**正确判别列 =「`formType` + `baseId` 是否随位置漂移 + 幅度是否波动 / 是否偶发低于阈值」**，不再单看 `formType`。

**（11）四条事实里第 3 条「幅度逐轮递减」同样被本步修掉**：#13 为 8.99 → 8.53 → 8.89 → **10.27** → 8.81，#12 为 8.49 → 9.55 → 7.38 → 8.42 → 9.10，**都不递减** ⇒ 那是 #11b 首轮建卡堆压力最大的**单次现象**，**不能算 H-GC 的证据**；第 1 条也需放宽为「每轮 **1–2** 条」（#13 的 R3、#12 的 R4 各只有 1 条 → 判据 4 的「±1」容差是必要的）。**这不削弱 H-GC**：位置漂移、命中轮仍在（复现证据）与 `baseId` 漂移、偶发消失（直接证据）都指向同一结论。

**4）判据 5：预登记口径不可判，改用两条可达成的对照**

| 对照 | 读数 | 判定 |
|------|------|------|
| 轮 3 / 4 / 5 内部离散（同工作负载、纯命中、无校验项） | **256.80 / 256.45 / 255.75 ms**，极差 **0.41 %** | ✅ ≤ 1 % |
| 轮 2 vs 轮 3 | 269.15 vs 256.80 = **+4.81 %** | ⚠️ **结构性，非探针成本**：轮 2 含 10 次校验全价往返 + 建缓存后的堆状态；**#12 同形 +4.91 %**（264.97 vs 252.57） |
| #13 vs #12 逐轮（除探针外同构建） | R1 **−1.83 %** / R2 +1.58 % / R3 +1.67 % / R4 +1.20 % / R5 +0.74 %；合计 **+0.60 %** | ⚠️ **符号在轮间翻转** ⇒ 差异由**跨会话离散（3–6 %，§12.4 第 3 点）+ GC 暂停落点**主导，**分辨不了 0.1 % 量级** |

**探针成本理论值** = 每项 2× `steady_clock::now()` + 1 次比较 ≈ 55 ns → 6441 × 55 ns ≈ **0.35 ms/轮 ≈ 0.14 %**（与 (11) 预估的 +0.1 % 一致）。**裁定：通过（按可达成口径）**；预登记口径登记为**缺陷**，不是失败。归因结论不受影响 —— GC 暂停 ≈ 14 ms/轮 与 0.14 % 的探针成本差两个数量级。

> **探针读数与 Tracy 读数之间 +0.04–0.07 ms 的系统偏移**（9 条离群值逐条都有）：探针只包住 `forwardVerbatim`（`src/ProfilingHooks.cpp:625-627`），而 Tracy 的 `AS::processEntry` zone（`:610`）**还包含阈值命中后的 `logProcessEntryOutlier(...)` 文件写入** → Tracy 读数 =「AS 体 + 日志 I/O」，探针读数 =「只 AS 体」。9 × ≈ 55 µs ≈ **0.1 ms/轮 ≈ 0.04 %**，对判据 2 / 5 无影响；**引用离群值幅度时以探针读数为准**。

**5）判据 3 的 `Kind` 直方图：`kAbsent` 从设计推理升级为实测覆盖**

| 字段 | absent | undefined | null | bool | number | string |
|------|--------|-----------|------|------|--------|--------|
| type / value / weight | 0 | 0 | 0 | 0 | **6440** | 0 |
| stolen | 0 | 0 | 0 | **6440** | 0 | 0 |
| effects | 0 | 0 | 0 | 0 | 0 | **6440** |
| armor | **4015** | 0 | 0 | 0 | 2425 | 0 |
| damage | **4055** | 0 | 0 | 0 | 2385 | 0 |
| poisoned | **4069** | 0 | 0 | **2371** | 0 | 0 |

- 「任何字段 `undefined` / `null` ≥ 1」这一行**未出现**（全 0）⇒ (11) 的两条约束里**第 2 条本轮不适用**（但**不可据此删除**：它只是没被触发）；
- 第 1 条**被大量覆盖**：三项合计 **12,139 次缺失成员**，且同轮 `validation 10/10 全等、0 differs` ⇒「**不写回 = 复现缺失成员**」这条 `applyField` 的 `kAbsent` 分支**实测正确**。**C++ 侧 `processEntry` 必须保留「成员不存在 ≠ 值是 `0` / `""`」的区分**；
- `effects` **恒为 `string`** ⇒ (11) 里「`!= ""` 的字符串比较语义不可降级」由实测确认。

**6）③ 的权重与 4b-ii 上界修正**

③ 读数 `empty 1591 / non-empty 4849 / total 584,112 chars / max 445`：**75 % 非空、非空项平均 ≈ 120 字符**。按 (11) 对 ③ 的措辞，这落在「**普遍非空且平均长度可观**」分支 ⇒ **③ 既未证伪也未证实 H12a**，H12a / H12b / H12c 的判别实验**仍然开放**（闭环仍是那条会改变行为的 `SetString` 替换实验，须单独一轮）。

**4b-ii 上界修正**（这是本步对 §6.4 / §8.1 那条「消 37.7 %」的唯一实质改动）：

- **GC 暂停实测 ≈ 14 ms/轮**：#13 逐轮 ≥ 5 ms 事件合计 **14.89 / 16.45 / 8.89 / 15.81 / 15.26 ms → 71.30 ms / 5 轮 = 14.26 ms/轮**；#12 = 68.81 ms / 5 = **13.76 ms/轮**；#11b 全价语料 = 106.99 ms / 5 = **21.40 ms/轮**（堆压力越大暂停越大，与 mark-sweep 一致）。落在 (11) 预登记的「**10–30 ms/轮**」区间**下沿**。
- **`S4 ≈ 303.3 ms/轮（37.7 %）` 不是全部可消**：扣除后**可消上界 ≈ 289 ms/轮 ≈ 36 %**。
- ⚠️ **不能反向承诺这 14 ms 一定残留**：4b-ii 把 `processEntry` 搬进 C++ 会**改变 AS2 分配画像**（`Translator.translate` / 字符串拼接从 AS 侧移走），暂停**可能缩小、可能不缩、也可能移位** ⇒ **必须在 B1 原型上实测**；
- **必须写进 4b-ii 的验收判据**（(11) 已预登记这一条）：`processEntry` 的 C++ 版即使做对，轮内**仍可能出现毫秒级尖峰**，它不是「没做干净」。

### 10.9 S4（=4b-ii）落地：`processEntry` 的 C++ 复刻（2026-09-21，**代码就绪 ✅ / 构建与静态交叉验证 ✅ / 真机实测 ✅ —— 判据全过，但耗时未达承诺，见下文「真机验收」**）

> ⚠️ **本节记录的是 2026-09-21 基于 skyui-5.1 源码树的落地；2026-09-22 已确认运行版实为 SkyUI-Community v6.11 并整体重转写。故本节的「102 字面量 / 51 组 material / 125 个 `case`」等数字均为 5.1 基线值，现行数字见 §10.10。**

**范围**：`InventoryDataSetter.as:24-110` 的 `processEntry` 与 `:115-878` 的 18 个 private helper，共 879 行 / 125 个 `case` / 132 次 `Translator.translate`（102 个不同字面量）——**（5.1 基线；v6.11 为 1304 行 / 20 个 helper / 320 个 `case` / 103 个不同字面量，见 §10.10）**。**不做** `processList`（S3，§10.7 已判不可测）、**不做** `fixSKSEExtendedObject`（entry 到手时已 fix）、**不做**缓存失效（4a-2）。

**交付物**

| 文件 | 内容 |
|------|------|
| `src/ProcessEntryReplica.h`（新增，331 行） | 102 个翻译字面量的枚举 + 表（**用脚本从 `.as` 抽取**，两个 `static_assert` 钉死顺序与条数）、`Write` / `WriteSet`、`Stage` 三态、`prepare` / `begin` / `finishValidation` / `reportAndResetRoundStats` |
| `src/ProcessEntryReplica.cpp`（新增，2094 行） | 逐行转写：`prologue`（:26-35）+ 17 个分支 helper（`armorClass` / `materialKeywords` / `armorPartMask` / `armorOther` / `armorBaseId` / `weaponSubType` / `weaponBaseId` / `ammoType` / `ammoBaseId` / `bookType` / `keyType` / `potionType` / `soulGemType` / `soulGemStatus` / `soulGemBaseId` / `miscType` / `miscBaseId`）+ `body` 的 `formType` switch + AS2 语义层 + 写入层 + 影子对照层 + 翻译 spike |
| `src/ProfilingHooks.cpp` | 新增总开关 **`SSE_REPLICATE_PROCESS_ENTRY`**（宏，定义在 `ProcessEntryReplica.h`）；`ProcessEntryHandler` 由纯转发改为「复刻优先、fail-closed 转发」；`install()` 里预热翻译表 + 每轮回填统计；`makeProbe` 注释与日志同步 |
| （插入点） | 不新增第四个插入点：仍挂在 S0a/S0b 的 `install()` 上，槽位表与 all-or-nothing 语义**一字未改** |

**决策 A–E 的落地形态**

- **A 翻译**：`Translator.translate` **不是查表**（`Translator.as:11-27` = `_root.createTextField` + `_translator.text = a_str` + 读回，即向**游戏本地化系统**问字符串），C++ 无等价物 ⇒ 唯一忠实做法是**调 AS**。四条候选路径依次试：① `_global.skyui.util.Translator` + `Invoke("translate")`；② `GFxMovie::Invoke("_global.skyui.util.Translator.translate")`；③ 已存在的 `_root._translator` TextField；④ 按 `Translator.as:14` 的写法**创建**该 TextField（同名，故日后 skyui 自己调用时会替换它而不是叠加）。**命中哪条路会打一行日志**——这是 spike 的结果；四者皆败 ⇒ `g_translationsReady` 保持 false ⇒ 复刻**整体拒绝**，退回纯转发（与 4b-ii 起点同态，最坏情况是「没变化」，不是「显示字符串错」）。
  102 次解析放在 `install()`（每轮一次、在 `AS::processEntry` zone **之外**），否则会变成第 1 件物品的 ~30 ms 假尖峰；热路径只剩一次 flag 读。失败重试**有界**（`kMaxPrepareAttempts = 4`，同 `kMaxInstallAttempts` 之理），避免 spike 失败后退化成每件 ~100 次废 GFx 调用。
- **B 输入**：直接读 `args[0]`（entry）/ `args[1]`（_itemInfo）两个 GFxValue，**完全不碰 `g_cards`**（`processEntry` 与 4a 的卡缓存无耦合）。
- **C 分流**：无条件 C++ 化 + **fail-closed 转发**。关键实现约束：**先读全、再决定、最后才写**——`gather` 只读不写，任一成员类型不可复刻（`stringW`/对象等）就整件退回；这是唯一能让「退回」对 AS 原版**不可见**的写法。
- **D 验证**：每轮前 10 件走**影子对照**——先让 AS 原版作答，再按成员逐个比对（`Kind` + 值，字符串 `strcmp`、数字无 epsilon，与 4a `fieldMatches` 同标准）。这是全步骤**唯一**「错得有道理也不会崩」的地方，因此必须有实测对照而不是目视。
- **E 复刻**：逐行直译保真优先；AS 侧常量（`Form.TYPE_*` / `Form.BASEID_*` / `Armor.PARTMASK_*` / `Material.*` …）以**字面量 + 行尾 AS 常量名注释**书写而不是 250 行命名常量——审阅者需要在同一行看到 `0x0139C0` 和 `BASEID_DAEDRICARROW`。

**三个 AS2 语义陷阱（本步最容易被「C++ 里差不多」写错的地方，均按 ES3 规则裁定而非挑选看起来合理的读法）**

1. **`!=` 不是 `!(== 0)`**：`undefined != ""` 与 `null != ""` 都是 **true**（ES3 11.9.3：null/undefined 彼此相等、与别的都不等，String/Number 的强制转换步骤根本不执行）⇒ **`effects` 成员缺失的 entry，在 AS 原版里读出 `isEnchanted = true`**。复刻照抄这一条，而不是「顺手修正」。已由 §10.8(6) 实测确认 `effects` 恒为 `string`，故该分支实际不可达；保留是为忠实而非为覆盖面。
2. **`switch` 用严格相等**：`switch (undefined)` **不匹配任何 case**（`processArmorClass` 的 `default` 是唯一接住它的地方），`case 0:` 只匹配 Number 0。若按 `==` 翻译，`null` 与 `0` 会走同一支。
3. **`null` 与 `undefined` 不可区分**：对象上读不到成员与显式 `undefined` 在 AS2 里都是 `undefined`（`Item.OTHER` 本身就等于 `undefined`），故 `readSlot` 把「缺失」直接记成 `kUndefined`，比对时「成员不存在」也算匹配。

**机械交叉验证（本次可复核的证据，全部通过）**

| 检查 | 结果 |
|------|------|
| 102 个字面量全部被 C++ 引用（防止某分支漏写 display） | 枚举 103（含 `kCount`）/ 引用 102 / 未用 **0** / 未知 **0** |
| AS 用到的 52 个 `Form.BASEID_*` 是否都在复刻中出现且**数值一致**（值从 `Form.as` 反查） | **0 缺失** |
| `processMaterialKeywords` + `processAmmoBaseId` 的 51 组 `(Material 值, display 字面量)` **顺序 + 值 + 字符串**逐一对齐 | 51/51，**0 mismatch** |
| 关键词判定链（`keywords["X"] != undefined`）逐函数按顺序比对：material 55 / misc 17 / book 2 / armorClass 2 | **0 mismatch**（条数亦一致） |
| 写入成员**静态站点数**对齐（AS ↔ 复刻）：`subType` 73 ↔ 60 调用 + 9 显式 number + 3 undefined + 1 null；`subTypeDisplay` 72 ↔ 60 + 12；`weightClass` 6 / `weightClassDisplay` 8；`duration` 2 / `magnitude` 2 / `infoArmor` 1 / `infoDamage` 2；`material` 52 / `materialDisplay` 52；`status` 3 / `isEnchanted` 3 | 全部相等 |
| 写入成员**集合** | AS 22 个，复刻无「多写」；`duration` / `magnitude` / `infoArmor` / `infoDamage` 经 `roundOrNull` 间接写入，站点数已单独核对 |

> 第 5 行那次比对**真的抓到了 bug**：`armorClass` 起初把 `:118` 的 `weightClass = null` 折叠写成「weightClass + display」成对写入，比 AS 多写了一次 `weightClassDisplay`。最终值不可观测（`:120` 随后必写 `$Other`），但「写入集合与 AS 精确相同」是本文件的立身之本，故已改为 longhand 并顺带把 `:120` 挪回 switch **之前**（与 AS 同序）。

**构建验证**：`RelWithDebInfo`，`/W4 /WX` ⇒ **0 warning / 0 error**。

**回退验证（§8.2 规则 1，**已实测**）**：`SSE_REPLICATE_PROCESS_ENTRY` 是**编译期宏**（不是 `constexpr bool`，理由见 `ProcessEntryReplica.h` 顶部注释：加了一条翻译单元，宏为 0 时该文件编译成**空 TU**、`ProfilingHooks.cpp` 里**再无任何 `pe::` 引用**，于是「回退产物 == 上一个构建」是**构造性**成立而非行为论证）。

| 构建 | 大小 | `/W4 /WX` | `pe::` / `ProcessEntryReplica` 符号（`dumpbin /SYMBOLS`） |
|------|------|-----------|--------------------------------------|
| `SSE_REPLICATE_PROCESS_ENTRY = 1`（ON） | 829 952 B | 0 warning / 0 error | 存在 |
| `SSE_REPLICATE_PROCESS_ENTRY = 0`（OFF = 回退产物） | 786 432 B | 0 warning / 0 error | **0 条**（已确认） |

差值 43 520 B 即整个复刻的静态体积。两次构建**不逐字节相同**——本项目未开 `/Brepro`，PE 头里带 `TimeDateStamp`，所以「逐字节」在本工程里本来就不可得；可复核的等价证据是**符号表为零 + 体积回落到 4a 量级**。
>
> **世代注（2026-09-23）**：本表是 §10.9 的 **5.1 基线**实测值（ON 829 952 B）。v6.11 重转写后的复核见 **§10.10** 的「回退契约」行：ON = **839 680 B**、OFF 仍 = **786 432 B**。复核方法改用**二进制字符串搜索**而非 `dumpbin /SYMBOLS` —— 本工程的 Release DLL **不带 COFF 符号表**（实测 `dumpbin /SYMBOLS` 只输出 Summary，无符号行），而复刻存在与否在二进制里直接可验：`4b-ii` / `default_potion` / `iconLabel` / `subTypeDisplay` —— ON 有、OFF 全 0。

**真机验收（采集 #14，2026-09-21 20:22–20:23，5 轮纯开 / 关背包；原始条件 / 日志原文 / zone 逐项对照见 [tracy-capture-log.md](./tracy-capture-log.md) §14）**

**指纹与条件**

| 项 | 值 |
|----|-----|
| trace 文件 | `TracyLog/log_2026_09_21_20_23.tracy`，**39 818 005 B** / 写入 **2026-09-21 20:23:39** |
| 实际加载的 DLL | **829 952 B** / 时间戳 **2026-09-21 20:06:58** / SHA-256 **`E3654CD3AC885B40DFE4A858A8BF23F7AAE301D450B9A6F20BC4E89DB57D564E`**（与 `build\bin\RelWithDebInfo\Template.dll` 实测同值；一并部署 `Template.pdb`） |
| 物品数 / 轮数 / 调用数 | **6441** / **5** / `AS::processEntry` **32 205** = 6441 × 5 ✅ |
| 场景 | 与 #11b / #12 / #13 一致的存档与「5 轮纯开 / 关背包」；**日志结构自证用 < 1 s 间隔分簇**（见下） |

**判据裁定（1 / 2 / 3 / 4 / 6 全中；判据 5 未达标，单独展开）**

| # | 判据 | 实测 | 判定 |
|---|------|------|------|
| 1 | 结构自证：`AS::processEntry` 计数 == 全量 `RequestItemCardInfo` | **32 205 == 32 205**（`RequestItemCardInfo` 6455 含 5 轮第 1 项走旧函数对象 + 悬停路径，与 #11b/#13 同形）；`AS::_requestItemInfo` **32 200 = 32 205 − 5** | ✅ |
| 2 | 复刻承接率（fail-closed 是否被触发） | 逐轮 **`processEntry calls 6441, replicated in C++ 6431, shadow-validated 10, forwarded 0, declines 0, apply failures 0`** —— **零回退、零拒件、零写失败**，5 轮全部如此 | ✅ |
| 3 | 影子对照逐成员相等 | 每轮 10 件 / 99 成员（formType 27/32/45/26，8–15 成员/件），**0 mismatch**，5 轮共 **50 件 / 495 成员**；原版自证「10 predicted member(s) identical to the AS body's own result」 | ✅ |
| 4 | 毫秒级尖峰 = AS2 堆暂停（≠ 没做干净） | 每轮**恰好 2 个** ≥ 5 ms 事件：**#35/#49/#50/#49/#49**（formType 恒 **27**）与 **#6230/#6239/#6240/#6239/#6239**（formType 恒 **26**），`formId` **逐轮漂移**（984087→110551→110588→110551→110551 / 883320→883238→883317→883238→883238）⇒ 位置与物品都随堆状态漂移 = **mark-sweep 暂停**。逐轮合计 **13.96 / 13.84 / 15.40 / 13.25 / 15.03 ms → 71.48 / 5 = 14.30 ms/轮**（#13 = 14.26、#12 = 13.76），**C++ 化没有把它消掉，也没有让它缩小**，与 §10.8(12) 第 6 点的预登记一致 | ✅ |
| 5 | 耗时 | **未达承诺 —— 见「耗时」一段** | ⚠️ |
| 6 | 回退契约未被破坏 | 开关仍是宏（见上「回退验证」表）；本轮**未触发任何回退路径**（判据 2 的四个计数全 0） | ✅ |

**耗时（判据 5）：这是本步唯一没兑现的地方**

`tracy-csvexport` 聚合，`AS::processEntry`：

| 采集 | 阶段 | total_ns | count | 均值 µs/次 | **ms/轮** |
|------|------|----------|-------|-----------|-----------|
| 09-20 18:35（#11b） | S0b 全价语料 | 1 516 406 195 | 32 205 | 47.09 | **303.28** |
| 09-21 13:25（#13） | 4a（本步前态） | 1 327 259 222 | 32 205 | 41.21 | **265.45** |
| **09-21 20:23（#14）** | **4a + 4b-ii** | **402 264 854** | 32 205 | **12.49** | **80.45** |

- 相对 **#13（4a）**：265.45 → 80.45 ms/轮 = **−69.7 %**，消掉 **185.0 ms/轮**；
- 相对 **#11b（S0b）**：303.28 → 80.45 ms/轮 = **−73.5 %**，消掉 **222.83 ms/轮**；
- **对照「可消上界 ≈ 289 ms/轮」：只拿到 77 %**；
- **对照「对外承诺 230–270 ms/轮」（口径 = 相对 S0b 的消减量）：实测 222.83，落在下沿之下约 7 ms**；若按「相对 4a 命中轮基线」计（消掉 185.0），则短 45 ms。

**同一份对照里没有任何 zone 上升**（这是判定「窗口变长不是回归」的依据）：

| zone | #14 ms | #13 ms | Δ ms | count（#14 / #13） |
|------|--------|--------|------|--------------------|
| `AS::processEntry` | 402.26 | 1327.26 | **−924.99** | 32 205 / 32 205 |
| `AS::_requestItemInfo` | 536.58 | 568.04 | −31.46 | 32 200 / 32 200 |
| `RequestItemCardInfo` | 400.90 | 419.64 | −18.74 | 6455 / 6455 |
| `FxDelegate::Callback[RequestItemCardInfo]` | 404.18 | 422.29 | −18.11 | 6455 / 6455 |
| `InventoryEntryData::GetValue` | 19.84 | 20.96 | −1.12 | 58 061 / 58 061 |
| `InventoryChanges::GetInventoryWeight` | 1.68 | 1.81 | −0.12 | 286 / 316 |
| `InventoryChanges::GetItemCount` | 18.86 | 22.01 | −3.14 | 258 / 277 |
| `AS::processList` | 10.29 | 12.61 | −2.32 | 4 / 4 |

> ⚠️ **`InventoryMenu opened → closed` 窗口本轮反而变长（R1 1225.3 / R2 1054.1 / R3 866.0 / R4 847.5 / R5 896.9 ms，对比 #12/#13 的 826.5 / 329.7 / 313.2 / 312.7 / 311.8 ms），但这不是回归**：上表显示**每一个 zone 都下降、没有一个上升**，且关键 zone 的 count 逐项相等（32 205 / 32 200 / 6455 全等）⇒ 工作负载相同、总工作量减少。**窗口跨度不是跨会话可比的量**（它含用户关闭菜单的反应时间），这正是 §12.4 第 3 点已经用「跨会话离散 3–6 %，纯 AS 基准甚至 26 %」记录过的教训 —— 本轮把它的**反面**也验证了一次：**窗口变长 + 所有 zone 变短 = 用户在菜单里多待了 ≥ 0.5 s**。任何以窗口跨度为主指标的对比都必须先扣掉这一项。

**修正后的成本模型（本步最有价值的负面结论）**

原来把 S4 读成「`303.3 = 289（解释器）+ 14（堆暂停）`，C++ 化能消掉那 289」。实测把它**证伪**了：

```
S0b  303.3 ms/轮 ≈ 14.3（AS2 堆暂停） + 289.0（AS 侧代码）
4b-ii 80.45 ms/轮 ≈ 14.3（同一暂停，未缩未移） + 66.2（复刻自身的 GFx 成员访问）
```

- **那 289 ms 里约 66 ms 从来不是「解释器开销」，而是 `GetMember` / `SetMember` / `CreateString` 的成本**——AS 原版也要碰同样这些成员，所以任何**忠实**的 C++ 复刻都绕不开它。C++ 化只消掉了 **223 ms** 的解释器/逻辑部分。
- 逐件拆账（80.45 ms / 6441 件 = **12.49 µs/件**，最小值 4.03 µs、最大值 8.72 ms）：单件仍有 **≈24 次 `GetMember`（读，`gather` 无条件读全 22 个成员 + `useSound` 嵌套）+ ≈10–18 次 `SetMember` + ≈5–15 次 `CreateString`（每个显示字符串一次）**，按 ≈0.25 µs/次 GFx 属性操作计 ≈ 10 µs，与实测吻合。扣掉 14.30 ms 暂停后为 **66.15 ms/轮 = 10.3 µs/件**。
- 由此得出**下一步只可能从「少碰成员」入手**（不是「再搬解释器」）：① 把 102 个显示字符串在 `prepare` 时建成 managed `GFxValue` 并在 `apply` 里复用，消掉每件 ~5–15 次 `CreateString`（**这一项是赢过 AS 原版**，AS 每次 `translate` 都新分配一个字符串）；② 把 `gather` 改成**分支化**（只读当前 `formType` 需要的那组），把读量减半，代价是「decline 是对象的性质」要放宽成「decline 是分支的性质」。两项的收益目前都还是**估计值**，按本项目惯例应先用 `-u` 逐事件把 66 ms 拆成 read / write / CreateString 三块再动手。

**仍未做 / 仍开放（如实登记）**

- **「3 个提交、每次构建 + 采集 + 验收」未按规定执行**：第一次落地时无法采集，拆分提交只会制造 3 个不可验收的中间态，故合为一次提交（`e3a01ce`）；本次采集的两份文档回填是第二次提交。
- **翻译 spike 的命中路径已实测 = 四条候选里的第 ①条**（`_global.skyui.util.Translator` + `Invoke(translate)`），102 个字面量（5.1 基线；v6.11 为 103）全部解析成功，日志 `4b-ii: translation spike resolved -- route …` 与 `all 102 display strings resolved` 各出现一次。所以 ②③④ 三条备用路径在本机**未被走到**，其可用性仍属未验证（保留它们仍是对的：换 SkyUI 版本/换 SWF 编译方式时它们是唯一退路）。
- **人工目视抽查**（§8.1 原始要求「UI 数值与 skyui 原版逐项一致」）仍建议补做一次；机器对照已给出 50 件 / 495 成员 0 differs，目视是独立证据而非重复。必须遵守 §12.0 的约束（**不拾取 / 不丢弃 / 不使用 / 不买卖**）。
- **判据 5 的口径登记为缺陷**（与 #13 同一性质）：4b-ii 是**改变行为**的一步，所以「耗时」的参照只能是**同一份 trace 内**的 zone 总量，不能跨会话比窗口跨度。本轮已改按 zone 总量对照（上表），并据此判定「所有 zone 下降」。
- **`kExtraDiagnostics = true` 的构建（829 952 B）已实测完毕**；下一次采集是否继续保留三条诊断（每件 ≈ 55 ns ≈ 0.14 %）由后续步骤决定。

**回退一句话**：`#define SSE_REPLICATE_PROCESS_ENTRY 0` 重建；见上表。


### 10.10 基线纠错：按运行版 SkyUI-Community v6.11 整体重转写 `processEntry`（2026-09-22，**代码 ✅ / 静态验证 ✅ / 真机验收 ✅ / zone 级耗时 ✅**〔2026-09-23 回填〕）

**起因**：§10.9 的复刻基线取自 SkyUI **5.1** 源码树，而本机实际加载的 SWF 是 **SkyUI-Community v6.11** —— 两者是不同世代的 `InventoryDataSetter.as`，于是出现「复刻忠实于源码、却不忠实于运行版」。完整取证（FFDec 反编译 46 个 SWF、v6.11 权威源逐项差异表、以及为何「10/11 分支全等」仍不构成证明）见 [skyui-version-divergence.md](./skyui-version-divergence.md)。

**判定依据（三条独立证据）**：① 反编译显示数据逻辑位于 `interface/inventorymenu.swf`（**不是** `inventorylists.swf`）；② S-1 运行时探针读到的 `_dataProcessors[0]` 成员集合与 v6.11 一致；③ 旧基线在 Falmer 箭上预测 `material = 18`，而 AS 原版给 `8` —— 世代差是唯一能同时解释这三条的分歧源（`FALMER = 8` 才是 v6.11 的值）。

**重转写范围**（逐项表见 skyui-version-divergence.md §6.1）：`T` 枚举 102 → **103** 并按 v6.11 首现顺序重抽（`static_assert` 同步为 103）；`Ctx` 增 `eslId` 且 `prologue` 补写；`infoValue` / `infoWeight` 极性改为 `<= 0 ? null : round`；`materialKeywords` 38 → **24 臂**；`weaponSubType` / `weaponBaseId` / `armorBaseId` / `ammoBaseId` 改为 `formId >>> 24` 双层开关；`armorPartMask` / `armorOther` 补 `PARTMASK_CLOAK` / `PARTMASK_BACKPACK`；**新增** `bookBaseId` / `scrollBaseId` / `potionBaseId`；`miscBaseId` 6 → **7 个插件槽分支**（含 `0xFE` 走 `eslId`）。

**与 §10.9 的口径变化**

| 指标 | §10.9（5.1 基线） | §10.10（v6.11，现行） |
| --- | --- | --- |
| 权威源规模 | `InventoryDataSetter.as` 879 行（`:24-110` + `:115-878`）/ 18 个 helper | **1304 行**（`processEntry` `:7-79` + **20 个 helper** `:80-1304`） |
| 翻译字面量 | 102 | **103**（`static_assert(kTranslationCount == 103)`，`ProcessEntryReplica.h:226`） |
| `material` 写入站点 | 52 | **40**（`material(c, …)` 实测 40 处） |
| `(Material, display)` 组 | 51 | **40**，逐项 `(值, display)` **40/40 完全一致** |
| 复刻规模 | `.h` 331 行 / `.cpp` 2094 行 | `.h` **393 行** / `.cpp` **3016 行**（`case` 标签 320 / `T::` 引用 176） |
| 静态验证 | 51/51 对齐、0 未用字面量 | **40/40 对齐**、`T::` 无悬空 / 无未用、枚举计数钉死为 103 |
| 编译 | 0 warning / 0 error | **0 warning / 0 error**（`/W4 /WX`），ON 产物 **838 656 B**（`iconLabel` 第二个缓存落地后 **839 680 B**，见下「仍开放」） |

**真机验收（采集 2026-09-22 23:32，4 轮）** —— 与 §10.9 同一套判据：

| 判据 | 观测值 |
| --- | --- |
| 判据 2（计数 + 影子对照，最高优先） | 每轮 `processEntry calls 6441 / replicated in C++ 6424 / shadow-validated 17 / forwarded 0 / declines 0 / apply failures 0`；**`17 item(s) / 190 member(s) / 0 mismatch(es) / 11/11 branch(es)`**，4 轮全同 |
| 翻译 spike | 仍是四条候选中的第 ①条；`103 literals to resolve` → `all 103 display strings resolved` |
| **原缺陷** | Falmer 箭 `formType 42 formId 230209`（`0x38341`）→ **`all 14 predicted member(s) identical`**，§10.9 时期那行 `material 18 vs 8` 的差异**消失** |
| Tier 1 字符串缓存 | 已生效：`103 display strings cached as managed GFxValue for this menu instance`（即 §10.9「下一步」所列 ① 项的落地形态） |
| 回退契约 | ✅ **已复核（2026-09-23）**：`SSE_REPLICATE_PROCESS_ENTRY=0` 重建 = **786 432 B**，且二进制内**零复刻字符串**（`4b-ii` / `default_potion` / `iconLabel` / `subTypeDisplay` 全 0，`AS::processEntry` 仅剩 zone 名）；改回 `1` 重建回 **839 680 B**，复刻字符串齐备 |

> `eslId` 补齐的副作用：每件被预测成员数**统一 +1**（scroll 11 / armor 16 / book 11 / ingredient 9 / light 9 / misc 10 / weapon 15 / ammo 14 / key 9 / potion 12 / soulgem 11），与 §10.9 时期逐项相差 1 —— 这是新增成员，不是预测膨胀。
>
> `4b/outlier`（formType 27 ≈ 6 ms、formType 26 ≈ 7 ms）仍在，属既有的 AS2 堆暂停，与正确性无关。
>
> **zone 级耗时对照（2026-09-23 回填；此前标为「需用 Tracy GUI 另行回填」）**：`tracy-csvexport` 默认聚合模式，本条 trace 的 `AS::processEntry` = **32,205 次 / total 346,142,273 ns / mean 10,748 ns（= 69.23 ms/轮）/ min 2,304 ns**；同 trace 的 `RequestItemCardInfo`（引擎侧，复刻影响不到）**62,015 ns / 6,455 次**、`AS::_requestItemInfo` **16,518 ns / 32,200 次**、`AS::processList` **2,543,941 ns / 4 次**。
>
> 与**分支化前**的同口径对照（四份 trace 一张表）见 [tracy-capture-log.md](./tracy-capture-log.md) **采集 #16**：`gather` 分支化（含显示字符串 managed 化）实测 **−1.575 µs/件 = −10.1 ms/轮**，落在 §15.3 预登记的 9–14 ms/轮区间内；而**本次换代本身是增重的** —— v6.11 重转写令同一 zone 由 10,535 → 10,748 ns（**+2.0 %**），与「每件多读 1 个 `eslId` 成员 + body 由 879 行增至 1304 行」一致。

**仍开放（如实登记）**

1. ✅ **`iconLabel` 已复刻（2026-09-23，第二个缓存）**：v6.11 `processMiscBaseId` 的 `MISCARTIFACT` 臂写裸字符串 `a_entryObject.iconLabel = "default_potion"`（`:970`），是全文件唯一不经 `Translator.translate` 的字符串成员。已按「第二个缓存」落地：`T::kRawDefaultPotion`（排在 `kCount` **之后**，故 `kTranslationCount == 103` 与两个 `static_assert` 不变）、`g_rawLiteralValue`（单值 managed `GFxValue`，与 103 条表缓存同生命周期、同在 `ensureManagedStrings` 里 all-or-nothing 创建）、`WriteSet::putLiteral`、`apply` 的按 key 双缓存分派（先判 `kRawDefaultPotion` 再判越界，越界仍 fail-closed）。`iconLabel` 因此进入预测集，**影子比对现在能看到它**。编译 0 warning / 0 error；ON 产物 838 656 → **839 680 B**（+1 024 B）。⚠️ **真机复验待做**：本改动改变了预测集，需下次采集确认 `iconLabel` identical 且 `0 mismatch` 不破。**（2026-09-23 更新：含本改动的 839,680 B 产物已部署至 MO2，两处 SHA-256 实测一致 —— 采集前置已就绪，见 [tracy-capture-log.md](./tracy-capture-log.md) 采集 #17.3。）**
2. ✅ **OFF 构建尺寸核对已通过（2026-09-23）**：见上表「回退契约」行。
3. ✅ **其余文档的世代口径已刷（2026-09-22）**：`docs/inventory-system-analysis.md` §7.4 加「5.1 vs v6.11」勘误、§8.2 新增 `extern/SkyUICommunity_v6.11_权威源/` 行；本文 §3.1 / §7.3 行内注「5.1 基线」、附录 B 表头加行号基线注；`docs/tracy-capture-log.md` §14 加基线横幅。（复查过：源码注释里 8 处 `102` 计数同步改为 `103`，剩下的 `102` 仅是 AS 行号引用与 `.h:90` 的历史说明。）

## 附录 A：探针与插入点清单

**AS 侧入口统一为**：`RE::InventoryMenu::RUNTIME_DATA::root`（= `_level0.Menu_mc` 的 `GFxValue` 句柄，`InventoryMenu.h`）→ `GetMember("inventoryLists")` → `GetMember("itemList")` → `GetMember("_dataProcessors")` → `GetElement(0)`。**不要用 `_root`** —— 它比 `Menu_mc` 高一层（§4.3）。

| 目标 | AS 位置 | 机制 | 阶段 |
|------|---------|------|------|
| `FxDelegate::Callback`（既有靶点 2.5） | C++ vtable slot 1（`src/ProfilingHooks.h:533-554`，slot 常量在 `:536`） | vtable patch | 已完成 |
| `RequestItemCardInfo`（既有靶点 2.1） | C++ `fxDelegate->callbacks` 表（`src/ProfilingHooks.cpp:818`） | Detours | 已完成 |
| `<list>._dataProcessors[0].processList` | `ItemcardDataExtender.as:40` | `GetMember` + `CreateFunction` + `SetMember` | **S0b ✅ 真机（§10.7）**，但 ⚠️ **只覆盖到「空转」调用**（窗口内零子事件）→ **S3 剔除** |
| `<list>._dataProcessors[0]._requestItemInfo` | `ItemcardDataExtender.as:22` | 同上（**实例成员**） | **S0a ✅ 真机通过**（§10.5）→ 4a |
| `<list>._dataProcessors[0].processEntry` | `InventoryDataSetter.as:24` | 同上 | **S0b ✅ 真机通过**（§10.7，count 严格相等）→ **4b-ii**（实测 **303.3 ms/轮，37.7 %**） |
| `BasicList.InvalidateData`（P4，未纳入） | `BasicList.as:263` | 同上（**影响面最大**） | 保留 |

> ⚠️ **S0a 实测补充（§10.5(6)）**：`RequestItemCardInfo` 还有一条**悬停/事件路径**（`ItemMenu.as:357` `onItemHighlightChange`、`InventoryMenu.as:223` / `:250`），它**直接调委托、不经过任何 processor 成员**，因此**不在上表的插入点范围内**。全量路径（上表三个 processor 成员 —— `processList` / `_requestItemInfo` / `processEntry`，即 S0a/S0b 的插入点）覆盖 99.9 % 的 `RequestItemCardInfo`；若要覆盖滚动瞬间的往返，需要**新增**插入点 —— 从 C++ 侧兜底（靶点 2.1 已 hook `RequestItemCardInfo`）拿不到 AS 侧的 `_selectedIndex` 语义，不能直接替代。

## 附录 B：源码行号速查

> ⚠️ **行号基线（2026-09-22 注）**：下表行号基于 **5.1 的 `extern/SkyUISrc/`**；运行版 SWF 实为 SkyUI-Community **v6.11**（权威源 `extern/SkyUICommunity_v6.11_权威源/`），其中 `InventoryDataSetter.as`（复刻目标）的行号以 v6.11 为准 —— 见 §10.10 与 [skyui-version-divergence.md](./skyui-version-divergence.md)。

| 文件 | 行 | 内容 |
|------|----|------|
| `extern/SkyUISrc/src/ItemMenus/ItemcardDataExtender.as` | 22-28 | `_requestItemInfo` 定义（实例成员） |
| 同上 | 26 | `GameDelegate.call("RequestItemCardInfo", [], a_target, "updateItemInfo")` |
| 同上 | 34-37 | `updateItemInfo` → 存 `_itemInfo` |
| 同上 | 40-58 | `processList`（`:44` 循环、`:46-47` 过滤、`:52` fixSKSE、`:55` 往返、`:56` processEntry） |
| 同上 | 63 | `processEntry` 抽象定义 |
| 同上 | 65-119 | `fixSKSEExtendedObject`（纯 AS） |
| `extern/SkyUISrc/src/ItemMenus/InventoryDataSetter.as` | 24-35 | `processEntry` 前段（`infoValue`/`infoWeight`/`infoValueWeight`） |
| 同上 | 37-101 | formType 分发（`:48` armor、`:77`/`:86` damage、`:47`/`:75`/`:85` effects、`:76` poisoned） |
| 同上 | 24-878 | 全文 879 行（4b-ii 的复刻量） |
| `extern/SkyUISrc/src/ItemMenus/InventoryMenu.as` | 15 / 53 / 73 / 74 | `extends ItemMenu` / `InitExtensions` / `itemList` / `addDataProcessor(new InventoryDataSetter())` ← **phase 4 目标**（实测 = `_dataProcessors[0]`，§10.4） |
| 同上 | 75 / 76 | `addDataProcessor(new InventoryIconSetter(...))` / `addDataProcessor(new PropertyDataExtender(...))` —— 实测 = `_dataProcessors[1]` / `[2]`，**无** `_requestItemInfo`，非目标（§10.4） |
| `extern/SkyUISrc/src/ItemMenus/ItemMenu.as` | 87-88 | `skse.ExtendData(true); skse.ForceContainerCategorization(true);` |
| `extern/SkyUISrc/src/Common/skyui/components/list/BasicList.as` | 263-287 | `InvalidateData`（`:275-276` 跑 processor；`:278` invalidate；`:283` UpdateList） |
| `extern/SkyUISrc/src/CLIK/gfx/io/GameDelegate.as` | 14-27 | `call()`（`:25` `ExternalInterface.call`；`:26` `delete` → 证明同步） |
| 同上 | 29-39 | `receiveResponse` |
| `extern/CommonLibSSE-NG/include/RE/F/FxDelegateArgs.h` | 16-20 | `Respond` / `operator[]` / `GetHandler` / `GetMovie` / `GetArgCount` |
| `extern/CommonLibSSE-NG/src/RE/F/FxDelegate.cpp` | 21 | `FxDelegateArgs(a_args[0], handler, movie, &a_args[1], a_argCount - 1)` —— **H3 证伪的证据链**（实参个数恒比 AS 侧 `params.length` 少 1） |
| `extern/CommonLibSSE-NG/include/RE/I/InventoryMenu.h` | RUNTIME_DATA | `root` @00 = `_level0.Menu_mc`（S-1 的**唯一入口**）；`itemList` @18 |
| `extern/CommonLibSSE-NG/include/RE/G/GPtr.h` | 157-177 | `get()` / `explicit operator bool` / `operator->`（`ui->GetMenu<T>()` 的返回类型） |
| `extern/CommonLibSSE-NG/include/RE/F/FxDelegate.h` | 41 / 19-26 | `Callback`（靶点 2.5）/ `CallbackDefn` |
| `extern/CommonLibSSE-NG/include/RE/G/GFxMovie.h` | 54-62 | `CreateObject` / `CreateArray` / `CreateFunction` / `SetVariable` / `GetVariable` / 数组族 |
| `extern/CommonLibSSE-NG/include/RE/G/GFxFunctionHandler.h` | 16-27 / 32 | `Params`（`thisPtr` 在 `:20`）/ `Call` |
| `extern/CommonLibSSE-NG/include/RE/G/GFxValue.h` | 330-341 / 359-384 | `Is*` / `GetNumber` / `GetMember` / `SetMember` / `Invoke` / `PushBack` / `GetArraySize` |
| `extern/CommonLibSSE-NG/include/RE/I/IMenu.h` | 106 / 113 | `uiMovie` / `fxDelegate` |
| `src/ProfilingHooks.h` | 452-484 / 533-554 | 靶点 2.1 / 靶点 2.5 的 hook |
| `src/ProfilingHooks.h` | 88-138 | **S0a + S0b 探针声明**：`requestAsProbeInstall` / `maybeInstallAsProbe`（含「三个探针原子装入」、`thisPtr` 语义分叉与「每轮重新上膛」的必要性） |
| `src/ProfilingHooks.h` | 452-484（调用点 `:474`） | `RequestItemCardInfoHook::hook` 里的三个 one-shot 挂点（`probeDelegateArgs` / `installAsPathProbe` / `maybeInstallAsProbe`） |
| `src/ProfilingHooks.cpp` | 786-836 / 838-896 | 两个 install 实现（幂等约定） |
| `src/ProfilingHooks.cpp` | 198-665 | **S0a + S0b 实现**（`s0` 命名空间）：注释与成员表 `:263-297`、状态 `:326-333`、`reportFirstCall` `:367-378`、`forwardVerbatim` `:405-408`、三个 handler `:439-493`、`makeProbe` `:508-521`、`resolveTargetProcessor` `:536-574`、`restoreMembers` `:585-590`、`install` `:612-663` |
| `src/ProfilingHooks.cpp` | 697-707 | `MenuOpenCloseListener` 的 `opening` 分支**只上膛**（`requestAsProbeInstall()`），不做图操作 |
| `src/ProfilingHooks.cpp` | 1072-1134 | `requestAsProbeInstall` / `maybeInstallAsProbe`（含 `kMaxInstallAttempts` 重试上限） |
| `src/Profiling.h` | 29-49 | `SSE_ZONE` 等宏（`TRACY_ENABLE` 关闭时为空） |

---

## 附录 C：本文档引用的实测数据来源

| 数据 | 出处 |
|------|------|
| skyui 增量 +779 ms、C++ 414 ms (53 %)、边界 1 ms (0.1 %)、AS 364 ms (47 %) | [tracy-capture-log.md](./tracy-capture-log.md) 采集 #9 / #10；[tracy-integration-plan.md](./tracy-integration-plan.md) §4.6.4 |
| 单次 `RequestItemCardInfo` 64.4 µs；`GetValue` 占比 ≈ 2 % | 同上（采集 #7–#10） |
| 物品数 6441（19323 ÷ 3；vanilla `GetValue` 9275 ÷ 1.44 双路自洽） | [tracy-capture-log.md](./tracy-capture-log.md) 采集 #9 结论 4 |
| 靶点 2.5 的 `Callback[…] − RequestItemCardInfo` = 1.0–1.2 ms/轮 | 同上（采集 #9） |
| skyui 与 vanilla 的机制差异（`processList` vs 高亮单次） | [inventory-system-analysis.md](./inventory-system-analysis.md) §7.4 / §7.6 |
| S-1 首跑：`argCount = 0`（H3 证伪）、路径探测零输出、两处实现缺陷 | 2026-09-20 11:10–11:13 真机日志；分析与修正记录见本文档 **§10.2** |
| S-1 第二次实测：6 条验收日志全命中、`inventoryLists.itemList` 经缓存句柄 `GetMember` 命中（H1 成立）、`_dataProcessors = array[3]` 且 `[0]` 为唯一目标、`entryList = array[6441]` | 2026-09-20 11:35–11:36 真机日志；分析与落档见本文档 **§10.4** |
| S-1 第三次实测：`dumpProcessors` 的 `isTarget` 修正生效（`[1]`/`[2]` 不再带 `TARGET` 后缀） | 2026-09-20 11:48 真机日志；见本文档 **§10.4 (3)** |
| S0a 落地：探针设计（一次 `Invoke` 满足三个语义、失败安全逐路径、handler 生命周期取舍）与构建验证（RelWithDebInfo 下 ON 762 880 / OFF 612 352 字节，0 警告；OFF 与 S-1 基线逐字节相同） | 本文档 **§10.5**；代码 `src/ProfilingHooks.h:88-138`、`src/ProfilingHooks.cpp:198-665` / `:1072-1134`；构建 2026-09-20（`/W4 /WX`）。**注**：其中 `h/cpp` 行号已按 S0b 落地后的当前值刷新（S0a 时期为 `h:88-128`、`cpp:198-450` / `:856-917`），见 §10.5(1) 表下注 |
| **S0a 真机实测（5 轮，判据 1–6 全中）**：`RequestItemCardInfo` 32242 = 全量 32206 + 悬停 36、`AS::_requestItemInfo` 32201、差 41 = 5（每轮第 1 次走旧函数）+ 36（悬停路径不经探针）；`RequestItemCardInfo` **双路径**（`ItemcardDataExtender.as:26` 全量 vs `ItemMenu.as:357` / `InventoryMenu.as:223`·`:250` 悬停/事件）；单次 69.4 µs（`RequestItemCardInfo`）/ 77.8 µs（`AS::_requestItemInfo`） | 2026-09-20 真机日志（5 轮开背包 + 中途滚动）+ `TracyLog/log_2026_09_20_15_29.tracy`，经 `extern\TracyProfiler\release\tracy-csvexport.exe -u` 逐事件分簇分析；**采集记录见 [tracy-capture-log.md](./tracy-capture-log.md) 采集 #11a**；对账与结论见本文档 **§10.5(6)** |
| **S0b 落地**：三个探针一次 `install()` 原子装入（三阶段 = 快照 / 建函数 / 写入 + 回滚）、`thisPtr` 语义分叉（`apply` → list；成员调用 → processor）、zone 名必须留在 `SSE_ZONE` 字面量处因而每探针一个小类；构建验证（RelWithDebInfo：ON 767 488 / OFF 612 352 字节，0 警告；OFF 与 S-1 基线**逐字节相同**） | 本文档 **§10.6**；代码 `src/ProfilingHooks.h:88-138`、`src/ProfilingHooks.cpp:198-665` / `:1072-1134`；构建 2026-09-20（`/W4 /WX`） |
| `BasicList.as:275-276` 的调用形态 `_dataProcessors[i].processList(this)`（→ S0b 两个探针的 `thisPtr` = processor，与 S0a 相反） | `extern/SkyUISrc/src/Common/skyui/components/list/BasicList.as:275-276`（源码查证，2026-09-20） |
| **S0b 真机实测（采集 #11b，判据 1–6 全中）**：`AS::processEntry` **32205 == 全量 `RequestItemCardInfo` 32205**（逐轮 6441）、`AS::_requestItemInfo` 32200 = 32205 − 5、`AS::processList` **4 次全为空转**（窗口内零子事件 → **S3 剔除**）；四个 zone 全部 `thread = 10`（H4 成立）；分解 **S4 = 303.3 ms/轮（37.7 %）** / S1+S2 AS 侧 57.9 ms（7.2 %）/ C++ 侧 443.4 ms（55.1 %） | `TracyLog/log_2026_09_20_18_35.tracy`（51,457,139 B，2026-09-20 18:35:50），经 `extern\TracyProfiler\release\tracy-csvexport.exe` 聚合导出 + `-u` 逐事件（窗口/线程/分簇）核对；**采集记录见 [tracy-capture-log.md](./tracy-capture-log.md) 采集 #11b**；对账与裁决见本文档 **§10.7** |
