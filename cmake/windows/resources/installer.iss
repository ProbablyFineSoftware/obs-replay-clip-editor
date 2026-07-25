; Inno Setup script for the Replay Clip Editor OBS plugin.
;
; Invoked by .github/scripts/Package-Windows.ps1, which passes the version, the
; packaged source directory, and the output name via /D command-line defines.
;
; The plugin installs into OBS Studio's per-machine plugin folder
; (C:\ProgramData\obs-studio\plugins). That location needs no OBS-path detection,
; works no matter where OBS itself is installed, and survives OBS updates.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\..\release\Release"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\..\release"
#endif
#ifndef OutputBaseFilename
  #define OutputBaseFilename "obs-replay-clip-editor-windows-x64"
#endif

#define AppName "Replay Clip Editor"
#define AppPublisher "Probably Fine Software"
#define AppUrl "https://github.com/ProbablyFineSoftware/obs-replay-clip-editor"
#define PluginFolder "obs-replay-clip-editor"

[Setup]
AppId={{9E7B4C21-3A5D-4F80-B1C6-2D9A7F0E5B34}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}
VersionInfoVersion={#AppVersion}
DefaultDirName={commonappdata}\obs-studio\plugins\{#PluginFolder}
DisableDirPage=yes
DisableProgramGroupPage=yes
UsePreviousAppDir=no
PrivilegesRequired=admin
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName} {#AppVersion}

[Messages]
WelcomeLabel2=This installs {#AppName} for OBS Studio.%n%nPlease close OBS Studio before continuing, then click Next.

[Files]
Source: "{#SourceDir}\{#PluginFolder}\bin\64bit\obs-replay-clip-editor.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#SourceDir}\{#PluginFolder}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs
