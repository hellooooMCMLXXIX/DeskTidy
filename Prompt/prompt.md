1. 这里要做一个桌面整理软件
* 在Windows桌面显示小窗口，窗口内显示指定目录里面的文件；
* 桌面小窗口默认永远显示在Windows窗口最底层，Windows的“显示桌面”功能也不影响它的显示；
* 桌面小窗口可以单独设置显示在窗口底层还是悬浮置顶；
* 软件主界面在启动后默认不显示，在Windows系统托盘显示小图标；
* 系统托盘图标菜单可以显示主界面、退出应用

2. 要求所有的参数配置都存放到exe所在目录的ini文件中
* Widget小窗口可以拖动位置，改变大小，并记录位置、大小信息到配置文件中，下次启动时恢复；
* WIdget小窗口显示的文件内容显示图标和文件名称（太长就用...省略）；
* 每一个Widget小窗口都可以单独指定其属性，比如：悬浮置顶，还是显示在最底层等等；

3. Widget小窗口内容，无论是列表视图，还是网格视图请确保每一项都对齐显示，列表视图也不显示所在目录列；切换视图显示方式后自动刷新

4. Widget置于屏幕底层时不要挂接到底层桌面宿主（WorkerW），现有的void CDeskTidyWidget::AttachToDesktop()保留不要删除
Widget小窗口，鼠标移动到边缘可拖动修改窗口大小时，修改图标形状
Widget小窗口置于屏幕最底层时，也可以拖动位置，改变大小

5. Widget小窗口的顶部标题栏添加小按钮，实现刷新列表，打开所在目录，切换视图，切换浮动置顶，置底等功能

6. Widget小窗口的顶部标题栏添加小按钮，点击后可以折叠或展开Widget小窗口，折叠时只显示Widget的标题栏；同时需要在ini文件保存状态；右键菜单也添加此功能；

7. 任务量图标增加右键菜单，显示Widget，隐藏wiget；

8. Widget小窗口实现鼠标拖放功能，允许鼠标拖放文件过来添加；拖放过来的文件如果是快捷方式就拷贝一份过来到Widget绑定的目录中，如果是其它文件就在绑定的目录中创建快捷方式；拖放后记得刷新Widget文件列表；


9. DeskTidy软件增加开机自启动功能，参数设置界面可以修改是否开机自启动；
10. DeskTidy软件启动时隐藏主界面（参数设置界面）
11. DeskTidy软件,参数设置界面增加设置快捷键功能，可以为Widget小窗口悬浮置顶，和置底功能自定义快捷键；快捷键可以不指定；指定时要判断快捷键有没有冲突；快捷键要求是全局快捷键；

12. Widget小窗口增加支持Ctrl+鼠标滚轮缩放内部展示的列表的图标、文字大小；
13. 请为刚才的缩放功能添加快捷键支持

14. Widget小窗口设置为置底时，使用SetWinEventHook 事件驱动：监听 EVENT_SYSTEM_FOREGROUND/EVENT_OBJECT_SHOW，检测到 z 序可能变化时压底
15. 现在CDeskTidyWidget会闪烁，修改这个问题

16. 如何实现Widget小窗口一直在Window桌面之上，其它所有窗口之下，并且“显示桌面”时也能正常显示，并且不影响鼠标操作；给出方案，但不要修改代码；

17. CDeskTidyWidget小窗口属性设置为置底时，把它设置成 WorkerW/SHELLDLL_DefView 的子窗口
18. CDeskTidyWidget小窗口设置成 WorkerW/SHELLDLL_DefView 的子窗口时会半透明，如何让他不透明
19. CDeskTidyWidget小窗口
置底模式不再挂接为桌面子窗口（会半透明），也不再仅靠"最小化后恢复"兜底，而是保持普通顶层窗口，但以桌面 Shell 窗口为 Owner（DeskTidyWidget.cpp）：
SetDesktopOwner() — 用 SetWindowLongPtr(GWLP_HWNDPARENT)（对顶层窗口该槽位即 Owner，同 WitchDrawer 的 WindowOwnerIndex = -8）挂接 Progman（或含 SHELLDLL_DefView 的 WorkerW）
被桌面拥有的顶层窗口：Win+D 不会最小化它；owned 窗口永远在 Owner 之上——桌面被抬升时小窗口跟随抬升，天然可见且在所有普通窗口之下
	
20. CDeskTidyWidget小窗口标题栏的按钮在鼠标放上去时要有文字提示信息；

21. CDeskTidyWidget小窗口增加支持Ctrl+鼠标滚轮放大后，有些文件图标被上一行的文字覆盖显示不完整了

22. CDeskTidyWidget小窗口内部显示的文件列表，网格视图时文件名最多只显示一行
23. CDeskTidyWidget小窗口内部显示的文件列表中，每个文件的名称显示不能太长，长度太长时使用...隐藏多余部分文字；

22. DeskTidy软件启动后自动显示桌面

23. 为DeskTidy软件换一个好看的图标；

24.  CDeskTidyWidget小窗口标题栏颜色、背景色可以单独设置；
25. CDeskTidyWidget小窗口标题栏颜色、背景色可以在设置界面调整；
26. 美化一下界面
27. DeskTidy需要防止重复启动，只能有一个进程在运行
28. 存在没有铺满全屏的窗口时，点击桌面会造成CDeskTidyWidget小窗口在其它窗口上方，能否修补这个问题

29. CDeskTidyWidget小窗口中的文件列表显示的文件名称字体颜色修改成可以配置，支持右键菜单修改和设置页面修改；

30. CDeskTidyWidget小窗口中的文件要做到鼠标拖放出来，把文件移动到拖放的文件夹，这种如何实现，给出方案，不要修改代码；【放弃】

31. CDeskTidyWidget小窗口中的文件要实现鼠标右键菜单：“复制”，“剪切”,“删除”功能，如何实现，给出方案，不要修改代码；
73538378

32. CDeskTidyWidget小窗口设置为置底时，新增一套置底方案：CDeskTidyWidget小窗口属性设置为置底时，把它设置成 WorkerW/SHELLDLL_DefView 的子窗口，这种方案不需要定时z 序维护；
在参数设置界面可以设置是否开启这种方式，如果不开启就采用当前代码的方案