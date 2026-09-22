
// DeskTidyDlg.cpp: 主对话框（设置界面）实现文件
//
// 承担托盘图标、隐藏启动、多小窗口管理与 INI 配置持久化等职责，
// 详见 DeskTidyDlg.h。

#include "pch.h"
#include "framework.h"
#include "DeskTidy.h"
#include "DeskTidyDlg.h"
#include "afxdialogex.h"

#include <shlobj.h>     // SHGetFolderPath：获取系统目录（桌面等）
#include <afxdlgs.h>    // CFolderPickerDialog：目录选择对话框
#include <dwmapi.h>     // WorkBuddy: DwmSetWindowAttribute——Win11 圆角窗口
#pragma comment(lib, "dwmapi.lib")

// Win11 22H2 新增的 DWM 属性/常量，旧 SDK 头文件里没有，兜底自定义
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

// ---------------------------------------------------------------------------
// WorkBuddy: 现代扁平界面配色方案（Design Tokens）
// ---------------------------------------------------------------------------
// 所有界面用色集中在此定义，避免散落的 RGB() 字面量。主色与桌面小窗口标题栏
// 默认色（WIDGET_DEFAULT_HEADER_COLOR = #2D6EB4）保持一致，保证主界面与
// 小窗口视觉同源；整体为"浅灰底 + 白色圆角卡片 + 品牌蓝主按钮"的现代浅色风格。
#define UI_CLR_DLG_BG               RGB(0xF3, 0xF4, 0xF6)   // 对话框底：浅灰
#define UI_CLR_CARD                 RGB(0xFF, 0xFF, 0xFF)   // 卡片 / 控件底：白
#define UI_CLR_CARD_BORDER          RGB(0xE5, 0xE7, 0xEB)   // 卡片细描边
#define UI_CLR_ACCENT               RGB(0x2D, 0x6E, 0xB4)   // 主色（品牌蓝）
#define UI_CLR_ACCENT_HOVER         RGB(0x3A, 0x82, 0xCE)   // 主色悬停
#define UI_CLR_ACCENT_DOWN          RGB(0x24, 0x5A, 0x96)   // 主色按下
#define UI_CLR_TEXT                 RGB(0x1F, 0x29, 0x37)   // 主文字：近黑
#define UI_CLR_TEXT_DISABLED        RGB(0x9C, 0xA3, 0xAF)   // 禁用文字：浅灰
#define UI_CLR_BTN_HOVER            RGB(0xF7, 0xF9, 0xFB)   // 次按钮悬停底
#define UI_CLR_BTN_DOWN             RGB(0xE8, 0xEA, 0xED)   // 次按钮按下底
#define UI_CLR_BTN_BORDER           RGB(0xD1, 0xD5, 0xDB)   // 次按钮描边
#define UI_CLR_BTN_DISABLED_BG      RGB(0xF3, 0xF4, 0xF6)   // 禁用按钮底
#define UI_CLR_BTN_DISABLED_BORDER  RGB(0xE5, 0xE7, 0xEB)   // 禁用按钮描边

// 两张卡片的位置（对话框单位 DLU，与 DeskTidy.rc 中控件坐标同坐标系）。
// 卡片不是控件，而是由 CDeskTidyDlg::OnEraseBkgnd 自绘的圆角白底——既能得到
// GROUPBOX 做不到的圆角与柔和描边，又不会引入额外子窗口（避免遮挡控件、
// 拦截鼠标等问题）。矩形按"包住原 GROUPBOX 内容 + 顶部留出一行标题"取值，
// 因此无需改动任何控件的坐标。
static const RECT g_rcCardDLU[2] =
{
	{ 10, 116, 398, 316 },   // 卡片 1：选中窗口的属性（层级 / 视图 / 三个配色 / 透明度 / 应用）
	{ 10, 340, 398, 478 },   // 卡片 2：全局快捷键（随卡片 1 增高 22 DLU 一并下移）
};
#define UI_CARD_COUNT   (sizeof(g_rcCardDLU) / sizeof(g_rcCardDLU[0]))


// 用于应用程序“关于”菜单项的 CAboutDlg 对话框

class CAboutDlg : public CDialogEx
{
public:
	CAboutDlg();

// 对话框数据
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_ABOUTBOX };
#endif

protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV 支持

// 实现
protected:
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialogEx(IDD_ABOUTBOX)
{
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialogEx)
END_MESSAGE_MAP()

// WorkBuddy: 自绘按钮的悬停跟踪（CButton 派生类，声明见 DeskTidyDlg.h）。
// 对话框收不到鼠标在子控件上的移动消息，故由按钮自己上报"进入/离开"。
BEGIN_MESSAGE_MAP(CFlatBtn, CButton)
	ON_WM_MOUSEMOVE()
	ON_WM_MOUSELEAVE()
END_MESSAGE_MAP()

// 鼠标进入/在按钮上移动：首次进入时注册 WM_MOUSELEAVE 跟踪，随后通知父对话框
// "当前悬停在本按钮上"，父对话框据此重绘出高亮态。
// 用 SendMessage 而非 PostMessage，保证悬停反馈即时、无滞后感
void CFlatBtn::OnMouseMove(UINT nFlags, CPoint point)
{
	CButton::OnMouseMove(nFlags, point);

	if (!m_bTracking)
	{
		TRACKMOUSEEVENT tme;
		ZeroMemory(&tme, sizeof(tme));
		tme.cbSize    = sizeof(tme);
		tme.dwFlags   = TME_LEAVE;
		tme.hwndTrack = m_hWnd;
		if (::TrackMouseEvent(&tme))
			m_bTracking = TRUE;
	}

	CWnd* pParent = GetParent();
	if (pParent != NULL && pParent->GetSafeHwnd() != NULL)
		pParent->SendMessage(WM_FLATBTN_HOVER, (WPARAM)GetDlgCtrlID(), (LPARAM)TRUE);
}

// 鼠标离开按钮：清除跟踪标志并通知父对话框取消高亮
void CFlatBtn::OnMouseLeave()
{
	m_bTracking = FALSE;

	CWnd* pParent = GetParent();
	if (pParent != NULL && pParent->GetSafeHwnd() != NULL)
		pParent->SendMessage(WM_FLATBTN_HOVER, (WPARAM)GetDlgCtrlID(), (LPARAM)FALSE);

	CButton::OnMouseLeave();
}


// CDeskTidyDlg 对话框

CDeskTidyDlg::CDeskTidyDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_DESKTIDY_DIALOG, pParent)
	, m_hTrayIcon(NULL)
	, m_nSelIndex(-1)
	, m_nLayerMode(0)
	, m_nViewMode(0)
	, m_bAutoStart(FALSE)
	, m_bHideAtStartup(TRUE)   // 启动阶段隐藏主界面，只驻留系统托盘
	// WorkBuddy: 设置界面配色初始值＝默认配色（未选中任何小窗口时色块显示默认色）
	, m_clrHeaderSel(WIDGET_DEFAULT_HEADER_COLOR)
	, m_clrBgSel(WIDGET_DEFAULT_BG_COLOR)
	// WorkBuddy: 名称颜色默认"自动跟随背景"（m_clrTextSel 仅作为切到自定义时的初值）
	, m_clrTextSel(WIDGET_DEFAULT_TEXT_COLOR)
	, m_bTextAutoSel(TRUE)
	// WorkBuddy: 透明度初始值＝100%（255，完全不透明），与窗口成员默认值一致
	, m_nHeaderAlphaSel(255)
	, m_nBgAlphaSel(255)
	, m_bZOrderMaintain(TRUE)       // WorkBuddy: z 序维护默认开启（保持历史行为）
	, m_bBottomViaChild(FALSE)      // WorkBuddy: 桌面子窗口置底默认关闭
	, m_bTopHotKeyEnable(FALSE)
	, m_bBottomHotKeyEnable(FALSE)
	, m_bZoomInHotKeyEnable(FALSE)
	, m_bZoomOutHotKeyEnable(FALSE)
	, m_nTopHotKey(0)
	, m_nBottomHotKey(0)
	, m_nZoomInHotKey(0)
	, m_nZoomOutHotKey(0)
	, m_hHookForeground(NULL)   // WinEvent 钩子：初始未注册
, m_hHookShow(NULL)
, m_hHookReorder(NULL)
	, m_dwLastBottomPush(0)     // 尚未执行过压底
	, m_nHoverBtn(0)            // WorkBuddy: 初始没有按钮处于悬停态
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);

	// 读取当前开机自启动状态（注册表 Run 键），供"开机自启动"复选框显示
	m_bAutoStart = QueryAutoStart();

	// WorkBuddy: 创建现代扁平界面所需的 GDI 资源。放在构造函数里（而非
	//   OnInitDialog）是为了保证任何一次 WM_ERASEBKGND / WM_CTLCOLOR 到达时
	//   画刷与字体都已就绪，避免首次绘制出现黑底或字体缺失
	m_brDlgBg.CreateSolidBrush(UI_CLR_DLG_BG);
	m_brCard.CreateSolidBrush(UI_CLR_CARD);

	// 分区标题字体：Segoe UI Semibold，字号按系统 DPI 换算
	//   （非 DPI 感知时 GetDeviceCaps 返回 96，即 9pt = 12px 高）
	int nFontHeight = -12;                      // 兜底：9pt @ 96 DPI
	HDC hScreenDC = ::GetDC(NULL);
	if (hScreenDC != NULL)
	{
		const int nDpiY = ::GetDeviceCaps(hScreenDC, LOGPIXELSY);
		::ReleaseDC(NULL, hScreenDC);
		nFontHeight = -MulDiv(9, nDpiY, 72);    // 9pt -> 像素高度
	}

	LOGFONT lf;
	ZeroMemory(&lf, sizeof(lf));
	lf.lfHeight       = nFontHeight;
	lf.lfWeight       = FW_SEMIBOLD;            // 半粗：层次清晰，又不似 Bold 那样笨重
	lf.lfCharSet      = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_TT_PRECIS;          // 优先 TrueType，缩放不锯齿
	lf.lfQuality      = CLEARTYPE_QUALITY;      // 现代清晰度渲染
	// lstrcpyn 保证以 NULL 结尾且不会越界（比 _tcscpy_s 更少的 CRT 依赖）
	lstrcpyn(lf.lfFaceName, _T("Segoe UI Semibold"), LF_FACESIZE);
	m_fontSection.CreateFontIndirect(&lf);
}

// 静态实例指针定义（WinEvent 回调通过它访问主对话框实例）
CDeskTidyDlg* CDeskTidyDlg::s_pSelf = NULL;

void CDeskTidyDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_LIST_WIDGETS, m_listWidgets);
	DDX_Radio(pDX, IDC_RADIO_BOTTOM, m_nLayerMode);
	DDX_Radio(pDX, IDC_RADIO_LIST, m_nViewMode);
	DDX_Check(pDX, IDC_CHECK_AUTOSTART, m_bAutoStart);
	// WorkBuddy: 文件名称颜色的"自动跟随背景"复选框（勾选 = 自动，取消 = 用自定义色）
	DDX_Check(pDX, IDC_CHECK_TEXT_AUTO, m_bTextAutoSel);
	// WorkBuddy: z 序（层级）维护开关
	DDX_Check(pDX, IDC_CHECK_ZORDER_MAINTAIN, m_bZOrderMaintain);
	DDX_Check(pDX, IDC_CHECK_BOTTOM_CHILD, m_bBottomViaChild);
	// 全局快捷键控件与启用复选框
	DDX_Control(pDX, IDC_EDIT_TOP_HOTKEY, m_ctlTopHotKey);
	DDX_Control(pDX, IDC_EDIT_BOTTOM_HOTKEY, m_ctlBottomHotKey);
	DDX_Control(pDX, IDC_EDIT_ZOOMIN_HOTKEY, m_ctlZoomInHotKey);
	DDX_Control(pDX, IDC_EDIT_ZOOMOUT_HOTKEY, m_ctlZoomOutHotKey);
	DDX_Check(pDX, IDC_CHECK_TOP_HOTKEY, m_bTopHotKeyEnable);
	DDX_Check(pDX, IDC_CHECK_BOTTOM_HOTKEY, m_bBottomHotKeyEnable);
	DDX_Check(pDX, IDC_CHECK_ZOOMIN_HOTKEY, m_bZoomInHotKeyEnable);
	DDX_Check(pDX, IDC_CHECK_ZOOMOUT_HOTKEY, m_bZoomOutHotKeyEnable);
	// WorkBuddy: 选中窗口的透明度滑块（拖滑块走 OnHScroll，此处只做控件绑定；
	// 滑块范围在 OnInitDialog 中设置，待应用值由 m_nHeaderAlphaSel/m_nBgAlphaSel 保存）
	DDX_Control(pDX, IDC_SLIDER_HEADER_ALPHA, m_sliderHeaderAlpha);
	DDX_Control(pDX, IDC_SLIDER_BG_ALPHA, m_sliderBgAlpha);
}

