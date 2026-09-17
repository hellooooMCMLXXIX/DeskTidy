## 1. 软件概述

DeskTidy（桌面归置）是一款运行在 Windows 桌面上的**桌面文件整理工具**。它在桌面上显示若干个小窗口（Widget），每个小窗口独立展示一个指定目录内的文件（图标 + 文件名），支持拖动、缩放、折叠、置顶/置底、列表/网格视图切换、Ctrl+滚轮缩放、文件拖放收纳等操作。

软件启动后主界面默认隐藏，仅驻留系统托盘；全部配置保存在 **exe 同目录的 `DeskTidy.ini`** 文件中，重启后自动恢复。

<img width="1765" height="1344" alt="ScreenShot_2026-09-17_222920_058" src="https://github.com/user-attachments/assets/df82d2f0-f5f8-4d20-a3ae-9e7d74a0e869" />



### 1.1 核心目标

| 目标       | 说明                                                         |
| ---------- | ------------------------------------------------------------ |
| 目录展示   | 小窗口内展示指定目录文件，图标 + 名称（过长用 `...` 省略）   |
| 层级控制   | 小窗口可置于"所有窗口最底层"或"悬浮置顶"                     |
| 抗 Win+D   | 按 Win+D（显示桌面）时小窗口依然可见                         |
| 配置持久化 | 目录、位置、大小、层级、视图、折叠、缩放、配色、透明度全部写入 INI |
| 轻量驻留   | 启动即驻托盘，无主窗口打扰                                   |

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

| 模块         | 功能                                             | 主要文件                                 |
| ------------ | ------------------------------------------------ | ---------------------------------------- |
| 单实例管理   | 只允许一个进程运行，重复启动唤醒已有实例         | `DeskTidy.cpp`                           |
| 系统托盘     | 启动隐藏主界面，托盘图标 + 右键菜单              | `DeskTidyDlg.cpp`                        |
| 小窗口管理   | 添加/删除/选中小窗口，批量显示/隐藏              | `DeskTidyDlg.cpp`                        |
| 配置持久化   | INI 读写全部配置，重启恢复                       | `DeskTidyDlg.cpp`                        |
| 桌面小窗口   | 目录文件展示、拖动/缩放/折叠、视图切换、双击打开 | `DeskTidyWidget.cpp`                     |
| 层级控制     | 最底层 / 悬浮置顶，Win+D 抗隐藏                  | `DeskTidyWidget.cpp`                     |
| 置底维护     | WinEvent 事件驱动 + 防抖定时器保持 z 序          | `DeskTidyDlg.cpp` / `DeskTidyWidget.cpp` |
| 全局快捷键   | 置顶 / 置底 / 缩放放大 / 缩放缩小                | `DeskTidyDlg.cpp`                        |
| 界面美化     | 现代扁平 UI：自绘卡片、扁平按钮、圆角列表        | `DeskTidyDlg.cpp`                        |
| 开机自启动   | 注册表 Run 键                                    | `DeskTidyDlg.cpp`                        |
| 文件收纳     | 拖放文件进小窗口，拷贝/创建快捷方式              | `DeskTidyWidget.cpp`                     |
| 缩放显示     | Ctrl+滚轮 / 快捷键缩放图标与文字（50%~300%）     | `DeskTidyWidget.cpp`                     |
| 配色与透明度 | 标题栏/背景独立颜色与透明度                      | `DeskTidyWidget.cpp`                     |



