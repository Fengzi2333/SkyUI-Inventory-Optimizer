# Tracy Profiler 集成方案与插桩点（阶段 1）

> 本文档是 [inventory-system-analysis.md](./inventory-system-analysis.md) 的阶段 1（Profiling 诊断）落地方案，记录 Tracy Profiler 的集成方式、编译开关与插桩靶点清单。
>
> - 目标项目：`SSEMod`（SKSE 插件 / CommonLibSSE-NG 模板）
> - Tracy 源码位置：`extern/TracyProfiler`
> - Tracy 版本：**0.14.1**（`public/common/TracyVersion.hpp`），git HEAD `dd29819f`
> - 实测归档：**[tracy-capture-log.md](./tracy-capture-log.md)** —— 每次采集的条件、实测数据与对 §5 判据（Q1–Q7）的贡献

---

## 1. 现状与环境

### 1.1 已就位的资源

| 项 | 路径 / 值 | 说明 |
|----|-----------|------|
| Tracy 客户端源码 | `extern/TracyProfiler/public/` | `TracyClient.cpp` + `tracy/`、`client/`、`common/` 头文件 |
| 主头文件 | `extern/TracyProfiler/public/tracy/Tracy.hpp` | 插桩宏入口 |
| **预编译 GUI** | `extern/TracyProfiler/release/tracy-profiler.exe` | 33 MB，**直接运行，无需自行编译 profiler** |
| 其它工具 | `release/tracy-capture.exe`、`tracy-merge.exe`、`tracy-csvexport.exe` | 命令行捕获 / 合并 / 导出 CSV |
| Tracy 版本 | 0.14.1 | 由 `public/common/TracyVersion.hpp` 的 `Major/Minor/Patch` 决定，`cmake/version.cmake` 解析 |

> **不需要**从零编译 profiler GUI：`release/` 下已有官方发布的预编译 `tracy-profiler.exe`。

### 1.2 项目侧关键事实

| 项 | 值 | 影响 |
|----|-----|------|
| 项目 target 名 | `${PROJECT_NAME}`（当前为 `Template`） | `target_link_libraries(${PROJECT_NAME} ...)` |
| C++ 标准 | **C++20**（`cmake/pluginconfig.cmake`） | Tracy 仅需 C++11，兼容 |
| 构建系统 | CMake ≥ 3.25 + vcpkg（`CMakePresets.json` / `vcpkg.json`） | Tracy 要求 CMake ≥ 3.13，兼容 |
| 源文件收集 | `cmake/addpluginsources.cmake` 用 `GLOB_RECURSE ... CONFIGURE_DEPENDS "src/*.cpp"` | **新增 `src/*.cpp` 会被自动编译**，无需改 CMakeLists 的源列表 |
| PCH | `src/PCH.h` 通过 `target_precompile_headers` 注入 | 可在此统一 include Tracy |
| 依赖目录 | **`extern/` 被 `.gitignore:2`（`/extern/`）整体忽略** —— 依赖是「本地可选目录」，不在版本控制中（`git ls-files extern` 为空，`git check-ignore -v extern/TracyProfiler` 命中 `.gitignore:2`） | 源码**不能无条件 `#include <tracy/...>`**：Tracy 必须按「可选依赖」对待（§3.3） |
| 现有日志 | spdlog（`src/Plugin.cpp:47` 有 `flush_on(spdlog::level::info)`） | 每写一条 info 都会**同步刷盘** → 不适合高频函数打点（详见 §4.5） |

### 1.3 Tracy 客户端的默认行为（重要）

`public/tracy/Tracy.hpp` 顶部的宏逻辑（第 25 行起）：

```cpp
#ifndef TRACY_ENABLE
    // 所有插桩宏在此分支被定义为「空操作」
    #define ZoneScoped
    #define FrameMark
    #define TracyPlot(x,y)
    // ...
#else
    // 真实实现（ScopedZone / FrameMark / Plot 等）
#endif
```

结论：

- **默认（不定义 `TRACY_ENABLE`）= 全部 no-op**（该分支共 **87 个** `#define`，`public/tracy/Tracy.hpp:25-130`），插桩点零开销，可安全把插桩代码留在发布版本里；
- **必须显式定义 `TRACY_ENABLE`** 才会真正采集；
- ⚠️ 两个分支的宏**参数名并不一致**（如 `ZoneScopedN(x)` vs `ZoneScopedN( name )`）→ **不要自行重复定义 Tracy 的宏名**（`/W4 /WX` 下 C4005 即构建失败），详见 §3.3。

---

## 2. CMake 集成方案

### 2.1 为什么可以直接 `add_subdirectory`

Tracy 的 `CMakeLists.txt` 已为本项目这类场景做好设计：

| 位置 | 内容 | 意义 |
|------|------|------|
| `CMakeLists.txt:48` | `add_library(TracyClient ${TRACY_VISIBILITY} ".../TracyClient.cpp")` | 客户端只有**一个翻译单元** |
| `CMakeLists.txt:104` | `add_library(Tracy::TracyClient ALIAS TracyClient)` | **`Tracy::TracyClient` 别名在 build tree 中可直接 link** |
| `CMakeLists.txt:51-53` | `target_include_directories(TracyClient SYSTEM PUBLIC $<BUILD_INTERFACE:.../public>)` | 头文件以 `<tracy/Tracy.hpp>` 形式可用，且标记 SYSTEM（不触发 `/W4 /WX` 告警） |
| `CMakeLists.txt:16` | `TRACY_STATIC` 默认 ON（`BUILD_SHARED_LIBS` 未开） | 静态链入插件 DLL |
| `CMakeLists.txt:25-27` + `pluginconfig.cmake:3` | 项目已设 `CMAKE_INTERPROCEDURAL_OPTIMIZATION ON` → `LTO_SUPPORTED=TRUE` → `TRACY_VISIBILITY=OBJECT` | 在本项目中 TracyClient 实际以 **OBJECT 库**形式构建（CMake ≥ 3.12 支持 link OBJECT 库） |
| `CMakeLists.txt:111-113` | `set_option(TRACY_ENABLE ...)` / `set_option(TRACY_ON_DEMAND ...)` / `set_option_value(TRACY_CALLSTACK ...)` | Tracy **原生 CMake 开关** |
| `cmake/options.cmake:17` | `target_compile_definitions(${ARGV3} PUBLIC ${option})` | **PUBLIC 传播**：设 `TRACY_ENABLE=ON` 后，链接 TracyClient 的插件 target 会自动获得 `TRACY_ENABLE` 宏，无需手工重复添加 |

> ⚠️ `CMakeLists.txt:8` 会执行 `file(GENERATE OUTPUT .gitignore CONTENT "*")`，在 **build 目录**生成一个 `.gitignore`，属正常行为，不影响源码树。

### 2.2 推荐改法（方案 A：条件化引入）

> **为什么方案 A 是默认推荐**：`extern/` 已被 `.gitignore:2`（`/extern/`）整体忽略 —— Tracy 与 CommonLibSSE-NG 同样是**本地可选依赖，不在版本控制中**（`git ls-files extern` 为空）。所以源码**不能无条件 `#include <tracy/Tracy.hpp>`**，否则在新环境（`extern/` 为空）会直接编不过。**方案 A + §3.3 的 `src/Profiling.h`** 是唯一能同时兼容「Tracy 存在 / 缺失」与「启用 / 未启用」的组合。

在 `d:\Documents\SSEMod\CMakeLists.txt` 的依赖区末尾（第 44 行 `####################` 之后）追加：

```cmake
######## profiler (Tracy)  (optional)
option(ENABLE_TRACY "Build with Tracy Profiler instrumentation" OFF)

if(ENABLE_TRACY)
	find_path(TracyPath "CMakeLists.txt"
			PATHS	"${CMAKE_SOURCE_DIR}/extern/TracyProfiler"
					"${CMAKE_SOURCE_DIR}/external/TracyProfiler" NO_DEFAULT_PATH)

	if(EXISTS "${TracyPath}")
		# Tracy 原生选项；均为 PUBLIC 传播，插件 target 会自动获得 TRACY_ENABLE 宏
		set(TRACY_ENABLE ON CACHE BOOL "Enable Tracy client" FORCE)
		set(TRACY_ON_DEMAND ON CACHE BOOL "Collect data only while GUI is connected" FORCE)

		add_subdirectory("${TracyPath}" TracyProfiler EXCLUDE_FROM_ALL)
		target_link_libraries(${PROJECT_NAME} PRIVATE Tracy::TracyClient)
		message(STATUS "Tracy Profiler: ENABLED (${TracyPath})")
	else()
		message(WARNING "ENABLE_TRACY=ON but extern/TracyProfiler was not found; profiling disabled")
	endif()
endif()
```

要点：

1. `set(TRACY_ENABLE ON CACHE BOOL "" FORCE)` **必须写在 `add_subdirectory` 之前**：Tracy 内部用 `option()` 声明同名变量，而 `option()` 不会覆盖已存在的 cache 值，因此这里预先设置即可生效（也可以从命令行传 `-DTRACY_ENABLE=ON`）。
2. `EXCLUDE_FROM_ALL` 避免 Tracy 的 install/export 规则干扰插件打包（`cmake/pluginpackage.cmake`）。
3. 用 `find_path` 探查 Tracy 路径，与 `plugintarget.cmake:5-12` 查找 CommonLibSSE 的既有写法保持一致。
4. 新增的 `src/*.cpp` 会被 `addpluginsources.cmake` 的 `GLOB_RECURSE ... CONFIGURE_DEPENDS` 自动纳入编译，**无需修改源文件列表**。

构建命令（RelWithDebInfo + 开启插桩）：

```powershell
cmake --preset default -DENABLE_TRACY=ON
cmake --build --preset relwithdebinfo
```

### 2.3 备选：方案 B（无条件引入 + 仅靠宏开关插桩）

去掉 `if(ENABLE_TRACY)` 判断，始终 `add_subdirectory` + link，只用 `TRACY_ENABLE` 控制是否真正插桩：

- **优点**：源码里永远可以无条件 `#include <tracy/Tracy.hpp>`，未启用时宏自动 no-op（§1.3）；
- **代价**：`TracyClient.cpp` 始终被编入 DLL（体积 +数百 KB）。`TRACY_ENABLE=OFF` 时插件侧无任何 zone 调用，因此 profiler 不会被初始化、不启动后台线程，**运行时开销仍为 0**。

> **方案 B 的前提**：Tracy 目录**必然存在**（需自己把 `extern/TracyProfiler` 纳入版本控制 / 改为 git submodule / 走 vcpkg）。收益是源码可直接用 Tracy 原生宏名、**无需 §3.3 的封装头**；代价是「Tracy 缺失即编译失败」，且 `TracyClient.cpp` 始终被编入 DLL（体积 +数百 KB，`TRACY_ENABLE=OFF` 时仍无运行时开销）。**在本仓库当前状态（`extern/` 被 `.gitignore` 忽略）下不满足该前提 → 采用方案 A。**

---

## 3. 编译开关与宏

### 3.1 宏速查

| 宏 / CMake 选项 | 默认 | 作用 |
|---|---|---|
| `TRACY_ENABLE` | **OFF**（Tracy 的 CMake option） | 总开关。未定义时 `Tracy.hpp` 内所有插桩宏为 no-op（§1.3） |
| `TRACY_ON_DEMAND` | OFF | **按需采集**：仅当 GUI 连接后才收集数据。长时间挂机、只想偶尔采样时**强烈建议开启** |
| `TRACY_CALLSTACK=<n>` | 未设置 | 给 zone 附加上下文调用栈（深度 n）。开销显著增大，**阶段 1 不要开** |
| `TRACY_NO_EXIT` | OFF | 进程退出前等待数据发完。**SKSE 插件是 DLL，"退出"语义不同 → 保持 OFF** |
| `TRACY_ONLY_LOCALHOST` | OFF | 仅监听 localhost，省去局域网广播探测 |
| `TRACY_NO_SAMPLING` | OFF | 关闭采样。若只要 zone 时间线，可开启以减少后台开销 |
| `TRACY_NO_CODE_TRANSFER` | OFF | 不传输源码（源码内嵌于 PDB/代码段）。可减小握手开销，但 GUI 里看不到源码行 |

### 3.2 启用 / 关闭的三种写法（推荐 ①）

1. **CMake 选项（推荐）**：`-DTRACY_ENABLE=ON -DTRACY_ON_DEMAND=ON`
   —— 配合 §2.2 的 `ENABLE_TRACY=ON` 使用，PUBLIC 传播自动生效（`cmake/options.cmake:17`）。
