# DeskTidy 桌面整理软件 —— 功能与技术方案分析

> 分析对象：`D:\WorkSpace\Workspace-CPP\DeskTidy` 工程源码
> 技术栈：C++ / MFC（动态链接）、Win32 SDK、Unicode 字符集、x64 与 Win32 双平台
> 工具链：Visual Studio 2022（v143）、Windows 10 SDK

---

## 1. 软件概述

DeskTidy（桌面归置）是一款运行在 Windows 桌面上的**桌面文件整理工具**。它在桌面上显示若干个小窗口（Widget），每个小窗口独立展示一个指定目录内的文件（图标 + 文件名），支持拖动、缩放、折叠、置顶/置底、列表/网格视图切换、Ctrl+滚轮缩放、文件拖放收纳等操作。

软件启动后主界面默认隐藏，仅驻留系统托盘；全部配置保存在 **exe 同目录的 `DeskTidy.ini`** 文件中，重启后自动恢复。

### 1.1 核心目标

| 目标 | 说明 |
|---|---|
| 目录展示 | 小窗口内展示指定目录文件，图标 + 名称（过长用 `...` 省略） |
| 层级控制 | 小窗口可置于"所有窗口最底层"或"悬浮置顶" |
| 抗 Win+D | 按 Win+D（显示桌面）时小窗口依然可见 |
| 配置持久化 | 目录、位置、大小、层级、视图、折叠、缩放、配色、透明度全部写入 INI |
| 轻量驻留 | 启动即驻托盘，无主窗口打扰 |

### 1.2 工程结构

```
DeskTidy/
├── DeskTidy.sln                 # 解决方案（Debug/Release × Win32/x64）
└── DeskTidy/
    ├── DeskTidy.h / .cpp        # 应用类：单实例机制、初始化流程
    ├── DeskTidyDlg.h / .cpp     # 主对话框：托盘、配置持久化、小窗口管理、全局快捷键、置底维护
    ├── DeskTidyWidget.h / .cpp  # 桌面小窗口：核心渲染、交互、层级维护、拖放、缩放
    ├── DeskTidy.rc / Resource.h # 资源：主界面对话框、托盘菜单、图标
    ├── targetver.h / pch.h      # 版本宏、预编译头
    └── DeskTidy.vcxproj         # 工程配置（MFC 动态库、Unicode）
```

---

## 2. 功能模块总览

| 模块 | 功能 | 主要文件 |
|---|---|---|
| 单实例管理 | 只允许一个进程运行，重复启动唤醒已有实例 | `DeskTidy.cpp` |
| 系统托盘 | 启动隐藏主界面，托盘图标 + 右键菜单 | `DeskTidyDlg.cpp` |
| 小窗口管理 | 添加/删除/选中小窗口，批量显示/隐藏 | `DeskTidyDlg.cpp` |
| 配置持久化 | INI 读写全部配置，重启恢复 | `DeskTidyDlg.cpp` |
| 桌面小窗口 | 目录文件展示、拖动/缩放/折叠、视图切换、双击打开 | `DeskTidyWidget.cpp` |
| 层级控制 | 最底层 / 悬浮置顶，Win+D 抗隐藏 | `DeskTidyWidget.cpp` |
| 置底维护 | WinEvent 事件驱动 + 防抖定时器保持 z 序 | `DeskTidyDlg.cpp` / `DeskTidyWidget.cpp` |
| 全局快捷键 | 置顶 / 置底 / 缩放放大 / 缩放缩小 | `DeskTidyDlg.cpp` |
| 界面美化 | 现代扁平 UI：自绘卡片、扁平按钮、圆角列表 | `DeskTidyDlg.cpp` |
| 开机自启动 | 注册表 Run 键 | `DeskTidyDlg.cpp` |
| 文件收纳 | 拖放文件进小窗口，拷贝/创建快捷方式 | `DeskTidyWidget.cpp` |
| 缩放显示 | Ctrl+滚轮 / 快捷键缩放图标与文字（50%~300%） | `DeskTidyWidget.cpp` |
| 配色与透明度 | 标题栏/背景独立颜色与透明度 | `DeskTidyWidget.cpp` |

