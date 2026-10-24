; Vitool Box installer (Inno Setup 6).
;
; Per-user install without administrator rights. Uninstalling removes Vitool
; Box and its cache, but leaves the apps installed through it in place.
;
; Built by build.ps1, which passes /DBuildDir=<folder with VitoolBox.exe>.

#define AppVersion "1.0.0"
#define AppName "Vitool Box"

#ifndef BuildDir
  #define BuildDir "..\build\app"
#endif

[Setup]
AppId={{E4A7C2D9-5B13-4F86-9C0A-3D8E1F6B2A75}
AppName={#AppName}
AppVerName={#AppName} {#AppVersion}
AppVersion={#AppVersion}
AppPublisher=VitomDev
AppPublisherURL=https://github.com/VitomDev/Vitool-Box
AppSupportURL=https://github.com/VitomDev/Vitool-Box/issues
AppCopyright=Copyright (c) 2026 VitomDev
VersionInfoVersion={#AppVersion}
VersionInfoCompany=VitomDev
VersionInfoDescription={#AppName} - All VitomDev apps in one place Setup
DefaultDirName={localappdata}\Programs\{#AppName}
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputBaseFilename=VitoolBoxSetup
SetupIconFile=..\assets\vitoolbox.ico
UninstallDisplayIcon={app}\VitoolBox.exe
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes

[Files]
Source: "{#BuildDir}\VitoolBox.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\ui\*"; DestDir: "{app}\ui"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; Files of an older UI that the new one no longer has.
Type: filesandordirs; Name: "{app}\ui"

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\VitoolBox.exe"

[Run]
Filename: "{app}\VitoolBox.exe"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\{#AppName}"
