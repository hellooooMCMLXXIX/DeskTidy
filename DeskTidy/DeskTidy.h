
// DeskTidy.h: PROJECT_NAME 应用程序的主头文件
//

#pragma once

#ifndef __AFXWIN_H__
	#error "在包含此文件之前包含 'pch.h' 以生成 PCH"
#endif

#include "resource.h"		// 主符号


// CDeskTidyApp:
// 有关此类的实现，请参阅 DeskTidy.cpp
//

class CDeskTidyApp : public CWinApp
{
public:
	CDeskTidyApp();

// 重写
public:
	virtual BOOL InitInstance();
	virtual int  ExitInstance();	// WorkBuddy: 释放单实例互斥体

// 实现
private:
	// WorkBuddy: 单实例运行——抢占互斥体。
	//   TRUE  ＝ 本进程是唯一实例（互斥体已持有，进程退出时释放）
	//   FALSE ＝ 已有实例在运行（已尝试唤醒它显示主界面），调用方应结束本次启动
	BOOL AcquireSingleInstance();

	HANDLE m_hSingleInst;			// WorkBuddy: 单实例互斥体句柄（NULL ＝ 未持有）

	DECLARE_MESSAGE_MAP()
};

extern CDeskTidyApp theApp;

// WorkBuddy: 单实例运行相关的自定义消息 WM_DESKTIDY_ACTIVATE 定义在
//   DeskTidyWidget.h 的自定义消息区（与 WM_TRAYICON / WM_WIDGET_CHANGED 同处），
//   本文件不需要重复声明。