---

## 3. 各功能模块技术方案

### 3.1 单实例管理（`DeskTidy.cpp`）

**技术方案：命名互斥体（Named Mutex）+ 跨进程消息唤醒**

- 用 `CreateMutex(NULL, FALSE, "Local\\DeskTidy_SingleInstance_Mutex")` 判定实例唯一性，互斥体由内核持有，进程崩溃也会自动回收，不会留下"僵尸锁"。
- 命名空间用 `Local\`（当前登录会话内严格互斥，不同用户会话可各跑一份）。
- 检测到已有实例时，通过 `EnumWindows` 枚举对话框窗口类 `#32770`，按**进程可执行文件名 + 无 Owner** 识别另一个 DeskTidy 实例的主窗口，然后 `PostMessage(WM_DESKTIDY_ACTIVATE)` 请它把主界面显示到前台（先 `AllowSetForegroundWindow(ASFW_ANY)` 让渡前台权限）。
- 重试 2 轮（每轮 12 × 100ms 等窗口建出）以消解"对方正在退出"的竞态。
- 命令行带 `--multi` 时允许多开（调试用）。

**技术要点**：`WM_DESKTIDY_ACTIVATE` 使用 `WM_APP` 私有区间常量（而非 `RegisterWindowMessage`），两个进程天然一致；`ExitInstance` 中关闭互斥体句柄。

### 3.2 系统托盘与启动隐藏（`DeskTidyDlg.cpp`）

**技术方案：`Shell_NotifyIcon(NIM_ADD)` + `WM_WINDOWPOSCHANGING` 拦截**

- `OnInitDialog` 中调用 `AddTrayIcon()` 添加托盘图标，回调消息 `WM_TRAYICON`（`WM_APP + 1`）。
- **启动隐藏主界面**：MFC 的 `RunModalLoop` 非虚函数无法重载，因此改为在 `OnWindowPosChanging` 中拦截 DoModal 的首次 `ShowWindow(SW_SHOWNORMAL)`，把 `SWP_SHOWWINDOW` 改写为 `SWP_HIDEWINDOW`，从消息层面阻止窗口出现。
- 托盘右键菜单：显示主界面 / 显示全部小窗口 / 隐藏全部小窗口 / 退出应用；左键双击显示主界面。
- 点窗口右上角 X 仅隐藏（`OnClose` → `ShowWindow(SW_HIDE)`），程序继续驻留托盘。

### 3.3 配置持久化（`DeskTidyDlg.cpp`）

**技术方案：`WritePrivateProfileString` / `GetPrivateProfileString` 读写 INI**

配置文件为 exe 同目录的 `DeskTidy.ini`，布局：

```ini
[Widgets]
Count=2
[Widget1]
Dir=C:\Users\xxx\Desktop
Left=... Top=... Right=... Bottom=...
LayerMode=0        ; 0=最底层 1=悬浮置顶
ViewMode=0         ; 0=列表 1=网格
Collapsed=0
Zoom=100           ; 50~300，步进 10
HeaderColor=...
BgColor=...
HeaderAlpha=255    ; 0~255，与 BgAlpha 独立
BgAlpha=255
[Settings]
TopHotKey=0        ; 高16位修饰键 + 低16位虚拟键码，0=未指定
BottomHotKey=0
ZoomInHotKey=0
ZoomOutHotKey=0
```

