; Mswrite 安装脚本(Inno Setup 6)
; 用法:ISCC.exe installer\Mswrite.iss
; 或直接跑 scripts\make-installer.ps1(会自动刷新 dist 并编译)
;
; 安装形态:免管理员,装到 %LOCALAPPDATA%\Programs\Mswrite。
; 原因:程序会在自身目录旁写 WebView2 缓存(webview-data)和用户文档
; (MSWriteData),用户目录下没有权限问题;装 Program Files 需要额外授权。

#define AppName "Mswrite"
#define AppVersion "2.2.1"
#define AppPublisher "moshuai"
#define AppExe "Mswrite.exe"
; 安装源:由 make-installer.ps1 准备的干净暂存目录(已剔除运行期产物:
; webview-data / export-tmp / MSWriteData / mswrite.log)
#ifndef StageDir
#define StageDir "..\dist\_stage"
#endif

[Setup]
AppId={{DB969F9E-765B-4F8A-9271-5B2DCE819775}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}.0
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} Setup
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\Mswrite
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename={#AppName}-{#AppVersion}-setup
SetupIconFile=..\logo.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
AllowNoIcons=yes

[Languages]
; 安装向导使用简体中文(与软件界面一致)。isl 取自 Inno 官方仓库
; (Files/Languages/ChineseSimplified.isl, 6.5.0+ 版翻译),随工程分发;
; 本机编译器较旧时,isl 里多出的键会被忽略并回落英文。
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; 整目录安装(exe + Qt 运行库 + QML/插件 + resources/skills + WebView2Loader)
Source: "{#StageDir}\*"; DestDir: "{app}"; Excludes: "ai-providers*.json,providers.json,ai-session*.json,auth.json,credentials*,.env*,mswrite.log*,ai-doctor.txt,ai-smoke.txt,MSWriteData\*,webview-data\*,export-tmp\*"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 卸载清掉运行期缓存/临时文件;MSWriteData(用户保存的文档)保留不删
Type: filesandordirs; Name: "{app}\webview-data"
Type: filesandordirs; Name: "{app}\export-tmp"
Type: files; Name: "{app}\mswrite.log"

[Code]
// 安装前检查 WebView2 运行时(Win10/11 通常自带;缺失只提示不阻断)
function WebView2Installed(): Boolean;
var
  pv: String;
begin
  Result :=
    RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', pv) or
    RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', pv) or
    RegQueryStringValue(HKCU, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', pv);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  // 静默安装不弹窗(否则 /VERYSILENT 会被卡住)
  if (not WizardSilent()) and (not WebView2Installed()) then
    MsgBox('未检测到 Microsoft Edge WebView2 运行时。' + #13#10 +
           'Mswrite 的编辑区依赖它,缺少时界面会空白。' + #13#10 + #13#10 +
           '可继续安装,之后从微软官网安装 WebView2 Runtime 即可:' + #13#10 +
           'https://developer.microsoft.com/microsoft-edge/webview2/',
           mbInformation, MB_OK);
end;

// 卸载时告知用户文档保留位置
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    if (not UninstallSilent()) and DirExists(ExpandConstant('{app}\MSWriteData')) then
      MsgBox('你的文档保存在:' + #13#10 + ExpandConstant('{app}\MSWriteData') + #13#10 + #13#10 +
             '卸载不会删除它。如需彻底清理,请手动删除该文件夹。',
             mbInformation, MB_OK);
end;
