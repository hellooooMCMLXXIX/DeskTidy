
// DeskTidyDlg.h: 主对话框（设置界面）头文件
//
// CDeskTidyDlg 承担三类职责：
//   1. 系统托盘图标：启动时隐藏主界面，仅显示托盘图标；托盘右键菜单可
//      "显示主界面" / "显示小窗口" / "隐藏小窗口" / "退出应用"；
//   2. 小窗口管理：管理多个 CDeskTidyWidget 桌面小窗口，每个小窗口独立
//      显示一个目录；在设置界面中可添加/删除小窗口、为选中的小窗口
//      单独指定层级（最底层/置顶）与视图（列表/网格）；
//   3. 配置持久化：全部配置写入 exe 所在目录的 DeskTidy.ini，重启后自动恢复，
//      包括每个小窗口的目录、位置、大小、层级模式与视图模式。

#pragma once

#include "DeskTidyWidget.h"
#include <afxtempl.h>   // CArray：小窗口对象数组
#include <afxcmn.h>     // CHotKeyCtrl：全局快捷键输入控件

// ---------------------------------------------------------------------------
// WorkBuddy: 现代扁平界面 —— 悬停感知按钮
// ---------------------------------------------------------------------------
// 自绘按钮需要感知鼠标悬停，但鼠标位于子控件上方时对话框收不到 WM_MOUSEMOVE
// （消息被按钮自己接收，不会冒泡给父窗口）。因此用一个极轻量的 CButton 派生类
// 只做一件事：把"鼠标进入/离开"转发给父对话框（WM_FLATBTN_HOVER 消息）。
// 按钮的实际绘制仍统一在 CDeskTidyDlg::OnDrawItem 中完成，避免两处维护样式。
#define WM_FLATBTN_HOVER    (WM_APP + 201)  // wParam = 控件 ID；lParam = TRUE 进入 / FALSE 离开

class CFlatBtn : public CButton
{
public:
	CFlatBtn() : m_bTracking(FALSE) {}

protected:
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);    // 悬停进入/移动：通知父窗口点亮
	afx_msg void OnMouseLeave();                            // 悬停离开：通知父窗口取消点亮
	DECLARE_MESSAGE_MAP()

private:
	BOOL m_bTracking;   // 是否已注册 WM_MOUSELEAVE 跟踪（避免重复注册）
};

// 小窗口数量上限
#define MAX_WIDGETS     32

// 全局快捷键消息 ID（WM_HOTKEY 消息的 wParam，同一窗口内必须唯一）
#define HOTKEY_ID_TOPMOST   1   // 置顶快捷键：所有小窗口悬浮置顶
#define HOTKEY_ID_BOTTOM    2   // 置底快捷键：所有小窗口最底层
#define HOTKEY_ID_ZOOMIN    3   // 缩放放大快捷键：所有小窗口放大一档
#define HOTKEY_ID_ZOOMOUT   4   // 缩放缩小快捷键：所有小窗口缩小一档

// 置底维护定时器 ID 与防抖延时（毫秒）：
// WinEvent 事件（前台窗口变化/窗口显示等）触发后，先重启本定时器，
// 延时结束后统一把"最底层"模式的小窗口压回 z 序最底。
// 这样可把短时间内密集触发的事件合并成一次压底，避免频繁 SetWindowPos 闪烁
#define TIMER_ID_BOTTOM_PUSH    1   // 置底维护定时器
#define BOTTOM_PUSH_DELAY       400 // 事件后延迟压底的时间（毫秒）

// 两次"实际压底"之间的最小间隔（毫秒）：
// 系统本身会持续产生前台/显示事件（输入法、任务栏、通知等），
// 若每次都执行压底，SetWindowPos 的重绘副作用会让小窗口持续闪烁。
// 置底小窗口一旦被压到底，只有被主动抬高（切换置顶等）才会离开最底，
// 因此压底操作无需高频重复，这里限制最少间隔即可杜绝闪烁
#define BOTTOM_PUSH_MIN_INTERVAL 1500

