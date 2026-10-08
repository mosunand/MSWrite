# Mswrite

类 Typora 的**所见即所得** Markdown 编辑器,Windows 桌面应用。

- 主程序:**C++17 / Qt 6 / Win32 COM**(原生菜单、标签页、大纲、状态栏)
- 编辑区:**WebView2**(系统自带 Chromium)承载 [Vditor](https://github.com/Vanessa219/vditor) 即时渲染内核
- 内置 **AI 写作助手**:多协议接入(Anthropic / OpenAI / Gemini),AI 可把内容直接写进文档光标处
- 编辑与 PDF 阅读完全离线可用;AI 功能连接用户自己配置的模型服务,无任何遥测

## 功能特性

### 编辑
- 所见即所得(IR)即时渲染,`Ctrl+/` 切换源码/分屏模式
- 公式(KaTeX)、Mermaid 图表、代码高亮、任务列表、表格、脚注、TOC
- 行内公式 `$x_0$` 编辑时保留源码、悬浮气泡预览;「优先 LaTeX 风格」可将
  `\(...\)` / `\[...\]` 自动转换为 `$...$` / `$$...$$`(可撤销,不扫描旧文档)
- 兼容 `** openai **` 等内侧带空格的粗体标记,保留原文与空格
- `Ctrl+1~6` 标题升降级保留光标相对位置与选区方向
- 右键菜单给选中文字上色(落盘为 `<font color>` 标记,可移植)
- **截图直接 `Ctrl+V` 粘贴**,图片存文档旁 `assets/`,插入相对路径
- 专注模式 / 打字机模式;整页缩放(Ctrl+滚轮)与字号调节分离
- 正文行距可调(偏好设置),代码块与公式行距保持独立

### 文档管理
- 多标签页(拖动排序、未保存提醒);`Ctrl+P` 快速打开;`Ctrl+Shift+O` 工作区
- `Ctrl+S` 保存 + 停笔 2 秒自动保存;最近文件 / 工作区 / 主题 / 窗口布局记忆
- 状态栏实时显示保存状态;正文行距与座右铭可在「偏好设置」调整
- **编码嗅探**:UTF-8 / 带 BOM / UTF-16 / 系统 ANSI(GBK)自动识别,按原编码写回

### 搜索与导出
- `Ctrl+F` 当前文档查找替换;`Ctrl+Shift+F` 工作区全库搜索(递归 *.md)
- 导出 HTML / PDF(A4 所见即所得)/ Word(兼容 HTML)

### PDF 阅读
- 文字选择、复制、搜索高亮、连续滚动、目录跳转
- 按屏幕像素密度超采样渲染,缩放后重绘;跟随浅色/夜间主题
- 选中文字可右键「AI 翻译」/「AI 分析」送入助手

### AI 写作助手
- 独立对话窗口(与主窗口解绑),流式回复,思考过程可折叠
- **写入模式**:不写入 / AI 决定 / 强制全写——AI 通过 Insert 工具把内容写进当前文档光标处
- **按需读取文档**:AI 只在你要求时读取当前 Markdown 或 PDF(含页面图像),请求绑定发起时的标签
- 多供应商管理:一个网址可挂多个模型,支持从 cc-switch 一键导入(含 Key,只读本机配置)
- 思考程度按模型档位可选;附件支持截图粘贴与文本文件
- 内置写作技能(公式/表格/颜色约定)自动注入系统提示;可自定义技能目录
- 对话历史本地持久化,重启恢复

### 主题
- 浅色(GitHub)/ 夜间(Dark)/ 纸白(WeChat),默认跟随系统深浅色
- 编辑器、PDF、AI 窗口、全部对话框统一跟随

## 环境要求

- Windows 10/11(x64)
- Qt 6.8(MinGW 版,含 Widgets / Concurrent / Network / Sql / Pdf / PdfWidgets / QuickWidgets / QuickControls2)
- CMake 3.21+ 与 Ninja(或 MinGW Makefiles)
- MinGW g++ 13(与 Qt 包配套)
- WebView2 运行时(Windows 10/11 通常自带;程序用系统自带版本,零安装)

## 从源码构建

```bash
cmake -G "MinGW Makefiles" -B build -DCMAKE_PREFIX_PATH="<Qt安装路径>/6.8.3/mingw_64" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
build\Mswrite.exe            # 开发运行(页面资源直接读源码树)
build\Mswrite.exe 某文档.md   # 打开指定文件
```

调试:启动前设置环境变量 `MSWRITE_DEV=1` 可启用 DevTools(F12/Shift+F12)。

## 打包

```powershell
# 便携版(产物在 dist\Mswrite\,零安装)
powershell -ExecutionPolicy Bypass -File scripts\package.ps1

# 安装版(需本机装有 Inno Setup 6;产物 dist\setup.exe)
powershell -ExecutionPolicy Bypass -File scripts\make-installer.ps1
```

- 打包脚本从构建产物与源码资源组装全新的暂存目录,**不读取**运行过的便携版或已有安装
- 发布前自动执行凭据审计(`scripts/assert-release-clean.ps1`),发现疑似 Key / 运行配置即失败
- 安装版:免管理员(装到 `%LOCALAPPDATA%\Programs\Mswrite`)、卸载保留用户文档、自动检测 WebView2

## 项目结构

```
Mswrite/
├─ CMakeLists.txt
├─ src/                    # 全部原生 C++(Qt6)
│  ├─ main.cpp             # 入口(AI 诊断 --ai-doctor / 冒烟 --ai-smoke)
│  ├─ mainwindow.*         # 主窗口:多标签/菜单/导出/搜索接线
│  ├─ webviewhost.*        # WebView2 宿主(COM 封装/共享环境/进程自愈/PDF 打印)
│  ├─ bridge.*             # C++ -> JS 调用构造(JSON 转义)
│  ├─ fileservice.*        # 读写/编码嗅探/最近文件/偏好
│  ├─ pdfview.*            # PDF 阅读器
│  ├─ uidialogs.*          # 主题化对话框(保存确认/偏好设置/技能目录)
│  └─ ai/                  # AI 写作助手
│     ├─ AiChatDock.*      # 独立对话窗口(主题/附件/会话持久化)
│     ├─ AiWorker.*        # 工作线程:对话循环 + Insert/ReadDocument 工具
│     ├─ Llm*. / Http.*    # 多协议流式客户端(Anthropic/OpenAI/Gemini)
│     ├─ AiProviders.*     # 供应商存储(%APPDATA%/Mswrite/ai-providers.json)
│     ├─ AiCcSwitch.*      # cc-switch 只读导入
│     ├─ ChatWebView.*     # 对话页 WebView(渲染状态机/自愈)
│     └─ AiModelPicker.h   # 供应商/模型两级切换弹层
├─ resources/
│  ├─ web/                 # editor.html + bridge.js(Vditor 内核) + chat.html(对话页)
│  ├─ skills/              # 内置 AI 写作技能(SKILL.md)
│  ├─ app.qrc / app.rc     # 资源与版本信息
│  └─ pdf/                 # PDF 阅读器 QML
├─ third_party/webview2/   # WebView2 SDK(WebView2.h + Loader.dll)
├─ installer/Mswrite.iss   # Inno Setup 6 安装脚本
└─ scripts/                # 便携版/安装版打包 + 凭据审计
```

## 架构要点

- **双 Web 面**:编辑页(app.local)与 AI 对话页(chat.local)是两个独立
  WebView2 宿主,共享同一用户数据目录与渲染环境;页面资源经虚拟主机映射从本地目录提供
- **桥接**:JS 侧 `window.chrome.webview.postMessage` 上行,宿主 `ExecuteScript` 下行;
  编辑内容以 Markdown 全文往返,图片以相对路径落盘
- **自愈链**:页面渲染有看门狗(首帧超时重载)、ExecuteScript 回调超时兜底、
  连续失败整链重建、`ProcessFailed` 事件即时重建、宿主 HWND 变更自动适配——
  任何一层失效都不会让用户永久停留在加载态
- **AI 线程隔离**:模型请求跑在工作线程,GUI 通过队列与阻塞回调交互,
  退出时先置位停止标志再停线程,避免卡死

## AI 供应商配置

- 配置保存在本机 `%APPDATA%/Mswrite/ai-providers.json`,**不属于源码、不进安装包**
- 支持 Anthropic Messages、OpenAI Chat Completions、OpenAI Responses、Gemini 原生协议
- Key 仅存本机;请求只发往用户配置的地址;内置凭据审计确保发布物不含任何密钥
- 可选:启动时只读导入本机 [cc-switch](https://github.com/farion1231/cc-switch) 的供应商配置

## 隐私与安全

- 无遥测、无崩溃上报、无内嵌统计
- 文档只存本机;AI 请求仅在你主动发送时发往你配置的服务
- 发布流水线内置 `assert-release-clean.ps1`:任何疑似 API Key / 运行时凭据都会让打包失败

## 第三方组件

| 组件 | 用途 | 许可 |
| --- | --- | --- |
| [Vditor](https://github.com/Vanessa219/vditor)(含 Lute/KaTeX/highlight.js) | 编辑内核 | MIT |
| [WebView2 SDK](https://learn.microsoft.com/microsoft-edge/webview2/) | 系统自带运行时的 C++ 接口 | 微软 SDK 再分发条款 |
| Qt 6 | 应用框架 | LGPL/GPL(按你的 Qt 发行方式) |

> 开源发布前请按你的分发方式复核上述组件许可的合规义务(尤其 Qt 的
> LGPL 动态链接要求与 Vditor 内 Lute 的许可说明)。

## 许可证

本项目源码以仓库根目录 `LICENSE` 所载许可发布。
