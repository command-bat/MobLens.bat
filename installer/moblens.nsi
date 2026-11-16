; MobLens - instalador (NSIS)
; Gerado por ./build.sh --installer  ->  dist/MobLens-Setup.exe
Unicode true
!include "MUI2.nsh"
!include "x64.nsh"

!define APP "MobLens"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\MobLens"
!ifndef VERSION
  !define VERSION "1.0.0"
!endif

Name "${APP}"
OutFile "..\dist\MobLens-Setup.exe"
InstallDir "$PROGRAMFILES64\MobLens"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "${APP} ${VERSION}"

!define MUI_ICON "..\icons\moblens.ico"
!define MUI_UNICON "..\icons\moblens.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\MobLens.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Abrir o MobLens"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "PortugueseBR"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "O MobLens requer Windows 11 de 64 bits."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Section "MobLens"
  ; Fecha uma versão em execução
  nsExec::Exec 'taskkill /F /IM MobLens.exe'
  nsExec::Exec 'taskkill /F /IM adb.exe'
  Sleep 500

  ; A DLL da webcam fica carregada pelo serviço de câmera do Windows
  nsExec::Exec 'net stop FrameServerMonitor /y'
  nsExec::Exec 'net stop FrameServer /y'
  Sleep 500

  SetOutPath "$INSTDIR"
  ; Se ainda estiver em uso, renomeia (permitido com a DLL carregada)
  Delete "$INSTDIR\moblens_vcam.dll.old"
  Delete "$INSTDIR\moblens_vcam.dll"
  IfFileExists "$INSTDIR\moblens_vcam.dll" 0 dll_free
    Rename "$INSTDIR\moblens_vcam.dll" "$INSTDIR\moblens_vcam.dll.old"
    Delete /REBOOTOK "$INSTDIR\moblens_vcam.dll.old"
  dll_free:
  File "..\dist\MobLens\MobLens.exe"
  File "..\dist\MobLens\moblens_vcam.dll"
  File "..\dist\MobLens\adb.exe"
  File "..\dist\MobLens\AdbWinApi.dll"
  File "..\dist\MobLens\AdbWinUsbApi.dll"
  File "..\dist\MobLens\scrcpy-server"
  File "..\dist\MobLens\LICENSE-scrcpy.txt"

  ; Registra a webcam virtual
  DetailPrint "Registrando a webcam virtual MobLens..."
  ExecWait '"$INSTDIR\MobLens.exe" --install' $0
  ${If} $0 != 0
    MessageBox MB_ICONEXCLAMATION "Não foi possível registrar a webcam virtual (código $0).$\nVeja %LOCALAPPDATA%\MobLens\moblens.log"
  ${EndIf}

  CreateShortcut "$SMPROGRAMS\MobLens.lnk" "$INSTDIR\MobLens.exe"
  CreateShortcut "$DESKTOP\MobLens.lnk" "$INSTDIR\MobLens.exe"

  WriteUninstaller "$INSTDIR\Desinstalar.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "MobLens"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\MobLens.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Desinstalar.exe"'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /F /IM MobLens.exe'
  nsExec::Exec 'taskkill /F /IM adb.exe'
  Sleep 500
  DetailPrint "Removendo a webcam virtual MobLens..."
  ExecWait '"$INSTDIR\MobLens.exe" --uninstall'
  nsExec::Exec 'net stop FrameServerMonitor /y'
  nsExec::Exec 'net stop FrameServer /y'
  Sleep 500

  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "MobLens"
  DeleteRegKey HKCU "Software\MobLens"
  Delete "$SMPROGRAMS\MobLens.lnk"
  Delete "$DESKTOP\MobLens.lnk"

  Delete "$INSTDIR\MobLens.exe"
  Delete /REBOOTOK "$INSTDIR\moblens_vcam.dll"
  Delete /REBOOTOK "$INSTDIR\moblens_vcam.dll.old"
  Delete "$INSTDIR\adb.exe"
  Delete "$INSTDIR\AdbWinApi.dll"
  Delete "$INSTDIR\AdbWinUsbApi.dll"
  Delete "$INSTDIR\scrcpy-server"
  Delete "$INSTDIR\LICENSE-scrcpy.txt"
  Delete "$INSTDIR\Desinstalar.exe"
  RMDir /REBOOTOK "$INSTDIR"
  DeleteRegKey HKLM "${UNINST_KEY}"
SectionEnd