2. **target 编译定义**：`target_compile_definitions(${PROJECT_NAME} PRIVATE TRACY_ENABLE TRACY_ON_DEMAND)`
   —— 仅在不想使用 Tracy 原生 CMake 选项时需要。
3. **源码内 `#define`**：在任何 `#include <tracy/Tracy.hpp>` 之前 `#define TRACY_ENABLE`
   —— ⚠️ 不推荐。

> ⚠️ 若把 Tracy 头放进 `src/PCH.h`，`TRACY_ENABLE` 必须在**编译 PCH 时**就已定义。方式 1/2 通过 target 属性满足（PCH 编译同属该 target）；方式 3 无效（`PCH.h` 自身就是 PCH 根，其它 `.cpp` 里的 `#define` 来不及生效）。

### 3.3 `src/Profiling.h`：**必需的**薄封装头

⚠️ **前提事实**：`extern/` 已被 `.gitignore:2`（`/extern/`）整体忽略 —— Tracy 是**本地可选依赖，不在版本控制中**（§1.2）。因此源码里**不能无条件 `#include <tracy/Tracy.hpp>`**：新环境 clone 后 `extern/` 为空，插件会**直接编不过**。

→ 结论：**必须有一个薄封装头**，使「Tracy 存在但未启用」与「Tracy 完全不存在」两种情况都退化为正常构建。

#### ✅ 正确写法：`#ifdef TRACY_ENABLE` + **自有前缀**宏，按需转发

```cpp
// src/Profiling.h
#pragma once

// 唯一职责：转发。未启用时提供自有命名的空操作宏。
// 关键：绝不重定义 Tracy 的宏名（87 个，且两个分支参数名不同 → C4005，/W4 /WX 下构建失败）。
#ifdef TRACY_ENABLE
    #include <tracy/Tracy.hpp>          // 此时 CMake 已 link Tracy，头文件必然存在
    #define SSE_ZONE(name)      ZoneScopedN(name)      // 命名 zone（热路径只用这个）
    #define SSE_ZONE_FN         ZoneScoped             // 以当前函数名作 zone 名
    #define SSE_VALUE(v)        ZoneValue(v)           // 附加一个数值（8 字节，零分配）
    #define SSE_FRAME(name)     FrameMarkNamed(name)   // 逻辑帧切分（§4.3）
    #define SSE_MSG_L(txt)      TracyMessageL(txt)     // 低频事件消息（禁止用于热路径）
#else
    #define SSE_ZONE(name)      ((void)0)
    #define SSE_ZONE_FN         ((void)0)
    #define SSE_VALUE(v)        ((void)0)
    #define SSE_FRAME(name)     ((void)0)
    #define SSE_MSG_L(txt)      ((void)0)
#endif
```

用 `#ifdef TRACY_ENABLE` 而不是 `__has_include(<tracy/Tracy.hpp>)`：

| 场景 | `#ifdef TRACY_ENABLE` | `__has_include` |
|---|---|---|
| Tracy 未集成（`extern/` 为空）+ 未启用 | ✅ 走 no-op，**不依赖头文件存在** | ✅ 走 no-op |
| Tracy 已集成 + `TRACY_ENABLE=OFF` | ✅ 走 no-op，零开销 | ⚠️ 仍会 include `Tracy.hpp`（引入 87 个 no-op 宏；能用但没必要） |
| Tracy 已集成 + 启用 | ✅ 转发到真实宏 | ✅ 同左 |

自有前缀的代价：**需按需追加用到的转发宏**（阶段 1 需要 `SSE_ZONE` / `SSE_VALUE` / `SSE_FRAME` / `SSE_MSG_L`，见 §4 各插桩点）。漏了会**编译报错**（不会静默失效），属可控成本。

#### ❌ 反面写法：重定义 Tracy 的宏名做兜底（禁止）

```cpp
// 不要这样写！
#ifdef TRACY_ENABLE
    #include <tracy/Tracy.hpp>
#else
    #define ZoneScopedN(x)      // ← 与 Tracy 的宏名/参数名不一致
    #define ZoneValue(x)        // ← Tracy 两个分支的参数名分别是 (x) 和 ( value )
#endif
```

两个硬问题：

1. **覆盖不可能完整**：no-op 分支有 **87 个**宏（`public/tracy/Tracy.hpp:25-130`），手写常用的 8 个之后，一旦用到 `ZoneTransient`、`ZoneValueV`、`ZoneName`、`TracyPlotConfig`、`TracyAlloc`… → **编译错误**（`/W4 /WX` 下即构建失败）。
2. **C4005 重定义风险**：MSVC 判定宏重定义时比较**参数名**，而 Tracy 自己的两个分支参数名就不一致：

   | 宏 | no-op 分支 | 真实分支 |
   |---|---|---|
   | `ZoneScopedN` | `#define ZoneScopedN(x)`（:38） | `#define ZoneScopedN( name )`（:179） |
   | `ZoneValue` | `#define ZoneValue(x)`（:52） | `#define ZoneValue( value )`（:193） |

   一旦重复定义落入同一 TU（例如 `src/PCH.h` 预包含 `Tracy.hpp` 后又出现兜底宏，或将来有人把 `TRACY_ENABLE` 判定写重复）→ 报 **C4005 → `/W4 /WX` 构建失败**。

#### 例外：仅当 Tracy 「必然存在」时才可省掉封装

若明确要求构建环境**始终提供** `extern/TracyProfiler`（把它纳入版本控制 / 改为 submodule / 走 vcpkg），则可采用 §2.3 方案 B：源码无条件 `#include <tracy/Tracy.hpp>`，未启用时由 Tracy 那 87 个 no-op 宏兜底，**无需任何封装**。本仓库当前状态（`extern/` 被忽略）**不满足**该前提。

---

## 4. 阶段 1 插桩点清单

### 4.0 为什么用插桩而不是采样

Tracy 的调用栈采样（sampling）依赖**符号解析**：Windows 上需读取目标模块的 PDB。

- `SkyrimSE.exe` / `SkyrimSE.AE.exe` **没有公开 PDB** → 采样只能拿到裸地址，无法映射到引擎函数名；
- 因此对引擎内部函数，**唯一可行路径是「函数入口 hook + 插桩」**（zone 由我们的 DLL 自己创建，函数名来自源码）。

> 采样对**插件自身代码**仍有价值（有 PDB），可用来看 hook 回调本身的开销。

### 4.1 hook 方式：复用项目现有基础设施

`src/Util.h` 已提供 `Hooking` 辅助（`src/PCH.h:157` 已 include），两种机制：

| 辅助函数 | 机制 | 是否适用本阶段 |
|---|---|---|
| `Hooking::writeCall<T, size>()` / `writeCall<T, size>(addr, offset)` | `SKSE::GetTrampoline().write_call<N>()`，改写**某条 `call` 指令**的目标 | ❌ 只覆盖单个调用点，无法测量函数被引擎内部调用的总耗时 |
| `Hooking::writeDetour<T>()` | Microsoft Detours `DetourAttach`，改写**函数入口** | ✅ **首选**：入口 hook，且可与其它 mod 的 Detours hook **链式共存** |
| `Hooking::writeBranch<T>()` | `SKSE::GetTrampoline().write_branch<5>()`，改写**函数入口** | ✅ **兜底**（`src/Util.h:47-60`，本次新增）：效果同上但**不链式**，仅在 detours 缺失时使用（见注意事项 3） |

> 二者都让 `SSE_ZONE` 的 zone 覆盖被 hook 函数体的全部执行 —— 这是它们相对 `writeCall` 的关键差别。

#### hook 声明模板（与 `src/Hooks.h:27-41` 现有风格一致）

```cpp
#include "Profiling.h"   // §3.3：Tracy 未启用/未集成时 SSE_* 全部退化为空操作

struct GetItemCountHook {
        // ❗ 成员函数：第一个参数必须是 this（x64 下 this 走 rcx）
        // ❗ 签名须逐字复制 RE::InventoryChanges::GetItemCount 的声明 —— 参数类型/数量/返回类型/const 全部一致（见注意点 7）
        static std::int16_t hook(RE::InventoryChanges* a_this, RE::TESBoundObject* a_obj) {
            SSE_ZONE("InventoryChanges::GetItemCount");
            return orig(a_this, a_obj);  // ❗ 必须转发返回值（原函数返回 int16_t）
        }

        static inline std::string logName = "GetItemCount";
        using FuncType = decltype(&hook);
        static inline FuncType orig;                                          // Detours 用裸函数指针
        static inline REL::RelocationID srcFunc = REL::RelocationID{15868, 16047};  // (SE, AE)
        static inline uint64_t srcFuncOffset = 0;                             // writeCall 才需要，此处不用

        static void install() {
            Hooking::writeDetour<GetItemCountHook>();
        }
};
```

安装点：在 `src/Hooks.cpp` 的 `Hooks::install()` 里追加一行（**已落地**：现为第 4-8 行，`ProfilingHooks::install()` 位于第 7 行）：

```cpp
void Hooks::install() {
    QuitGameHook::install();
    QuitGameDetoursHook::install();
    ProfilingHooks::install();   // ← 新增
}
```

调用链（现有）：`SKSEPlugin_Load`（`src/Plugin.cpp:56`）→ `GameEventHandler::onLoad()` → `Hooks::install()`（`src/GameEventHandler.cpp:13`）。

#### 注意事项

1. **返回值必须转发**：现有 `QuitGameDetoursHook` 返回 `void`（`src/Hooks.h:28-31`）所以没体现这点；`GetItemCount` 返回 `std::int16_t`，hook 若漏了 `return` 会读到未初始化值 → 严重行为异常。
2. **`REL::RelocationID{SE, AE}` 顺序**：与 `src/Hooks.h:19`（`{35545, 36544}`）一致，**SE 在前**。
3. **入口 hook 的机制：Detours 优先，SKSE trampoline 兜底**：`Hooking::writeDetour` 内含 `static_assert(BUILDOPTIONS.detoursFound, ...)`（`src/Util.h:78`），而 `DETOURS_LIBRARY` 由 `cmake/BuildOptions.h.in` 的 `#cmakedefine01 DETOURS_LIBRARY` 决定（根 `CMakeLists.txt:32-43` 用 `find_library` 探测）。
   ⚠️ **本次落地时发现的实际问题**：`vcpkg.json` 原先**从未**声明 detours 依赖 → `build/cmake/BuildOptions.h` 一直是 `#define DETOURS_LIBRARY 0` → `writeDetour` 实际是**空操作**（`QuitGameDetoursHook` 也一直没真正生效）。已修正为：
   - `vcpkg.json` 增加**可选 feature** `detours`（**默认不启用**，避免给不需要的构建引入一个 GitHub 依赖；见附录 B）；
   - `ProfilingHooks::install()` 用 `if constexpr (BUILDOPTIONS.detoursFound)` 自动二选一：
     - **detours 可用** → `Hooking::writeDetour<T>()`（可与其它 mod 链式共存）；
     - **detours 缺失** → `Hooking::writeBranch<T>()`（新增，`src/Util.h:47-60`；测量语义相同，但不支持多级链式）。
   - 启用 Detours：`cmake --preset default -DENABLE_TRACY=ON -DVCPKG_MANIFEST_FEATURES=detours`。
4. **hook 冲突**：其它 mod 若也用 Detours，会**自动链式**（Detours 支持多级 hook）；但对方若用 trampoline 直接改写相同字节则会冲突（**走兜底 `writeBranch` 路径时同样不链式** —— 与其它 trampoline 使用者互斥）。安装失败时 `Hooking::writeDetour` 会打 `SKSE::log::error("Failed to install hook")`（`src/Util.h:76`）。
5. **线程安全（分「安装期 / 运行期」两层看）**：
   - **安装期**：`install()` 在 `SKSEPlugin_Load` → `onLoad()` 期间执行（`src/GameEventHandler.cpp:14`），此时处于**单线程初始化阶段、游戏主循环之前**，引擎其它线程尚未大量活动。`static inline FuncType orig` 属于「**安装期写一次 → 之后全线程只读**」的单次初始化模式，**不存在数据竞争**——前提正是「安装先于主循环」，现设计满足。
   - **运行期**：这些函数会在多个线程被调用（渲染 / AI / Papyrus VM / 任务线程）。`SSE_ZONE`（即 Tracy zone）本身线程安全（Tracy 队列按线程分片）。阶段 1 只测量、不引入共享状态 → 天然安全；若后续阶段要在 hook 内加计数器，必须用 `std::atomic` 而非普通变量。