BEGIN_MESSAGE_MAP(CDeskTidyDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_WINDOWPOSCHANGING()
	ON_MESSAGE(WM_HOTKEY, &CDeskTidyDlg::OnHotKey)
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_WM_TIMER()
	ON_WM_CLOSE()
	ON_WM_DESTROY()
	ON_MESSAGE(WM_TRAYICON, &CDeskTidyDlg::OnTrayIcon)
	ON_MESSAGE(WM_WIDGET_CHANGED, &CDeskTidyDlg::OnWidgetChanged)
	// WorkBuddy: 单实例——第二个进程重复启动时投递此消息，请本实例显示主界面
	// （消息 ID 定义见 DeskTidyWidget.h 的自定义消息区）
	ON_MESSAGE(WM_DESKTIDY_ACTIVATE, &CDeskTidyDlg::OnActivateExisting)
	ON_COMMAND(ID_TRAY_SHOW, &CDeskTidyDlg::OnTrayShow)
	ON_COMMAND(ID_TRAY_SHOW_WIDGETS, &CDeskTidyDlg::OnTrayShowWidgets)
	ON_COMMAND(ID_TRAY_HIDE_WIDGETS, &CDeskTidyDlg::OnTrayHideWidgets)
	ON_COMMAND(ID_TRAY_EXIT, &CDeskTidyDlg::OnTrayExit)
	ON_BN_CLICKED(IDC_BTN_ADD, &CDeskTidyDlg::OnBnClickedAdd)
	ON_BN_CLICKED(IDC_BTN_DEL, &CDeskTidyDlg::OnBnClickedDelete)
	ON_BN_CLICKED(IDC_BTN_APPLY, &CDeskTidyDlg::OnBnClickedApply)
	ON_LBN_SELCHANGE(IDC_LIST_WIDGETS, &CDeskTidyDlg::OnSelChangeWidget)
	// WorkBuddy: 配色相关（标题栏颜色 / 背景色 / 文件名称颜色 / 恢复默认 / 自绘色块）
	ON_BN_CLICKED(IDC_BTN_HEADER_COLOR, &CDeskTidyDlg::OnBnClickedHeaderColor)
	ON_BN_CLICKED(IDC_BTN_BG_COLOR, &CDeskTidyDlg::OnBnClickedBgColor)
	ON_BN_CLICKED(IDC_BTN_TEXT_COLOR, &CDeskTidyDlg::OnBnClickedTextColor)
	ON_BN_CLICKED(IDC_CHECK_TEXT_AUTO, &CDeskTidyDlg::OnBnClickedTextAuto)
	ON_BN_CLICKED(IDC_BTN_COLOR_RESET, &CDeskTidyDlg::OnBnClickedColorReset)
	ON_WM_DRAWITEM()
	// WorkBuddy: 透明度滑块（两个 CSliderCtrl 都通过 WM_HSCROLL 通知父窗口）
	ON_WM_HSCROLL()
	// WorkBuddy: 现代扁平界面（自绘背景与卡片 / 控件配色 / 自绘列表行高 / 按钮悬停）
	ON_WM_ERASEBKGND()
	ON_WM_CTLCOLOR()
	ON_WM_MEASUREITEM()
	ON_MESSAGE(WM_FLATBTN_HOVER, &CDeskTidyDlg::OnFlatBtnHover)
END_MESSAGE_MAP()


// CDeskTidyDlg 消息处理程序

BOOL CDeskTidyDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// 将“关于...”菜单项添加到系统菜单中。

	// IDM_ABOUTBOX 必须在系统命令范围内。
	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);

	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != nullptr)
	{
		BOOL bNameValid;
		CString strAboutMenu;
		bNameValid = strAboutMenu.LoadString(IDS_ABOUTBOX);
		ASSERT(bNameValid);
		if (!strAboutMenu.IsEmpty())
		{
			pSysMenu->AppendMenu(MF_SEPARATOR);
			pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
		}
	}

	// 设置此对话框的图标。  当应用程序主窗口不是对话框时，框架将自动
	//  执行此操作
	SetIcon(m_hIcon, TRUE);			// 设置大图标
	SetIcon(m_hIcon, FALSE);		// 设置小图标

	// WorkBuddy: Win11 圆角窗口——DWM 合成层直接对整窗圆角（含阴影/贴边适配）。
	// 低版本系统上调用失败即保持原直角外观，无任何副作用
	{
		int nPref = DWMWCP_ROUND;
		::DwmSetWindowAttribute(GetSafeHwnd(), DWMWA_WINDOW_CORNER_PREFERENCE,
		                        &nPref, sizeof(nPref));
	}

	// ---- 业务初始化 ----

	// 1. 从 INI 读取配置并创建全部小窗口
	LoadConfig();

	// 2. 把每个小窗口对应的目录填充到列表控件
	for (int i = 0; i < (int)m_arrWidgets.GetCount(); i++)
		m_listWidgets.AddString(m_arrWidgets[i]->GetDirectory());

	// 3. 默认选中第一个小窗口，载入其属性到单选按钮
	if (m_listWidgets.GetCount() > 0)
	{
		m_listWidgets.SetCurSel(0);
		LoadSelProps();
	}

	// 4. 全局快捷键：把 INI 载入的快捷键配置同步到界面控件。
	//    - 已指定快捷键（值非 0）时勾选对应"启用"复选框；
	//    - UpdateData(FALSE) 把启用状态显示到复选框；
	//    - 再把保存的快捷键组合显示到 CHotKeyCtrl 输入框中。
	m_bTopHotKeyEnable    = (m_nTopHotKey    != 0);
	m_bBottomHotKeyEnable = (m_nBottomHotKey != 0);
	m_bZoomInHotKeyEnable  = (m_nZoomInHotKey  != 0);
	m_bZoomOutHotKeyEnable = (m_nZoomOutHotKey != 0);
	UpdateData(FALSE);
	SetHotKeyToCtrl(m_ctlTopHotKey, m_nTopHotKey);
	SetHotKeyToCtrl(m_ctlBottomHotKey, m_nBottomHotKey);
	SetHotKeyToCtrl(m_ctlZoomInHotKey, m_nZoomInHotKey);
	SetHotKeyToCtrl(m_ctlZoomOutHotKey, m_nZoomOutHotKey);

	// 5. 添加系统托盘图标
	AddTrayIcon();

	// 6. 注册全局快捷键（按 INI 配置；若组合已被其他程序占用会提示）
	RegisterHotKeys();

	// 7. 注册 WinEvent 事件钩子，事件驱动地维护"最底层"小窗口的 z 序：
	//    监听到前台窗口变化/窗口显示等事件后，把置底的小窗口重新压回最底。
	//    钩子以 WINEVENT_OUTOFCONTEXT 方式注册，回调在本对话框的消息循环
	//    （DoModal 模态循环）中执行，因此可安全操作本窗口与定时器。
	//    同时记录静态实例指针，供静态回调访问
	s_pSelf = this;
	RegisterWinEventHooks();

	// 8. 启动小窗口存活巡检定时器：
	//    置底小窗口挂接为桌面子窗口后会被 Explorer 重启连带销毁，
	//    周期检查并重建，保证 Explorer 重启后小窗口自动恢复
	SetTimer(TIMER_ID_WIDGET_CHECK, WIDGET_CHECK_INTERVAL, NULL);

	// 9. 启动后自动"显示桌面"：最小化其它应用窗口，让桌面与 DeskTidy
	//    小窗口显露出来。放在窗口创建/定时器就绪之后，确保所有小窗口句柄
	//    都已存在，从而能被正确排除在"最小化"范围之外
	ShowDesktopAll();

	// 10. 启动后隐藏主界面，程序驻留系统托盘。
	//    MFC 的 DoModal 会在消息循环空闲时自动 ShowWindow(SW_SHOWNORMAL)
	//    把模态对话框显示出来；RunModalLoop 是非虚函数、无法通过重载拦截，
	//    因此这里不做处理，改由 OnWindowPosChanging 在首次"显示"时强制隐藏。

	// 11. WorkBuddy: 明确两个透明度滑块的范围（0~100%，单位百分比）。
	//     滑块控件默认范围本就是 0~100，这里显式声明以保证语义清晰；
	//     滑块位置与百分比文字已在步骤 3（LoadSelProps → UpdateAlphaControls）
	//     中按选中窗口的实际透明度设置好，此处仅补范围，不影响已设位置。
	//     先判句柄有效：DDX_Control 的绑定发生在基类 OnInitDialog 内，
	//     正常此时已完成；万一绑定失败也不至于在 SetRange 里触发断言
	if (m_sliderHeaderAlpha.GetSafeHwnd() != NULL)
		m_sliderHeaderAlpha.SetRange(0, 100, TRUE);
	if (m_sliderBgAlpha.GetSafeHwnd() != NULL)
		m_sliderBgAlpha.SetRange(0, 100, TRUE);

	// 12. WorkBuddy: 现代扁平界面接线。
	//     四个自绘按钮改用 CFlatBtn 子类化，以便感知鼠标悬停（对话框本身收不到
	//     鼠标在子控件上的移动消息）；按钮外观仍由 OnDrawItem 统一绘制。
	//     列表行高由 OnMeasureItem 按当前字体动态给出，此处无需额外设置。
	//     最后整体重绘一次，让首次显示即为新样式（背景 + 卡片）。
	m_btnAdd.SubclassDlgItem(IDC_BTN_ADD, this);
	m_btnDelete.SubclassDlgItem(IDC_BTN_DEL, this);
	m_btnApply.SubclassDlgItem(IDC_BTN_APPLY, this);
	m_btnColorReset.SubclassDlgItem(IDC_BTN_COLOR_RESET, this);
	// 三个色块按钮同样子类化：悬停时描边点亮，提示"此处可点击取色"
	m_btnHeaderColor.SubclassDlgItem(IDC_BTN_HEADER_COLOR, this);
	m_btnBgColor.SubclassDlgItem(IDC_BTN_BG_COLOR, this);
	m_btnTextColor.SubclassDlgItem(IDC_BTN_TEXT_COLOR, this);
	Invalidate();
	return TRUE;  // 除非将焦点设置到控件，否则返回 TRUE
}

// 启动时拦截首次"显示"：把主界面（参数设置界面）保持在隐藏状态，程序只驻留系统托盘。
// 原理：窗口每次改变位置/大小/可见性之前都会收到 WM_WINDOWPOSCHANGING 消息，
// 而 DoModal 的消息循环空闲时正是通过 ShowWindow(SW_SHOWNORMAL) 显示对话框的，
// 该调用会触发本消息；在这里把"显示"请求改写成"隐藏"即可阻止窗口出现。
void CDeskTidyDlg::OnWindowPosChanging(WINDOWPOS* lpwndpos)
{
	CDialogEx::OnWindowPosChanging(lpwndpos);

	// 仅启动阶段拦截；用户通过托盘菜单/双击托盘图标主动"显示主界面"时，
	// ShowMainWindow 会先清除 m_bHideAtStartup 标志，因此不再被隐藏。
	if (m_bHideAtStartup)
	{
		lpwndpos->flags &= ~SWP_SHOWWINDOW;   // 取消"显示窗口"请求
		lpwndpos->flags |= SWP_HIDEWINDOW;    // 强制窗口保持隐藏
	}
}

void CDeskTidyDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	if ((nID & 0xFFF0) == IDM_ABOUTBOX)
	{
		CAboutDlg dlgAbout;
		dlgAbout.DoModal();
	}
	else
	{
		CDialogEx::OnSysCommand(nID, lParam);
	}
}

// 如果向对话框添加最小化按钮，则需要下面的代码
//  来绘制该图标。  对于使用文档/视图模型的 MFC 应用程序，
//  这将由框架自动完成。

void CDeskTidyDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // 用于绘制的设备上下文

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// 使图标在工作区矩形中居中
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// 绘制图标
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}

//当用户拖动最小化窗口时系统调用此函数取得光标
//显示。
HCURSOR CDeskTidyDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

// 点击标题栏 X：仅隐藏主界面，程序继续驻留托盘
void CDeskTidyDlg::OnClose()
{
	SaveConfig();           // 记住当前所有小窗口的设置与位置
	ShowWindow(SW_HIDE);
}

// 对话框销毁：反注册 WinEvent 钩子与全局快捷键，清理托盘图标并释放全部小窗口
void CDeskTidyDlg::OnDestroy()
{
	// 先清除静态实例指针，再反注册事件钩子：
	// 确保此后不再有回调访问本窗口，避免钩子回调用到已销毁的窗口
	s_pSelf = NULL;
	UnregisterWinEventHooks();

	KillTimer(TIMER_ID_WIDGET_CHECK);   // 停止小窗口存活巡检定时器
	UnregisterHotKeys();    // 反注册全局快捷键，避免进程退出后快捷键仍被占用
	RemoveTrayIcon();
	DestroyAllWidgets();
	CDialogEx::OnDestroy();
}

// ---------------------------------------------------------------------------
// 托盘相关
// ---------------------------------------------------------------------------

// 添加系统托盘图标
BOOL CDeskTidyDlg::AddTrayIcon()
{
	m_hTrayIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);

	NOTIFYICONDATA nid = {};
	nid.cbSize = sizeof(NOTIFYICONDATA);
	nid.hWnd = m_hWnd;              // 托盘消息发送给本对话框
	nid.uID = 1;                    // 托盘图标 ID
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_TRAYICON;   // 回调消息号
	nid.hIcon = m_hTrayIcon;
	lstrcpyn(nid.szTip, _T("桌面归置"), _countof(nid.szTip));

	return Shell_NotifyIcon(NIM_ADD, &nid);
}

// 移除系统托盘图标
void CDeskTidyDlg::RemoveTrayIcon()
{
	NOTIFYICONDATA nid = {};
	nid.cbSize = sizeof(NOTIFYICONDATA);
	nid.hWnd = m_hWnd;
	nid.uID = 1;
	Shell_NotifyIcon(NIM_DELETE, &nid);
}

// 显示主界面并置于前台（托盘菜单"显示主界面"/双击托盘图标时调用）
void CDeskTidyDlg::ShowMainWindow()
{
	m_bHideAtStartup = FALSE;   // 用户主动要求显示，此后不再拦截隐藏
	ShowWindow(SW_SHOW);
	ShowWindow(SW_RESTORE);
	SetForegroundWindow();

	// WorkBuddy: 主界面隐藏期间用户可能通过小窗口右键菜单改过配色，
	// 重新显示时刷新一次色块，保证界面与当前配置一致
	LoadSelProps();
}

// 托盘图标回调：左键双击显示主界面，右键弹出托盘菜单
LRESULT CDeskTidyDlg::OnTrayIcon(WPARAM wParam, LPARAM lParam)
{
	// wParam 是我们的托盘图标 ID
	if (wParam != 1)
		return 0;

	switch (LOWORD(lParam))
	{
	case WM_RBUTTONUP:
	{
		// 右键：在鼠标位置弹出托盘菜单
		::SetForegroundWindow(m_hWnd);      // 防止菜单打开即被关闭
		CMenu menu;
		menu.LoadMenu(IDR_TRAY_MENU);
		CMenu* pPopup = menu.GetSubMenu(0);
		CPoint pt;
		GetCursorPos(&pt);
		pPopup->TrackPopupMenu(TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, this);
		::PostMessage(m_hWnd, WM_NULL, 0, 0);   // 让菜单正常关闭
		break;
	}
	case WM_LBUTTONDBLCLK:
		// 左键双击：显示主界面
		ShowMainWindow();
		break;
	}
	return 0;
}

