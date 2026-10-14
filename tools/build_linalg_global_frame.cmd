@echo off
setlocal
cd /d "%~dp0.."
powershell -NoProfile -Command "if(Get-Process linalg_global_frame,linalg_batch_frame,linalg_batch_graph,linalg_attention_unit -ErrorAction SilentlyContinue){exit 1}"
if errorlevel 1 exit /b 1
set "CVBASE=work\b580\cooperative-vectors\linalg7213\window-normalization-shapes"
set "CVOUT=work\b580\cooperative-vectors\linalg7213\global-b1"
if "%~1"=="int8-study" set "CVOUT=work\b580\cooperative-vectors\linalg7213\global-int8-study"
set "CVSDK=work\b580\cooperative-vectors\agility7213\build\native"
if not exist "%CVOUT%\D3D12" mkdir "%CVOUT%\D3D12"
copy /y "%CVBASE%\D3D12\*.dll" "%CVOUT%\D3D12\" >nul
if errorlevel 1 exit /b 1
copy /y "%CVBASE%\*.dxil" "%CVOUT%\" >nul
if errorlevel 1 exit /b 1
copy /y src\native\global_frame.hlsl "%CVOUT%\" >nul
for /f "usebackq tokens=*" %%V in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "NRVS=%%V"
call "%NRVS%\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /std:c++17 /EHsc /W4 /O2 /I"%CVSDK%\include" /Iwork\optiscaler-source\external\nvngx_dlss_sdk src\native\linalg_global_frame.cpp /Fo:"%CVOUT%\global.obj" /Fe:"%CVOUT%\linalg_global_frame.exe" /link d3d12.lib dxgi.lib directml.lib d3dcompiler.lib
exit /b %errorlevel%
