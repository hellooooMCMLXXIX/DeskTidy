// DeskTidyWidget.h: 桌面小窗口类
//
// CDeskTidyWidget 是显示在 Windows 桌面上、展示指定目录文件的小窗口。
// 每个小窗口只显示一个目录，并且拥有自己独立的属性：
//   1. 层级模式：最底层（普通顶层窗口压到 z 序最底，位于所有普通窗口之下，
//                鼠标可正常交互、可拖动/缩放；不再挂接为桌面子窗口）
//                或悬浮置顶（浮于所有窗口之上）；
//   2. 视图模式：列表（报表视图）或网格（大图标视图）；
//   3. 位置与大小：可拖动标题条移动、拖动边缘缩放（鼠标移到边缘时
//      光标变为对应的缩放形状），由主界面保存到 INI。
//   4. 折叠状态：可折叠为仅显示标题栏（窗口缩到标题条高度），
//      展开时恢复折叠前的完整大小，状态由主界面保存到 INI。
//   5. 缩放级别：Ctrl+鼠标滚轮缩放列表图标与文字大小（50%~300%，
//      每格 10%），缩放级别持久化到 INI，重启后恢复。
// 小窗口内的文件项显示"图标 + 文件名"，文件名过长时以 "..." 省略；
// 支持双击打开文件、右键菜单（刷新/打开所在目录/切换视图/折叠展开）。
// 支持从资源管理器拖放文件进来：拖入 .lnk 快捷方式会拷贝一份到绑定目录，
// 拖入其它文件/目录会在绑定目录中创建指向它的快捷方式，完成后自动刷新列表。
// 标题条右侧提供 6 个小按钮：刷新列表 / 打开所在目录 / 切换列表·网格视图 /
// 切换浮动置顶·最底层 / 直接置底 / 折叠展开，按钮带悬停、按下与层级激活高亮反馈。
// WorkBuddy: 最底层模式采用 WitchDrawer 的"桌面 Shell Owner"方案（不再挂接桌面、
//   也不再依赖"最小化后恢复"兜底）：小窗口保持普通顶层窗口（完全不透明、可交互），
//   但以桌面 Shell 窗口（Progman，或其含 SHELLDLL_DefView 的 WorkerW）为 Owner
//   （SetWindowLongPtr(GWLP_HWNDPARENT)，对顶层窗口该槽位即 Owner）。被 Shell
//   桌面拥有的顶层窗口：
//   1) "显示桌面"（Win+D / 任务栏按钮）不会最小化它 —— 从根本上不受 Win+D 影响；
//   2) 位于 Owner 之上、所有普通窗口之下，符合"最底层"语义；
//   3) 不进入系统置顶层（topmost band），不影响其它应用置顶行为。
//   配套处理（同 WitchDrawer）：
//   - 点击时返回 MA_NOACTIVATE 并在鼠标交互期间临时解除 Owner（防止 Explorer
//     把小窗口记为 Progman 的 last-active-popup，否则下次 Win+D 会把它激活到前台）；
//   - 周期性修复 Progman 的 last-active-popup 指针；
//   - 原有"最小化后恢复"链（OnSysCommand/OnSize/WM_WIDGET_RESTORE）保留作兜底。
//   旧的挂接实现保留为 SetBottomLayer_bk()，仅作备份对比，不再被调用。

#pragma once

#include <afxwin.h>
#include <afxcmn.h>
#include <ShlObj.h>      // IShellLink / CLSID_ShellLink（读取 .lnk 快捷方式）
#include <atlbase.h>     // CComPtr（COM 智能指针）

// 托盘回调消息号（由主对话框使用）
#define WM_TRAYICON   (WM_APP + 1)

// 小窗口状态变更消息（由主对话框使用）
// 小窗口拖动/缩放结束时，会把本消息投递给创建它的主对话框，
// 主对话框收到后自动把最新的位置、大小写入 INI，防止异常退出丢失配置
#define WM_WIDGET_CHANGED   (WM_APP + 2)

// WorkBuddy: 系统 Shell 钩子消息（显示桌面 / Win+D 事件）。部分旧 Windows SDK
// 可能未导出这些宏，此处做兜底定义（值固定，跨 Windows 版本不变）。
#ifndef WM_SHELLHOOKMESSAGE
#define WM_SHELLHOOKMESSAGE 0x04AE
#endif
#ifndef HSHELL_ACTIVATESHELLWINDOW
#define HSHELL_ACTIVATESHELLWINDOW 4
#endif
#ifndef HSHELL_WINDOWACTIVATED
#define HSHELL_WINDOWACTIVATED 5
#endif
#ifndef HSHELL_RUDEAPPACTIVATED
#define HSHELL_RUDEAPPACTIVATED 0x8004
#endif

// 小窗口顶部标题条高度（像素）
#define WIDGET_HEADER_HEIGHT   24
// WorkBuddy: 小窗口四角圆角半径（像素）。RenderLayered 的逐像素 alpha 循环
// 按该半径把四角抠成抗锯齿圆弧——分层窗口内容全部来自 ULW 位图，
// 圆角必须做在位图 alpha 上（DWM 圆角属性对 ULW 窗口不可靠）
#define WIDGET_CORNER_RADIUS   10
// WorkBuddy: 展开矩形无效（高度不大于标题条+边缘，即被污染成折叠条尺寸）时
// 用于兜底的默认展开高度——保证"展开"动作永远能把窗口拉回可视大小
#define WIDGET_DEFAULT_EXPAND_HEIGHT  400

// WorkBuddy: 小窗口默认配色。小窗口构造函数、设置界面的"恢复默认"按钮
// 与 INI 默认值三处共用同一份定义，避免默认色散落多处不一致
#define WIDGET_DEFAULT_HEADER_COLOR   RGB(45, 110, 180)   // 默认标题栏颜色：深蓝
#define WIDGET_DEFAULT_BG_COLOR       RGB(238, 238, 238)  // 默认背景色：浅灰

// WorkBuddy: 文件名称（文件列表项文字）颜色的默认值。
//   名称颜色有"自动跟随背景"与"自定义"两种模式（见 SetTextColor 的 bAuto 参数）：
//   默认是自动模式，即沿用改造前"浅底黑字、深底白字"的老行为，保证老配置
//   升级后观感完全不变；本宏只在"自定义"模式按下时作为取色器初值与兜底色
#define WIDGET_DEFAULT_TEXT_COLOR     RGB(31, 41, 55)     // 自定义名称色默认值：近黑

// 标题栏小按钮 ID（标题条右侧，从左向右排列：刷新/打开目录/切换视图/置顶/置底/折叠）
enum WIDGET_BTN_ID
{
    WIDGET_BTN_NONE      = 0,
    WIDGET_BTN_REFRESH   = 1,   // 刷新列表
    WIDGET_BTN_OPENDIR   = 2,   // 打开所在目录
    WIDGET_BTN_VIEW      = 3,   // 切换列表/网格视图
    WIDGET_BTN_TOPMOST   = 4,   // 切换浮动置顶/最底层
    WIDGET_BTN_BOTTOM    = 5,   // 置底
    WIDGET_BTN_COLLAPSE  = 6,   // 折叠/展开窗口（折叠时只显示标题栏）
};