6. **禁止运行期动态 attach / detach**：Detours 的 `DetourAttach` 本身线程安全（以原子操作改写指令前缀，微软文档明确支持运行期挂载），但被 hook 函数若正被其它线程执行，理论上仍存在「某线程恰好停在**被改写指令中间**」的极小窗口。**本方案只在加载期一次性安装、退出前不卸载** → 规避该窗口。不要为了「临时开关测量」而在游戏运行中反复 attach / detach。
7. **hook 签名必须与 CommonLib 声明 1:1 一致**（最容易踩的坑）：
   - x64 Windows 只有**一种**调用约定（MS x64 ABI）：`__cdecl` / `__stdcall` / `__fastcall` / `__thiscall` 在 x64 上**全部统一**——前 4 个参数走 `rcx/rdx/r8/r9`、返回值走 `rax`（浮点走 `xmm0`）。因此**无需也无法**为 hook 指定调用约定；成员函数的 `this` 就是隐式第一个参数（`rcx`）。
   - **必须逐字复制** CommonLib 头文件中的参数类型、数量、返回类型、`const` / 引用限定，**禁止自行「简化」或「推断」**。典型陷阱：`GetItemCount` 返回 `std::int16_t`（窄类型）→ `rax` 高 48 位是垃圾，若签名误写成 `std::int32_t` 就会读到垃圾高位，得到错误数值。
   - 返回**大结构体**的函数使用 **sret**（隐式首参 `rcx` = 返回缓冲区指针），CommonLib 的声明已正确体现，照抄即可。
   - 残余风险：若目标函数是**手写汇编**并违反 ABI（依赖调用返回后的寄存器 / rflags 状态），hook 可能引入难解崩溃。老滚 5 核心逻辑为编译器生成，概率极低；若出现无法解释的崩溃，先反汇编确认该函数是否为纯 C++ 生成。

---

### 4.2 靶点表

reloc 均取自 [inventory-system-analysis.md](./inventory-system-analysis.md) §6（`RELOCATION_ID(SE, AE)`）。

| # | 靶点 | SE | AE | 复杂度 | 优先级 | 说明 |
|---|------|----|----|--------|--------|------|
| 1 | `InventoryChanges::GetItemCount(TESBoundObject*)` | **15868** | **16047** | O(n) | ★★★ | 最高频聚合查询；`n` 次链表跳转 |
| 2 | `InventoryChanges::VisitInventory(IItemChangeVisitor&)` | **15855** | **16095** | O(n) | ★★★ | 大量 mod / UI 用它枚举背包 |
| 3 | `InventoryChanges::GetInventoryWeight()` | 15883 | 16123 | O(n) | ★★ | 重量 UI 每刷新必调 |
| 4 | `InventoryChanges::SendContainerChangedEvent(...)` | **15909** | **16149** ⚠️ | — | ★★ | 度量**放大效应**（拆解文档 §4.2：一次批量操作触发多次事件 → 多次全量刷新） |
| 5 | `InventoryEntryData::GetValue()` | 15757 | 15995 | O(m) | ★★ | 内层 `extraLists` 遍历代表，用于验证 `n × m` 假设 |
| 6 | `InventoryChanges::GetArmorInSlot(int32_t)` | 15873 | 16113 | O(n) | ★ | 找指定槽位护甲 |
| 7 | `InventoryChanges::GetWornMask()` | 15806 | 16044 | O(n) | ★ | 装备状态查询 |
| 8 | `InventoryEntryData::GetEnchantment()` | 15788 | 16026 | O(m) | ★ | 附魔查询（同样走 `extraLists` 遍历） |
| 9 | `ActorEquipManager::EquipObject(...)` | 37938 | 38894 | — | ★ | 装备路径（写侧，观察触发频率） |
| 10 | `ActorEquipManager::UnequipObject(...)` | 37945 | 38901 | — | ★ | |
| — | `TESObjectREFR::GetInventory(filter, noInit)` | —（**CommonLib 自实现**，无 reloc） | — | O(n log n) | ★★ | **无需 hook**：只在**我们自己的调用点**包 `SSE_ZONE`（引擎内部不经此函数） |

⚠️ **已知上游标注可疑**（§6.1 原注）：`SendContainerChangedEvent` / `SetUniqueID` / `TransferItemUID` 三者的 **AE 值均标为 16149**。**hook 前请先反汇编核对**，否则可能 hook 到错误地址 → 游戏崩溃。

#### 当前插桩状态（2026-09-19 构建，`build/bin/RelWithDebInfo/Template.dll`）

落地位置：`src/ProfilingHooks.h`（hook 定义）+ `src/ProfilingHooks.cpp`（`install()` 注册）。

| # | 靶点 | 状态 | 备注 |
|---|------|------|------|
| 1 | `GetItemCount` | ✅ 已插桩 | 首个落地靶点 |
| 2 | `VisitInventory` | ✅ 已插桩 | 回答 Q1（枚举占比） |
| 3 | `GetInventoryWeight` | ✅ 已插桩 | 回答 Q1（重量刷新占比） |
| 4 | `SendContainerChangedEvent` | ⛔ **有意未插桩** | 见下方「为什么 #4 暂缓」 |
| 5 | `GetValue` | ✅ 已插桩 | 回答 Q2（`n × m` 验证） |
| 6 | `GetArmorInSlot` | ✅ 已插桩 | O(n) 槽位查询 |
| 7 | `GetWornMask` | ✅ 已插桩 | O(n) 装备状态查询 |
| 8 | `GetEnchantment` | ✅ 已插桩 | 内层 `extraLists` 遍历的第二样本 |
| 9/10 | `EquipObject` / `UnequipObject` | ⛔ 未插桩 | 写侧、★ 最低优先级，且不补充 Q1–Q7 所需证据 |

> ✅ **reloc 核实口径**：已插桩靶点的 `(SE, AE)` **全部**对照 `extern/CommonLibSSE-NG/src/RE/I/InventoryChanges.cpp` 与 `InventoryEntryData.cpp` 的实际 `RELOCATION_ID(...)` 取用，**未**照抄 §6.1 的注释表（该表确实存在 `InitScripts`/`GenerateLeveledListChanges` 同值等问题）。

**为什么 #4 暂缓**：`extern/CommonLibSSE-NG/include/RE/Offsets.h:339-341` 三行如下——

```cpp
constexpr auto SendContainerChangedEvent = RELOCATION_ID(15909, 16149);
constexpr auto SetUniqueID             = RELOCATION_ID(15907, 16149);
constexpr auto TransferItemUID         = RELOCATION_ID(15909, 16149);
```

三个**不同**函数的 AE 值同为 `16149`（SE 侧则为 15907/15909/15909，互不相同）——AE 的 `16149` 至少有两个是错的。若照抄 `RELOCATION_ID{15909, 16149}` 在 AE 运行时挂 hook，会改写**错误函数**的入口（静默的错误数据，更坏的情况是崩溃）。故 #4 前置条件是**先反汇编 AE 二进制核对 16149 的真实目标**，核对前不落地。

**#4 缺失对验收的影响**：Q3（批量操作是否触发多次全量刷新）与 Q5（遍历成本 vs 事件风暴 谁主导）缺少直接计数证据。但 Q1/Q2/Q4/Q6 不受影响，且「阶段 2a（哈希索引）是否值得做」的判据来自 Q1/Q2（遍历成本），**不依赖 #4**。因此先采一轮数据即可推进决策，#4 作为并行待办。

#### 为什么不在 zone 里记录「背包规模 n」

想要 `SSE_VALUE(n)` 就必须先数一遍链表 —— **测量行为本身会改变被测对象**。务实做法：

- 依靠 Tracy GUI 的 **zone 统计视图**（调用次数 / 平均 / 最大 / 总耗时）反推规模影响；
- 需要精确 `n` 时，改为在**低频点**（例如 `MenuOpenCloseEvent` 打开背包时）单独统计一次，不要放在每调用必经的热函数里。

### 4.3 「逻辑帧」与时间线定位

`RE::Main`（`extern/CommonLibSSE-NG/include/RE/M/Main.h`）**没有公开的每帧 `Update`**（只有 `QFrameAnimTime` / `WorldRootCamera` / `GetSingleton` 等），因此想要严格渲染帧需要 hook DXGI `Present` —— 项目当前无此基础设施，成本高、收益有限。

**阶段 1 建议不追求渲染帧**，改用「逻辑帧」定位：

- Tracy 的**时间线视图 + zone 统计视图本身不依赖 `FrameMark`**，已能回答「打开背包时哪个函数耗时多少、被调用多少次」；
- 若要在时间线上标出「打开背包」区间，可在 `RE::UI` 的 `MenuOpenCloseEvent` 处理中（`menuName == "InventoryMenu"`）打：
  ```cpp
  SSE_MSG_L("InventoryMenu opened");   // 低频事件：仅菜单开关时打（勿用 spdlog，见 §4.5）
  SSE_FRAME("InventoryMenu");          // 切出一个逻辑"帧"，便于在 GUI 里对齐区间
  ```
  （`SSE_FRAME` → Tracy 的 `FrameMarkNamed`，会在此刻切出一个新"帧"。）

### 4.4 一次完整测量的操作序列

```
1. 构建带插桩的 DLL：
   cmake --preset default -DENABLE_TRACY=ON
   cmake --build --preset relwithdebinfo
   → build/bin/ 下的 DLL 安装/复制到游戏 Data/SKSE/Plugins/
     ⚠️ MO2 用户注意：经 MO2（VFS）加载时，实际目标为 <MO2 base>\mods\<mod>\SKSE\Plugins\；
        本机 = C:\Mod Organizer 2\mods\ModTest\SKSE\Plugins\（游戏目录下的 Data\SKSE\Plugins 可能根本不存在）。
        详见 tracy-capture-log.md §1.6（一次因未部署导致数据不完整的复盘）。

2. 先启动游戏（保持标题界面或已进存档）

3. 运行 extern/TracyProfiler/release/tracy-profiler.exe
   → 客户端会在 ~0.5 s 内被 GUI 发现并连接（TRACY_ON_DEMAND=ON 时为 GUI 主动连接后才采集）

4. 在游戏中打开背包（或执行批量拿取 / 丢弃物品）
   → 观察时间线中出现的 zone

5. 在 GUI 中查看：
   - Statistics（统计）：各 zone 的 count / mean / max / total
   - 时间线：zone 嵌套层次（外层 = 调用者）
   - Memory / Messages：容器变更事件标记

6. 退出游戏 → 采集结束（TRACY_NO_EXIT 保持 OFF，不阻塞 DLL 卸载）
```

### 4.5 测量开销与偏差控制

| 风险 | 说明 | 对策 |
|---|---|---|
| Detours 入口 hook 开销 | 每次调用多 ~2 次跳转（几十 ns） | 对 O(n) 函数（n = 2000 时本身几十~几百 μs）可忽略 |
| 极短 getter 的偏差 | `GetValue`（m 小时可能 < 100 ns）相对开销变大 | 对比「带插桩 / 不带插桩」两次测量，只信数量级 |
| **zone 内附加文本** | ❗ `ZoneText` / `ZoneTextF` / `ZoneName` **每次调用都会 `tracy_malloc` 堆分配**（`TracyScoped.hpp:96` / `:117` / `:135`）；`ZoneTextF` 还额外执行**两次 `vsnprintf`**（`:112` 测长 + `:119` 写入） | 热函数内**连 `ZoneText` 都别用**；只用 `SSE_ZONE("常量名")` + `SSE_VALUE(...)`（`TracyScoped.hpp:180-188`：仅写 8 字节，**无分配、无格式化**） |
| **spdlog 污染** | `src/Plugin.cpp:47` 有 `log->flush_on(spdlog::level::info)` → 每条 info **同步刷盘** | ❗ **绝不用 spdlog 打高频点**，结构性事件（背包开关）才用日志 |
| `TRACY_CALLSTACK` | 每次 zone 采集调用栈，开销可达数量级增长 | 阶段 1 **保持关闭** |
| 采样线程 | Tracy 后台采样线程本身占 CPU | 可用 `TRACY_NO_SAMPLING=ON` 关闭（本阶段只需要 zone） |

> **`ZoneText*` 开销的源码依据**（`extern/TracyProfiler/public/client/TracyScoped.hpp`）：
>
> | 宏 | 实现 | 每次调用开销 |
> |---|---|---|
> | `ZoneText` | `ScopedZone::Text()`（:89-102） | `tracy_malloc(size)` + `memcpy` |
> | `ZoneTextF` | `ScopedZone::TextFmt()`（:104-126） | **两次 `vsnprintf`** + `tracy_malloc` |
> | `ZoneName` / `ZoneNameF` | `ScopedZone::Name()`（:128-141）/ `NameFmt()`（:143-165） | 同 `ZoneText` / `ZoneTextF` |
> | `ZoneValue` | `ScopedZone::Value()`（:180-188） | **无分配、无格式化**（仅写 8 字节队列项） |
>
> 要点：**不是只有 `ZoneTextF` 有开销**——只要 zone 带动态文本，每次调用就有一次堆分配。在 `GetValue`（O(m)、可能 < 100 ns）这类热点里，一次 `tracy_malloc` 就足以让测得耗时失真一个数量级。

