
// DeskTidy.cpp: 定义应用程序的类行为。
//

#include "pch.h"
#include "framework.h"
#include "DeskTidy.h"
#include "DeskTidyDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif


// ---------------------------------------------------------------------------
// WorkBuddy: 单实例运行支持
// ---------------------------------------------------------------------------
// 目标：同一时刻只允许一个 DeskTidy 进程运行。用户重复双击图标时，不弹
//       "程序已在运行"的阻断式提示，而是把已在运行那个实例的主界面显示到
//       前台 —— 用户按下图标的本意是"我要看到界面"，不是"我要开第二个进程"。
//
// 为什么用命名互斥体而不是"查找已有窗口"来判唯一性：
//   - 互斥体由内核持有，进程无论正常退出还是崩溃，句柄都会自动回收，不会
//     留下僵尸锁导致此后永远启动不了；
//   - 窗口查找存在启动竞态（第一个实例的主窗口可能还没建出来），互斥体在
//     进程启动瞬间就完成判定，没有这个窗口期。
//
// 命名空间用 "Local\" 而不是 "Global\"：
//   - 同一登录会话内严格互斥（含"以管理员身份运行"与普通权限混合启动）；
//   - 不同用户会话各有独立桌面，允许各跑一份，符合桌面整理工具的使用直觉。
// ---------------------------------------------------------------------------

// 跨进程唤醒用的自定义消息 WM_DESKTIDY_ACTIVATE 定义在 DeskTidyWidget.h
// （WM_APP + 20，与 WM_TRAYICON / WM_WIDGET_CHANGED 等统一管理）。
// 用固定 ID 而不是 RegisterWindowMessage 的原因：
//   - 它是编译期常量，两个进程天然一致，无需"两边都注册一遍"的隐式约定；
//   - WM_APP 区间（0x8000~0xBFFF）本就是留给应用自行使用的消息号段，
//     跨进程 PostMessage 完全合法，不会与系统消息冲突。

namespace
{
	// 单实例互斥体名（Local\ 作用域 ＝ 当前登录会话）
	LPCTSTR const kSingleInstMutexName = _T("Local\\DeskTidy_SingleInstance_Mutex");

	// 调试开关：命令行带 --multi 时允许多开（便于新旧版本对照调试），默认不生效。
	// 用 CWinApp::m_lpCmdLine（MFC 已剥离程序名）逐词比较，避免 exe 路径里
	// 恰好含 "--multi" 子串被误判；引号也作为分隔符，兼容带空格的路径
	BOOL IsMultiInstanceAllowed()
	{
		CWinApp* pApp = AfxGetApp();
		if (pApp == NULL || pApp->m_lpCmdLine == NULL)
			return FALSE;

		CString strCmd(pApp->m_lpCmdLine);
		int nPos = 0;
		CString strToken = strCmd.Tokenize(_T(" \t\""), nPos);
		while (!strToken.IsEmpty())
		{
			if (strToken.CompareNoCase(_T("--multi")) == 0)
				return TRUE;
			strToken = strCmd.Tokenize(_T(" \t\""), nPos);
		}
		return FALSE;
	}

	// 取得本进程 exe 的文件名（如 "DeskTidy.exe"）。
	// 用它来识别"另一个 DeskTidy 实例"的窗口：Debug/Release、改名、换目录
	// 都自动适用，不必硬编码程序名
	void GetSelfExeName(LPTSTR pszName, int cchName)
	{
		pszName[0] = _T('\0');

		TCHAR szPath[MAX_PATH] = {};
		if (::GetModuleFileName(NULL, szPath, (DWORD)_countof(szPath)) == 0)
			return;

		LPCTSTR pszBase = _tcsrchr(szPath, _T('\\'));
		lstrcpyn(pszName, (pszBase != NULL) ? pszBase + 1 : szPath, cchName);
	}

	// EnumWindows 回调的上下文
	struct FindMainWndCtx
	{
		HWND  hFound;				// 找到的主窗口（未找到为 NULL）
		TCHAR szSelfExe[MAX_PATH];	// 本进程 exe 文件名
	};

	// 判断某个顶层窗口是否就是"另一个 DeskTidy 实例的主窗口"：
	//   ① 必须是对话框窗口类 #32770（主界面 IDD_DESKTIDY_DIALOG 即对话框）；
	//   ② 其所属进程的 exe 文件名与本进程相同。
	// 用"进程可执行文件名 + 窗口类"而不是"窗口标题"来识别，是为了不与界面文案
	// 耦合 —— 标题一旦调整，功能不该跟着失效。
	// 取不到对方进程信息（例如对方以管理员身份运行、本进程为普通权限）时按
	// 不匹配处理：那种情况下即便找到窗口也发不过去消息（UIPI 会拦截），
	// 交由调用方给出"已在运行"的提示即可。
	BOOL CALLBACK EnumFindMainWndProc(HWND hWnd, LPARAM lParam)
	{
		FindMainWndCtx* pCtx = (FindMainWndCtx*)lParam;

		TCHAR szClass[32] = {};
		if (::GetClassName(hWnd, szClass, _countof(szClass)) == 0)
			return TRUE;
		if (_tcsicmp(szClass, _T("#32770")) != 0)
			return TRUE;			// 不是对话框窗口，继续枚举

		// 主对话框（IDD_DESKTIDY_DIALOG）创建时没有父窗口，故 owner 为 NULL；
		// 而"关于"、标题栏/背景透明度设置等同进程的模态子对话框都挂在主窗口
		// 之下（有 owner）。借此把它们排除，保证唤起的始终是设置主界面
		if (::GetWindow(hWnd, GW_OWNER) != NULL)
			return TRUE;

		DWORD dwPid = 0;
		::GetWindowThreadProcessId(hWnd, &dwPid);
		if (dwPid == 0 || dwPid == ::GetCurrentProcessId())
			return TRUE;			// 已退出或本进程自己的窗口

		HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, dwPid);
		if (hProc == NULL)
			return TRUE;			// 权限不足，无法确认身份，跳过