- `LoadConfig()`：读取数量与每个 Widget 的节，目录不存在则跳过；从未配置过时默认创建显示桌面目录的小窗口。
- `SaveConfig()`：写回全部小窗口的目录、位置（`GetLastRect()`）、层级、视图、折叠、缩放、配色、透明度及全局快捷键。
- 小窗口拖动/缩放/折叠/切换视图结束后，通过 `WM_WIDGET_CHANGED` 消息通知主对话框立即 `SaveConfig()`，防止异常退出丢失配置。
- 开机自启动状态**不存 INI**，直接读写注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`（注册表是唯一事实来源）。

### 3.4 桌面小窗口（`DeskTidyWidget.cpp`）

**技术方案：`WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED` 顶层弹出窗口 + 自绘标题栏 + `CListCtrl` 子类化**

**窗口样式**：工具窗口（无任务栏按钮）、点击不抢焦点（`WS_EX_NOACTIVATE`）、分层窗口（`WS_EX_LAYERED`，支撑独立透明度渲染）。

**文件列表**：
- 列表控件派生类 `CWidgetListCtrl` 拦截 `WM_MOUSEWHEEL`，Ctrl+滚轮回调宿主 `ZoomByWheel()` 完成缩放。
- 视图模式：列表（`LVS_REPORT`，仅"名称"列并拉伸等宽）与网格（`LVS_ICON` + `LVS_AUTOARRANGE` + `LVS_ALIGNTOP` + `LVS_NOLABELWRAP`）。
- 文件名超长时用 `FormatDisplayName` 截断：基名最多 10 字符 + `...` + 保留扩展名，再按列宽/单元格宽度做二分宽度兜底。
- 图标提取：`.lnk` 优先通过 `IShellLink::GetIconLocation` + `ExtractIconEx` 直接提取，绕开系统图标缓存；普通文件回退 `SHGetFileInfo`。图标统一用 `CopyImage` 缩放为标准尺寸后放入自有 `CImageList`（小图标/大图标索引一一对应）。

**交互**：
- 标题条（24px 高）自绘，右侧 6 个小按钮（刷新/打开目录/切换视图/置顶/置底/折叠），GDI 简笔画图标 + 悬停/按下/激活高亮 + Tooltip 文字提示。
- 拖动/缩放：`HitTestPoint` 自定义命中测试（边缘 4px 内为缩放区、标题条为移动区），鼠标捕获 + `OnSetCursor` 切换光标形状，`OnGetMinMaxInfo` 限制最小尺寸（200×120）。
- 折叠：窗口缩到仅标题栏高度，`m_rcExpanded` 记录展开矩形用于恢复。
- 双击文件用 `ShellExecute` 默认程序打开；右键菜单提供刷新/打开所在目录/切换视图/切换层级/折叠展开/标题栏颜色/背景颜色/标题栏透明度/背景透明度。
- 拖放收纳：`DragAcceptFiles(TRUE)` 注册拖放目标；`.lnk` 直接拷贝一份到绑定目录，其它文件/目录创建快捷方式（`IShellLink` + `IPersistFile`），重名自动追加 ` (2)`、` (3)` 序号。

### 3.5 层级控制与"显示桌面"抗隐藏（`DeskTidyWidget.cpp`，核心难点）

**技术方案：WitchDrawer 式"桌面 Shell Owner"机制**

置底模式**不再挂接为桌面子窗口**（Raised Desktop 下 Progman 以 `WS_EX_NOREDIRECTIONBITMAP` 创建、无 GDI 内容，挂接必然半透明且不可交互），改为：

1. **`SetDesktopOwner()`**：用 `SetWindowLongPtr(GWLP_HWNDPARENT)`（对顶层窗口该槽位即 Owner，同 WitchDrawer 的 `WindowOwnerIndex = -8`）把桌面 Shell 窗口（Progman，或含 `SHELLDLL_DefView` 的 WorkerW）设为小窗口的 Owner。被桌面拥有的顶层窗口：
   - "显示桌面"（Win+D）不会最小化它 —— 从根本上不受 Win+D 影响；
   - owned 窗口永远位于 Owner 之上，桌面被系统抬升时小窗口跟随抬升，天然位于所有普通窗口之下、桌面之上。
2. **`SuspendDesktopOwnerForMouse()`**：`OnMouseActivate` 返回 `MA_NOACTIVATE`，并在鼠标按下瞬间临时解除 Owner（防止 Explorer 把小窗口记为 Progman 的 `last-active-popup`，否则下次 Win+D 会把它激活到前台）；交互结束后经 `WM_WIDGET_RESTORE_OWNER`（PostMessage）与 `OnCaptureChanged` 双路径恢复 Owner（幂等）。
3. **`RepairShellLastActivePopup()`**：周期（约 5 秒节流）临时 `SetForegroundWindow(桌面)` 再恢复原前台，重置 Progman 的 `last-active-popup` 指针。
4. **三层兜底**：`OnSysCommand` 拦截 `SC_MINIMIZE`（Win+M）；`OnSize(SIZE_MINIMIZED)` → `PostMessage(WM_WIDGET_RESTORE)` → `OnRestoreMinimized` 按 `m_rcLast` 归位恢复（覆盖 Win+D 直接 `ShowWindow(SW_MINIMIZE)` 的路径）；`MaintainZOrder` 低频定时器兜底检测隐藏/最小化状态。

置顶模式（`SwitchToTopmost`）先 `ClearDesktopOwner()` 解除桌面 Owner，再 `SetWindowPos(HWND_TOPMOST)`。

**不透明保障 `ForceOpaque()`**：① `DwmSetWindowAttribute(DWMWA_TRANSITIONS_FORCEDISABLED)` 禁用 DWM 过渡动画（掐掉 cloak 淡入淡出中间态）；② 清除 `WS_EX_TRANSPARENT / WS_EX_COMPOSITED`；③ 保留 `WS_EX_LAYERED`（独立透明度依赖，且**不**调用 `SetLayeredWindowAttributes`，避免覆盖逐像素 alpha）。

### 3.6 置底 z 序维护（`DeskTidyDlg.cpp`）

**技术方案：`SetWinEventHook` 事件驱动 + 防抖定时器**

- 注册 3 个系统级钩子（`WINEVENT_OUTOFCONTEXT`，回调在 DoModal 消息循环中执行）：
  - `EVENT_SYSTEM_FOREGROUND`（前台窗口切换）
  - `EVENT_OBJECT_SHOW`（窗口显示/隐藏）
  - `EVENT_OBJECT_REORDER`（z 序重排，"显示桌面"抬升桌面根窗口时触发）
- 回调只做轻量工作：重启防抖定时器（400ms）。定时器到期后 `PushBottomWidgets()` 统一把"最底层且可见"的小窗口 `MaintainZOrder()`。
- 压底频率抑制：两次实际压底间隔 ≥ 1500ms，杜绝系统高频事件造成的持续闪烁。
- `MaintainZOrder()` 内部：桌面 Owner 挂接有效时直接返回（owned 窗口随桌面升降天然正确）；Owner 失效则重挂；挂接失败才走"显示桌面检测 → 提顶/压底"兜底。兜底路径用 `IsDesktopMode()`（全局枚举，按进程排除本应用窗口，避免多 Widget 互相误判抖动），提顶后需连续 3 次检测不到桌面模式才压回。
- 小窗口存活巡检定时器（3 秒）：Explorer 重启重建桌面后，`CheckWidgetsAlive()` 调用 `RecreateWidget()` 按对象保留的配置重建窗口。

### 3.7 全局快捷键（`DeskTidyDlg.cpp`）

**技术方案：`RegisterHotKey` / `UnregisterHotKey`**

- 4 个可选的全局快捷键：置顶 / 置底 / 缩放放大 / 缩放缩小，注册 ID 为 `HOTKEY_ID_*`（1~4）。
- 校验三重规则：勾选了启用必须输入按键；至少含一个修饰键（Ctrl/Alt/Shift）；四个快捷键两两不能相同。
- 存储格式：高 16 位 = 修饰键（`MOD_*`），低 16 位 = 虚拟键码，直接存 INI `[Settings]` 节。
- `CHotKeyCtrl` 的 `HOTKEYF_*` 标志与 `MOD_*` 双向映射（`GetHotKeyFromCtrl` / `SetHotKeyToCtrl`）。
- 注册失败（与系统或其他程序冲突）时给出提示并回滚已注册项；`OnDestroy` 中反注册，避免进程退出后快捷键残留。
- 快捷键回调对所有小窗口批量生效：`ApplyAllWidgetLayer(nMode)` / `ApplyAllWidgetZoom(nStep)`，随后立即保存配置。
- 消息映射用 `ON_MESSAGE(WM_HOTKEY, ...)` 而非 `ON_WM_HOTKEY()` 宏（后者对自定义签名存在 C2440 问题）。

### 3.8 现代扁平界面（`DeskTidyDlg.cpp`）

**技术方案：纯 GDI 自绘（无第三方库）**

- **Design Tokens**：集中定义浅灰底 `#F3F4F6`、白卡片、品牌蓝 `#2D6EB4`（与桌面小窗口标题栏默认色同源）等配色常量。
- **自绘卡片**：`OnEraseBkgnd` 用 `RoundRect` 画两张白色圆角卡片（替代 GROUPBOX），卡片矩形用 `MapDialogRect` 把 DLU 换算成像素，自动跟随 DPI。
- **自绘扁平按钮**：`CFlatBtn`（`CButton` 派生）通过 `WM_FLATBTN_HOVER` 上报悬停状态；`OnDrawItem` 统一绘制主按钮（品牌蓝填充）/次按钮（白底描边），含悬停/按下/禁用/焦点态。
- **自绘列表项**：`LBS_OWNERDRAWFIXED` + `OnDrawItem`，选中项为圆角"药丸"高亮（品牌蓝底白字）；行高由 `OnMeasureItem` 按字体动态计算。
- **控件配色**：`OnCtlColor` 按控件矩形与卡片矩形求交，卡内白色、卡外浅灰，文字统一近黑。
- **透明度滑块**：设置界面用 0~100% 展示，与 0~255 的 alpha 精确互逆换算（0%⇔0、100%⇔255）。