> **测量有效性检查**：同一操作重复 3 次，若 zone 耗时第一轮明显高于后续轮次，说明存在 cache 冷启动效应（第一次打开背包 vs 之后），应取**稳定后的轮次**作为结论，并单独记录首轮值（玩家感知到的正是首轮）。

### 4.6 阶段 2（定位 skyui 那 ~1.5 s）插桩靶点

> **背景**：阶段 1 实测已确认——数秒卡顿的根因是 **skyui** 在 `InventoryMenu` 打开后对全部物品做一次确定性遍历（`GetValue` **27211 次/次**，仅 ~11.5 ms），而 ~1.5 s 的额外耗时在其**伴随操作**里。
> **源码查证（2026-09-19）已把「伴随操作」定位到 GFx 委托往返**：`ItemcardDataExtender.processList()`（`ItemMenus/ItemcardDataExtender.as:44-57`）对全部 27211 个条目**各调一次** `GameDelegate.call("RequestItemCardInfo", [], this, "updateItemInfo")`；vanilla 的同名调用只在「高亮 / 打开」时发生（个位数次）。由 1.5 s ÷ 27211 反推 **≈ 55 µs / 次**，量级吻合。详见 [inventory-system-analysis.md](./inventory-system-analysis.md) §7.4。
> 本阶段**只测量、不改行为**，目标是**用实测确认这 1.5 s 的归属层**。完整数据见 [tracy-capture-log.md](./tracy-capture-log.md) §6 / §7。
>
> ✅ **阶段 2 实测已完成（2026-09-19 21:27 / 21:43，见 capture-log 采集 #7 / #8）**。同存档单变量对照（唯一变量 = skyui 是否加载）显示：skyui 的增量是 **≈ +1.1 s**，其中 **`RequestItemCardInfo` 的 C++ 实现 ≈ 0.6 s（≈ 55 %）**，**其余 ≈ 0.5 s（≈ 45 %）在 GFx 边界 / AS 层**（未插桩）。→ **3a′ 的天花板 = 55 %**；根治仍指向**阶段 4**，并建议先补**靶点 2.5** 把剩余 45 % 再拆一层。完整裁定见 **§4.6.2**。
>
> ✅✅ **阶段 2 已最终闭环（2026-09-19 23:08，见 capture-log 采集 #9 / #10）**：**靶点 2.5 已生效并完成判读** —— 同存档单变量对照（存档规模 **6441** 件）显示 skyui 增量 ≈ **+779 ms**，其中 **C++ 实现 ≈ 414 ms（≈ 53 %）**、**GFx 边界（引擎侧分发）≈ 1 ms（< 0.2 %）**、**AS 层 + Scaleform 编组 ≈ 364 ms（≈ 47 %，Tracy 视野外）**。→ **「边界可拿 45 %」的假设被证伪**；**3a′ 的天花板 = 53 %**（且其内部 `GetValue` 仅占 2 %）；**阶段 4 是唯一能拿到那 47 % 的路径**。最终裁定见 **§4.6.4**。

| # | 靶点 | 类型 | 用途 / 判读 |
|---|------|------|------------|
| 2.1 | **`RequestItemCardInfo` 的 C++ 委托实现** —— ✅ **已落地 + 已实测命中（两轮）**：运行时从 `IMenu::fxDelegate` 的委托表解析真实函数指针，**不需要 reloc**（见 §4.6.1、§4.6.2） | hook（只包 zone + 原样转发） | **首要靶点**：量它的**调用次数**与**单次耗时**。**实测（采集 #7）**：每轮 **8466 次 = 物品数 × 1**（严格成立）、**70.5 µs / 次**、3 轮总 **1791 ms**，是引擎侧次大 zone 的 **39 倍**；**实测（采集 #9，6441 件）**：每轮 6441 次、**64.4 µs / 次**、3 轮总 **1243.5 ms**（同量级复核 → 耗时有 ±10 % 的存档相关性） |
| 2.2 | `TESObjectREFR::GetInventory`（Q7 首号嫌疑） | 自有调用点包 zone | CommonLib 自实现（非引擎 reloc），**包 zone 零风险**；验证 skyui 是否触发 `std::map` + 逐 entry 深拷贝 |
| 2.3 | `InventoryEntryData::GetDisplayName` / `GetEnchantment` / `GetWeight` / `IsWorn` | 自有调用点包 zone | 均为 CommonLib 自实现；量 skyui 是否在逐项查属性（与 #5 `GetValue` 共同刻画遍历） |
| 2.4 | `InventoryChanges::SendContainerChangedEvent`（原 #4） | hook（reloc **15909/16149** ⚠️ 需先反汇编核对） | 作为**缓存失效标记**（阶段 3 用）+ 补测 Q3 |
| 2.5 | **GFx 委托边界本身**：`FxDelegate::Callback`（`FxDelegate.h:41` 覆写 `GFxExternalInterface`；所有菜单的公共分发入口，**必须按 `a_methodName` 过滤**）—— ✅ **已落地 + 已实测判读**（改 vtable 槽，无 Detours、无 reloc 对；见 §4.6.3） | 改 vtable 槽（`VTABLE_FxDelegate[0]` 的 slot 1） | **实测（采集 #9）**：`Callback[…]` 与 `RequestItemCardInfo` **严格嵌套**（19323 == 19323，3 轮各 6441），但差值仅 **1.0–1.2 ms/轮（< 0.2 %）** → **「剩余在 GFx 边界」的假设被证伪**；那 **≈ 47 % 几乎全在 AS 层**（含 Scaleform 内部编组，Tracy 视野外）。最终裁定见 **§4.6.4** |
| 2.6 | `BSScript::Internal::VirtualMachine` 的 dispatch（原 2.1 的 Papyrus 设想） | hook（**仅作证伪**） | 实测应为 ~0 次；用于**排除** Papyrus VM（源码已证伪，实测再确认一次即闭环） |

#### 4.6.1 靶点 2.1 的实现：运行时解析 GFx 委托表（无 reloc）

> ✅ **2026-09-19 已实现并编译通过**（`build/lib/RelWithDebInfo/...` → `Template.dll`，0 warning / 0 error，`/W4 /WX`）。

**结论**：`RequestItemCardInfo` 在 Address Library 里**没有**条目——也不需要。它是引擎注册进 `InventoryMenu` 的 **GFx 委托回调**，可按名字在运行时查到真实函数指针：

```
RE::UI::GetSingleton()->GetMenu<RE::InventoryMenu>()       // UI.h:110 模板（按 MENU_NAME 匹配）
 └─ IMenu::fxDelegate        偏移 0x28  GPtr<FxDelegate>              // IMenu.h:113
     └─ FxDelegate::callbacks 偏移 0x18  GHash<GString, CallbackDefn> // FxDelegate.h:50
         └─ CallbackDefn::callback                                    // FxDelegate.h:24
             = FxDelegateHandler::CallbackFn* = void(*)(const FxDelegateArgs&)  // FxDelegateHandler.h:16
```

查表用的是 `GetAlt("RequestItemCardInfo")`，与引擎自己的分发**完全同一条路径**（`extern/CommonLibSSE-NG/src/RE/F/FxDelegate.cpp:19`）——所以命中的就是 `RequestItemCardInfo` 本身，而**不是**一个需要再甄别的通用分发器。

**为什么这条路径优于「找 reloc」**：

| 维度 | 走 reloc | 运行时查表（**采用**） |
|---|---|---|
| 地址库条目 | 需要一个（上游确实没有） | **不需要** |
| 跨 SE / AE / VR | 每版本一组 ID，易失配 | **天然一致**：解析的是当轮真实地址；日志同时输出 `SkyrimSE.exe + RVA` 供人工核对 |
| 命中精度 | 需另证函数身份 | 按名字取，**精确命中** |
| 观测噪声 | — | **零**：不像 hook 通用分发点 `FxDelegate::Callback` 会把所有委托都算进来（那只留作 2.5 的交叉验证） |

**关键签名陷阱**：`CallbackFn` 是**普通函数指针**，不是成员函数指针——引擎侧直接 `cbDef->callback(params)`（`FxDelegate.cpp:23`），**没有 `this`**。菜单实例在参数里（`FxDelegateArgs::GetHandler()`）。因此 hook 必须写成 `void hook(const RE::FxDelegateArgs&)`，**不能**自作主张加 `RE::InventoryMenu*` 首参。

**安装时机（唯一硬约束）**：`fxDelegate` 只在菜单**加载 movie 时**注册 handler，即**首次打开** `InventoryMenu` 之后才可查表。故安装挂在项目已有的 `MenuOpenCloseEvent` sink（`ProfilingHooks.cpp` 的 `MenuOpenCloseListener`）里：每次 `opening` / `closing` 都尝试一次（幂等，命中"已安装"立即返回，几近零成本），**哪一次先查到表就在那一次完成**「解析 → 打日志 → Detours attach」。失败不置位，下一个菜单事件自动重试。

> ⚠️ **已知代价**：首次打开**可能**不被插桩——取决于 `opening` 事件的派发是否早于 skyui 的遍历。保守按「首轮漏采、第 2 轮起必有 zone」规划，与阶段 1 口径一致（取稳定轮次、首轮单独记录，§4.3）。若要无条件覆盖首轮，需额外 hook `BSScaleformManager::LoadMovie`（有 reloc）并在其返回后立即解析；本阶段不做。

**代码落点**（全部为「只测量、不改行为」）：

| 文件 | 改动 |
|---|---|
| `src/Util.h` | 新增 `Hooking::writeDetourAt<T>(uintptr_t)`：`writeDetour<T>()` 的**裸地址**版，返回 `bool`，仅在 Detours 事务提交成功时为 true |
| `src/ProfilingHooks.h` | 新增 `ProfilingHooks::RequestItemCardInfoHook`（`SSE_ZONE("RequestItemCardInfo")` + 原样转发）与 public `installRequestItemCardInfoHook()` |
| `src/ProfilingHooks.cpp` | `MenuOpenCloseListener` 的 `InventoryMenu` 分支（open / close 均尝试，幂等）调用安装；实现「查表 → 日志 → attach」 |

**跑测时的日志自检点**（`…/SKSE/Template.log`）：首次打开背包后应出现两行

```
RequestItemCardInfo resolved: 0x<abs> (SkyrimSE.exe + 0x<RVA>)     ← 必须非 0；RVA 应落在 .text 内
RequestItemCardInfo hook installed at runtime address 0x<abs> ...
```

若出现 `not registered in the InventoryMenu FxDelegate table`，说明委托名被改写或不符——此时退回备选方案（hook 通用分发点 `FxDelegate::Callback` 并按名过滤，噪声大、**仅作交叉验证**，即靶点 2.5）。**这个 `0x<abs>` 同时就是阶段 3a′ 的 hook 目标地址。**

**阶段 2 验收判据（决定阶段 3 分支）**：

| 观察 | 结论 | 下一步 | **✅ 实测裁定（采集 #7–#10）** |
|------|------|--------|--------------------------------|
| `RequestItemCardInfo` 调用数 ≈ 27211，且总耗时占 1.5 s 主体 | 1.5 s 主要是**「全量物品 × GFx 委托往返」** | **阶段 3a′**（降单次成本：hook 该委托，跳过 skyui 不需要的字段 / 复用构建结果）；**根治需阶段 4**（批量通道 + 改 SWF 消费方式） | ⚠️ **半命中**：次数 ✅ 严格 = 物品数 × 1（8466 / 6441 × 3）；但总耗时**只占增量 ≈ 53 %**，**不是「主体」** → 3a′ 天花板 53 % |
| Papyrus 少，但 `GetInventory` / 属性查询 zone 耗时占主体 | 1.5 s 在 **引擎枚举层** | 阶段 **3b**（背包快照 + 脏标记） | ❌ **未出现**：`GetValue`（0.42 µs）等属性查询合计 < 2 % → **3b 依据不成立** |
| 二者都很小，`GetValue` 之外无引擎热点 | 1.5 s 在 **GFx / ActionScript 层** | 阶段 **4**（引擎侧缓存不可达，另议；可对照 `extern/SkyUISrc` 评估改 SWF 可行性） | ✅ **命中**（**这是实测裁决**）：引擎侧确无其他热点（合计 < 1 %）；剩余 ≈ **47 %** 经靶点 2.5 判定落在 **AS 层（含 Scaleform 内部编组）**，**不是**引擎侧 GFx 边界 → **阶段 4** |