// 小窗口存活巡检定时器（毫秒）：
// 置底模式的小窗口挂接为桌面（WorkerW/Progman）的子窗口后，Explorer 重启
// 会销毁桌面窗口并连带销毁小窗口（子窗口随父窗口一起销毁）；本定时器周期
// 检查并重建被销毁的小窗口，保证 Explorer 重启后小窗口自动恢复
#define TIMER_ID_WIDGET_CHECK     2     // 小窗口存活巡检定时器
#define WIDGET_CHECK_INTERVAL     3000  // 巡检间隔（毫秒）

// CDeskTidyDlg 对话框
class CDeskTidyDlg : public CDialogEx
{
// 构造
public:
	CDeskTidyDlg(CWnd* pParent = nullptr);	// 标准构造函数

// 对话框数据
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_DESKTIDY_DIALOG };
#endif

protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV 支持

// 实现
protected:
	HICON m_hIcon;

	// 生成的消息映射函数
	virtual BOOL OnInitDialog();
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnWindowPosChanging(WINDOWPOS* lpwndpos);      // 启动时拦截首次"显示"，保持主界面隐藏
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	afx_msg void OnTimer(UINT_PTR nIDEvent);                    // 定时器（置底维护等）
	afx_msg void OnClose();                                 // 点 X：仅隐藏，程序继续驻留托盘
	afx_msg void OnDestroy();                               // 销毁：清理托盘图标与小窗口
	afx_msg LRESULT OnTrayIcon(WPARAM wParam, LPARAM lParam);// 托盘回调消息
	afx_msg LRESULT OnWidgetChanged(WPARAM wParam, LPARAM lParam); // 小窗口状态变更（拖动/缩放结束）
	afx_msg LRESULT OnActivateExisting(WPARAM wParam, LPARAM lParam); // WorkBuddy: 单实例——第二个进程请求显示主界面
	afx_msg void OnTrayShow();                              // 托盘菜单：显示主界面
	afx_msg void OnTrayShowWidgets();                       // 托盘菜单：显示全部小窗口
	afx_msg void OnTrayHideWidgets();                       // 托盘菜单：隐藏全部小窗口
	afx_msg void OnTrayExit();                              // 托盘菜单：退出应用
	afx_msg void OnBnClickedAdd();                          // 添加小窗口（选择目录）
	afx_msg void OnBnClickedDelete();                       // 删除选中的小窗口
	afx_msg void OnBnClickedApply();                        // 应用按钮：把属性应用到选中窗口
	afx_msg void OnSelChangeWidget();                       // 列表选择变化：载入该窗口的属性
	// WorkBuddy: 设置界面中的配色操作（作用于"选中窗口的标题栏颜色 / 背景色"）
	afx_msg void OnBnClickedHeaderColor();                  // 点击标题栏色块：弹出取色器选色
	afx_msg void OnBnClickedBgColor();                      // 点击背景色块：弹出取色器选色
	afx_msg void OnBnClickedTextColor();                    // WorkBuddy: 点击"名称颜色"色块：弹出取色器选色
	afx_msg void OnBnClickedTextAuto();                     // WorkBuddy: 勾选/取消"自动跟随背景"（名称颜色）
	afx_msg void OnBnClickedColorReset();                   // 点击"恢复默认"：三个颜色一起复位
	afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct); // 自绘色块按钮 / 扁平按钮 / 列表项
	afx_msg void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pBar);      // WorkBuddy: 两个透明度滑块（0~100%）
	// WorkBuddy: 现代扁平界面的自绘支持
	afx_msg BOOL   OnEraseBkgnd(CDC* pDC);                                  // 自绘浅灰底 + 两张白色圆角卡片
	afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);        // 控件背景/文字配色（融入卡片）
	afx_msg void   OnMeasureItem(int nIDCtl, LPMEASUREITEMSTRUCT lpMeasureItemStruct); // 自绘列表项行高
	afx_msg LRESULT OnFlatBtnHover(WPARAM wParam, LPARAM lParam);           // 自绘按钮的悬停高亮
	afx_msg LRESULT OnHotKey(WPARAM wParam, LPARAM lParam); // 全局快捷键回调（置顶/置底/缩放）
	DECLARE_MESSAGE_MAP()

