@echo off
rem Builds SpeedBreakerLauncher.exe with the C# compiler that ships with Windows (.NET Framework 4.x).
setlocal
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
cd /d "%~dp0"
"%CSC%" /nologo /optimize+ /target:winexe /out:SpeedBreakerLauncher.exe /reference:System.Windows.Forms.dll /reference:System.Drawing.dll SpeedBreakerLauncher.cs
if errorlevel 1 (echo Build failed. & exit /b 1)
echo Built %~dp0SpeedBreakerLauncher.exe