// 标题栏小按钮尺寸与布局（宽/高/间距/右边缘留白）
#define WIDGET_BTN_WIDTH    20
#define WIDGET_BTN_HEIGHT   16
#define WIDGET_BTN_GAP      2
#define WIDGET_BTN_MARGIN   2

// 创建后首次刷新的延时（毫秒）：窗口显示后再重新枚举一次目录，
// 保证刚创建的小窗口内容为最新（CreateWidget 里已立即刷新一次，
// 延时刷新用于覆盖创建瞬间到显示期间目录内容的变动）
#define WIDGET_INIT_REFRESH_TIMER   2     // 一次性"创建后刷新"定时器 ID
#define WIDGET_INIT_REFRESH_DELAY   300   // 创建后延时刷新间隔（毫秒）

// 最小化恢复消息（PostMessage 投递）：
// Win10/11 的 Win+D（显示桌面）/ 任务栏"显示桌面"按钮 / Win+M 都是直接
// 调用 ShowWindow(SW_MINIMIZE) 最小化所有顶层窗口（并不经过 WM_SYSCOMMAND
// 的 SC_MINIMIZE，因此无法靠拦截系统命令阻止）。小窗口收到 SIZE_MINIMIZED
// 后投递本消息，等系统最小化流程完全结束、消息循环恢复后再处理：
// 用最小化前记录的矩形放回原位并重新显示，保证"显示桌面"后依然可见
#define WM_WIDGET_RESTORE           (WM_APP + 10)

// 恢复桌面 Owner 消息（PostMessage 投递，见 OnMouseActivate）：
// WitchDrawer 方案要求"鼠标按下瞬间临时解除桌面 Owner"（防止 Explorer 把
// 小窗口记为 Progman 的 last-active-popup），交互结束后再恢复。用 PostMessage
// 投递本消息，等当前输入批次（按下/移动/抬起）全部处理完再恢复 Owner：
// 与 WitchDrawer 的 Dispatcher.BeginInvoke(Input) 语义一致——既不打断鼠标
// 交互，又能保证 Owner 及时复位。RestoreDesktopOwnerAfterMouse 幂等，与
// OnCaptureChanged 里的立即恢复路径可安全并存
#define WM_WIDGET_RESTORE_OWNER     (WM_APP + 11)

// WorkBuddy: 身份翻转稳定后的延迟重渲染消息（PostMessage 投递）。
// 实证（2026-09-13）：挂接为桌面子窗口时，AttachToDesktop 内同步调用
// RenderLayered 的 ULW 会被 DWM 丢弃（SetParent/WS_CHILD 样式翻转尚未在
// DWM 侧稳定）——表现即"桌面子窗口 + 未折叠启动后小窗口不显示"。而折叠
// 路径因 SetCollapsed→SetWindowPos→WM_SIZE→OnSize→RenderLayered 发生在
// 稍后（身份已稳定）才生效。故所有身份翻转路径在同步渲染之外，再投递本
// 消息，由消息循环派发时（此时翻转早已完成）补一次渲染，双保险
#define WM_WIDGET_CHILD_RENDER      (WM_APP + 12)

// 单实例运行消息（由 DeskTidy.cpp 的启动检查跨进程 PostMessage 投递）：
// 用户重复启动（双击桌面图标 / 开始菜单 / 任务栏）时，后启动的进程不会
// 再起第二份实例，而是找到已在运行实例的主窗口并投递本消息，
// 请它把设置界面显示到前台——把"我要启动"翻译成"唤出已有窗口"。
// 用 WM_APP 私有区间（0x8000~0xBFFF）而非 RegisterWindowMessage：
// 消息 ID 是编译期常量，两个进程天然一致，也便于像其它自定义消息一样统一管理
#define WM_DESKTIDY_ACTIVATE        (WM_APP + 20)

// 低频 z 序维护定时器：周期检查"显示桌面"（Win+D）状态。
// Windows 10/11 的"显示桌面"并不（总是）最小化窗口，而是把桌面根窗口
// （Progman/WorkerW）抬到 z 序顶部盖住所有窗口——这就是置底小窗口
// "Win+D 后不见了"的根本原因。本定时器周期调用 MaintainZOrder()：
// 检测到桌面模式（Progman 位于 z 序顶部）时，把小窗口提到桌面之上，
// 桌面恢复后再压回最底。检查是只读的（不操作窗口），仅在"普通模式
// <-> 桌面模式"状态翻转时执行一次 SetWindowPos，因此不会造成闪烁
#define WIDGET_ZORDER_TIMER         4     // z 序维护定时器 ID
#define WIDGET_ZORDER_INTERVAL      1000  // z 序维护检查间隔（毫秒）

// Ctrl+鼠标滚轮缩放的范围与步进（百分比）：
// 缩放同时作用于列表内文件的图标大小与文字大小
#define WIDGET_ZOOM_MIN      50     // 最小缩放级别：50%
#define WIDGET_ZOOM_MAX      300    // 最大缩放级别：300%
#define WIDGET_ZOOM_STEP     10     // 每格滚轮的缩放步进：±10%

// 前向声明（列表控件派生类持有宿主指针，Ctrl+滚轮时回调缩放）
class CDeskTidyWidget;

// 文件列表控件的派生类：拦截 Ctrl+鼠标滚轮并转发给宿主小窗口。
// 为什么需要派生类：列表控件默认会"消费"滚轮消息（滚动自身内容），
// 即使宿主窗口自己处理 WM_MOUSEWHEEL 也收不到该消息，
// 必须在列表控件层面把 Ctrl+滚轮主动上抛给宿主完成缩放。
class CWidgetListCtrl : public CListCtrl
{
public:
    CWidgetListCtrl() : m_pOwner(NULL), m_bDragPending(FALSE), m_ptDragStart(0, 0) {}

    // 关联宿主小窗口（Ctrl+滚轮时回调其 ZoomByWheel 完成缩放）
    void SetZoomOwner(CDeskTidyWidget* pOwner) { m_pOwner = pOwner; }

protected:
    afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
    // WorkBuddy: 分层窗口下列表控件是"不可见子控件"（父窗口用 UpdateLayeredWindow
    // 呈现逐像素 alpha 位图，子控件不会自动进入该位图），必须由父窗口用 PrintWindow
    // 抓取列表内容合成。因此列表自身发生重绘/滚动时需要通知宿主重新合成：
    //   OnPaint     - 列表自身内容变化（选择、增删、悬停）→ 重绘后请宿主重新合成
    //   OnVScroll / OnHScroll - 滚动条滚动 → 请宿主重新合成
    afx_msg void OnPaint();
    afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pBar);
    afx_msg void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pBar);
    // WorkBuddy: 把列表项"拖出"到资源管理器/桌面（本控件即标准 OLE 拖放源）。
    //   OnLButtonDown - 记录按下点并置"可能起拖"标志；必须先调用基类，
    //                   否则列表的选择/双击行为会被破坏
    //   OnMouseMove   - 左键仍按下且位移超过系统拖拽阈值（SM_CXDRAG/SM_CYDRAG）
    //                   才真正起拖；该阈值不可省，否则单击与"双击的第一下"
    //                   都会被误判为拖拽
    //   OnLButtonUp   - 清"可能起拖"标志
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
    DECLARE_MESSAGE_MAP()

