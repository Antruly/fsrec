; fsrec — Inno Setup 安装脚本（简体中文安装向导）
; 编译：ISCC.exe packaging\fsrec.iss   （或运行 scripts\build_release.ps1）

#define MyAppName "fsrec"
#define MyAppVersion "1.0.11"
#define MyAppPublisher "fsrec"
#define MyAppExeName "recovery_server.exe"

[Setup]
AppId={{6F2A8C1D-3E4B-4A5C-9D7E-2B1F0A8C6E4D}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\release
OutputBaseFilename=fsrec_Setup_{#MyAppVersion}
SetupIconFile=..\resources\fsrec.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
PrivilegesRequired=admin
LicenseFile=..\docs\免责声明.txt

[Languages]
Name: "chinesesimplified"; MessagesFile: "languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："

[Files]
Source: "..\build\Release\recovery_server.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\uvcpp.dll";         DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\uv.dll";           DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\llhttp.dll";       DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\libssl-3-x64.dll";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\libcrypto-3-x64.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\frontend\dist\*"; DestDir: "{app}\frontend\dist"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\docs\*";          DestDir: "{app}\docs";          Flags: ignoreversion recursesubdirs
Source: "..\start.bat";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\stop.bat";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\resources\fsrec.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\vc++\vcredist_vs2022_x64.exe"; DestDir: "{app}\vc++"; Flags: ignoreversion

[Dirs]
Name: "{app}\data"

[Icons]
Name: "{group}\fsrec"; Filename: "{app}\start.bat"; WorkingDir: "{app}"; IconFilename: "{app}\fsrec.ico"
Name: "{group}\fsrec 使用说明"; Filename: "notepad.exe"; Parameters: """{app}\docs\使用说明.md"""
Name: "{group}\停止 fsrec"; Filename: "{app}\stop.bat"; WorkingDir: "{app}"; IconFilename: "{app}\fsrec.ico"
Name: "{autodesktop}\fsrec"; Filename: "{app}\start.bat"; Tasks: desktopicon; WorkingDir: "{app}"; IconFilename: "{app}\fsrec.ico"

[Run]
Filename: "{app}\{#MyAppExeName}"; Parameters: "--port {code:GetPort}"; Description: "立即启动 fsrec"; Flags: postinstall nowait skipifsilent runascurrentuser

[Code]
var
  WarnPage: TInputOptionWizardPage;
  PortPage: TInputQueryWizardPage;
  SelectedPort: Integer;

function PortInUse(Port: Integer): Boolean;
var
  ResultCode: Integer;
  PsFile, OutFile, Script: String;
  Line: AnsiString;
begin
  Result := False;
  PsFile := ExpandConstant('{tmp}\fsrec_portcheck.ps1');
  OutFile := ExpandConstant('{tmp}\fsrec_portcheck.txt');
  Script :=
    '$p = ' + IntToStr(Port) + #13#10 +
    'try {' + #13#10 +
    '  $c = New-Object System.Net.Sockets.TcpClient' + #13#10 +
    '  $c.Connect("127.0.0.1", $p)' + #13#10 +
    '  $c.Close()' + #13#10 +
    '  "INUSE"' + #13#10 +
    '} catch {' + #13#10 +
    '  "FREE"' + #13#10 +
    '}';
  SaveStringToFile(PsFile, Script, False);
  Exec(ExpandConstant('{cmd}'),
       '/C powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + PsFile + '" > "' + OutFile + '"',
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  if LoadStringFromFile(OutFile, Line) then
    Result := (Pos('INUSE', Line) > 0);
end;

function GetPort(Param: String): String;
begin
  Result := IntToStr(SelectedPort);
end;

procedure InitializeWizard();
begin
  SelectedPort := 8080;

  WarnPage := CreateInputOptionPage(
    wpLicense,
    '重要安全提示',
    '请勿安装到需要恢复数据的磁盘',
    'fsrec 用于从磁盘恢复已删除/丢失的数据。安装程序本身会向磁盘写入文件，'
    + '如果把 fsrec 安装到需要恢复数据的磁盘（分区）上，安装过程很可能会覆盖掉'
    + '待恢复的数据，导致数据永久丢失。'
    + #13#10#13#10
    + '因此，请务必把 fsrec 安装到一个【与待恢复磁盘不同】的磁盘或分区。',
    False, False);
  WarnPage.Add('我已知晓风险，并确认不会将 fsrec 安装到需要恢复数据的磁盘/分区。');

  PortPage := CreateInputQueryPage(
    WarnPage.ID,
    '服务端口设置',
    '选择 fsrec 服务端口',
    'fsrec 启动后通过该端口提供网页界面，请设置一个未被占用的端口（默认 8080）。');
  PortPage.Add('服务端口：', False);
  PortPage.Values[0] := '8080';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Port: Integer;
begin
  Result := True;
  if CurPageID = WarnPage.ID then
  begin
    if not WarnPage.Values[0] then
    begin
      MsgBox('请先勾选确认框，表示您已知晓并同意不将 fsrec 安装到需要恢复数据的磁盘。',
             mbError, MB_OK);
      Result := False;
    end;
  end
  else if CurPageID = PortPage.ID then
  begin
    Port := StrToIntDef(Trim(PortPage.Values[0]), 0);
    if (Port < 1) or (Port > 65535) then
    begin
      MsgBox('端口必须是 1–65535 之间的整数。', mbError, MB_OK);
      Result := False;
    end
    else if PortInUse(Port) then
    begin
      MsgBox('端口 ' + IntToStr(Port) + ' 已被占用，请选择其他端口。', mbError, MB_OK);
      Result := False;
    end
    else
      SelectedPort := Port;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    SaveStringToFile(ExpandConstant('{app}\port.txt'), IntToStr(SelectedPort), False);
end;
