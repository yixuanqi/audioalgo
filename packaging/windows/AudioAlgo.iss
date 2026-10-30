; AudioAlgo Windows installer script (Inno Setup 6)
; Consumed by .github/workflows/windows-build.yml with /D parameters:
;   /DAppVersion  e.g. 0.1.0
;   /DSourceDir   portable staging directory (AudioAlgoEditor.exe at its root)
;   /DOutputDir   directory that receives the setup executable

#ifndef AppVersion
#define AppVersion "0.0.0"
#endif

#ifndef SourceDir
#define SourceDir "dist\portable"
#endif

#ifndef OutputDir
#define OutputDir "dist"
#endif

[Setup]
AppId={{7C1E5F84-9A2B-4E0D-B6A3-1F2C8D45E9A1}
AppName=AudioAlgo
AppVersion={#AppVersion}
AppPublisher=AudioAlgo
DefaultDirName={autopf}\AudioAlgo
DefaultGroupName=AudioAlgo
DisableProgramGroupPage=yes
OutputBaseFilename=AudioAlgo-{#AppVersion}-windows-x64-setup
OutputDir={#OutputDir}
Compression=lzma2/max
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
UninstallDisplayName=AudioAlgo
UninstallDisplayIcon={app}\AudioAlgoEditor.exe

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\AudioAlgo Editor"; Filename: "{app}\AudioAlgoEditor.exe"
Name: "{group}\AudioAlgo CLI"; Filename: "{app}\audio_cli.exe"; Parameters: "--help"
Name: "{autodesktop}\AudioAlgo Editor"; Filename: "{app}\AudioAlgoEditor.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\AudioAlgoEditor.exe"; Description: "{cm:LaunchProgram,AudioAlgo Editor}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Remove user-authored scratch state left inside the install directory.
Type: filesandordirs; Name: "{app}\examples"