		TCHAR szExe[MAX_PATH] = {};
		DWORD dwLen = (DWORD)_countof(szExe);	// 显式转换，避免 C4267（size_t -> DWORD）
		BOOL bSameExe = FALSE;
		if (::QueryFullProcessImageName(hProc, 0, szExe, &dwLen))
		{
			LPCTSTR pszBase = _tcsrchr(szExe, _T('\\'));
			pszBase = (pszBase != NULL) ? pszBase + 1 : szExe;
			bSameExe = (_tcsicmp(pszBase, pCtx->szSelfExe) == 0);
		}
		::CloseHandle(hProc);

		if (bSameExe)
		{
			pCtx->hFound = hWnd;
			return FALSE;			// 已找到，停止枚举
		}
		return TRUE;
	}

	// 找到已运行实例的主窗口，发消息请它把设置界面显示到前台。
	//   - 被隐藏（驻留托盘）的窗口同样能被枚举到，因此主界面是否可见都不影响；
	//   - 第一个实例可能尚未把对话框建出来（启动竞态），故做有限重试；
	//   - 返回 TRUE 表示消息已投递成功。
	BOOL ActivateExistingInstance()
	{
		FindMainWndCtx ctx;
		ZeroMemory(&ctx, sizeof(ctx));
		GetSelfExeName(ctx.szSelfExe, (int)_countof(ctx.szSelfExe));

		for (int i = 0; i < 12; i++)		// 12 × 100ms，最多约 1.2 秒
		{
			ctx.hFound = NULL;
			::EnumWindows(EnumFindMainWndProc, (LPARAM)&ctx);

			if (ctx.hFound != NULL && ::IsWindow(ctx.hFound))
			{
				// 本进程即将退出，把前台权限让渡出去，否则另一个进程的
				// SetForegroundWindow 会被系统拒绝，界面只能"显示"却不会
				// 跳到最前，用户看起来像没反应
				::AllowSetForegroundWindow(ASFW_ANY);
				::PostMessage(ctx.hFound, WM_DESKTIDY_ACTIVATE, 0, 0);
				return TRUE;
			}
			::Sleep(100);
		}
		return FALSE;
	}
}


// CDeskTidyApp

BEGIN_MESSAGE_MAP(CDeskTidyApp, CWinApp)
	ON_COMMAND(ID_HELP, &CWinApp::OnHelp)
END_MESSAGE_MAP()


// CDeskTidyApp 构造

CDeskTidyApp::CDeskTidyApp()
	: m_hSingleInst(NULL)		// WorkBuddy: 尚未持有单实例互斥体
{
	// 支持重新启动管理器
	m_dwRestartManagerSupportFlags = AFX_RESTART_MANAGER_SUPPORT_RESTART;

	// TODO: 在此处添加构造代码，
	// 将所有重要的初始化放置在 InitInstance 中
}


// 唯一的 CDeskTidyApp 对象

CDeskTidyApp theApp;


// CDeskTidyApp 初始化