private:
    CDeskTidyWidget* m_pOwner;      // 宿主小窗口（Ctrl+滚轮缩放回调目标）
    BOOL             m_bDragPending;// 左键按下后"可能起拖"标志（松开/起拖后清除）
    CPoint           m_ptDragStart; // 起拖判定的参考点（按下时的客户区坐标）
};

class CDeskTidyWidget : public CWnd
{
    // WorkBuddy: 列表控件派生类需要调用私有的 RenderLayered()/查询渲染状态
    friend class CWidgetListCtrl;

public:
    // 构造/析构
    CDeskTidyWidget();
    virtual ~CDeskTidyWidget();

    // 创建小窗口（先以顶层弹出窗口创建，随后按层级模式设置层级）
    // rect: 初始位置大小；strDir: 要显示的目录；hNotify: 收到 WM_WIDGET_CHANGED
    // 消息的通知窗口（通常是主对话框，拖动/缩放结束时通知它保存配置）
    BOOL CreateWidget(const CRect& rect, const CString& strDir, HWND hNotify = NULL);

    // 重建窗口（窗口被系统销毁后调用，如 Explorer 重启销毁了桌面挂接的
    // 子窗口）：按对象保留的配置（目录/位置/层级/视图/缩放/折叠/可见意图）
    // 重新创建并重新挂接桌面。供主对话框存活巡检（CheckWidgetsAlive）调用
    void RecreateWidget();

    // 设置目录并刷新文件列表
    void SetDirectory(const CString& strDir);

    // 设置层级模式：0 = 最底层（普通顶层窗口，z 序最底），1 = 悬浮置顶
    void SetLayerMode(int nMode);

    // 设置视图模式：0 = 列表（报表视图），1 = 网格（大图标视图）
    void SetViewMode(int nMode);

    // 设置折叠状态：TRUE = 折叠（窗口缩到只剩标题栏，列表隐藏），
    // FALSE = 展开（恢复折叠前的完整大小）；状态变化时自动通知主对话框保存配置
    void SetCollapsed(BOOL bCollapsed);

    // 设置用户可见意图：记录用户是"主动显示"还是"主动隐藏"小窗口。
    // 由主对话框托盘菜单"显示全部小窗口 / 隐藏全部小窗口"调用。
    // 与系统级隐藏（如"显示桌面"特殊处理、最小化）区分开：只有用户
    // 主动隐藏（bVisible = FALSE）时，z 序维护兜底才不恢复窗口
    void SetUserVisible(BOOL bVisible);

    // 设置缩放级别（百分比，50~300，默认 100）。窗口已创建时立即重建
    // 图标与字体并刷新显示；未创建时仅记录，创建后（OnCreate）按此值应用；
    // 变化时自动通知主对话框保存配置（重启后恢复相同缩放）
    void SetZoom(int nZoom);
    // 获取当前缩放级别（百分比）
    int  GetZoom() const { return m_nZoom; }
    // 按滚轮方向缩放列表图标/文字大小：zDelta>0 放大一档、<0 缩小一档。
    // 供列表控件派生类（CWidgetListCtrl）在 Ctrl+滚轮时回调；
    // 返回 TRUE = 已处理（调用方不再把滚轮继续传递给默认处理）
    BOOL ZoomByWheel(short zDelta);
    // 按指定步数缩放列表图标/文字大小：nStep>0 放大、<0 缩小，
    // 每步 WIDGET_ZOOM_STEP（10%）。供全局快捷键等非滚轮场景调用；
    // 到达缩放边界时由 SetZoom 自动钳制并停在边界值
    void ZoomStep(int nStep);

    // 设置/获取标题栏颜色（默认深蓝 RGB(45, 110, 180)）。
    // 颜色可单独设置，绘制标题条与整体边框时统一使用本值；
    // 标题文字与按钮图标颜色按标题栏亮度自动取黑/白以保证可读
    void SetHeaderColor(COLORREF clrHeader);
    COLORREF GetHeaderColor() const { return m_clrHeader; }
    // 设置/获取窗口背景色（默认浅灰 RGB(238, 238, 238)）
    void SetBgColor(COLORREF clrBg);
    COLORREF GetBgColor() const { return m_clrBg; }

    // WorkBuddy: 设置"文件名称颜色"（文件列表项文字 / 网格视图标签文字）。
    //   bAuto   = TRUE  → "自动跟随背景"：按 m_clrBg 亮度自动取黑/白（默认）；
    //   bAuto   = FALSE → 列表项文字固定使用 clrFixed。
    // clrFixed 在两种模式下都会被保存（自动模式下不参与显示），这样用户在
    // "自动 ⇔ 自定义"之间来回切换时不会丢失上次选好的颜色
    void SetTextColor(COLORREF clrFixed, BOOL bAuto);
    // 取回用户保存的自定义名称颜色（供设置界面回显色块）
    COLORREF GetTextColor() const { return m_clrTextFixed; }
    // 名称颜色当前是否处于"自动跟随背景"模式
    BOOL IsTextColorAuto() const { return m_bTextAuto; }

    // WorkBuddy: 设置/获取标题栏透明度（0~255：0=完全透明，255=完全不透明）。
    // 与背景透明度相互独立——窗口为分层窗口，按区域用各自的 alpha 逐像素合成：
    // 标题栏区域用本值，标题栏以下（背景 + 文件列表）用 SetBgAlpha 的值
    void SetHeaderAlpha(BYTE byAlpha);
    BYTE GetHeaderAlpha() const { return m_byHeaderAlpha; }
    // WorkBuddy: 设置/获取窗口背景透明度（含文件列表区域，0~255）
    void SetBgAlpha(BYTE byAlpha);
    BYTE GetBgAlpha() const { return m_byBgAlpha; }
    // WorkBuddy: 是否正在执行分层重绘（供列表控件判断，避免 PrintWindow 抓取列表时递归）
    BOOL IsRenderingLayered() const { return m_bRendering; }

    // WorkBuddy: 标记"本小窗口有一个模态子对话框正在打开"（如透明度选择对话框）。
    // 置位期间暂停 MaintainZOrder()：它会调用 RepairShellLastActivePopup()，
    // 其中 `SetForegroundWindow(桌面)` + 抢回前台 会在对话框打开时反复抢走前台，
    // 导致用户在对话框上的第一次点击被"重新激活"吞掉（点"确定"像没反应）。
    // 淡入淡出/置底维护在对话框关闭后会照常恢复，因此暂停是安全且必要的
    void SetModalChildOpen(BOOL bOpen) { m_bModalChildOpen = bOpen; }
    BOOL IsModalChildOpen() const { return m_bModalChildOpen; }

