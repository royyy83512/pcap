cd %~dp0

::Remove the Release directory if it exists
if exist Release RMDIR /S /Q Release

::build for Windows 7
mkdir Release\Windows7\x86
mkdir Release\Windows7\x64

call cmd.exe /c driver_build.bat x86 WIN7 Release\Windows7\x86
call cmd.exe /c driver_build.bat x64 WIN7 Release\Windows7\x64

::Copy the USBPcapCMD.exe
copy USBPcapCMD\objfre_win7_x86\i386\USBPcapCMD.exe Release\USBPcapCMD_x86.exe
copy USBPcapCMD\objfre_win7_amd64\amd64\USBPcapCMD.exe Release\USBPcapCMD_x64.exe

::Build for Windows 8
mkdir Release\Windows8\x86
mkdir Release\Windows8\x64

call cmd.exe /c driver_build_win8.bat x86 Release\Windows8\x86
call cmd.exe /c driver_build_win8.bat x86_amd64 Release\Windows8\x64

::Create Attestation signing cabinet files
call cmd.exe /c driver_attestation_signing.bat
@echo off
echo Attestation signing packages should be available in disk1 directory
echo Sign these files with EV Code Signing certificate and upload them
echo to Hardware Dev Center Dashboard
echo After you are ready, unzip signed files to Release directory
@echo on
pause

::Build finished

@echo off
echo Build finished. Please check if Release directory contains all required
echo files. Now you can build the installer using nsis\USBPcap.nsi
@echo on

pause
