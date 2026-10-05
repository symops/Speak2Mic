@echo off
rem Builds Speak2Mic.exe, Speak2Mic-Setup.exe, s2minstall.exe and s2mdebug.exe with MSVC. Run from a "x64 Native Tools Command Prompt for VS 2022".
cd /d "%~dp0"
rc /nologo s2mpanel.rc || exit /b 1
cl /nologo /O2 /EHs-c- /GR- /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /FeSpeak2Mic.exe s2mpanel.cpp mp3player.cpp audio.cpp applog.cpp lang.cpp audiosvc.cpp devctl.cpp s2mpanel.res ^
   ole32.lib avrt.lib mfplat.lib mfreadwrite.lib mfuuid.lib propsys.lib bcrypt.lib comctl32.lib comdlg32.lib setupapi.lib shell32.lib gdi32.lib advapi32.lib user32.lib /link /SUBSYSTEM:WINDOWS || exit /b 1
rc /nologo s2minstall.rc || exit /b 1
cl /nologo /O2 /EHs-c- /GR- /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 s2minstall.cpp audiosvc.cpp devctl.cpp setupcore.cpp setupfiles.cpp diag.cpp ksprobe.cpp applog.cpp lang.cpp s2minstall.res wevtapi.lib setupapi.lib newdev.lib crypt32.lib advapi32.lib ole32.lib shell32.lib || exit /b 1
rc /nologo s2msetup.rc || exit /b 1
cl /nologo /O2 /EHs-c- /GR- /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /FeSpeak2Mic-Setup.exe s2msetup.cpp audiosvc.cpp devctl.cpp setupcore.cpp setupfiles.cpp diag.cpp ksprobe.cpp applog.cpp lang.cpp s2msetup.res wevtapi.lib ^
   setupapi.lib newdev.lib crypt32.lib comctl32.lib shell32.lib gdi32.lib advapi32.lib user32.lib ole32.lib /link /SUBSYSTEM:WINDOWS || exit /b 1
rc /nologo s2mdebug.rc || exit /b 1
cl /nologo /O2 /EHs-c- /GR- /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 s2mdebug.cpp audio.cpp audiosvc.cpp devctl.cpp setupcore.cpp setupfiles.cpp diag.cpp ksprobe.cpp applog.cpp lang.cpp s2mdebug.res ^
   wevtapi.lib setupapi.lib newdev.lib crypt32.lib advapi32.lib ole32.lib shell32.lib avrt.lib propsys.lib || exit /b 1
rc /nologo s2mautotest.rc || exit /b 1
cl /nologo /O2 /EHs-c- /GR- /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 s2mautotest.cpp mp3player.cpp audio.cpp audiosvc.cpp devctl.cpp setupcore.cpp setupfiles.cpp diag.cpp ksprobe.cpp applog.cpp lang.cpp s2mautotest.res ^
   wevtapi.lib setupapi.lib newdev.lib crypt32.lib advapi32.lib ole32.lib wtsapi32.lib mfplat.lib mfreadwrite.lib mfuuid.lib bcrypt.lib shell32.lib avrt.lib propsys.lib || exit /b 1
rc /nologo s2mctl.rc || exit /b 1
cl /nologo /O2 /EHs-c- /GR- /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 s2mctl.cpp audio.cpp audiosvc.cpp devctl.cpp setupcore.cpp setupfiles.cpp diag.cpp ksprobe.cpp applog.cpp lang.cpp s2mctl.res ^
   wevtapi.lib setupapi.lib newdev.lib crypt32.lib advapi32.lib ole32.lib shell32.lib avrt.lib propsys.lib || exit /b 1
echo built: Speak2Mic.exe Speak2Mic-Setup.exe s2minstall.exe s2mdebug.exe s2mautotest.exe s2mctl.exe