// 小窗口状态变更（拖动/缩放结束、右键切换视图/层级后触发）：
// 直接把最新配置写入 INI，防止异常退出丢失位置/大小
LRESULT CDeskTidyDlg::OnWidgetChanged(WPARAM wParam, LPARAM lParam)
{
	SaveConfig();

	// WorkBuddy: 小窗口通过右键菜单改配色/透明度也会走到这里；若被改的正是当前
	// 选中项，且设置界面可见，则同步刷新色块与透明度滑块，避免界面显示与实际不一致。
	// （界面隐藏时不刷，避免拖动/缩放期间的高频无效重绘）
	if (IsWindowVisible() &&
	    m_nSelIndex >= 0 && m_nSelIndex < (int)m_arrWidgets.GetCount())
	{
		m_clrHeaderSel = m_arrWidgets[m_nSelIndex]->GetHeaderColor();
		m_clrBgSel     = m_arrWidgets[m_nSelIndex]->GetBgColor();
		m_nHeaderAlphaSel = (int)m_arrWidgets[m_nSelIndex]->GetHeaderAlpha();
		m_nBgAlphaSel     = (int)m_arrWidgets[m_nSelIndex]->GetBgAlpha();
		UpdateColorButtons();
		UpdateAlphaControls();
	}
	return 0;
}

// WorkBuddy: 单实例——第二个（重复启动的）进程会向本窗口投递此消息，
// 请本实例把主界面显示到前台并激活。
// 效果：用户无论从桌面图标、开始菜单还是任务栏重复启动 DeskTidy，
//       都只会唤出这唯一一个实例的设置界面，不会再起第二个进程。
LRESULT CDeskTidyDlg::OnActivateExisting(WPARAM wParam, LPARAM lParam)
{
	ShowMainWindow();

	// 兜底提示：Windows 的前台锁定策略偶尔会拒绝非用户的显式操作抢前台
	// （SetForegroundWindow 被忽略），此时窗口虽然已显示却停在后面。让任务栏
	// 图标闪烁几下，用户就知道"程序响应了，只是窗口在别的窗口后面"，
	// 而不是误以为双击没反应
	if (::GetForegroundWindow() != m_hWnd)
	{
		FLASHWINFO fw = {};
		fw.cbSize    = sizeof(fw);
		fw.hwnd      = m_hWnd;
		fw.dwFlags   = FLASHW_ALL;
		fw.uCount    = 3;
		fw.dwTimeout = 0;
		::FlashWindowEx(&fw);
	}
	return 0;
}

// 托盘菜单：显示主界面
void CDeskTidyDlg::OnTrayShow()
{
	ShowMainWindow();
}

// 托盘菜单：显示全部小窗口（不激活任何窗口，保持原有层级/折叠状态）
void CDeskTidyDlg::OnTrayShowWidgets()
{
	for (int i = 0; i < (int)m_arrWidgets.GetCount(); i++)
	{
		CDeskTidyWidget* pWnd = m_arrWidgets[i];
		if (pWnd->GetSafeHwnd() != NULL)
			pWnd->SetUserVisible(TRUE);    // 记录用户意图 + 显示（不抢占焦点）
	}
}

// 托盘菜单：隐藏全部小窗口（小窗口保留在数组中，可随时再显示）
void CDeskTidyDlg::OnTrayHideWidgets()
{
	for (int i = 0; i < (int)m_arrWidgets.GetCount(); i++)
	{
		CDeskTidyWidget* pWnd = m_arrWidgets[i];
		if (pWnd->GetSafeHwnd() != NULL)
			pWnd->SetUserVisible(FALSE);   // 记录用户意图 + 隐藏
	}
}

// 托盘菜单：退出应用
void CDeskTidyDlg::OnTrayExit()
{
	// 先保存配置，再清理托盘与小窗口，最后结束对话框（程序退出）
	SaveConfig();
	RemoveTrayIcon();
	DestroyAllWidgets();
	EndDialog(IDOK);
}

// ---------------------------------------------------------------------------
// 全局快捷键（置顶 / 置底 / 缩放放大 / 缩放缩小）
// ---------------------------------------------------------------------------
// 说明：
//   - 通过 RegisterHotKey 注册为"全局"快捷键，无论焦点在哪个程序，
//     按下组合键都会把 WM_HOTKEY 投递给本对话框（主界面隐藏时同样生效）；
//   - 四个快捷键都可选（不指定 = 不注册）：置顶 / 置底 / 缩放放大 / 缩放缩小；
//   - 指定时做三重校验：必须输入按键、至少含一个修饰键（Ctrl/Alt/Shift）、
//     四个快捷键两两不能相同；RegisterHotKey 失败即表示与系统或其他程序冲突；
//   - 快捷键值格式：高 16 位 = 修饰键位（MOD_*），低 16 位 = 虚拟键码；
//     0 表示未指定，该值直接存 INI（[Settings] 节）。

// 全局快捷键回调：系统在用户按下已注册的组合键时投递 WM_HOTKEY 到本窗口
LRESULT CDeskTidyDlg::OnHotKey(WPARAM wParam, LPARAM /*lParam*/)
{
	if (wParam == HOTKEY_ID_TOPMOST)
		ApplyAllWidgetLayer(1);     // 置顶快捷键：全部小窗口悬浮置顶
	else if (wParam == HOTKEY_ID_BOTTOM)
		ApplyAllWidgetLayer(0);     // 置底快捷键：全部小窗口最底层
	else if (wParam == HOTKEY_ID_ZOOMIN)
		ApplyAllWidgetZoom(1);      // 缩放放大快捷键：全部小窗口放大一档
	else if (wParam == HOTKEY_ID_ZOOMOUT)
		ApplyAllWidgetZoom(-1);     // 缩放缩小快捷键：全部小窗口缩小一档
	return 0;
}

// 把所有小窗口统一设置为指定层级（0 = 最底层，1 = 悬浮置顶），
// 并立即把最新配置写入 INI，保证快捷键调整后的层级在重启后依然生效
void CDeskTidyDlg::ApplyAllWidgetLayer(int nMode)
{
	for (int i = 0; i < (int)m_arrWidgets.GetCount(); i++)
	{
		CDeskTidyWidget* pWnd = m_arrWidgets[i];
		if (pWnd->GetSafeHwnd() != NULL)
			pWnd->SetLayerMode(nMode);
	}
	SaveConfig();
}

// 把所有小窗口统一放大/缩小一档缩放级别（nStep = 1 放大、-1 缩小），
// 并立即把最新配置写入 INI，保证快捷键调整后的缩放级别在重启后依然生效。
// 说明：与置顶/置底快捷键一致，缩放快捷键对所有小窗口同时生效；
// 每个小窗口可再用 Ctrl+鼠标滚轮单独微调
void CDeskTidyDlg::ApplyAllWidgetZoom(int nStep)
{
	for (int i = 0; i < (int)m_arrWidgets.GetCount(); i++)
	{
		CDeskTidyWidget* pWnd = m_arrWidgets[i];
		if (pWnd->GetSafeHwnd() != NULL)
			pWnd->ZoomStep(nStep);
	}
	SaveConfig();
}

// 启动时按 INI 配置注册四个全局快捷键（置顶/置底/缩放放大/缩放缩小）。
// 未指定的组合（值为 0）跳过；注册失败说明组合已被系统或其他程序占用，给出提示
void CDeskTidyDlg::RegisterHotKeys()
{
	if (m_nTopHotKey != 0)
	{
		UINT uMods, uVk;
		SplitHotKey(m_nTopHotKey, uMods, uVk);
		if (!::RegisterHotKey(m_hWnd, HOTKEY_ID_TOPMOST, uMods, uVk))
			AfxMessageBox(_T("“置顶”快捷键注册失败：该组合已被系统或其他程序占用。"));
	}
	if (m_nBottomHotKey != 0)
	{
		UINT uMods, uVk;
		SplitHotKey(m_nBottomHotKey, uMods, uVk);
		if (!::RegisterHotKey(m_hWnd, HOTKEY_ID_BOTTOM, uMods, uVk))
			AfxMessageBox(_T("“置底”快捷键注册失败：该组合已被系统或其他程序占用。"));
	}
	if (m_nZoomInHotKey != 0)
	{
		UINT uMods, uVk;
		SplitHotKey(m_nZoomInHotKey, uMods, uVk);
		if (!::RegisterHotKey(m_hWnd, HOTKEY_ID_ZOOMIN, uMods, uVk))
			AfxMessageBox(_T("“缩放放大”快捷键注册失败：该组合已被系统或其他程序占用。"));
	}
	if (m_nZoomOutHotKey != 0)
	{
		UINT uMods, uVk;
		SplitHotKey(m_nZoomOutHotKey, uMods, uVk);
		if (!::RegisterHotKey(m_hWnd, HOTKEY_ID_ZOOMOUT, uMods, uVk))
			AfxMessageBox(_T("“缩放缩小”快捷键注册失败：该组合已被系统或其他程序占用。"));
	}
}

// 反注册四个全局快捷键（未注册时调用无副作用）
void CDeskTidyDlg::UnregisterHotKeys()
{
	::UnregisterHotKey(m_hWnd, HOTKEY_ID_TOPMOST);
	::UnregisterHotKey(m_hWnd, HOTKEY_ID_BOTTOM);
	::UnregisterHotKey(m_hWnd, HOTKEY_ID_ZOOMIN);
	::UnregisterHotKey(m_hWnd, HOTKEY_ID_ZOOMOUT);
}

// 从界面控件读取并校验四个快捷键（置顶/置底/缩放放大/缩放缩小），
// 通过后注册并更新成员变量。
// 返回 TRUE = 已生效；返回 FALSE = 校验/注册失败（给出提示），
// 此时不修改成员变量，保证不会把未生效的快捷键写入 INI
BOOL CDeskTidyDlg::ApplyHotKeys()
{
	// 未勾选"启用"的快捷键一律视为不指定
	int nTop     = m_bTopHotKeyEnable    ? GetHotKeyFromCtrl(m_ctlTopHotKey)    : 0;
	int nBottom  = m_bBottomHotKeyEnable ? GetHotKeyFromCtrl(m_ctlBottomHotKey) : 0;
	int nZoomIn  = m_bZoomInHotKeyEnable ? GetHotKeyFromCtrl(m_ctlZoomInHotKey) : 0;
	int nZoomOut = m_bZoomOutHotKeyEnable ? GetHotKeyFromCtrl(m_ctlZoomOutHotKey) : 0;

	// 四个快捷键的名称（用于提示信息）
	LPCTSTR aNames[] = { _T("置顶"), _T("置底"), _T("缩放放大"), _T("缩放缩小") };

	// 校验 1：勾选了"启用"但输入框中没有按键
	int aVals[] = { nTop, nBottom, nZoomIn, nZoomOut };
	BOOL aEnables[] = { m_bTopHotKeyEnable, m_bBottomHotKeyEnable,
						m_bZoomInHotKeyEnable, m_bZoomOutHotKeyEnable };
	for (int i = 0; i < 4; i++)
	{
		if (aEnables[i] && aVals[i] == 0)
		{
			CString strMsg;
			strMsg.Format(_T("已勾选“%s”的启用，请先在输入框中按下组合键。"), aNames[i]);
			AfxMessageBox(strMsg);
			return FALSE;
		}
	}

	// 校验 2：至少包含一个修饰键（Ctrl/Alt/Shift），防止误触普通按键
	for (int i = 0; i < 4; i++)
	{
		if (aVals[i] != 0 && (aVals[i] & 0xFFFF0000) == 0)
		{
			CString strMsg;
			strMsg.Format(_T("“%s”快捷键至少需要包含 Ctrl、Alt 或 Shift 中的一个修饰键。"), aNames[i]);
			AfxMessageBox(strMsg);
			return FALSE;
		}
	}

	// 校验 3：四个快捷键两两不能完全相同（互相冲突）
	for (int i = 0; i < 4; i++)
	{
		for (int j = i + 1; j < 4; j++)
		{
			if (aVals[i] != 0 && aVals[i] == aVals[j])
			{
				CString strMsg;
				strMsg.Format(_T("“%s”与“%s”的快捷键不能设置为相同组合。"), aNames[i], aNames[j]);
				AfxMessageBox(strMsg);
				return FALSE;
			}
		}
	}

	// 先反注册当前已生效的快捷键，再注册新值（避免重复点击"应用"造成叠加注册）
	UnregisterHotKeys();

	// 四个快捷键的注册 ID
	int aIds[] = { HOTKEY_ID_TOPMOST, HOTKEY_ID_BOTTOM,
				   HOTKEY_ID_ZOOMIN, HOTKEY_ID_ZOOMOUT };

	// 逐个注册；任一失败则回滚本次已成功注册的全部快捷键，并提示冲突
	for (int i = 0; i < 4; i++)
	{
		if (aVals[i] == 0)
			continue;   // 未指定：不注册

		UINT uMods, uVk;
		SplitHotKey(aVals[i], uMods, uVk);
		if (!::RegisterHotKey(m_hWnd, aIds[i], uMods, uVk))
		{
			// 回滚：反注册本次循环中已成功注册的快捷键
			for (int k = 0; k < i; k++)
			{
				if (aVals[k] != 0)
					::UnregisterHotKey(m_hWnd, aIds[k]);
			}
			CString strMsg;
			strMsg.Format(_T("“%s”快捷键注册失败：该组合已被系统或其他程序占用，请换一个组合。"), aNames[i]);
			AfxMessageBox(strMsg);
			return FALSE;
		}
	}

	// 全部成功：更新成员（供 WM_HOTKEY 回调与 INI 持久化使用）
	m_nTopHotKey    = nTop;
	m_nBottomHotKey = nBottom;
	m_nZoomInHotKey  = nZoomIn;
	m_nZoomOutHotKey = nZoomOut;
	return TRUE;
}

// 读取快捷键控件中的组合，转换为保存格式（高 16 位 = 修饰键 MOD_*，低 16 位 = 虚拟键码）。
// 输入框为空（未按下按键）时返回 0，表示未指定
int CDeskTidyDlg::GetHotKeyFromCtrl(CHotKeyCtrl& ctrl)
{
	WORD wVk = 0, wMods = 0;
	ctrl.GetHotKey(wVk, wMods);

	if (wVk == 0)
		return 0;   // 未输入按键

	// 把 CHotKeyCtrl 的修饰键标志（HOTKEYF_*）转换为 RegisterHotKey 的标志（MOD_*）
	UINT uMods = 0;
	if (wMods & HOTKEYF_ALT)     uMods |= MOD_ALT;
	if (wMods & HOTKEYF_CONTROL) uMods |= MOD_CONTROL;
	if (wMods & HOTKEYF_SHIFT)   uMods |= MOD_SHIFT;
	// HOTKEYF_EXT（扩展键标志）不映射：CHotKeyCtrl 不支持把 Win 键作为修饰键

	return (int)((uMods << 16) | wVk);
}