    // WorkBuddy: 设置/获取"z 序维护"开关（默认开启，保持历史行为）。
    // 关闭后做两件事，二者缺一不可：
    //   1) KillTimer(WIDGET_ZORDER_TIMER)——停掉每秒一次的维护定时器；
    //   2) MaintainZOrder() 入口直接 return——挡掉主对话框由 WinEvent 事件
    //      驱动的调用（否则定时器停了，事件驱动仍会继续维护，开关形同虚设）。
    // 目的：彻底消除每秒日志、约 5 秒一次的抢前台、压底重绘带来的观感干扰，
    // 供不需要自动置底/防遮挡的用户关闭
    void SetZOrderMaintain(BOOL bEnable);
    BOOL GetZOrderMaintain() const { return m_bZOrderMaintain; }

    // WorkBuddy: "置底采用桌面子窗口"开关。开启且处于最底层模式时，把小窗口
    // 挂接为桌面内容窗口（WorkerW/SHELLDLL_DefView 的父窗口，由 FindDesktopWorkerW
    // 解析）的子窗口，天然位于所有普通窗口之下且随桌面一起被系统抬升，无需任何
    // 定时 z 序维护。关闭（默认）则沿用当前顶层窗口 + 桌面 Owner 的置底方案
    void SetBottomViaChild(BOOL bEnable);
    BOOL GetBottomViaChild() const { return m_bBottomViaChild; }

    // 刷新文件列表（重新枚举当前目录）
    void RefreshFiles();