> **零成本前置（2026-09-19 已修正并完成）**：~~反编译 skyui 的 `SKI_*.pex`~~ —— **此路已断**（脚本层无遍历逻辑）。**改用读 AS 源码，已直接定位**：`ItemcardDataExtender.processList()`（`ItemcardDataExtender.as:44-57`）的循环里对每个条目调用 `GameDelegate.call("RequestItemCardInfo", [], this, "updateItemInfo")`（`:26`）—— 这是 **GFx（ActionScript ↔ C++）委托往返**，**不经过 Papyrus VM**。vanilla 同名调用只在高亮 / 打开时发生（`extern/SkyUIUnofficialSDK/src/common/ItemMenu.as:150-168`）。完整证据链见 [inventory-system-analysis.md](./inventory-system-analysis.md) §7.4。引擎侧定位手段（如何拿到该委托的 C++ 实现地址）见 §4.6.1。

#### 4.6.2 阶段 2 实测结果（2026-09-19 已闭环）

> ✅ 靶点 2.1 已命中并量化，同存档 vanilla 对照已完成。原始数据见 [tracy-capture-log.md](./tracy-capture-log.md) 采集 **#7**（skyui + 靶点 2.1）与 **#8**（vanilla 对照）。

**同存档单变量对照**（qasmoke 搬空存档；唯一变量 = skyui 是否加载；取稳定轮 = 第 2/3 轮）：

| 指标 | vanilla | skyui | **增量** |
|------|---------|-------|----------|
| wall time | ~1.0 s（959 / 1088 ms） | ~2.1 s（2141 / 2056 ms） | **≈ +1.1 s** |
| `RequestItemCardInfo` | **0 次**（zone 未出现） | 8466 次/轮 @ 70.5 µs | **+0.6 s/轮** |
| `GetValue` | 11884 次/轮 | 39095 次/轮 | +27211 次/轮（仅 **+11 ms**） |

**对 §4.6.1 验收判据的裁定**：

| 判据 | 实测 | 裁定 |
|------|------|------|
| 调用数 = 物品数 × 轮数 | 8466 × 3 = **25398** | ✅ 严格成立（且**首轮即生效**，未漏采） |
| 单次耗时 ≈ 55 µs | **70.5 µs** | ✅ 同量级（略高 → 此前 1.5 s 估计偏保守；27211 条目 ≈ **1.9 s**） |
| 总耗时占增量**主体** | 0.6 / 1.1 = **≈ 55 %** | ⚠️ **只占一半**，不是「主体」 |

**→ 阶段 3 分支裁定**：

1. **3a′（降低 `RequestItemCardInfo` 单次成本）仍值得做，但天花板是 55 %**。注意单次 70 µs 中 `GetValue` 仅占 1.8 µs（2.5 %），真正成本在 item card 构建 + `GFxValue` 组装 + `Respond` 编组——要压低它须改引擎侧的构建逻辑。
2. **其余 ~45 %（≈ 0.5 s）在 GFx 边界 / AS 层，3a′ 触及不到** → 需**靶点 2.5** 把这段再拆开（边界编组 vs AS 层），或直接进入**阶段 4**。
3. **根治方向仍是阶段 4**：把 `value` / `weight` / `damage` / `armor` / `effects` 并入 `skse.ExtendData(true)` 批量通道 + 改 SWF 消费方式（`InventoryDataSetter.processEntry()` 已证实这些字段目前是逐项请回来的，§7.6 E）。

> ⚠️ **口径修正（重要）**：§4.6 / §7.4 / §7.6 H 段此前写作「1.5 s = 全量物品 × GFx 委托往返」。经本次对照，应读作**两段之和**——委托的 **C++ 实现（≈ 55 %）** + **边界编组与 AS 层（≈ 45 %）**；**不能**再把它当作单一靶点（`RequestItemCardInfo` 的 C++ 实现）的耗时。
>
> 🔁 **第二轮实测（2026-09-19 23:08，靶点 2.5 生效）已把上述「45 %」进一步收窄**：新存档（**6441** 件）下 skyui 增量 ≈ **+779 ms**，`RequestItemCardInfo` ≈ **414 ms（53 %）**、`Callback[…] − RequestItemCardInfo` ≈ **1 ms（< 0.2 %）** → **剩余 ≈ 47 % 几乎全在 AS 层（含 Scaleform 内部编组），引擎侧边界可忽略**。判读结果见 §4.6.3 末尾「✅ 实测结果」列，**最终裁定见 §4.6.4**。

#### 4.6.3 靶点 2.5 的实现：改 `FxDelegate` 的 vtable 槽

> ✅ **2026-09-19 已实现并编译通过**（`Template.dll` 731,136 B → **735,232 B**，0 warning / 0 error，`/W4 /WX`）。

**为什么改选「改 vtable 槽」而不是 Detours attach 函数体**：

| 维度 | Detours attach 函数体 | **改 vtable 槽（采用）** |
|------|----------------------|--------------------------|
| 需要的输入 | 必须先拿到一个**实例**才能读出函数地址 | `RE::VTABLE_FxDelegate[0]`（地址库有条目）→ **不需要实例** |
| 库依赖 | 需要 `detours` 端口 | **不需要** → 在 `BUILDOPTIONS.detoursFound == false` 的构建里依然可用（那种构建下 2.1 只能打警告） |
| 调用路径覆盖 | 捕获一切（含直接调用） | `Callback` 是虚函数，Scaleform **只能**经 vtable 调用 → 覆盖等价 |

**槽位为什么是 1（推导，不是猜测）**：

```
FxDelegate                          FxDelegate.h:14
 └─ GFxExternalInterface            GFxExternalInterface.h:10
     └─ GFxState                    GFxState.h:8
         └─ GRefCountBase<…>        GRefCountBase.h:11
             └─ GRefCountImpl       GRefCountImpl.h:7
                 └─ GRefCountImplCore  GRefCountImplCore.h:5
                     virtual ~GRefCountImplCore()      ← vtable[0]（唯一的基类虚函数）
     virtual void Callback(...) = 0                      ← vtable[1]
         ↑ FxDelegate 覆写它                             FxDelegate.h:41
```

`GRefCountImplCore` 只有**一个**虚函数（析构；`AddRef` / `Release` 不是 virtual）→ `Callback` 落在 **slot 1**。槽位来自**类层次**而非某个游戏版本，因此跨 SE / AE / VR 稳定；需要（SE, AE）配对的只是 **vtable 地址**，由 `VTABLE_FxDelegate`（`Offsets_VTABLE.h:6150`）给出。

**校验先行，不是「照猜执行」**。`REL::Relocation::write_vfunc()` 会直接写 vtable（`Relocation.h:395` 用 `address() + 8 * idx` 索引，**不解引用**），所以一旦槽号错位就会去 patch 无辜的内存。故安装前强制两条独立路径必须一致：

| # | 校验 | 失败时 |
|---|------|--------|
| 1 | 活实例的 vtable 指针 **必须等于** `REL::Relocation<std::uintptr_t>{ RE::VTABLE_FxDelegate[0] }.address()` | 打 `error` 日志 + **不 hook**（并停止重试）→ **零崩溃风险**：地址库条目错位时不会 patch 任何东西 |
| 2 | **先读槽内容存入 `orig`，再覆盖槽** | 与已占用该槽的其它 hook **链式**共存，不会把别人踢掉 |
| 3 | 读取/覆盖的**先后顺序**是硬要求，不是风格 | vtable 被**所有菜单共享**：槽一旦被覆盖，任何菜单的委托调用都会进入本 hook 并解引用 `orig`。若先覆盖后赋值，中间存在 `orig == nullptr` 的窗口 → **崩溃** |
| 4 | `orig` 从地址读出（`address() + 8*idx`，与 `write_vfunc` 内部同式）而非依赖 `write_vfunc` 的返回值 | 把「取值」与「写入」解耦，才能满足 #3 的顺序要求 |

**代码落点**：

| 文件 | 改动 |
|------|------|
| `src/ProfilingHooks.h` | 新增 private 常量 `kItemCardMethodName`（2.1 / 2.5 共用的唯一字符串定义，避免两侧漂移）与 private `FxDelegateCallbackHook`（`SSE_ZONE("FxDelegate::Callback[RequestItemCardInfo]")` + `strcmp` 过滤 + 原样转发）；新增 public `installFxDelegateCallbackHook()` |
| `src/ProfilingHooks.cpp` | 在 `MenuOpenCloseListener` 里与 2.1 并排调用安装（同一触发点，因为校验需要活实例）；实现「取实例 → 校验 vtable → `write_vfunc` → 记 `orig`」；2.1 的局部方法名改为引用 `kItemCardMethodName` |

**过滤是必需的，不是优化**：这是**所有菜单、所有委托**的公共入口；不加过滤的 zone 会被无关流量淹没，且嵌套会让时间线不可读。`a_methodName` 是 Scaleform 持有的 NUL 结尾 C 串（不是 `GString`），故用 `strcmp`——无分配，这点很重要，因为它在每次委托调用时执行一次。

**跑测时的日志自检点**（`…/SKSE/Template.log`）：首次打开背包后应出现

```
FxDelegate::Callback hook installed at vtable 0x<abs> slot 1 (was 0x<abs>, SkyrimSE.exe + 0x<RVA>)
```

**判读（与 2.1 嵌套，用于决定阶段 3 / 4 的重心）**：

```
FxDelegate::Callback[RequestItemCardInfo]      ← 边界（含 AS↔C++ 编组）
 └─ RequestItemCardInfo                        ← item card 构建（靶点 2.1）
```

| 观察 | 结论 | 下一步 | **✅ 实测结果（采集 #9，每轮 6441 次）** |
|------|------|--------|------------------------------------------|
| `Callback[…]` 次数 == `RequestItemCardInfo` 次数 | 槽位与过滤都正确（**这同时就是 2.5 自身的正确性自检**） | 继续看耗时 | **命中**：19323 == 19323（3 轮各 6441，**首轮即生效**） |
| `Callback[…]` total ≫ `RequestItemCardInfo` total | 差值 = **GFx 边界编组成本** | 阶段 3/4 应把力气花在**边界**（批量通道收益更大） | ❌ **未兑现**：差值 = 1246.7 − 1243.5 = **3.3 ms / 3 轮** |
| 两者 total 几乎相等 | 边界成本可忽略，剩余 ~45 % 全在 **AS 层** | 引擎侧无从下手 → **阶段 4**（改 SWF 消费方式） | ✅ **成立**：差值仅 **1.0–1.2 ms/轮（< 0.2 %）**，剩余 **≈ 47 %** 判归 AS 层 |

> ⚠️ **本判读表的测量盲区（实测后必须写清）**：`Callback[…]` zone 的起点在**进入引擎 `FxDelegate::Callback` 之后**（= `callbacks.GetAlt()` 查表 + `FxDelegateArgs` 构造 + `cbDef->callback(params)`），而 **AS 参数 → `GFxValue[]` 的真正编组发生在 Scaleform 内部、进入 `Callback` 之前** → **Tracy 测不到**。所以：
> (a) 第 2 行的「差值」严格只代表**引擎侧分发开销**（实测 **0.17 µs/次**），**不是**「边界编组」的全部；
> (b) 第 3 行的「AS 层 ≈ 47 %」实际是「**Scaleform 内部编组 + AS `processList` 循环 + 6441 次 `updateItemInfo` + 列表 UI 更新**」的**合体**，**无法再用 Tracy 细分**（若要细分只能上 SWF 侧插桩，超出本插桩框架的能力范围）。

#### 4.6.4 阶段 2 最终裁定（2026-09-19 23:08）—— 靶点 2.5 判读结果

> ✅ 原始数据见 [tracy-capture-log.md](./tracy-capture-log.md) 采集 **#9**（skyui，靶点 2.5 生效）/ **#10**（同存档 vanilla 对照）。本次存档规模 **6441** 件（比采集 #7 的 8466 更接近常见规模）。

**同存档单变量对照**（取稳定轮 = 第 2/3 轮中位）：