// 把保存的快捷键值显示到快捷键控件（与 GetHotKeyFromCtrl 互逆）
void CDeskTidyDlg::SetHotKeyToCtrl(CHotKeyCtrl& ctrl, int nHotKey)
{
	if (nHotKey == 0)
	{
		ctrl.SetHotKey(0, 0);   // 未指定：清空显示
		return;
	}

	WORD wVk   = (WORD)(nHotKey & 0xFFFF);
	WORD wMods = 0;
	if (nHotKey & ((int)MOD_ALT << 16))     wMods |= HOTKEYF_ALT;
	if (nHotKey & ((int)MOD_CONTROL << 16)) wMods |= HOTKEYF_CONTROL;
	if (nHotKey & ((int)MOD_SHIFT << 16))   wMods |= HOTKEYF_SHIFT;
	ctrl.SetHotKey(wVk, wMods);
}

// 把保存的快捷键值拆分为 RegisterHotKey 所需的修饰键与虚拟键码
void CDeskTidyDlg::SplitHotKey(int nHotKey, UINT& uMods, UINT& uVk)
{
	uMods = (UINT)(((DWORD)nHotKey) >> 16) & 0xFFFF;
	uVk   = (UINT)(nHotKey & 0xFFFF);
}

// ---------------------------------------------------------------------------
// 小窗口管理：添加 / 删除 / 应用属性
// ---------------------------------------------------------------------------

// 添加小窗口：弹出目录选择对话框，为所选目录创建新的桌面小窗口
void CDeskTidyDlg::OnBnClickedAdd()
{
	if (m_arrWidgets.GetCount() >= MAX_WIDGETS)
	{
		AfxMessageBox(_T("小窗口数量已达上限。"));
		return;
	}

	CFolderPickerDialog dlg(NULL, BIF_NEWDIALOGSTYLE | BIF_RETURNONLYFSDIRS, this);
	if (dlg.DoModal() != IDOK)
		return;
	CString strDir = dlg.GetPathName();

	// 避免与已有小窗口显示相同目录
	for (int i = 0; i < (int)m_arrWidgets.GetCount(); i++)
	{
		if (m_arrWidgets[i]->GetDirectory().CompareNoCase(strDir) == 0)
		{
			AfxMessageBox(_T("该目录已有一个小窗口在显示。"));
			return;
		}
	}

	// 创建新小窗口：默认矩形 + 目录
	CDeskTidyWidget* pWnd = new CDeskTidyWidget;
	if (!pWnd->CreateWidget(GetDefaultRect(), strDir, m_hWnd))
	{
		delete pWnd;
		AfxMessageBox(_T("小窗口创建失败。"));
		return;
	}
	m_arrWidgets.Add(pWnd);

	// 加入列表并选中，载入其属性
	int nIdx = m_listWidgets.AddString(strDir);
	m_listWidgets.SetCurSel(nIdx);
	LoadSelProps();

	// 立即保存配置
	SaveConfig();
}

// 删除选中的小窗口：销毁窗口并移出列表
void CDeskTidyDlg::OnBnClickedDelete()
{
	int nSel = m_listWidgets.GetCurSel();
	if (nSel == LB_ERR || nSel >= (int)m_arrWidgets.GetCount())
	{
		AfxMessageBox(_T("请先在列表中选中小窗口。"));
		return;
	}

	// 销毁并释放窗口对象
	CDeskTidyWidget* pWnd = m_arrWidgets[nSel];
	if (pWnd->GetSafeHwnd() != NULL)
		pWnd->DestroyWindow();
	delete pWnd;
	m_arrWidgets.RemoveAt(nSel);
	m_listWidgets.DeleteString(nSel);

	// 重新选择一项（优先同级，其次最后一项）
	int nCount = m_listWidgets.GetCount();
	if (nCount > 0)
	{
		if (nSel >= nCount)
			nSel = nCount - 1;
		m_listWidgets.SetCurSel(nSel);
	}
	LoadSelProps();

	// 立即保存配置
	SaveConfig();
}

// 应用按钮：应用"开机自启动"复选框、"全局快捷键"与选中窗口的属性
void CDeskTidyDlg::OnBnClickedApply()
{
	UpdateData(TRUE);   // 取回单选按钮、复选框与快捷键控件状态

	// 1. 应用开机自启动设置：写入/删除注册表 Run 键值（与选中项无关）
	SetAutoStart(m_bAutoStart != FALSE);

	// 2. 应用全局快捷键：从输入控件读取、校验冲突并注册（与选中项无关）。
	//    校验或注册失败时会给出提示并返回 FALSE，此时不保存配置
	if (!ApplyHotKeys())
		return;

	if (m_nSelIndex < 0 || m_nSelIndex >= (int)m_arrWidgets.GetCount())
	{
		AfxMessageBox(_T("请先在列表中选中小窗口。"));
		return;
	}

	// 3. 应用到选中的小窗口
	CDeskTidyWidget* pWnd = m_arrWidgets[m_nSelIndex];
	pWnd->SetLayerMode(m_nLayerMode);
	pWnd->SetViewMode(m_nViewMode);

	// WorkBuddy: 应用设置界面选定的配色。SetHeaderColor/SetBgColor 内部会
	// 让小窗口立即按新配色重绘并重新合成（分层窗口），无需额外刷新
	pWnd->SetHeaderColor(m_clrHeaderSel);
	pWnd->SetBgColor(m_clrBgSel);

	// WorkBuddy: 应用"文件名称颜色"（文件列表项文字）。勾选"自动"时让小窗口
	// 按背景亮度自动取黑/白，否则固定使用色块上的颜色。
	// 注意顺序：必须在 SetBgColor 之后——自动色依赖背景色，背景先落地才能算对
	pWnd->SetTextColor(m_clrTextSel, m_bTextAutoSel);

	// WorkBuddy: 应用透明度（滑块上的百分比已换算为 0~255 的 alpha）。
	// SetHeaderAlpha/SetBgAlpha 同样会触发立即重新合成，两者互相独立：
	// 标题栏区域用标题栏 alpha，标题栏以下（背景 + 文件列表）用背景 alpha
	pWnd->SetHeaderAlpha((BYTE)m_nHeaderAlphaSel);
	pWnd->SetBgAlpha((BYTE)m_nBgAlphaSel);

	// WorkBuddy: 应用 z 序（层级）维护开关。立即生效：
	// 关闭会停掉维护定时器，并让 MaintainZOrder() 直接返回
	pWnd->SetZOrderMaintain(m_bZOrderMaintain);

	// WorkBuddy: 应用"桌面子窗口置底"开关，必须在 SetLayerMode 之前落地——
	// SetLayerMode→SetBottomLayer 会据此选择置底方案（子窗口 / 旧方案）
	pWnd->SetBottomViaChild(m_bBottomViaChild);

	// 保存到 INI
	SaveConfig();
}

// 列表选择变化：把选中窗口的属性载入单选按钮
void CDeskTidyDlg::OnSelChangeWidget()
{
	LoadSelProps();
}

// ---------------------------------------------------------------------------
// WorkBuddy: 选中窗口的配色设置（标题栏颜色 / 背景色）
// ---------------------------------------------------------------------------
// 交互方式与层级/视图等属性一致：在设置界面里改的是"待应用值"（m_clrHeaderSel /
// m_clrBgSel，同时用自绘色块做实时预览），点【应用】才写回小窗口并保存到 INI。
// 小窗口自身右键菜单里的取色入口保持可用，两条路径改的是同一份配置。

