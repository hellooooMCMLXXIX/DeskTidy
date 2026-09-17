// DeskTidyWidget.cpp: 桌面小窗口类的实现
//
// 详细说明见 DeskTidyWidget.h。
// 每个小窗口只显示一个目录；窗口可拖动标题条移动、拖动边缘缩放，
// 标题条右侧提供刷新/打开目录/切换视图/置顶/置底/折叠等小按钮，
// 位置/大小/层级/视图/折叠状态等属性由主对话框持久化到 exe 所在目录的 INI 文件。

#include "pch.h"
#include "framework.h"
#include "DeskTidyWidget.h"
#include "Resource.h"

#include <shellapi.h>   // ShellExecute / SHFileOperation / DragQueryFile
// WorkBuddy: CFSTR_PREFERREDDROPEFFECT、CFSTR_PERFORMEDDROPEFFECT 定义在
// ShlObj_core.h（由 shlobj.h 引入）。前者是"复制"与"剪切"在 Windows 层面的
// 唯一区别，后者是判定"拖出后是否需由源删文件"的可靠依据——必须显式包含，
// 不能依赖 afxole.h 的间接引入（不同 SDK 组合下不保证）
#include <shlobj.h>
#include <commctrl.h>   // 列表控件相关
#include <dwmapi.h>     // DwmSetWindowAttribute / DwmGetWindowAttribute（过渡动画、cloak 状态）
#include <afxdlgs.h>    // CColorDialog：标题栏/背景颜色选取对话框（WorkBuddy 新增）
// WorkBuddy: OLE 拖放/剪贴板（COleDataSource / COleDropSource / DoDragDrop）。
// 拖出与"复制·剪切"都以 CF_HDROP 形式承载文件列表，统一走 OLE 数据对象
#include <afxole.h>
#pragma comment(lib, "dwmapi.lib")

// WorkBuddy: 部分 SDK 组合下 WS_EX_COMPOSITED 未定义，这里兜底补全，
// 保证 ForceOpaque() 中的样式清除逻辑可编译
#ifndef WS_EX_COMPOSITED
#define WS_EX_COMPOSITED   0x02000000L
#endif

// WorkBuddy: 窗口不渲染到重定向表面（Raised Desktop 判定用，见 IsRaisedDesktop）
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP   0x00200000L
#endif

// 调试模式下的内存泄漏检测辅助宏（MFC 标准写法）
#ifdef _DEBUG
#define new DEBUG_NEW
#endif

// 窗口拖拽/缩放检测的边缘宽度（像素）
#define WIDGET_EDGE_SIZE    4
// 窗口最小尺寸（像素）
#define WIDGET_MIN_WIDTH    200
#define WIDGET_MIN_HEIGHT   120
// 列表视图中，文件名右侧为省略号预留的最小文本宽度（像素）
#define WIDGET_TRIM_MIN     10
// WorkBuddy: 文件名（基名部分）显示的最大字符数，按视图分别设定。
// 两者差异的理由：列表（报表）视图一行只显示一个名字，列宽可以很宽，
// 上限放宽到 100 —— 此时实际显示的约束几乎只剩列宽本身（由 FormatDisplayName
// 的宽度兜底自动加 "..."），用户看到的是"列够宽就显示全名"；
// 网格（大图标）视图的标签挤在图标单元格里，多个格子并排，
// 上限过长会破坏整齐的网格对齐，因此沿用原来较短的 10。
// 扩展名（含点）始终不计入该上限，超长时只截基名。想调整直接改这两个值
#define WIDGET_NAME_MAX_CHARS_LIST   100     // 列表视图：基名最多 100 字符
#define WIDGET_NAME_MAX_CHARS_GRID   10      // 网格视图：基名最多 10 字符

BEGIN_MESSAGE_MAP(CDeskTidyWidget, CWnd)
    ON_WM_CREATE()
    ON_WM_DESTROY()
    ON_WM_SYSCOMMAND()
    ON_MESSAGE(WM_WIDGET_RESTORE, &CDeskTidyWidget::OnRestoreMinimized)
    ON_WM_TIMER()
    ON_WM_SIZE()
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_LBUTTONDOWN()
    ON_WM_MOUSEMOVE()
    ON_WM_LBUTTONUP()
    ON_WM_MOUSELEAVE()
    ON_WM_MOUSEACTIVATE()
    ON_WM_CAPTURECHANGED()
    ON_MESSAGE(WM_WIDGET_RESTORE_OWNER, &CDeskTidyWidget::OnRestoreOwnerAfterMouse)
    ON_MESSAGE(WM_WIDGET_CHILD_RENDER, &CDeskTidyWidget::OnChildRenderStable)
    ON_WM_SETCURSOR()
    ON_WM_GETMINMAXINFO()
    ON_WM_DROPFILES()
    ON_NOTIFY(NM_DBLCLK, IDC_WIDGET_LIST, &CDeskTidyWidget::OnListDblClk)
    ON_NOTIFY(NM_RCLICK, IDC_WIDGET_LIST, &CDeskTidyWidget::OnListRClick)
    ON_NOTIFY(NM_CUSTOMDRAW, IDC_WIDGET_LIST, &CDeskTidyWidget::OnListCustomDraw)
    // WorkBuddy: 剪贴板内容变化（AddClipboardFormatListener）→ 清掉失效的"剪切"置灰态。
    // 用 ON_MESSAGE 而非 MFC 的 ON_WM_CLIPBOARDUPDATE：后者在各版本 MFC 中
    // 处理函数签名不一致，统一按 LRESULT(WPARAM, LPARAM) 处理最稳
    ON_MESSAGE(WM_CLIPBOARDUPDATE, &CDeskTidyWidget::OnClipboardUpdate)
    // WorkBuddy: Shell 钩子（显示桌面/Win+D 事件），确定性捕获，不依赖 z 序开关
    ON_MESSAGE(WM_SHELLHOOKMESSAGE, &CDeskTidyWidget::OnShellHook)
END_MESSAGE_MAP()

// 列表控件派生类的消息映射：拦截 WM_MOUSEWHEEL。
// 列表控件默认会"消费"滚轮消息（滚动自身内容），宿主窗口收不到，
// 必须在列表控件层面判断 Ctrl 是否按下：按下则转交给宿主完成缩放。
BEGIN_MESSAGE_MAP(CWidgetListCtrl, CListCtrl)
    ON_WM_MOUSEWHEEL()
    ON_WM_PAINT()       // WorkBuddy: 列表自身重绘后通知宿主重新合成分层位图
    ON_WM_VSCROLL()     // WorkBuddy: 垂直滚动后通知宿主重新合成
    ON_WM_HSCROLL()     // WorkBuddy: 水平滚动后通知宿主重新合成
    // WorkBuddy: 列表项拖出（列表项 → 资源管理器/桌面）的起拖判定
    ON_WM_LBUTTONDOWN()
    ON_WM_MOUSEMOVE()
    ON_WM_LBUTTONUP()
END_MESSAGE_MAP()

// 列表控件滚轮处理：
//   - 按住 Ctrl 滚动：把滚轮方向交给宿主小窗口缩放（放大/缩小一档）；
//   - 未按 Ctrl：保持列表控件原有的滚动行为。
// WorkBuddy: 无论哪种分支，滚轮都会改变列表可见内容，结束后请宿主重新合成分层位图
BOOL CWidgetListCtrl::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
    BOOL bRet;
    if ((nFlags & MK_CONTROL) && m_pOwner != NULL)
        bRet = m_pOwner->ZoomByWheel(zDelta);
    else
        bRet = CListCtrl::OnMouseWheel(nFlags, zDelta, pt);

    // 滚轮导致列表内容变化：请宿主重新抓取列表并更新分层位图
    if (m_pOwner != NULL && !m_pOwner->IsRenderingLayered())
        m_pOwner->RenderLayered();
    return bRet;
}

// WorkBuddy: 列表控件自绘后，宿主分层位图中的列表内容是旧帧，需要重新合成。
// 注意：先执行默认绘制（内容画到列表自身 DC，供随后 PrintWindow 抓取），
// 再请求宿主 RenderLayered；宿主渲染时会置"渲染中"标志，避免 PrintWindow
// 触发本函数造成递归（且 PrintWindow 走 WM_PRINT，不会回调 WM_PAINT）
void CWidgetListCtrl::OnPaint()
{
    CListCtrl::OnPaint();
    if (m_pOwner != NULL && !m_pOwner->IsRenderingLayered())
        m_pOwner->RenderLayered();
}

// WorkBuddy: 垂直滚动（滚动条/键盘）后请求宿主重新合成
void CWidgetListCtrl::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pBar)
{
    CListCtrl::OnVScroll(nSBCode, nPos, pBar);
    if (m_pOwner != NULL && !m_pOwner->IsRenderingLayered())
        m_pOwner->RenderLayered();
}

// WorkBuddy: 水平滚动（滚动条/键盘）后请求宿主重新合成
void CWidgetListCtrl::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pBar)
{
    CListCtrl::OnHScroll(nSBCode, nPos, pBar);
    if (m_pOwner != NULL && !m_pOwner->IsRenderingLayered())
        m_pOwner->RenderLayered();
}

// ---------------------------------------------------------------------------
// WorkBuddy: 列表项"拖出"（列表 → 资源管理器/桌面）的起拖判定
// ---------------------------------------------------------------------------
// 为什么要在列表控件里自己判定：本控件是标准 OLE 拖放源，起拖时机必须由
// "按下 + 移动超过拖拽阈值"共同决定。若直接用 LVS_EX_... 或按钮抬起即起拖，
// 单击选中、双击打开都会被误判成拖拽，基本可用性被破坏
void CWidgetListCtrl::OnLButtonDown(UINT nFlags, CPoint point)
{
    // 记录按下点，先只置"可能起拖"；是否真的起拖留给 OnMouseMove 判断。
    // 必须调用基类：列表的选中、双击、键盘导航等默认行为都在基类里
    m_bDragPending = TRUE;
    m_ptDragStart  = point;
    CListCtrl::OnLButtonDown(nFlags, point);
}

void CWidgetListCtrl::OnMouseMove(UINT nFlags, CPoint point)
{
    if (m_bDragPending && (nFlags & MK_LBUTTON) && m_pOwner != NULL)
    {
        // 只有位移超过系统拖拽阈值才起拖。阈值来自 SM_CXDRAG/SM_CYDRAG
        // （默认 4px）：这是"单击/双击"与"拖拽"的系统级分界线，不可省略。
        // 取绝对值用手写条件而非 abs()：避免依赖 <stdlib.h> 的间接引入顺序
        const int dx = point.x - m_ptDragStart.x;
        const int dy = point.y - m_ptDragStart.y;
        const int ax = (dx < 0) ? -dx : dx;
        const int ay = (dy < 0) ? -dy : dy;
        if (ax >= ::GetSystemMetrics(SM_CXDRAG) ||
            ay >= ::GetSystemMetrics(SM_CYDRAG))
        {
            m_bDragPending = FALSE;     // 起拖后本次按住不再重复触发
            CListCtrl::OnMouseMove(nFlags, point);

            // 拖出的对象 = "按下点命中的那一项"（与资源管理器一致：
            // 拖的是按下时选中的项目，而不是鼠标飘到别处后的项）
            LVHITTESTINFO hti = {};
            hti.pt = m_ptDragStart;
            const int nItem = HitTest(&hti);
            if (nItem >= 0 && nItem < (int)m_pOwner->m_arrPaths.GetCount())
            {
                // 只把这一项作为拖出对象（列表为单选）。多选支持留待后续：
                // StartDragOut 接收的就是路径数组，扩展时改为采集全部选中项即可
                CStringArray arr;
                arr.Add(m_pOwner->m_arrPaths[nItem]);
                m_pOwner->StartDragOut(arr);
            }
            return;
        }
    }
    CListCtrl::OnMouseMove(nFlags, point);
}