| 指标 | vanilla（#10） | skyui（#9） | 增量 |
|------|----------------|-------------|------|
| wall time | **941 ms**（758.9 / 1122.8） | **1720 ms**（1782.9 / 1657.2） | **≈ +779 ms** |
| `RequestItemCardInfo`（2.1） | 0 次 | 6441 次/轮 @ 64.4 µs → **≈ 414 ms/轮** | **≈ +414 ms（53 %）** |
| `FxDelegate::Callback[…]`（2.5） | 0 次 | 6441 次/轮 @ 64.5 µs → ≈ 415 ms/轮 | 含上项 |
| **边界差（2.5 − 2.1）** | — | **1.0–1.2 ms/轮** | **≈ +1 ms（< 0.2 %）** |
| `GetValue` | 9275 次/轮 | 20945 次/轮 | +5.5 ms/轮（< 1 %） |
| `GetItemCount` / `GetInventoryWeight` | 295 / 284 次 | 289 / 299 次 | 可忽略 |

**最终增量归属**：

| 段 | 每轮 | 占比 | Tracy 可见 |
|----|------|------|-----------|
| `RequestItemCardInfo` C++ 实现 | ≈ 414 ms | **≈ 53 %** | ✅ |
| GFx 边界（引擎侧分发：`GetAlt` + `FxDelegateArgs`） | ≈ 1 ms | **≈ 0.1 %** | ✅ |
| **AS 层 + Scaleform 内部编组** | ≈ 364 ms | **≈ 47 %** | ❌（视野外） |

**三条裁定**：

1. **「边界编组占一半」被证伪**。§4.6.2 基于「45 % 在 GFx 边界 / AS 层」的二分法，在 2.5 实测后应改写为：**几乎全在 AS 层**。引擎侧每次委托的固定开销仅 **0.17 µs**（1.1 ms ÷ 6441），**优化它无意义**。
2. **Tracy 的能力边界要写清**（同 §4.6.3 的盲区说明）：`Callback[…]` 测不到「AS 参数 → `GFxValue[]`」的编组；那 47 % 是 **Scaleform 编组 + AS 层逻辑** 的合体，**要么接受、要么改 SWF**。
3. **分支结论**：

   | 方向 | 天花板 | 判决 |
   |------|--------|------|
   | **3a′**（降 `RequestItemCardInfo` 单次成本） | **53 %** | ⚠️ 可做但有限：单次 64.4 µs 中 `GetValue` 仅 ≈ **2 %**，其余是引擎 item card 构建 + `GFxValue` 组装 + `Respond` 编组 —— **改动深、风险高** |
   | **边界优化**（原以为可拿 45 %） | **≈ 0.1 %** | ❌ **无利可图**（已被 2.5 证伪） |
   | **阶段 4**（`skse.ExtendData(true)` 批量通道 + 改 SWF 消费方式） | **100 %** | ✅ **唯一根治路径**：它同时消掉「逐项 `RequestItemCardInfo` 往返（53 %）」与「AS 层逐项处理（47 %）」—— 因为 AS 层不再需要逐项请求 / 消费 |

> **顺带闭环的正确性自检**：`Callback[…]` 次数**严格等于** `RequestItemCardInfo` 次数（19323 = 19323）→ vtable slot 1 与 `strcmp` 过滤**都正确**；3 轮各 6441 次说明**首轮即时生效**、无丢失；未出现 vtable 校验失败日志 → §4.6.3 的「校验先行 + 先读槽后写槽」策略在真机上成立。
>
> 📄 **阶段 4 的落地方案已单独成文：[phase4-design.md](./phase4-design.md)**。该文给出一个本表未考虑的实现路径 —— **运行时热替换 AS 函数**（`GFxMovie::CreateFunction` + `GetVariable` + `SetVariable`，`extern/CommonLibSSE-NG/include/RE/G/GFxMovie.h:54-62`；入口是 `FxDelegateArgs::GetMovie()` / `IMenu::uiMovie`）。它可以在**不改 SKSE、不编译 SWF** 的前提下同时干预 53 % 与 47 %，从而把上表「阶段 4」的两个重工程依赖（改 SKSE 的 `ExtendData`、重编译 SWF）**都去掉**。
>
> 尤其是：**§4.6.3 所述「Tracy 测不到 AS 参数编组」的盲区，有一条能在 Tracy 视野内绕开它的路** —— 用同一套 `CreateFunction` 机制给 AS 侧的 `processList` / `_requestItemInfo` / `processEntry` 包装 zone（phase4-design.md §4）。这条路是**测量**，不是优化，因此**风险与 §4.6.3 同级**（只读、可回退）。

---

## 5. 阶段 1 的验收判据

> ✅ **2026-09-19 已完成（6 轮采集，详见 [tracy-capture-log.md](./tracy-capture-log.md) §7）**。实测终裁：
>
> | # | 结论 |
> |---|------|
> | 1 | 遍历占「打开背包」耗时 **< 1 %**（vanilla ≈ 0.4 %）→ **不满足「> 50 %」判据**，**阶段 2a（哈希索引）收益 ≈ 0，暂停** |
> | 2 | `GetValue / n ≈ 3`，**严格线性**，无 `n × m` 放大 → §5.2 假设**不成立** |
> | 3 | #4 zone 未落地，未直接观测 |
> | 4 | 卡顿发生在**主线程** |
> | 5 | 遍历与事件风暴**均非主导**；主导是 **skyui 打开后全量遍历（27211 次 `GetValue`/次）伴随的 Papyrus / GFx 操作** |
> | 6 | 0→2000 物品持平（~0.85–1.1 s）；「极大量」跳变由 **skyui** 造成 |
> | 7 | 未直接观测（`GetInventory` 无插桩点），仍是**首号待验证嫌疑** |
>
> **验收标志达成**：已能给出「先做 2a 还是先做 3」的排序依据 → 结论是**两者都不宜先做**，应转向「定位 / 消除 skyui 打开后遍历的伴随开销」（见 capture-log §7.4）。

阶段 1 的目标（见 [inventory-system-analysis.md](./inventory-system-analysis.md) §7.3）是：**用数据确认瓶颈占比，据此确定索引优先级**。插桩跑完后应能回答下表问题：

| # | 待回答问题 | 依据靶点（§4.2） | 判据 / 决策含义 |
|---|-----------|------------------|------------------|
| 1 | 「打开背包」总耗时中，`entryList` 遍历占多少 | #1 / #2 / #3 的 total 时间 ÷ 操作总时长 | > 50% → **阶段 2a（哈希索引）**收益明确 |
| 2 | `n × m` 放大效应是否真实 | #5（`GetValue`）count ÷ #1（`GetItemCount`）count | 比值 ≈ 10~30 → §5.2 的 `n × m` 假设成立 |
| 3 | 批量操作是否触发多次全量刷新 | #4（`SendContainerChangedEvent`）count / 单次操作 | count > 1 → 放大效应存在 → **阶段 3（UI 节流）**优先级提升 |
| 4 | 卡顿发生在哪个线程 | 时间线的线程泳道分布 | 主线程 vs 任务线程 → 决定优化是否需要考虑线程安全 |
| 5 | 瓶颈性质：遍历成本 vs 事件风暴 | #1 total 耗时 **对比** #4 count | 前者主导 → 索引；后者主导 → 节流 |
| 6 | 背包规模 `n` 与耗时关系 | 不同存档（不同 `n`）下同一操作对比 | 明显线性 → `n` 大时收益线性增长；超线性 → 存在额外开销（如 `std::map` 插入） |
| 7 | `std::map` 拷贝开销占比（§5.1 瓶颈 5） | 在自有调用点给 `TESObjectREFR::GetInventory` 包 zone（靶点表最后一行） | 占比可观 → 独立优化点（§7.2 已列为独立收益项） |

**阶段 1 完成标志**：上述 7 个问题中至少 1~5 有明确数据结论，且能给出「先做 2a 还是先做 3」的排序依据。

> 阶段 1 **只测量、不修改行为**（风险极低）：所有 hook 都是「包一层 zone + 原样转发调用」。

> ✅ **阶段 2 的验收判据（§4.6）已实测闭环**（2026-09-19；采集 #7–#10，见 [tracy-capture-log.md](./tracy-capture-log.md) 阶段 2 采集）：源码定位的「全量物品 × `RequestItemCardInfo` GFx 委托往返」被确认**次数严格 = 物品数 × 1**，但它只占 skyui 增量的 **≈ 53 %**（C++ 实现）；剩余 **≈ 47 %** 经**靶点 2.5** 裁定落在 **AS 层（含 Scaleform 内部编组）**，**引擎侧 GFx 边界 < 0.2 %（可忽略）**。→ 裁决：**3b 依据不成立（属性查询 < 2 %）**；**3a′ 天花板 53 %**（可做但有限）；**阶段 4 为唯一根治路径**（批量数据通道 + 改 SWF 消费方式）。完整裁定见 **§4.6.4**；阶段路线总表见 [inventory-system-analysis.md](./inventory-system-analysis.md) §7.3。

---

## 6. 注意事项与风险

### 6.1 与 CommonLibSSE-NG 的关系（重要）

CommonLibSSE-NG 自己的 `.cpp` 只是**转发 wrapper**，例如 `extern/CommonLibSSE-NG/src/RE/I/InventoryChanges.cpp:41-45`：

```cpp
std::int16_t InventoryChanges::GetItemCount(TESBoundObject* a_obj)
{
    using func_t = decltype(&InventoryChanges::GetItemCount);
    REL::Relocation<func_t> func{ RELOCATION_ID(15868, 16047) };
    return func(this, a_obj);
}
```

结论：

- 我们用 Detours hook 的是 **reloc 指向的引擎函数入口**；
- 因此**所有调用者都会被覆盖**：引擎内部调用、CommonLib wrapper 转发、第三方 mod —— 这正是选 `writeDetour` 而非 `writeCall` 的原因（§4.1）；
- 由于 CommonLib 的 wrapper 是内联转发，**不要**去 hook wrapper 本身（它在我们的 DLL 里，只能影响我们自己的调用）。

### 6.2 风险清单

| 风险 | 后果 | 缓解 |
|------|------|------|
| reloc 标注有误（尤其 AE） | hook 到错误地址 → **游戏崩溃** | 先按 §4.2 结尾的 ⚠️ 提示反汇编核对；从**单个**靶点起步，逐个验证 |
| hook 漏转发返回值 | 未定义行为 / 数值错误 | 模板强制 `return orig(...)`（§4.1 注意点 1） |
| Detours 与其它 mod 冲突 | hook 安装失败 / 崩溃 | 检查 SKSE 日志中的 `Failed to install hook`（`src/Util.h:76`）；走 `writeBranch` 兜底时与其它 trampoline 使用者互斥 |
| 依赖目录 `extern/TracyProfiler` 缺失 + `ENABLE_TRACY=ON` | 只打 CMake WARNING，不编译 Tracy | 属预期：`extern/` 被 gitignore，源码经 `src/Profiling.h` 已全部 no-op（§3.3） |
| Tracy 自带 crash handler 与崩溃日志工具冲突 | 崩溃日志丢失 | 设 `TRACY_NO_CRASH_HANDLER=ON` |
| 端口 8086 被占用 | GUI 连不上 | 关闭占用进程；Tracy GUI 可显示客户端端口 |
| 忘记关闭插桩就发版 | 玩家环境受测速开销影响 | 发布构建保持 `ENABLE_TRACY=OFF`（§2.2）；未启用时 Tracy 宏自动 no-op，插件侧无残留调用 |
| **重定义 Tracy 的宏名做兜底** | ① 覆盖不全 → **编译错误**；② 参数名不同 → **C4005**，`/W4 /WX` 下**构建失败** | 用 §3.3 的**自有前缀**宏（`SSE_ZONE` / `SSE_VALUE` 等）**按需转发**；**绝不重定义 Tracy 的宏名** |
| 依赖目录缺失（`extern/` 为空） | 若源码无条件 `#include <tracy/Tracy.hpp>` → **编译失败** | 采用 §2.2 方案 A + §3.3 的 `#ifdef TRACY_ENABLE` 封装 |
| hook 到**手写汇编**函数 / 签名与 ABI 不符 | 难解崩溃、数值错误 | 签名**逐字复制** CommonLib 声明（§4.1 注意点 7）；出现无法解释的崩溃时先反汇编确认该函数是否违反 ABI |
| DLL 卸载时未收尾 | 退出卡顿 | `TRACY_NO_EXIT` 保持 **OFF**（§3.1） |

### 6.3 网络行为说明

Tracy 客户端通过 **localhost:8086** 与 GUI 通信（可选局域网广播发现）。数据**不出本机**。

---

## 7. 参考资料