// 亮度自适应文字色（Rec.601 感知亮度）：亮底色用黑字、暗底色用白字。
// 判据与小窗口 CDeskTidyWidget::GetContrastTextColor 完全一致，保证两边观感统一
static COLORREF DlgContrastTextColor(COLORREF clr)
{
	const int r = GetRValue(clr);
	const int g = GetGValue(clr);
	const int b = GetBValue(clr);
	const double lum = 0.299 * r + 0.587 * g + 0.114 * b;
	return (lum > 150.0) ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

// 自绘分发：色块按钮 / 扁平按钮 / 列表项
//   色块按钮 —— 以按钮自身矩形直接填当前配色 + 边框 + 居中名称，色块本身就是
//                "所见即所得"的预览，不必打开取色器也能看到当前配色
//   扁平按钮 —— 见 DrawFlatButton（现代扁平风格，含悬停/按下/禁用/焦点态）
//   列表项   —— 见 DrawListItem（选中项为圆角强调色药丸）
void CDeskTidyDlg::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct)
{
	if (lpDrawItemStruct == NULL)
		return;

	// WorkBuddy: 列表控件（LBS_OWNERDRAWFIXED）的每一个可见项
	if (lpDrawItemStruct->CtlType == ODT_LISTBOX &&
	    lpDrawItemStruct->CtlID == IDC_LIST_WIDGETS)
	{
		DrawListItem(lpDrawItemStruct);
		return;
	}

	// WorkBuddy: 四个扁平按钮（BS_OWNERDRAW）
	if (lpDrawItemStruct->CtlType == ODT_BUTTON && IsFlatButton(nIDCtl))
	{
		DrawFlatButton(lpDrawItemStruct);
		return;
	}

	if (nIDCtl == IDC_BTN_HEADER_COLOR || nIDCtl == IDC_BTN_BG_COLOR ||
	    nIDCtl == IDC_BTN_TEXT_COLOR)
	{
		const BOOL bHeader = (nIDCtl == IDC_BTN_HEADER_COLOR);
		const BOOL bText   = (nIDCtl == IDC_BTN_TEXT_COLOR);

		// WorkBuddy: 名称色块在"自动"态下不显示某个固定色，而是直接显示
		// 当前背景色算出来的实际结果色（黑或白）——色块本身就是"所见即所得"的
		// 预览，比画棋盘格更直观；配合标题里的"(自动)"后缀，状态一目了然
		const BOOL bTextAuto = (bText && m_bTextAutoSel != FALSE);
		const COLORREF clr = bText
		                     ? (bTextAuto ? DlgContrastTextColor(m_clrBgSel) : m_clrTextSel)
		                     : (bHeader ? m_clrHeaderSel : m_clrBgSel);

		LPCTSTR pszText = bText
		                  ? (bTextAuto ? _T("名称颜色(自动)") : _T("名称颜色"))
		                  : (bHeader ? _T("标题栏颜色") : _T("背景色"));

		CDC* pDC = CDC::FromHandle(lpDrawItemStruct->hDC);
		if (pDC == NULL)
			return;

		CRect rc(lpDrawItemStruct->rcItem);

		// 圆角填色 + 1px 描边：与扁平按钮使用同一套视觉语言。
		// 悬停时描边转为品牌蓝、按下时同样保持蓝框，给出清晰的交互反馈
		// （不再使用 Draw3dRect 的立体凹凸边框——那是 Win95 时代的视觉符号）
		const BOOL bPushed = (lpDrawItemStruct->itemState & ODS_SELECTED) ? TRUE : FALSE;
		const BOOL bHover  = (m_nHoverBtn == nIDCtl) ? TRUE : FALSE;
		{
			CPen penBorder(PS_SOLID, 1,
			               (bPushed || bHover) ? UI_CLR_ACCENT : UI_CLR_BTN_BORDER);
			CPen*   pOldPen   = pDC->SelectObject(&penBorder);
			CBrush  brush(clr);
			CBrush* pOldBrush = pDC->SelectObject(&brush);
			pDC->RoundRect(rc, CPoint(8, 8));
			pDC->SelectObject(pOldBrush);
			pDC->SelectObject(pOldPen);
		}

		// 文字颜色随底色亮度取黑/白，保证在任意底色上都可读。
		// 字体取对话框字体（Segoe UI），与相邻静态文本保持一致；
		// GetFont 理论上不会为空，这里仍做空指针保护，避免极端的
		// SelectObject(NULL) 断言/崩溃
		pDC->SetBkMode(TRANSPARENT);
		pDC->SetTextColor(DlgContrastTextColor(clr));
		CFont* pFont = GetFont();
		CFont* pOldFont = (pFont != NULL) ? pDC->SelectObject(pFont) : NULL;
		pDC->DrawText(pszText, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		if (pOldFont != NULL)
			pDC->SelectObject(pOldFont);

		// Tab 键切到本按钮时显示焦点虚框
		if (lpDrawItemStruct->itemState & ODS_FOCUS)
			pDC->DrawFocusRect(rc);
		return;
	}

	CDialogEx::OnDrawItem(nIDCtl, lpDrawItemStruct);
}

// ---------------------------------------------------------------------------
// WorkBuddy: 现代扁平界面绘制
// ---------------------------------------------------------------------------

// 该控件 ID 是否属于自绘扁平按钮集合
BOOL CDeskTidyDlg::IsFlatButton(int nID) const
{
	return (nID == IDC_BTN_ADD ||
	        nID == IDC_BTN_DEL ||
	        nID == IDC_BTN_APPLY ||
	        nID == IDC_BTN_COLOR_RESET) ? TRUE : FALSE;
}

// 把 g_rcCardDLU 中的对话框单位矩形换算为客户区像素矩形。
// MapDialogRect 是对话框专用换算 API（基于对话框当前字体），因此卡片位置会
// 自动跟随字体与 DPI 变化，与控件坐标先天对齐、不会错位
void CDeskTidyDlg::GetCardRects(CRect* pRects, int& nCount)
{
	nCount = (int)UI_CARD_COUNT;
	if (pRects == NULL)
		return;

	for (int i = 0; i < nCount; i++)
	{
		CRect rc(g_rcCardDLU[i]);
		MapDialogRect(&rc);      // DLU -> 像素（原地换算，结果相对客户区左上角）
		pRects[i] = rc;
	}
}

// 绘制一张卡片：白色圆角底 + 1px 柔和描边 + 左上角加粗分区标题。
// RoundRect 的圆角参数是椭圆的宽高（即圆角直径），取 12 -> 半径 6px，
// 接近现代设计规范中小圆角的尺度
void CDeskTidyDlg::DrawCard(CDC* pDC, const CRect& rcCard, LPCTSTR pszTitle)
{
	if (pDC == NULL || rcCard.IsRectEmpty() || m_brCard.GetSafeHandle() == NULL)
		return;

	CPen penBorder(PS_SOLID, 1, UI_CLR_CARD_BORDER);
	CPen*   pOldPen   = pDC->SelectObject(&penBorder);
	CBrush* pOldBrush = pDC->SelectObject(&m_brCard);
	pDC->RoundRect(rcCard, CPoint(12, 12));
	pDC->SelectObject(pOldBrush);
	pDC->SelectObject(pOldPen);

	// 分区标题：加粗深色，贴卡片左上角内缘
	if (pszTitle != NULL && *pszTitle != _T('\0'))
	{
		CFont* pOldFont = pDC->SelectObject(&m_fontSection);
		const int      nOldBkMode = pDC->SetBkMode(TRANSPARENT);
		const COLORREF clrOldText = pDC->SetTextColor(UI_CLR_TEXT);

		CRect rcTitle(rcCard);
		rcTitle.left  += 12;
		rcTitle.top   += 7;
		rcTitle.bottom = rcTitle.top + 18;
		pDC->DrawText(pszTitle, rcTitle,
		              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

		pDC->SetTextColor(clrOldText);
		pDC->SetBkMode(nOldBkMode);
		pDC->SelectObject(pOldFont);
	}
}

// 绘制扁平按钮：
//   主按钮（"应用"）—— 品牌蓝填充 + 白字，视觉焦点明确；
//   次按钮          —— 白底 + 浅灰描边 + 深色字，与卡片和谐共处；
//   悬停 / 按下 / 禁用各有独立配色（悬停态由 CFlatBtn 上报，存于 m_nHoverBtn）
void CDeskTidyDlg::DrawFlatButton(LPDRAWITEMSTRUCT lpDIS)
{
	CDC* pDC = CDC::FromHandle(lpDIS->hDC);
	if (pDC == NULL || m_brCard.GetSafeHandle() == NULL)
		return;

	const CRect rc(lpDIS->rcItem);
	if (rc.IsRectEmpty())
		return;

	const BOOL bPrimary  = (lpDIS->CtlID == IDC_BTN_APPLY) ? TRUE : FALSE;
	const BOOL bDisabled = (lpDIS->itemState & ODS_DISABLED) ? TRUE : FALSE;
	const BOOL bPressed  = (lpDIS->itemState & ODS_SELECTED) ? TRUE : FALSE;
	const BOOL bHover    = (m_nHoverBtn == (int)lpDIS->CtlID) ? TRUE : FALSE;

	COLORREF clrFill;
	COLORREF clrBorder;
	COLORREF clrText;
	if (bDisabled)
	{
		clrFill   = UI_CLR_BTN_DISABLED_BG;
		clrBorder = UI_CLR_BTN_DISABLED_BORDER;
		clrText   = UI_CLR_TEXT_DISABLED;
	}
	else if (bPrimary)
	{
		clrFill   = bPressed ? UI_CLR_ACCENT_DOWN
		                     : (bHover ? UI_CLR_ACCENT_HOVER : UI_CLR_ACCENT);
		clrBorder = clrFill;                    // 主按钮无独立描边，靠色块本身成形
		clrText   = RGB(255, 255, 255);
	}
	else
	{
		clrFill   = bPressed ? UI_CLR_BTN_DOWN
		                     : (bHover ? UI_CLR_BTN_HOVER : UI_CLR_CARD);
		clrBorder = UI_CLR_BTN_BORDER;
		clrText   = UI_CLR_TEXT;
	}

	// 圆角填充 + 描边（圆角直径 8 -> 半径 4px，按钮小巧精致）
	{
		CPen penBorder(PS_SOLID, 1, clrBorder);
		CPen*   pOldPen   = pDC->SelectObject(&penBorder);
		CBrush  brush(clrFill);
		CBrush* pOldBrush = pDC->SelectObject(&brush);
		pDC->RoundRect(rc, CPoint(8, 8));
		pDC->SelectObject(pOldBrush);
		pDC->SelectObject(pOldPen);
	}

	// 文字：取按钮自身标题。DrawText 默认把 "&x" 渲染成带下划线的助记符，
	// 因此这里刻意不加 DT_NOPREFIX，保留 Alt 快捷键提示
	TCHAR szText[128] = { 0 };
	::GetWindowText(lpDIS->hwndItem, szText, _countof(szText));

	pDC->SetBkMode(TRANSPARENT);
	pDC->SetTextColor(clrText);

	CFont* pFont    = GetFont();
	CFont* pOldFont = (pFont != NULL) ? pDC->SelectObject(pFont) : NULL;

	CRect rcText(rc);
	if (bPressed)
		rcText.OffsetRect(0, 1);                // 按下时文字下沉 1px，强化点击反馈
	pDC->DrawText(szText, rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

	if (pOldFont != NULL)
		pDC->SelectObject(pOldFont);

	// 键盘焦点：内缩虚线框，保证纯键盘操作也能看清焦点位置（可访问性）
	if ((lpDIS->itemState & ODS_FOCUS) && !bDisabled)
	{
		CRect rcFocus(rc);
		rcFocus.DeflateRect(3, 3);
		pDC->DrawFocusRect(rcFocus);
	}
}

// 绘制列表项：
//   选中项 —— 圆角强调色药丸 + 白字（替代系统默认的方块蓝底，柔和且现代）；
//   其余项 —— 卡片白底 + 近黑字；
//   禁用项 —— 浅灰字
void CDeskTidyDlg::DrawListItem(LPDRAWITEMSTRUCT lpDIS)
{
	CDC* pDC = CDC::FromHandle(lpDIS->hDC);
	if (pDC == NULL)
		return;

	const CRect rc(lpDIS->rcItem);

	// 列表底：白色（LBS_OWNERDRAWFIXED 下每一项的背景都要由本函数负责擦除）
	pDC->FillSolidRect(rc, UI_CLR_CARD);

	// itemID == -1 表示"控件为空"的绘制请求：只需背景，没有文字
	if ((int)lpDIS->itemID < 0)
		return;

	const BOOL bSelected = (lpDIS->itemState & ODS_SELECTED) ? TRUE : FALSE;
	const BOOL bDisabled = (lpDIS->itemState & ODS_DISABLED) ? TRUE : FALSE;

	CString strText;
	m_listWidgets.GetText((int)lpDIS->itemID, strText);

	CRect rcText(rc);
	rcText.left  += 10;
	rcText.right -= 8;

	if (bSelected)
	{
		// 药丸式选中底：整行内缩 2px、两端圆角，比系统默认方块高亮更柔和
		CRect rcSel(rc);
		rcSel.DeflateRect(2, 1);

		CPen penSel(PS_SOLID, 1, UI_CLR_ACCENT);
		CPen*   pOldPen   = pDC->SelectObject(&penSel);
		CBrush  brSel(UI_CLR_ACCENT);
		CBrush* pOldBrush = pDC->SelectObject(&brSel);
		pDC->RoundRect(rcSel, CPoint(8, 8));
		pDC->SelectObject(pOldBrush);
		pDC->SelectObject(pOldPen);

		rcText.left = rcSel.left + 8;           // 文字避开圆角
	}

	pDC->SetBkMode(TRANSPARENT);
	pDC->SetTextColor(bDisabled ? UI_CLR_TEXT_DISABLED
	                            : (bSelected ? RGB(255, 255, 255) : UI_CLR_TEXT));

	CFont* pFont    = GetFont();
	CFont* pOldFont = (pFont != NULL) ? pDC->SelectObject(pFont) : NULL;
	pDC->DrawText(strText, rcText,
	              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
	if (pOldFont != NULL)
		pDC->SelectObject(pOldFont);

	// 键盘焦点：仅在未选中时画虚线框（选中态已整行高亮，再加框反而杂乱）
	if ((lpDIS->itemState & ODS_FOCUS) && !bSelected)
	{
		CRect rcFocus(rc);
		rcFocus.DeflateRect(1, 1);
		pDC->DrawFocusRect(rcFocus);
	}
}

// ---------------------------------------------------------------------------
// WorkBuddy: 背景 / 控件配色 / 列表行高 / 按钮悬停
// ---------------------------------------------------------------------------

// 擦除并绘制对话框背景：整体浅灰底 + 两张白色圆角卡片。
// 注意：对话框带 WS_CLIPCHILDREN，本函数的 DC 已被裁剪掉子控件所占区域，
// 卡片被子控件覆盖的部分不会在这里绘制——但子控件自身在 OnCtlColor 中同样
// 拿到白色背景，因此视觉上仍是一整块连续的卡片，不会出现色差缝隙
BOOL CDeskTidyDlg::OnEraseBkgnd(CDC* pDC)
{
	if (pDC == NULL || GetSafeHwnd() == NULL)
		return CDialogEx::OnEraseBkgnd(pDC);

	CRect rcClient;
	GetClientRect(&rcClient);
	pDC->FillSolidRect(rcClient, UI_CLR_DLG_BG);

	CRect rcCards[UI_CARD_COUNT];
	int nCount = 0;
	GetCardRects(rcCards, nCount);

	// 卡片标题沿用原 GROUPBOX 的分组语义，保持用户既有的认知不变
	if (nCount > 0)
		DrawCard(pDC, rcCards[0], _T("选中窗口的属性"));
	if (nCount > 1)
		DrawCard(pDC, rcCards[1], _T("全局快捷键"));

	return TRUE;    // 已完整擦除，无需再走默认处理
}

// 控件配色：按"控件是否落在卡片内"返回白色（卡内）或浅灰（卡外）背景画刷，
// 使静态文本 / 单选 / 复选 / 滑块 / 列表 / 快捷键输入框都与卡片无缝融合。
// 文字统一为近黑，保证浅色主题下的长时间可读性
HBRUSH CDeskTidyDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
	HBRUSH hbr = CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);

	if (pDC == NULL || pWnd == NULL || pWnd->GetSafeHwnd() == NULL)
		return hbr;

	// 自绘按钮不接受 WM_CTLCOLOR（外观完全由 OnDrawItem 负责），直接放行
	if (IsFlatButton(pWnd->GetDlgCtrlID()))
		return hbr;

	// 判断控件是否落在卡片内：用控件窗口矩形与卡片矩形求交。
	// 这里用"位置"而不是"控件 ID"判断，是因为模板里大量静态文本共用
	// IDC_STATIC，无法按 ID 区分它在卡片内还是卡片外
	BOOL bInCard = FALSE;
	{
		CRect rcCards[UI_CARD_COUNT];
		int nCount = 0;
		GetCardRects(rcCards, nCount);

		CRect rcWnd;
		pWnd->GetWindowRect(&rcWnd);
		ScreenToClient(&rcWnd);

		for (int i = 0; i < nCount && !bInCard; i++)
		{
			CRect rcInter;
			if (rcInter.IntersectRect(rcWnd, rcCards[i]))
				bInCard = TRUE;
		}
	}

	switch (nCtlColor)
	{
	case CTLCOLOR_STATIC:       // 静态文本 / 单选 / 复选 / 滑块
	case CTLCOLOR_BTN:
	case CTLCOLOR_LISTBOX:
	case CTLCOLOR_EDIT:         // 快捷键输入框
	case CTLCOLOR_SCROLLBAR:
		pDC->SetBkMode(TRANSPARENT);
		pDC->SetTextColor(UI_CLR_TEXT);
		return (HBRUSH)(bInCard ? m_brCard.GetSafeHandle()
		                        : m_brDlgBg.GetSafeHandle());

	default:
		break;
	}

	return hbr;
}

// 列表行高：所有者绘制（LBS_OWNERDRAWFIXED）的固定行高由本函数给出。
// 按当前字体的实际高度 + 8px 内边距计算，从而自动适配不同 DPI 与字体，
// 也让列表在视觉上与卡片其他控件保持同一节奏
void CDeskTidyDlg::OnMeasureItem(int nIDCtl, LPMEASUREITEMSTRUCT lpMeasureItemStruct)
{
	if (nIDCtl == IDC_LIST_WIDGETS &&
	    lpMeasureItemStruct != NULL &&
	    lpMeasureItemStruct->CtlType == ODT_LISTBOX)
	{
		int nItemHeight = 0;

		CDC* pDC = GetDC();
		if (pDC != NULL)
		{
			CFont* pFont    = GetFont();
			CFont* pOldFont = (pFont != NULL) ? pDC->SelectObject(pFont) : NULL;

			TEXTMETRIC tm;
			ZeroMemory(&tm, sizeof(tm));
			if (pDC->GetTextMetrics(&tm))
				nItemHeight = tm.tmHeight + 8;      // 8px 呼吸感

			if (pOldFont != NULL)
				pDC->SelectObject(pOldFont);
			ReleaseDC(pDC);
		}

		if (nItemHeight < 18)
			nItemHeight = 18;                       // 兜底：度量失败时也要有可点击高度
		lpMeasureItemStruct->itemHeight = (UINT)nItemHeight;
		return;
	}

	CDialogEx::OnMeasureItem(nIDCtl, lpMeasureItemStruct);
}

// 自绘按钮悬停上报（来自 CFlatBtn）：更新悬停 ID 并立即重绘该按钮。
// 只重绘被悬停的那一个按钮，不做整窗刷新
LRESULT CDeskTidyDlg::OnFlatBtnHover(WPARAM wParam, LPARAM lParam)
{
	const int  nID    = (int)wParam;
	const BOOL bHover = (BOOL)lParam;

	if (bHover)
		m_nHoverBtn = nID;
	else if (m_nHoverBtn == nID)
		m_nHoverBtn = 0;

	CWnd* pBtn = GetDlgItem(nID);
	if (pBtn != NULL && pBtn->GetSafeHwnd() != NULL)
	{
		pBtn->Invalidate();
		pBtn->UpdateWindow();   // 立即重绘，悬停反馈不延迟
	}
	return 0;
}

// 把当前配色刷新到三个色块按钮（仅触发重绘，按钮标题文字不变）
void CDeskTidyDlg::UpdateColorButtons()
{
	if (GetSafeHwnd() == NULL)
		return;

	// 三个色块都是自绘按钮：Invalidate 会引发 WM_DRAWITEM，
	// 再 UpdateWindow 立即重画，取色后能马上看到新色块。
	// 名称色块在"自动"态下显示的是"按当前背景色算出的结果色"，
	// 因此背景色一改也必须一并刷新它（这里统一刷新，天然满足）
	static const UINT arrColorBtn[3] =
	{
		IDC_BTN_HEADER_COLOR, IDC_BTN_BG_COLOR, IDC_BTN_TEXT_COLOR
	};
	for (size_t i = 0; i < _countof(arrColorBtn); i++)
	{
		CWnd* pBtn = GetDlgItem(arrColorBtn[i]);
		if (pBtn != NULL && pBtn->GetSafeHwnd() != NULL)
		{
			pBtn->Invalidate();
			pBtn->UpdateWindow();
		}
	}
}

// WorkBuddy: 把 m_bTextAutoSel 同步到"自动跟随背景"复选框。
// 供 LoadSelProps（切换选中窗口）与"恢复默认"调用，保证界面状态与待应用值一致
void CDeskTidyDlg::UpdateTextAutoCheck()
{
	if (GetSafeHwnd() == NULL)
		return;

	CWnd* pChk = GetDlgItem(IDC_CHECK_TEXT_AUTO);
	if (pChk != NULL && pChk->GetSafeHwnd() != NULL)
		pChk->SendMessage(BM_SETCHECK,
		                  m_bTextAutoSel ? BST_CHECKED : BST_UNCHECKED, 0);
}

// ---------------------------------------------------------------------------
// WorkBuddy: 透明度换算与刷新（界面 0~100% ⇔ 配置 0~255）
// ---------------------------------------------------------------------------
// 透明度在界面以百分比（0~100%）展示、在小窗口中以 0~255 的 alpha 保存。
// 下面两个换算函数成对使用，并保证两个关键点精确互逆：
//   0%   ⇔ 0   （完全透明，可透出桌面）
//   100% ⇔ 255 （完全不透明）
// 其余取值四舍五入，误差不超过 1 个 alpha 级（人眼不可辨）

// alpha(0~255) → 百分比(0~100)
static int DlgAlphaToPercent(int nAlpha)
{
	if (nAlpha < 0)   nAlpha = 0;
	if (nAlpha > 255) nAlpha = 255;
	return (nAlpha * 100 + 127) / 255;
}

// 百分比(0~100) → alpha(0~255)
static int DlgPercentToAlpha(int nPercent)
{
	if (nPercent < 0)   nPercent = 0;
	if (nPercent > 100) nPercent = 100;
	return (nPercent * 255 + 50) / 100;
}

// 把百分比文字（形如 "100%"）写到指定 ID 的静态控件上
static void DlgSetPercentText(CWnd* pParent, int nID, int nPercent)
{
	if (pParent == NULL || pParent->GetSafeHwnd() == NULL)
		return;

	CWnd* pWnd = pParent->GetDlgItem(nID);
	if (pWnd == NULL || pWnd->GetSafeHwnd() == NULL)
		return;

	CString str;
	str.Format(_T("%d%%"), nPercent);
	pWnd->SetWindowText(str);
}

// 把待应用透明度刷新到两个滑块与百分比文字上。
// 与 UpdateColorButtons 同理：本函数只刷新界面，不写小窗口——真正写回由
// 【应用】按钮统一完成（与层级/视图/配色等属性的交互完全一致）
void CDeskTidyDlg::UpdateAlphaControls()
{
	if (GetSafeHwnd() == NULL)
		return;

	// 标题栏透明度
	if (m_sliderHeaderAlpha.GetSafeHwnd() != NULL)
	{
		const int nPercent = DlgAlphaToPercent(m_nHeaderAlphaSel);
		m_sliderHeaderAlpha.SetPos(nPercent);
		DlgSetPercentText(this, IDC_STATIC_HEADER_ALPHA_VAL, nPercent);
	}

	// 背景透明度
	if (m_sliderBgAlpha.GetSafeHwnd() != NULL)
	{
		const int nPercent = DlgAlphaToPercent(m_nBgAlphaSel);
		m_sliderBgAlpha.SetPos(nPercent);
		DlgSetPercentText(this, IDC_STATIC_BG_ALPHA_VAL, nPercent);
	}
}

// 透明度滑块消息：CSliderCtrl 拖动的过程/结果都通过 WM_HSCROLL 通知父窗口。
// 两个滑块共用一个处理函数，按发出通知的控件句柄区分来源，避免维护两套映射。
// 拖动时只更新"待应用值"（m_nHeaderAlphaSel / m_nBgAlphaSel）与百分比文字，
// 不实时改小窗口——与配色一致，点【应用】才生效（避免拖动中反复重新合成分层位图）
void CDeskTidyDlg::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pBar)
{
	if (pBar != NULL && pBar->GetSafeHwnd() != NULL)
	{
		const HWND hSlider = pBar->GetSafeHwnd();

		const BOOL bHeader = (hSlider == m_sliderHeaderAlpha.GetSafeHwnd());
		const BOOL bBg     = (hSlider == m_sliderBgAlpha.GetSafeHwnd());

		if (bHeader || bBg)
		{
			CSliderCtrl* pSlider = bHeader ? &m_sliderHeaderAlpha : &m_sliderBgAlpha;
			const int nPercent = pSlider->GetPos();     // 0~100

			// 百分比 → alpha 存回待应用值（0~255）
			if (bHeader)
				m_nHeaderAlphaSel = DlgPercentToAlpha(nPercent);
			else
				m_nBgAlphaSel = DlgPercentToAlpha(nPercent);

			// 同步刷新该滑块右侧的百分比文字；另一侧不动
			DlgSetPercentText(this,
			                  bHeader ? IDC_STATIC_HEADER_ALPHA_VAL : IDC_STATIC_BG_ALPHA_VAL,
			                  nPercent);
			return;     // 已处理，无需交给基类（基类处理的是滚动条，与此无关）
		}
	}

	CDialogEx::OnHScroll(nSBCode, nPos, pBar);
}

// 点击"标题栏颜色"色块：弹出系统取色器（初始选中当前色），确认后只更新
// 待应用值与色块预览，真正写入小窗口由【应用】按钮统一完成
void CDeskTidyDlg::OnBnClickedHeaderColor()
{
	// CC_FULLOPEN = 直接展开"自定义颜色"区；CC_ANYCOLOR = 允许任意颜色
	CColorDialog dlg(m_clrHeaderSel, CC_FULLOPEN | CC_ANYCOLOR, this);
	if (dlg.DoModal() != IDOK)
		return;     // 用户取消：保持原配色

	m_clrHeaderSel = dlg.GetColor();
	UpdateColorButtons();
}

// 点击"背景色"色块：同上，作用于背景色
void CDeskTidyDlg::OnBnClickedBgColor()
{
	CColorDialog dlg(m_clrBgSel, CC_FULLOPEN | CC_ANYCOLOR, this);
	if (dlg.DoModal() != IDOK)
		return;

	m_clrBgSel = dlg.GetColor();
	UpdateColorButtons();
}

// WorkBuddy: 点击"名称颜色"色块：弹出取色器选一个固定颜色。
// 选定后自动取消"自动跟随背景"——用户的动作本身就表达了"我要指定颜色"，
// 不必再让他手动去掉那个勾（这与大多数设计工具的行为一致）
void CDeskTidyDlg::OnBnClickedTextColor()
{
	// 取色器初值：自定义态用已选颜色；自动态用"当前背景算出的结果色"，
	// 打开时看到的就是眼下实际生效的颜色，便于在其基础上微调
	const COLORREF clrInit = m_bTextAutoSel ? DlgContrastTextColor(m_clrBgSel)
	                                        : m_clrTextSel;
	CColorDialog dlg(clrInit, CC_FULLOPEN | CC_ANYCOLOR, this);
	if (dlg.DoModal() != IDOK)
		return;     // 用户取消：保持原配色与原自动态

	m_clrTextSel   = dlg.GetColor();
	m_bTextAutoSel = FALSE;     // 选了具体颜色 → 退出自动态
	UpdateColorButtons();
	UpdateTextAutoCheck();
}

// WorkBuddy: 勾选/取消"自动跟随背景"。
// 复选框是 BS_AUTOCHECKBOX，点击时其勾选状态已由系统切换，这里只需读回、
// 存进待应用值并刷新色块预览（色块在自动态显示结果色，取消则显示自定义色）
void CDeskTidyDlg::OnBnClickedTextAuto()
{
	m_bTextAutoSel = (IsDlgButtonChecked(IDC_CHECK_TEXT_AUTO) == BST_CHECKED);
	UpdateColorButtons();
}

// 点击"恢复默认"：标题栏颜色、背景色、文件名称颜色与两者的透明度一起复位为默认外观
// （只改待应用值，仍需点【应用】才生效，与其它属性一致）
void CDeskTidyDlg::OnBnClickedColorReset()
{
	m_clrHeaderSel = WIDGET_DEFAULT_HEADER_COLOR;
	m_clrBgSel     = WIDGET_DEFAULT_BG_COLOR;
	// WorkBuddy: 名称颜色复位为"自动跟随背景"，并保留一份默认自定义色作为
	// 用户取消勾选时的起点（与构造函数的初始状态一致）
	m_clrTextSel   = WIDGET_DEFAULT_TEXT_COLOR;
	m_bTextAutoSel = TRUE;
	// WorkBuddy: 透明度一并复位为 100%（255 = 完全不透明）
	m_nHeaderAlphaSel = 255;
	m_nBgAlphaSel     = 255;
	UpdateColorButtons();
	UpdateTextAutoCheck();
	UpdateAlphaControls();
}

// ---------------------------------------------------------------------------
// 开机自启动（注册表 HKCU\...\CurrentVersion\Run 键）
// ---------------------------------------------------------------------------
// 自启动必须写入注册表才能生效，因此该项不存入 INI，
// 而是直接读写注册表 Run 键，注册表即是唯一的事实来源。

// 查询当前是否已设置开机自启动：Run 键下存在字符串值"DeskTidy"即为已启用
BOOL CDeskTidyDlg::QueryAutoStart()
{
	HKEY hKey = NULL;
	if (::RegOpenKeyEx(HKEY_CURRENT_USER,
	                   _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
	                   0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
		return FALSE;

	DWORD dwType = 0;
	TCHAR szPath[MAX_PATH] = {};
	DWORD dwSize = sizeof(szPath);
	LONG lRet = ::RegQueryValueEx(hKey, _T("DeskTidy"), NULL, &dwType,
	                              (LPBYTE)szPath, &dwSize);
	::RegCloseKey(hKey);

	// 值存在且为字符串类型（REG_SZ / REG_EXPAND_SZ）即视为已启用
	return (lRet == ERROR_SUCCESS && (dwType == REG_SZ || dwType == REG_EXPAND_SZ));
}

// 设置或取消开机自启动：启用时写入 exe 完整路径，禁用时删除键值。
// 路径加引号，兼容 exe 所在路径含空格的情况。
void CDeskTidyDlg::SetAutoStart(BOOL bEnable)
{
	HKEY hKey = NULL;
	if (::RegCreateKeyEx(HKEY_CURRENT_USER,
	                     _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
	                     0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
	                     NULL, &hKey, NULL) != ERROR_SUCCESS)
		return;

	if (bEnable)
	{
		// 取 exe 完整路径并加引号写入
		TCHAR szExe[MAX_PATH] = {};
		::GetModuleFileName(NULL, szExe, MAX_PATH);
		CString strCmd = _T("\"");
		strCmd += szExe;
		strCmd += _T("\"");
		::RegSetValueEx(hKey, _T("DeskTidy"), 0, REG_SZ,
		                (const BYTE*)(LPCTSTR)strCmd,
		                (DWORD)((strCmd.GetLength() + 1) * sizeof(TCHAR)));
	}
	else
	{
		// 禁用：删除 Run 键值（值不存在时 RegDeleteValue 返回 ERROR_FILE_NOT_FOUND，可忽略）
		::RegDeleteValue(hKey, _T("DeskTidy"));
	}
	::RegCloseKey(hKey);
}

// ---------------------------------------------------------------------------
// 配置持久化（exe 所在目录的 DeskTidy.ini）
// ---------------------------------------------------------------------------

// 返回 exe 所在目录下的 DeskTidy.ini 完整路径
CString CDeskTidyDlg::GetIniPath()
{
	TCHAR szPath[MAX_PATH] = {};
	::GetModuleFileName(NULL, szPath, MAX_PATH);

	// 去掉文件名，保留目录部分
	CString strExe = szPath;
	int nSlash = strExe.ReverseFind(_T('\\'));
	if (nSlash >= 0)
		strExe = strExe.Left(nSlash + 1);

	return strExe + _T("DeskTidy.ini");
}

// 从 INI 读取配置并创建所有小窗口。
// INI 布局：
//   [Widgets]
//   Count=2
//   [Widget1]  Dir=... Left=... Top=... Right=... Bottom=...
//              LayerMode=0 ViewMode=0 Collapsed=0 Zoom=100
//   [Widget2]  ...
//   [Settings] TopHotKey=0 BottomHotKey=0 ZoomInHotKey=0 ZoomOutHotKey=0
//              （全局快捷键，0 = 未指定）
void CDeskTidyDlg::LoadConfig()
{
	m_strIniPath = GetIniPath();

	// 读取全局快捷键配置（0 = 未指定；其余值为"修饰键<<16 | 虚拟键码"）
	m_nTopHotKey    = GetPrivateProfileInt(_T("Settings"), _T("TopHotKey"), 0, m_strIniPath);
	m_nBottomHotKey = GetPrivateProfileInt(_T("Settings"), _T("BottomHotKey"), 0, m_strIniPath);
	m_nZoomInHotKey  = GetPrivateProfileInt(_T("Settings"), _T("ZoomInHotKey"), 0, m_strIniPath);
	m_nZoomOutHotKey = GetPrivateProfileInt(_T("Settings"), _T("ZoomOutHotKey"), 0, m_strIniPath);

	// 读取小窗口数量
	int nCount = GetPrivateProfileInt(_T("Widgets"), _T("Count"), 0, m_strIniPath);

	for (int i = 1; i <= nCount && i <= MAX_WIDGETS; i++)
	{
		TCHAR szSection[16];
		wsprintf(szSection, _T("Widget%d"), i);

		// 读取目录；目录不存在则跳过（避免创建无内容的空窗口）
		TCHAR szDir[MAX_PATH] = {};
		GetPrivateProfileString(szSection, _T("Dir"), _T(""), szDir, MAX_PATH, m_strIniPath);
		if (szDir[0] == 0)
			continue;
		DWORD dwAttr = ::GetFileAttributes(szDir);
		if (dwAttr == INVALID_FILE_ATTRIBUTES || !(dwAttr & FILE_ATTRIBUTE_DIRECTORY))
			continue;

		// 读取属性
		int nLayer = GetPrivateProfileInt(szSection, _T("LayerMode"), 0, m_strIniPath);
		int nView = GetPrivateProfileInt(szSection, _T("ViewMode"), 0, m_strIniPath);
		int nCollapsed = GetPrivateProfileInt(szSection, _T("Collapsed"), 0, m_strIniPath);
		// 读取缩放级别（Ctrl+滚轮缩放的图标/文字大小比例，默认 100%）
		int nZoom = GetPrivateProfileInt(szSection, _T("Zoom"), 100, m_strIniPath);
		// 读取标题栏颜色与背景色（COLORREF 以十进制整数读写，默认深蓝/浅灰）
		int nHeaderColor = GetPrivateProfileInt(szSection, _T("HeaderColor"),
		                                        (int)WIDGET_DEFAULT_HEADER_COLOR, m_strIniPath);
		int nBgColor = GetPrivateProfileInt(szSection, _T("BgColor"),
		                                    (int)WIDGET_DEFAULT_BG_COLOR, m_strIniPath);
		// WorkBuddy: 读取文件名称颜色（文件列表项文字）的配置。
		//   默认 TextAuto=1（自动跟随背景）——与改造前"按背景亮度自动黑白"的
		//   行为一致，老配置（没有这两个键）升级后观感不变
		int nTextColor = GetPrivateProfileInt(szSection, _T("TextColor"),
		                                      (int)WIDGET_DEFAULT_TEXT_COLOR, m_strIniPath);
		int nTextAuto  = GetPrivateProfileInt(szSection, _T("TextAuto"), 1, m_strIniPath);
		// WorkBuddy: 读取标题栏/背景透明度（0~255，默认 255=不透明；两者独立）
		int nHeaderAlpha = GetPrivateProfileInt(szSection, _T("HeaderAlpha"), 255, m_strIniPath);
		int nBgAlpha     = GetPrivateProfileInt(szSection, _T("BgAlpha"),     255, m_strIniPath);
		if (nHeaderAlpha < 0)   nHeaderAlpha = 0;   if (nHeaderAlpha > 255) nHeaderAlpha = 255;
		if (nBgAlpha < 0)       nBgAlpha = 0;       if (nBgAlpha > 255)     nBgAlpha = 255;
		// WorkBuddy: 读取 z 序（层级）维护开关（1 = 开启）。
		// 默认 1：老配置没有这个键时保持历史行为（一直都在维护）
		int nZOrderMaintain = GetPrivateProfileInt(szSection, _T("ZOrderMaintain"), 1, m_strIniPath);

		// 读取位置与大小（未记录时为 0，随后使用默认矩形）
		CRect rc;
		rc.left   = GetPrivateProfileInt(szSection, _T("Left"), 0, m_strIniPath);
		rc.top    = GetPrivateProfileInt(szSection, _T("Top"), 0, m_strIniPath);
		rc.right  = GetPrivateProfileInt(szSection, _T("Right"), 0, m_strIniPath);
		rc.bottom = GetPrivateProfileInt(szSection, _T("Bottom"), 0, m_strIniPath);
		if (rc.Width() <= 0 || rc.Height() <= 0)
			rc = GetDefaultRect();

		// WorkBuddy: 修复"折叠启动→展开不恢复"（2026-09-13）——INI 矩形高度
		// 若只有标题条尺寸（历史 bug 把 24px 折叠条存成了窗口矩形），补齐默认
		// 展开高度，让已被污染的配置重启即恢复正常大小（宽高分离：仅补高度）
		if (rc.Height() <= WIDGET_HEADER_HEIGHT)
			rc.bottom = rc.top + WIDGET_DEFAULT_EXPAND_HEIGHT;

		// 创建小窗口（先设置属性再创建，CreateWidget 内部会应用），
		// 创建后再按配置应用折叠状态（窗口缩到只剩标题栏）
		CDeskTidyWidget* pWnd = new CDeskTidyWidget;
		// WorkBuddy: 先读取"桌面子窗口置底"开关再创建——CreateWidget 内部
		// 会按层级模式应用 SetBottomLayer，必须在创建前落地，否则会先用
		// 默认（关闭）走旧方案
		int nBottomViaChild = GetPrivateProfileInt(szSection, _T("BottomViaChild"), 0, m_strIniPath);
		pWnd->SetBottomViaChild(nBottomViaChild != 0);
		pWnd->SetLayerMode(nLayer);
		pWnd->SetViewMode(nView);
		pWnd->SetZoom(nZoom);       // 记录缩放级别，OnCreate 创建后按此值应用
		pWnd->SetHeaderColor((COLORREF)nHeaderColor); // 记录标题栏色，创建后渲染时采用
		pWnd->SetBgColor((COLORREF)nBgColor);         // 记录背景色，创建后渲染时采用
		pWnd->SetHeaderAlpha((BYTE)nHeaderAlpha);     // WorkBuddy: 记录标题栏透明度
		pWnd->SetBgAlpha((BYTE)nBgAlpha);             // WorkBuddy: 记录背景透明度
		// WorkBuddy: 记录文件名称颜色（自定义色 + 是否自动跟随背景）
		pWnd->SetTextColor((COLORREF)nTextColor, nTextAuto != 0);
		// WorkBuddy: 记录 z 序维护开关。此时窗口尚未创建，SetZOrderMaintain
		// 只记录取值；真正的定时器启停由 OnCreate 按该值决定
		pWnd->SetZOrderMaintain(nZOrderMaintain != 0);
		if (pWnd->CreateWidget(rc, szDir, m_hWnd))
		{
			pWnd->SetCollapsed(nCollapsed != 0);
			m_arrWidgets.Add(pWnd);
		}
		else
			delete pWnd;
	}

	// 从未配置过任何小窗口时，默认创建一个显示系统桌面目录的小窗口
	if (m_arrWidgets.GetCount() == 0)
	{
		TCHAR szDesktop[MAX_PATH] = {};
		SHGetFolderPath(NULL, CSIDL_DESKTOP, NULL, SHGFP_TYPE_CURRENT, szDesktop);

		CDeskTidyWidget* pWnd = new CDeskTidyWidget;
		if (pWnd->CreateWidget(GetDefaultRect(), szDesktop, m_hWnd))
			m_arrWidgets.Add(pWnd);
		else
			delete pWnd;
	}
}

// 把当前全部小窗口的配置写入 INI（目录、位置、大小、层级、视图）
void CDeskTidyDlg::SaveConfig()
{
	if (m_strIniPath.IsEmpty())
		m_strIniPath = GetIniPath();

	TCHAR szBuf[64];

	// 写入小窗口数量
	int nCount = (int)m_arrWidgets.GetCount();
	wsprintf(szBuf, _T("%d"), nCount);
	WritePrivateProfileString(_T("Widgets"), _T("Count"), szBuf, m_strIniPath);

	for (int i = 0; i < nCount; i++)
	{
		TCHAR szSection[16];
		wsprintf(szSection, _T("Widget%d"), i + 1);

		CDeskTidyWidget* pWnd = m_arrWidgets[i];

		// 目录
		WritePrivateProfileString(szSection, _T("Dir"), pWnd->GetDirectory(), m_strIniPath);

		// 位置与大小（读取窗口最近一次矩形，窗口存活与否均可）
		CRect rc = pWnd->GetLastRect();
		wsprintf(szBuf, _T("%d"), rc.left);
		WritePrivateProfileString(szSection, _T("Left"), szBuf, m_strIniPath);
		wsprintf(szBuf, _T("%d"), rc.top);
		WritePrivateProfileString(szSection, _T("Top"), szBuf, m_strIniPath);
		wsprintf(szBuf, _T("%d"), rc.right);
		WritePrivateProfileString(szSection, _T("Right"), szBuf, m_strIniPath);
		wsprintf(szBuf, _T("%d"), rc.bottom);
		WritePrivateProfileString(szSection, _T("Bottom"), szBuf, m_strIniPath);

		// 层级模式与视图模式（读取小窗口实际状态，含右键菜单切换的结果）
		wsprintf(szBuf, _T("%d"), pWnd->GetLayerMode());
		WritePrivateProfileString(szSection, _T("LayerMode"), szBuf, m_strIniPath);
		wsprintf(szBuf, _T("%d"), pWnd->GetViewMode());
		WritePrivateProfileString(szSection, _T("ViewMode"), szBuf, m_strIniPath);

		// 折叠状态（折叠时只显示标题栏；下次启动时恢复同样状态）
		wsprintf(szBuf, _T("%d"), pWnd->IsCollapsed() ? 1 : 0);
		WritePrivateProfileString(szSection, _T("Collapsed"), szBuf, m_strIniPath);

		// 缩放级别（Ctrl+滚轮缩放：图标/文字大小比例，默认 100%；
		// 重启后按此值恢复列表的图标与文字大小）
		wsprintf(szBuf, _T("%d"), pWnd->GetZoom());
		WritePrivateProfileString(szSection, _T("Zoom"), szBuf, m_strIniPath);

		// 标题栏颜色与背景色（COLORREF 以十进制整数写入，重启后按此恢复）
		wsprintf(szBuf, _T("%u"), (UINT)pWnd->GetHeaderColor());
		WritePrivateProfileString(szSection, _T("HeaderColor"), szBuf, m_strIniPath);
		wsprintf(szBuf, _T("%u"), (UINT)pWnd->GetBgColor());
		WritePrivateProfileString(szSection, _T("BgColor"), szBuf, m_strIniPath);

		// WorkBuddy: 标题栏/背景透明度（0~255，独立保存，重启后按此恢复）
		wsprintf(szBuf, _T("%u"), (UINT)pWnd->GetHeaderAlpha());
		WritePrivateProfileString(szSection, _T("HeaderAlpha"), szBuf, m_strIniPath);
		wsprintf(szBuf, _T("%u"), (UINT)pWnd->GetBgAlpha());
		WritePrivateProfileString(szSection, _T("BgAlpha"), szBuf, m_strIniPath);

		// WorkBuddy: 文件名称颜色（文件列表项文字）。拆成两个键保存：
		//   TextAuto  = 1 表示"自动跟随背景"（默认），0 表示使用 TextColor；
		//   TextColor = 自定义颜色（COLORREF 十进制），自动模式下也照常保存，
		//               这样下次切回自定义时能接着用上次选好的颜色
		wsprintf(szBuf, _T("%d"), pWnd->IsTextColorAuto() ? 1 : 0);
		WritePrivateProfileString(szSection, _T("TextAuto"), szBuf, m_strIniPath);
		wsprintf(szBuf, _T("%u"), (UINT)pWnd->GetTextColor());
		WritePrivateProfileString(szSection, _T("TextColor"), szBuf, m_strIniPath);

		// WorkBuddy: z 序（层级）维护开关（1 = 开启，0 = 关闭）。
		// 值一律从小窗口读——它是唯一事实来源（右键菜单/设置界面都改它）
		wsprintf(szBuf, _T("%d"), pWnd->GetZOrderMaintain() ? 1 : 0);
		WritePrivateProfileString(szSection, _T("ZOrderMaintain"), szBuf, m_strIniPath);
		// WorkBuddy: 写入"桌面子窗口置底"开关
		wsprintf(szBuf, _T("%d"), pWnd->GetBottomViaChild() ? 1 : 0);
		WritePrivateProfileString(szSection, _T("BottomViaChild"), szBuf, m_strIniPath);
	}

	// 写入全局快捷键配置（0 = 未指定；其余值为"修饰键<<16 | 虚拟键码"）
	wsprintf(szBuf, _T("%d"), m_nTopHotKey);
	WritePrivateProfileString(_T("Settings"), _T("TopHotKey"), szBuf, m_strIniPath);
	wsprintf(szBuf, _T("%d"), m_nBottomHotKey);
	WritePrivateProfileString(_T("Settings"), _T("BottomHotKey"), szBuf, m_strIniPath);
	wsprintf(szBuf, _T("%d"), m_nZoomInHotKey);
	WritePrivateProfileString(_T("Settings"), _T("ZoomInHotKey"), szBuf, m_strIniPath);
	wsprintf(szBuf, _T("%d"), m_nZoomOutHotKey);
	WritePrivateProfileString(_T("Settings"), _T("ZoomOutHotKey"), szBuf, m_strIniPath);
}

// ---------------------------------------------------------------------------
// 小窗口管理辅助
// ---------------------------------------------------------------------------

// 销毁并释放全部小窗口对象
void CDeskTidyDlg::DestroyAllWidgets()
{
	for (int i = 0; i < (int)m_arrWidgets.GetCount(); i++)
	{
		CDeskTidyWidget* pWnd = m_arrWidgets[i];
		if (pWnd->GetSafeHwnd() != NULL)
			pWnd->DestroyWindow();
		delete pWnd;
	}
	m_arrWidgets.RemoveAll();
	m_nSelIndex = -1;
}

// 生成一个合理的默认矩形：主屏工作区右下角，320x240
CRect CDeskTidyDlg::GetDefaultRect()
{
	CRect rcWork;
	::SystemParametersInfo(SPI_GETWORKAREA, 0, &rcWork, 0);

	CRect rc(0, 0, 320, 240);
	rc.MoveToXY(rcWork.right - rc.Width() - 16, rcWork.bottom - rc.Height() - 16);
	return rc;
}

// 把列表选中项对应的属性加载到单选按钮上
void CDeskTidyDlg::LoadSelProps()
{
	m_nSelIndex = m_listWidgets.GetCurSel();
	if (m_nSelIndex == LB_ERR || m_nSelIndex >= (int)m_arrWidgets.GetCount())
	{
		m_nSelIndex = -1;
		// WorkBuddy: 无选中项时色块回到默认配色，避免残留上一个窗口的颜色
		m_clrHeaderSel = WIDGET_DEFAULT_HEADER_COLOR;
		m_clrBgSel     = WIDGET_DEFAULT_BG_COLOR;
		// 名称颜色同样回到默认：自动跟随背景
		m_clrTextSel   = WIDGET_DEFAULT_TEXT_COLOR;
		m_bTextAutoSel = TRUE;
		// 透明度同样回到默认（100% = 255，完全不透明）
		m_nHeaderAlphaSel = 255;
		m_nBgAlphaSel     = 255;
		// z 序维护开关回到默认（开启）
		m_bZOrderMaintain = TRUE;
		UpdateColorButtons();
		UpdateTextAutoCheck();
		UpdateAlphaControls();
		return;
	}

	CDeskTidyWidget* pWnd = m_arrWidgets[m_nSelIndex];
	m_nLayerMode = pWnd->GetLayerMode();
	m_nViewMode = pWnd->GetViewMode();
	// WorkBuddy: 同步读出该窗口的标题栏颜色与背景色，显示到两个色块按钮上
	m_clrHeaderSel = pWnd->GetHeaderColor();
	m_clrBgSel     = pWnd->GetBgColor();
	// WorkBuddy: 同步读出该窗口的"文件名称颜色"配置（自定义色 + 是否自动）。
	// 两种状态是分开存的，因此这里直接一一对应地回显，不做任何猜测或折算
	m_clrTextSel   = pWnd->GetTextColor();
	m_bTextAutoSel = pWnd->IsTextColorAuto();
	// WorkBuddy: 同步读出该窗口的两项透明度（0~255），显示到两个滑块上
	m_nHeaderAlphaSel = (int)pWnd->GetHeaderAlpha();
	m_nBgAlphaSel     = (int)pWnd->GetBgAlpha();
	// WorkBuddy: 同步读出 z 序维护开关
	m_bZOrderMaintain = pWnd->GetZOrderMaintain();
	// WorkBuddy: 同步读出"桌面子窗口置底"开关
	m_bBottomViaChild = pWnd->GetBottomViaChild();
	UpdateData(FALSE);      // 把属性显示到单选按钮与"自动"复选框
	UpdateColorButtons();   // 把配色显示到色块按钮
	UpdateAlphaControls();  // 把透明度显示到滑块与百分比文字
}

// ---------------------------------------------------------------------------
// 置底维护（SetWinEventHook 事件驱动）
// ---------------------------------------------------------------------------

// WinEvent 事件回调（静态函数，SetWinEventHook 要求回调必须是静态的）。
// 职责：只做轻量工作——重启防抖定时器，把所有"密集事件合并成一次压底"。
// 真正的压底操作（PushBottomWidgets）由定时器到期后在主对话框消息循环中执行，
// 避免在回调上下文里做耗时操作（SetWindowPos 等）造成系统卡顿或闪烁。
//
// 参数说明：
//   hWinEventHook  触发本回调的事件钩子句柄（本函数未使用，仅满足回调签名）
//   event          事件类型（EVENT_SYSTEM_FOREGROUND / EVENT_OBJECT_SHOW 等）
//   hwnd           事件对应的窗口句柄
//   idObject       "对象级事件"（如控件）时为控件所属的窗口句柄所在对象标识
//   idChild        子对象标识（CHILDID_SELF = 0 表示事件针对窗口/对象本身）
//   dwEventThread  触发事件的线程 ID
//   dwmsEventTime  事件发生时间（系统启动以来的毫秒数）
void CALLBACK CDeskTidyDlg::WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event,
										  HWND hwnd, LONG idObject, LONG idChild,
										  DWORD dwEventThread, DWORD dwmsEventTime)
{
	// 只处理"窗口级"事件：idObject == OBJID_WINDOW 且 idChild == CHILDID_SELF。
	// 控件级事件（如进度条/列表项显示，idObject == OBJID_CLIENT）与 z 序无关，
	// 直接过滤掉，避免无效触发白白启动防抖定时器
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
		return;

	// 过滤本进程自己窗口产生的事件（小窗口/主对话框的显示、切换等）：
	// 置底维护只关心"其它程序"的窗口弹出/切换造成的 z 序变化，
	// 自己的窗口事件不参与，避免自我触发形成事件->压底->再触发的循环
	DWORD dwPid = 0;
	::GetWindowThreadProcessId(hwnd, &dwPid);
	if (dwPid == GetCurrentProcessId())
		return;

	// 主对话框已销毁或句柄失效：直接返回，防止访问悬空指针
	if (s_pSelf == NULL || !::IsWindow(s_pSelf->m_hWnd))
		return;

	// 重启防抖定时器：短时间内密集触发的事件（例如连续切换窗口）会被合并，
	// 只留下最后一次到期后统一压底，避免对每个事件都执行 SetWindowPos 造成闪烁
	s_pSelf->KillTimer(TIMER_ID_BOTTOM_PUSH);
	s_pSelf->SetTimer(TIMER_ID_BOTTOM_PUSH, BOTTOM_PUSH_DELAY, NULL);
}

// 注册 WinEvent 事件钩子，事件驱动地维护"最底层"模式小窗口的 z 序。
// 监听两类可能导致 z 序变化的事件：
//   EVENT_SYSTEM_FOREGROUND  前台窗口切换（用户点开/切换到其它程序窗口时触发）
//   EVENT_OBJECT_SHOW        窗口显示（新窗口弹出、窗口取消隐藏时触发）
// 任一事件发生后，经防抖定时器统一把所有"最底层"小窗口重新压回 z 序最底。
//
// 注意：dwProcessId/dwThreadId 必须传 0（系统范围）。若传 GetCurrentProcessId()，
// 则只会在"本进程窗口成为前台"时收到事件——其它程序弹窗覆盖置底小窗口时
// 根本不会触发，压底机制就失效了。WINEVENT_OUTOFCONTEXT 表示回调在注册进程
// （即本对话框所在的 DoModal 消息循环）内执行，因此回调里操作定时器是安全的
void CDeskTidyDlg::RegisterWinEventHooks()
{
	// 已注册则先反注册，避免重复注册产生多个钩子
	UnregisterWinEventHooks();

	// 监听系统范围内所有前台窗口切换事件
	m_hHookForeground = ::SetWinEventHook(
		EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, // 事件范围：仅前台切换事件
		NULL,                                             // 回调所在模块：NULL 表示使用当前模块
		WinEventProc,                                     // 回调函数（静态）
		0, 0,                                             // 0 = 系统范围（所有进程/线程的事件）
		WINEVENT_OUTOFCONTEXT);                           // 回调由本进程消息循环执行

	// 监听系统范围内所有窗口显示事件
	m_hHookShow = ::SetWinEventHook(
		EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,             // 事件范围：仅窗口显示事件
		NULL,
		WinEventProc,
		0, 0,
		WINEVENT_OUTOFCONTEXT);

	// 监听系统范围内所有 z 序重排事件：
	// Windows 10/11 的"显示桌面"（Win+D / 任务栏"显示桌面"按钮）不（总是）
	// 最小化窗口，而是把桌面根窗口 Progman/WorkerW 抬到 z 序顶部——该动作
	// 会产生 z 序重排事件。监听它让"显示桌面"状态变化能第一时间触发
	// 小窗口的层级维护（提到桌面之上 / 压回最底）
	m_hHookReorder = ::SetWinEventHook(
		EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER,       // 事件范围：仅 z 序重排事件
		NULL,
		WinEventProc,
		0, 0,
		WINEVENT_OUTOFCONTEXT);
}

// 反注册 WinEvent 事件钩子。钩子句柄为 NULL（未注册）时无副作用，
// 因此可在 RegisterWinEventHooks 开头与 OnDestroy 中安全重复调用
void CDeskTidyDlg::UnregisterWinEventHooks()
{
	if (m_hHookForeground != NULL)
	{
		::UnhookWinEvent(m_hHookForeground);
		m_hHookForeground = NULL;
	}
	if (m_hHookShow != NULL)
	{
		::UnhookWinEvent(m_hHookShow);
		m_hHookShow = NULL;
	}
	if (m_hHookReorder != NULL)
	{
		::UnhookWinEvent(m_hHookReorder);
		m_hHookReorder = NULL;
	}
}

// 定时器消息处理：防抖延时到期后统一执行压底操作
void CDeskTidyDlg::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == TIMER_ID_BOTTOM_PUSH)
	{
		// 先停掉定时器再压底，防止压底过程中新事件又重启定时器导致重复执行
		KillTimer(TIMER_ID_BOTTOM_PUSH);
		PushBottomWidgets();
		return;   // 已处理完，不再交给基类
	}

	if (nIDEvent == TIMER_ID_WIDGET_CHECK)
	{
		CheckWidgetsAlive();   // 巡检并重建被 Explorer 重启销毁的挂接小窗口
		return;
	}
	CDialogEx::OnTimer(nIDEvent);
}

