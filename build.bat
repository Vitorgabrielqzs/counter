@echo off
rem Compila o Counter.exe (C++ nativo) nesta pasta.
rem Requer o Visual Studio com a carga "Desenvolvimento para desktop com C++".
setlocal
cd /d "%~dp0"

rem Acha o Visual Studio que tem o compilador C++ instalado
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio com C++ nao encontrado. Instale a carga "Desenvolvimento para desktop com C++".
    exit /b 1
)
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul

rem Ícone (Counter.rc -> Counter.res, embutido no .exe)
rc /nologo Counter.rc || exit /b 1

rem /O1 /Os = otimiza para tamanho pequeno; /MT = sem dependencia de DLLs do Visual C++;
rem /GR- /EHs-c- = sem RTTI nem excecoes (nao usadas); /SUBSYSTEM:WINDOWS = sem console
cl /nologo /std:c++17 /O1 /Os /MT /GR- /EHs-c- /W4 /utf-8 /DUNICODE /D_UNICODE Counter.cpp Counter.res /Fe:Counter.exe ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF user32.lib shell32.lib gdi32.lib advapi32.lib d2d1.lib dwrite.lib
set RESULT=%ERRORLEVEL%
del Counter.obj Counter.res 2>nul
exit /b %RESULT%
