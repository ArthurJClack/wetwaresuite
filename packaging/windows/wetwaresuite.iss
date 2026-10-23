; wetwaresuite windows installer (Inno Setup 6, free).
; builds wetwaresuite-windows-x64-setup.exe from the folder the downloads
; workflow packs into the zip:
;   iscc /DAppVersion=0.2.0 /DStageDir=C:\path\to\wetwaresuite-windows-x64 /DOutputDir=C:\out wetwaresuite.iss
; installs for the current user by default (no admin prompt), adds a start menu
; folder with the launcher and every app (each opens in its own window, with its
; own icon), the file types each app opens ("Open with", and the default for
; wetwaresuite's own formats), optional desktop shortcuts and PATH entry, and a
; normal uninstaller under Settings > Apps.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\..\wetwaresuite-windows-x64"
#endif
#ifndef OutputDir
  #define OutputDir "..\.."
#endif

[Setup]
AppId={{6F1D2C4E-8A3B-4F7E-9C51-2B7D0E4A9F13}
AppName=wetwaresuite
AppVersion={#AppVersion}
AppVerName=wetwaresuite {#AppVersion}
AppPublisher=wetwaresuite
AppPublisherURL=https://github.com/ArthurJClack/wetwaresuite
AppSupportURL=https://github.com/ArthurJClack/wetwaresuite/issues
AppUpdatesURL=https://github.com/ArthurJClack/wetwaresuite/releases/latest
DefaultDirName={autopf}\wetwaresuite
DefaultGroupName=wetwaresuite
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\..\LICENSE
SetupIconFile=..\..\branding\wetwaresuite.ico
UninstallDisplayIcon={app}\wetwaresuite-gui.exe
UninstallDisplayName=wetwaresuite
OutputDir={#OutputDir}
OutputBaseFilename=wetwaresuite-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ChangesEnvironment=yes
CloseApplications=yes
ChangesAssociations=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "desktopapps"; Description: "a desktop shortcut for every app too"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "filetypes"; Description: "open files in the right app (adds each app to ""Open with"" for the files it opens)"; GroupDescription: "extras:"
Name: "addtopath"; Description: "use the apps from a terminal (adds them to PATH)"; GroupDescription: "extras:"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\wetwaresuite\wetwaresuite"; Filename: "{app}\wetwaresuite-gui.exe"; Comment: "pick an app: each one opens in its own window"
; one entry per app, same list as packaging/apps.tsv (the downloads workflow checks they match)
Name: "{autoprograms}\wetwaresuite\wetwaresuite image"; Filename: "{app}\wetwaresuite-image.exe"; Comment: "photos, painting and textures"
Name: "{autoprograms}\wetwaresuite\wetwaresuite draw"; Filename: "{app}\wetwaresuite-draw.exe"; Comment: "vector art, fonts and whiteboards"
Name: "{autoprograms}\wetwaresuite\wetwaresuite model"; Filename: "{app}\wetwaresuite-model.exe"; Comment: "3D modeling, renders and animation"
Name: "{autoprograms}\wetwaresuite\wetwaresuite game"; Filename: "{app}\wetwaresuite-game.exe"; Comment: "game assets and levels"
Name: "{autoprograms}\wetwaresuite\wetwaresuite cad"; Filename: "{app}\wetwaresuite-cad.exe"; Comment: "engineering, drafting and 3D printing"
Name: "{autoprograms}\wetwaresuite\wetwaresuite electronics"; Filename: "{app}\wetwaresuite-electronics.exe"; Comment: "circuits and microcontrollers"
Name: "{autoprograms}\wetwaresuite\wetwaresuite sound"; Filename: "{app}\wetwaresuite-sound.exe"; Comment: "audio editing and music"
Name: "{autoprograms}\wetwaresuite\wetwaresuite video"; Filename: "{app}\wetwaresuite-video.exe"; Comment: "video editing and screen capture"
Name: "{autoprograms}\wetwaresuite\wetwaresuite office"; Filename: "{app}\wetwaresuite-office.exe"; Comment: "documents, sheets, slides and PDFs"
Name: "{autoprograms}\wetwaresuite\wetwaresuite maps"; Filename: "{app}\wetwaresuite-maps.exe"; Comment: "offline maps and GIS"
Name: "{autoprograms}\wetwaresuite\wetwaresuite system"; Filename: "{app}\wetwaresuite-system.exe"; Comment: "calculator, code editor, files, terminal, monitor, clock"
Name: "{autoprograms}\wetwaresuite\wetwaresuite files"; Filename: "{app}\wetwaresuite-files.exe"; Comment: "open, convert and find any file"
Name: "{autoprograms}\wetwaresuite\uninstall wetwaresuite"; Filename: "{uninstallexe}"
Name: "{autodesktop}\wetwaresuite"; Filename: "{app}\wetwaresuite-gui.exe"; Tasks: desktopicon
Name: "{autodesktop}\wetwaresuite image"; Filename: "{app}\wetwaresuite-image.exe"; Tasks: desktopapps
Name: "{autodesktop}\wetwaresuite draw"; Filename: "{app}\wetwaresuite-draw.exe"; Tasks: desktopapps
Name: "{autodesktop}\wetwaresuite model"; Filename: "{app}\wetwaresuite-model.exe"; Tasks: desktopapps
Name: "{autodesktop}\wetwaresuite game"; Filename: "{app}\wetwaresuite-game.exe"; Tasks: desktopapps
Name: "{autodesktop}\wetwaresuite cad"; Filename: "{app}\wetwaresuite-cad.exe"; Tasks: desktopapps
Name: "{autodesktop}\wetwaresuite electronics"; Filename: "{app}\wetwaresuite-electronics.exe"; Tasks: desktopapps
Name: "{autodesktop}\wetwaresuite sound"; Filename: "{app}\wetwaresuite-sound.exe"; Tasks: desktopapps
Name: "{autodesktop}\wetwaresuite video"; Filename: "{app}\wetwaresuite-video.exe"; Tasks: desktopapps
Name: "{autodesktop}\wetwaresuite office"; Filename: "{app}\wetwaresuite-office.exe"; Tasks: desktopapps
Name: "{autodesktop}\wetwaresuite maps"; Filename: "{app}\wetwaresuite-maps.exe"; Tasks: desktopapps
Name: "{autodesktop}\wetwaresuite system"; Filename: "{app}\wetwaresuite-system.exe"; Tasks: desktopapps
Name: "{autodesktop}\wetwaresuite files"; Filename: "{app}\wetwaresuite-files.exe"; Tasks: desktopapps

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Tasks: addtopath; Check: NeedsAddPath(ExpandConstant('{app}'))

; which app opens which file type: made from apps/common/apps.cpp by packaging/file_types.py
#include "associations.iss"

[Run]
Filename: "{app}\wetwaresuite-gui.exe"; Description: "{cm:LaunchProgram,wetwaresuite}"; Flags: nowait postinstall skipifsilent

[Code]
const
  EnvKey = 'Environment';

// is Dir missing from the user's PATH?
function NeedsAddPath(Dir: string): Boolean;
var
  Paths: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvKey, 'Path', Paths) then
  begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Paths) + ';') = 0;
end;

// take Dir back out of the user's PATH on uninstall
procedure RemovePath(Dir: string);
var
  Paths: string;
  P: Integer;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvKey, 'Path', Paths) then
    exit;
  P := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Paths) + ';');
  if P = 0 then
    exit;
  // P is where Dir starts in Paths; drop it with one of its separators
  if P > 1 then
    Delete(Paths, P - 1, Length(Dir) + 1)
  else
    Delete(Paths, 1, Length(Dir) + 1);
  RegWriteExpandStringValue(HKEY_CURRENT_USER, EnvKey, 'Path', Paths);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemovePath(ExpandConstant('{app}'));
end;