    // 获取当前目录 / 层级模式 / 视图模式 / 折叠状态
    const CString& GetDirectory() const { return m_strDir; }
    int  GetLayerMode() const { return m_nLayerMode; }
    int  GetViewMode() const { return m_nViewMode; }
    BOOL IsCollapsed() const { return m_bCollapsed; }
    // 层级维护（仅最底层模式生效）。
    // 由主对话框在收到 WinEvent 事件（前台窗口变化/窗口显示等，
    // 可能改变 z 序）后调用，实现事件驱动的"保持置底"。
    // 挂接模式下：小窗口已是桌面（WorkerW/Progman）的子窗口，天然位于
    // 所有普通窗口之下，只需验证挂接仍有效、宿主失效时重新挂接
    // （Explorer 重启重建桌面后由这里恢复）；
    // 未挂接的兜底模式下：把窗口压回 z 序最底（内部先检查下方是否已无
    // 普通窗口，避免无意义的重复压底造成闪烁）
    void CheckHostWindow();
    // z 序综合维护（仅最底层模式生效）：由主对话框 WinEvent 事件驱动
    // 与本窗口低频定时器（WIDGET_ZORDER_TIMER）共同调用。
    // 挂接模式下只需确保挂接有效——小窗口随桌面一起被系统抬升/放下，
    // "显示桌面"（Win+D）时依然可见，无需"提顶/压底"维护；
    // 宿主暂不可用的兜底模式沿用"显示桌面"检测 + 提顶/压底逻辑
    void MaintainZOrder();
    // 获取用于持久化的位置与大小（拖动/缩放/折叠后实时更新，
    // 即使窗口被销毁也能从该值恢复配置）。
    // 折叠状态返回"展开尺寸 + 折叠条当前位置"，保证展开大小不会因折叠而丢失
    CRect GetLastRect() const;

protected:
    // ---- 消息处理函数 ----
    afx_msg int  OnCreate(LPCREATESTRUCT lpCreateStruct);          // 创建子控件
    afx_msg void OnDestroy();                                       // 销毁时清理
    afx_msg void OnSysCommand(UINT nID, LPARAM lParam);             // 拦截 SC_MINIMIZE（Win+M 兜底）
    afx_msg LRESULT OnRestoreMinimized(WPARAM wParam, LPARAM lParam); // 最小化后恢复显示（WM_WIDGET_RESTORE）
    afx_msg void OnTimer(UINT_PTR nIDEvent);                        // 重挂检测定时器
    afx_msg void OnSize(UINT nType, int cx, int cy);                // 布局列表控件
    afx_msg void OnPaint();                                         // 验证绘制区并触发分层呈现（RenderLayered）
    //afx_msg BOOL OnEraseBkgnd(CDC* pDC);                            // 背景填充
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);          // 开始拖动/缩放
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);            // 拖动/缩放中
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);            // 结束拖动/缩放
    afx_msg void OnMouseLeave();                                    // 鼠标离开窗口（清除按钮悬停）
    afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message); // 边缘缩放/拖动光标
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);                // 限制最小尺寸
    afx_msg void OnDropFiles(HDROP hDropInfo);                      // 从资源管理器拖放文件进来
    afx_msg void OnListDblClk(NMHDR* pNMHDR, LRESULT* pResult);     // 双击文件打开
    afx_msg void OnListRClick(NMHDR* pNMHDR, LRESULT* pResult);     // 右键弹出菜单
    // WorkBuddy: 列表控件自定义绘制（NM_CUSTOMDRAW）：仅处理报表视图"列头"——
    // 通过 clrTextBk 把列头背景设为 m_clrBg、clrText 按亮度自适应（CHeaderCtrl
    // 无 SetBkColor，列头背景只能在此设置）；列表项文字由 SetTextColor 着色，
    // 本回调对列表项放行默认绘制
    afx_msg void OnListCustomDraw(NMHDR* pNMHDR, LRESULT* pResult);
    // WorkBuddy: 剪贴板内容变化（由 AddClipboardFormatListener 注册后投递
    // WM_CLIPBOARDUPDATE）。别的程序复制了新内容时，本窗口此前"剪切"的置灰态
    // 已失效，需要把它清掉
    afx_msg LRESULT OnClipboardUpdate(WPARAM wParam, LPARAM lParam);
    // WitchDrawer 方案（桌面 Shell Owner）：鼠标按下瞬间系统会发送
    // WM_MOUSEACTIVATE。这里返回 MA_NOACTIVATE（不激活本窗口、不抢焦点），
    // 并在鼠标交互期间临时解除桌面 Owner（见 SuspendDesktopOwnerForMouse）
    afx_msg int  OnMouseActivate(CWnd* pDesktopWnd, UINT nHitTest, UINT message);
    // 鼠标捕获结束（松开按钮/取消捕获等）后恢复桌面 Owner，
    // 确保 Owner 在交互完成后立即恢复（见 RestoreDesktopOwnerAfterMouse）
    afx_msg void OnCaptureChanged(CWnd* pWnd);
    // WM_WIDGET_RESTORE_OWNER：鼠标输入批次处理完后恢复桌面 Owner
    //（见 WM_WIDGET_RESTORE_OWNER 定义与 OnMouseActivate 的说明）
    afx_msg LRESULT OnRestoreOwnerAfterMouse(WPARAM wParam, LPARAM lParam);
    // WorkBuddy: 身份翻转（挂接/脱离桌面宿主）稳定后的延迟重渲染
    //（WM_WIDGET_CHILD_RENDER，PostMessage 投递；宏定义处有完整说明）
    afx_msg LRESULT OnChildRenderStable(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    // ---- 私有辅助函数 ----
    // WorkBuddy: 判定系统是否处于 "Raised Desktop" 模式（Win11 24H2 / 较新 Win10）。
    // 该模式下 Progman 以 WS_EX_NOREDIRECTIONBITMAP 创建、没有 GDI 内容，
    // 普通窗口挂到它下面必然半透明且不可交互，必须放弃挂接改用顶层窗口
    static BOOL IsRaisedDesktop();
    // 查找桌面图标层宿主窗口：包含 SHELLDLL_DefView 的 WorkerW（找不到时
    // 回退 Progman）。最底层模式把小窗口挂接为该宿主的子窗口（置于
    // SHELLDLL_DefView 之上），从而随桌面一起显示/隐藏、且位于所有普通
    // 窗口之下、鼠标可正常交互
    static HWND FindDesktopWorkerW();
    // 将窗口挂接为桌面图标层宿主（WorkerW/Progman）的子窗口，并置于
    // SHELLDLL_DefView 之上。成功返回 TRUE；宿主暂不可用（explorer
    // 重启间隙等）返回 FALSE，由调用方决定兜底策略
    BOOL AttachToDesktop();
    // WorkBuddy: 强制小窗口在 DWM 合成中按"完全不透明"渲染。
    // 挂接为桌面子窗口后 DWM 可能把它与壁纸做 alpha 混合（半透明/发白），
    // 本函数从三条路径消除：①禁用 DWM 过渡动画（掐掉 cloak 淡入淡出中间态）；
    // ②清除 WS_EX_TRANSPARENT / WS_EX_COMPOSITED（避免透出下层、alpha 异常）；
    // ③确保保留 WS_EX_LAYERED（标题栏/背景独立透明度依赖 UpdateLayeredWindow 的
    //   逐像素 alpha；注意不能调用 SetLayeredWindowAttributes，否则会覆盖逐像素 alpha）
    void ForceOpaque();
    // WorkBuddy: 诊断用——把小窗口与宿主的 DWM cloak 状态同时输出到
    // 调试输出与 DeskTidy_zorder.log。cloaked 位：1=APP 2=SHELL 4=INHERITED
    void DumpDwmState(LPCTSTR pszTag);
    // 切换到最底层模式：首选挂接为桌面（WorkerW/Progman）的子窗口
    // （随桌面升降、位于所有普通窗口之下、鼠标可交互）；
    // 宿主暂不可用时退化为普通顶层窗口压到 z 序最底
    // WorkBuddy: 切换到最底层模式（当前实现）。
    // 不再挂接为桌面（WorkerW/Progman）的子窗口，改为普通顶层窗口压到 z 序最底：
    // Raised Desktop 下桌面父窗口没有 GDI 内容，挂接必然半透明，且挂接层无法兼顾
    // 鼠标交互；顶层窗口有完整重定向表面 → 完全不透明、可交互、位于所有普通窗口之下。
    // Win+D（显示桌面）由 OnSysCommand 拦截 SC_MINIMIZE +
    // OnSize(SIZE_MINIMIZED) → WM_WIDGET_RESTORE → OnRestoreMinimized 兜底保持显示
    void SetBottomLayer();
    // WorkBuddy: 旧版"最底层"实现（挂接桌面宿主优先、失败则压底），
    // 仅作备份/对比用，当前逻辑不再调用。请勿删除
    void SetBottomLayer_bk();

    // ---- WitchDrawer 桌面 Owner 机制（置底模式核心）----
    // 解析可作 Owner 的桌面 Shell 窗口：优先 Progman（含 SHELLDLL_DefView
    // 时），否则找含 SHELLDLL_DefView 的 WorkerW（Win10 图标层），再兜底
    // Progman。复用 FindDesktopWorkerW() 的宿主判定逻辑
    static HWND ResolveDesktopOwner();
    // 以桌面 Shell 窗口为 Owner（SetWindowLongPtr(GWLP_HWNDPARENT)，对顶层
    // 窗口该槽位即 Owner，WitchDrawer 的 WindowOwnerIndex 同为 -8）。
    // 被桌面拥有的顶层窗口："显示桌面"（Win+D）不会最小化它；始终位于
    // Owner 之上（随桌面一起升降）但仍在所有普通窗口之下。成功返回 TRUE
    BOOL SetDesktopOwner();
    // 解除桌面 Owner：恢复原始 Owner（或置空），用于切回置顶模式/销毁前
    void ClearDesktopOwner();
    // 鼠标按下瞬间临时解除桌面 Owner（恢复原始 Owner 或置空），防止
    // Explorer 把小窗口记为 Progman 的 last-active-popup（否则下次 Win+D
    // 会把小窗口激活到前台）。返回 TRUE 表示本次按压缩放成功/无需处理
    BOOL SuspendDesktopOwnerForMouse();
    // 鼠标交互结束后恢复桌面 Owner（SetDesktopOwner + 按需压回最底）。
    // 幂等：未被挂起时直接返回。由 WM_WIDGET_RESTORE_OWNER（PostMessage）
    // 与 OnCaptureChanged 共同调用
    void RestoreDesktopOwnerAfterMouse();
    // 修复 Progman（及其 WorkerW 桌面 Owner）的 last-active-popup 指针：
    // 若它指向本进程的小窗口，临时 SetForegroundWindow(Progman) 再恢复原
    // 前台窗口，把 popup 指针重置回 Progman 自身。由 MaintainZOrder 低频
    // 节流调用（约 5 秒一次），保证 Win+D 永远不会激活小窗口
    void RepairShellLastActivePopup();
    // 判断窗口是否完全可见（未被任何其它窗口遮挡）：
    // 在客户区四角（向内缩 3 像素）与中心采样 5 个屏幕点，全部命中
    // 本窗口（含子控件）即视为完全可见。CheckHostWindow 用它做短路：
    // 完全可见时压不压底视觉无差异，跳过压底可避免无谓重绘闪烁
    BOOL IsCompletelyVisible();
    // WorkBuddy: 判断本窗口是否被"桌面窗口"遮挡（详见实现处说明）：
    // 客户区四角+中心采样，任一点最上层窗口的顶层祖先属于桌面类窗口
    //（Progman / WorkerW，含其子窗口 SHELLDLL_DefView）即返回 TRUE。
    // 与 IsCompletelyVisible() 互补——被普通窗口盖住是置底模式的正常语义，
    // 被桌面盖住则一定是异常（小窗口必须始终位于桌面之上）
    BOOL IsCoveredByDesktop();
    // WorkBuddy: 处理系统 Shell 钩子消息（RegisterShellHookWindow 注册）。
    // 确定性捕获"显示桌面"（Win+D）事件，替代脆弱的 z 序轮询判定
    LRESULT OnShellHook(WPARAM nCode, LPARAM lParam);
    // WorkBuddy: 保证"小窗口始终位于桌面之上"（置底模式的核心不变式）。
    // 被桌面盖住 → 放到最上层桌面窗口之上；之前被抬过且遮挡解除 → 压回
    // 最下层桌面窗口之上恢复置底语义。由 MaintainZOrder 每周期调用，
    // 是"Win+D（显示桌面）后小窗口消失"的直接兜底（详见实现处说明）
    void EnsureAboveDesktop();
    // WorkBuddy: 桌面子窗口方案挂载失败后的自动重试（详见实现处说明）。
    // 由 1s 定时器驱动：已挂接且宿主存活则零开销返回；未挂接/宿主已失效
    // 则按约 3 秒节流重试 AttachToDesktop，直至挂载成功
    void TryReattachDesktopChild();
    // 调试日志：格式化文本追加写入 exe 同目录 DeskTidy_zorder.log
    // （带时间戳、限制文件大小）。用于复现与定位"显示桌面（Win+D）
    // 后小窗口消失"问题：记录各维护分支的判定结果与执行动作
    void LogZ(const TCHAR* pszFmt, ...);
    // 判断系统是否处于"显示桌面"模式（仅宿主暂不可用时的兜底路径使用）：
    // 从全局视角枚举所有桌面窗口（Progman/WorkerW），若存在某个桌面窗口
    // 被系统抬到 z 序顶部且其上方没有"其它程序的普通窗口"（本应用自己的
    // 小窗口/主界面与桌面、任务栏等系统窗口均排除），则视为"显示桌面"
    // 模式。该判定不受小窗口自身 z 序影响，且按进程排除本应用全部窗口，
    // 避免多 Widget 互相误判为普通窗口形成"提顶→压底"抖动循环
    BOOL IsDesktopMode();
    // 切换到悬浮置顶模式：脱离桌面宿主（WorkerW/Progman），恢复为
    // 普通顶层窗口并置顶，浮于所有窗口之上
    void SwitchToTopmost();
    // 枚举并添加当前目录下的文件/子目录到列表
    void AddFilesFromDir(const CString& strDir);
    // 获取一个文件的"小图标 + 大图标"并加入自有图像列表，返回列表索引
    // （.lnk 优先读取快捷方式存储的 IconLocation 直接提取，绕开系统图标缓存，
    //  避免缓存失效/解析失败时 .lnk 显示为空白图标）
    int  AddFileIcon(const CString& strPath);
    // 判断是否为 .lnk 快捷方式（按扩展名，不区分大小写）
    static BOOL IsLnkFile(const CString& strPath);
    // 按当前视图模式应用列表控件样式与列头
    void ApplyViewMode();
    // WorkBuddy: 把当前背景色应用到文件列表控件及其列头，使"背景色"设置
    // 在列表区域真正可见（列表默认白底会挡住 OnPaint 填充、且父窗口绘制因
    // WS_CLIPCHILDREN 不覆盖列表区）。列头背景同步为 m_clrBg；列头文字色由
    // OnListCustomDraw 自定义绘制按亮度自适应；列表项文字由 SetTextColor 着色
    void ApplyListColors();
    // 按当前视图刷新每个列表项的显示文本（列表视图按列宽以 "..." 省略）
    void UpdateDisplayTexts();
    // WorkBuddy: 生成文件名显示文本——基名最多 nMaxChars 个字符（列表/网格视图
    // 各用各的上限，见 WIDGET_NAME_MAX_CHARS_LIST / _GRID），扩展名始终保留；
    // 若 nMaxWidth>0 且结果仍超宽，再按宽度从基名侧二次缩短
    // （仍保留扩展名，且仅追加一个 "..."）。nMaxWidth<=0 时只做字符数截断
    CString FormatDisplayName(LPCTSTR pszName, int nMaxWidth, CDC* pDC,
                              int nMaxChars) const;
    // 列表视图下把"名称"列拉伸到与列表客户区等宽，保证各项整行对齐
    void UpdateColumnWidth();
    // 按背景颜色亮度返回自适应文字色（亮底用黑、暗底用白），保证标题/图标可读
    COLORREF GetContrastTextColor(COLORREF clr) const;
    // 自定义命中测试：返回 HT* 区域码（标题条拖动 / 边缘缩放）
    int  HitTestPoint(CPoint ptScreen) const;
    // 计算标题条上某个按钮的矩形（按钮 ID 见 WIDGET_BTN_ID）
    CRect GetButtonRect(int nBtnId) const;
    // 命中测试：返回客户区坐标点所在的标题栏按钮 ID（未命中返回 WIDGET_BTN_NONE）
    int  HitTestButton(const CPoint& ptClient) const;
    // 处理标题栏按钮点击动作（刷新列表/打开所在目录/切换视图/置顶/置底）
    void OnButtonClick(int nBtnId);
    // 绘制标题条上的一个小按钮（状态背景 + 图标）
    void DrawButton(CDC& dc, const CRect& rcBtn, int nBtnId);
    // 用 GDI 简笔画绘制按钮图标（bHighlight：激活态使用高亮颜色）
    void DrawButtonIcon(CDC& dc, const CRect& rcBtn, int nBtnId, BOOL bHighlight);
    // 仅重绘标题条区域（按钮状态变化时避免整窗刷新）
    void InvalidateHeader();
    // WorkBuddy: 分层窗口渲染核心——把标题栏区域与背景区域分别带不同 alpha 合成成
    // 一张 32 位 ARGB 位图（标题栏用 m_byHeaderAlpha、背景+列表区用 m_byBgAlpha），
    // 再用 UpdateLayeredWindow 逐像素 alpha 呈现，实现"标题栏/背景各自独立透明度"。
    // 文件列表是子控件、不会自动进入分层位图，这里用 PrintWindow 抓取其内容后按背景
    // alpha 叠加；窗口未创建或正在渲染（防递归）时直接返回
    void RenderLayered();
    // WorkBuddy: 分层渲染下的"不透明内容"绘制（背景除外）：标题条底色、标题文字、
    // 右侧按钮、整体边框。由 RenderLayered 在临时位图上调用，随后统一叠加区域 alpha
    void DrawContent(CDC& dc, const CRect& rcClient);
    // 创建标题栏按钮的文字提示控件（Tooltip）。
    // 说明：标题栏按钮是自绘的、并非独立子窗口，无法用系统自动提示，
    // 因此这里用"手动跟踪"模式（TTF_TRACK），显示/隐藏/位置/文本全部由代码控制
    void InitTooltip();
    // 取指定标题栏按钮的提示文字（随视图模式/层级模式/折叠状态动态变化）
    CString GetButtonTipText(int nBtnId) const;
    // 在指定按钮正下方显示文字提示（nBtnId 为 WIDGET_BTN_NONE 时隐藏）
    void ShowButtonTip(int nBtnId);
    // 隐藏文字提示（鼠标离开、按下按钮、开始拖拽、窗口销毁时调用）
    void HideButtonTip();
    // 把鼠标位移应用到窗口矩形（拖动或缩放）
    void ApplyDragDelta(const CPoint& ptCursor);
    // 把拖放过来的路径添加到绑定目录（.lnk 拷贝一份 / 其它文件创建快捷方式）
    void AddDroppedFile(const CString& strSrcPath);
    // WorkBuddy: 把拖放过来的路径"移动"到绑定目录（Shift + 拖放时使用）。
    // 与 AddDroppedFile 不同：搬运的是源文件本身，成功后源位置不再保留该文件
    // （等价于资源管理器的剪切 → 粘贴）。经 SHFileOperation(FO_MOVE) 实现，
    // 可正确处理跨卷移动、目录整体搬移与目标重名
    void MoveDroppedFile(const CString& strSrcPath);
    // WorkBuddy: 判断 strPath 是否"直接"位于目录 strDir 之下（strDir 需以 '\' 结尾）。
    // 用于避免"移动到自身所在目录"的无意义操作；更深的子目录路径返回 FALSE
    // （这类路径仍属于需要搬移的对象）
    static BOOL IsPathDirectlyInDir(const CString& strPath, const CString& strDir);

    // ---- WorkBuddy: 文件操作（拖出 / 复制 / 剪切 / 粘贴 / 删除）----
    // 把一组路径作为 OLE 拖放源"拖出去"（供列表控件起拖时调用）。内部构造
    // CF_HDROP 数据对象、暂停 z 序维护、执行 DoDragDrop，并按 OLE 的"优化移动"
    // 约定判定是否需要源自己删除原文件；结束后刷新列表并恢复层级
    void StartDragOut(const CStringArray& arrPaths);
    // 构造 CF_HDROP 要求的全局内存（DROPFILES + 双 null 结尾的路径列表）。
    // 拖出与"复制/剪切"共用；失败返回 NULL
    static HGLOBAL BuildHDropGlobal(const CStringArray& arrPaths);
    // 把源路径"移动"或"复制"进绑定目录：SHFileOperation(FO_MOVE / FO_COPY)，
    // 目标重名由 FOF_RENAMEONCOLLISION 自动追加 " (2)"；bMove 为 TRUE 且源已
    // 直接位于绑定目录内时跳过（"移动到自己所在目录"无意义）
    BOOL TransferIntoDir(const CString& strSrcPath, BOOL bMove);
    // 把一组路径写入系统剪贴板（bCut: TRUE=剪切(MOVE)、FALSE=复制(COPY)）。
    // 关键是附带 CFSTR_PREFERREDDROPEFFECT——它是"复制"与"剪切"在 Windows
    // 层面的唯一区别，不写它资源管理器粘贴时分不清两者
    BOOL PutPathsToClipboard(const CStringArray& arrPaths, BOOL bCut);
    // 把剪贴板中的 CF_HDROP 粘贴进绑定目录（按 CFSTR_PREFERREDDROPEFFECT
    // 判断移动还是复制）；剪贴板中没有文件列表时什么都不做
    void PasteFromClipboard();
    // 删除一组路径（进回收站，可撤销）。bShowErrors = TRUE 时失败会提示用户
    // （用户主动删除→提示；拖出后的"源自删"→静默，MSDN 建议不要弹 UI）
    BOOL DeletePaths(const CStringArray& arrPaths, BOOL bShowErrors);
    // 采集当前选中项的完整路径（无选中返回空数组）
    void CollectSelectedPaths(CStringArray& arrPaths);
    // 刷新列表并重新合成分层位图。分层窗口下列表是"不可见子控件"，内容靠
    // PrintWindow 抓进分层位图——只改列表不重新合成，屏幕上的内容不会变
    void RefreshFilesAndRepaint();
    // 从"已剪切路径"里剔除磁盘上已不存在的项（粘贴/移动/删除后自愈）
    void PruneCutPaths();
    // 清空"已剪切"高亮（剪切内容被替换、文件被处理掉时调用）
    void ClearCutState();
    // 指定路径当前是否处于"已剪切待粘贴"（列表项据此置灰显示）
    BOOL IsCutPath(const CString& strPath) const;

    // 在指定位置创建指向目标路径的快捷方式 (.lnk)
    static BOOL CreateShortcut(const CString& strTargetPath, const CString& strLnkPath);
    // 生成绑定目录中不重名的完整路径（重名时追加 " (2)"、" (3)"…）
    static CString MakeUniquePath(const CString& strDir, const CString& strFileName);
    // 按当前缩放级别重建列表字体：字号 = 基础字高 × 缩放 / 100
    void RebuildFont();
    // 按当前缩放级别重建图标列表：图标尺寸随缩放变化，
    // 销毁旧图像列表后以新尺寸重建，并重新提取所有文件图标
    void RebuildImages();
    // 应用当前缩放级别到列表（重建字体 + 图标 + 刷新省略文本），
    // 由 SetZoom / OnCreate 在缩放值变化或窗口创建时调用
    void ApplyZoom();
    // 计算按当前缩放级别缩放后的图标尺寸（小图标/大图标各一组），
    // 供图像列表创建与图标提取统一使用，保证各处尺寸一致
    void GetZoomIconSizes(int& cxSmall, int& cySmall,
                          int& cxLarge, int& cyLarge) const;
    // WorkBuddy: 按缩放级别重算网格视图的图标间距（LVM_SETICONSPACING）。
    // 放大时图标与标签文字都变大，但控件默认间距仍是系统基准值，导致上一行
    // 的标签文字画进下一行图标区域（图标被覆盖、显示不全）。本函数在图标
    // 尺寸与字体都已更新后调用，把纵向间距设为"图标高度+标签高度+留白"、
    // 横向间距留出标签左右边距，确保每行项目完整不重叠。仅对图标视图生效
    void UpdateIconSpacing();

    // ---- 成员变量 ----
    CWidgetListCtrl m_list;          // 文件列表控件（派生类，拦截 Ctrl+滚轮缩放）
    CToolTipCtrl m_toolTip;         // 标题栏按钮的文字提示控件（手动跟踪模式）
    CImageList   m_smallImages;     // 自有小图标列表（列表视图使用；每个文件
                                    // 的图标复制到其中，索引与小图标列表一一对应）
    CImageList   m_largeImages;     // 自有大图标列表（网格视图使用；索引与
                                    // 小图标列表同步，切换视图时同一索引都有效）
    CString      m_strDir;          // 本小窗口显示的目录
    CStringArray m_arrNames;        // 完整文件名（与列表项一一对应）
    CStringArray m_arrPaths;        // 完整路径（与列表项一一对应，项 lParam = 索引）
    HWND         m_hHost;           // 当前桌面宿主窗口（WorkerW/Progman；
                                    // 未挂接或置顶模式为 NULL）
    int          m_nLayerMode;      // 层级模式：0 = 最底层，1 = 悬浮置顶
    int          m_nViewMode;       // 视图模式：0 = 列表，1 = 网格
    int          m_nDragMode;       // 当前拖拽区域（0 = 无，HT* 值）
    CRect        m_rcDragStart;     // 拖拽开始时的窗口矩形
    CPoint       m_ptDragStart;     // 拖拽开始时的鼠标屏幕坐标
    BOOL         m_bAttached;       // 当前是否已挂接到底层桌面
    HWND         m_hNotify;         // 通知窗口句柄（主对话框，拖动结束通知保存配置）
    CRect        m_rcLast;          // 最近一次窗口矩形（用于持久化，不依赖窗口是否存活）
    int          m_nBtnHover;       // 当前悬停的标题栏按钮 ID（WIDGET_BTN_NONE 表示无）
    int          m_nBtnPressed;     // 当前按下的标题栏按钮 ID（WIDGET_BTN_NONE 表示无）
    BOOL         m_bCollapsed;      // 折叠状态：TRUE = 只显示标题栏，FALSE = 展开
    CRect        m_rcExpanded;      // 展开时的窗口矩形（折叠时记录，供展开恢复）
    BOOL         m_bUserVisible;    // 用户可见意图：TRUE = 用户主动显示，
                                    // FALSE = 用户从托盘主动隐藏。
                                    // 用于区分"系统级隐藏/最小化"（显示桌面
                                    // 等）与"用户主动隐藏"：只有用户主动隐藏时
                                    // 才不恢复窗口，其余情况由 z 序维护兜底恢复
    BOOL         m_bDesktopCovered; // "显示桌面"模式状态记忆：TRUE = 系统正处
                                    // 于桌面模式，小窗口已被提到桌面之上。
                                    // 用于防止"提顶/压底"在检测结果翻转时
                                    // 反复执行 SetWindowPos 造成抖动闪烁
    DWORD        m_dwLastRestore;   // 上次"不可见兜底恢复"的时间戳（GetTickCount），
                                    // 用于节流：至少间隔 2 秒才恢复一次，
                                    // 防止系统持续隐藏时每周期反复 ShowWindow
                                    // 造成闪烁与日志刷屏
    int          m_nNotCoveredCount;// "未检测到桌面盖住"的连续次数：
                                    // 提顶后需连续 3 次（约 3 秒）检测不到桌面
                                    // 盖住小窗口，才确认"显示桌面"已退出并压回
                                    // 最底。防止"提顶 → 检测翻转 → 压底 → 再被
                                    // 盖住 → 再提顶"的抖动
    BOOL         m_bRaisedAboveDesktop; // WorkBuddy: 小窗口是否已被
                                    // EnsureAboveDesktop 抬到"最上层桌面窗口
                                    // 之上"（Win+D 被桌面盖住时置位）。
                                    // 桌面遮挡解除后据此压回最底、恢复置底
                                    // 语义；整个周期只执行一次，不会抖动
    BOOL         m_bShowDesktop;   // WorkBuddy: 是否处于"显示桌面"(Win+D)模式。
                                  // 由 ShellHook 确定性设置（非轮询）。TRUE 期间
                                  // 小窗口保持提顶且 CheckHostWindow 禁止压底
                                  //（HWND_BOTTOM 会压到抬顶的桌面之下→消失）
    int          m_nZoom;           // 缩放级别（百分比，50~300，默认 100）
    CFont        m_font;            // 按缩放级别生成的列表字体（SetFont 到列表控件）
    LOGFONT      m_lfBaseFont;      // 列表基础字体（OnCreate 捕获，缩放以它为基准）
    COLORREF    m_clrHeader;        // 标题栏颜色（默认 RGB(45,110,180)，可单独设置）
    COLORREF    m_clrBg;            // 窗口背景色（默认 RGB(238,238,238)，可单独设置）
    COLORREF    m_clrTextFixed;     // WorkBuddy: 自定义"文件名称颜色"（bAuto 为 TRUE
                                    // 时不参与显示，仅保留用户上次的选择）
    BOOL        m_bTextAuto;        // WorkBuddy: 名称颜色是否"自动跟随背景"（默认 TRUE，
                                    // 与改造前"按背景亮度自动黑白"的行为一致）
    BYTE        m_byHeaderAlpha;    // WorkBuddy: 标题栏透明度（0=全透明，255=不透明，默认 255）
    BYTE        m_byBgAlpha;        // WorkBuddy: 背景透明度（含列表区，0=全透明，255=不透明，默认 255）
    BOOL        m_bRendering;       // WorkBuddy: 分层重绘进行中标志（防止 PrintWindow 抓取列表时递归）
    BOOL        m_bModalChildOpen;  // WorkBuddy: 是否有模态子对话框正在打开
                                    // （置位时暂停 MaintainZOrder，避免抢前台吞掉点击）
    CStringArray m_arrCutPaths;     // WorkBuddy: 已被"剪切"的路径（对应列表项置灰显示，
                                    // 粘贴/移动/删除或剪贴板易主后清除）
    BOOL        m_bInDragOut;       // WorkBuddy: 拖出（DoDragDrop）进行中，用于防重入
    BOOL        m_bZOrderMaintain;  // WorkBuddy: z 序维护总开关（默认 TRUE）。
                                    // FALSE 时停止 WIDGET_ZORDER_TIMER 且
                                    // MaintainZOrder() 立即返回，彻底关闭
                                    // 置底/防遮挡/显示桌面恢复等自动维护
    DWORD       m_dwLastAttachTryTick;  // WorkBuddy: 上次重试挂载的时刻
                                    // （GetTickCount，用于 TryReattachDesktopChild
                                    // 约 3 秒一次的节流，避免 explorer 重启间隙
                                    // 每秒空转 FindWindow 与日志刷屏）
    BOOL        m_bBottomViaChild;  // WorkBuddy: "置底采用桌面子窗口"开关（默认 FALSE）。
                                    // TRUE 且最底层模式时，小窗口作为桌面内容窗口
                                    // 的子窗口，无需定时 z 序维护

    // ---- WitchDrawer 桌面 Owner 机制的状态 ----
    HWND         m_hDesktopOwner;   // 当前桌面 Owner 窗口（Progman 或其 WorkerW）；
                                    // 置底模式挂接成功时非 NULL，置顶模式/未挂接为 NULL
    BOOL         m_bOwnerSuspended; // Owner 是否因鼠标交互被临时解除（见
                                    // SuspendDesktopOwnerForMouse / OnMouseActivate）
    BOOL         m_bOriginalOwnerValid; // 是否已捕获"设置桌面 Owner 前的原始 Owner"
    HWND         m_hOriginalOwner;  // 原始 Owner 句柄（首次挂接前记录，恢复用；
                                    // 可为 NULL——原本无 Owner）
    DWORD        m_dwLastPopupRepair;   // 上次修复 Progman last-active-popup 的
                                        // 时间戳（GetTickCount），用于节流（约 5 秒）
};