### 3.9 分层渲染与独立透明度（`DeskTidyWidget.cpp`）

**技术方案：`UpdateLayeredWindow` 逐像素 alpha 合成**

小窗口为分层窗口（`WS_EX_LAYERED`），`RenderLayered()` 实现标题栏/背景**各自独立**的透明度：

1. 创建两张 32 位 DIB 位图（临时 + 最终）。
2. 临时位图填充背景色 `m_clrBg`，`DrawContent()` 绘制标题条（垂直微渐变、由标题栏色派生端色）、标题文字、右侧 6 按钮、整体边框。
3. 文件列表是子控件，不会自动进入分层位图，用 `PrintWindow(PW_CLIENTONLY)` 抓取列表内容 `BitBlt` 到临时位图对应位置（`m_bRendering` 标志防止递归）。
4. 逐像素叠加区域 alpha 并预乘（标题栏区域 `m_byHeaderAlpha`，其余 `m_byBgAlpha`）。
5. `UpdateLayeredWindow` + `BLENDFUNCTION{AC_SRC_OVER, 255, AC_SRC_ALPHA}` 呈现。

列表控件自身重绘/滚动后会请求宿主重新合成（`CWidgetListCtrl::OnPaint/OnVScroll/OnHScroll`）。

### 3.10 缩放显示（`DeskTidyWidget.cpp`）

