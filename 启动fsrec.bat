@echo off
rem ============================================================
rem  fsrec - NTFS 数据恢复工具 一键启动
rem  recovery_server.exe 已内嵌 requireAdministrator 清单,
rem  用 start 启动时系统会自动弹出 UAC 提权,无需脚本自提权。
rem ============================================================

cd /d "%~dp0"

rem 读取安装时选择的端口(默认 8080)
set "PORT=8080"
if exist "%~dp0port.txt" set /p PORT=<"%~dp0port.txt"

rem 检查 VC++ 运行库:缺失则静默安装(需管理员权限,弹 UAC)
if exist "%SystemRoot%\System32\vcruntime140_1.dll" goto vc_ok
echo [fsrec] 检测到缺少 VC++ 运行库,正在安装(请在 UAC 提示中允许)...
powershell -NoProfile -Command "Start-Process -FilePath '%~dp0vc++\vcredist_vs2022_x64.exe' -ArgumentList '/install','/quiet','/norestart' -Verb RunAs -Wait"
:vc_ok

rem 避免重复启动
tasklist /FI "IMAGENAME eq recovery_server.exe" 2>nul | find /I "recovery_server.exe" >nul
if not errorlevel 1 (
    echo [fsrec] 服务已在运行,正在打开浏览器...
    start "" http://localhost:%PORT%/
    exit /b 0
)

echo 正在启动 fsrec (端口 %PORT%,请在 UAC 提示中点击"是"以授予管理员权限)...
start "" "%~dp0recovery_server.exe" --port %PORT%
exit /b 0
