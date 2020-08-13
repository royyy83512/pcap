call C:\WinDDK\7600.16385.1\bin\setenv.bat C:\WinDDK\7600.16385.1\ fre x64 WIN7

if "%_BUILDARCH%"=="x86" (
    set USBPcap_arch=i386
    if "%DDK_TARGET_OS%"=="WinLH" (
        set USBPcap_OS=Vista_X86
    ) else if "%DDK_TARGET_OS%"=="WinXP" (
        set USBPcap_OS=XP_X86
    ) else if "%DDK_TARGET_OS%"=="Win7" (
        set USBPcap_OS=7_X86
    )
) else (
    set USBPcap_arch=amd64
    if "%DDK_TARGET_OS%"=="WinLH" (
        set USBPcap_OS=Vista_X64
    ) else if "%DDK_TARGET_OS%"=="WinXP" (
        set USBPcap_OS=XP_X64
    ) else if "%DDK_TARGET_OS%"=="Win7" (
        set USBPcap_OS=7_X64
    )
)

cd %~dp0
CALL config.bat

build -ceZg
if exist build%BUILD_ALT_DIR%.err goto error

%_USBPCAP_SIGNTOOL% %_USBPCAP_SIGN_OPTS_SHA256% USBPcapDriver\obj%BUILD_ALT_DIR%\%USBPcap_arch%\USBPcap.sys
if errorlevel 1 goto error

Inf2cat.exe /driver:USBPcapDriver\obj%BUILD_ALT_DIR%\%USBPcap_arch%\ /os:%USBPcap_OS%

goto end

:error
echo ===== BUILD FAILED! =====
cd %~dp0
pause
exit /B 1

:end
cd %~dp0
pause
exit /B 0