private:
	// 添加/移除系统托盘图标
	BOOL AddTrayIcon();
	void RemoveTrayIcon();
	// 显示主界面并置前
	void ShowMainWindow();

	// ---- 配置持久化 ----
	// 返回 exe 所在目录下的 DeskTidy.ini 完整路径
	CString GetIniPath();
	// 从 INI 读取配置并创建所有小窗口
	void LoadConfig();
	// 把当前全部小窗口的配置写入 INI
	void SaveConfig();

	// ---- 小窗口管理辅助 ----
	// 销毁并释放全部小窗口对象
	void DestroyAllWidgets();
	// 为不存在的目录生成一个合理的默认矩形（主屏工作区右下角）
	CRect GetDefaultRect();
	// 把列表选中项对应的属性加载到单选按钮上
	void LoadSelProps();
	// WorkBuddy: 把 m_clrHeaderSel / m_clrBgSel / m_clrTextSel 刷新到三个自绘色块按钮上
	// （触发重绘）；名称色块还会按 m_bTextAutoSel 画出"自动"态
	void UpdateColorButtons();
	// WorkBuddy: 把 m_bTextAutoSel 同步到"自动跟随背景"复选框
	void UpdateTextAutoCheck();
	// WorkBuddy: 把 m_nHeaderAlphaSel / m_nBgAlphaSel 刷新到两个透明度滑块与百分比文字上
	void UpdateAlphaControls();

	// ---- WorkBuddy: 现代扁平界面绘制辅助 ----
	// 该控件 ID 是否为自绘扁平按钮
	BOOL IsFlatButton(int nID) const;
	// 取得两张卡片的客户区像素矩形（源矩形以 DLU 定义，见 g_rcCardDLU）
	void GetCardRects(CRect* pRects, int& nCount);
	// 在指定矩形内绘制圆角白卡 + 加粗分区标题
	void DrawCard(CDC* pDC, const CRect& rcCard, LPCTSTR pszTitle);
	// 绘制扁平按钮（主按钮＝品牌蓝填充，次按钮＝白底描边；含悬停/按下/禁用态）
	void DrawFlatButton(LPDRAWITEMSTRUCT lpDIS);
	// 绘制列表项（选中项为圆角强调色药丸 + 白字，其余为深灰字）
	void DrawListItem(LPDRAWITEMSTRUCT lpDIS);

	// ---- 开机自启动（注册表 Run 键）----
	// 查询当前是否已设置开机自启动
	BOOL QueryAutoStart();
	// 设置或取消开机自启动（写/删注册表 Run 键值）
	void SetAutoStart(BOOL bEnable);

	// ---- 全局快捷键 ----
	// 启动时按 INI 配置注册四个全局快捷键（置顶/置底/缩放放大/缩放缩小）
	void RegisterHotKeys();
	// 反注册四个全局快捷键（未注册时调用无副作用）
	void UnregisterHotKeys();
	// 从界面控件读取并校验四个快捷键，通过后注册并更新成员；失败返回 FALSE
	BOOL ApplyHotKeys();
	// 读取快捷键控件中的组合，转换为保存格式（高 16 位修饰键 + 低 16 位键码）
	int  GetHotKeyFromCtrl(CHotKeyCtrl& ctrl);
	// 把保存的快捷键值显示到快捷键控件（与 GetHotKeyFromCtrl 互逆）
	void SetHotKeyToCtrl(CHotKeyCtrl& ctrl, int nHotKey);
	// 把保存的快捷键值拆分为 RegisterHotKey 所需的修饰键与虚拟键码
	static void SplitHotKey(int nHotKey, UINT& uMods, UINT& uVk);
	// 把所有小窗口统一设置为指定层级（0 = 最底层，1 = 悬浮置顶）并保存配置（快捷键触发）
	void ApplyAllWidgetLayer(int nMode);
	// 把所有小窗口统一放大/缩小一档缩放级别（nStep = 1 放大、-1 缩小）
	// 并保存配置（快捷键触发，与置顶/置底快捷键一样对所有小窗口生效）
	void ApplyAllWidgetZoom(int nStep);

	// ---- 置底维护（SetWinEventHook 事件驱动）----
	// 注册 WinEvent 事件钩子：监听 EVENT_SYSTEM_FOREGROUND（前台窗口变化）、
	// EVENT_OBJECT_SHOW（窗口显示/隐藏）与 EVENT_OBJECT_REORDER（z 序重排，
	// "显示桌面"抬升桌面根窗口时产生），三者都可能改变 z 序；
	// 事件触发后经防抖定时器统一维护所有"最底层"模式小窗口的层级，
	// 替代周期轮询，反应更快且几乎不占额外开销
	void RegisterWinEventHooks();
	// 反注册 WinEvent 事件钩子（未注册时调用无副作用）
	void UnregisterWinEventHooks();
	// 维护所有"最底层"模式且可见的小窗口的层级（防抖定时器到期时调用）：
	// 检测到"显示桌面"模式时把小窗口提到桌面之上，否则压回 z 序最底
	void PushBottomWidgets();
	// WorkBuddy: 启动时"自动显示桌面"——最小化除本程序窗口以外的全部可见
	// 顶层应用窗口（等效 Win+D 的"显示桌面"效果），保留主对话框（已隐藏）
	// 与全部小窗口不被最小化，让桌面与 DeskTidy 小窗口显露出来
	void ShowDesktopAll();
	// 小窗口存活巡检（定时器周期调用）：
	// 置底小窗口挂接为桌面子窗口后会被 Explorer 重启连带销毁，这里检查
	// 已销毁的小窗口并按其保留的配置（目录/位置/层级/视图/缩放/折叠）
	// 重新创建，保证 Explorer 重启后小窗口自动恢复
	void CheckWidgetsAlive();
	// WinEvent 事件回调（静态）：SetWinEventHook 要求回调必须是静态函数，
	// 通过 s_pSelf 访问主对话框实例；只做轻量工作——重启防抖定时器
	static void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event,
									  HWND hwnd, LONG idObject, LONG idChild,
									  DWORD dwEventThread, DWORD dwmsEventTime);
	// 静态实例指针：WinEvent 回调通过它访问本对话框（OnInitDialog 设置、
	// OnDestroy 清空，回调中先判空再使用，避免窗口销毁后悬空调用）
	static CDeskTidyDlg* s_pSelf;

	// ---- 成员变量 ----
	CArray<CDeskTidyWidget*, CDeskTidyWidget*> m_arrWidgets; // 全部小窗口对象（与列表项一一对应）
	int             m_nSelIndex;    // 当前选中的小窗口在数组中的下标（-1 = 未选中）
	CString         m_strIniPath;   // INI 配置文件完整路径
	HICON           m_hTrayIcon;    // 托盘用图标句柄

	// DDX 绑定的控件/数据
	CListBox        m_listWidgets;  // 小窗口列表控件
	int             m_nLayerMode;   // 层级模式：0 = 最底层，1 = 置顶
	int             m_nViewMode;    // 视图模式：0 = 列表，1 = 网格
	BOOL            m_bAutoStart;   // 开机自启动：是否随 Windows 登录自动运行
	BOOL            m_bHideAtStartup; // 启动阶段是否隐藏主界面（用户主动显示后清除，见 ShowMainWindow）

	// WorkBuddy: 设置界面中"选中窗口"的待应用配色。
	//   由 LoadSelProps 从选中窗口读出并显示在色块按钮上；用户点色块改色、
	//   点"恢复默认"复位；点【应用】时再写回选中窗口并保存到 INI
	COLORREF        m_clrHeaderSel; // 标题栏颜色（默认 WIDGET_DEFAULT_HEADER_COLOR）
	COLORREF        m_clrBgSel;     // 背景色（默认 WIDGET_DEFAULT_BG_COLOR）
	// WorkBuddy: 文件名称颜色（文件列表项文字）的待应用值。
	//   m_bTextAutoSel = TRUE 时表示"自动跟随背景"（不使用 m_clrTextSel，
	//   但仍保留其值，便于用户在自动/自定义之间来回切换不丢已选颜色）；
	//   FALSE 时使用 m_clrTextSel 这个具体颜色
	COLORREF        m_clrTextSel;   // 名称颜色（默认 WIDGET_DEFAULT_TEXT_COLOR）
	BOOL            m_bTextAutoSel; // 名称颜色是否"自动跟随背景"（默认 TRUE）

	// WorkBuddy: 设置界面中"选中窗口"的待应用透明度（0~255，255 = 完全不透明）。
	//   与配色同理：滑块拖动时只更新这三个值 + 百分比文字，点【应用】才写回
	//   选中窗口（SetHeaderAlpha/SetBgAlpha）并保存到 INI。
	//   界面上按 0~100% 展示，写入小窗口前换算成 0~255 的 alpha
	int             m_nHeaderAlphaSel;  // 标题栏透明度（默认 255 = 100%）
	int             m_nBgAlphaSel;      // 背景透明度（默认 255 = 100%）

	// WorkBuddy: "z 序（层级）维护"开关的待应用值（默认 TRUE = 开启）。
	//   关闭后小窗口不再执行自动置底 / 防遮挡 / 显示桌面恢复，
	//   也就没有每秒日志、约 5 秒一次的抢前台与压底重绘。
	//   需要自己手动管理窗口层级、或觉得自动维护干扰使用时可关闭
	BOOL            m_bZOrderMaintain;
	BOOL            m_bBottomViaChild;  // WorkBuddy: "置底采用桌面子窗口"开关（UI 选中态）
	CSliderCtrl     m_sliderHeaderAlpha;// "标题栏透明度"滑块（范围 0~100，单位：百分比）
	CSliderCtrl     m_sliderBgAlpha;    // "背景透明度"滑块（范围 0~100，单位：百分比）

	// ---- WorkBuddy: 现代扁平界面状态与资源 ----
	// 画刷/字体均在构造函数中创建（不依赖窗口句柄），
	// 保证首次 WM_ERASEBKGND / WM_CTLCOLOR 到达时已经可用
	int             m_nHoverBtn;    // 当前鼠标悬停的自绘按钮 ID（0 = 无）
	CBrush          m_brDlgBg;      // 对话框浅灰底画刷
	CBrush          m_brCard;       // 卡片与控件白色底画刷
	CFont           m_fontSection;  // 分区标题字体（Segoe UI Semibold，按 DPI 计算字号）
	CFlatBtn        m_btnAdd;       // "添加目录"自绘按钮（悬停感知）
	CFlatBtn        m_btnDelete;    // "删除选中"自绘按钮
	CFlatBtn        m_btnApply;     // "应用"自绘按钮（主按钮，品牌蓝填充）
	CFlatBtn        m_btnColorReset;// "恢复默认"自绘按钮
	CFlatBtn        m_btnHeaderColor; // "标题栏颜色"色块按钮（悬停感知，便于提示可点击）
	CFlatBtn        m_btnBgColor;     // "背景色"色块按钮（悬停感知）
	CFlatBtn        m_btnTextColor;   // WorkBuddy: "名称颜色"色块按钮（悬停感知）

	// 全局快捷键相关（仅主界面内部使用）
	CHotKeyCtrl     m_ctlTopHotKey;      // "置顶"快捷键输入控件
	CHotKeyCtrl     m_ctlBottomHotKey;   // "置底"快捷键输入控件
	CHotKeyCtrl     m_ctlZoomInHotKey;   // "缩放放大"快捷键输入控件
	CHotKeyCtrl     m_ctlZoomOutHotKey;  // "缩放缩小"快捷键输入控件
	BOOL            m_bTopHotKeyEnable;      // "置顶"快捷键是否启用（勾选"启用"复选框）
	BOOL            m_bBottomHotKeyEnable;   // "置底"快捷键是否启用
	BOOL            m_bZoomInHotKeyEnable;   // "缩放放大"快捷键是否启用
	BOOL            m_bZoomOutHotKeyEnable;  // "缩放缩小"快捷键是否启用
	int             m_nTopHotKey;           // "置顶"快捷键值（0 = 未指定）
	int             m_nBottomHotKey;        // "置底"快捷键值（0 = 未指定）
	int             m_nZoomInHotKey;        // "缩放放大"快捷键值（0 = 未指定）
	int             m_nZoomOutHotKey;       // "缩放缩小"快捷键值（0 = 未指定）

	// 置底维护（SetWinEventHook 事件钩子句柄，NULL = 未注册）
	HWINEVENTHOOK   m_hHookForeground;      // 前台窗口变化事件钩子
	HWINEVENTHOOK   m_hHookShow;            // 窗口显示/隐藏事件钩子
	HWINEVENTHOOK   m_hHookReorder;         // z 序重排事件钩子（"显示桌面"抬升桌面时触发）
	// 上次实际执行压底的时间（GetTickCount 毫秒值，0 = 尚未执行）：
	// PushBottomWidgets 用它与 BOTTOM_PUSH_MIN_INTERVAL 比较，抑制高频压底
	DWORD           m_dwLastBottomPush;
};