| 来源 | 路径 | 内容 |
|------|------|------|
| 拆解文档 | `docs/inventory-system-analysis.md` | 瓶颈定位（§5）、reloc 速查（§6）、分阶段路线（§7.3） |
| 现有 hook 基础设施 | `src/Util.h` | `Hooking::writeCall` / `writeDetour` / **`writeBranch`（新增，第 47-60 行）** |
| 现有 hook 声明范例 | `src/Hooks.h` / `src/Hooks.cpp` | `REL::RelocationID{SE, AE}` 写法、`Hooks::install()` |
| 插桩封装头 | `src/Profiling.h`（新增） | `SSE_ZONE` / `SSE_ZONE_FN` / `SSE_VALUE` / `SSE_FRAME` / `SSE_MSG_L`（§3.3） |
| 靶点 hook 与逻辑帧标记 | `src/ProfilingHooks.h` / `src/ProfilingHooks.cpp`（新增） | `GetItemCountHook`、`MenuOpenCloseListener`（§4.1 / §4.3） |
| 安装时机 | `src/GameEventHandler.cpp:14` | `onLoad()` → `Hooks::install()`；`onDataLoaded()` / `onPostLoadGame()` → `installTimelineMarkers()` |
| 日志配置 | `src/Plugin.cpp:40-51` | `flush_on(info)` → 不适合高频打点（§4.5） |
| PCH | `src/PCH.h:152-157` | `<detours/detours.h>`、`Util.h` |
| 构建配置 | `CMakePresets.json` / `cmake/pluginconfig.cmake` | preset `default` / `relwithdebinfo`，C++20，IPO ON |
| Tracy 客户端 | `extern/TracyProfiler/public/tracy/Tracy.hpp` | 插桩宏（§1.3 的 `TRACY_ENABLE` 分支） |
| Tracy CMake | `extern/TracyProfiler/CMakeLists.txt`、`extern/TracyProfiler/cmake/options.cmake` | target 别名 `Tracy::TracyClient`（CMakeLists:104）、选项的 PUBLIC 传播（options.cmake:17） |
| Tracy GUI | `extern/TracyProfiler/release/tracy-profiler.exe` | **预编译，直接运行**（0.14.1） |

---

## 附录 A：落地清单（checklist）

> ✅ = 本次已落地并完成编译验证；⬜ = 需在游戏内/完整工具链上完成。

- [x] `CMakeLists.txt` 追加 §2.2 的 Tracy 集成块（`option(ENABLE_TRACY ...)`，第 45-76 行）
- [x] `src/Profiling.h`：§3.3 的 `#ifdef TRACY_ENABLE` + 自有前缀宏（`SSE_ZONE` / `SSE_ZONE_FN` / `SSE_VALUE` / `SSE_FRAME` / `SSE_MSG_L`）
- [x] 所有插桩点统一用 `SSE_*` 宏（未启用构建下没有 Tracy 头，裸写 `ZoneScopedN` 会编译失败）
- [x] `src/ProfilingHooks.h` / `.cpp`：**按 §4.2 要求只启用靶点 #1**（`GetItemCount`），其余 9 个以模板注释列出待逐一核实 reloc
- [x] `src/Util.h`：新增 `Hooking::writeBranch<T>()`（detours 缺失时的入口 hook 兜底，第 47-60 行）
- [x] `src/Hooks.cpp`：`Hooks::install()` 中追加 `ProfilingHooks::install()`（第 7 行）
- [x] `src/GameEventHandler.cpp`：`onDataLoaded()` / `onPostLoadGame()` 调用 `ProfilingHooks::installTimelineMarkers()`（§4.3 逻辑帧）
- [x] `vcpkg.json`：新增**可选** feature `detours`（默认不启用）
- [x] **严格编译验证**：`cl.exe /std:c++latest /W4 /WX`（`TRACY_ENABLE` 关 / 开 两种）编译 `Hooks.cpp` + `ProfilingHooks.cpp` + `GameEventHandler.cpp` → **0 warning / 0 error**
- [x] **完整 `cmake --build`**：本机已跑通（generator `Visual Studio 18 2026` + toolset `v143`），产出 `build/bin/RelWithDebInfo/Template.dll`；详见附录 B.3 / B.4
- [x] **双向对照构建**：`ENABLE_TRACY=OFF` 重建后插桩与 Tracy 依赖零残留（见 B.4）
- [x] **detours 离线安装验证**：手动播种 vcpkg 下载缓存后，`-DVCPKG_MANIFEST_FEATURES=detours` 全离线安装成功（`-- Found detours`、`DETOURS_LIBRARY 1`、`Template.dll` 695,808 → 714,752 B；详见 B.5）
- [ ] 游戏内验证：打开背包 → 时间线出现 zone → 统计视图有 count / mean / max
- [x] 按 §5 采集 7 项结论 → 决定阶段 2a / 阶段 3 的优先级（✅ **阶段 1 已完成：阶段 2a 暂停**，见 §5 终裁表）
- [x] **阶段 2 定位闭环**：靶点 2.1 + 2.5 已落地并实测（采集 #7–#10）→ **阶段 4 为唯一根治路径**（见 §4.6.4）
- [ ] 关闭插桩（`ENABLE_TRACY=OFF`）后打包发版

---

## 附录 B：落地实现记录

### B.1 改动文件

| 文件 | 状态 | 内容 |
|------|------|------|
| `CMakeLists.txt` | 修改 | §2.2 的 Tracy 集成块：`option(ENABLE_TRACY)` + 预设 `TRACY_ENABLE` / `TRACY_ON_DEMAND` / `TRACY_NO_CRASH_HANDLER` + `add_subdirectory(... EXCLUDE_FROM_ALL)` + link `Tracy::TracyClient` |
| `src/Profiling.h` | **新增** | 薄封装头：`TRACY_ENABLE` 时转发到 Tracy，否则全部退化为 no-op（§3.3） |
| `src/ProfilingHooks.h` | **新增** | `ProfilingHooks` 类；靶点 #1 hook 声明 + 其余 9 个的模板注释 |
| `src/ProfilingHooks.cpp` | **新增** | `install()`（入口 hook）+ `installTimelineMarkers()`（`MenuOpenCloseEvent` sink） |
| `src/Util.h` | 修改 | 新增 `Hooking::writeBranch<T, size = 5>()`（第 47-60 行） |
| `src/Hooks.cpp` | 修改 | `Hooks::install()` 追加 `ProfilingHooks::install()` |
| `src/GameEventHandler.cpp` | 修改 | 新增 include；`onDataLoaded()` / `onPostLoadGame()` 调用 `installTimelineMarkers()` |
| `vcpkg.json` | 修改 | 新增可选 feature `detours`（默认不启用） |
| `CMakePresets.json` | 修改 | `msvc` 预设：generator `Visual Studio 17 2022` → `Visual Studio 18 2026`，并新增 `"toolset": "v143"` 锁定 MSVC 14.44（原因见 B.3） |

### B.2 与原方案的偏差及原因

1. **Detours 改为「可选、默认不启用」**：原方案假定 `vcpkg.json` 已含 detours，实际**从未声明**（`build/cmake/BuildOptions.h` 一直是 `#define DETOURS_LIBRARY 0`，`writeDetour` 是空操作）。改为可选 feature + `if constexpr (BUILDOPTIONS.detoursFound)` 编译期自动兜底，保证**任何环境都能构建**；代价是默认路径不与其它 mod 链式共存。
2. **新增 `Hooking::writeBranch`**：原方案没有兜底路径 —— detours 缺失时阶段 1 会**静默失效**（编译通过但零采集），这比编译失败更危险。
3. **落地了 §4.3 的逻辑帧标记**：在 `onDataLoaded()` / `onPostLoadGame()` 注册 `MenuOpenCloseEvent` sink，`InventoryMenu` 开/关时打 `SSE_MSG_L` + `SSE_FRAME`。否则 §5 的问题 1/3/4（「打开背包总耗时」及事件次数）没有可对齐的时间区间；`installTimelineMarkers()` 内部带一次性标志，`onDataLoaded` 时 UI 尚未就绪也不会重复注册。
4. **插桩代码全部置于严格模式**：`install()` / `installTimelineMarkers()` 整体被 `#ifdef TRACY_ENABLE` 包围 → **发布构建（`ENABLE_TRACY=OFF`）不安装任何 hook、不注册任何 sink，零开销**。

### B.3 构建环境适配：generator 与 toolset（已解决）

本机唯一的 Visual Studio 是 **VS 18（2026）Community**（`C:\Program Files\Microsoft Visual Studio\18\Community`）；`C:\Program Files\Microsoft Visual Studio\2022` 是空目录，所以 `Visual Studio 17 2022` 这个 generator 在本机不可用。

> 修正早先的一个错误判断：曾以为「本机 CMake 4.2.3-msvc3 未提供 VS 18 的 generator」。实际上 `cmake --help` 已列出 `* Visual Studio 18 2026`，用它配置 + 构建**全部成功**。

`CMakePresets.json` 的 `msvc` 预设因此调整：

| 项 | 原值 | 现值 | 原因 |
|----|------|------|------|
| `generator` | `Visual Studio 17 2022` | **`Visual Studio 18 2026`** | 本机唯一可用的 VS 实例 |
| `toolset` | 未设置（默认取最新 = v145 / MSVC 14.50） | **`v143`**（MSVC 14.44.35207） | VS 18 同时装了 `14.16`(v141) / `14.29`(v142) / `14.44`(**v143**) / `14.50`(v145)。SKSE 插件与 CommonLibSSE-NG 按 v143 构建，本项目此前生成的 `.vcxproj` 也正是 `<PlatformToolset>v143</PlatformToolset>`；不锁定就会在 VS 18 上静默换成 v145 |

> JSON 细节：`CMakePresets.json` 的解析器**不支持 `//` 注释**（会报 `CMake Error: … Missing '}' or object member name`），所以上述原因写在 `description` 字段里。

其余两项历史障碍（均不再阻塞构建；其中 detours 一项已在 B.5 彻底解决）：

| 障碍 | 现象 | 说明 |
|------|------|------|
| Ninja 不可用 | `-G Ninja` 在 `CMakeDetermineCompilerABI` 的 `try_compile` 阶段失败：`ninja: error: build.ninja:35: loading 'CMakeFiles\rules.ninja': The system cannot find the file specified.` | 换成无空格路径的 ninja 副本、以及 `Ninja Multi-Config` 均同样失败；与本次改动无关（`ENABLE_TRACY=OFF` 亦如此） |
| detours 下载受阻（**已解决**） | vcpkg 拉取 `github.com/microsoft/Detours/archive/…tar.gz` 时报 `curl operation failed with error code 56` | 终端网络问题；**已用手动播种 vcpkg 下载缓存的方式解决**（见 B.5）。这也是把 detours 设为**默认关闭**的直接原因 |

> detours 仍保持**默认关闭**：它只影响「能否与其它 mod 链式共存」，不影响能否采集数据，且默认开启会给所有构建引入一个 GitHub 依赖。需要时加 `-DVCPKG_MANIFEST_FEATURES=detours` 启用（离线做法见 B.5）；未启用时代码以 `if constexpr (BUILDOPTIONS.detoursFound)` 自动走 `writeBranch` 兜底，SKSE 日志出现 `GetItemCount: detours unavailable, using SKSE trampoline entry hook` 属**预期行为**。

### B.4 完整构建与双向对照验证（本机已完成）

构建命令（与 §4.4 一致）：

```powershell
cmake --preset default -DENABLE_TRACY=ON
cmake --build --preset relwithdebinfo --parallel
```

| 环节 | 实测证据 |
|------|----------|
| 编译器 | `The CXX compiler identification is MSVC 19.44.35226.0`（= v143）；`CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026`；`CMAKE_GENERATOR_TOOLSET:INTERNAL=v143` |
| 依赖 | vcpkg 复用已有安装：`All requested installations completed successfully in: 856 us`（零下载）；`CMAKE_TOOLCHAIN_FILE` 已解析为真实路径 `…/18/Community/VC/vcpkg/scripts/buildsystems/vcpkg.cmake` |
| Tracy 选项 | `TRACY_ENABLE: ON`、`TRACY_ON_DEMAND: ON`、`TRACY_NO_CRASH_HANDLER: ON`、`Tracy Profiler enabled (D:/Documents/SSEMod/extern/TracyProfiler)` |
| 编译 | `GameEventHandler.cpp` / `Hooks.cpp` / `Plugin.cpp` / `ProfilingHooks.cpp` 全部通过（`/W4 /WX` 下 0 error）；完整日志见 `build/build-tracy-full.log`（`build/` 已被 gitignore） |
| 链接产出 | `TracyClient.lib`（4,631,880 B）→ `Template.vcxproj -> build\bin\RelWithDebInfo\Template.dll`（695,808 B） |
| 生成工程 | `<PlatformToolset>v143</PlatformToolset>`、`<WindowsTargetPlatformVersion>10.0.22621.0</WindowsTargetPlatformVersion>` |