// 小窗口存活巡检：重建被 Explorer 重启销毁的挂接小窗口。
// 置底小窗口挂接为桌面（WorkerW/Progman）的子窗口，Explorer 重启时桌面
// 窗口被销毁，子窗口随之销毁（MFC 把该对象的 m_hWnd 置为 NULL，对象本身
// 保留在 m_arrWidgets 中）；这里调用 RecreateWidget 按对象保留的配置重新
// 创建并重新挂接。巡检开销极小（每个窗口仅一次 GetSafeHwnd 判断），
// 3 秒一次可忽略。
void CDeskTidyDlg::CheckWidgetsAlive()
{
	for (int i = 0; i < (int)m_arrWidgets.GetCount(); i++)
	{
		CDeskTidyWidget* pWnd = m_arrWidgets[i];
		if (pWnd->GetSafeHwnd() != NULL)
			continue;   // 窗口仍存活，无需处理

		pWnd->RecreateWidget();   // 按保留的配置重建并重新挂接桌面
	}
}

// 维护所有"最底层"模式且可见的小窗口的层级。
// 由防抖定时器到期时调用（WinEvent 事件仅负责启动定时器）。
// 逐个调用小窗口的 MaintainZOrder：该函数会先检测"显示桌面"模式
// （桌面被系统抬到 z 序顶部）——是则把小窗口提到桌面之上，
// 否则压回 z 序最底（内部有"已最底/完全可见"短路，避免闪烁）
void CDeskTidyDlg::PushBottomWidgets()
{
	// 频率抑制：距上次压底不足最小间隔时直接跳过。
	// 系统本身会持续产生前台/显示事件（输入法、任务栏、通知等），
	// 防抖只能合并"短时突发"，无法阻止事件静默期反复触发压底；
	// 而置底小窗口压到底后，只有被主动抬高才会离开最底，因此压底操作
	// 无需高频重复——限制最小间隔即可杜绝持续闪烁
	DWORD dwNow = ::GetTickCount();
	if (m_dwLastBottomPush != 0 &&
		(DWORD)(dwNow - m_dwLastBottomPush) < BOTTOM_PUSH_MIN_INTERVAL)
		return;
	m_dwLastBottomPush = dwNow;

	for (int i = 0; i < (int)m_arrWidgets.GetCount(); i++)
	{
		CDeskTidyWidget* pWnd = m_arrWidgets[i];
		// 仅处理：最底层模式（GetLayerMode() == 0）且窗口有效且可见的小窗口；
		// 悬浮置顶模式的小窗口不需要维护 z 序
		if (pWnd->GetLayerMode() == 0 && pWnd->GetSafeHwnd() != NULL &&
			::IsWindowVisible(pWnd->GetSafeHwnd()))
		{
			pWnd->MaintainZOrder();   // 内部判断桌面模式并维护层级
		}
	}
}