**技术方案：按百分比重建字体与图标列表**

- 缩放范围 50%~300%，每格 ±10%；Ctrl+滚轮（列表子类拦截）与全局快捷键（`ZoomStep`）两条入口。
- 字体重建：以 `OnCreate` 捕获的基础字体 `m_lfBaseFont` 为基准，字高 `MulDiv(lfHeight, zoom, 100)`。
- 图标重建：图标尺寸 `MulDiv(系统标准图标尺寸, zoom, 100)`，销毁旧 `CImageList` 后按新尺寸重新提取全部文件图标。
- 网格视图下按缩放后尺寸重算 `LVM_SETICONSPACING`（图标高度 + 标签高度 + 留白），防止放大后上一行标签覆盖下一行图标。
- 缩放级别持久化到 INI，重启恢复。

---

## 4. 关键技术难点与解决方案总结

| 难点 | 方案 |
|---|---|
| "显示桌面"（Win+D）后小窗口消失 | 桌面 Shell Owner（`GWLP_HWNDPARENT`）→ owned 窗口不被最小化、随桌面升降；三层最小化兜底（拦截 SC_MINIMIZE / SIZE_MINIMIZED 恢复 / 定时器巡检） |
| 挂接桌面子窗口后半透明 | 放弃挂接（Raised Desktop 无重定向表面），改顶层窗口 + 桌面 Owner；`ForceOpaque` 禁用 DWM 过渡动画、清 `WS_EX_TRANSPARENT/COMPOSITED` |
| 置底 z 序被普通窗口覆盖 | `SetWinEventHook` 事件驱动 + 400ms 防抖 + 1500ms 最小间隔压底；`IsCompletelyVisible`/已最底短路避免无谓 `SetWindowPos` 闪烁 |
| 点击小窗口导致 Win+D 把它激活到前台 | `MA_NOACTIVATE` + 鼠标交互期间临时解除 Owner + 周期修复 Progman `last-active-popup` |
| Explorer 重启销毁窗口 | 3 秒存活巡检 + `RecreateWidget` 按对象保留配置重建 |
| 列表控件默认"吞掉"滚轮消息 | `CWidgetListCtrl` 子类拦截 `WM_MOUSEWHEEL`，Ctrl 按下转交宿主缩放 |
| 标题栏/背景独立透明度 | 分层窗口 + `UpdateLayeredWindow` 逐像素 alpha（PrintWindow 抓取子控件） |
| 长文件名破坏对齐 | 字符数 + 宽度双重截断，基名 10 字符 + `...` + 保留扩展名；网格视图强制单行、按单元格宽度省略 |
| .lnk 图标显示空白 | `IShellLink::GetIconLocation` + `ExtractIconEx` 直接提取，绕开系统图标缓存 |
| 多 Widget 互相误判导致压底抖动 | `IsDesktopMode` 按进程排除本应用全部窗口 |
| 模态子对话框点击被"抢前台"吞掉 | `m_bModalChildOpen` 置位期间暂停 `MaintainZOrder` 的 popup 修复 |

---

## 5. 工程构建信息

| 项 | 值 |
|---|---|
| 解决方案 | `DeskTidy.sln` |
| 工程类型 | MFC 应用程序（`UseOfMfc=Dynamic`） |
| 字符集 | Unicode |
| 平台工具集 | v143（VS2022） |
| 目标平台 | Windows 10 SDK |
| 配置 | Debug / Release × Win32 / x64（实际构建使用 x64） |
| 运行依赖 | 输出目录含 `DeskTidy.exe` + `DeskTidy.ini`（自动生成）+ 调试日志 `DeskTidy_zorder.log` |

---

## 6. 运行时产物说明

| 文件 | 说明 |
|---|---|
| `DeskTidy.ini` | 全部配置（小窗口目录/位置/层级/视图/折叠/缩放/配色/透明度 + 全局快捷键），exe 同目录 |
| `DeskTidy_zorder.log` | z 序维护 / DWM cloak 状态 / 透明度设置等诊断日志，超 1MB 自动清空重写 |