void CWidgetListCtrl::OnLButtonUp(UINT nFlags, CPoint point)
{
    m_bDragPending = FALSE;     // 松开左键：本次不再可能起拖
    CListCtrl::OnLButtonUp(nFlags, point);
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

CDeskTidyWidget::CDeskTidyWidget()
    : m_hHost(NULL)
    , m_nLayerMode(0)        // 默认最底层
    , m_nViewMode(0)         // 默认列表视图
    , m_nDragMode(0)
    , m_bAttached(FALSE)
    , m_hNotify(NULL)
    , m_rcLast(0, 0, 0, 0)
    , m_nBtnHover(WIDGET_BTN_NONE)    // 初始无悬停按钮
    , m_nBtnPressed(WIDGET_BTN_NONE)  // 初始无按下按钮
    , m_bCollapsed(FALSE)             // 默认展开
    , m_rcExpanded(0, 0, 0, 0)        // 展开矩形初始为空
    , m_bUserVisible(TRUE)            // 初始为"用户主动显示"
    , m_bDesktopCovered(FALSE)        // 初始非"显示桌面"模式
    , m_dwLastRestore(0)              // 初始无"不可见兜底恢复"记录
    , m_nNotCoveredCount(0)           // 初始未连续检测到"桌面未盖住"
    , m_bRaisedAboveDesktop(FALSE)    // WorkBuddy: 初始未被抬到"桌面之上"
    , m_bShowDesktop(FALSE)           // WorkBuddy: 初始非"显示桌面"模式
    , m_nZoom(100)                    // 默认缩放级别：100%
    , m_clrHeader(WIDGET_DEFAULT_HEADER_COLOR) // 默认标题栏颜色：深蓝（宏定义见 DeskTidyWidget.h）
    , m_clrBg(WIDGET_DEFAULT_BG_COLOR)         // 默认背景色：浅灰（与设置界面"恢复默认"共用）
    , m_clrTextFixed(WIDGET_DEFAULT_TEXT_COLOR) // WorkBuddy: 自定义名称色初值（近黑）
    , m_bTextAuto(TRUE)                        // WorkBuddy: 名称颜色默认"自动跟随背景"
    , m_byHeaderAlpha(255)            // WorkBuddy: 标题栏透明度默认 255（不透明）
    , m_byBgAlpha(255)                // WorkBuddy: 背景透明度默认 255（不透明）
    , m_bRendering(FALSE)             // WorkBuddy: 分层重绘标志（防止 PrintWindow 递归）
    , m_bModalChildOpen(FALSE)        // WorkBuddy: 初始无模态子对话框打开
    , m_bInDragOut(FALSE)             // WorkBuddy: 初始无拖出操作进行中
    , m_bZOrderMaintain(TRUE)         // WorkBuddy: z 序维护默认开启（保持历史行为）
    , m_bBottomViaChild(FALSE)        // WorkBuddy: "桌面子窗口"置底默认关闭（保持历史方案）
    , m_dwLastAttachTryTick(0)        // WorkBuddy: 挂载重试节流基线（0=立即可试）
    , m_hDesktopOwner(NULL)           // 尚未挂接桌面 Owner
    , m_bOwnerSuspended(FALSE)        // Owner 未被鼠标交互挂起
    , m_bOriginalOwnerValid(FALSE)    // 尚未捕获原始 Owner
    , m_hOriginalOwner(NULL)
    , m_dwLastPopupRepair(0)          // 尚未执行过 last-active-popup 修复
{
    // 基础字体 LOGFONT 清零（OnCreate 捕获列表默认字体后填充，
    // 缩放时以它为基准按百分比调整字高）
    ZeroMemory(&m_lfBaseFont, sizeof(m_lfBaseFont));
}

CDeskTidyWidget::~CDeskTidyWidget()
{
}

// ---------------------------------------------------------------------------
// 创建小窗口
// ---------------------------------------------------------------------------

BOOL CDeskTidyWidget::CreateWidget(const CRect& rect, const CString& strDir, HWND hNotify)
{
    // 记录目录、通知窗口与初始矩形（供后续持久化使用）
    m_strDir = strDir;
    m_hNotify = hNotify;
    m_rcLast = rect;

    // 注册窗口类：启用重绘重排与双击消息，使用系统箭头光标
    LPCTSTR pszClass = AfxRegisterWndClass(
        CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
        ::LoadCursor(NULL, IDC_ARROW),
        NULL, NULL);

    // 统一扩展样式（工具窗口，不抢焦点）：
    //   WS_EX_TOOLWINDOW  - 不在任务栏显示
    //   WS_EX_NOACTIVATE  - 点击时不抢占焦点（保持其他应用的前台状态）
    //   WS_EX_LAYERED     - WorkBuddy: 分层窗口。标题栏/背景各自独立透明度依赖
    //                       逐像素 alpha 呈现（UpdateLayeredWindow），必须是分层窗口；
    //                       两个 alpha 均为 255 时渲染结果与不透明窗口完全一致
    DWORD dwExStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED;

    // WorkBuddy: 窗口样式与父窗口——两种层级模式都按"普通顶层弹出窗口"创建。
    // 置底模式不再挂接为桌面图标层宿主（WorkerW/Progman）的子窗口：
    //   Raised Desktop 下桌面父窗口以 WS_EX_NOREDIRECTIONBITMAP 创建、没有
    //   GDI 内容，挂接后 DWM 无法合成 → 必然半透明且不可交互（见 IsRaisedDesktop）。
    // 统一用顶层窗口：有完整重定向表面（不透明、可交互），置底由
    // SetBottomLayer 压 HWND_BOTTOM 实现，Win+D 由最小化兜底链保持可见。
    // WS_CLIPCHILDREN：父窗口绘制时跳过子控件区域（列表控件），
    // 避免父窗口擦背景把列表盖住再重刷造成的闪烁
    DWORD dwStyle = WS_POPUP | WS_CLIPCHILDREN;
    HWND  hParent = NULL;

    if (!CreateEx(dwExStyle, pszClass, _T("DeskTidy"), dwStyle,
                  rect.left, rect.top, rect.Width(), rect.Height(),
                  hParent, NULL))
        return FALSE;

    // WorkBuddy: 置底模式不再挂接桌面，hParent 恒为 NULL，
    // 因此不存在"创建即为桌面子窗口"的分支（旧逻辑见 SetBottomLayer_bk）。
    // 挂接状态保持构造时的初值：m_bAttached = FALSE、m_hHost = NULL

    // WorkBuddy: 创建后立即建立"不透明基线"——禁用 DWM 过渡动画、清除
    // 透出下层的扩展样式（顶层窗口不会再置为分层窗口）。放在首次绘制之前，
    // 确保小窗口从第一帧起就按不透明合成
    ForceOpaque();
    DumpDwmState(_T("CreateWidget"));   // WorkBuddy: 诊断输出 cloak 状态

    // 列表控件在 OnCreate 中已创建，先填充文件内容
    RefreshFiles();

    // 按配置的层级模式设置层级（最底层/悬浮置顶）
    SetLayerMode(m_nLayerMode);

    // 启动一次性"创建后刷新"定时器：窗口完全显示后再刷新一次内容，
    // 保证每个 Widget 创建后都先刷新内容（覆盖创建瞬间到显示期间的目录变动）
    SetTimer(WIDGET_INIT_REFRESH_TIMER, WIDGET_INIT_REFRESH_DELAY, NULL);

    // 启动低频 z 序维护定时器：周期检查"显示桌面"（Win+D）状态，
    // 检测到桌面模式时把小窗口提到桌面之上（见 MaintainZOrder）。
    // 检查只读，仅在状态翻转时操作窗口，不会造成闪烁。
    // WorkBuddy: 定时器无条件启动——"启用层级维护"开关只控制 MaintainZOrder
    // 内部是否执行（入口短路，无日志无操作）；桌面子窗口方案的"挂载失败自动
    // 重试"（OnTimer → TryReattachDesktopChild）也依赖此定时器，不能停
    SetTimer(WIDGET_ZORDER_TIMER, WIDGET_ZORDER_INTERVAL, NULL);

    // WorkBuddy: 注册 Shell 钩子，确定性捕获"显示桌面"（Win+D）事件。
    // 不依赖"启用层级维护"开关——Win+D 是系统级事件，应始终处理；
    // 即使关闭 z 序维护，此钩子也能在显示桌面时把小窗口提到桌面之上、
    // 退出时压回置底。注册失败（极罕见）不影响其余功能
    if (!::RegisterShellHookWindow(m_hWnd))
        LogZ(_T("OnCreate: RegisterShellHookWindow 失败，Win+D 兜底退化为轮询"));

    return TRUE;
}

// WorkBuddy: 重建窗口：窗口被系统销毁后，按对象保留的配置重新创建并应用全部属性。
// （不再依赖"桌面挂接"，Explorer 重启不会销毁本窗口；此处主要应对异常销毁）
// 与首次创建的区别：m_bCollapsed 等状态已存在于对象中，SetCollapsed 在
// "状态未变化"时会提前返回，因此创建前先把折叠标志复位，创建后再重新
// 应用折叠；同时尊重用户的可见意图（曾从托盘"隐藏全部小窗口"则保持隐藏）
void CDeskTidyWidget::RecreateWidget()
{
    if (m_hWnd != NULL)
        return;   // 窗口仍存活，无需重建

    // 记录要恢复的折叠状态与可见意图（创建期间复位折叠标志）
    BOOL bCollapsed = m_bCollapsed;
    BOOL bUserVisible = m_bUserVisible;
    m_bCollapsed = FALSE;   // 复位：让创建后的 SetCollapsed 能触发折叠

    // 先设置属性（m_hWnd 为空时仅记录，窗口创建后由 OnCreate 应用缩放等）；
    // CreateWidget 内部会按层级模式重新挂接桌面（WorkerW/Progman）。
    // 位置使用 GetLastRect()：折叠状态返回"展开尺寸 + 折叠条当前位置"，
    // 保证折叠窗口重建后仍能正确恢复展开大小
    SetLayerMode(m_nLayerMode);
    SetViewMode(m_nViewMode);
    SetZoom(m_nZoom);
    if (CreateWidget(GetLastRect(), m_strDir, m_hNotify))
    {
        SetCollapsed(bCollapsed);
        // 用户曾从托盘"隐藏全部小窗口"时，重建后保持隐藏
        if (!bUserVisible)
            ::ShowWindow(m_hWnd, SW_HIDE);
    }
}

// ---------------------------------------------------------------------------
// 层级模式：0 = 最底层（普通顶层窗口，z 序最底），1 = 悬浮置顶
// ---------------------------------------------------------------------------

void CDeskTidyWidget::SetLayerMode(int nMode)
{
    m_nLayerMode = nMode;
    if (m_hWnd == NULL)
        return;

    if (nMode == 0)
        SetBottomLayer();       // 底层模式：普通顶层窗口压到 z 序最底（不挂接桌面）
    else
        SwitchToTopmost();      // 置顶模式：浮于所有窗口之上
}

// WorkBuddy: 设置"置底采用桌面子窗口"开关（设置界面 IDC_CHECK_BOTTOM_CHILD）。
// 开启且处于置底模式时，SetBottomLayer() 会把小窗口挂接为桌面内容窗口
// （WorkerW/SHELLDLL_DefView 的父窗口）的子窗口——天然位于所有普通窗口之下、
// 随桌面一起升降，无需定时 z 序维护（MaintainZOrder / OnShellHook 均短路）。
// 关闭则沿用旧方案：普通顶层窗口 + 桌面 Owner（WitchDrawer）+ 定时 z 序维护。
// 若当前仍以"桌面子窗口"方式挂接而开关被关闭，立即脱离恢复为普通顶层窗口
//（随后由 SetLayerMode 按当前层级模式重新落位）。
void CDeskTidyWidget::SetBottomViaChild(BOOL bEnable)
{
    if (m_bBottomViaChild == bEnable)
        return;                       // 状态未变化，避免无谓的身份切换/闪烁

    m_bBottomViaChild = bEnable;

    // 从"桌面子窗口"回退：立即脱离桌面宿主，恢复 WS_POPUP 顶层窗口身份。
    // 脱离顺序与 AttachToDesktop/SwitchToTopmost 保持一致：
    // 先 ForceOpaque 建 不透明基线 → 隐藏 → SetParent(NULL) → 改样式 →
    // SWP_FRAMECHANGED 让样式立即生效 → 补不透明 → 强制重绘
    if (!bEnable && m_bAttached && m_hWnd != NULL && ::GetParent(m_hWnd) != NULL)
    {
        ForceOpaque();
        ::ShowWindow(m_hWnd, SW_HIDE);
        ::SetParent(m_hWnd, NULL);
        ModifyStyle(WS_CHILD | WS_CLIPSIBLINGS, WS_POPUP);
        m_bAttached = FALSE;
        m_hHost = NULL;
        ::SetWindowPos(m_hWnd, NULL, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        ForceOpaque();
        ::ShowWindow(m_hWnd, SW_SHOWNA);
        ::RedrawWindow(m_hWnd, NULL, NULL,
                       RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);
        // WorkBuddy: 身份翻转后分层位图可能失效（ULW 窗口不收 WM_PAINT），
        // 显式重新合成（同 AttachToDesktop 说明）
        RenderLayered();
        PostMessage(WM_WIDGET_CHILD_RENDER);   // 延迟补渲染（见宏定义说明）
        LogZ(_T("SetBottomViaChild: 已脱离桌面子窗口，恢复为普通顶层窗口"));
    }
}

// WorkBuddy: 桌面子窗口方案挂载失败后的自动重试。
// 触发场景：SetBottomLayer 时桌面宿主暂不可用（explorer 重启间隙、桌面
// 尚未创建完成），小窗口停留在普通顶层窗口身份——不重试就永远浮在
// 普通窗口 z 序里，失去"作为桌面一部分、免 z 序维护"的意义。
// 由 1s 定时器（WIDGET_ZORDER_TIMER）驱动：
//   1) 已挂接且宿主窗口仍存活 → 零开销直接返回（每秒只有一次 GetParent/
//      IsWindow 调用，不碰任何窗口属性，无闪烁之虞）；
//   2) 未挂接或宿主已失效（explorer 重启会销毁旧 WorkerW/Progman 子结构）
//     → 按约 3 秒节流重试 AttachToDesktop（内部完成脱离旧宿主、样式翻转、
//      不透明保障与强制重绘），成功后小窗口重新成为桌面内容窗口的子窗口；
//   3) 重试成功/失败都写一条日志（仅状态变化时各一条，不刷屏）。
void CDeskTidyWidget::TryReattachDesktopChild()
{
    if (m_hWnd == NULL)
        return;

    // 仅"置底 + 桌面子窗口"模式需要重试
    if (m_nLayerMode != 0 || !m_bBottomViaChild)
        return;

    // 拖拽/模态操作期间跳过：DoDragDrop 是嵌套模态循环，中途 SetParent
    // 改变窗口身份会干扰拖放目标对源窗口的判定（与 MaintainZOrder 的
    // m_bModalChildOpen 暂停策略保持一致）
    if (m_bModalChildOpen)
        return;

    // 已挂接且宿主仍然有效：无需重试（零开销路径）
    HWND hParent = ::GetParent(m_hWnd);
    if (m_bAttached && hParent != NULL && ::IsWindow(hParent))
        return;

    // 节流：约 3 秒尝试一次。挂载失败多为 explorer 重启间隙，短时间
    // 反复 FindWindow/样式翻转没有意义，日志也会刷屏
    DWORD dwNow = ::GetTickCount();
    if (m_dwLastAttachTryTick != 0 &&
        (dwNow - m_dwLastAttachTryTick) < 3000)
        return;
    m_dwLastAttachTryTick = dwNow;

    // 重试挂接（AttachToDesktop 内部处理：脱离旧宿主 → 隐藏 → SetParent →
    // WS_CHILD 样式 → HWND_TOP → 不透明保障 → SW_SHOWNA → 强制重绘）
    ClearDesktopOwner();
    if (AttachToDesktop())
    {
        LogZ(_T("TryReattach: 挂载失败重试成功，宿主=0x%08X"),
             (unsigned)(DWORD_PTR)::GetParent(m_hWnd));
    }
    else
    {
        LogZ(_T("TryReattach: 桌面宿主暂不可用，约 3 秒后再次重试"));
    }
}

// 查找桌面图标层宿主窗口。
// 说明：Windows 10/11 的桌面由 Progman -> WorkerW(图标层，含 SHELLDLL_DefView)
// -> WorkerW(壁纸层) 构成。最底层模式把小窗口挂接为"图标层宿主"的子窗口：
//   1. 小窗口随桌面一起被系统抬升/放下，"显示桌面"（Win+D）时依然可见；
//   2. 小窗口作为桌面层的一部分，天然位于所有普通窗口之下；
//   3. 宿主是图标层（而非壁纸层），且小窗口置于 SHELLDLL_DefView 之上，
//      因此位于桌面图标之上，鼠标可以正常交互（点击/拖动/缩放）。
HWND CDeskTidyWidget::FindDesktopWorkerW()
{
    // 找到 Progman（桌面根窗口）
    HWND hProgman = ::FindWindow(_T("Progman"), NULL);
    if (hProgman == NULL)
        return NULL;

    // 情况一：桌面图标视图（SHELLDLL_DefView）直接作为 Progman 的子窗口
    //（Win10 常见配置）→ 直接以 Progman 为宿主
    if (::FindWindowEx(hProgman, NULL, _T("SHELLDLL_DefView"), NULL) != NULL)
        return hProgman;

    // 情况二：遍历所有 WorkerW 顶层窗口，找到包含 SHELLDLL_DefView 的那个
    //（Win11 常见配置：SHELLDLL_DefView 被放到 WorkerW 图标层里）
    HWND hWorker = NULL;
    while ((hWorker = ::FindWindowEx(NULL, hWorker, _T("WorkerW"), NULL)) != NULL)
    {
        if (::FindWindowEx(hWorker, NULL, _T("SHELLDLL_DefView"), NULL) != NULL)
            return hWorker;    // 图标层 WorkerW 就是宿主
    }

    // 兜底：直接使用 Progman 作为宿主
    return hProgman;
}

// ---------------------------------------------------------------------------
// WorkBuddy: Raised Desktop 判定
// ---------------------------------------------------------------------------

// 判定系统是否处于 "Raised Desktop" 模式（Win11 24H2 及较新 Win10）。
// 微软官方说明要点：新版桌面把 Progman 改为以 WS_EX_NOREDIRECTIONBITMAP 创建的
// 顶级窗口 —— 该窗口**根本没有 GDI 内容**；SHELLDLL_DefView 变成 WS_EX_LAYERED
// 子窗口（几乎完全透明，只显示图标与文字）；壁纸改由 DefView 之下的 WorkerW 渲染。
// 后果（这正是小窗口半透明的真正原因）：
//   把普通窗口 SetParent 到 Progman/WorkerW 之下，由于父窗口没有重定向表面，
//   子窗口的 GDI 内容无法被 DWM 正常合成 → 必然半透明、与壁纸混合、时隐时现。
//   这不是样式问题，靠 DWMWA_* / cloak / alpha 都修不好。
// 微软给出的要求是：应用必须自建 WS_EX_LAYERED 子窗口，且 Z 序位于 DefView **之下**
// —— 那是"壁纸层"，鼠标事件会被 DefView 吞掉，无法交互，与本程序需求冲突。
// 因此本程序在 Raised Desktop 下放弃挂接，改用顶层窗口 + Z 序维护（见 SetBottomLayer）。
BOOL CDeskTidyWidget::IsRaisedDesktop()
{
    HWND hProgman = ::FindWindow(_T("Progman"), NULL);
    if (hProgman == NULL)
        return FALSE;

    return (::GetWindowLongPtr(hProgman, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) != 0;
}

// ---------------------------------------------------------------------------
// WorkBuddy: DWM 不透明保障 / 半透明诊断
// ---------------------------------------------------------------------------

// 强制小窗口在 DWM 合成中按"完全不透明"渲染。
// 问题背景：小窗口挂接为桌面宿主（WorkerW/Progman）的子窗口后会出现半透明
// （与壁纸 alpha 混合、标题栏发白）。根因在 DWM 合成层而非绘制层：
//   a) 父窗口属 shell 层，其 cloak 状态可由子窗口继承（DWM_CLOAKED_INHERITED）；
//   b) 窗口从顶层（WS_POPUP）在运行期翻转成子窗口（WS_CHILD）时，DWM 会走一次
//      cloak → uncloak 的淡入淡出过渡；过渡未走完（或被中断）就卡在中间态。
// 本函数从三条路径同时消除：
//   1) DWMWA_TRANSITIONS_FORCEDISABLED = TRUE —— 禁止 DWM 过渡动画，
//      从根本上掐掉 cloak/uncloak 的 fade 中间态；
//   2) 清除 WS_EX_TRANSPARENT / WS_EX_COMPOSITED —— 前者延迟绘制会透出下层，
//      后者与跨进程父窗口叠加时易产生 alpha 异常；
//   3) 置为分层窗口并显式 alpha=255 —— 分层窗口由 DWM 单独合成一层，
//      alpha 明确为 255 即强制不透明（Win8+ 起支持 WS_EX_LAYERED 子窗口）。
// 调用时机：窗口创建后、以及任何 SetParent / 样式翻转的前后。
void CDeskTidyWidget::ForceOpaque()
{
    if (m_hWnd == NULL || !::IsWindow(m_hWnd))
        return;

    // 1) 禁用 DWM 过渡动画（须早于任何 SetParent / 样式翻转生效）
    BOOL bDisable = TRUE;
    ::DwmSetWindowAttribute(m_hWnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                            &bDisable, sizeof(bDisable));

    // 2) 清除会造成"透出下层"的扩展样式；同时确保分层样式（WS_EX_LAYERED）存在。
    // WorkBuddy: 标题栏/背景各自独立透明度采用"分层窗口 + UpdateLayeredWindow
    //   逐像素 alpha"方案，因此这里必须保留 WS_EX_LAYERED，且**不能**调用
    //   SetLayeredWindowAttributes(LWA_ALPHA)——它会改用统一 alpha 呈现，覆盖
    //   UpdateLayeredWindow 的逐像素 alpha。两个 alpha 均为 255 时效果与不透明一致
    LONG_PTR ex = ::GetWindowLongPtr(m_hWnd, GWL_EXSTYLE);
    LONG_PTR exNew = ex & ~(WS_EX_TRANSPARENT | WS_EX_COMPOSITED);
    if ((exNew & WS_EX_LAYERED) == 0)
        exNew |= WS_EX_LAYERED;
    if (exNew != ex)
        ::SetWindowLongPtr(m_hWnd, GWL_EXSTYLE, exNew);

    // 3) 样式变更后通知系统重算窗口框架，让 layered 立即生效。
    // 不改变位置/大小/z 序/激活，且不触发重绘（重绘由调用方统一处理）
    ::SetWindowPos(m_hWnd, NULL, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                   SWP_FRAMECHANGED | SWP_NOREDRAW);
}

// 诊断：把小窗口与其宿主的 DWM cloak 状态输出到调试输出与 DeskTidy_zorder.log。
// 判定：cloaked 非 0 即被 DWM 隐藏/淡化（1=APP 2=SHELL 4=INHERITED）；
//       父窗口被 cloak 时子窗口会继承，因此两者必须一起看。
void CDeskTidyWidget::DumpDwmState(LPCTSTR pszTag)
{
    if (m_hWnd == NULL || !::IsWindow(m_hWnd))
        return;

    DWORD dwCloaked = 0;
    HRESULT hr = ::DwmGetWindowAttribute(m_hWnd, DWMWA_CLOAKED,
                                         &dwCloaked, sizeof(dwCloaked));
    LONG_PTR ex = ::GetWindowLongPtr(m_hWnd, GWL_EXSTYLE);

    TRACE(_T("[DWM] %s hwnd=%p cloaked=0x%X hr=0x%08X\n"), pszTag, m_hWnd, dwCloaked, hr);
    TRACE(_T("[DWM] %s exStyle=0x%X LAYERED=%d TRANSPARENT=%d COMPOSITED=%d\n"),
          pszTag, (DWORD)ex,
          (ex & WS_EX_LAYERED)      ? 1 : 0,
          (ex & WS_EX_TRANSPARENT)  ? 1 : 0,
          (ex & WS_EX_COMPOSITED)   ? 1 : 0);
    LogZ(_T("[DWM] %s hwnd=%p cloaked=0x%X hr=0x%08X exStyle=0x%X LAYERED=%d"),
         pszTag, m_hWnd, dwCloaked, hr, (DWORD)ex, (ex & WS_EX_LAYERED) ? 1 : 0);

    // WorkBuddy: Raised Desktop 判定 + 桌面各层窗口的扩展样式，
    // 这是"挂接后半透明"的根因排查关键：
    //   Progman 有 WS_EX_NOREDIRECTIONBITMAP → Raised Desktop → 挂接必然半透明
    //   DefView  有 WS_EX_LAYERED            → 新版桌面（图标层本身是分层窗口）
    HWND hProgman = ::FindWindow(_T("Progman"), NULL);
    if (hProgman != NULL)
    {
        LONG_PTR exP = ::GetWindowLongPtr(hProgman, GWL_EXSTYLE);
        TRACE(_T("[DWM] %s Progman=%p exStyle=0x%X NOREDIRECTION=%d LAYERED=%d\n"),
              pszTag, hProgman, (DWORD)exP,
              (exP & WS_EX_NOREDIRECTIONBITMAP) ? 1 : 0,
              (exP & WS_EX_LAYERED) ? 1 : 0);
        LogZ(_T("[DWM] %s Progman=%p exStyle=0x%X NOREDIRECTION=%d LAYERED=%d"),
             pszTag, hProgman, (DWORD)exP,
             (exP & WS_EX_NOREDIRECTIONBITMAP) ? 1 : 0,
             (exP & WS_EX_LAYERED) ? 1 : 0);

        // DefView：可能直接挂在 Progman 下（旧式），也可能在某个 WorkerW 下（Win11）
        HWND hDv = ::FindWindowEx(hProgman, NULL, _T("SHELLDLL_DefView"), NULL);
        if (hDv == NULL)
        {
            HWND hW = NULL;
            while ((hW = ::FindWindowEx(NULL, hW, _T("WorkerW"), NULL)) != NULL)
            {
                hDv = ::FindWindowEx(hW, NULL, _T("SHELLDLL_DefView"), NULL);
                if (hDv != NULL)
                    break;
            }
        }
        if (hDv != NULL)
        {
            LONG_PTR exD = ::GetWindowLongPtr(hDv, GWL_EXSTYLE);
            LogZ(_T("[DWM] %s DefView=%p exStyle=0x%X LAYERED=%d"),
                 pszTag, hDv, (DWORD)exD, (exD & WS_EX_LAYERED) ? 1 : 0);
        }
    }
    LogZ(_T("[DWM] %s RaisedDesktop=%d"), pszTag, IsRaisedDesktop());

    // 宿主（父窗口）的 cloak 状态：父被 cloak 时子窗口必然受影响
    HWND hParent = ::GetParent(m_hWnd);
    if (hParent != NULL)
    {
        DWORD dwParentCloaked = 0;
        ::DwmGetWindowAttribute(hParent, DWMWA_CLOAKED,
                                &dwParentCloaked, sizeof(dwParentCloaked));
        TCHAR szCls[64] = {};
        ::GetClassName(hParent, szCls, _countof(szCls));
        TRACE(_T("[DWM] %s parent=%p cls=%s cloaked=0x%X\n"),
              pszTag, hParent, szCls, dwParentCloaked);
        LogZ(_T("[DWM] %s parent=%p cls=%s cloaked=0x%X"),
             pszTag, hParent, szCls, dwParentCloaked);
    }
}

// 将窗口挂接为桌面图标层宿主（WorkerW/Progman）的子窗口。
// 成功返回 TRUE；宿主暂不可用（explorer 重启间隙等）返回 FALSE，
// 由调用方决定兜底策略。
// WorkBuddy: 当前逻辑已不再调用本函数——置底改用普通顶层窗口（SetBottomLayer）。
//   本函数仅为 SetBottomLayer_bk()（旧实现备份）保留，请勿删除。
//   注意：Raised Desktop 下挂接会导致窗口半透明，属已知问题（见 IsRaisedDesktop）
BOOL CDeskTidyWidget::AttachToDesktop()
{
    // 查找桌面宿主窗口（explorer 重启期间可能为 NULL，稍后由定时器重试）
    HWND hHost = FindDesktopWorkerW();
    if (hHost == NULL)
    {
        m_bAttached = FALSE;
        m_hHost = NULL;
        return FALSE;
    }

    // 已经挂在同一个宿主上，无需重复处理
    if (m_bAttached && ::GetParent(m_hWnd) == hHost)
        return TRUE;

    // WorkBuddy: 翻转前先禁用 DWM 过渡动画并建立不透明基线，
    // 避免随后的 SetParent / 样式翻转触发 cloak 淡入淡出而卡在半透明中间态
    ForceOpaque();

    // 若当前挂在其它窗口下（置顶模式/旧宿主），先脱离为顶层窗口
    if (::GetParent(m_hWnd) != NULL)
    {
        ::SetParent(m_hWnd, NULL);
        ModifyStyle(WS_CHILD, WS_POPUP);
    }

    // WorkBuddy: 关键顺序 —— 先隐藏 → 建父子 → 改样式 → 再显示。
    // 旧顺序是"先 ModifyStyle(WS_POPUP→WS_CHILD)、后 SetParent"：改样式时
    // 父窗口仍是 NULL，窗口处于"已声明为子窗口却没有父"的非法中间态，
    // DWM 会将其 cloak；之后再 SetParent 也不保证走完 uncloak → 半透明。
    // 改为先 SetParent 建立父子再改样式，并用 ShowWindow(HIDE→SHOWNA)
    // 强制 DWM 重建该窗口的合成节点，彻底完成 uncloak。
    // WS_CLIPSIBLINGS：小窗口绘制时裁剪掉重叠兄弟（SHELLDLL_DefView）的
    // 区域，避免小窗口绘图渗入图标层造成画面残留
    ::ShowWindow(m_hWnd, SW_HIDE);
    ::SetParent(m_hWnd, hHost);
    ModifyStyle(WS_POPUP, WS_CHILD | WS_CLIPSIBLINGS);

    // 置于宿主 z 序顶部（HWND_TOP 仅影响宿主内部兄弟顺序）：
    // 位于 SHELLDLL_DefView（桌面图标）之上，鼠标可正常点击/拖动/缩放；
    // 不移动、不改变大小、不激活。SWP_FRAMECHANGED 让样式改动立即生效
    ::SetWindowPos(m_hWnd, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // WorkBuddy: 挂接并改完样式后再补一次，覆盖最终样式下的合成状态
    ForceOpaque();

    // 确保可见（SW_SHOWNA：不激活）
    ::ShowWindow(m_hWnd, SW_SHOWNA);

    // WorkBuddy: 立即强制重绘整窗（含子控件），确保不透明背景第一时间铺满，
    // 避免 uncloak 过程中残留的半透明帧被留在屏幕上
    ::RedrawWindow(m_hWnd, NULL, NULL,
                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);

    // WorkBuddy: 关键修复——挂接为 WS_CHILD 后必须重新合成分层位图。
    // 调用过 UpdateLayeredWindow 的窗口不再接收 WM_PAINT（内容完全由 ULW
    // 位图提供），上面的 RedrawWindow 对它无效；而 SetParent +
    // WS_POPUP→WS_CHILD 的身份翻转可能使 DWM 丢弃旧分层位图 → 内容为空 =
    // 分层窗口完全透明 = "启动不显示"（未折叠路径挂接后无任何尺寸变化、
    // 再无 RenderLayered 机会；折叠路径因 SetCollapsed→OnSize→RenderLayered
    // 恰好补渲染才可见）。此处以子窗口身份显式重渲染一次
    RenderLayered();

    // WorkBuddy: 实证同步调用在挂接流程内会被 DWM 丢弃（折叠窗口靠稍后的
    // OnSize 渲染才显示、展开窗口无此机会）——再投递延迟渲染，消息派发时
    // 身份已稳定，ULW 必然生效。TryReattachDesktopChild 重试成功也走这里
    PostMessage(WM_WIDGET_CHILD_RENDER);

    m_hHost = hHost;
    m_bAttached = TRUE;

    DumpDwmState(_T("AttachToDesktop"));   // WorkBuddy: 诊断输出 cloak 状态
    return TRUE;
}

// 切换到悬浮置顶模式：脱离 WorkerW，恢复为顶层窗口并置顶
void CDeskTidyWidget::SwitchToTopmost()
{
    // WorkBuddy: 翻转前先禁用 DWM 过渡动画，避免"子窗口 → 顶层窗口"的
    // 身份切换触发 cloak 淡入淡出而残留半透明帧
    ForceOpaque();

    // 若仍挂接在桌面宿主上，先脱离
    if (::GetParent(m_hWnd) != NULL)
    {
        ::SetParent(m_hWnd, NULL);
        ModifyStyle(WS_CHILD, WS_POPUP);
    }
    m_bAttached = FALSE;
    m_hHost = NULL;

    // WorkBuddy: WitchDrawer 方案——切回置顶模式前必须先解除桌面 Owner，
    // 否则窗口仍是 Progman 的 owned 窗口，置顶后会随桌面一起被系统抬升
    //（不满足"悬浮于所有窗口之上"的语义）
    ClearDesktopOwner();

    // 设置为顶层窗口，浮于所有普通窗口之上
    ::SetWindowPos(m_hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ::ShowWindow(m_hWnd, SW_SHOWNA);

    // WorkBuddy: 脱离后补一次不透明保障，并立即重绘铺满背景
    ForceOpaque();
    ::RedrawWindow(m_hWnd, NULL, NULL,
                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);

    // WorkBuddy: 子窗口→顶层窗口的身份翻转同样会使旧分层位图失效，
    // ULW 窗口不再收 WM_PAINT，必须显式重新合成（同 AttachToDesktop 说明）
    RenderLayered();
    PostMessage(WM_WIDGET_CHILD_RENDER);   // 延迟补渲染（见宏定义说明）
}

// 切换到最底层模式。
// 方案一（非 Raised Desktop）：挂接为桌面图标层宿主（WorkerW/Progman）的子窗口，
// 并置于 SHELLDLL_DefView 之上——小窗口随桌面一起升降（"显示桌面"时依然可见）、
// 天然位于所有普通窗口之下，且鼠标可正常交互。
// 方案二（Raised Desktop / 宿主暂不可用）：退化为普通顶层窗口压到 z 序最底，
// 由 MaintainZOrder 负责"显示桌面"时提顶、退出后压回。
// WorkBuddy: Raised Desktop（Win11 24H2 / 较新 Win10）下必须用方案二——
//   桌面父窗口以 WS_EX_NOREDIRECTIONBITMAP 创建、没有 GDI 内容，挂接必然半透明，
//   微软给出的可显示位置在 DefView 之下（壁纸层、鼠标被吞、不可交互）。
//   详见 IsRaisedDesktop() 的说明。
void CDeskTidyWidget::SetBottomLayer_bk()
{
    // 首选：挂接为桌面子窗口
    if (AttachToDesktop())
        return;

    // WorkBuddy: 翻转前先禁用 DWM 过渡动画，避免身份切换卡在半透明中间态
    ForceOpaque();

    // 兜底：宿主暂不可用，先脱离为普通顶层窗口（若还挂着旧宿主）
    if (::GetParent(m_hWnd) != NULL)
    {
        ::SetParent(m_hWnd, NULL);
        ModifyStyle(WS_CHILD, WS_POPUP);
    }
    m_bAttached = FALSE;
    m_hHost = NULL;

    // 解除置顶（若之前在置顶模式），并把窗口压到 z 序最底层
    ::SetWindowPos(m_hWnd, HWND_BOTTOM, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ::ShowWindow(m_hWnd, SW_SHOWNA);

    // WorkBuddy: 脱离后补一次不透明保障，并立即重绘铺满背景
    ForceOpaque();
    ::RedrawWindow(m_hWnd, NULL, NULL,
                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);
}

// WorkBuddy: 切换到最底层模式（当前实现，不再挂接桌面）。
// 与 SetBottomLayer_bk() 的区别：不再把小窗口挂接为桌面图标层宿主
// （WorkerW/Progman）的子窗口，而是保持普通顶层窗口并压到 z 序最底。
// 原因：Raised Desktop（Win11 24H2 / 较新 Win10）下 Progman 以
// WS_EX_NOREDIRECTIONBITMAP 创建、没有 GDI 内容，挂在它下面的窗口无法被
// DWM 合成，必然半透明（详见 IsRaisedDesktop()）；且微软给出的可显示位置
// 在 SHELLDLL_DefView 之下（壁纸层），鼠标事件会被吞掉、无法交互。
// 改为顶层窗口后：
//   1) 有完整重定向表面，DWM 正常合成 → 完全不透明；
//   2) 位于所有普通窗口之下（桌面窗口始终更靠下），符合"最底层"语义；
//   3) 鼠标可正常点击/拖动/缩放/操作列表；
//   4) Win+D（显示桌面）时靠三条兜底保持可见：
//      OnSysCommand 拦截 SC_MINIMIZE → 拒绝最小化；
//      OnSize(SIZE_MINIMIZED) → PostMessage(WM_WIDGET_RESTORE)；
//      OnRestoreMinimized → 按 m_rcLast 归位并 SW_SHOWNOACTIVATE 恢复显示。
void CDeskTidyWidget::SetBottomLayer()
{
    if (m_hWnd == NULL)
        return;

    // WorkBuddy: 新方案——"置底采用桌面子窗口"。最底层模式且开启该开关时，
    // 把小窗口挂接为桌面内容窗口（WorkerW/SHELLDLL_DefView 的父窗口，由
    // FindDesktopWorkerW 解析）的子窗口：天然位于所有普通窗口之下、随桌面
    // 一起被系统抬升（Win+D 显示桌面时依然可见），且无需任何定时 z 序维护。
    // 注意：Raised Desktop（Win11 24H2 / 较新 Win10）下桌面父窗口以
    // WS_EX_NOREDIRECTIONBITMAP 创建，子窗口会半透明/不可交互——此为已知
    // 取舍，由用户在设置界面自行决定是否开启
    if (m_bBottomViaChild)
    {
        ClearDesktopOwner();          // 确保旧方案遗留的桌面 Owner 已解除
        // WorkBuddy: 挂载可能失败（宿主暂不可用：explorer 重启间隙、桌面
        // 尚未就绪等）。失败时窗口暂时保持普通顶层窗口身份，由 1s 定时器
        // 触发 TryReattachDesktopChild 自动重试，直至挂载成功
        if (AttachToDesktop())        // 内部处理 WS_CHILD + SetParent + 去半透明
        {
            ::ShowWindow(m_hWnd, SW_SHOWNA);
            LogZ(_T("SetBottomLayer: 桌面子窗口方案，已挂接为桌面内容窗口的子窗口"));
        }
        else
        {
            LogZ(_T("SetBottomLayer: 桌面宿主暂不可用，挂载失败，定时器将自动重试"));
        }
        return;
    }

    // 若当前仍挂接在桌面宿主下（旧逻辑残留或手动回退的场景），先脱离为顶层窗口
    if (::GetParent(m_hWnd) != NULL)
    {
        ::SetParent(m_hWnd, NULL);
        ModifyStyle(WS_CHILD, WS_POPUP);
    }
    m_bAttached = FALSE;
    m_hHost = NULL;

    // WorkBuddy: 不透明保障（顶层窗口不会加 WS_EX_LAYERED，见 ForceOpaque 说明）
    ForceOpaque();

    // WorkBuddy: WitchDrawer 方案——以桌面 Shell 窗口（Progman 或其 WorkerW）
    // 为 Owner。被桌面拥有的顶层窗口："显示桌面"（Win+D）不会最小化它，
    // 且始终位于 Owner 之上（随桌面一起升降）。置顶模式切换前由
    // SwitchToTopmost() 调用 ClearDesktopOwner() 解除
    SetDesktopOwner();

    // 解除置顶（若之前在置顶模式），并把窗口压到 z 序最底层。
    // 桌面窗口（Progman/WorkerW）始终位于更下方，因此压底后仍在桌面之上、可见
    ::SetWindowPos(m_hWnd, HWND_BOTTOM, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ::ShowWindow(m_hWnd, SW_SHOWNA);

    // WorkBuddy: 立即重绘整窗，确保不透明背景第一时间铺满
    ::RedrawWindow(m_hWnd, NULL, NULL,
                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);

    // WorkBuddy: 若刚从"桌面子窗口"脱离回顶层（上方 GetParent!=NULL 分支），
    // 分层位图可能已随身份翻转失效（ULW 窗口不收 WM_PAINT），显式重新合成
    RenderLayered();
    PostMessage(WM_WIDGET_CHILD_RENDER);   // 延迟补渲染（见宏定义说明）

    LogZ(_T("SetBottomLayer: 顶层窗口压底 + 桌面 Owner(0x%p)，host=%d"),
         m_hDesktopOwner, m_bAttached);
}

// ---------------------------------------------------------------------------
// WitchDrawer 桌面 Owner 机制
// ---------------------------------------------------------------------------

// 解析可作 Owner 的桌面 Shell 窗口（详见头文件声明）。
// 复用 FindDesktopWorkerW() 的宿主判定：Progman 含 SHELLDLL_DefView 时直接
// 用它（Win10 常见配置；WitchDrawer 在 Win11 同样只用 Progman），否则找含
// SHELLDLL_DefView 的 WorkerW 图标层（Win11 常见配置），再兜底 Progman。
// 注意：Owner 必须是"桌面本身"（Progman 或其图标层 WorkerW），不能是壁纸层
// WorkerW——壁纸层在桌面图标之下，把它当 Owner 会在 Win+D 时把小窗口藏到
// 图标层下面
HWND CDeskTidyWidget::ResolveDesktopOwner()
{
    return FindDesktopWorkerW();
}

// 以桌面 Shell 窗口为 Owner（WitchDrawer 的 TryAttachToDesktop）：
//   1. 已正确挂接 → 直接成功（幂等）；
//   2. 首次挂接前用 GetWindowLongPtr(GWLP_HWNDPARENT) 捕获原始 Owner（恢复用）；
//   3. SetWindowLongPtr(GWLP_HWNDPARENT, hDesktop) 挂接，并读回验证。
// 被桌面拥有的顶层窗口不会被"显示桌面"（Win+D）最小化；Windows 保证
// owned 窗口始终位于 Owner 之上——Owner（桌面）被系统抬升时小窗口跟随抬升，
// 因此"显示桌面"下小窗口依然可见（这正是 WitchDrawer 方案的根本原理）。
BOOL CDeskTidyWidget::SetDesktopOwner()
{
    if (m_hWnd == NULL || !::IsWindow(m_hWnd))
        return FALSE;

    // 幂等：Owner 已是目标桌面窗口且有效 → 直接成功
    // 注意：SDK 没有 GWLP_HWNDOWNER，Owner 就是顶层窗口的"parent 槽位"
    //（GWLP_HWNDPARENT = -8，WitchDrawer 的 WindowOwnerIndex 同为 -8）
    HWND hNow = (HWND)::GetWindowLongPtr(m_hWnd, GWLP_HWNDPARENT);
    if (m_hDesktopOwner != NULL && ::IsWindow(m_hDesktopOwner) && hNow == m_hDesktopOwner)
        return TRUE;

    // 解析桌面 Shell 窗口；不可用（explorer 重启间隙等）时失败，
    // 由调用方（SetBottomLayer/MaintainZOrder）决定兜底
    HWND hDesktop = ResolveDesktopOwner();
    if (hDesktop == NULL || !::IsWindow(hDesktop))
        return FALSE;

    // 首次挂接前保存原始 Owner：清除/恢复（ClearDesktopOwner、
    // SuspendDesktopOwnerForMouse）时用它把窗口还原到"无桌面拥有"状态
    if (!m_bOriginalOwnerValid)
    {
        m_hOriginalOwner = (hNow != NULL && ::IsWindow(hNow)) ? hNow : NULL;
        m_bOriginalOwnerValid = TRUE;
    }

    // 挂接：改变 Owner 不改变 z 序，也不影响窗口可见性
    ::SetWindowLongPtr(m_hWnd, GWLP_HWNDPARENT, (LONG_PTR)hDesktop);

    // 读回验证：少数情况下 SetWindowLongPtr 可能失败（如窗口正处于
    // 系统模态流程中），失败则恢复原始 Owner
    if ((HWND)::GetWindowLongPtr(m_hWnd, GWLP_HWNDPARENT) != hDesktop)
    {
        ::SetWindowLongPtr(m_hWnd, GWLP_HWNDPARENT, (LONG_PTR)m_hOriginalOwner);
        LogZ(_T("SetDesktopOwner: 挂接失败，恢复原始 Owner"));
        return FALSE;
    }

    m_hDesktopOwner = hDesktop;
    m_bOwnerSuspended = FALSE;
    LogZ(_T("SetDesktopOwner: 已挂接桌面 Owner=0x%p"), m_hDesktopOwner);
    return TRUE;
}

// 解除桌面 Owner（WitchDrawer 的 RestoreOriginalOwner）：
// 恢复原始 Owner（或置空），并把状态清零。用于切换置顶模式、销毁窗口前、
// 以及挂接目标失效时复位。Owner 变更不影响 z 序，不会造成闪烁
void CDeskTidyWidget::ClearDesktopOwner()
{
    if (m_hWnd != NULL && ::IsWindow(m_hWnd) && m_hDesktopOwner != NULL)
    {
        HWND hRestore = (m_bOriginalOwnerValid && m_hOriginalOwner != NULL &&
                         ::IsWindow(m_hOriginalOwner))
                        ? m_hOriginalOwner : NULL;
        ::SetWindowLongPtr(m_hWnd, GWLP_HWNDPARENT, (LONG_PTR)hRestore);
        LogZ(_T("ClearDesktopOwner: 已解除桌面 Owner，恢复原始 Owner=0x%p"), hRestore);
    }
    m_hDesktopOwner = NULL;
    m_bOwnerSuspended = FALSE;
}

// 鼠标按下瞬间临时解除桌面 Owner（WitchDrawer 的
// SuspendDesktopOwnershipForMouseInput）：
// 小窗口是 Progman 的 owned 窗口，Explorer 可能把"最近点击的 owned 窗口"
// 记为 Progman 的 last-active-popup；下次 Win+D（显示桌面）会激活该 popup，
// 把小窗口带到前台——破坏"最底层"语义。因此在鼠标按下（WM_MOUSEACTIVATE）
// 时先把 Owner 临时改回原始 Owner（或 NULL），交互结束后再恢复。
// 只恢复 Owner 不改变 z 序/焦点，安全无副作用。幂等：已挂起时直接返回
BOOL CDeskTidyWidget::SuspendDesktopOwnerForMouse()
{
    if (m_bOwnerSuspended)
        return TRUE;
    if (m_hWnd == NULL || m_hDesktopOwner == NULL || !::IsWindow(m_hDesktopOwner))
        return FALSE;

    // 当前 Owner 必须仍是桌面窗口，否则无需处理
    HWND hNow = (HWND)::GetWindowLongPtr(m_hWnd, GWLP_HWNDPARENT);
    if (hNow != m_hDesktopOwner)
        return FALSE;

    // 临时解除：恢复原始 Owner（或置空）
    HWND hSuspend = (m_bOriginalOwnerValid && m_hOriginalOwner != NULL &&
                     ::IsWindow(m_hOriginalOwner))
                    ? m_hOriginalOwner : NULL;
    ::SetWindowLongPtr(m_hWnd, GWLP_HWNDPARENT, (LONG_PTR)hSuspend);
    m_bOwnerSuspended = TRUE;
    return TRUE;
}

// 鼠标交互结束后恢复桌面 Owner（WitchDrawer 的
// RestoreDesktopOwnershipAfterMouseInput）：重新挂接桌面 Owner，再按需压回
// 最底（CheckHostWindow 内部有"已最底/完全可见"短路，不会无谓 SetWindowPos
// 造成闪烁）。幂等：未处于挂起状态时直接返回
void CDeskTidyWidget::RestoreDesktopOwnerAfterMouse()
{
    if (!m_bOwnerSuspended)
        return;
    m_bOwnerSuspended = FALSE;

    if (!SetDesktopOwner())
        return;     // 恢复失败（explorer 重启间隙等）：交给 MaintainZOrder 重试

    // 交互期间窗口可能被其它窗口盖住，恢复后压回最底，保持"最底层"语义
    CheckHostWindow();
}

// 修复 Progman（及其 WorkerW 桌面 Owner）的 last-active-popup 指针
// （WitchDrawer 的 RepairShellLastActivePopup / RepairLastActivePopup）：
// 若 popup 指向本进程的小窗口（上次点击残留），说明下次 Win+D 会把它激活
// 到前台。修复方法：临时 SetForegroundWindow(桌面窗口) 再立即恢复原前台
// 窗口——Windows 在窗口变为前台时会把其 last-active-popup 重置为它自身，
// popup 指针即被修复回桌面窗口。由 MaintainZOrder 低频节流调用
void CDeskTidyWidget::RepairShellLastActivePopup()
{
    // WorkBuddy: 显示桌面（Win+D）期间跳过本修复。此时小窗口本就由 ShellHook
    // 提顶，且不应被置为前台；每 5 秒的 SetForegroundWindow(桌面) 抢前台反而会让
    // 分层窗口重新呈现而闪动。退出显示桌面后恢复正常巡检
    if (m_bShowDesktop)
        return;

    // 节流：约 5 秒一次（SetForegroundWindow 有系统级副作用，不宜过频）
    DWORD dwNow = ::GetTickCount();
    if (m_dwLastPopupRepair != 0 &&
        (DWORD)(dwNow - m_dwLastPopupRepair) < 5000)
        return;
    m_dwLastPopupRepair = dwNow;

    HWND hShell = ::GetShellWindow();   // 桌面根窗口（Progman）
    if (hShell == NULL || !::IsWindow(hShell))
        return;

    // 主 Shell 与桌面 Owner（Win10 下可能是图标层 WorkerW）分别修复
    // 使用局部函数对象避免重复代码
    auto RepairOne = [](HWND hDesktop) -> BOOL {
        if (hDesktop == NULL || !::IsWindow(hDesktop))
            return FALSE;

        HWND hPopup = ::GetLastActivePopup(hDesktop);
        // popup 为空 / 就是桌面自身 / 已销毁 → 无需修复
        if (hPopup == NULL || hPopup == hDesktop || !::IsWindow(hPopup))
            return FALSE;

        // 只修复"指向本进程窗口"的 popup（其它进程的 popup 由对方自己负责）
        DWORD dwPid = 0;
        ::GetWindowThreadProcessId(hPopup, &dwPid);
        if (dwPid != ::GetCurrentProcessId())
            return FALSE;

        // 临时把桌面设为前台（重置其 last-active-popup），随后恢复原前台
        HWND hPrevFg = ::GetForegroundWindow();
        if (!::SetForegroundWindow(hDesktop))
            return FALSE;
        if (hPrevFg != NULL && hPrevFg != hDesktop && ::IsWindow(hPrevFg))
            ::SetForegroundWindow(hPrevFg);
        return TRUE;
    };

    BOOL bRepaired = RepairOne(hShell);
    if (m_hDesktopOwner != NULL && m_hDesktopOwner != hShell)
        bRepaired = RepairOne(m_hDesktopOwner) || bRepaired;

    if (bRepaired)
        LogZ(_T("RepairShellLastActivePopup: 已修复桌面 last-active-popup"));
}

// 层级维护（仅最底层模式生效）。
// 由主对话框通过 SetWinEventHook 事件驱动调用：监听到
// EVENT_SYSTEM_FOREGROUND / EVENT_OBJECT_SHOW 事件（前台窗口变化、
// 新窗口弹出等都可能导致 z 序改变）后，主对话框逐个调用本函数。
// WorkBuddy: 当前置底模式为"普通顶层窗口压底"，不再挂接桌面，
//   因此走下面的压底分支（内部先检查下方是否已无普通窗口，若已在最底
//   则直接返回，避免无意义的重复压底造成闪烁）。
//   开头的挂接分支因 m_bAttached 恒为 FALSE 不会进入，保留是为了兼容
//   SetBottomLayer_bk() 手动启用挂接时的维护需求。
void CDeskTidyWidget::CheckHostWindow()
{
    // 仅最底层模式需要维护层级
    if (m_nLayerMode != 0)
        return;

    // WorkBuddy: 显示桌面（Win+D）期间禁止压底。此时系统把桌面窗口抬到 z 序
    // 顶部，HWND_BOTTOM 会把小窗口压到抬顶的桌面之下 → 表现为"消失"。
    // m_bShowDesktop 由 ShellHook 确定性置位；此期间小窗口由 ShellHook 提顶
    if (m_bShowDesktop)
        return;

    // ---- 挂接模式：验证并修复挂接（当前逻辑下不会进入）----
    if (m_bAttached)
    {
        // 宿主仍有效且仍是小窗口的父窗口 → 挂接正常，无需任何操作
        if (m_hHost != NULL && ::IsWindow(m_hHost) &&
            ::GetParent(m_hWnd) == m_hHost)
            return;

        // 挂接失效（宿主被重建等）：清除状态后重新挂接
        m_bAttached = FALSE;
        m_hHost = NULL;
        AttachToDesktop();
        return;
    }

    // ---- 兜底模式：普通顶层窗口压底 ----

    // 窗口完全可见（未被任何其它窗口遮挡）时无需压底：
    // 此时无论 z 序如何，视觉上窗口都直接呈现在最上层可见区域，
    // 压底与否没有视觉差异；跳过压底可避免无谓的 SetWindowPos
    // 重绘副作用（这是小窗口持续闪烁的重要来源）
    if (IsCompletelyVisible())
        return;

    // 从本窗口往下（GW_HWNDNEXT）检查是否存在"普通窗口"：
    // 只有桌面根窗口（Progman）、桌面工作区窗口（WorkerW）与任务栏
    // （Shell_TrayWnd，视为桌面的一部分）在下方时，才认为已经处于
    // 最底层，无需重新压底
    HWND hBelow = ::GetWindow(m_hWnd, GW_HWNDNEXT);
    while (hBelow != NULL)
    {
        TCHAR szClass[64] = {};
        ::GetClassName(hBelow, szClass, _countof(szClass));
        if (_tcsicmp(szClass, _T("Progman")) != 0 &&
            _tcsicmp(szClass, _T("WorkerW")) != 0 &&
            _tcsicmp(szClass, _T("Shell_TrayWnd")) != 0)
            break;      // 下方存在普通窗口，需要重新压底
        hBelow = ::GetWindow(hBelow, GW_HWNDNEXT);
    }
    if (hBelow == NULL)
        return;         // 已处于最底层，无需重复压底（避免闪烁）

    // 重新压到 z 序最底（不移动、不改变大小、不激活）
    ::SetWindowPos(m_hWnd, HWND_BOTTOM, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// 判断系统是否处于"显示桌面"模式（详见头文件声明）。
// 原理（全局视角，不受本小窗口 z 序影响）：
//   桌面模式 = 存在某个桌面窗口（Progman/WorkerW）被系统抬到 z 序顶部，
//   其上方没有普通窗口（只剩其它桌面窗口/任务栏等系统窗口）。
//   普通模式 = 所有桌面窗口都在 z 序底部，上方必然存在普通窗口。
// 注意排除两类窗口：
//   1. 本进程自己的全部窗口（所有小窗口/主界面）：系统里通常有多个 Widget，
//      Win+D 后它们都会被提到桌面之上；若只排除调用者自身，其它 Widget 位于
//      桌面之上时会被误判为"普通窗口"，导致桌面模式判定翻转、形成
//      "提顶→压底"无限循环（窗口闪烁/消失）——按进程判断可一并排除；
//   2. 任务栏等系统窗口（Shell_TrayWnd 始终在 z 序顶部，不算普通窗口）。
BOOL CDeskTidyWidget::IsDesktopMode()
{
    // 回调上下文：判定结果（进程判断取代了原先的"排除自身"）
    struct Ctx
    {
        BOOL bTop;    // 是否检测到桌面窗口位于 z 序顶部
    } ctx = { FALSE };

    // 枚举所有顶层窗口，找"上方没有普通窗口"的桌面窗口
    ::EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        Ctx* pCtx = (Ctx*)lParam;

        // 仅处理桌面窗口（Progman 根窗口 / WorkerW 桌面工作区窗口）
        TCHAR szClass[64] = {};
        ::GetClassName(hwnd, szClass, _countof(szClass));
        if (_tcsicmp(szClass, _T("Progman")) != 0 &&
            _tcsicmp(szClass, _T("WorkerW")) != 0)
            return TRUE;    // 非桌面窗口：继续枚举

        // 从该桌面窗口向上（GW_HWNDPREV = z 序更高）遍历：
        // 若上方出现"其它程序的普通窗口"，说明该桌面窗口不在顶部，
        // 继续检查下一个桌面窗口
        HWND h = ::GetWindow(hwnd, GW_HWNDPREV);
        while (h != NULL)
        {
            TCHAR szCls2[64] = {};
            ::GetClassName(h, szCls2, _countof(szCls2));
            DWORD dwPid = 0;
            ::GetWindowThreadProcessId(h, &dwPid);

            // 跳过两类窗口（都不是"其它程序窗口"，不应使桌面模式判定失效）：
            //   1. 本进程自己的窗口：Win+D 后所有小窗口都被提到桌面之上，
            //      它们位于桌面窗口上方但属于本应用，必须排除——若只排除
            //      调用者自身，其它小窗口会互相把对方误判为普通窗口，
            //      造成"提顶→压底"抖动循环（多 Widget 时必现）；
            //   2. 系统窗口：桌面（Progman/WorkerW）与任务栏（含副屏）
            if (dwPid == ::GetCurrentProcessId() ||
                _tcsicmp(szCls2, _T("Progman")) == 0 ||
                _tcsicmp(szCls2, _T("WorkerW")) == 0 ||
                _tcsicmp(szCls2, _T("Shell_TrayWnd")) == 0 ||
                _tcsicmp(szCls2, _T("Shell_SecondaryTrayWnd")) == 0)
            {
                h = ::GetWindow(h, GW_HWNDPREV);
                continue;
            }

            // 上方存在其它程序的普通窗口 → 该桌面窗口在底部 → 非桌面模式
            return TRUE;    // 继续检查下一个桌面窗口
        }

        // 该桌面窗口上方没有普通窗口（只剩系统窗口/本应用小窗口）→
        // 桌面被抬到 z 序顶部 → "显示桌面"模式
        pCtx->bTop = TRUE;
        return FALSE;           // 已确认，停止枚举
    }, (LPARAM)&ctx);

    return ctx.bTop;
}

// z 序综合维护（仅最底层模式生效，详见头文件声明）。
// WitchDrawer 桌面 Owner 模式下：小窗口是桌面 Shell 窗口（Progman/WorkerW）
// 的 owned 窗口——"显示桌面"（Win+D / 任务栏按钮）不会最小化它；且 owned
// 窗口始终位于 Owner 之上，桌面被系统抬升时小窗口跟随抬升，天然可见，
// 无需再做"提顶/压底"维护；只需确保 Owner 挂接有效（Explorer 重启重建
// 桌面后重新挂接）。
// Owner 暂不可用的兜底模式下：沿用"显示桌面"检测 → 提顶/压底逻辑。
// 由主对话框 WinEvent 事件驱动与本窗口低频定时器（WIDGET_ZORDER_TIMER）
// 共同调用。
// WorkBuddy: 设置 z 序维护开关（详见头文件声明）。
// 立即生效：开启则（重新）启动定时器，关闭则停掉定时器。
// 关闭时顺带把"桌面模式"状态记忆清零——否则下次开启时会沿用过期状态，
// 可能一上来就误判为"仍处于桌面模式"而白白提顶一次
void CDeskTidyWidget::SetZOrderMaintain(BOOL bEnable)
{
    m_bZOrderMaintain = (bEnable != FALSE);

    // 窗口尚未创建：不碰定时器，OnCreate 会按该值决定是否启动
    if (m_hWnd == NULL)
        return;

    if (m_bZOrderMaintain)
    {
        m_bDesktopCovered  = FALSE;
        m_nNotCoveredCount = 0;
        m_bRaisedAboveDesktop = FALSE;   // WorkBuddy: 一并清零，避免沿用过期状态
        // 同一 ID 重复 SetTimer 只会重置计时周期，不会创建多个定时器
        SetTimer(WIDGET_ZORDER_TIMER, WIDGET_ZORDER_INTERVAL, NULL);
    }
    else
    {
        // WorkBuddy: 桌面子窗口方案下保留定时器——挂载失败自动重试
        //（TryReattachDesktopChild）依赖它；重试成功后 MaintainZOrder
        // 依旧被开关短路，不会产生任何 z 序操作与日志
        if (!(m_nLayerMode == 0 && m_bBottomViaChild))
            KillTimer(WIDGET_ZORDER_TIMER);
    }
}

void CDeskTidyWidget::MaintainZOrder()
{
    // WorkBuddy: z 序维护总开关（设置界面可关闭）。
    // 必须放在函数最前面——紧随其后的就是每秒一次的诊断日志，
    // 若把开关判断放到日志之后，关闭状态下日志依旧每秒刷屏，开关只做了一半
    if (!m_bZOrderMaintain)
        return;

    // WorkBuddy: 模态子对话框（透明度选择等）打开期间整体暂停层级维护。
    // 原因：本函数末尾会调用 RepairShellLastActivePopup()，其内部执行
    //   SetForegroundWindow(桌面) → SetForegroundWindow(原前台)
    // 这种"抢前台"会把正在使用对话框的用户焦点反复夺走——用户在对话框上
    // 的第一次点击会被"窗口重新激活"消耗掉（点"确定/取消"看起来毫无反应）。
    // 期间也无须维护置底/恢复显示：对话框是模态的，用户正在操作本窗口。
    // 此处不加日志，避免每秒刷屏；对话框关闭后本函数立即恢复正常工作
    if (m_bModalChildOpen)
        return;

    // WorkBuddy: 桌面子窗口方案（置底且开启"作为桌面子窗口"）：小窗口已是
    // 桌面内容窗口的子窗口，天然位于所有普通窗口之下且随桌面一起被系统抬升，
    // 无需任何 z 序维护（显示桌面兜底/压底/抢前台全部跳过）。直接返回，
    // 避免每秒无谓操作与日志刷屏
    if (m_nLayerMode == 0 && m_bBottomViaChild)
        return;

    // ---- 无条件诊断日志 ----
    // 每次维护调用都记录窗口关键状态，用于确认维护逻辑是否执行
    // 以及"显示桌面"后各状态如何变化（定位消失问题）
    WINDOWPLACEMENT wp = { sizeof(wp) };
    ::GetWindowPlacement(m_hWnd, &wp);
    LogZ(_T("Maint: attached=%d visible=%d iconic=%d showCmd=%d covered=%d coveredCnt=%d byDesktop=%d"),
         m_bAttached, ::IsWindowVisible(m_hWnd), ::IsIconic(m_hWnd), (int)wp.showCmd,
         m_bDesktopCovered, m_nNotCoveredCount, (int)IsCoveredByDesktop());

    // 仅最底层模式需要维护 z 序
    if (m_nLayerMode != 0)
        return;

    // ---- 兜底恢复 1：窗口被系统隐藏（不是用户主动隐藏）----
    // 若用户并未主动隐藏（m_bUserVisible 为 TRUE）但窗口当前不可见，
    // 说明被系统处理了（如 Explorer 重建桌面瞬间）：恢复显示。
    // 用节流（2 秒）防止反复恢复
    if (m_bUserVisible && !::IsWindowVisible(m_hWnd))
    {
        DWORD dwNow = ::GetTickCount();
        if (m_dwLastRestore == 0 || (DWORD)(dwNow - m_dwLastRestore) > 2000)
        {
            m_dwLastRestore = dwNow;
            LogZ(_T("MaintainZOrder: 窗口不可见，恢复显示"));
            ::ShowWindow(m_hWnd, SW_SHOWNA);   // 显示但不抢占焦点
        }
    }

    // ---- 兜底恢复 2：窗口处于最小化状态 ----
    // 子窗口不会收到最小化，此分支主要供未挂接的兜底模式使用：
    // 用 GetWindowPlacement 检测最小化状态，用最小化前矩形放回原位
    // 再恢复显示，避免窗口"恢复"到屏幕外（-32000,-32000）仍然不可见
    if (m_bUserVisible && wp.showCmd == SW_SHOWMINIMIZED)
    {
        LogZ(_T("MaintainZOrder: 检测到最小化状态，恢复显示"));
        if (m_rcLast.Width() > 0 && m_rcLast.Height() > 0)
        {
            ::SetWindowPos(m_hWnd, NULL, m_rcLast.left, m_rcLast.top,
                           m_rcLast.Width(), m_rcLast.Height(),
                           SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ::ShowWindow(m_hWnd, SW_SHOWNOACTIVATE);
    }

    // ---- 修复桌面 last-active-popup ----
    // WitchDrawer 方案：周期性把 Progman（及桌面 Owner）的 last-active-popup
    // 指针重置回桌面自身，防止"显示桌面"（Win+D）把最近点击过的小窗口
    // 激活到前台（内部自带约 5 秒节流）
    RepairShellLastActivePopup();

    // ---- WorkBuddy: 兜底恢复 3：被桌面窗口遮挡（Win+D 后消失的直接原因）----
    // 必须放在下方"桌面 Owner 有效 → 直接 return"之前：Owner 机制只能保证
    // 小窗口位于**它的 Owner** 之上，而 Win+D 时系统抬到顶部的可能是另一个
    // 桌面窗口，此时 owned 跟随不生效，小窗口就被桌面层盖住看不见了
    EnsureAboveDesktop();

    // ---- WitchDrawer 桌面 Owner 机制：保证 Win+D 下依然可见 ----
    // Owner 挂接成功时：小窗口是桌面 Shell 窗口（Progman/WorkerW）的 owned
    // 窗口——"显示桌面"不会最小化它；且 owned 窗口始终位于 Owner 之上，
    // 桌面被系统抬升时小窗口跟随抬升（Win+D 时桌面必然抬到顶部），因此
    // 无需再做"提顶/压底"维护，只需保证挂接有效（Explorer 重启重建桌面
    // 后 Owner 会失效，这里重新挂接）
    if (m_hDesktopOwner != NULL)
    {
        // 挂接仍有效：Owner 窗口存活，且小窗口的 Owner 仍是它
        if (::IsWindow(m_hDesktopOwner) &&
            (HWND)::GetWindowLongPtr(m_hWnd, GWLP_HWNDPARENT) == m_hDesktopOwner)
            return;

        // 挂接失效（桌面被重建等）：清除状态后重新挂接
        m_hDesktopOwner = NULL;
        m_bOwnerSuspended = FALSE;
        if (SetDesktopOwner())
            return;     // 重新挂接成功，无需提顶/压底

        // 重新挂接失败（explorer 重启间隙等）：落到下方提顶/压底兜底
        LogZ(_T("MaintainZOrder: 桌面 Owner 重新挂接失败，走提顶/压底兜底"));
    }
    else
    {
        // 从未挂接成功过（SetBottomLayer 时桌面暂不可用等）：再试一次；
        // 成功则直接返回（无需提顶/压底）
        if (SetDesktopOwner())
            return;
    }

    // ---- 置底兜底：沿用"显示桌面"提顶/压底逻辑 ----
    // "显示桌面"模式：桌面（Progman/WorkerW）被系统抬到小窗口上方 →
    // 把小窗口提到桌面之上，满足"显示桌面时依然显示"。此时其它窗口
    // 均被桌面盖住不可见，提到最顶无视觉冲突；任务栏等系统窗口仍在
    // 置顶层，会正常盖住小窗口的重叠区域
    if (IsDesktopMode() || m_bShowDesktop)
    {
        // 只要检测到桌面盖住小窗口，就视为仍处于桌面模式
        m_nNotCoveredCount = 0;
        // 仅在"非桌面模式 → 桌面模式"翻转时提顶一次（状态记忆），
        // 避免每周期重复提顶造成闪烁
        if (!m_bDesktopCovered)
        {
            m_bDesktopCovered = TRUE;
            LogZ(_T("MaintainZOrder: 检测到桌面模式，提到桌面之上"));
            ::SetWindowPos(m_hWnd, HWND_TOP, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return;
    }

    // ---- 普通模式 ----
    if (m_bDesktopCovered)
    {
        // 提顶后需连续 3 次（约 3 秒）检测不到桌面盖住，才确认
        // "显示桌面"已退出（防止提顶/压底抖动）；期间保持提顶
        if (++m_nNotCoveredCount < 3)
        {
            LogZ(_T("MaintainZOrder: 桌面模式未确认退出，保持提顶 (%d/3)"),
                 m_nNotCoveredCount);
            return;
        }
        m_bDesktopCovered = FALSE;
        m_nNotCoveredCount = 0;
        LogZ(_T("MaintainZOrder: 桌面模式结束，压回最底"));
    }
    // 压回 z 序最底（CheckHostWindow 内部有"已最底/完全可见"短路，不会闪烁）
    CheckHostWindow();
}

// 判断窗口是否完全可见（未被任何其它窗口遮挡）。
// 方法：在窗口客户区四角（各向内缩 3 像素，避免采样点落在边框外
// 被误判为被遮挡）与中心共采样 5 个屏幕坐标点，逐个用 WindowFromPoint
// 查询该点最上层的窗口；若 5 个点全部命中本窗口自身或其子窗口
// （列表控件等），说明没有其它窗口盖住本窗口，即完全可见。
// 用于 CheckHostWindow 的短路判断：完全可见时压不压底视觉无差异。
BOOL CDeskTidyWidget::IsCompletelyVisible()
{
    CRect rcClient;
    GetClientRect(&rcClient);
    if (rcClient.Width() <= 0 || rcClient.Height() <= 0)
        return TRUE;    // 空窗口视为完全可见，无需压底

    // 5 个采样点：四角（向内缩 3 像素）+ 中心
    CPoint pts[5];
    pts[0] = CPoint(3, 3);
    pts[1] = CPoint(rcClient.right - 3, 3);
    pts[2] = CPoint(3, rcClient.bottom - 3);
    pts[3] = CPoint(rcClient.right - 3, rcClient.bottom - 3);
    pts[4] = rcClient.CenterPoint();

    for (int i = 0; i < 5; i++)
    {
        CPoint ptScreen = pts[i];
        ClientToScreen(&ptScreen);

        // 查询该屏幕点最上层的窗口：命中本窗口自身或其子窗口即视为"未被遮挡"。
        // 注意 ::IsChild 只对"真子窗口"返回 TRUE（不包含自身），所以需单独比较自身
        HWND hTop = ::WindowFromPoint(ptScreen);
        if (hTop == NULL || (hTop != m_hWnd && !::IsChild(m_hWnd, hTop)))
            return FALSE;   // 该点被其它窗口盖住：窗口未完全可见
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// WorkBuddy: "显示桌面（Win+D）后小窗口消失"的定向修复
// ---------------------------------------------------------------------------

// 判断本窗口是否被"桌面窗口"遮挡。
// 方法：在客户区四角（各向内缩 3 像素）与中心采样 5 个屏幕点，逐点用
// WindowFromPoint 取最上层窗口并归一到顶层祖先（GetAncestor GA_ROOT）；
// 只要任一点命中桌面类窗口（Progman / WorkerW，含其子窗口 SHELLDLL_DefView）
// 即视为被桌面盖住。
// 与 IsCompletelyVisible() 的区别：后者只回答"是否被遮挡"，本函数进一步
// 回答"遮挡者是不是桌面"——被普通窗口盖住是置底模式的正常语义，被桌面
// 盖住则一定异常（置底语义要求小窗口位于"所有普通窗口之下、桌面之上"）
BOOL CDeskTidyWidget::IsCoveredByDesktop()
{
    if (m_hWnd == NULL || !::IsWindow(m_hWnd))
        return FALSE;

    CRect rcClient;
    GetClientRect(&rcClient);
    if (rcClient.Width() <= 0 || rcClient.Height() <= 0)
        return FALSE;

    // 5 个采样点：四角（向内缩 3 像素）+ 中心（与 IsCompletelyVisible 一致）
    CPoint pts[5];
    pts[0] = CPoint(3, 3);
    pts[1] = CPoint(rcClient.right - 3, 3);
    pts[2] = CPoint(3, rcClient.bottom - 3);
    pts[3] = CPoint(rcClient.right - 3, rcClient.bottom - 3);
    pts[4] = rcClient.CenterPoint();

    for (int i = 0; i < 5; i++)
    {
        CPoint ptScreen = pts[i];
        ClientToScreen(&ptScreen);

        HWND hTop = ::WindowFromPoint(ptScreen);
        if (hTop == NULL || hTop == m_hWnd || ::IsChild(m_hWnd, hTop))
            continue;                   // 命中自身/子控件：该点未被遮挡

        // 归一到顶层祖先：桌面图标层 SHELLDLL_DefView 是 WorkerW/Progman 的
        // 子窗口，必须归一后才能按类名判定
        HWND hRoot = ::GetAncestor(hTop, GA_ROOT);
        if (hRoot == NULL)
            hRoot = hTop;

        TCHAR szClass[64] = {};
        if (::GetClassName(hRoot, szClass, _countof(szClass)) == 0)
            continue;
        if (_tcsicmp(szClass, _T("Progman")) == 0 ||
            _tcsicmp(szClass, _T("WorkerW")) == 0)
            return TRUE;                // 被桌面（壁纸层/图标层）盖住
    }
    return FALSE;
}


// 保证"小窗口始终位于桌面之上"（置底模式的核心不变式）。
// 为什么需要它：WitchDrawer 桌面 Owner 机制只能保证小窗口位于**它的 Owner**
//（Progman 或图标层 WorkerW）之上；而 Windows 在"显示桌面"（Win+D）时把桌面
// 抬到 z 序顶部用的可能是**另一个**桌面窗口（常见于壁纸层 WorkerW），此时
// owned 跟随不生效，小窗口被抬顶的桌面层盖住 → 表现为"Win+D 后小窗口消失"。
// 日志也佐证了这一点：全程 visible=1、showCmd=1，从未收到 SIZE_MINIMIZED，
// 即窗口并没有被最小化，只是被盖住了。
// 处理（本函数作为 ShellHook 的冗余兜底；主修复见 OnShellHook）：
//   1) m_bShowDesktop 为 TRUE（ShellHook 已确认处于显示桌面）→ 直接保持提顶；
//   2) 否则若被桌面盖住、或 IsDesktopMode 判定为显示桌面且被遮挡 → 提到
//      HWND_TOP（显示桌面时桌面为空，HWND_TOP = 位于桌面之上，方向正确）；
//   3) 之前被抬过、现在遮挡解除 → 压回 HWND_BOTTOM 恢复"位于所有普通窗口
//      之下"的置底语义。关键：绝不可在显示桌面期间用 HWND_BOTTOM 压底
//      （会把小窗口压到抬顶的桌面之下 → 消失，正是上一版 bug 的根因）。
// 由 MaintainZOrder 每周期调用，且位于"桌面 Owner 有效 → 直接 return"之前，
// 因此无论 Owner 挂接是否成功都会生效
void CDeskTidyWidget::EnsureAboveDesktop()
{
    if (m_hWnd == NULL || !::IsWindow(m_hWnd))
        return;
    if (m_nLayerMode != 0)                  // 仅最底层模式需要本维护
        return;
    if (!::IsWindowVisible(m_hWnd))         // 用户主动隐藏：不干预
        return;

    // WorkBuddy: 已被 ShellHook 明确标记为"显示桌面"模式时，无需再轮询采样，
    // 直接保持提顶即可；此时桌面为空（无普通窗口），HWND_TOP = 位于桌面之上。
    // 绝不能用 HWND_BOTTOM——系统把桌面抬到 z 序顶部时 HWND_BOTTOM 会把本窗口
    // 压到抬顶的桌面之下，反而"消失"（这正是上一版 bug 的根因）
    if (m_bShowDesktop)
    {
        // WorkBuddy: 已处于最顶（m_bRaisedAboveDesktop 记忆）则不再重复
        // SetWindowPos——否则每秒一次的无谓 z 序变更会迫使 DWM 重新呈现分层
        // 窗口，表现为"显示桌面时小窗口周期性闪动"。仅状态翻转时提顶一次
        if (!m_bRaisedAboveDesktop)
        {
            ::SetWindowPos(m_hWnd, HWND_TOP, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            m_bRaisedAboveDesktop = TRUE;
        }
        return;
    }

    BOOL bCoveredByDesktop = IsCoveredByDesktop();

    // 抬升条件：被桌面盖住；或处于"显示桌面"模式但被遮挡
    //（后者覆盖遮挡者不是桌面类窗口的情况，如某些系统的桌面层实现）
    BOOL bNeedRaise = bCoveredByDesktop;
    if (!bNeedRaise && IsDesktopMode() && !IsCompletelyVisible())
        bNeedRaise = TRUE;

    if (bNeedRaise)
    {
        // 置于所有窗口最前：显示桌面时桌面为空，HWND_TOP 即"位于桌面之上"；
        // 普通模式被遮挡时提顶也无害（CheckHostWindow 随后会压回置底）。
        // 修正上一版 bug：曾误用 SetWindowPos(m_hWnd, hTopDesktop) 把本窗口
        // 放到桌面窗口"之下"，方向完全相反，导致越抬越被盖住
        ::SetWindowPos(m_hWnd, HWND_TOP, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        m_bRaisedAboveDesktop = TRUE;
        LogZ(_T("EnsureAboveDesktop: 被桌面遮挡(covered=%d)，已提顶(置于桌面之上)"),
             (int)bCoveredByDesktop);
        return;
    }

    // 不再被桌面盖住：若之前被抬过，压回最底恢复置底语义。
    // 此时已不在显示桌面（m_bShowDesktop 为假），HWND_BOTTOM 正确
    //（桌面回到 z 序底部，"最底"= 位于桌面之上、普通窗口之下）
    if (m_bRaisedAboveDesktop)
    {
        m_bRaisedAboveDesktop = FALSE;
        ::SetWindowPos(m_hWnd, HWND_BOTTOM, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        LogZ(_T("EnsureAboveDesktop: 桌面遮挡解除，压回最底"));
    }
}

// WorkBuddy: 处理系统 Shell 钩子消息（OnCreate 中 RegisterShellHookWindow 注册）。
// 用途：确定性地捕获"显示桌面"（Win+D / 任务栏"显示桌面"按钮）事件，
// 替代原先脆弱的 z 序轮询（IsDesktopMode 在 Win+D 时误判，导致小窗口被压到
// 桌面之下而消失）。
//   HSHELL_ACTIVATESHELLWINDOW：系统激活桌面 Shell，即"显示桌面"动作。无论进入
//     还是退出都会触发，我们把它当作"进入 / 仍在显示桌面"的乐观信号——进入即把
//     小窗口提到最前（HWND_TOP）；此时桌面为空（无普通窗口），HWND_TOP 即"位于
//     桌面之上"，必然可见。
//   HSHELL_WINDOWACTIVATED / HSHELL_RUDEAPPACTIVATED：某窗口被激活。若之前处于
//     显示桌面且本次激活的是"普通程序窗口"（非桌面/任务栏/自身），即判定"退出
//     显示桌面"，把小窗口压回最底（HWND_BOTTOM）恢复置底语义。用"普通窗口激活"
//     而非再次依赖 HSHELL_ACTIVATESHELLWINDOW 来区分进出，避免该事件成对触发
//     （进入、退出各触发一次）造成状态错位
LRESULT CDeskTidyWidget::OnShellHook(WPARAM wParam, LPARAM lParam)
{
    if (m_nLayerMode != 0 || m_bBottomViaChild)  // 仅最底层且非桌面子窗口方案
        return 0;                                 // 才需处理显示桌面（子窗口由
                                                 // 桌面自身托管，无需干预）
    if (!m_bUserVisible || !::IsWindowVisible(m_hWnd))
        return 0;                          // 用户主动隐藏：不干预

    switch (wParam)
    {
    case HSHELL_ACTIVATESHELLWINDOW:
        // 进入 / 仍在"显示桌面"：立即把小窗口提到桌面之上（可见）。
        // 用 m_bShowDesktop 记忆状态，供 MaintainZOrder / CheckHostWindow 统一决策
        m_bShowDesktop = TRUE;
        m_bRaisedAboveDesktop = TRUE;   // 记忆：已进入最顶，避免每周期重复 SetWindowPos
        ::SetWindowPos(m_hWnd, HWND_TOP, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        LogZ(_T("ShellHook: 显示桌面激活，已提顶（置于桌面之上）"));
        break;

    case HSHELL_WINDOWACTIVATED:
    case HSHELL_RUDEAPPACTIVATED:
        {
            HWND hActivated = (HWND)lParam;
            // 仅当之前处于"显示桌面"且现在激活的是普通程序窗口时，判定退出
            if (m_bShowDesktop && hActivated != NULL && hActivated != m_hWnd)
            {
                TCHAR szClass[64] = {};
                ::GetClassName(hActivated, szClass, _countof(szClass));
                // 排除桌面与任务栏自身被激活（这些不算"退出显示桌面"）
                if (_tcsicmp(szClass, _T("Progman")) != 0 &&
                    _tcsicmp(szClass, _T("WorkerW")) != 0 &&
                    _tcsicmp(szClass, _T("Shell_TrayWnd")) != 0 &&
                    _tcsicmp(szClass, _T("Shell_SecondaryTrayWnd")) != 0)
                {
                    m_bShowDesktop = FALSE;
                    m_bRaisedAboveDesktop = FALSE;  // 复位：下次进入显示桌面可再次提顶
                    // 退出显示桌面：压回最底，恢复"位于所有普通窗口之下"的置底语义
                    //（此时桌面已回到 z 序底部，HWND_BOTTOM 正确，不会压到桌面之下）
                    ::SetWindowPos(m_hWnd, HWND_BOTTOM, 0, 0, 0, 0,
                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    LogZ(_T("ShellHook: 退出显示桌面（普通窗口激活），已压回最底"));
                }
            }
        }
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 目录 / 视图 / 刷新
// ---------------------------------------------------------------------------

void CDeskTidyWidget::SetDirectory(const CString& strDir)
{
    // 记录新目录并立即刷新文件列表
    m_strDir = strDir;
    RefreshFiles();
}

void CDeskTidyWidget::SetViewMode(int nMode)
{
    m_nViewMode = nMode;
    if (m_list.GetSafeHwnd() == NULL)
        return;

    ApplyViewMode();
}

// 设置标题栏颜色：记录后立即重新合成分层位图（标题文字/按钮图标按新底色自适应）。
// 已在窗口上时触发重绘；未创建时仅记录，创建后渲染时自然采用本值
void CDeskTidyWidget::SetHeaderColor(COLORREF clrHeader)
{
    m_clrHeader = clrHeader;
    if (m_hWnd != NULL)
        RenderLayered();   // 重新合成：标题条 + 边框 + 标题文字均需按新底色更新
}

// WorkBuddy: 设置标题栏透明度（0~255）。与背景透明度独立——标题栏区域按本值合成
void CDeskTidyWidget::SetHeaderAlpha(BYTE byAlpha)
{
    m_byHeaderAlpha = byAlpha;
    // WorkBuddy: 诊断日志——确认设置值确实到达本对象（排查"设置不生效"）
    LogZ(_T("SetHeaderAlpha: alpha=%d(%.0f%%) hwnd=%p"),
         (int)byAlpha, byAlpha * 100.0 / 255.0, (void*)m_hWnd);
    if (m_hWnd != NULL)
        RenderLayered();
}

// WorkBuddy: 设置背景透明度（含文件列表区域，0~255）。与标题栏透明度独立
void CDeskTidyWidget::SetBgAlpha(BYTE byAlpha)
{
    m_byBgAlpha = byAlpha;
    // WorkBuddy: 诊断日志——确认设置值确实到达本对象（排查"设置不生效"）
    LogZ(_T("SetBgAlpha: alpha=%d(%.0f%%) hwnd=%p"),
         (int)byAlpha, byAlpha * 100.0 / 255.0, (void*)m_hWnd);
    if (m_hWnd != NULL)
        RenderLayered();
}

// 设置窗口背景色：记录后同步到列表控件（否则会被列表默认白底挡住）并重新合成
void CDeskTidyWidget::SetBgColor(COLORREF clrBg)
{
    m_clrBg = clrBg;
    if (m_hWnd != NULL)
    {
        // 列表控件覆盖标题条以下全部客户区，因此必须直接把背景色作用到列表
        // （背景、文字背景、文字色）；随后重新合成分层位图
        ApplyListColors();
        // 标题条以下的边缘留白与列表区由 RenderLayered 按 m_clrBg + 背景 alpha 合成
        RenderLayered();
    }
}

// WorkBuddy: 设置"文件名称颜色"（文件列表项文字 / 网格视图标签文字）。
//   bAuto = TRUE → 自动跟随背景（按 m_clrBg 亮度取黑/白）；
//   bAuto = FALSE → 固定使用 clrFixed。
// clrFixed 在任何模式下都会被记住，便于用户在两种模式间切换时不丢颜色。
// 设置后必须同步到列表控件并重新合成分层位图——列表是子控件，文字画在
// 它自己的 DC 上，父窗口重绘不会自动带上新颜色
void CDeskTidyWidget::SetTextColor(COLORREF clrFixed, BOOL bAuto)
{
    m_clrTextFixed = clrFixed;      // 无论哪种模式都记住该颜色
    m_bTextAuto    = bAuto;
    if (m_hWnd != NULL)
    {
        ApplyListColors();   // 背景/文字背景不变，只有文字色需要重新下发
        RenderLayered();     // 分层窗口：文件列表内容需重新抓取合成
    }
}

// 按背景颜色亮度返回自适应文字色：亮底用黑、暗底用白，保证标题文字与按钮图标可读。
// 采用 Rec.601 感知亮度（0.299R+0.587G+0.114B），阈值 150 区分明暗
COLORREF CDeskTidyWidget::GetContrastTextColor(COLORREF clr) const
{
    int r = GetRValue(clr);
    int g = GetGValue(clr);
    int b = GetBValue(clr);
    double lum = 0.299 * r + 0.587 * g + 0.114 * b;
    return (lum > 150.0) ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

// 把当前背景色应用到文件列表控件（及其列头），使"背景色"设置真正可见。
// 根因：小窗口背景由 OnPaint 填充 m_clrBg，但列表控件（m_list）覆盖标题条以下
// 的全部客户区，且父窗口绘制因 WS_CLIPCHILDREN 不会覆盖列表区，导致列表区域
// 始终显示列表控件自身的默认白底，m_clrBg 被完全挡住。
// 解决：直接把列表背景、文字背景设为 m_clrBg；列表项文字色取"名称颜色"配置——
// 自动模式下按背景亮度自适应黑/白，否则固定使用用户选定的颜色；
// 列头（报表视图"名称"列）背景也设为 m_clrBg，其文字色由 OnListCustomDraw
// 自定义绘制随亮度自适应（避免深色背景下黑色列名看不清）。最后触发列表重绘
void CDeskTidyWidget::ApplyListColors()
{
    if (m_list.GetSafeHwnd() == NULL)
        return;

    // WorkBuddy: 文件名颜色——"自动"时跟随背景亮度取黑/白（老行为），
    // 否则用用户选定的固定颜色。列头文字不受本值影响（仍自动），
    // 因为列头属于控件外框而非"文件名称"
    COLORREF clrText = m_bTextAuto ? GetContrastTextColor(m_clrBg) : m_clrTextFixed;
    m_list.SetBkColor(m_clrBg);      // 列表客户区背景
    m_list.SetTextBkColor(m_clrBg);  // 项/标签文字背景（与背景同色，融为一体）
    m_list.SetTextColor(clrText);    // 列表项文字（报表视图文件名 / 网格视图标签）

    // 列头背景色与文字色由 OnListCustomDraw 自定义绘制按 m_clrBg 设置
    // （CHeaderCtrl 无 SetBkColor，只能通过 NM_CUSTOMDRAW 的 clrTextBk 着色）
    m_list.Invalidate();   // 触发列表重绘以应用新背景色
}

// WorkBuddy: 把前景色向背景色按比例混合（nKeepPercent = 保留前景色的百分比，
// 0~100）。用于"已剪切待粘贴"列表项的置灰：文字向背景淡出，观感与资源管理器
// 里"剪切后待粘贴"的项一致（半透明感）
static COLORREF BlendColor(COLORREF clrFg, COLORREF clrBg, int nKeepPercent)
{
    if (nKeepPercent < 0)   nKeepPercent = 0;
    if (nKeepPercent > 100) nKeepPercent = 100;
    const int k = nKeepPercent, b = 100 - nKeepPercent;
    return RGB((GetRValue(clrFg) * k + GetRValue(clrBg) * b) / 100,
               (GetGValue(clrFg) * k + GetGValue(clrBg) * b) / 100,
               (GetBValue(clrFg) * k + GetBValue(clrBg) * b) / 100);
}

// 列表控件自定义绘制回调（NM_CUSTOMDRAW），两条职责：
//   1) 报表视图"列头"文字/背景色随 m_clrBg 自适应（CHeaderCtrl 无 SetBkColor，
//      列头背景只能通过 clrTextBk 着色）；
//   2) 列表项中处于"已剪切待粘贴"状态的项，文字向背景淡出（置灰）。
// 其余列表项放行默认绘制（文字色由 m_list.SetTextColor 统一着色）
void CDeskTidyWidget::OnListCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
    NMLVCUSTOMDRAW* pCD = reinterpret_cast<NMLVCUSTOMDRAW*>(pNMHDR);
    *pResult = CDRF_DODEFAULT;   // 默认：列表项按 SetTextColor 着色，列头默认绘制

    CHeaderCtrl* pHeader = m_list.GetHeaderCtrl();
    const BOOL bHeader = (pHeader != NULL && pCD->nmcd.hdr.hwndFrom == pHeader->m_hWnd);

    if (bHeader)
    {
        // ---- 列头：背景随 m_clrBg，文字随亮度自适应 ----
        if (pCD->nmcd.dwDrawStage == CDDS_PREPAINT)
        {
            // 需要进一步通知每个列头项的绘制前阶段，以便设置文字颜色
            *pResult = CDRF_NOTIFYITEMDRAW;
        }
        else if (pCD->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
        {
            pCD->clrTextBk = m_clrBg;
            pCD->clrText   = GetContrastTextColor(m_clrBg);
            *pResult = CDRF_NEWFONT;
        }
        return;
    }

    // ---- 列表项：仅处理"已剪切待粘贴"项的置灰 ----
    // 没有剪切项时完全不介入（不请求项级通知），避免无谓开销
    if (m_arrCutPaths.GetSize() == 0)
        return;

    if (pCD->nmcd.dwDrawStage == CDDS_PREPAINT)
    {
        *pResult = CDRF_NOTIFYITEMDRAW;   // 需要项级通知才能逐项着色
    }
    else if (pCD->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
    {
        const int nItem = (int)pCD->nmcd.dwItemSpec;
        if (nItem >= 0 && nItem < (int)m_arrPaths.GetCount() &&
            IsCutPath(m_arrPaths[nItem]))
        {
            const COLORREF clrText = m_bTextAuto ? GetContrastTextColor(m_clrBg)
                                                 : m_clrTextFixed;
            pCD->clrTextBk = m_clrBg;
            pCD->clrText   = BlendColor(clrText, m_clrBg, 40);  // 40% 原色 + 60% 背景
            *pResult = CDRF_NEWFONT;
        }
    }
}

// 折叠/展开小窗口：
//   折叠 - 记住当前完整矩形，把窗口缩到只剩标题栏高度，隐藏文件列表；
//   展开 - 恢复折叠前的完整大小并重新显示文件列表。
// 状态变化时通知主对话框保存配置（折叠状态 + 展开矩形）。
void CDeskTidyWidget::SetCollapsed(BOOL bCollapsed)
{
    // 状态未变化：无需处理（m_hWnd 为空时仅记录标志，创建窗口后自然生效）
    if (m_bCollapsed == bCollapsed)
        return;
    m_bCollapsed = bCollapsed;
    if (m_hWnd == NULL)
        return;

    CRect rcWnd;
    GetWindowRect(&rcWnd);

    if (bCollapsed)
    {
        // 折叠：记录展开矩形，窗口缩到标题条高度，隐藏列表。
        // WorkBuddy: 防污染（2026-09-13 "折叠启动→展开不恢复"）——仅当当前
        // 窗口高度明显大于标题条（即处于正常展开状态）时才更新 m_rcExpanded；
        // 若窗口本身已是折叠条尺寸（如 INI 矩形曾被污染成 24px 高），保留
        // 既有展开矩形不覆盖；仅当历史值完全无效时才退而记录当前矩形
        if (rcWnd.Height() > WIDGET_HEADER_HEIGHT + WIDGET_EDGE_SIZE ||
            m_rcExpanded.Width() <= 0 || m_rcExpanded.Height() <= 0)
        {
            m_rcExpanded = rcWnd;
        }
        LogZ(_T("SetCollapsed(TRUE): rc=%dx%d expanded=%dx%d"),
             rcWnd.Width(), rcWnd.Height(),
             m_rcExpanded.Width(), m_rcExpanded.Height());
        m_list.ShowWindow(SW_HIDE);
        ::SetWindowPos(m_hWnd, NULL, rcWnd.left, rcWnd.top,
                       rcWnd.Width(), WIDGET_HEADER_HEIGHT,
                       SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    }
    else
    {
        // 展开：恢复折叠前的完整矩形（若从未记录过则保持当前尺寸），
        // 折叠期间若被拖动过，展开位置跟随折叠条当前位置。
        // WorkBuddy: 兜底（2026-09-13）——展开矩形高度若不大于标题条+边缘
        //（被历史 bug 污染成折叠条尺寸，或从未有效记录），按默认展开高度
        // 补齐，保证"展开"动作永远能把窗口拉回可视大小，而不是原地保持
        // 24px 折叠条（宽度保留当前值，只补高度）
        CRect rcExp = (m_rcExpanded.Width() > 0 && m_rcExpanded.Height() > 0)
                          ? m_rcExpanded : rcWnd;
        if (rcExp.Height() <= WIDGET_HEADER_HEIGHT + WIDGET_EDGE_SIZE)
            rcExp.bottom = rcExp.top + WIDGET_DEFAULT_EXPAND_HEIGHT;
        rcExp.MoveToXY(rcWnd.left, rcWnd.top);
        LogZ(_T("SetCollapsed(FALSE): rc=%dx%d expanded=%dx%d -> %dx%d"),
             rcWnd.Width(), rcWnd.Height(),
             m_rcExpanded.Width(), m_rcExpanded.Height(),
             rcExp.Width(), rcExp.Height());
        m_list.ShowWindow(SW_SHOW);
        ::SetWindowPos(m_hWnd, NULL, rcExp.left, rcExp.top,
                       rcExp.Width(), rcExp.Height(),
                       SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    }

    // 通知主对话框保存配置（折叠状态 + 最新矩形）
    if (m_hNotify != NULL && ::IsWindow(m_hNotify))
        ::PostMessage(m_hNotify, WM_WIDGET_CHANGED, 0, 0);

    // 折叠/展开后按钮图标与窗口显示同步变化，重绘标题条
    InvalidateHeader();
}

// 设置用户可见意图（详见头文件声明）。
// 由主对话框托盘菜单"显示全部小窗口 / 隐藏全部小窗口"调用：
// 记录用户意图的同时，执行对应的显示/隐藏窗口操作。
// z 序维护（MaintainZOrder）据此区分"用户主动隐藏"与"系统级隐藏/
// 最小化"（如"显示桌面"）：只有用户主动隐藏时才不恢复窗口
void CDeskTidyWidget::SetUserVisible(BOOL bVisible)
{
    m_bUserVisible = bVisible;
    if (m_hWnd == NULL)
        return;

    if (bVisible)
        ::ShowWindow(m_hWnd, SW_SHOWNA);   // 显示但不抢占焦点（保持层级/折叠状态）
    else
        ::ShowWindow(m_hWnd, SW_HIDE);     // 用户主动隐藏
}

// 调试日志：格式化文本追加写入 exe 同目录 DeskTidy_zorder.log。
// 带时间戳、限制文件大小（超过 1MB 清空重写），用于复现与定位
// "显示桌面（Win+D）后小窗口消失"问题
void CDeskTidyWidget::LogZ(const TCHAR* pszFmt, ...)
{
    TCHAR szMsg[512] = {};
    va_list args;
    va_start(args, pszFmt);
    _vstprintf_s(szMsg, _countof(szMsg), pszFmt, args);
    va_end(args);

    // 日志路径：exe 同目录 DeskTidy_zorder.log
    TCHAR szPath[MAX_PATH] = {};
    ::GetModuleFileName(NULL, szPath, _countof(szPath));
    TCHAR* pDot = _tcsrchr(szPath, _T('.'));
    if (pDot != NULL)
        *pDot = 0;                          // 去掉扩展名
    _tcscat_s(szPath, _T("_zorder.log"));

    // 文件超过 1MB 时清空重写，防止日志无限增长
    HANDLE hFile = ::CreateFile(szPath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    LARGE_INTEGER liSize = {};
    ::GetFileSizeEx(hFile, &liSize);
    if (liSize.QuadPart > 1024 * 1024)
        ::SetEndOfFile(hFile);              // 清空旧内容

    DWORD dwWritten = 0;
    TCHAR szLine[600] = {};
    _stprintf_s(szLine, _countof(szLine), _T("[%08u] %s\r\n"),
                ::GetTickCount(), szMsg);
    ::SetFilePointer(hFile, 0, NULL, FILE_END);
    ::WriteFile(hFile, szLine, (DWORD)(_tcslen(szLine) * sizeof(TCHAR)),
                &dwWritten, NULL);
    ::CloseHandle(hFile);
}

// 获取用于持久化的位置与大小。
// 折叠状态下返回"展开尺寸 + 折叠条当前位置"：
// 保存到 INI 后，下次启动仍以完整矩形创建窗口，再按 Collapsed 标志折叠，
// 这样展开时的完整大小不会因折叠而丢失，折叠期间拖动条目的新位置也能保留。
CRect CDeskTidyWidget::GetLastRect() const
{
    if (m_bCollapsed && m_rcExpanded.Width() > 0 && m_rcExpanded.Height() > 0)
    {
        // m_rcLast 始终记录窗口实际矩形：折叠时其左上角即折叠条当前位置
        return CRect(m_rcLast.left, m_rcLast.top,
                     m_rcLast.left + m_rcExpanded.Width(),
                     m_rcLast.top + m_rcExpanded.Height());
    }
    return m_rcLast;
}

// 刷新文件列表：清空后重新枚举当前目录
void CDeskTidyWidget::RefreshFiles()
{
    if (m_list.GetSafeHwnd() == NULL)
        return;

    // 清空列表、名称/路径缓存与图标列表
    // （图标列表需要同步清空，否则每次刷新都会向列表末尾追加图标，
    //   而列表项索引从 0 重新开始，会造成图标错位/堆积）
    m_list.DeleteAllItems();
    m_arrNames.RemoveAll();
    m_arrPaths.RemoveAll();
    // 仅将图标计数清零（不销毁列表句柄，列表控件仍可安全引用），
    // 索引从 0 重新开始，与重新插入的列表项一一对应
    m_smallImages.SetImageCount(0);
    m_largeImages.SetImageCount(0);

    // 枚举当前目录下的文件/子目录
    AddFilesFromDir(m_strDir);

    // 按视图模式刷新显示文本（列表视图会按列宽以 "..." 省略超长文件名）
    UpdateDisplayTexts();
}

// 枚举并添加当前目录下的文件/子目录到列表
void CDeskTidyWidget::AddFilesFromDir(const CString& strDir)
{
    CFileFind finder;

    // 构造目录搜索模式：<目录>\*.*
    CString strPattern = strDir;
    if (strPattern.Right(1) != _T("\\"))
        strPattern += _T("\\");
    strPattern += _T("*.*");

    BOOL bMore = finder.FindFile(strPattern);
    while (bMore)
    {
        bMore = finder.FindNextFile();

        // 跳过 "." 与 ".."
        if (finder.IsDots())
            continue;
        // 跳过隐藏文件与系统文件（与桌面图标的显示习惯一致）
        if (finder.IsHidden() || finder.IsSystem())
            continue;

        CString strName = finder.GetFileName();
        CString strPath = finder.GetFilePath();

        // 获取该文件的图标（.lnk 会读取快捷方式内存储的图标位置直接提取）
        int nIconIdx = AddFileIcon(strPath);

        // 插入列表项：先带完整文件名插入（随后按列宽做省略处理），
        // lParam 记录路径在 m_arrPaths 中的索引
        int nItem = m_list.InsertItem(m_list.GetItemCount(), strName, nIconIdx);
        if (nItem >= 0)
        {
            // 名称与路径缓存与列表项一一对应（完整名保留，省略仅影响显示）
            m_arrNames.Add(strName);
            m_arrPaths.Add(strPath);
            m_list.SetItemData(nItem, (DWORD_PTR)(m_arrPaths.GetCount() - 1));
        }
    }
    finder.Close();
}

// 判断是否为 .lnk 快捷方式（按扩展名，不区分大小写）
BOOL CDeskTidyWidget::IsLnkFile(const CString& strPath)
{
    return strPath.GetLength() >= 4 &&
           _tcsicmp(strPath.Right(4), _T(".lnk")) == 0;
}

// 获取一个文件的"小图标 + 大图标"，加入自有图像列表，返回列表索引。
// 普通文件通过 SHGetFileInfo 获取系统关联图标；
// .lnk 快捷方式则优先读取快捷方式内存储的图标位置（IconLocation），
// 用 ExtractIconEx 直接从该位置提取，绕开系统图标缓存/快捷方式解析，
// 避免系统缓存失效时（资源管理器里也显示空白）本窗口仍能显示正确图标。
int CDeskTidyWidget::AddFileIcon(const CString& strPath)
{
    HICON hSmall = NULL;
    HICON hLarge = NULL;
    BOOL  bOk    = FALSE;

    // ---- .lnk 快捷方式：读取 IconLocation 并直接提取 ----
    if (IsLnkFile(strPath))
    {
        // 通过 COM 打开快捷方式，读取其显式图标位置（可能指向 exe/ico）
        CComPtr<IShellLink> pLink;
        HRESULT hr = pLink.CoCreateInstance(CLSID_ShellLink, NULL,
                                            CLSCTX_INPROC_SERVER);
        if (SUCCEEDED(hr))
        {
            CComPtr<IPersistFile> pFile;
            if (SUCCEEDED(pLink.QueryInterface(&pFile)) &&
                SUCCEEDED(pFile->Load(strPath, STGM_READ)))
            {
                TCHAR szIconPath[MAX_PATH] = {};
                int   nIconIndex = 0;
                if (SUCCEEDED(pLink->GetIconLocation(szIconPath, MAX_PATH,
                                                     &nIconIndex)) &&
                    szIconPath[0] != 0)
                {
                    // 从图标位置提取大/小图标各一枚
                    UINT nGot = ::ExtractIconEx(szIconPath, nIconIndex,
                                                &hLarge, &hSmall, 1);
                    if (nGot > 0 && hSmall != NULL && hLarge != NULL)
                        bOk = TRUE;     // 提取成功，不再回退系统接口
                }
            }
        }
    }

    // ---- 回退：通过系统接口获取关联图标 ----
    if (!bOk)
    {
        SHFILEINFO sfi = {};
        if (SHGetFileInfo(strPath, 0, &sfi, sizeof(sfi),
                          SHGFI_ICON | SHGFI_SMALLICON) && sfi.hIcon != NULL)
        {
            hSmall = sfi.hIcon;
            bOk = TRUE;
        }
        if (SHGetFileInfo(strPath, 0, &sfi, sizeof(sfi),
                          SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon != NULL)
            hLarge = sfi.hIcon;
    }

    // 图标获取失败：返回 -1（列表项将不显示图标）
    if (!bOk || hSmall == NULL || hLarge == NULL)
    {
        if (hSmall != NULL) ::DestroyIcon(hSmall);
        if (hLarge != NULL) ::DestroyIcon(hLarge);
        return -1;
    }

    // 把大小图标统一缩放到"按当前缩放级别计算的标准尺寸"后再加入列表：
    // 不同来源的图标（exe 图标、文件关联图标、系统缩略图等）原生尺寸不一，
    // 若直接加入会造成网格间距不均、行列错位；同时缩放级别越大图标越大
    int cxSmall = 0, cySmall = 0, cxLarge = 0, cyLarge = 0;
    GetZoomIconSizes(cxSmall, cySmall, cxLarge, cyLarge);
    HICON hSmallScaled = (HICON)::CopyImage(hSmall, IMAGE_ICON, cxSmall, cySmall, 0);
    HICON hLargeScaled = (HICON)::CopyImage(hLarge, IMAGE_ICON, cxLarge, cyLarge, 0);
    if (hSmallScaled == NULL) hSmallScaled = hSmall;   // 缩放失败时退回原图
    if (hLargeScaled == NULL) hLargeScaled = hLarge;

    // 把大小图标按相同顺序加入两份列表（索引一一对应，切换视图均有效）
    int nSmallIdx = m_smallImages.Add(hSmallScaled);
    int nLargeIdx = m_largeImages.Add(hLargeScaled);

    // 释放缩放产生的临时图标与原始图标
    if (hSmallScaled != hSmall) ::DestroyIcon(hSmallScaled);
    if (hLargeScaled != hLarge) ::DestroyIcon(hLargeScaled);
    ::DestroyIcon(hSmall);
    ::DestroyIcon(hLarge);

    // 两份列表索引必须一致，否则回退为无图标（避免视图切换后错位）
    if (nSmallIdx < 0 || nSmallIdx != nLargeIdx)
        return -1;
    return nSmallIdx;
}

// ---------------------------------------------------------------------------
// 缩放（Ctrl+鼠标滚轮）：缩放列表内文件的图标与文字大小
// ---------------------------------------------------------------------------

// 设置缩放级别（百分比，50~300，默认 100）。
// 窗口已创建时立即重建字体与图标并刷新显示；未创建时仅记录，
// 由 OnCreate 在创建后按此值应用；级别变化时通知主对话框保存配置，
// 保证重启后恢复相同的缩放级别。
void CDeskTidyWidget::SetZoom(int nZoom)
{
    // 限制在合法范围内（防止越界导致图标/字体尺寸异常）
    if (nZoom < WIDGET_ZOOM_MIN) nZoom = WIDGET_ZOOM_MIN;
    if (nZoom > WIDGET_ZOOM_MAX) nZoom = WIDGET_ZOOM_MAX;

    // 级别未变化：无需重建，直接返回（窗口未创建时同样只记录一次）
    if (nZoom == m_nZoom)
        return;

    m_nZoom = nZoom;

    // 列表控件尚未创建（LoadConfig 在 CreateWidget 之前调用）：
    // 只记录级别，创建后由 OnCreate 调用 ApplyZoom 应用
    if (m_list.GetSafeHwnd() == NULL)
        return;

    ApplyZoom();

    // 缩放级别是需要持久化的属性：通知主对话框写入 INI
    if (m_hNotify != NULL && ::IsWindow(m_hNotify))
        ::PostMessage(m_hNotify, WM_WIDGET_CHANGED, 0, 0);
}

// 按滚轮方向缩放一档：zDelta > 0 放大（+10%）、zDelta < 0 缩小（-10%）。
// 由列表控件派生类 CWidgetListCtrl 在按住 Ctrl 滚动滚轮时回调；
// 到达缩放边界时不再缩放，但消息仍视为已处理（避免列表同时发生滚动）。
BOOL CDeskTidyWidget::ZoomByWheel(short zDelta)
{
    int nNew = m_nZoom + (zDelta > 0 ? WIDGET_ZOOM_STEP : -WIDGET_ZOOM_STEP);

    if (nNew < WIDGET_ZOOM_MIN || nNew > WIDGET_ZOOM_MAX)
        return TRUE;    // 已到边界：不缩放，但消耗本次滚轮

    SetZoom(nNew);
    return TRUE;
}

// 按指定步数缩放列表图标/文字大小（供全局快捷键等非滚轮场景调用）：
// nStep > 0 放大、< 0 缩小，每步 WIDGET_ZOOM_STEP（10%）；
// 到达缩放边界时由 SetZoom 自动钳制并停在边界值
void CDeskTidyWidget::ZoomStep(int nStep)
{
    SetZoom(m_nZoom + nStep * WIDGET_ZOOM_STEP);
}

// 应用当前缩放级别：重建字体与图标，并按新尺寸刷新列表显示
void CDeskTidyWidget::ApplyZoom()
{
    if (m_list.GetSafeHwnd() == NULL)
        return;

    // 1. 重建字体：字号随缩放级别变化（以基础字高 × 缩放 / 100）
    RebuildFont();

    // 2. 重建图标列表：图标尺寸随缩放级别变化（重新提取所有文件图标）
    RebuildImages();

    // 2.5 WorkBuddy: 图标尺寸与字体都已更新后，先重算网格视图图标间距，
    // 否则放大后上一行标签会盖住下一行图标；必须在刷新省略文本之前调用，
    // 这样网格视图的"单元格宽度"才会按新间距计算
    UpdateIconSpacing();

    // 3. 图标/字体尺寸变化后，重新按新尺寸省略文件名
    //    （列表视图按列宽、网格视图按图标单元格宽度）
    UpdateDisplayTexts();
    m_list.Invalidate();
}

// 重建列表字体：以 OnCreate 捕获的基础字体（m_lfBaseFont）为基准，
// 把字高按缩放百分比等比缩放后生成新字体并设置到列表控件
void CDeskTidyWidget::RebuildFont()
{
    // 基础字高（负值表示按像素指定的字高，缩放后仍为负值，
    // 保证新字体与原字体是同一度量方式）
    LOGFONT lf = m_lfBaseFont;
    lf.lfHeight = MulDiv(lf.lfHeight, m_nZoom, 100);

    // 重建字体对象并应用到列表控件
    m_font.DeleteObject();
    m_font.CreateFontIndirect(&lf);
    m_list.SetFont(&m_font);
}

// 重建图标列表：按当前缩放级别重新计算图标尺寸，销毁旧图像列表后
// 以新尺寸重建，并为每个文件重新提取图标加入列表（索引与列表项对应）
void CDeskTidyWidget::RebuildImages()
{
    // 计算缩放后的图标尺寸
    int cxSmall = 0, cySmall = 0, cxLarge = 0, cyLarge = 0;
    GetZoomIconSizes(cxSmall, cySmall, cxLarge, cyLarge);

    // 先解除列表控件对旧图像列表的引用，再销毁旧列表，
    // 避免删除句柄后控件仍引用悬空句柄导致崩溃
    m_list.SetImageList(NULL, LVSIL_SMALL);
    m_list.SetImageList(NULL, LVSIL_NORMAL);
    m_smallImages.DeleteImageList();
    m_largeImages.DeleteImageList();

    // 以缩放后的尺寸重建图像列表
    m_smallImages.Create(cxSmall, cySmall, ILC_COLOR32, 16, 8);
    m_largeImages.Create(cxLarge, cyLarge, ILC_COLOR32, 16, 8);

    // 为每个文件重新提取图标并加入新列表，同时更新对应列表项的图标索引。
    // 刷新期间禁止重绘，避免列表项图标逐个更新时闪烁
    m_list.SetRedraw(FALSE);
    int nCount = (int)m_arrPaths.GetCount();
    for (int i = 0; i < nCount; i++)
    {
        int nIconIdx = AddFileIcon(m_arrPaths[i]);   // 失败返回 -1（无图标）
        m_list.SetItem(i, 0, LVIF_IMAGE, NULL, nIconIdx, 0, 0, 0);
    }
    m_list.SetRedraw(TRUE);

    // 按当前视图模式重新挂接对应的图像列表
    if (m_nViewMode == 0)
        m_list.SetImageList(&m_smallImages, LVSIL_SMALL);
    else
    {
        m_list.SetImageList(&m_largeImages, LVSIL_NORMAL);
        // 网格视图：图标尺寸变化后强制按新网格间距重排，保证行列对齐
        m_list.Arrange(LVA_ALIGNTOP);
    }
}

// 计算按当前缩放级别缩放后的图标尺寸（小图标/大图标各一组）。
// 以系统标准图标尺寸为基准（SM_CXSMICON/SM_CXICON），乘以缩放百分比：
// 缩放 100% 时等于系统标准尺寸，50% 时减半，300% 时放大三倍。
// 所有使用图标尺寸的地方（图像列表创建、图标提取）统一调用本函数，
// 保证缩放后各处尺寸一致
void CDeskTidyWidget::GetZoomIconSizes(int& cxSmall, int& cySmall,
                                       int& cxLarge, int& cyLarge) const
{
    cxSmall = MulDiv(GetSystemMetrics(SM_CXSMICON), m_nZoom, 100);
    cySmall = MulDiv(GetSystemMetrics(SM_CYSMICON), m_nZoom, 100);
    cxLarge = MulDiv(GetSystemMetrics(SM_CXICON),   m_nZoom, 100);
    cyLarge = MulDiv(GetSystemMetrics(SM_CYICON),   m_nZoom, 100);
}

// WorkBuddy: 按当前缩放级别重算网格（大图标）视图的图标间距。
// 网格视图每个项目 = 上方图标 + 下方标签，项目按"图标间距"网格排布。
// 放大时图标尺寸（cyLarge）和标签字体都变大，但控件默认的图标间距仍是
// 系统基准值（SM_CXICONSPACING/SM_CYICONSPACING），不会随缩放增长，
// 于是上一行的标签文字向下延伸，盖住了下一行的图标，导致图标显示不全。
// 这里显式把间距设大：纵向至少容纳"图标高度 + 单行标签高度 + 留白"，
// 横向留出标签左右边距，使每行项目完整、互不重叠。仅对图标视图生效，
// 报表视图的行高由字体/图标尺寸自动撑开，无需此处理。
void CDeskTidyWidget::UpdateIconSpacing()
{
    if (m_list.GetSafeHwnd() == NULL)
        return;

    // 当前缩放后的大图标尺寸（网格视图使用 LVSIL_NORMAL）
    int cxSmall = 0, cySmall = 0, cxLarge = 0, cyLarge = 0;
    GetZoomIconSizes(cxSmall, cySmall, cxLarge, cyLarge);

    // 标签为单行（UpdateDisplayTexts 已按单元格宽度截断追加 "..."），
    // 用当前缩放字体的文本度量得到单行高度（含外部行距）
    int labelH = 0;
    CClientDC dc(&m_list);
    CFont* pOldFont = dc.SelectObject(&m_font);
    if (pOldFont != NULL)
    {
        TEXTMETRIC tm = { 0 };
        if (dc.GetTextMetrics(&tm))
            labelH = tm.tmHeight + tm.tmExternalLeading;
        dc.SelectObject(pOldFont);
    }
    if (labelH <= 0)
        labelH = abs(m_lfBaseFont.lfHeight) * m_nZoom / 100 + 2;  // 兜底

    // 横向间距：至少比大图标宽留左右边距，同时不低于系统图标间距的缩放值，
    // 保证单行文件名有合理显示宽度（不顶到相邻图标、也不被过度截断）；
    // 同时作为 UpdateDisplayTexts 中网格视图的"单元格宽度"基准
    int cxBase = MulDiv(GetSystemMetrics(SM_CXICONSPACING), m_nZoom, 100);
    int cxSpace = max(cxLarge + 24, cxBase);
    // 纵向间距：图标高度 + 标签高度 + 标签下方留白，杜绝上下行重叠
    int cySpace = cyLarge + labelH + 8;

    // 设置图标间距（仅对 LVS_ICON 视图生效；报表视图忽略）
    m_list.SetIconSpacing(CSize(cxSpace, cySpace));
}

// 按当前视图刷新每个列表项的显示文本：基名按各视图的字符上限截断（列表 100 /
// 网格 10）、扩展名始终保留（".lnk" 例外，见 FormatDisplayName），列表视图按列宽、
// 网格视图按单元格宽度做二次宽度兜底，保证不溢出、不错位。
void CDeskTidyWidget::UpdateDisplayTexts()
{
    if (m_list.GetSafeHwnd() == NULL)
        return;

    int nCount = m_list.GetItemCount();

    if (m_nViewMode == 0)
    {
        // ---- 列表（报表）视图：按第一列列宽省略 ----
        CHeaderCtrl* pHeader = m_list.GetHeaderCtrl();
        if (pHeader == NULL)
            return;
        HDITEM hdi = {};
        hdi.mask = HDI_WIDTH;
        if (!pHeader->GetItem(0, &hdi))
            return;
        int nColWidth = hdi.cxy;                // 名称列宽度

        CClientDC dc(&m_list);
        CFont* pFont = m_list.GetFont();
        CFont* pOldFont = dc.SelectObject(pFont);

        // WorkBuddy: 报表视图每个文件名经 FormatDisplayName 处理——基名最多
        // WIDGET_NAME_MAX_CHARS_LIST（100）字符、扩展名始终保留；nMaxWidth 传
        // 列宽-留白，用于长名或列变窄时的宽度兜底
        for (int i = 0; i < nCount && i < (int)m_arrNames.GetCount(); i++)
        {
            CString strDisp = FormatDisplayName(m_arrNames[i], nColWidth - 4, &dc,
                                                WIDGET_NAME_MAX_CHARS_LIST);
            m_list.SetItemText(i, 0, strDisp);
        }

        dc.SelectObject(pOldFont);
    }
    else
    {
        // ---- 网格（大图标）视图：按图标单元格宽度省略，保证整齐对齐 ----
        // 获取图标单元格（项间距）宽度；取不到时用"缩放后的大图标宽度"的
        // 3 倍兜底（与缩放级别保持一致，避免缩放后省略文本仍按原宽度计算）
        int nCellCx = (int)::SendMessage(m_list.GetSafeHwnd(), LVM_GETITEMSPACING, FALSE, 0);
        if (nCellCx <= 0)
        {
            int cxSmall = 0, cySmall = 0, cxLarge = 0, cyLarge = 0;
            GetZoomIconSizes(cxSmall, cySmall, cxLarge, cyLarge);
            nCellCx = cxLarge * 3;
        }
        // 标签可占的最大文本宽度（单元格宽度减去左右留白）
        int nMaxText = max(WIDGET_TRIM_MIN, nCellCx - 8);

        CClientDC dc(&m_list);
        CFont* pFont = m_list.GetFont();
        CFont* pOldFont = dc.SelectObject(pFont);

        // WorkBuddy: 网格视图同样走 FormatDisplayName——但字符上限用
        // WIDGET_NAME_MAX_CHARS_GRID（10）而非列表视图的 100：多个图标格子并排，
        // 标签过长会破坏网格的整齐对齐。nMaxWidth 传单元格可容纳宽度，
        // 作为缩放后单元格变窄时的二次兜底
        for (int i = 0; i < nCount && i < (int)m_arrNames.GetCount(); i++)
        {
            CString strDisp = FormatDisplayName(m_arrNames[i], nMaxText, &dc,
                                                WIDGET_NAME_MAX_CHARS_GRID);
            m_list.SetItemText(i, 0, strDisp);
        }

        dc.SelectObject(pOldFont);
    }
}

// WorkBuddy: 生成文件名显示文本。
// 规则：基名最多 nMaxChars 个字符（由调用方按视图传入：
// 列表视图 WIDGET_NAME_MAX_CHARS_LIST、网格视图 WIDGET_NAME_MAX_CHARS_GRID），
// 扩展名（含点）始终保留——唯一例外是 ".lnk"（快捷方式），它属于实现细节而非
// 用户关心的信息，一律不显示（与资源管理器默认行为一致）；
// 基名超长时取前 N 个字符并追加 "..."，再拼接扩展名。
// 当 nMaxWidth>0 且拼接结果仍超出可用宽度时，再从基名侧做宽度二分缩短
// （保留扩展名、仅一个 "..."），用于极端小单元格的兜底，避免溢出到相邻项。
//
// 注意两者的分工：nMaxChars 是"字符数上限"（与字体、列宽无关，纯语义限制），
// nMaxWidth 是"像素宽度上限"（随列宽/缩放变化）。列表视图把 nMaxChars 放宽到
// 100 后，真正的约束就落在 nMaxWidth 上——列够宽就显示全名，列窄才省略，
// 这符合"文件名应该尽量显示完整"的直觉
CString CDeskTidyWidget::FormatDisplayName(LPCTSTR pszName, int nMaxWidth, CDC* pDC,
                                           int nMaxChars) const
{
    CString strFull(pszName);

    // 拆分基名与扩展名：点不在首、不在尾才视为扩展名分隔（如 ".git" 不拆、"a." 不拆）
    CString strBase = strFull;
    CString strExt;
    int nDot = strBase.ReverseFind(_T('.'));
    if (nDot > 0 && nDot < strBase.GetLength() - 1)
    {
        strExt  = strBase.Mid(nDot);     // 含点的扩展名，如 ".txt"
        strBase = strBase.Left(nDot);    // 纯基名

        // WorkBuddy: ".lnk"（快捷方式）不显示扩展名。
        // 本小窗口里绝大多数项都是拖入文件后生成的快捷方式，".lnk" 是实现细节
        // 而非用户关心的信息——显示它只会让名字变长、在窄列/窄格子里更早触发
        // 省略号。资源管理器默认也隐藏该扩展名，这里与之保持一致。
        // 注意：隐藏后基名可能自带点（如 "报表.docx.lnk" → "报表.docx"），
        // 下方字符数截断与宽度二分都以该基名为准，逻辑不受影响
        if (_tcsicmp(strExt, _T(".lnk")) == 0)
            strExt.Empty();
    }

    // 基名按字符数上限截断（上限由调用方按视图给定，<=0 表示不限制字符数）
    CString strBaseShown = strBase;
    BOOL bCapped = FALSE;
    if (nMaxChars > 0 && strBase.GetLength() > nMaxChars)
    {
        strBaseShown = strBase.Left(nMaxChars);
        bCapped = TRUE;
    }
    CString strOut = strBaseShown + (bCapped ? _T("...") : _T("")) + strExt;

    // 宽度兜底：连"字符数上限内的基名 + 扩展名"都超宽时，从基名侧继续缩短
    if (pDC != NULL && nMaxWidth > 0 && pDC->GetTextExtent(strOut).cx > nMaxWidth)
    {
        CString strEll = _T("...");
        int nLo = 0, nHi = strBaseShown.GetLength();
        CString strBest = strBaseShown;
        while (nLo < nHi)
        {
            int nMid = (nLo + nHi + 1) / 2;
            CString s = strBaseShown.Left(nMid) + strEll + strExt;
            if (pDC->GetTextExtent(s).cx <= nMaxWidth)
            {
                strBest = strBaseShown.Left(nMid);
                nLo = nMid;
            }
            else
            {
                nHi = nMid - 1;
            }
        }
        strOut = strBest + strEll + strExt;
    }

    return strOut;
}

// 应用当前视图模式（列表 <-> 网格）
void CDeskTidyWidget::ApplyViewMode()
{
    if (m_list.GetSafeHwnd() == NULL)
        return;

    // 清除原有列头
    while (m_list.DeleteColumn(0))
        ;

    if (m_nViewMode == 0)
    {
        // 列表模式：报表视图，仅"名称"一列（不显示所在目录列），使用小图标
        m_list.ModifyStyle(LVS_TYPEMASK, LVS_REPORT, SWP_FRAMECHANGED);
        m_list.InsertColumn(0, _T("名称"), LVCFMT_LEFT, 150);
        m_list.SetImageList(&m_smallImages, LVSIL_SMALL);
        // 把名称列拉伸到与列表客户区等宽，使各项整行左对齐
        UpdateColumnWidth();
    }
    else
    {
        // 网格模式：大图标视图，使用大图标；
        // 强制"顶对齐"排列（逐行从左到右排布），保证项目整齐成行
        m_list.ModifyStyle(LVS_TYPEMASK, LVS_ICON, SWP_FRAMECHANGED);
        m_list.ModifyStyle(LVS_ALIGNMASK, LVS_ALIGNTOP);
        m_list.SetImageList(&m_largeImages, LVSIL_NORMAL);
        // WorkBuddy: 切换到网格视图时按当前缩放重算图标间距，避免放大状态
        // 下切换视图后标签覆盖图标；再按网格重排（图标间距均匀，行列对齐）
        UpdateIconSpacing();
        m_list.Arrange(LVA_ALIGNTOP);
    }

    // 重新按新视图刷新显示文本（切换视图后自动重排/省略）
    UpdateDisplayTexts();

    // WorkBuddy: 切换视图（尤其报表视图 InsertColumn）后重新应用背景色，
    // 保证列头背景与列表背景色一致、且不被系统默认白底覆盖
    ApplyListColors();
    m_list.Invalidate();
}

// 列表（报表）视图下，把"名称"列拉伸到与列表客户区等宽，
// 使所有项目整行左对齐；窗口改变宽度时也会被 OnSize 调用以重新省略
void CDeskTidyWidget::UpdateColumnWidth()
{
    if (m_list.GetSafeHwnd() == NULL || m_nViewMode != 0)
        return;

    CRect rcList;
    m_list.GetClientRect(&rcList);

    int nWidth = rcList.Width();
    // 若存在垂直滚动条，扣除其宽度避免出现横向滚动条
    if (m_list.GetStyle() & WS_VSCROLL)
        nWidth -= GetSystemMetrics(SM_CXVSCROLL);

    m_list.SetColumnWidth(0, max(60, nWidth));
}

// ---------------------------------------------------------------------------
// 消息处理
// ---------------------------------------------------------------------------

int CDeskTidyWidget::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
    if (CWnd::OnCreate(lpCreateStruct) == -1)
        return -1;

    // 创建文件列表控件：
    //   LVS_REPORT          - 初始为报表视图（随后按配置切换）
    //   LVS_SINGLESEL       - 单击选中
    //   LVS_SHAREIMAGELISTS - 共享图标列表（列表为成员对象，生命周期由窗口管理）
    //   LVS_AUTOARRANGE     - 图标视图下自动排列：增删项目/改变大小时
    //                         图标始终按行列网格对齐（报表视图下自动忽略）
    //   LVS_NOLABELWRAP     - WorkBuddy: 网格（大图标）视图下文件名禁止换行，
    //                         始终单行显示（超宽由 UpdateDisplayTexts 截断追加 "..."），
    //                         避免长文件名换行成两行后盖住图标或破坏行列对齐
    //   LVS_NOCOLUMNHEADER  - WorkBuddy: 报表视图下不显示"名称"列头（小窗口
    //                         只有单列，列头纯属多余且挤占纵向空间）。该样式
    //                         仅对报表视图生效，网格（图标）视图自动忽略；
    //                         "名称"列本身仍存在（InsertColumn/列宽拉伸、整行
    //                         左对齐与文件名省略逻辑均不受影响）
    if (!m_list.Create(WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                       LVS_SHAREIMAGELISTS | LVS_AUTOARRANGE | LVS_NOLABELWRAP |
                       LVS_NOCOLUMNHEADER,
                       CRect(0, 0, 0, 0), this, IDC_WIDGET_LIST))
        return -1;

    // 关联宿主小窗口：列表控件收到 Ctrl+滚轮时回调本窗口完成缩放
    m_list.SetZoomOwner(this);

    // 捕获列表控件当前字体（系统默认控件字体）作为缩放基准：
    // 之后每次缩放都在 m_lfBaseFont 的字高上乘以缩放百分比生成新字体
    CFont* pBaseFont = m_list.GetFont();
    if (pBaseFont != NULL)
        pBaseFont->GetLogFont(&m_lfBaseFont);

    // 创建自有小/大图标列表（供列表视图 / 网格视图使用）。
    // 每个文件的图标都被"复制"进这两个列表（Add 时按同一顺序加入，
    // 索引一一对应，切换视图时同一索引在两份列表中均有效）；
    // 不直接使用系统图标列表，是为了 .lnk 可以读取快捷方式内存储的
    // IconLocation 直接提取正确图标，不受系统图标缓存失效的影响。
    // 图标尺寸按当前缩放级别计算（缩放越大图标越大）
    int cxSmall = 0, cySmall = 0, cxLarge = 0, cyLarge = 0;
    GetZoomIconSizes(cxSmall, cySmall, cxLarge, cyLarge);
    m_smallImages.Create(cxSmall, cySmall, ILC_COLOR32, 16, 8);
    m_largeImages.Create(cxLarge, cyLarge, ILC_COLOR32, 16, 8);

    // 开启双缓冲，减少刷新时的闪烁
    m_list.SetExtendedStyle(m_list.GetExtendedStyle() | LVS_EX_DOUBLEBUFFER);

    // 注册为文件拖放目标：允许从资源管理器把文件/快捷方式拖进本小窗口
    // （拖到列表控件上时，系统会沿父窗口链找到本窗口并投递 WM_DROPFILES）
    DragAcceptFiles(TRUE);

    // WorkBuddy: 监听剪贴板内容变化（与 OnDestroy 的 RemoveClipboardFormatListener 配对）。
    // 用途：本窗口"剪切"的项会被置灰显示，若之后剪贴板被别的程序接管（用户在别处
    // 复制了东西），那些项的"待粘贴"状态就永久失效了——收到本消息即可及时清掉置灰，
    // 避免留下永远不会被粘贴走的"幽灵项"。失败不影响其它功能，仅失去该自愈能力
    ::AddClipboardFormatListener(m_hWnd);

    // 应用当前缩放级别：窗口创建前可能已由 LoadConfig 设置了缩放值
    // （非 100），这里按该值重建字体与图标尺寸（值为 100 时仅重建字体）
    ApplyZoom();

    // 按当前视图模式设置列表样式与列头
    ApplyViewMode();

    // WorkBuddy: 应用当前背景色到列表控件（否则列表默认白底会挡住小窗口背景色）
    ApplyListColors();

    // WorkBuddy: 创建标题栏按钮的文字提示控件（鼠标悬停按钮时显示功能说明）。
    // 必须在列表/视图就绪后、首次绘制前创建，保证后续悬停即可显示
    InitTooltip();

    // 调试日志：记录小窗口创建，确认日志写入功能正常
    LogZ(_T("OnCreate: 小窗口创建完成 (hwnd=%p)"), (void*)m_hWnd);

    // WorkBuddy: 首次合成分层位图并呈现（分层窗口在调用 UpdateLayeredWindow 前不可见）
    RenderLayered();

    return 0;
}

void CDeskTidyWidget::OnDestroy()
{
    // WorkBuddy: 注销剪贴板监听（与 OnCreate 的 AddClipboardFormatListener 配对）
    ::RemoveClipboardFormatListener(m_hWnd);

    // WorkBuddy: 销毁前先关闭可能正在显示的按钮文字提示，
    // 否则提示框（独立弹出窗口）会滞后消失，造成"提示残留"
    HideButtonTip();
    if (m_toolTip.m_hWnd != NULL)
        m_toolTip.DestroyWindow();

    // 停止"创建后刷新"一次性定时器（若在延时到达前被销毁）
    KillTimer(WIDGET_INIT_REFRESH_TIMER);

    // 若仍挂接在 WorkerW 上，先脱离为顶层窗口再销毁，避免留下异常父子关系
    if (::GetParent(m_hWnd) != NULL)
    {
        ::SetParent(m_hWnd, NULL);
        ModifyStyle(WS_CHILD, WS_POPUP);
    }
    m_bAttached = FALSE;
    m_hHost = NULL;

    // WorkBuddy: WitchDrawer 方案——销毁前解除桌面 Owner（恢复原始 Owner），
    // 避免窗口销毁时把"已失效的 owned 关系"残留给桌面 Shell
    ClearDesktopOwner();

    // WorkBuddy: 注销 Shell 钩子（与 OnCreate 的 RegisterShellHookWindow 配对）
    ::DeregisterShellHookWindow(m_hWnd);

    // 销毁前把最终位置记入 m_rcLast（主对话框在保存配置时会读取）
    GetWindowRect(&m_rcLast);

    CWnd::OnDestroy();
}

void CDeskTidyWidget::OnTimer(UINT_PTR nIDEvent)
{
    // 一次性"创建后刷新"：窗口显示后重新枚举目录内容一次
    if (nIDEvent == WIDGET_INIT_REFRESH_TIMER)
    {
        KillTimer(nIDEvent);        // 只执行一次
        RefreshFiles();
        return;
    }

    // 低频 z 序维护：周期检查"显示桌面"模式并维护层级。
    // 检查是只读的，仅在状态翻转时操作窗口，不会造成闪烁。
    // WorkBuddy: 桌面子窗口方案不走 MaintainZOrder（免 z 序维护），
    // 改为驱动"挂载失败自动重试"——已挂接时该函数零开销返回
    if (nIDEvent == WIDGET_ZORDER_TIMER)
    {
        if (m_nLayerMode == 0 && m_bBottomViaChild)
            TryReattachDesktopChild();
        else
            MaintainZOrder();
		//KillTimer(nIDEvent);        // 只执行一次
        return;
    }

    CWnd::OnTimer(nIDEvent);
}

// 拦截系统命令中的"最小化"（SC_MINIMIZE）：
// Win+M 等途径会向所有顶层窗口发送 WM_SYSCOMMAND 的 SC_MINIMIZE 命令，
// 拦截后窗口不会被最小化。注意：Win10/11 的 Win+D（显示桌面）和任务栏
// "显示桌面"按钮是直接调用 ShowWindow(SW_MINIMIZE)，不经过本消息，
// 因此对它们无效，由 OnSize 的 SIZE_MINIMIZED 分支 + OnRestoreMinimized
// 负责兜底恢复。小窗口没有系统菜单和标题栏最小化按钮，正常操作不会
// 触发 SC_MINIMIZE，拦截无副作用。
void CDeskTidyWidget::OnSysCommand(UINT nID, LPARAM lParam)
{
    if ((nID & 0xFFF0) == SC_MINIMIZE)
        return;         // 拒绝最小化，保持显示

    CWnd::OnSysCommand(nID, lParam);
}

// 最小化恢复处理（WM_WIDGET_RESTORE，由 OnSize 的 SIZE_MINIMIZED 分支投递）：
// 在系统最小化流程完全结束后恢复小窗口显示，保证"显示桌面"后依然可见。
// 关键点：小窗口是无任务栏按钮的顶层窗口（WS_EX_TOOLWINDOW），系统对它
// 执行最小化时没有任务栏缩略图可用，会把窗口移到屏幕外（如 -32000,-32000）。
// 因此必须先按最小化前记录的矩形 m_rcLast 把窗口放回原位，再恢复显示，
// 否则窗口会"恢复"到屏幕外仍然看不见（表现为"Win+D 后小窗口不见了"）。
LRESULT CDeskTidyWidget::OnRestoreMinimized(WPARAM wParam, LPARAM lParam)
{
    LogZ(_T("OnRestoreMinimized: 恢复最小化窗口"));

    // 1) 用最小化前记录的矩形放回原位（保持 z 序、不激活）
    if (m_rcLast.Width() > 0 && m_rcLast.Height() > 0)
    {
        ::SetWindowPos(m_hWnd, NULL, m_rcLast.left, m_rcLast.top,
                       m_rcLast.Width(), m_rcLast.Height(),
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // 2) 不激活地恢复显示（SW_SHOWNOACTIVATE）：清除最小化状态、
    //    不抢焦点、不打断用户当前正在进行的操作；z 序不变（仍为最底层）
    ShowWindow(SW_SHOWNOACTIVATE);

    return 0;
}

void CDeskTidyWidget::OnSize(UINT nType, int cx, int cy)
{
    CWnd::OnSize(nType, cx, cy);

    // 兜底拦截最小化：Win10/11 的 Win+D / 任务栏"显示桌面" / Win+M 等
    // 途径不经过 WM_SYSCOMMAND，窗口会被 ShowWindow(SW_MINIMIZE) 直接
    // 最小化（收到 SIZE_MINIMIZED）。这里投递 WM_WIDGET_RESTORE 消息，
    // 等系统最小化流程完全结束后再恢复显示（OnRestoreMinimized），
    // 避免在最小化流程进行中立即恢复被系统再次覆盖
    if (nType == SIZE_MINIMIZED)
    {
        LogZ(_T("OnSize: 收到 SIZE_MINIMIZED，投递 WM_WIDGET_RESTORE"));
        PostMessage(WM_WIDGET_RESTORE);
        return;
    }

    // 折叠状态下若窗口被拖高到超过标题条，自动展开：
    // 避免出现"窗口变大但列表仍隐藏"的中间状态
    if (m_bCollapsed && cy > WIDGET_HEADER_HEIGHT + WIDGET_EDGE_SIZE)
    {
        SetCollapsed(FALSE);
        return;     // SetCollapsed 内部会再次触发 OnSize 完成后续布局
    }

    // 列表控件占据标题条以下、四周留出边缘留白（供边缘缩放使用）
    if (m_list.GetSafeHwnd() != NULL)
    {
        int x = WIDGET_EDGE_SIZE;
        int y = WIDGET_HEADER_HEIGHT;
        int w = max(0, cx - WIDGET_EDGE_SIZE * 2);
        int h = max(0, cy - WIDGET_HEADER_HEIGHT - WIDGET_EDGE_SIZE);
        m_list.MoveWindow(x, y, w, h);

        // 窗口宽度变化后：列表视图重新拉伸列宽并按新列宽省略文件名
        if (m_nViewMode == 0)
        {
            UpdateColumnWidth();
            UpdateDisplayTexts();
        }
    }

    // 同步记录最新窗口矩形（供持久化）
    GetWindowRect(&m_rcLast);

    // 展开状态下同步记录展开矩形（供折叠后恢复；折叠状态下由 SetCollapsed 维护）。
    // WorkBuddy: 防污染——高度不大于"标题条+边缘"的展开矩形是病态值（历史
    // bug 污染或异常拖拽产生），不用它覆盖有效的展开矩形，避免"折叠→展开
    // 只剩 24px 折叠条"的死循环
    if (!m_bCollapsed && m_rcLast.Height() > WIDGET_HEADER_HEIGHT + WIDGET_EDGE_SIZE)
        m_rcExpanded = m_rcLast;

    // WorkBuddy: 尺寸变化后重新合成分层位图（宽/高、列表位置都已更新）
    RenderLayered();
}

//BOOL CDeskTidyWidget::OnEraseBkgnd(CDC* /*pDC*/)
//{
//    // 不在此处擦除背景：父窗口擦背景会整块覆盖客户区（包括列表控件区域），
//    // 随后列表再重刷一次，正是闪烁的根源。
//    // 返回 TRUE 表示已"处理"，背景改由 OnPaint 双缓冲统一绘制
//    // （配合 WS_CLIPCHILDREN，父窗口绘制也不会覆盖到列表控件上）。
//    //return TRUE;
//}

// WorkBuddy: 颜色明暗微调（nDelta > 0 提亮、< 0 压暗，结果自动钳制到 0~255）。
// 用于从用户设定的标题栏颜色派生出"渐变端色 / 按钮悬停色 / 分隔线色"，
// 使这些派生色始终与用户自定义配色协调，而不是写死一组固定颜色
static COLORREF ShadeColor(COLORREF clr, int nDelta)
{
    int r = GetRValue(clr) + nDelta;
    int g = GetGValue(clr) + nDelta;
    int b = GetBValue(clr) + nDelta;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return RGB(r, g, b);
}

// WorkBuddy: 垂直线性渐变填充（从上到下 clrTop -> clrBottom）。
// 用逐行 FillSolidRect 实现，避免为了一次渐变就引入 msimg32.lib（GradientFill）
// 的链接依赖；标题条仅 24px 高，这点逐行开销可以忽略
static void DrawVGradient(CDC& dc, const CRect& rc, COLORREF clrTop, COLORREF clrBottom)
{
    const int nHeight = rc.Height();
    if (nHeight <= 0 || rc.Width() <= 0)
        return;

    if (nHeight == 1)
    {
        dc.FillSolidRect(rc, clrTop);
        return;
    }

    const int r1 = GetRValue(clrTop),  r2 = GetRValue(clrBottom);
    const int g1 = GetGValue(clrTop),  g2 = GetGValue(clrBottom);
    const int b1 = GetBValue(clrTop),  b2 = GetBValue(clrBottom);

    for (int y = 0; y < nHeight; y++)
    {
        const int t = y * 255 / (nHeight - 1);      // 0..255
        const int r = r1 + (r2 - r1) * t / 255;
        const int g = g1 + (g2 - g1) * t / 255;
        const int b = b1 + (b2 - b1) * t / 255;
        dc.FillSolidRect(rc.left, rc.top + y, rc.Width(), 1, RGB(r, g, b));
    }
}

// WorkBuddy: 绘制"不透明内容"（背景除外）：标题条底色、标题文字、右侧按钮、整体边框。
// 由 RenderLayered 在临时 32 位位图上调用，随后整图按区域叠加 alpha 呈现。
// 注意：不在此处填充背景色——背景由 RenderLayered 统一填充，便于按区域设置 alpha
void CDeskTidyWidget::DrawContent(CDC& dc, const CRect& rcClient)
{
    // 1. 标题条（使用可单独设置的标题栏颜色；标题文字按亮度自适应黑/白）
    //    WorkBuddy: 改用垂直微渐变（顶部略亮 -> 底部略暗），比纯色更有质感与
    //    体积感；端色由用户设定的标题栏色派生，因此任意自定义配色都协调
    CRect rcHeader(rcClient);
    rcHeader.bottom = WIDGET_HEADER_HEIGHT;
    DrawVGradient(dc, rcHeader, ShadeColor(m_clrHeader, +20), ShadeColor(m_clrHeader, -14));

    // WorkBuddy: 标题条下沿压一条暗线，与内容区形成清晰层次（现代标题栏的常规做法）
    if (rcHeader.Height() > 2)
        dc.FillSolidRect(rcHeader.left, rcHeader.bottom - 1, rcHeader.Width(), 1,
                         ShadeColor(m_clrHeader, -40));

    // 标题文字：显示本小窗口对应的目录名（取路径最后一段，根目录显示完整路径）
    CString strTitle = m_strDir;
    int nSlash = strTitle.ReverseFind(_T('\\'));
    if (nSlash >= 0 && nSlash < strTitle.GetLength() - 1)
        strTitle = strTitle.Mid(nSlash + 1);
    if (strTitle.IsEmpty())
        strTitle = m_strDir;

    // 标题文字区域：右侧预留按钮区（从刷新按钮左边缘往右都留给按钮），
    // 避免长目录名与按钮重叠
    CRect rcTitle(rcHeader);
    rcTitle.left += 4;
    rcTitle.right = GetButtonRect(WIDGET_BTN_REFRESH).left - 2;

    dc.SetBkMode(TRANSPARENT);
    // 标题文字颜色：按标题栏背景亮度自适应（亮底黑字、暗底白字），保证可读
    dc.SetTextColor(GetContrastTextColor(m_clrHeader));
    dc.SelectStockObject(DEFAULT_GUI_FONT);
    dc.DrawText(strTitle, &rcTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 2. 标题条右侧的小按钮（刷新/打开目录/切换视图/置顶/置底/折叠）
    //    按 ID 从左向右依次绘制，带悬停/按下/激活状态反馈
    for (int nBtn = WIDGET_BTN_REFRESH; nBtn <= WIDGET_BTN_COLLAPSE; nBtn++)
        DrawButton(dc, GetButtonRect(nBtn), nBtn);

    // 3. 窗口整体边框
    //    WorkBuddy: 改为统一的浅灰细描边（原来"上边标题栏色 + 其余深灰"的立体
    //    边框是 Win9x 时代的视觉符号）；标题栏下沿已单独压暗线，边框只需承担
    //    干净利落的收边职责
    dc.Draw3dRect(rcClient, RGB(150, 155, 162), RGB(150, 155, 162));
}

// WorkBuddy: 分层窗口渲染核心。步骤：
//   1) 建 32 位不透明临时位图，填背景色 + 画标题/按钮/边框 + PrintWindow 抓列表；
//   2) 逐像素叠加区域 alpha（标题栏区域 m_byHeaderAlpha、其余 m_byBgAlpha）并预乘；
//   3) UpdateLayeredWindow 呈现，得到"标题栏/背景各自独立透明度"的效果。
// 说明：分层窗口的子控件（文件列表）不会自动进入分层位图，必须 PrintWindow 抓取；
// m_bRendering 防止 PrintWindow 抓取列表时经列表 OnPaint 递归回本函数
// WorkBuddy: 身份翻转稳定后的延迟重渲染（WM_WIDGET_CHILD_RENDER）。
// PostMessage 到达时，挂接/脱离的 SetParent 与样式翻转早已完成，
// 此时 RenderLayered 的 ULW 才能真正被 DWM 采纳（宏定义处有完整说明）
// WorkBuddy: 第 4 轮实证修正（2026-09-13）——仅"再调一次 ULW"（无论在
// 挂接流程内同步直调，还是延迟到本消息再调）都可能被 DWM 忽略：本轮
// 实验 PostMessage(WM_SIZE)→OnSize→RenderLayered 仍透明，而真实
// SetWindowPos(+1px) 走完全相同的 OnSize→RenderLayered 却立即显示。
// 结论：SetParent 身份翻转后，DWM 要到下一次**真实几何变化**才重建
// 合成节点、重新采纳 ULW 位图。因此本函数先复刻实验三的成功路径——
// 做一次真实尺寸变化（宽 +1px）再复原，两次都走完整系统路径
//（WM_WINDOWPOSCHANGED → WM_SIZE → OnSize → RenderLayered），中间
// 1px 背景色差异肉眼不可察觉，最终尺寸与配置一致。
// SWP_NOMOVE：子窗口身份下 SetWindowPos 的 x/y 是父窗口客户区坐标，
// 不碰位置可完全避开"屏幕坐标 ↔ 父客户区坐标"的换算坑。
LRESULT CDeskTidyWidget::OnChildRenderStable(WPARAM wParam, LPARAM lParam)
{
    if (m_hWnd == NULL || !::IsWindow(m_hWnd))
        return 0;

    CRect rc;
    GetWindowRect(&rc);
    if (rc.Width() > 0 && rc.Height() > 0)
    {
        // 第一步：真实几何变化（宽 +1px）——触发 DWM 重建合成节点，
        // OnSize 内的 RenderLayered 因此生效（实验三已证有效的确切路径）
        ::SetWindowPos(m_hWnd, NULL, 0, 0, rc.Width() + 1, rc.Height(),
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        // 第二步：立即复原到配置尺寸——同样走真实路径，最终状态一致
        ::SetWindowPos(m_hWnd, NULL, 0, 0, rc.Width(), rc.Height(),
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        LogZ(_T("ChildRender: 真实几何触发重渲染 %dx%d"), rc.Width(), rc.Height());
    }

    RenderLayered();   // 双保险（两次 OnSize 已各渲染一次；此处幂等直调兜底）
    return 0;
}

void CDeskTidyWidget::RenderLayered()
{
    if (m_hWnd == NULL || !::IsWindow(m_hWnd))
        return;
    if (m_bRendering)
        return;     // 渲染进行中：直接返回，避免 PrintWindow 抓取列表时递归

    CRect rcClient;
    GetClientRect(&rcClient);
    int W = rcClient.Width();
    int H = rcClient.Height();
    if (W <= 0 || H <= 0)
        return;

    m_bRendering = TRUE;

    HDC hScreen = ::GetDC(NULL);

    // 32 位不透明位图信息：自上而下（负高度），行 0 即窗口顶部
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = W;
    bmi.bmiHeader.biHeight      = -H;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pTemp  = NULL;
    void* pFinal = NULL;
    HBITMAP hTemp  = ::CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &pTemp,  NULL, 0);
    HBITMAP hFinal = ::CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &pFinal, NULL, 0);
    if (hTemp == NULL || hFinal == NULL)
    {
        if (hTemp)  ::DeleteObject(hTemp);
        if (hFinal) ::DeleteObject(hFinal);
        ::ReleaseDC(NULL, hScreen);
        m_bRendering = FALSE;
        return;
    }

    CDC dcTemp;  dcTemp.CreateCompatibleDC(NULL);
    CDC dcFinal; dcFinal.CreateCompatibleDC(NULL);
    HBITMAP hOldTemp  = (HBITMAP)dcTemp.SelectObject(hTemp);
    HBITMAP hOldFinal = (HBITMAP)dcFinal.SelectObject(hFinal);

    // 1) 背景色（不透明）：随后整图按区域叠加 alpha
    dcTemp.FillSolidRect(0, 0, W, H, m_clrBg);

    // 2) 标题条 / 标题文字 / 按钮 / 边框（不透明绘制到临时位图）
    DrawContent(dcTemp, rcClient);

    // 3) 抓取文件列表内容：分层窗口的子控件不会自动合成，必须 PrintWindow 取内容
    if (m_list.GetSafeHwnd() != NULL && m_list.IsWindowVisible())
    {
        CRect rcL;
        m_list.GetClientRect(&rcL);
        if (rcL.Width() > 0 && rcL.Height() > 0)
        {
            BITMAPINFO bmiL = {0};
            bmiL.bmiHeader = bmi.bmiHeader;
            bmiL.bmiHeader.biWidth  = rcL.Width();
            bmiL.bmiHeader.biHeight = -rcL.Height();

            void* pList = NULL;
            HBITMAP hList = ::CreateDIBSection(hScreen, &bmiL, DIB_RGB_COLORS, &pList, NULL, 0);
            if (hList != NULL)
            {
                CDC dcList; dcList.CreateCompatibleDC(NULL);
                HBITMAP hOldList = (HBITMAP)dcList.SelectObject(hList);

                // 只抓客户区（列表无边框，客户区即全部内容）
                m_list.PrintWindow(&dcList, PW_CLIENTONLY);

                // 计算列表在窗口客户区中的左上角坐标，把列表内容贴到临时位图对应位置
                CRect rcWin, rcListWin;
                GetWindowRect(&rcWin);
                m_list.GetWindowRect(&rcListWin);
                int dx = rcListWin.left - rcWin.left;
                int dy = rcListWin.top  - rcWin.top;
                dcTemp.BitBlt(dx, dy, rcL.Width(), rcL.Height(), &dcList, 0, 0, SRCCOPY);

                dcList.SelectObject(hOldList);
                ::DeleteObject(hList);
            }
        }
    }

    // 4) 逐像素叠加 alpha 并预乘：标题栏区域用 m_byHeaderAlpha、其余（背景+列表）用 m_byBgAlpha。
    //    UpdateLayeredWindow 要求预乘 alpha：RGB' = RGB * A / 255
    BYTE* pSrc = (BYTE*)pTemp;
    BYTE* pDst = (BYTE*)pFinal;
    int nStride = W * 4;
    for (int y = 0; y < H; y++)
    {
        BYTE a = (y < WIDGET_HEADER_HEIGHT) ? m_byHeaderAlpha : m_byBgAlpha;
        BYTE* pRowS = pSrc + y * nStride;
        BYTE* pRowD = pDst + y * nStride;
        for (int x = 0; x < W; x++)
        {
            int i = x * 4;
            pRowD[i]     = (BYTE)((pRowS[i]     * a) / 255);   // B
            pRowD[i + 1] = (BYTE)((pRowS[i + 1] * a) / 255);   // G
            pRowD[i + 2] = (BYTE)((pRowS[i + 2] * a) / 255);   // R
            pRowD[i + 3] = a;                                  // A
        }
    }

    // 5) 通过 UpdateLayeredWindow 呈现（逐像素 alpha，统一 alpha 交给 alpha 通道控制）
    CRect rcWin;
    GetWindowRect(&rcWin);
    POINT ptSrc   = { 0, 0 };
    POINT ptDst   = { rcWin.left, rcWin.top };
    SIZE  sizeWnd = { W, H };
    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;
    BOOL bULW = ::UpdateLayeredWindow(m_hWnd, hScreen, &ptDst, &sizeWnd,
                                      dcFinal.m_hDC, &ptSrc, 0, &bf, ULW_ALPHA);

    // WorkBuddy: 诊断——UpdateLayeredWindow 失败是"透明度设置不生效"的首要嫌疑
    // （失败时窗口会保持上一次的画面或完全不可见）。为避免悬停重绘频繁刷屏，
    // 只在"失败状态发生变化"时各写一次日志
    static BOOL  s_bULWOk   = TRUE;   // 上一次调用是否成功
    static DWORD s_dwULWErr = 0;      // 上一次记录的失败错误码
    if (!bULW)
    {
        DWORD dwErr = ::GetLastError();
        if (s_bULWOk || dwErr != s_dwULWErr)
        {
            s_dwULWErr = dwErr;
            LogZ(_T("RenderLayered: UpdateLayeredWindow 失败 err=%u (hdrA=%d bgA=%d %dx%d)"),
                 dwErr, (int)m_byHeaderAlpha, (int)m_byBgAlpha, W, H);
        }
        s_bULWOk = FALSE;
    }
    else if (!s_bULWOk)
    {
        s_bULWOk = TRUE;
        LogZ(_T("RenderLayered: UpdateLayeredWindow 恢复正常"));
    }

    // 清理 GDI 资源
    dcFinal.SelectObject(hOldFinal);
    dcTemp.SelectObject(hOldTemp);
    ::DeleteObject(hFinal);
    ::DeleteObject(hTemp);
    ::ReleaseDC(NULL, hScreen);

    m_bRendering = FALSE;
}

// WorkBuddy: 分层窗口下 WM_PAINT 仅用于验证绘制区域（防止 WM_PAINT 死循环），
// 实际内容由 RenderLayered 通过 UpdateLayeredWindow 呈现
void CDeskTidyWidget::OnPaint()
{
    CPaintDC dc(this);      // 验证绘制区域（必须创建，否则 WM_PAINT 会持续排队）
    RenderLayered();
}

void CDeskTidyWidget::OnLButtonDown(UINT nFlags, CPoint point)
{
    // 1. 优先判断是否按下了标题栏按钮：按钮命中优先于拖拽/缩放
    int nBtn = HitTestButton(point);
    if (nBtn != WIDGET_BTN_NONE)
    {
        // 记录按下的按钮并捕获鼠标，在 WM_LBUTTONUP 中判断是否触发动作
        m_nBtnPressed = nBtn;
        SetCapture();
        InvalidateHeader();      // 重绘标题条，显示按下状态
        return;
    }

    // 2. 非按钮区域：按原有逻辑进入拖动/缩放
    CPoint ptScreen = point;
    ClientToScreen(&ptScreen);

    // 判断点击位置属于哪种拖拽区域
    int nHit = HitTestPoint(ptScreen);
    if (nHit == HTCLIENT)
    {
        // 普通客户区：交给默认处理（此处列表控件会独立处理点击）
        CWnd::OnLButtonDown(nFlags, point);
        return;
    }

    // 记录拖拽起始状态并捕获鼠标，之后在 WM_MOUSEMOVE 中实时移动/缩放
    m_nDragMode = nHit;
    GetWindowRect(&m_rcDragStart);
    m_ptDragStart = ptScreen;
    SetCapture();
    // WorkBuddy: 拖动/缩放期间窗口会跟着鼠标走，固定在屏幕坐标的提示框
    // 会脱离按钮，先收起；拖拽结束后若鼠标仍在按钮上会重新显示
    HideButtonTip();
}

void CDeskTidyWidget::OnMouseMove(UINT nFlags, CPoint point)
{
    // 拖拽中且左键按住：应用移动/缩放
    if (m_nDragMode != 0 && (nFlags & MK_LBUTTON))
    {
        CPoint ptScreen = point;
        ClientToScreen(&ptScreen);
        ApplyDragDelta(ptScreen);
        return;
    }

    // 标题栏按钮悬停跟踪：计算当前鼠标下的按钮，
    // 与上次悬停不同时重绘标题条；鼠标进入按钮区时请求 WM_MOUSELEAVE，
    // 以便鼠标离开窗口时清除悬停状态
    int nBtn = HitTestButton(point);
    if (nBtn != m_nBtnHover)
    {
        m_nBtnHover = nBtn;
        if (m_nBtnHover != WIDGET_BTN_NONE)
        {
            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_hWnd, 0 };
            ::TrackMouseEvent(&tme);
        }
        InvalidateHeader();

        // WorkBuddy: 悬停按钮切换时同步刷新文字提示——
        // 移到按钮上显示对应说明，移开（WIDGET_BTN_NONE）立即隐藏
        ShowButtonTip(m_nBtnHover);
    }

    CWnd::OnMouseMove(nFlags, point);
}

void CDeskTidyWidget::OnLButtonUp(UINT nFlags, CPoint point)
{
    // 1. 标题栏按钮松开：若鼠标仍停留在按下的按钮上则触发对应动作
    if (m_nBtnPressed != WIDGET_BTN_NONE)
    {
        int nBtn = m_nBtnPressed;
        m_nBtnPressed = WIDGET_BTN_NONE;
        ReleaseCapture();
        InvalidateHeader();      // 重绘标题条，清除按下状态

        // 按下与松开位置一致时才视为一次有效点击（防止误触）
        if (HitTestButton(point) == nBtn)
        {
            OnButtonClick(nBtn);

            // WorkBuddy: 点击后（视图/层级/折叠状态可能已改变）按新状态
            // 重新显示提示，文字随之更新为"点击后会变成什么"
            ShowButtonTip(nBtn);
        }
        return;
    }

    // 2. 结束拖拽/缩放，释放鼠标捕获
    if (m_nDragMode != 0)
    {
        ReleaseCapture();
        m_nDragMode = 0;

        // 记录最终矩形，并通知主对话框保存配置（下次启动恢复位置/大小）
        GetWindowRect(&m_rcLast);
        if (m_hNotify != NULL && ::IsWindow(m_hNotify))
            ::PostMessage(m_hNotify, WM_WIDGET_CHANGED, 0, 0);
        return;
    }

    CWnd::OnLButtonUp(nFlags, point);
}

void CDeskTidyWidget::OnMouseLeave()
{
    // WorkBuddy: 鼠标离开窗口时收起文字提示（无条件调用：即使
    // m_nBtnHover 已为 NONE，也可能存在尚未关闭的提示框）
    HideButtonTip();

    // 鼠标离开窗口：清除按钮悬停状态并重绘标题条
    if (m_nBtnHover != WIDGET_BTN_NONE)
    {
        m_nBtnHover = WIDGET_BTN_NONE;
        InvalidateHeader();
    }

    CWnd::OnMouseLeave();
}

// 鼠标按下瞬间（WM_MOUSEACTIVATE）：WitchDrawer 方案——临时解除桌面 Owner
// 并返回 MA_NOACTIVATE（见头文件与 SuspendDesktopOwnerForMouse 的说明）。
// 为什么必须在这里处理：小窗口以桌面 Shell 窗口（Progman）为 Owner 后，
// Explorer 可能把"最近点击的 owned 窗口"记为 Progman 的 last-active-popup；
// 下次 Win+D（显示桌面）会激活该 popup，把小窗口带到前台。返回 MA_NOACTIVATE
// 保证点击不激活本窗口、不抢焦点；同时把 Owner 临时解除，让 Explorer 无法
// 把小窗口记为 popup。最后 PostMessage(WM_WIDGET_RESTORE_OWNER) 等当前输入
// 批次（按下/移动/抬起）全部处理完再恢复 Owner
int CDeskTidyWidget::OnMouseActivate(CWnd* pDesktopWnd, UINT nHitTest, UINT message)
{
    // 置底模式下临时解除桌面 Owner（置顶模式 m_hDesktopOwner 为空，自动跳过）
    SuspendDesktopOwnerForMouse();

    // 输入批次处理完后恢复 Owner（PostMessage 排队，不打断当前鼠标交互；
    // 与 OnCaptureChanged 的立即恢复路径并存，RestoreDesktopOwnerAfterMouse 幂等）
    if (m_bOwnerSuspended)
        PostMessage(WM_WIDGET_RESTORE_OWNER, 0, 0);

    // MA_NOACTIVATE：交付鼠标消息，但不激活本窗口、不把本窗口设为前台
    return MA_NOACTIVATE;
}

// 鼠标捕获结束（松开按钮 / 取消捕获 / 捕获被其它窗口夺走）：
// 立即恢复桌面 Owner。覆盖"标题栏拖拽/缩放结束后 ReleaseCapture"的场景；
// 与 WM_WIDGET_RESTORE_OWNER（输入批次后恢复）并存，二者都调用幂等的
// RestoreDesktopOwnerAfterMouse，先后调用无副作用
void CDeskTidyWidget::OnCaptureChanged(CWnd* pWnd)
{
    RestoreDesktopOwnerAfterMouse();
    CWnd::OnCaptureChanged(pWnd);
}

// 鼠标输入批次处理完后恢复桌面 Owner（由 WM_WIDGET_RESTORE_OWNER 触发，
// 见 OnMouseActivate 的说明）。覆盖"点击列表控件"等没有本窗口捕获、
// 也没有本窗口按钮抬起消息的场景
LRESULT CDeskTidyWidget::OnRestoreOwnerAfterMouse(WPARAM wParam, LPARAM lParam)
{
    RestoreDesktopOwnerAfterMouse();
    return 0;
}

// 光标形状：鼠标移到窗口边缘/标题条上时，把光标切换为对应的
// 缩放/移动形状，提示当前区域可进行拖拽操作
BOOL CDeskTidyWidget::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message)
{
    // 本窗口无边框/标题栏（整个窗口都是客户区），
    // 在客户区内按鼠标所在位置自行判断拖拽区域
    if (nHitTest == HTCLIENT)
    {
        CPoint ptScreen;
        ::GetCursorPos(&ptScreen);
        CPoint ptClient = ptScreen;
        ScreenToClient(&ptClient);

        // 标题栏按钮上显示手型光标，提示可点击
        if (HitTestButton(ptClient) != WIDGET_BTN_NONE)
        {
            ::SetCursor(::LoadCursor(NULL, IDC_HAND));
            return TRUE;
        }

        int nHit = HitTestPoint(ptScreen);
        LPCTSTR pszCursor = IDC_ARROW;
        switch (nHit)
        {
        case HTLEFT:                        // 左/右边缘：水平缩放
        case HTRIGHT:
            pszCursor = IDC_SIZEWE;
            break;
        case HTTOP:                         // 上/下边缘：垂直缩放
        case HTBOTTOM:
            pszCursor = IDC_SIZENS;
            break;
        case HTTOPLEFT:                     // 主对角线方向缩放
        case HTBOTTOMRIGHT:
            pszCursor = IDC_SIZENWSE;
            break;
        case HTTOPRIGHT:                    // 副对角线方向缩放
        case HTBOTTOMLEFT:
            pszCursor = IDC_SIZENESW;
            break;
        case HTCAPTION:                     // 标题条：四向移动
            pszCursor = IDC_SIZEALL;
            break;
        default:                            // 其余区域：普通箭头
            pszCursor = IDC_ARROW;
            break;
        }
        ::SetCursor(::LoadCursor(NULL, pszCursor));
        return TRUE;
    }

    // 其它命中区域交给默认处理
    return CWnd::OnSetCursor(pWnd, nHitTest, message);
}

void CDeskTidyWidget::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
    // 限制最小尺寸，避免窗口被缩得太小
    lpMMI->ptMinTrackSize.x = WIDGET_MIN_WIDTH;
    lpMMI->ptMinTrackSize.y = WIDGET_MIN_HEIGHT;

    CWnd::OnGetMinMaxInfo(lpMMI);
}

// 自定义命中测试：
//   - 靠近窗口边缘（4px 内）→ 返回对应的缩放区域码（HTLEFT/HTRIGHT/...）
//   - 标题条区域 → HTCAPTION（移动）
//   - 其余 → HTCLIENT
int CDeskTidyWidget::HitTestPoint(CPoint ptScreen) const
{
    CRect rcWnd;
    GetWindowRect(&rcWnd);

    const int nEdge = WIDGET_EDGE_SIZE;

    // 判断是否处于各边缘
    bool bLeft   = (ptScreen.x >= rcWnd.left   && ptScreen.x <  rcWnd.left   + nEdge);
    bool bRight  = (ptScreen.x >  rcWnd.right  - nEdge && ptScreen.x <= rcWnd.right);
    bool bTop    = (ptScreen.y >= rcWnd.top    && ptScreen.y <  rcWnd.top    + nEdge);
    bool bBottom = (ptScreen.y >  rcWnd.bottom - nEdge && ptScreen.y <= rcWnd.bottom);

    // 四个角优先
    if (bLeft  && bTop)    return HTTOPLEFT;
    if (bRight && bTop)    return HTTOPRIGHT;
    if (bLeft  && bBottom) return HTBOTTOMLEFT;
    if (bRight && bBottom) return HTBOTTOMRIGHT;
    if (bLeft)             return HTLEFT;
    if (bRight)            return HTRIGHT;
    if (bTop)              return HTTOP;
    if (bBottom)           return HTBOTTOM;

    // 标题条区域（客户区顶部）→ 允许拖动移动窗口
    CPoint ptClient = ptScreen;
    ScreenToClient(&ptClient);
    if (ptClient.y >= 0 && ptClient.y < WIDGET_HEADER_HEIGHT)
        return HTCAPTION;

    return HTCLIENT;
}

// 根据鼠标位移实时移动或缩放窗口
void CDeskTidyWidget::ApplyDragDelta(const CPoint& ptCursor)
{
    int dx = ptCursor.x - m_ptDragStart.x;
    int dy = ptCursor.y - m_ptDragStart.y;

    // 从拖拽起始矩形出发，按拖拽区域调整
    CRect rcNew = m_rcDragStart;
    switch (m_nDragMode)
    {
    case HTCAPTION:                    // 移动整个窗口
        rcNew.OffsetRect(dx, dy);
        break;
    case HTLEFT:                       // 左边缘：只改左边
        rcNew.left += dx;
        break;
    case HTRIGHT:                      // 右边缘：只改右边
        rcNew.right += dx;
        break;
    case HTTOP:                        // 上边缘
        rcNew.top += dy;
        break;
    case HTBOTTOM:                     // 下边缘
        rcNew.bottom += dy;
        break;
    case HTTOPLEFT:
        rcNew.left += dx; rcNew.top += dy;
        break;
    case HTTOPRIGHT:
        rcNew.right += dx; rcNew.top += dy;
        break;
    case HTBOTTOMLEFT:
        rcNew.left += dx; rcNew.bottom += dy;
        break;
    case HTBOTTOMRIGHT:
        rcNew.right += dx; rcNew.bottom += dy;
        break;
    default:
        return;
    }

    // 保证不小于最小尺寸
    if (rcNew.Width() < WIDGET_MIN_WIDTH)
        rcNew.right = rcNew.left + WIDGET_MIN_WIDTH;
    if (rcNew.Height() < WIDGET_MIN_HEIGHT)
        rcNew.bottom = rcNew.top + WIDGET_MIN_HEIGHT;

    // 应用新位置与尺寸（保持 z 序不变，不激活；
    // SWP_NOCOPYBITS 让尺寸变化时直接重绘，避免 CopyBits 残留画面造成的闪烁）
    ::SetWindowPos(m_hWnd, NULL, rcNew.left, rcNew.top,
                   rcNew.Width(), rcNew.Height(),
                   SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING | SWP_NOCOPYBITS);
}

// ---------------------------------------------------------------------------
// 标题栏小按钮：布局 / 命中测试 / 绘制 / 点击
// ---------------------------------------------------------------------------

// 计算标题条上某个按钮的矩形。
// 按钮排列在标题条右侧，从左向右依次为：刷新/打开目录/切换视图/置顶/置底/折叠
// （按钮 ID 越小越靠左），按钮区右边缘距标题条右端留白 WIDGET_BTN_MARGIN。
CRect CDeskTidyWidget::GetButtonRect(int nBtnId) const
{
    CRect rcClient;
    GetClientRect(&rcClient);

    // 按钮 ID 从 1 连续递增，最大值 WIDGET_BTN_COLLAPSE 即按钮总数
    const int nCount = WIDGET_BTN_COLLAPSE;

    // 按钮区总宽度 = 按钮宽度×数量 + 间隔×(数量-1)
    int nTotalWidth = nCount * WIDGET_BTN_WIDTH + (nCount - 1) * WIDGET_BTN_GAP;

    // 按钮区左起点：从右端向左留白 WIDGET_BTN_MARGIN
    int nStartX = rcClient.right - WIDGET_BTN_MARGIN - nTotalWidth;

    // 按钮在标题条内垂直居中
    int nTop = (WIDGET_HEADER_HEIGHT - WIDGET_BTN_HEIGHT) / 2;

    // 第 idx 个按钮的横向位置 = 起点 + idx×(宽度+间隔)
    int nIdx = nBtnId - 1;
    int x = nStartX + nIdx * (WIDGET_BTN_WIDTH + WIDGET_BTN_GAP);

    return CRect(x, nTop, x + WIDGET_BTN_WIDTH, nTop + WIDGET_BTN_HEIGHT);
}

// 命中测试：返回客户区坐标点所在的标题栏按钮 ID（未命中返回 WIDGET_BTN_NONE）
int CDeskTidyWidget::HitTestButton(const CPoint& ptClient) const
{
    // 仅标题条高度内才可能命中按钮
    if (ptClient.y < 0 || ptClient.y >= WIDGET_HEADER_HEIGHT)
        return WIDGET_BTN_NONE;

    // 依次判断每个按钮的矩形是否包含该点
    for (int nBtn = WIDGET_BTN_REFRESH; nBtn <= WIDGET_BTN_COLLAPSE; nBtn++)
    {
        if (GetButtonRect(nBtn).PtInRect(ptClient))
            return nBtn;
    }
    return WIDGET_BTN_NONE;
}

// 创建标题栏按钮的文字提示控件（Tooltip）。
// WorkBuddy: 标题栏按钮是自绘图形、并非独立子窗口，系统无法自动给出提示，
// 因此创建一个"手动跟踪"模式的 tooltip：
//   TTF_TRACK    - 由代码显式激活/关闭，不依赖鼠标停留计时
//   TTF_ABSOLUTE - TTM_TRACKPOSITION 传入的是屏幕坐标（按钮位置随时可能变，
//                  每次显示前重新计算，比相对坐标更可控）
//   TTS_ALWAYSTIP- 小窗口非活动状态（置底层模式）下也要能显示提示
//   TTS_NOPREFIX - 不做 & 加速符处理，文本原样显示
void CDeskTidyWidget::InitTooltip()
{
    // 已创建过（窗口重建时）则不重复创建
    if (m_toolTip.m_hWnd != NULL)
        return;

    if (!m_toolTip.Create(this, TTS_ALWAYSTIP | TTS_NOPREFIX))
        return;

    // 注册一个虚拟工具：手动跟踪模式下只需要一个"槽位"来承载文本，
    // uId 用窗口句柄本身（配合 TTF_IDISHWND），后续更新文本/位置时保持一致
    TOOLINFO ti;
    ::ZeroMemory(&ti, sizeof(ti));
    ti.cbSize   = sizeof(ti);
    ti.uFlags   = TTF_TRACK | TTF_ABSOLUTE | TTF_IDISHWND;
    ti.hwnd     = m_hWnd;
    ti.uId      = (UINT_PTR)m_hWnd;
    ti.lpszText = const_cast<LPTSTR>(_T(""));
    m_toolTip.SendMessage(TTM_ADDTOOL, 0, (LPARAM)&ti);

    // 允许文本自动换行，避免长提示横向拉得过宽
    m_toolTip.SendMessage(TTM_SETMAXTIPWIDTH, 0, 240);
    m_toolTip.Activate(TRUE);
}

// 取指定标题栏按钮的提示文字。
// WorkBuddy: 视图/层级/折叠三个按钮是"状态切换"型，提示文字随当前状态变化，
// 这样鼠标放上去既能看到按钮含义，也能看到点击后会变成什么
CString CDeskTidyWidget::GetButtonTipText(int nBtnId) const
{
    switch (nBtnId)
    {
    case WIDGET_BTN_REFRESH:
        return _T("刷新列表");

    case WIDGET_BTN_OPENDIR:
        return _T("打开所在目录");

    case WIDGET_BTN_VIEW:
        return (m_nViewMode == 0) ? _T("切换为网格视图") : _T("切换为列表视图");

    case WIDGET_BTN_TOPMOST:
        return (m_nLayerMode == 1) ? _T("当前悬浮置顶，点击切换为最底层")
                                   : _T("切换为悬浮置顶");

    case WIDGET_BTN_BOTTOM:
        return (m_nLayerMode == 0) ? _T("当前置于最底层") : _T("置于最底层");

    case WIDGET_BTN_COLLAPSE:
        return m_bCollapsed ? _T("展开窗口") : _T("折叠为仅标题栏");

    default:
        return _T("");
    }
}

// 在指定按钮正下方显示文字提示（nBtnId 为 WIDGET_BTN_NONE 时隐藏）。
// WorkBuddy: 位置每次显示前重新计算（按钮在标题条右侧，窗口宽度变化时位置跟着变）
void CDeskTidyWidget::ShowButtonTip(int nBtnId)
{
    if (m_toolTip.m_hWnd == NULL || nBtnId == WIDGET_BTN_NONE)
    {
        HideButtonTip();
        return;
    }

    // 提示框锚点：按钮矩形左下角下方 4 像素处（屏幕坐标）
    CRect rcBtn = GetButtonRect(nBtnId);
    CPoint ptAnchor(rcBtn.left, rcBtn.bottom + 4);
    ClientToScreen(&ptAnchor);

    CString strTip = GetButtonTipText(nBtnId);

    TOOLINFO ti;
    ::ZeroMemory(&ti, sizeof(ti));
    ti.cbSize   = sizeof(ti);
    ti.uFlags   = TTF_IDISHWND;
    ti.hwnd     = m_hWnd;
    ti.uId      = (UINT_PTR)m_hWnd;
    ti.lpszText = const_cast<LPTSTR>((LPCTSTR)strTip);

    // 先更新文本，再定位，最后激活——顺序颠倒会出现"上一按钮的旧文本"
    m_toolTip.SendMessage(TTM_UPDATETIPTEXT, 0, (LPARAM)&ti);
    m_toolTip.SendMessage(TTM_TRACKPOSITION, 0,
                          MAKELONG(ptAnchor.x, ptAnchor.y));
    m_toolTip.SendMessage(TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);

    // 提一次 z 序：小窗口处于"悬浮置顶"模式时也是 topmost 窗口，
    // 同为 topmost 时提示框可能被压在小窗口之下，这里保证提示框在最上层
    m_toolTip.SetWindowPos(&CWnd::wndTopMost, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}

// 隐藏标题栏按钮的文字提示
void CDeskTidyWidget::HideButtonTip()
{
    if (m_toolTip.m_hWnd == NULL)
        return;

    TOOLINFO ti;
    ::ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND;
    ti.hwnd   = m_hWnd;
    ti.uId    = (UINT_PTR)m_hWnd;

    m_toolTip.SendMessage(TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
}

// 处理标题栏按钮的点击动作（与右键菜单逻辑一致，复用既有实现）
void CDeskTidyWidget::OnButtonClick(int nBtnId)
{
    switch (nBtnId)
    {
    case WIDGET_BTN_REFRESH:
        // 刷新列表：重新枚举当前目录
        RefreshFiles();
        break;

    case WIDGET_BTN_OPENDIR:
        // 打开本小窗口所在目录
        if (!m_strDir.IsEmpty())
            ShellExecute(NULL, _T("open"), m_strDir, NULL, NULL, SW_SHOWNORMAL);
        break;

    case WIDGET_BTN_VIEW:
        // 切换列表 / 网格视图，并通知主对话框保存配置
        SetViewMode(m_nViewMode == 0 ? 1 : 0);
        if (m_hNotify != NULL && ::IsWindow(m_hNotify))
            ::PostMessage(m_hNotify, WM_WIDGET_CHANGED, 0, 0);
        break;

    case WIDGET_BTN_TOPMOST:
        // 切换浮动置顶 / 最底层，并通知主对话框保存配置
        SetLayerMode(m_nLayerMode == 0 ? 1 : 0);
        if (m_hNotify != NULL && ::IsWindow(m_hNotify))
            ::PostMessage(m_hNotify, WM_WIDGET_CHANGED, 0, 0);
        break;

    case WIDGET_BTN_BOTTOM:
        // 直接置底，并通知主对话框保存配置
        SetLayerMode(0);
        if (m_hNotify != NULL && ::IsWindow(m_hNotify))
            ::PostMessage(m_hNotify, WM_WIDGET_CHANGED, 0, 0);
        break;

    case WIDGET_BTN_COLLAPSE:
        // 折叠/展开窗口（SetCollapsed 内部会通知主对话框保存配置）
        SetCollapsed(!m_bCollapsed);
        break;
    }

    // 层级/视图变化可能改变按钮的激活高亮，重绘标题条
    InvalidateHeader();
}

// 绘制标题条上的一个小按钮：先画状态背景，再画图标。
// 状态优先级：激活（层级按钮当前生效）> 按下 > 悬停 > 普通（透明）
void CDeskTidyWidget::DrawButton(CDC& dc, const CRect& rcBtn, int nBtnId)
{
    BOOL bHover   = (m_nBtnHover == nBtnId);
    BOOL bPressed = (m_nBtnPressed == nBtnId);

    // 层级按钮的"激活"态：置顶按钮在置顶模式下高亮，置底按钮在底层模式下高亮；
    // 折叠按钮在折叠状态下高亮
    BOOL bActive = FALSE;
    if (nBtnId == WIDGET_BTN_TOPMOST && m_nLayerMode == 1)
        bActive = TRUE;
    if (nBtnId == WIDGET_BTN_BOTTOM && m_nLayerMode == 0)
        bActive = TRUE;
    if (nBtnId == WIDGET_BTN_COLLAPSE && m_bCollapsed)
        bActive = TRUE;

    // 按状态选择背景色与描边色。
    // WorkBuddy: 悬停/按下色不再写死为固定蓝，而是从用户设定的标题栏颜色派生
    //   （悬停提亮、按下压暗），因此在任意自定义标题栏配色下都能得到自然的
    //   交互反馈；激活态仍用品牌亮蓝，便于一眼识别当前层级/折叠状态
    COLORREF clrBg   = 0;
    COLORREF clrEdge = 0;
    if (bActive)
    {
        clrBg   = RGB(0, 140, 210);                 // 激活态：品牌亮蓝
        clrEdge = RGB(0, 108, 168);
    }
    else if (bPressed)
    {
        clrBg   = ShadeColor(m_clrHeader, -34);     // 按下态：较标题栏明显下沉
        clrEdge = ShadeColor(m_clrHeader, -52);
    }
    else if (bHover)
    {
        clrBg   = ShadeColor(m_clrHeader, +30);     // 悬停态：较标题栏微微提亮
        clrEdge = ShadeColor(m_clrHeader, -28);
    }

    if (clrBg != 0)
    {
        // WorkBuddy: 圆角填充 + 1px 细描边，替代原来的直角填充 + 立体凹凸边框，
        //   与主设置界面的扁平按钮保持同一套视觉语言
        CPen penEdge(PS_SOLID, 1, clrEdge);
        CPen*   pOldPen   = dc.SelectObject(&penEdge);
        CBrush  brFill(clrBg);
        CBrush* pOldBrush = dc.SelectObject(&brFill);
        dc.RoundRect(rcBtn, CPoint(6, 6));
        dc.SelectObject(pOldBrush);
        dc.SelectObject(pOldPen);
    }

    // 绘制图标（激活态使用高亮颜色）
    DrawButtonIcon(dc, rcBtn, nBtnId, bActive);
}

// 用 GDI 简笔画绘制按钮图标（细实线、只画边框不填充）
void CDeskTidyWidget::DrawButtonIcon(CDC& dc, const CRect& rcBtn, int nBtnId, BOOL bHighlight)
{
    // 图标颜色：激活态用亮青色；其余按标题栏亮度自适应黑/白（浅色标题栏上
    // 白色图标会看不见，自适应保证按钮图标始终可读）
    COLORREF clr = bHighlight ? RGB(120, 230, 255) : GetContrastTextColor(m_clrHeader);
    CPen pen(PS_SOLID, 1, clr);
    CPen* pOldPen = dc.SelectObject(&pen);
    // 选择空画刷：Rectangle/Ellipse 等只画轮廓，不做填充
    CBrush* pOldBrush = (CBrush*)dc.SelectStockObject(NULL_BRUSH);
    dc.SetBkMode(TRANSPARENT);

    // 按钮中心与左上角坐标（图标在按钮内居中绘制）
    int cx = rcBtn.CenterPoint().x;
    int cy = rcBtn.CenterPoint().y;
    int x  = rcBtn.left;
    int y  = rcBtn.top;

    switch (nBtnId)
    {
    case WIDGET_BTN_REFRESH:
    {
        // 刷新：上半圆弧 + 左端箭头，表示顺时针循环
        int r = 4;
        CRect rcArc(cx - r, cy - r, cx + r, cy + r);
        dc.Arc(rcArc, CPoint(cx + r, cy), CPoint(cx - r, cy));  // 从右端逆时针画上半圆到左端
        dc.MoveTo(cx - r, cy);                                  // 左端画箭头
        dc.LineTo(cx - r + 3, cy - 2);
        dc.MoveTo(cx - r, cy);
        dc.LineTo(cx - r + 3, cy + 2);
        break;
    }

    case WIDGET_BTN_OPENDIR:
    {
        // 文件夹：顶部小标签 + 下方矩形主体
        dc.Rectangle(x + 2, y + 5, x + 13, y + 12);   // 主体（只描边）
        dc.MoveTo(x + 2, y + 5);                      // 顶盖左侧起点
        dc.LineTo(x + 6, y + 5);
        dc.LineTo(x + 7, y + 3);                      // 标签斜边
        dc.LineTo(x + 11, y + 3);
        dc.LineTo(x + 12, y + 5);                     // 顶盖右侧
        break;
    }

    case WIDGET_BTN_VIEW:
    {
        // 切换视图：左半为列表（三条横线），右半为网格（两个小方框）
        for (int i = 0; i < 3; i++)
        {
            dc.MoveTo(x + 2, y + 3 + i * 4);
            dc.LineTo(x + 6, y + 3 + i * 4);
        }
        dc.Rectangle(x + 8, y + 3, x + 13, y + 7);   // 网格上半格
        dc.Rectangle(x + 8, y + 8, x + 13, y + 12);  // 网格下半格
        break;
    }

    case WIDGET_BTN_TOPMOST:
    {
        // 置顶：向上的箭头
        dc.MoveTo(cx, y + 3);
        dc.LineTo(cx, y + 10);
        dc.MoveTo(cx - 3, y + 6);
        dc.LineTo(cx, y + 3);
        dc.MoveTo(cx + 3, y + 6);
        dc.LineTo(cx, y + 3);
        break;
    }

    case WIDGET_BTN_BOTTOM:
    {
        // 置底：向下的箭头 + 底部横线（表示落到最底层）
        dc.MoveTo(cx, y + 3);
        dc.LineTo(cx, y + 10);
        dc.MoveTo(cx - 3, y + 7);
        dc.LineTo(cx, y + 10);
        dc.MoveTo(cx + 3, y + 7);
        dc.LineTo(cx, y + 10);
        dc.MoveTo(x + 3, y + 12);                  // 底部"地面"横线
        dc.LineTo(x + 12, y + 12);
        break;
    }

    case WIDGET_BTN_COLLAPSE:
    {
        // 折叠/展开：两行小箭头。展开态→向上（点击收起）；折叠态→向下（点击展开）。
        // nSign = -1 向上（收起），+1 向下（展开）
        int nSign = m_bCollapsed ? 1 : -1;
        for (int i = 0; i < 2; i++)
        {
            int yBase = cy + i * 3 - 1;            // 两行箭头的基线
            dc.MoveTo(cx - 3, yBase + nSign * 2);
            dc.LineTo(cx, yBase - nSign * 1);
            dc.LineTo(cx + 3, yBase + nSign * 2);
        }
        break;
    }
    }

    // 恢复原有的画刷与画笔
    dc.SelectObject(pOldBrush);
    dc.SelectObject(pOldPen);
}

// 仅重绘标题条区域（按钮状态变化时避免整窗刷新）
void CDeskTidyWidget::InvalidateHeader()
{
    CRect rcClient;
    GetClientRect(&rcClient);
    rcClient.bottom = WIDGET_HEADER_HEIGHT;
    InvalidateRect(&rcClient, FALSE);
}

// 双击文件：用系统默认程序打开
void CDeskTidyWidget::OnListDblClk(NMHDR* pNMHDR, LRESULT* pResult)
{
    NMITEMACTIVATE* pNMI = (NMITEMACTIVATE*)pNMHDR;
    if (pNMI->iItem >= 0 && pNMI->iItem < (int)m_arrPaths.GetCount())
    {
        // 通过 ShellExecute 以默认程序打开文件（或打开文件夹）
        CString strPath = m_arrPaths[pNMI->iItem];
        ShellExecute(NULL, _T("open"), strPath, NULL, NULL, SW_SHOWNORMAL);
    }
    *pResult = 0;
}

// ---------------------------------------------------------------------------
// WorkBuddy: 透明度选择对话框（无资源模板，运行时自建控件）
// ---------------------------------------------------------------------------
// 用 0~100 的滑块表示"不透明度百分比"（0% = 完全透明，100% = 完全不透明），
// 确定后映射为 0~255 的 alpha 返回；取消返回 -1。
// 采用 RunModalLoop 实现模态（对话框期间禁用父窗口），关闭后恢复父窗口。
// 不依赖任何对话框资源：控件在 WM_CREATE 中动态创建，便于随功能独立维护
class CAlphaDlg : public CWnd
{
public:
    // 弹出模态对话框并返回选中 alpha（0~255）；取消返回 -1
    static int PickAlpha(CWnd* pParent, BYTE byCur, LPCTSTR pszTitle)
    {
        CAlphaDlg dlg(pParent, byCur, pszTitle);

        // WorkBuddy: 窗口尺寸必须能让客户区容下全部控件。
        // 旧值 300x150 是"窗口矩形"，扣除 WS_CAPTION 后客户区仅约 296x116，
        // 而按钮放在 y=106..134 —— 被裁掉近 90%，用户几乎点不到"确定"，
        // 表现就是"设置对话框点了没反应"。这里改为 360x230，客户区约 356x196，
        // 控件最下沿到 y=172，底部留 24px 边距
        if (!dlg.CreateEx(0,
                          AfxRegisterWndClass(CS_DBLCLKS,
                                              ::LoadCursor(NULL, IDC_ARROW),
                                              (HBRUSH)(COLOR_BTNFACE + 1), NULL),
                          pszTitle,
                          WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_BORDER,
                          CRect(0, 0, 360, 230), pParent, 0))
            return -1;

        dlg.CenterWindow(pParent);
        if (pParent != NULL)
            pParent->EnableWindow(FALSE);   // 模态：禁用父窗口
        dlg.m_nRet = -1;

        // WorkBuddy: 显式显示 + 激活，不依赖 RunModalLoop 的 MLF_SHOWONIDLE。
        //   MLF_SHOWONIDLE 只在"消息队列为空"的空闲时刻才 ShowWindow：本程序
        //   小窗口带有 1 秒周期的 z 序维护定时器，队列经常非空，会出现对话框
        //   已进入模态循环却始终不显示的窗口期。这里先显示再进入循环，最稳妥
        dlg.ShowWindow(SW_SHOW);
        dlg.SetForegroundWindow();
        dlg.UpdateWindow();

        // WorkBuddy: CWnd::RunModalLoop 只识别 MLF_NOKICKIDLE / MLF_NOIDLEMSG / MLF_SHOWONIDLE
        //   三个标志（MFC 无 MLF_NOMODAL 这个标识符，误写会报 C2065）。
        //   模态屏蔽由上面的 EnableWindow(FALSE) 手工完成，此处不依赖额外标志：
        //     MLF_NOKICKIDLE - 空闲时不发 WM_KICKIDLE（避免空闲链干扰模态循环）
        //     MLF_NOIDLEMSG  - 空闲时不再向已禁用的父窗口转发 WM_ENTERIDLE
        //   WF_CONTINUEMODAL / WF_MODALLOOP 由 RunModalLoop 内部自行置位，
        //   EndModalLoop() 清除并 PostMessage(WM_NULL) 使循环退出，无需手工管理。
        dlg.RunModalLoop(MLF_NOKICKIDLE | MLF_NOIDLEMSG);

        if (pParent != NULL)
            pParent->EnableWindow(TRUE);    // 恢复父窗口
        return dlg.m_nRet;
    }

    CAlphaDlg(CWnd* pParent, BYTE byCur, LPCTSTR pszTitle)
        : m_pParent(pParent), m_byCur(byCur), m_strTitle(pszTitle), m_nRet(-1) {}

protected:
    afx_msg int  OnCreate(LPCREATESTRUCT lpcs);
    afx_msg void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pBar);
    afx_msg void OnBnClickedOk();
    afx_msg void OnBnClickedCancel();
    afx_msg void OnClose();     // 点标题栏关闭按钮：按"取消"结束模态循环
    virtual BOOL PreTranslateMessage(MSG* pMsg);    // 手工映射回车/ESC -> 确定/取消
    DECLARE_MESSAGE_MAP()

    // 刷新"不透明度：xx%"文字
    void UpdateValLabel()
    {
        CString s;
        s.Format(_T("不透明度：%d%%"), m_slider.GetPos());
        m_stVal.SetWindowText(s);
    }
    // 结束模态循环并销毁对话框
    void CloseDlg(int nRet) { m_nRet = nRet; EndModalLoop(0); DestroyWindow(); }

    CWnd*       m_pParent;
    BYTE        m_byCur;      // 初始 alpha（0~255）
    CString     m_strTitle;
    int         m_nRet;
    CStatic     m_stTitle;
    CSliderCtrl m_slider;
    CStatic     m_stVal;
    CButton     m_btnOk;
    CButton     m_btnCancel;
};

BEGIN_MESSAGE_MAP(CAlphaDlg, CWnd)
    ON_WM_CREATE()
    ON_WM_HSCROLL()
    ON_WM_CLOSE()
    ON_BN_CLICKED(IDOK, &CAlphaDlg::OnBnClickedOk)
    ON_BN_CLICKED(IDCANCEL, &CAlphaDlg::OnBnClickedCancel)
END_MESSAGE_MAP()

int CAlphaDlg::OnCreate(LPCREATESTRUCT lpcs)
{
    if (CWnd::OnCreate(lpcs) == -1)
        return -1;

    // WorkBuddy: 控件布局按 360x230 窗口（客户区约 356x196）排布，
    //   与旧的 300x150 布局相比整体下移并留出底部按钮区，避免按钮被裁掉。
    //   若任一控件创建失败则直接返回 -1（CreateEx 随之失败，调用方得到 -1），
    //   不再出现"对话框能显示但控件缺失"的半残状态
    if (!m_stTitle.Create(m_strTitle, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                          CRect(16, 14, 344, 38), this))
        return -1;

    // 滑块：0~100 表示不透明度百分比，刻度和数值统一用百分比展示
    if (!m_slider.Create(WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_BOTH | TBS_AUTOTICKS,
                         CRect(16, 56, 344, 82), this, 1001))
        return -1;
    m_slider.SetRange(0, 100, TRUE);
    m_slider.SetTicFreq(10);
    m_slider.SetPos((int)m_byCur * 100 / 255);   // alpha -> 百分比

    if (!m_stVal.Create(_T(""), WS_CHILD | WS_VISIBLE | SS_CENTER,
                        CRect(16, 88, 344, 112), this))
        return -1;
    UpdateValLabel();

    // 按钮：横向居中排在底部，两键间隔 20px
    if (!m_btnOk.Create(_T("确定"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                        CRect(104, 140, 174, 172), this, IDOK))
        return -1;
    if (!m_btnCancel.Create(_T("取消"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                            CRect(190, 140, 260, 172), this, IDCANCEL))
        return -1;

    // 默认焦点给滑块（可直接用方向键微调）
    m_slider.SetFocus();
    return 0;
}

// WorkBuddy: 原始 CWnd 不是对话框，不会像 CDialog 那样自动把回车/ESC 映射到
//   IDOK/IDCANCEL。这里手工补齐：回车=确定、ESC=取消，符合用户对对话框的预期，
//   也避免"按了回车没反应"被误认为设置不生效
BOOL CAlphaDlg::PreTranslateMessage(MSG* pMsg)
{
    if (pMsg->message == WM_KEYDOWN)
    {
        if (pMsg->wParam == VK_RETURN)
        {
            OnBnClickedOk();
            return TRUE;
        }
        if (pMsg->wParam == VK_ESCAPE)
        {
            OnBnClickedCancel();
            return TRUE;
        }
    }
    return CWnd::PreTranslateMessage(pMsg);
}

void CAlphaDlg::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pBar)
{
    if (pBar == (CScrollBar*)&m_slider)
        UpdateValLabel();
    CWnd::OnHScroll(nSBCode, nPos, pBar);
}

void CAlphaDlg::OnBnClickedOk()
{
    // 百分比 -> 0~255 alpha
    CloseDlg(m_slider.GetPos() * 255 / 100);
}

void CAlphaDlg::OnBnClickedCancel()
{
    CloseDlg(-1);
}

// 点标题栏关闭按钮等价于"取消"：必须以 EndModalLoop 结束模态循环，
// 否则 RunModalLoop 永不返回（父窗口会一直处于禁用状态）
void CAlphaDlg::OnClose()
{
    CloseDlg(-1);
}

// 右键菜单：文件操作（复制 / 剪切 / 粘贴 / 删除）+ 刷新·打开目录
//          + 视图·层级 + 折叠 + 外观配色
void CDeskTidyWidget::OnListRClick(NMHDR* pNMHDR, LRESULT* pResult)
{
    // WorkBuddy: 先把"作用对象"归一化到光标下的那一项。
    // 列表控件被右键点击时不会自动改变选择，若不做归一化，就会出现
    // "右键点的是 A、命令却作用在 B（上次的选中项）"——对"删除"尤其危险。
    // 做法与资源管理器一致：右键命中某项即把它设为唯一选中（并聚焦）项
    NMITEMACTIVATE* pNMI = reinterpret_cast<NMITEMACTIVATE*>(pNMHDR);
    int nHit = -1;
    if (pNMI != NULL)
    {
        LVHITTESTINFO hti = {};
        hti.pt = pNMI->ptAction;
        nHit = m_list.HitTest(&hti);
        if (nHit < 0 && pNMI->iItem >= 0)
            nHit = pNMI->iItem;
    }
    if (nHit >= 0 && nHit < m_list.GetItemCount())
    {
        m_list.SetItemState(nHit, LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
        m_list.EnsureVisible(nHit, FALSE);
    }

    // 有选中项时"复制 / 剪切 / 删除"才可用（无选中则置灰）。
    // 变量名避开下面 case 分支里的同名局部量，防止变量遮蔽告警
    const int  nMenuSel    = m_list.GetNextItem(-1, LVNI_SELECTED);
    const BOOL bHasSel     = (nMenuSel >= 0 && nMenuSel < (int)m_arrPaths.GetCount());
    const UINT nFileFlags  = bHasSel ? (UINT)MF_STRING
                                     : (UINT)(MF_STRING | MF_GRAYED);

    // 构造上下文菜单。文件操作（对文件有副作用）与窗口外观/布局分成两区，
    // 且文件操作置顶——其语义与资源管理器一致，用户的心智位置也在这里
    CMenu menu;
    menu.CreatePopupMenu();
    // WorkBuddy: 打开 / 以管理员权限打开（与资源管理器一致置顶）。
    // 24 = 用系统默认程序打开选中项（与双击同路径）；
    // 25 = ShellExecuteEx + lpVerb="runas" 触发 UAC 提权后打开
    menu.AppendMenu(nFileFlags, 24, _T("打开"));
    menu.AppendMenu(nFileFlags, 25, _T("以管理员权限打开"));
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(nFileFlags, 20, _T("复制"));
    menu.AppendMenu(nFileFlags, 21, _T("剪切"));
    // "粘贴"只要剪贴板里有文件列表即可用（可能是别的程序复制的，
    // 不限于本窗口；剪贴板被占用时可能误报不可用，属系统固有行为）
    menu.AppendMenu(::IsClipboardFormatAvailable(CF_HDROP)
                        ? (UINT)MF_STRING : (UINT)(MF_STRING | MF_GRAYED),
                    22, _T("粘贴"));
    menu.AppendMenu(nFileFlags, 23, _T("删除（移到回收站）"));
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, 1, _T("刷新列表"));
    menu.AppendMenu(MF_STRING, 2, _T("打开所在目录"));
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, 3,
                    (m_nViewMode == 0) ? _T("切换为网格视图") : _T("切换为列表视图"));
    menu.AppendMenu(MF_STRING, 4,
                    (m_nLayerMode == 0) ? _T("切换到悬浮置顶") : _T("切换到最底层"));
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, 5,
                    m_bCollapsed ? _T("展开窗口") : _T("折叠窗口"));
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, 6, _T("标题栏颜色..."));
    menu.AppendMenu(MF_STRING, 7, _T("背景颜色..."));
    // WorkBuddy: 文件名称颜色（文件列表项文字）。两种入口：
    //   10 = 弹取色器选一个固定颜色；11 = 回到"自动跟随背景色"（默认）
    // 自动态时在菜单项前打勾，一眼看出当前处于哪种状态
    menu.AppendMenu(MF_STRING, 10, _T("文件名称颜色..."));
    menu.AppendMenu(MF_STRING | (m_bTextAuto ? MF_CHECKED : MF_UNCHECKED),
                    11, _T("文件名称颜色：自动跟随背景"));
    menu.AppendMenu(MF_SEPARATOR);
    // WorkBuddy: 标题栏/背景各自的透明度（独立设置，互不影响）
    menu.AppendMenu(MF_STRING, 8, _T("标题栏透明度..."));
    menu.AppendMenu(MF_STRING, 9, _T("背景透明度..."));

    // 在鼠标位置弹出菜单；TPM_RETURNCMD 直接返回选中的命令 ID
    CPoint ptScreen;
    GetCursorPos(&ptScreen);
    ::SetForegroundWindow(m_hWnd);              // 防止菜单打开即关闭
    UINT nCmd = menu.TrackPopupMenu(TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                    ptScreen.x, ptScreen.y, this);
    ::PostMessage(m_hWnd, WM_NULL, 0, 0);       // 让菜单正常关闭

    switch (nCmd)
    {
    case 1:
        // 刷新列表
        RefreshFiles();
        break;

    case 2:
    {
        // 打开选中文件所在目录；无选中项时打开本小窗口的目录
        CString strDir;
        int nSel = m_list.GetNextItem(-1, LVNI_SELECTED);
        if (nSel >= 0 && nSel < (int)m_arrPaths.GetCount())
        {
            strDir = m_arrPaths[nSel];
            // 若选中的是文件，取其父目录
            DWORD dwAttr = ::GetFileAttributes(strDir);
            if (!(dwAttr & FILE_ATTRIBUTE_DIRECTORY))
            {
                int nSlash = strDir.ReverseFind(_T('\\'));
                if (nSlash > 0)
                    strDir = strDir.Left(nSlash);
            }
        }
        else
        {
            strDir = m_strDir;
        }

        if (!strDir.IsEmpty())
            ShellExecute(NULL, _T("open"), strDir, NULL, NULL, SW_SHOWNORMAL);
        break;
    }

    case 3:
        // 切换列表 / 网格视图，并通知主对话框保存配置
        SetViewMode(m_nViewMode == 0 ? 1 : 0);
        if (m_hNotify != NULL && ::IsWindow(m_hNotify))
            ::PostMessage(m_hNotify, WM_WIDGET_CHANGED, 0, 0);
        break;

    case 4:
        // 切换最底层 / 悬浮置顶，并通知主对话框保存配置
        SetLayerMode(m_nLayerMode == 0 ? 1 : 0);
        if (m_hNotify != NULL && ::IsWindow(m_hNotify))
            ::PostMessage(m_hNotify, WM_WIDGET_CHANGED, 0, 0);
        break;

    case 5:
        // 折叠 / 展开窗口（SetCollapsed 内部会通知主对话框保存配置）
        SetCollapsed(!m_bCollapsed);
        break;

    case 6:
        // 选取标题栏颜色：弹出系统颜色对话框，初始选中当前标题栏色，
        // 确认后设置并通知主对话框保存配置（重启后恢复）
        {
            CColorDialog dlg(m_clrHeader, CC_FULLOPEN, this);
            if (dlg.DoModal() == IDOK)
            {
                SetHeaderColor(dlg.GetColor());
                if (m_hNotify != NULL && ::IsWindow(m_hNotify))
                    ::PostMessage(m_hNotify, WM_WIDGET_CHANGED, 0, 0);
            }
        }
        break;

    case 7:
        // 选取背景颜色：弹出系统颜色对话框，初始选中当前背景色，
        // 确认后设置并通知主对话框保存配置（重启后恢复）
        {
            CColorDialog dlg(m_clrBg, CC_FULLOPEN, this);
            if (dlg.DoModal() == IDOK)
            {
                SetBgColor(dlg.GetColor());
                if (m_hNotify != NULL && ::IsWindow(m_hNotify))
                    ::PostMessage(m_hNotify, WM_WIDGET_CHANGED, 0, 0);
            }
        }
        break;

    case 10:
        // WorkBuddy: 选取文件名称颜色（文件列表项文字）。初始色取当前"生效色"——
        // 自动模式下用按背景算出的黑/白作为初值，取色器打开时即为当前所见颜色，
        // 便于在其基础上微调；确认后转为自定义模式
        {
            COLORREF clrInit = m_bTextAuto ? GetContrastTextColor(m_clrBg)
                                           : m_clrTextFixed;
            CColorDialog dlg(clrInit, CC_FULLOPEN, this);
            if (dlg.DoModal() == IDOK)
            {
                SetTextColor(dlg.GetColor(), FALSE);   // 选具体颜色 → 自定义模式
                if (m_hNotify != NULL && ::IsWindow(m_hNotify))
                    ::PostMessage(m_hNotify, WM_WIDGET_CHANGED, 0, 0);
            }
        }
        break;

    case 11:
        // WorkBuddy: 文件名称颜色设为"自动跟随背景"（按背景亮度取黑/白）。
        // 传入当前自定义色，仅为了让该颜色被继续记住（自动模式下不参与显示）
        SetTextColor(m_clrTextFixed, TRUE);
        if (m_hNotify != NULL && ::IsWindow(m_hNotify))
            ::PostMessage(m_hNotify, WM_WIDGET_CHANGED, 0, 0);
        break;

    case 8:
        // WorkBuddy: 设置标题栏透明度（0~100% → 0~255），独立于背景透明度
        {
            // 打开对话框期间暂停 MaintainZOrder()：它内部的抢前台会吞掉对话框上的点击
            SetModalChildOpen(TRUE);
            LogZ(_T("OnListRClick: 打开标题栏透明度对话框（当前 alpha=%d）"),
                 (int)m_byHeaderAlpha);
            int nA = CAlphaDlg::PickAlpha(this, m_byHeaderAlpha, _T("标题栏透明度"));
            SetModalChildOpen(FALSE);
            LogZ(_T("OnListRClick: 标题栏透明度对话框返回 nA=%d"), nA);
            if (nA >= 0)
            {
                SetHeaderAlpha((BYTE)nA);
                if (m_hNotify != NULL && ::IsWindow(m_hNotify))
                    ::PostMessage(m_hNotify, WM_WIDGET_CHANGED, 0, 0);
            }
        }
        break;

    case 9:
        // WorkBuddy: 设置背景透明度（0~100% → 0~255，含文件列表区），独立于标题栏
        {
            // 打开对话框期间暂停 MaintainZOrder()：它内部的抢前台会吞掉对话框上的点击
            SetModalChildOpen(TRUE);
            LogZ(_T("OnListRClick: 打开背景透明度对话框（当前 alpha=%d）"),
                 (int)m_byBgAlpha);
            int nA = CAlphaDlg::PickAlpha(this, m_byBgAlpha, _T("背景透明度"));
            SetModalChildOpen(FALSE);
            LogZ(_T("OnListRClick: 背景透明度对话框返回 nA=%d"), nA);
            if (nA >= 0)
            {
                SetBgAlpha((BYTE)nA);
                if (m_hNotify != NULL && ::IsWindow(m_hNotify))
                    ::PostMessage(m_hNotify, WM_WIDGET_CHANGED, 0, 0);
            }
        }
        break;

    // -----------------------------------------------------------------------
    // WorkBuddy: 文件操作（打开 / 以管理员权限打开 / 复制 / 剪切 / 粘贴 / 删除）
    // -----------------------------------------------------------------------

    case 24:
        // 打开：与双击一致，用系统默认程序打开选中文件/文件夹
        {
            int nSel = m_list.GetNextItem(-1, LVNI_SELECTED);
            if (nSel >= 0 && nSel < (int)m_arrPaths.GetCount())
                ShellExecute(NULL, _T("open"), m_arrPaths[nSel],
                             NULL, NULL, SW_SHOWNORMAL);
        }
        break;

    case 25:
        // 以管理员权限打开：ShellExecuteEx + lpVerb="runas" 触发 UAC 提权。
        // 用户在 UAC 框点"否"属正常取消（GetLastError=ERROR_CANCELLED），静默返回
        {
            int nSel = m_list.GetNextItem(-1, LVNI_SELECTED);
            if (nSel >= 0 && nSel < (int)m_arrPaths.GetCount())
            {
                SHELLEXECUTEINFO sei = { sizeof(SHELLEXECUTEINFO) };
                sei.lpVerb = _T("runas");
                sei.lpFile = m_arrPaths[nSel];
                sei.nShow  = SW_SHOWNORMAL;
                ::ShellExecuteEx(&sei);
            }
        }
        break;

    case 20:
        // 复制：把选中项以 CF_HDROP 写入系统剪贴板（附带 CFSTR_PREFERREDDROPEFFECT
        // = COPY）。本窗口不搬文件——真正的动作发生在用户到目标位置粘贴时，由粘贴方执行
        {
            CStringArray arr;
            CollectSelectedPaths(arr);
            if (arr.GetSize() > 0)
            {
                ClearCutState();        // 复制成功后，先前的"剪切"态不再成立
                if (PutPathsToClipboard(arr, FALSE))
                {
                    LogZ(_T("OnListRClick: 已复制 %d 项到剪贴板"), (int)arr.GetSize());
                }
                else
                {
                    // 剪贴板写入失败绝不能静默：用户会以为已经复制成功
                    LogZ(_T("OnListRClick: 复制到剪贴板失败"));
                    SetModalChildOpen(TRUE);
                    ::MessageBox(m_hWnd,
                        _T("复制到剪贴板失败。\n\n剪贴板可能正被其它程序占用，请稍后重试。"),
                        _T("DeskTidy"),
                        MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
                    SetModalChildOpen(FALSE);
                }
            }
        }
        break;

    case 21:
        // 剪切：与复制共用同一段"写剪贴板"逻辑，唯一区别是附带的效果意图为 MOVE。
        // 注意：这里绝不删除文件——用户可能剪切后并未粘贴，文件必须原地还在；
        // 真正的删除发生在粘贴那一刻、由粘贴方执行
        {
            CStringArray arr;
            CollectSelectedPaths(arr);
            if (arr.GetSize() > 0)
            {
                if (PutPathsToClipboard(arr, TRUE))
                {
                    // 记录"已剪切"路径：对应列表项置灰显示，与资源管理器的剪切态一致
                    m_arrCutPaths.RemoveAll();
                    for (int i = 0; i < arr.GetSize(); ++i)
                        m_arrCutPaths.Add(arr[i]);
                    m_list.Invalidate();    // 触发重绘 → 项级 NM_CUSTOMDRAW 置灰
                    RenderLayered();        // 分层窗口：列表变了必须重新合成才可见
                    LogZ(_T("OnListRClick: 已剪切 %d 项"), (int)arr.GetSize());
                }
                else
                {
                    LogZ(_T("OnListRClick: 剪切到剪贴板失败"));
                    SetModalChildOpen(TRUE);
                    ::MessageBox(m_hWnd,
                        _T("剪切到剪贴板失败。\n\n剪贴板可能正被其它程序占用，请稍后重试。"),
                        _T("DeskTidy"),
                        MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
                    SetModalChildOpen(FALSE);
                }
            }
        }
        break;

    case 22:
        // 粘贴：读取剪贴板中的 CF_HDROP，按 CFSTR_PREFERREDDROPEFFECT 判断
        // 是"移动（剪切）"还是"复制"，搬进绑定目录后刷新列表
        PasteFromClipboard();
        break;

    case 23:
        // 删除（移到回收站）：SHFileOperation(FO_DELETE) + FOF_ALLOWUNDO，
        // 可从回收站还原。失败（只读 / 被占用 / 无权限）必须提示用户，不能静默
        {
            CStringArray arr;
            CollectSelectedPaths(arr);
            if (arr.GetSize() > 0)
            {
                // 屏蔽 z 序维护：它内部会抢前台，可能吞掉提示框的焦点
                SetModalChildOpen(TRUE);
                const BOOL bOk = DeletePaths(arr, TRUE);
                SetModalChildOpen(FALSE);
                if (bOk)
                {
                    PruneCutPaths();           // 被删掉的项不再需要"剪切"高亮
                    RefreshFilesAndRepaint();
                }
            }
        }
        break;
    }

    *pResult = 0;
}

// ---------------------------------------------------------------------------
// 文件拖放：把资源管理器中的文件拖到本小窗口上，添加到绑定目录
// ---------------------------------------------------------------------------

// 处理 WM_DROPFILES：遍历本次拖放的所有路径，逐个添加到绑定目录，然后刷新列表。
//
// 两种投放语义（由拖放时是否按住 Shift 决定）：
//   - 未按 Shift：默认行为——.lnk 拷贝一份 / 其它文件创建快捷方式，源文件保持不动；
//   - 按住 Shift：把源文件"移动"进绑定目录（等价于剪切 → 粘贴），源位置不再保留。
// 这与资源管理器的习惯一致（Shift + 拖放 = 移动）。
void CDeskTidyWidget::OnDropFiles(HDROP hDropInfo)
{
    // 检测拖放落下瞬间 Shift 是否按下。
    // 说明：WM_DROPFILES（DragQueryFile）本身不携带修饰键信息，只能主动查询键态；
    // 选用 GetAsyncKeyState 而非 GetKeyState——本小窗口为 WS_EX_NOACTIVATE 且
    // 不接收键盘焦点，GetKeyState 依赖线程消息队列的同步键态，此处不可靠；
    // GetAsyncKeyState 直接反映物理键态，而拖放落下时 Shift 仍处于按下状态，
    // 可稳定捕获。
    BOOL bShift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    // 查询本次拖放的文件数量（参数传 0xFFFFFFFF 表示"只要数量"）
    UINT nFiles = ::DragQueryFile(hDropInfo, 0xFFFFFFFF, NULL, 0);
    LogZ(_T("OnDropFiles: nFiles=%u, shift=%d -> %s"), nFiles, (int)bShift,
         bShift ? _T("移动") : _T("创建快捷方式"));

    for (UINT i = 0; i < nFiles; i++)
    {
        TCHAR szPath[MAX_PATH] = {};
        if (::DragQueryFile(hDropInfo, i, szPath, MAX_PATH) > 0)
        {
            if (bShift)
                MoveDroppedFile(szPath);    // Shift：把源文件搬进绑定目录
            else
                AddDroppedFile(szPath);     // 默认：创建快捷方式 / 拷贝 .lnk
        }
    }
    ::DragFinish(hDropInfo);        // 释放系统为拖放分配的内存

    // 拖放后刷新文件列表，让新添加/移动进来的文件立即显示出来
    RefreshFiles();
}

// 把一个拖放过来的路径添加到绑定目录：
//   - .lnk 快捷方式：直接拷贝一份到绑定目录（保持快捷方式内容不变）；
//   - 其它文件/目录：在绑定目录中创建指向它的快捷方式（不移动原文件）。
void CDeskTidyWidget::AddDroppedFile(const CString& strSrcPath)
{
    // 拼出绑定目录完整路径（尾部补 '\'）
    CString strDir = m_strDir;
    if (strDir.Right(1) != _T("\\"))
        strDir += _T("\\");

    // 取出源文件名（含扩展名）
    int nSlash = strSrcPath.ReverseFind(_T('\\'));
    CString strName = (nSlash >= 0) ? strSrcPath.Mid(nSlash + 1) : strSrcPath;
    if (strName.IsEmpty())
        return;

    if (IsLnkFile(strSrcPath))
    {
        // ---- 快捷方式：拷贝一份到绑定目录 ----
        // 源快捷方式若已经在绑定目录里，则无需拷贝（避免同名覆盖失败）
        if (strSrcPath.Left(strDir.GetLength()).CompareNoCase(strDir) == 0)
            return;

        CString strDst = MakeUniquePath(strDir, strName);
        ::CopyFile(strSrcPath, strDst, TRUE);   // 重名已由唯一名保证，不会覆盖已有文件
    }
    else
    {
        // ---- 其它文件/目录：创建指向它的快捷方式 ----
        // 快捷方式名 = 去掉扩展名的源文件名 + ".lnk"
        CString strLnkName = strName;
        int nDot = strLnkName.ReverseFind(_T('.'));
        if (nDot > 0)
            strLnkName = strLnkName.Left(nDot);
        strLnkName += _T(".lnk");

        CString strLnkPath = MakeUniquePath(strDir, strLnkName);
        CreateShortcut(strSrcPath, strLnkPath);
    }
}

// WorkBuddy: 把一个拖放过来的路径"移动"到绑定目录（Shift + 拖放时调用）。
//
// 与 AddDroppedFile 的本质区别：搬运的是源文件本身，成功后源位置不再保留该文件
// （等价于资源管理器的"剪切 → 粘贴"）。
//
// 实现统一收敛到 TransferIntoDir()（SHFileOperation(FO_MOVE)，可正确处理跨卷、
// 目录整体搬移与目标重名）：拖放移动、粘贴（移动 / 复制）三处共用同一段逻辑，
// 避免同样的搬运代码散落多份、行为逐渐走样
void CDeskTidyWidget::MoveDroppedFile(const CString& strSrcPath)
{
    LogZ(_T("MoveDroppedFile: 移动 %s -> %s"), (LPCTSTR)strSrcPath, (LPCTSTR)m_strDir);
    TransferIntoDir(strSrcPath, TRUE);
}

// WorkBuddy: 判断 strPath 是否"直接"位于目录 strDir 之下（strDir 需以 '\' 结尾）。
// 前缀命中后要求剩余部分不含 '\'——更深的子目录路径返回 FALSE，因为那些路径
// 依然是可以搬到绑定目录根下的对象（例如把 绑定目录\子目录\a.txt 上移一级）。
BOOL CDeskTidyWidget::IsPathDirectlyInDir(const CString& strPath, const CString& strDir)
{
    if (strDir.IsEmpty() || strPath.GetLength() <= strDir.GetLength())
        return FALSE;

    if (strPath.Left(strDir.GetLength()).CompareNoCase(strDir) != 0)
        return FALSE;

    // 剩余部分仍含目录分隔符 => 位于更深的子目录中，不属于"直接位于"本目录
    CString strRest = strPath.Mid(strDir.GetLength());
    return strRest.Find(_T('\\')) < 0;
}

// 生成绑定目录中不重名的完整路径：
// 目标文件已存在时，在文件名中间追加 " (2)"、" (3)"… 序号，直到不存在为止。
CString CDeskTidyWidget::MakeUniquePath(const CString& strDir, const CString& strFileName)
{
    // 把文件名拆成"主名 + 扩展名"，便于在中间插入序号
    CString strBase = strFileName;
    CString strExt;
    int nDot = strFileName.ReverseFind(_T('.'));
    if (nDot > 0)
    {
        strExt  = strFileName.Mid(nDot);    // 含 "."，如 ".lnk"
        strBase = strFileName.Left(nDot);
    }

    CString strPath = strDir + strFileName;
    int nIndex = 2;
    while (::GetFileAttributes(strPath) != INVALID_FILE_ATTRIBUTES)
    {
        CString strSeq;
        strSeq.Format(_T(" (%d)"), nIndex++);
        strPath = strDir + strBase + strSeq + strExt;
    }
    return strPath;
}

// 在指定位置创建指向目标路径的快捷方式 (.lnk)。
// 通过 COM 的 IShellLink + IPersistFile 完成：与资源管理器创建的快捷方式等价。
BOOL CDeskTidyWidget::CreateShortcut(const CString& strTargetPath, const CString& strLnkPath)
{
    // 创建 IShellLink 对象
    CComPtr<IShellLink> pLink;
    HRESULT hr = pLink.CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER);
    if (FAILED(hr))
        return FALSE;

    // 设置快捷方式目标
    pLink->SetPath(strTargetPath);

    // 若目标是目录，设置其工作目录为自身，便于"在文件夹中显示"等操作定位
    DWORD dwAttr = ::GetFileAttributes(strTargetPath);
    if (dwAttr != INVALID_FILE_ATTRIBUTES && (dwAttr & FILE_ATTRIBUTE_DIRECTORY))
        pLink->SetWorkingDirectory(strTargetPath);

    // 通过 IPersistFile 把快捷方式保存为 .lnk 文件
    CComPtr<IPersistFile> pFile;
    if (FAILED(pLink.QueryInterface(&pFile)))
        return FALSE;

    hr = pFile->Save(strLnkPath, TRUE);
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// WorkBuddy: 文件操作（拖出 / 复制 / 剪切 / 粘贴 / 删除）的实现
//
// 统一约定：
//   * 列表中的每一项都对应绑定目录里的一个真实路径（m_arrPaths[i]），因此
//     "对文件的操作"作用的就是那一项本身（.lnk 就搬 .lnk），与在资源管理器里
//     操作一个快捷方式完全一致；
//   * 写剪贴板 / 搬移 / 删除一律走系统接口（OLE 数据对象、SHFileOperation），
//     不自己拼装搬运逻辑——跨卷移动、目录整体搬移、重名冲突、回收站撤销
//     这些边界行为都由系统保证；
//   * 分层窗口下列表是"不可见子控件"（内容靠 PrintWindow 抓进分层位图），
//     改完列表必须调用 RefreshFilesAndRepaint() 重新合成，否则屏幕不会变
// ---------------------------------------------------------------------------

// 拖出（DoDragDrop）用的数据源：在 COleDataSource 基础上捕获目标回写的
// CFSTR_PERFORMEDDROPEFFECT。
//
// 为什么必须捕获它——"文件最终由谁删除"不是源能单方面决定的：
// MSDN《Handling Optimized Move Operations》指出，shell 对 CF_HDROP 常常自己
// 就把文件搬完了（"优化移动"），却**仍然**可能返回 DROPEFFECT_MOVE。这个从
// Windows 95 沿用至今的行为意味着"看到 MOVE 就删源"是不可靠的写法，会丢文件。
// 可靠判据只有一个：目标是否通过 SetData 回写了 CFSTR_PERFORMEDDROPEFFECT
class CWidgetDataSource : public COleDataSource
{
public:
    CWidgetDataSource()
        : m_dwPerformedEffect(DROPEFFECT_NONE), m_bPerformedValid(FALSE) {}

    DWORD m_dwPerformedEffect;   // 目标实际执行的效果（CFSTR_PERFORMEDDROPEFFECT）
    BOOL  m_bPerformedValid;     // 目标是否回写了该格式

protected:
    // 目标（资源管理器等）在执行完搬运动作后会调用源数据对象的 SetData，
    // 把"实际执行的效果"回写进来；MFC 会把它转发到本可覆写虚函数
    virtual BOOL OnSetData(LPFORMATETC lpFormatEtc, LPSTGMEDIUM lpStgMedium, BOOL bRelease)
    {
        if (lpFormatEtc != NULL && lpStgMedium != NULL &&
            lpStgMedium->hGlobal != NULL &&
            lpFormatEtc->tymed == TYMED_HGLOBAL &&
            lpFormatEtc->cfFormat == ::RegisterClipboardFormat(CFSTR_PERFORMEDDROPEFFECT))
        {
            const DWORD* pdw = (const DWORD*)::GlobalLock(lpStgMedium->hGlobal);
            if (pdw != NULL)
            {
                m_dwPerformedEffect = *pdw;
                m_bPerformedValid   = TRUE;
                ::GlobalUnlock(lpStgMedium->hGlobal);
            }
        }
        return COleDataSource::OnSetData(lpFormatEtc, lpStgMedium, bRelease);
    }
};

// 构造 CF_HDROP 要求的全局内存：
//   DROPFILES 头（本项目为 Unicode 字符集，fWide 必须为 TRUE）+ 每条路径以
//   '\0' 结尾 + 末尾再补一个 '\0'（"双 null 结尾"）。
// 成功后句柄归调用方所有（交给 OLE/剪贴板后由其负责释放）；失败返回 NULL
HGLOBAL CDeskTidyWidget::BuildHDropGlobal(const CStringArray& arrPaths)
{
    if (arrPaths.GetSize() == 0)
        return NULL;

    // 需要写入的字符数：全部路径 + 每条一个 '\0' + 末尾额外一个 '\0'
    SIZE_T cbChars = 1;                                 // 末尾额外的 '\0'
    for (int i = 0; i < (int)arrPaths.GetSize(); ++i)
        cbChars += (SIZE_T)arrPaths[i].GetLength() + 1;
    const SIZE_T cbTotal = sizeof(DROPFILES) + cbChars * sizeof(TCHAR);

    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, cbTotal);
    if (hMem == NULL)
        return NULL;

    BYTE* pBase = (BYTE*)::GlobalLock(hMem);
    if (pBase == NULL)
    {
        ::GlobalFree(hMem);
        return NULL;
    }

    DROPFILES* pdf = (DROPFILES*)pBase;
    pdf->pFiles = (DWORD)sizeof(DROPFILES);   // 文件列表相对本结构起点的偏移
    pdf->fWide  = TRUE;                       // Unicode 项目：必须是宽字符
    // pt / fNC 依赖 GMEM_ZEROINIT 清零

    TCHAR* psz = (TCHAR*)(pBase + sizeof(DROPFILES));
    for (int i = 0; i < (int)arrPaths.GetSize(); ++i)
    {
        const int n = arrPaths[i].GetLength();
        memcpy(psz, (LPCTSTR)arrPaths[i], (SIZE_T)n * sizeof(TCHAR));
        psz += n;
        *psz++ = _T('\0');
    }
    *psz = _T('\0');        // 末尾第二个 null（与上一条路径的终止符构成双 null）

    ::GlobalUnlock(hMem);
    return hMem;
}

// 把一组路径拼成 SHFileOperation 要求的"双 null 结尾"缓冲区，
// 其数据指针可直接作为 pFrom / pTo
static CString MakeDoubleNullPathList(const CStringArray& arrPaths)
{
    int nTotal = 1;                         // 末尾额外的 '\0'
    for (int i = 0; i < (int)arrPaths.GetSize(); ++i)
        nTotal += arrPaths[i].GetLength() + 1;

    CString strList;
    LPTSTR pBuf = strList.GetBuffer(nTotal);
    LPTSTR p = pBuf;
    for (int i = 0; i < (int)arrPaths.GetSize(); ++i)
    {
        const int n = arrPaths[i].GetLength();
        memcpy(p, (LPCTSTR)arrPaths[i], (SIZE_T)n * sizeof(TCHAR));
        p += n;
        *p++ = _T('\0');
    }
    *p = _T('\0');
    strList.ReleaseBuffer(nTotal - 1);      // 长度不含最后一个终止符
    return strList;
}

// 把源路径"移动"或"复制"进绑定目录。
//
// 采用 SHFileOperation(FO_MOVE / FO_COPY) 而非 CopyFile / MoveFileEx，原因：
//   1) 跨卷移动：SHFileOperation 内部自动"复制 + 删除源"，MoveFileEx 对目录
//      跨卷会直接失败（MOVEFILE_COPY_ALLOWED 只对文件有效）；
//   2) 目录整体搬移：一次性搬走整个目录树；
//   3) 目标重名：FOF_RENAMEONCOLLISION 自动追加 " (2)"、" (3)" 序号；
//   4) 行为与资源管理器完全一致，符合用户对"移动 / 复制"的直觉。
// FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI 保证全程静默、不弹任何系统对话框
BOOL CDeskTidyWidget::TransferIntoDir(const CString& strSrcPath, BOOL bMove)
{
    if (strSrcPath.IsEmpty())
        return FALSE;

    // 绑定目录（用于"是否已在目标目录内"的前缀比较，尾部补 '\'）
    CString strDir = m_strDir;
    if (strDir.Right(1) != _T("\\"))
        strDir += _T("\\");

    // 移动时若源已"直接"位于绑定目录内则跳过（把文件移动到自己所在目录无意义，
    // SHFileOperation 会报错）；更深层的子目录文件仍可被上移一级到绑定目录根下。
    // 复制则允许——资源管理器同样支持"在原目录粘贴出副本"
    if (bMove && IsPathDirectlyInDir(strSrcPath, strDir))
    {
        LogZ(_T("TransferIntoDir: 已在绑定目录内，跳过移动：%s"), (LPCTSTR)strSrcPath);
        return FALSE;
    }

    // SHFileOperation 要求 pFrom / pTo 均为"双 null 结尾"的字符串。
    // 用清零的定长缓冲区：_tcsncpy_s 拷贝后末尾自带一个 '\0'，数组剩余部分
    // 仍为 0，二者叠加即构成双 null 结尾（也便于静态分析识别）
    TCHAR szFrom[MAX_PATH + 2] = {};
    TCHAR szTo[MAX_PATH + 2] = {};
    _tcsncpy_s(szFrom, _countof(szFrom), strSrcPath, _TRUNCATE);
    _tcsncpy_s(szTo,   _countof(szTo),   m_strDir,    _TRUNCATE);   // 目标为目录

    SHFILEOPSTRUCT fos = {};
    fos.hwnd   = m_hWnd;                                    // 本小窗口（可为 NULL）
    fos.wFunc  = bMove ? FO_MOVE : FO_COPY;                 // 移动 / 复制
    fos.pFrom  = szFrom;                                    // 源：双 null 结尾
    fos.pTo    = szTo;                                      // 目标：绑定目录
    fos.fFlags = FOF_NOCONFIRMATION                         // 不弹"是否替换"确认
               | FOF_SILENT                                 // 不显示进度对话框
               | FOF_NOERRORUI                              // 出错也不弹系统提示
               | FOF_RENAMEONCOLLISION;                     // 重名自动加 " (2)" 序号

    const int  nRet = ::SHFileOperation(&fos);
    const BOOL bOk  = (nRet == 0 && !fos.fAnyOperationsAborted);
    LogZ(_T("TransferIntoDir: %s ret=%d aborted=%d：%s -> %s"),
         bMove ? _T("移动") : _T("复制"), nRet, (int)fos.fAnyOperationsAborted,
         (LPCTSTR)strSrcPath, (LPCTSTR)m_strDir);
    return bOk;
}

// 把一组路径写入系统剪贴板（bCut: TRUE=剪切(MOVE)、FALSE=复制(COPY)）。
//
// "复制"与"剪切"在 Windows 层面的唯一区别，就是剪贴板里附带的一个"效果意图"
// （CFSTR_PREFERREDDROPEFFECT）；不写它，资源管理器粘贴时就分不清两者。
//
// 这里用经典 Win32 剪贴板 API 而非 OLE 的 COleDataSource::SetClipboard：
// 数据是现成的、不需要延迟渲染，经典写法行为确定、生命周期无歧义
BOOL CDeskTidyWidget::PutPathsToClipboard(const CStringArray& arrPaths, BOOL bCut)
{
    if (arrPaths.GetSize() == 0)
        return FALSE;

    HGLOBAL hDrop = BuildHDropGlobal(arrPaths);
    if (hDrop == NULL)
        return FALSE;

    if (!::OpenClipboard(m_hWnd))
    {
        ::GlobalFree(hDrop);
        return FALSE;
    }

    // 必须先清空（同时取得剪贴板所有权）再放置数据
    if (!::EmptyClipboard())
    {
        ::CloseClipboard();
        ::GlobalFree(hDrop);
        return FALSE;
    }

    // 成功后内存所有权移交系统，不能再 GlobalFree
    if (::SetClipboardData(CF_HDROP, hDrop) == NULL)
    {
        ::GlobalFree(hDrop);
        ::CloseClipboard();
        return FALSE;
    }

    // 附带"效果意图"：这就是剪切与复制的本质区别
    HGLOBAL hEff = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (hEff != NULL)
    {
        DWORD* pdw = (DWORD*)::GlobalLock(hEff);
        if (pdw != NULL)
        {
            *pdw = bCut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            ::GlobalUnlock(hEff);
        }
        if (::SetClipboardData(::RegisterClipboardFormat(CFSTR_PREFERREDDROPEFFECT),
                               hEff) == NULL)
        {
            ::GlobalFree(hEff);     // 放置失败时内存仍归本进程，需自行释放
        }
    }

    ::CloseClipboard();
    return TRUE;
}

// 把剪贴板中的 CF_HDROP 粘贴进绑定目录。
// "移动"还是"复制"由剪贴板里的 CFSTR_PREFERREDDROPEFFECT 决定：
// 该格式为 MOVE 即"剪切"（粘贴后源文件消失），否则按复制处理
void CDeskTidyWidget::PasteFromClipboard()
{
    if (!::IsClipboardFormatAvailable(CF_HDROP))
        return;

    if (!::OpenClipboard(m_hWnd))
        return;

    // 读取"效果意图"。注意它可能不存在——别的程序复制文件时不一定附带
    BOOL bMove = FALSE;
    const UINT uEffFmt = ::RegisterClipboardFormat(CFSTR_PREFERREDDROPEFFECT);
    if (uEffFmt != 0)
    {
        HGLOBAL hEff = ::GetClipboardData(uEffFmt);
        if (hEff != NULL)
        {
            const DWORD* pdw = (const DWORD*)::GlobalLock(hEff);
            if (pdw != NULL)
            {
                bMove = (((*pdw) & DROPEFFECT_MOVE) != 0);
                ::GlobalUnlock(hEff);
            }
        }
    }

    // 剪贴板数据句柄只在剪贴板打开期间有效，必须先把路径全部取出
    CStringArray arrPaths;
    HDROP hDrop = (HDROP)::GetClipboardData(CF_HDROP);
    if (hDrop != NULL)
    {
        const UINT nFiles = ::DragQueryFile(hDrop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < nFiles; ++i)
        {
            TCHAR szPath[MAX_PATH] = {};
            if (::DragQueryFile(hDrop, i, szPath, MAX_PATH) > 0)
                arrPaths.Add(szPath);
        }
    }
    ::CloseClipboard();

    if (arrPaths.GetSize() == 0)
        return;

    // 剪贴板内容是否来自本窗口（用于判断要不要清掉"剪切"置灰）
    const BOOL bFromSelf = (::GetClipboardOwner() == m_hWnd);

    for (int i = 0; i < (int)arrPaths.GetSize(); ++i)
        TransferIntoDir(arrPaths[i], bMove);

    if (bFromSelf)
        ClearCutState();
    RefreshFilesAndRepaint();

    LogZ(_T("PasteFromClipboard: n=%d move=%d self=%d"),
         (int)arrPaths.GetSize(), (int)bMove, (int)bFromSelf);
}

// 删除一组路径（进回收站，可撤销）。
//
// 与"拖出后的源自删"刚好相反的处理：用户主动删除时失败必须提示（只读、被
// 占用都很常见，静默失败会让人以为删掉了）；而 MSDN 建议"源自删"失败时不要
// 弹 UI，因此由 bShowErrors 区分两种场景
BOOL CDeskTidyWidget::DeletePaths(const CStringArray& arrPaths, BOOL bShowErrors)
{
    if (arrPaths.GetSize() == 0)
        return FALSE;

    const CString strFrom = MakeDoubleNullPathList(arrPaths);   // 双 null 结尾

    SHFILEOPSTRUCT fos = {};
    fos.hwnd   = m_hWnd;
    fos.wFunc  = FO_DELETE;
    fos.pFrom  = (LPCTSTR)strFrom;
    fos.pTo    = NULL;                                          // 删除无目标
    // FOF_ALLOWUNDO：移入回收站（可撤销）。正因为可撤销，"不弹二次确认"
    // 才是可接受的——误删可以从回收站找回。菜单文案也据此写明"移到回收站"
    fos.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    if (!bShowErrors)
        fos.fFlags |= FOF_SILENT;      // 拖出后的"源自删"：完全静默

    const int  nRet = ::SHFileOperation(&fos);
    const BOOL bOk  = (nRet == 0 && !fos.fAnyOperationsAborted);
    LogZ(_T("DeletePaths: n=%d showErr=%d ret=%d aborted=%d"),
         (int)arrPaths.GetSize(), (int)bShowErrors, nRet, (int)fos.fAnyOperationsAborted);

    if (!bOk && bShowErrors)
    {
        CString strMsg;
        strMsg.Format(_T("删除失败（错误码 %d）。\n\n")
                      _T("文件可能正在被使用，或没有足够的权限。"), nRet);
        ::MessageBox(m_hWnd, strMsg, _T("DeskTidy"),
                     MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    }
    return bOk;
}

// 采集当前选中项的完整路径（列表为单选，通常只有一项）
void CDeskTidyWidget::CollectSelectedPaths(CStringArray& arrPaths)
{
    arrPaths.RemoveAll();
    POSITION pos = m_list.GetFirstSelectedItemPosition();
    while (pos != NULL)
    {
        const int nItem = m_list.GetNextSelectedItem(pos);
        if (nItem >= 0 && nItem < (int)m_arrPaths.GetCount())
            arrPaths.Add(m_arrPaths[nItem]);
    }
}

// 刷新列表并重新合成分层位图（两者必须成对：见文件末尾的统一约定说明）
void CDeskTidyWidget::RefreshFilesAndRepaint()
{
    PruneCutPaths();        // 先把已不存在的"剪切"项剔掉，避免残留置灰
    RefreshFiles();
    RenderLayered();
}

// 从"已剪切路径"里剔除磁盘上已不存在的项：粘贴成功、拖出被搬走、直接删除
// 之后调用，保证置灰高亮不会指向不存在的文件
void CDeskTidyWidget::PruneCutPaths()
{
    for (int i = (int)m_arrCutPaths.GetSize() - 1; i >= 0; --i)
    {
        if (::GetFileAttributes(m_arrCutPaths[i]) == INVALID_FILE_ATTRIBUTES)
            m_arrCutPaths.RemoveAt(i);
    }
}

// 清空"已剪切"高亮
void CDeskTidyWidget::ClearCutState()
{
    if (m_arrCutPaths.GetSize() == 0)
        return;

    m_arrCutPaths.RemoveAll();
    if (m_list.GetSafeHwnd() != NULL)
    {
        m_list.Invalidate();    // 去掉置灰（会走 NM_CUSTOMDRAW 重绘）
        RenderLayered();        // 分层窗口：列表变了必须重新合成
    }
}

// 指定路径当前是否处于"已剪切待粘贴"
BOOL CDeskTidyWidget::IsCutPath(const CString& strPath) const
{
    for (int i = 0; i < (int)m_arrCutPaths.GetSize(); ++i)
    {
        if (m_arrCutPaths[i].CompareNoCase(strPath) == 0)
            return TRUE;
    }
    return FALSE;
}

// 剪贴板内容变化（AddClipboardFormatListener）：
// 若剪贴板已被别的程序接管，本窗口此前的"剪切"置灰态就失效了，需要清掉——
// 否则列表里会一直留着一个永远不会被粘贴走的"幽灵置灰项"
LRESULT CDeskTidyWidget::OnClipboardUpdate(WPARAM /*wParam*/, LPARAM /*lParam*/)
{
    if (::GetClipboardOwner() != m_hWnd)
        ClearCutState();
    return 0;
}

// 把一组路径作为 OLE 拖放源"拖出去"（拖到资源管理器文件夹 / 桌面 / 任何接受
// CF_HDROP 的目标）。核心原则：本函数自己不搬文件——由目标决定怎么接收，
// 这恰好与"拖入"（OnDropFiles）相反
void CDeskTidyWidget::StartDragOut(const CStringArray& arrPaths)
{
    if (m_bInDragOut || arrPaths.GetSize() == 0)
        return;
    if (!::IsWindow(m_hWnd))
        return;

    HGLOBAL hDrop = BuildHDropGlobal(arrPaths);
    if (hDrop == NULL)
        return;

    // 用派生数据源，以便捕获目标回写的 CFSTR_PERFORMEDDROPEFFECT（见类注释）。
    // 堆分配 + 用毕 InternalRelease：MFC 的 DoDragDrop 并不接管对象所有权
    // （它只借用接口指针），CCmdTarget 的引用计数初值为 1，释放即销毁
    CWidgetDataSource* pSrc = new CWidgetDataSource;
    pSrc->CacheGlobalData(CF_HDROP, hDrop);     // 内存所有权自此移交数据源

    // 首选效果 = 移动（无修饰键即移动、Ctrl = 复制），与资源管理器一致，
    // 也与已实现的"Shift + 拖入 = 移动"形成对称
    {
        HGLOBAL hEff = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (hEff != NULL)
        {
            DWORD* pdw = (DWORD*)::GlobalLock(hEff);
            if (pdw != NULL)
            {
                *pdw = DROPEFFECT_MOVE;
                ::GlobalUnlock(hEff);
            }
            pSrc->CacheGlobalData((CLIPFORMAT)::RegisterClipboardFormat(
                                      CFSTR_PREFERREDDROPEFFECT), hEff);
        }
    }

    // 拖拽期间暂停 z 序维护：DoDragDrop 是嵌套模态消息循环，而 MaintainZOrder
    // 由 1 秒定时器周期驱动、内部会抢前台并 SetWindowPos 压底，会干扰拖拽与落点
    m_bInDragOut = TRUE;
    const BOOL bOldModal = IsModalChildOpen();
    SetModalChildOpen(TRUE);

    // 使用 MFC 默认的 COleDropSource（DoDragDrop 内部自建）。
    // 注意不要自行覆写 COleDropSource::OnBeginDrag 来"跳过等待"——该函数
    // 同时负责初始化"哪个按键取消、哪个按键投放"，跳过它会让拖拽立即被取消
    const DWORD dwEffect = pSrc->DoDragDrop(DROPEFFECT_MOVE | DROPEFFECT_COPY);

    const BOOL  bPerformedValid   = pSrc->m_bPerformedValid;
    const DWORD dwPerformedEffect = pSrc->m_dwPerformedEffect;
    pSrc->InternalRelease();        // 数据源用毕释放

    SetModalChildOpen(bOldModal);
    m_bInDragOut = FALSE;

    LogZ(_T("StartDragOut: n=%d effect=%lu performedValid=%d performed=%lu"),
         (int)arrPaths.GetSize(), dwEffect, (int)bPerformedValid, dwPerformedEffect);

    // ---- 是否由源删除原文件 ----
    // 只有"拖放结果是 MOVE 且目标未回写 MOVE"时才删：此时目标只是读取了数据，
    // 按 OLE 约定由源负责删除。若目标已回写 MOVE（优化移动），文件早已被搬走，
    // 再删就是删空/误删；若结果是 COPY，源文件必须保留。
    // 这个保守取向的最坏结果只是"文件多留一份"，永远不会丢文件
    if (dwEffect == DROPEFFECT_MOVE &&
        !(bPerformedValid && dwPerformedEffect == DROPEFFECT_MOVE))
    {
        LogZ(_T("StartDragOut: 目标未完成移动，由源删除原文件"));
        DeletePaths(arrPaths, FALSE);       // 进回收站；静默（MSDN 建议不弹 UI）
    }

    if (dwEffect != DROPEFFECT_NONE)
        RefreshFilesAndRepaint();

    // 拖完把层级钉回桌面之上（拖拽期间被 SetModalChildOpen 暂停了维护）
    MaintainZOrder();
}