BOOL CDeskTidyApp::InitInstance()
{
	// 如果应用程序存在以下情况，Windows XP 上需要 InitCommonControlsEx()
	// 使用 ComCtl32.dll 版本 6 或更高版本来启用可视化方式，
	//则需要 InitCommonControlsEx()。  否则，将无法创建窗口。
	INITCOMMONCONTROLSEX InitCtrls;
	InitCtrls.dwSize = sizeof(InitCtrls);
	// 将它设置为包括所有要在应用程序中使用的
	// 公共控件类。
	InitCtrls.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&InitCtrls);

	CWinApp::InitInstance();

	// WorkBuddy: 单实例检查。
	//   位置选在 MFC 框架初始化之后、Socket/OLE/Shell 管理器等重初始化之前：
	//   既保证 MFC 资源与消息映射已就绪（AfxMessageBox 可用），又能在重复启动时
	//   尽早退出，不做无谓的初始化。此时 m_pMainWnd 仍为 NULL，MFC 的
	//   AfxWinMain 不会额外销毁窗口，直接走 ExitInstance 干净收尾。
	if (!AcquireSingleInstance())
		return FALSE;	// 已有实例在运行（已请它显示主界面），本次启动到此结束

	if (!AfxSocketInit())
	{
		AfxMessageBox(IDP_SOCKETS_INIT_FAILED);
		return FALSE;
	}


	AfxEnableControlContainer();

	// 初始化 OLE/COM 支持（提取 .lnk 快捷方式图标时需要使用 IShellLink COM 接口）
	if (!AfxOleInit())
	{
		AfxMessageBox(_T("OLE 初始化失败，快捷方式图标可能无法正确显示。"));
		return FALSE;
	}

	// 创建 shell 管理器，以防对话框包含
	// 任何 shell 树视图控件或 shell 列表视图控件。
	CShellManager *pShellManager = new CShellManager;

	// 激活“Windows Native”视觉管理器，以便在 MFC 控件中启用主题
	CMFCVisualManager::SetDefaultManager(RUNTIME_CLASS(CMFCVisualManagerWindows));

	// 标准初始化
	// 如果未使用这些功能并希望减小
	// 最终可执行文件的大小，则应移除下列
	// 不需要的特定初始化例程
	// 更改用于存储设置的注册表项
	// 本程序的实际配置通过 %APPDATA%\DeskTidy\DeskTidy.ini 保存，
	// 这里仅为 MFC 框架保留一个规范的注册表键名
	SetRegistryKey(_T("DeskTidy"));

	CDeskTidyDlg dlg;
	m_pMainWnd = &dlg;
	INT_PTR nResponse = dlg.DoModal();
	if (nResponse == IDOK)
	{
		// TODO: 在此放置处理何时用
		//  “确定”来关闭对话框的代码
	}
	else if (nResponse == IDCANCEL)
	{
		// TODO: 在此放置处理何时用
		//  “取消”来关闭对话框的代码
	}
	else if (nResponse == -1)
	{
		TRACE(traceAppMsg, 0, "警告: 对话框创建失败，应用程序将意外终止。\n");
		TRACE(traceAppMsg, 0, "警告: 如果您在对话框上使用 MFC 控件，则无法 #define _AFX_NO_MFC_CONTROLS_IN_DIALOGS。\n");
	}

	// 删除上面创建的 shell 管理器。
	if (pShellManager != nullptr)
	{
		delete pShellManager;
	}

#if !defined(_AFXDLL) && !defined(_AFX_NO_MFC_CONTROLS_IN_DIALOGS)
	ControlBarCleanUp();
#endif

	// 由于对话框已关闭，所以将返回 FALSE 以便退出应用程序，
	//  而不是启动应用程序的消息泵。
	return FALSE;
}


// WorkBuddy: 抢占单实例互斥体（设计说明见本文件开头注释）。
// 返回值语义见头文件；被判定为"重复启动"时，本函数负责唤醒已有实例。
BOOL CDeskTidyApp::AcquireSingleInstance()
{
	if (IsMultiInstanceAllowed())
		return TRUE;		// 调试开关：允许多开，不做唯一性限制

	// 最多两轮：第二轮用于消解"上一个实例刚好正在退出"的竞态 —— 那时它的
	// 主窗口已经销毁、但进程（连同互斥体）尚未完全退出，只等一轮会误报
	// "程序已在运行"，把用户卡在门外
	for (int nAttempt = 0; nAttempt < 2; nAttempt++)
	{
		// bInitialOwner = FALSE：不要求所有权，只把互斥体当作"实例存在标记"，
		// 因此无需 ReleaseMutex，进程结束由内核回收即可
		HANDLE hMutex = ::CreateMutex(NULL, FALSE, kSingleInstMutexName);
		if (hMutex == NULL)
			return TRUE;	// 创建失败（极罕见）：放行，不因互斥体异常阻断启动

		if (::GetLastError() != ERROR_ALREADY_EXISTS)
		{
			m_hSingleInst = hMutex;		// 由本进程持有，直到进程结束
			return TRUE;				// 抢占成功，本次启动继续
		}

		::CloseHandle(hMutex);			// 不是本进程该持有的，必须关掉，避免句柄泄漏

		// 已有实例在运行：唤醒它的设置界面，让用户看到"唯一那一个"程序
		if (ActivateExistingInstance())
			return FALSE;

		// 没找到它的主窗口，多半是对方正在退出：稍候再抢一次
		::Sleep(300);
	}

	AfxMessageBox(_T("DeskTidy 已经在运行。"), MB_OK | MB_ICONINFORMATION);
	return FALSE;
}


// WorkBuddy: 释放单实例互斥体。
//   正常退出（含 InitInstance 返回 FALSE 的提前退出）都会走到这里；
//   若进程异常崩溃未走到，内核也会在进程对象销毁时自动关闭句柄并释放互斥体，
//   因此不存在"上次崩溃后无法再启动"的问题。
int CDeskTidyApp::ExitInstance()
{
	if (m_hSingleInst != NULL)
	{
		::CloseHandle(m_hSingleInst);
		m_hSingleInst = NULL;
	}
	return CWinApp::ExitInstance();
}