**双向对照实验**（同一构建目录，切换 `ENABLE_TRACY` 后重建，其余条件完全相同）：

| 检查项 | `ENABLE_TRACY=ON` | `ENABLE_TRACY=OFF` |
|--------|-------------------|--------------------|
| `Template.dll` 体积 | 695,808 B | 590,848 B（−104,960 B，约 −15%） |
| `findstr "InventoryChanges::GetItemCount"`（zone 名字符串） | 命中 | **未命中**（exit=1） |
| `findstr "TracyProfiler"` | 命中 | **未命中**（exit=1） |
| `dumpbin /dependents` 中的 Tracy 特征依赖 | `WS2_32.dll`、`dbghelp.dll`、`Secur32.dll`、`ADVAPI32.dll` | **四项全部消失** |
| 反例对照 `NoSuchZoneNameXYZ` | 未命中（排除假阳性） | — |

> 结论：
> 1. 插桩确实进了二进制 —— zone 名字符串与 Tracy 运行时（socket / dbghelp 依赖）都能在 ON 版 DLL 中直接找到；
> 2. `TRACY_ENABLE` 关闭时插桩代码与 Tracy 运行时**零残留**（字符串、体积、导入表三个维度同时归零）；`ENABLE_TRACY` 默认 `OFF`，发布构建无需任何额外操作；
> 3. 运行期验证（打开背包看 zone / 时间线）仍待游戏内完成，见 §5。

---

### B.5 Detours 的离线安装（网络受限环境，已在本机验证）

本机终端对 `github.com` 的拉取会在传输中途中断（`curl operation failed with error code 56`：TLS 握手成功、数据接收被重置）。vcpkg 支持**手动播种下载缓存**：把源码包按 vcpkg 期望的**文件名**放进它的下载目录，vcpkg 校验 SHA512 通过后即直接使用，**不再联网**。

**位置与文件名**（由失败时残留的 `.part` 文件反推，并已实测生效）：

| 项 | 值 |
|----|----|
| 下载缓存目录 | `%LOCALAPPDATA%\vcpkg\downloads`（本机 = `C:\Users\<用户名>\AppData\Local\vcpkg\downloads`） |
| 文件名规则 | `<所有者>-<仓库>-<ref>.tar.gz`（同目录既有样例：`gabime-spdlog-v1.17.0.tar.gz`、`fmtlib-fmt-12.2.0.tar.gz`） |
| 需下载的 URL | `https://github.com/microsoft/Detours/archive/9764cebcb1a75940e68fa83d6730ffaf0f669401.tar.gz` |
| 保存为 | `microsoft-Detours-9764cebcb1a75940e68fa83d6730ffaf0f669401.tar.gz`（505,583 B） |
| 期望 SHA512 | `30f689a7f7dd…b3dd9eb7`（与 `ports/detours/portfile.cmake` 一致；基线 `9e593bb` 与当前 master 的 detours 端口完全相同） |

> 自检：`certutil -hashfile <文件> SHA512`。若哈希不符（例如误用了 GitHub 的 "Download ZIP"），vcpkg 会拒绝并回退到联网下载。

**前置条件**：端口元数据（registry）与其余依赖的源码包都已缓存在本机（`%LOCALAPPDATA%\vcpkg\downloads\git` 与 `…\downloads\*.tar.gz`），因此**只需补这一个 tarball**。

**安装与验证**：

```powershell
$env:VCPKG_ROOT = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg'
cmake --preset default -DENABLE_TRACY=ON -DVCPKG_MANIFEST_FEATURES=detours
cmake --build --preset relwithdebinfo --parallel
```

实测结果（全程离线，零下载）：

| 检查项 | 结果 |
|--------|------|
| 配置日志 | `-- Found detours`（不再出现 `-- Could NOT find detours`）；`configure exit=0` |
| `build/cmake/BuildOptions.h` | `#define DETOURS_LIBRARY 1` |
| vcpkg 安装树 | `vcpkg_installed/x64-windows-static-md/lib/detours.lib`、`include/detours/detours.h`；`vcpkg/status` 出现 `Package: detours` |
| 链接 | `build/Template.vcxproj` 中 `detours.lib` 被引用 **4 处** |
| `Template.dll` | 695,808 B → **714,752 B**（+18,944 B） |
| DLL 中 `detours unavailable` 字符串 | **消失**（`if constexpr (BUILDOPTIONS.detoursFound)` 把 fallback 分支编译期消除 → detours 路径生效） |
| Tracy 字符串 | `InventoryChanges::GetItemCount`、`TracyProfiler` 仍命中 |
| 运行时期望 | SKSE 日志**不再**出现 `GetItemCount: detours unavailable…`，而是 `GetItemCount hook installed at address 0x… (id …)` |

> 附带影响：`QuitGameDetoursHook`（`src/Hooks.h:39`）此前因 `DETOURS_LIBRARY` 恒为 0 而一直是**空操作**，本次启用 detours 后**真正生效**。

---

### B.6 关键故障与修复：客户端与 GUI 的 Tracy 版本必须同源（已在本机解决）

#### B.6.1 现象

GUI 中大量 `???`：

| 位置 | 现象 |
|------|------|
| Zone 名 | `InventoryChanges::GetItemCount` 等全部显示为 `???` |
| 线程名 | 实时视图中的线程名显示 `???` |
| 源码位置 | 无法定位到 `src/*.cpp:行号` |

而 zone 的**时间数据看起来正常**（数量、均值都像模像样），因此一开始误判为"只是字符串查询失败"。

#### B.6.2 根因：Tracy 的 wire 协议是 LZ4 流式压缩，要求两端实现逐位一致

客户端把**整条数据流**用 LZ4 的 **linked-block 流模式**压缩后经 socket 发出，GUI 侧对应解压：

| 端 | 文件:行 | 调用 |
|----|---------|------|
| 客户端（压缩） | `public/client/TracyProfiler.cpp:3477` | `LZ4_compress_fast_continue( m_stream, data, … )` |
| GUI（解压） | `server/TracyWorker.cpp:2771` | `LZ4_decompress_safe_continue( m_stream, lz4buf, buf, lz4sz, TargetFrameSize )` |
| GUI（连接时重置字典） | `server/TracyWorker.cpp:2893` | `LZ4_setStreamDecode( m_stream, nullptr, 0 )` |

> `*_continue` 系列是 LZ4 的 **linked-block** 模式：压缩器会把**前一个数据块**当作后续块的字典，解压器必须持有**完全相同**的字典历史。**因此压缩端与解压端必须是同一份 LZ4 源码** —— 这里没有"LZ4 块格式跨版本兼容"那层保护。

本项目此前用 `extern/TracyProfiler` 的 **master（`dd29819f`）** 编译客户端，GUI 却是 **v0.14.1（`30997d5`）** 的发行版。两者在 LZ4 上差了一个大版本：

```diff
  /*   LZ4 - Fast LZ compression algorithm
-  Copyright (C) 2011-2020, Yann Collet.
+  Copyright (C) 2011-2023, Yann Collet.
```

| 文件 | v0.14.1 → master 改动量 |
|------|------------------------|
| `public/common/tracy_lz4.cpp` | 301 行 |
| `public/common/tracy_lz4.hpp` | 180 行 |
| `public/common/tracy_lz4hc.cpp` | 1327 行 |
| `public/common/tracy_lz4hc.hpp` | 85 行 |
| `public/common/tracy_xxhash.h` | 1315 行 |

（对照：`public/common/TracyQueue.hpp` 与 `Profiler::SendData` 本身**未**改动；master 对 `TracyProfiler.cpp` 的改动集中在 external-target / reserved-socket 等本地 profiling 用不到的路径上。）

#### B.6.3 为什么"时间正常、名字 `???`"

`ProtocolVersion` 两端**都是 82**（`public/common/TracyProtocol.hpp`），所以 Tracy 既不拒绝连接、也不报错 —— 损坏是**静默**的：

| 层 | 结果 |
|----|------|
| LZ4 解压 | 字典不一致 → 解出的字节流整体错位（不报错：`LZ4_decompress_safe_continue` 只按块长尽力解码） |
| 消息解析 | Tracy 消息是**变长**的。错位后大多数字段非法，但字节流里**偶然**能凑出合法的 `ZoneBegin`/`ZoneEnd`（header 仅 type+size）→ "zone 数量 / 均值"看起来正常，实为噪声 |
| 字符串 / 源码位置 | 需要精确的 `ptr` / 长度 / 索引，错位后必然查不到 → 一律回退 `???` |

> ⚠️ 这意味着此前所有采集数据都是**不可用的**（包括那个 ~833 MB 的 capture）：不是显示问题，而是**数据本身已损坏**，不能用于任何性能结论。

#### B.6.4 修复：让客户端与 GUI 同源

不必自己编译 GUI —— 直接把客户端源码降到 GUI 的版本即可（`release/tracy-profiler.exe --help` 自报 `Tracy Profiler 0.14.1 / 30997d5`，正是 tag `v0.14.1` 的 commit `30997d5`）：

```powershell
git -C extern/TracyProfiler checkout v0.14.1
```

v0.14.1 的原生 CMake 接口与 §2.2 的集成**完全一致**，项目 `CMakeLists.txt` 无需任何改动：

| 项目依赖的接口 | v0.14.1 中的位置 |
|---------------|-----------------|
| `add_library(Tracy::TracyClient ALIAS TracyClient)` | `CMakeLists.txt:104` |
| `set_option(TRACY_ENABLE …)` / `set_option(TRACY_ON_DEMAND …)` | `CMakeLists.txt:111-112` |
| `set_option(TRACY_NO_CRASH_HANDLER …)` | `CMakeLists.txt:132` |
| 选项 PUBLIC 传播（插件 target 自动获得 `TRACY_ENABLE` 宏） | `cmake/options.cmake:17` |

#### B.6.5 重新构建（本机已完成）

配置 `cmake` **必须在 VS 开发环境（vcvars）下执行**，否则 vcpkg 探测不到编译器：
`error: vcpkg was unable to detect the active compiler's information.` → `No CMAKE_CXX_COMPILER could be found.`

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d D:\Documents\SSEMod
cmake --preset default -DENABLE_TRACY=ON -DVCPKG_MANIFEST_FEATURES=detours
cmake --build --preset relwithdebinfo --parallel
```

| 检查项 | 结果 |
|--------|------|
| Tracy 版本解析 | `Parsing public/common/TracyVersion.hpp file` → `VERSION 0.14.1` |
| 编译器 | `MSVC 19.44.35226.0`（v143，`…/MSVC/14.44.35207/bin/HostX64/x64/cl.exe`） |
| vcpkg | `All requested installations completed successfully in: 1.2 ms`（零下载，ABI 命中） |
| Tracy 选项 | `TRACY_ENABLE=ON`、`TRACY_ON_DEMAND=ON`、`TRACY_NO_CRASH_HANDLER=ON` |
| 配置 / 构建 | `CONFIGURE_EXIT=0` / `BUILD_EXIT=0` |
| 产物 | `build\bin\RelWithDebInfo\Template.dll`（714,240 B，2026-09-19 09:29） |
| DLL 字符串 | `InventoryChanges::GetItemCount`、`Tracy Profiler`、`ProfilingHooks` 均命中 |

> 上述两步已封装为 `build\_run_cfg.bat` 与 `build\_run_build.bat`（vcvars + configure / build），可直接双击或 `cmd /c` 调用；`build/` 已被 gitignore。

#### B.6.6 教训（务必遵守）

1. **同版本号 ≠ 兼容。** Tracy 不会为内部实现变更（如升级 vendored LZ4）bump `ProtocolVersion`，因此"0.14.x 对 0.14.x"同样可能**静默损坏**。**必须保证客户端与 GUI 来自同一 commit。**
2. `extern/TracyProfiler` 应**固定到 tag**，并与 `release/tracy-profiler.exe --help` 自报的 commit 对齐，不要让它跟着 master 漂移。
3. **配置 `cmake` 必须在 vcvars 环境下执行**（VS 开发命令行 / VS IDE 的 CMake 集成），否则 vcpkg 编译器探测失败。
4. **数据可疑时先质疑数据**：`???` 配上"看起来正常的数字"，是"静默损坏"的典型特征，不要当成显示 bug 绕过去。
5. 将来升级 Tracy 时，`extern/TracyProfiler` 与 `release/` 下的 GUI **必须成对升级**；`release/` 未被 git 跟踪，别只换一半。
