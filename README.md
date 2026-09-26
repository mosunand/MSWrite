# Mswrite

类 Typora 的**所见即所得** Markdown 编辑器,Windows 桌面应用。

- 主程序:**C++17 / Qt 6 / Win32 COM**(原生菜单、文件树、大纲、状态栏、多标签)
- 编辑区:**WebView2**(系统自带 Chromium)承载 [Vditor](https://github.com/Vanessa219/vditor) 即时渲染内核
- 编辑和 PDF 阅读可离线使用；AI 功能连接用户配置的模型服务。

## 构建与运行

要求:Qt 6.8(MinGW 版)、CMake、MinGW g++ 13

```bash
cd E:\Projects\zhipucode\Mswrite
cmake -G "MinGW Makefiles" -B build -DCMAKE_PREFIX_PATH="D:/Users/qt/6.8.3/mingw_64" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
build\Mswrite.exe            # 开发运行(页面资源直接读源码树)
build\Mswrite.exe 某文档.md   # 打开指定文件
```

调试:启动前设置环境变量 `MSWRITE_DEV=1` 可启用 DevTools(F12)。

打包便携版(产物在 `dist\Mswrite\`,零安装):

```powershell
powershell -File scripts\package.ps1
```

打包安装版(需要本机装有 [Inno Setup 6](https://jrsoftware.org/isinfo.php);产物在 `dist\Mswrite-<版本>-setup.exe`):

```powershell
powershell -File scripts\make-installer.ps1
```

安装版特性:免管理员(装到 `%LOCALAPPDATA%\Programs\Mswrite`)、桌面/开始菜单快捷方式、卸载器保留 `MSWriteData` 用户文档、安装前自动检测 WebView2 运行时。脚本内部会把便携版暂存为 `dist\_stage`(剔除 `webview-data`、`MSWriteData`、`mswrite.log`、`export-tmp` 等运行期产物)再交给 Inno 编译。

## 功能清单

**编辑**
- 所见即所得(IR)即时渲染,`Ctrl+/` 切换源码/分屏模式
- 公式(KaTeX)、Mermaid/图表、代码高亮、任务列表、表格、脚注、TOC
- 「设置 → 优先 LaTeX 风格」默认关闭。启用后，新输入、粘贴或 AI 插入的成对 `\$f(x)\$` 会转换为 `$f(x)$`，支持两端各一个或各两个反斜杠。代码框或行内代码的完整内容为单个有效 `$$…$$` 公式时，会转为数学框，也支持从网页复制的富文本。普通代码、单边转义和不完整公式保持原样。适用于即时渲染和源码模式；开关不会扫描旧文档，也不会还原已转换的内容，转换可撤销。
- 已有文档可用「设置 → 修复当前文档的 LaTeX」主动转换，支持撤销。AI 写入的 Markdown 直接经过解析，普通 `$…$` 公式也能即时渲染。
- `Ctrl+B/I/K/1~4` 等 Vditor 原生快捷键;撤销/重做/剪切/复制/粘贴/全选菜单
- 右键菜单可给选中文字上色(红/紫/蓝/绿/黄/橙/灰),落盘为 `<font color>` 标记
- **截图直接 `Ctrl+V` 粘贴**,自动存入文档同级 `assets/` 目录,插入 `./assets/image-xxx.png` 相对路径(文档夹整体可移植)
- 专注模式 / 打字机模式;字号 `Ctrl+=` / `Ctrl+-` / `Ctrl+0`

**文档管理**
- 多标签页(移动/关闭/未保存提醒);`Ctrl+N` 新标签、`Ctrl+W` 关闭
- `Ctrl+P` 快速打开(最近文件 + 工作区模糊搜索)
- 大纲侧栏点击跳转;`Ctrl+Shift+O` 设定工作区(供 `Ctrl+F` 全库搜索与 `Ctrl+P` 快速打开)
- `Ctrl+S` 保存 + 停笔 2 秒自动保存;最近文件/工作区/主题/窗口布局记忆
- 状态栏右下角实时显示保存状态(已保存/未保存,加粗,未保存为红色)
- **编码嗅探**:UTF-8 / 带 BOM / UTF-16 / 系统 ANSI(GBK)自动识别,按原编码写回,
  老 GBK 文档不会被改成 UTF-8 乱码

**搜索与导出**
- `Ctrl+F` 全文搜索:工作区(递归 *.md)或当前文档,点击结果跳转
- 导出 HTML / 导出 PDF(A4,所见即所得)/ 导出 Word(.doc)
- PDF 阅读支持鼠标选字、Ctrl+C、全选当前页、搜索高亮、连续滚动和书签跳转；目录按标题长度与层级自动定宽，也可手动拖动。底栏提供页码输入、缩放、适宽/整页切换，跟随浅色/夜间主题。
- PDF 页面按屏幕像素密度超采样，缩放后重新渲染；限制单页位图大小，避免高倍放大时持续增加内存。扫描页的细节仍取决于原图质量。
- PDF 选中文字后可右键「AI 翻译」或「AI 分析」，自动打开助手并提交选区和页码，保留输入框草稿。未配置 API Key、地址或模型时显示「没有连接服务器」。

**主题**:浅色(GitHub)/ 夜间(Dark)/ 纸白(WeChat),默认跟随系统深浅色

**AI 写作助手**(主菜单「AI」,或按 `F9`):右侧对话面板,AI 能且只能把内容写进当前文档的光标处——插入即时渲染、自动标脏、停笔 2 秒自动落盘,像它自己在写。

- 对话式交互:面板里下指令(如「在文末续写一段总结」),AI 流式回复,正文经 Insert 工具写进文档光标处,连续插入按顺序落位
- 对话正文支持选择文字、代码高亮与单独复制、Markdown 表格及 KaTeX 公式；流式输出时上翻阅读保留位置，可一键回到最新消息。
- 聊天页面确认渲染完成后才切换到排版视图；加载或渲染失败时保留可读的近期对话，重开窗口重新同步消息。兼容一次性返回正文的模型响应。
- 「优先 LaTeX 风格」也适用于启用期间发起的新 AI 回复。每条回复保存当时的开关状态，之后关闭或重启软件仍保留已有公式的排版。会话目录自动创建，以原子替换方式保存记录。
- 空白会话显示折页图案、时段问候与常用写作入口；卡片随窗口宽度切换为两列或三列，小窗口可滚动。
- AI 通过 ReadDocument 按需读取当前 Markdown 或 PDF，打开助手本身不会发送正文或主动分析。Markdown 读取编辑器当前内容，包含未保存的修改；PDF 支持分页文字及页面图像，扫描页和图表需要模型支持图像输入。每次读取返回范围和后续位置。请求绑定发起时的标签，切换文件后旧请求不能读取或写入另一份文档；PDF 始终只读。
- 供应商在「AI → AI 供应商」或「设置 → AI 供应商」里配置(协议 Anthropic/OpenAI、模型、API 地址、Key、1M 上下文窗口、回复上限),可一键从 MS-Agent 的 `~/.ms-agent/providers.json` 导入副本
- Mswrite 的写作能力(KaTeX/图表/`<font>` 颜色/任务列表/脚注等什么渲染得好)已写成技能注入系统提示,AI 写出来的内容直接符合本软件的渲染习惯
- 「设置 → AI 技能目录」和助手的设置菜单可指定技能目录。递归加载 Markdown 文件，包含 `技能名/SKILL.md`，修改后下次提问生效。内置 PDF 翻译和分析技能位于程序旁 `skills/`；更新程序保留已有技能文件。最多读取 32 个文件，每个不超过 128 KiB，合计约 120000 字符。技能作为模型指令使用，不执行本地脚本。
- 附件入口位于输入框左侧；中文输入法预编辑时隐藏占位提示，确认候选词不会误发送消息。
- 「帮助」中的快捷键窗口支持按操作或按键搜索；关于窗口展示版本、编辑和阅读能力，支持浅色、夜间主题。
- 写入文档的内容走编辑器桥,与手动输入同一条路(即时渲染 + 自动保存);面板回复轮末重渲染为 Markdown

## 快捷键

> 编辑区焦点在 WebView2 内部,键盘事件不经过 Qt —— 菜单快捷键只在焦点位于
> 搜索框/标签栏等 Qt 控件时由 Qt 处理,编辑区内一律走原生加速键表与页面键位,
> 两处都已对齐(下表在编辑区内均可用)。

| 按键 | 功能 | 按键 | 功能 |
| ---- | ---- | ---- | ---- |
| Ctrl+N | 新建标签页 | Ctrl+P | 快速打开 |
| Ctrl+O / Ctrl+Shift+O | 打开文件 / 文件夹 | Ctrl+W | 关闭标签 |
| Ctrl+S / Ctrl+Shift+S | 保存 / 另存为 | Ctrl+Q | 退出 |
| Ctrl+/ | 源码模式切换 | Ctrl+F | 搜索 |
| Ctrl+H | 替换 | Ctrl+Shift+F | 全局搜索 |
| Ctrl+= / - / 0 | 页面缩放 放大/缩小/重置 | Ctrl+Z / Y | 撤销 / 重做 |
| Ctrl+滚轮 | 页面缩放(右下角百分比,点击回 100%) | Ctrl+Alt+= / - / 0 | 字号增大/缩小/重置 |
| Ctrl+B / I / U / K | 粗体 / 斜体 / 下划线 / 链接 | Ctrl+1~4 | 标题 1~4 |
| Ctrl+Shift+K | 代码块 | Ctrl+Shift+` | 行内代码 |
| Ctrl+Shift+M / Q | 公式 / 引用 | Ctrl+Shift+[ / ] | 有序 / 无序列表 |
| Ctrl+T | 插入表格 | Ctrl+Shift+E/P/W | 导出 HTML/PDF/Word |
| Ctrl+Shift+T | 重开已关闭的文件 | Ctrl+, | 偏好设置 |
| Ctrl+Shift+1 / 2 | 大纲面板 / 文档列表 | F3 / Shift+F3 | 查找下一个 / 上一个 |
| Ctrl+Shift+I | 插入图片 | Ctrl+Shift+C | 复制为 Markdown |
| Ctrl+Shift+V | 粘贴为纯文本 | Alt+Shift+5 | 删除线 |
| Ctrl+L / Ctrl+E | 选中当前块 | Ctrl+D / Ctrl+Shift+D | 选词 / 删除词 |
| Ctrl+\ | 清除格式 | Ctrl+J | 跳转到所选内容 |
| F11 / Shift+F12 | 全屏 / 开发者工具 | Ctrl+Shift+↑ / ↓ | 标题升级 / 降级 |
| F9 | AI 写作助手面板 | Enter / Shift+Enter(面板内) | 发送 / 换行 |

## 目录结构

```
Mswrite/
├─ CMakeLists.txt
├─ src/                  # 全部原生 C++(Qt6)
│  ├─ main.cpp
│  ├─ webviewhost.*      # WebView2 宿主(COM 封装/共享环境/PDF 打印)
│  ├─ mainwindow.*       # 主窗口:多标签/菜单/导出/搜索接线
│  ├─ bridge.*           # C++ -> JS 调用构造(JSON 转义)
│  ├─ fileservice.*      # 读写/最近文件/偏好
│  ├─ outlinedock.*      # 大纲侧栏
│  ├─ searchdock.*       # 全文搜索面板
│  └─ quickopendialog.*  # Ctrl+P 快速打开
├─ resources/
│  ├─ web/editor.html    # 编辑页
│  ├─ web/bridge.js      # JS 桥 + Vditor 配置
│  ├─ web/themes/        # light / dark / paper
│  └─ web/vditor/dist/   # Vditor 3.11.3 内核(本地)
├─ third_party/webview2/ # WebView2 SDK:WebView2.h + Loader.dll(≈3MB)
└─ scripts/package.ps1   # 便携版打包
```

## 安装红线合规

- 唯一引入的第三方文件:WebView2 SDK 两个文件(≈3MB)+ Vditor dist(≈24MB),全部在**工程目录内**
- WebView2 运行时使用系统自带版本(v153),零安装
- 程序数据(`webview-data/`、`export-tmp/`)在 exe 同目录;用户偏好存注册表 HKCU(几 KB)
- 图片存文档同级 `assets/` 目录(相对路径引用,随文档移动)
- 曾用于查询的临时文件与 aqtinstall 小工具均已清理,零残留

## 已知限制与后续计划

- 当前发布版统一放在 `dist/Mswrite/Mswrite.exe`。打包脚本更新同一目录并保留用户数据，避免积累多个完整版本。
- 从文件启动时直接打开目标文档。Markdown 在 WebView2 初始化期间先显示可阅读的正文预览，就绪后自动切到编辑器；首次内容与主题一起初始化，避免二次填充。
- 标签、大纲、菜单、状态栏和 PDF 工具栏统一浅色/深色外观。AI 对话支持可选文字、Markdown、代码复制、KaTeX 与保留阅读位置的流式更新。
- PDF 支持文字层选择、复制、搜索和目录导航；扫描图片仍需 OCR 才能选择文字。目前 PDF 为阅读模式。

- 文字颜色:文件里存 `<font color="...">` 标记(可移植、换编辑器也能读)。
  Vditor 的 IR 内核不渲染行内 HTML,所以编辑器里由显示层把标记隐藏、给文字上色
  —— 屏幕上看到的就是颜色本身,重开文档颜色仍在;标记文本只是视觉隐藏,
  markdown 往返不受影响
- 标题的 `#` 与 H1~H6 徽标只在光标落在该标题里时显示,其余时候保持干净排版
- Word 导出目前是 Word 兼容 HTML(.doc);后续可升级 python-docx 纯文档导出
- 二期候选:PicGo 图床上传、设置面板(快捷键自定义)、更多主题
- 极大文档(>10MB)性能未做专项优化