// WorkBuddy: 启动时"自动显示桌面"的窗口枚举回调上下文。
// m_arrWidgets 是 CDeskTidyDlg 的私有成员，自由函数回调无法直接访问，
// 因此在 ShowDesktopAll 中先把需要排除的窗口句柄（主对话框 + 全部小窗口）
// 收集进本结构体，再通过 lParam 传入回调；回调只访问普通 POD 数据。
struct ShowDesktopCtx
{
	HWND hMainDlg;                       // 主对话框句柄（启动后即隐藏，需排除）
	HWND arrWidgetHwnd[64];              // 全部小窗口句柄，用于排除本程序 UI
	int  nWidgetCount;
};

// WorkBuddy: 启动时"自动显示桌面"的窗口枚举回调。必须是 __stdcall 自由函数
// （EnumWindows 要求的调用约定），通过 lParam 取回 ShowDesktopCtx 指针。
// 对每个顶层窗口判断是否属于"需要被最小化的普通应用窗口"，是则异步最小化。
static BOOL CALLBACK EnumMinimizeProc(HWND hwnd, LPARAM lParam)
{
	ShowDesktopCtx* pCtx = reinterpret_cast<ShowDesktopCtx*>(lParam);

	// 跳过：不可见 或 已最小化的窗口（无需重复操作）
	if (!::IsWindowVisible(hwnd) || ::IsIconic(hwnd))
		return TRUE;

	// 跳过：本程序自身的窗口——主对话框（启动后即隐藏）+ 全部小窗口，
	// 避免把自己的 UI 也一起最小化
	if (pCtx->hMainDlg != NULL && hwnd == pCtx->hMainDlg)
		return TRUE;
	for (int i = 0; i < pCtx->nWidgetCount; i++)
	{
		if (pCtx->arrWidgetHwnd[i] == hwnd)
			return TRUE;
	}

	// 跳过：系统/桌面窗口（任务栏 Shell_TrayWnd、桌面根 Progman/WorkerW、
	// 任务栏上的开始/任务视图按钮 Button 等），它们不属于"应用窗口"
	TCHAR szCls[64] = { 0 };
	if (::GetClassName(hwnd, szCls, _countof(szCls)) > 0)
	{
		if (::_tcsicmp(szCls, _T("Shell_TrayWnd")) == 0 ||
			::_tcsicmp(szCls, _T("Progman")) == 0 ||
			::_tcsicmp(szCls, _T("WorkerW")) == 0 ||
			::_tcsicmp(szCls, _T("Button")) == 0)
			return TRUE;
	}

	// 跳过：工具窗口（无任务栏按钮的浮动面板），避免误收起系统级辅助窗口
	LONG_PTR ex = ::GetWindowLongPtr(hwnd, GWL_EXSTYLE);
	if (ex & WS_EX_TOOLWINDOW)
		return TRUE;

	// 异步最小化该应用窗口（ShowWindowAsync 不阻塞当前调用栈，
	// 防止目标窗口处理消息时与启动流程相互等待）
	::ShowWindowAsync(hwnd, SW_MINIMIZE);
	return TRUE;
}

// WorkBuddy: 启动时"自动显示桌面"——最小化除本程序窗口以外的全部可见顶层
// 应用窗口，等效 Win+D 的"显示桌面"效果。保留主对话框（已隐藏）与全部小窗口
// 不被最小化，使桌面与 DeskTidy 小窗口显露出来，方便用户整理桌面文件。
void CDeskTidyDlg::ShowDesktopAll()
{
	// 先把需排除的窗口句柄收集到本地上下文（m_arrWidgets 为私有成员，
	// 不能在自由函数回调里直接访问），再交给 EnumWindows 枚举回调。
	ShowDesktopCtx ctx = { 0 };
	ctx.hMainDlg = m_hWnd;
	int nMax = (int)_countof(ctx.arrWidgetHwnd);
	for (int i = 0; i < (int)m_arrWidgets.GetCount() && i < nMax; i++)
	{
		CDeskTidyWidget* pWnd = m_arrWidgets[i];
		if (pWnd != NULL && pWnd->GetSafeHwnd() != NULL)
			ctx.arrWidgetHwnd[ctx.nWidgetCount++] = pWnd->GetSafeHwnd();
	}

	::EnumWindows(EnumMinimizeProc, reinterpret_cast<LPARAM>(&ctx));
}
