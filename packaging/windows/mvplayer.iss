; Inno Setup script for MV Player. ci/winpkg.sh fills "stage" with the
; application and everything it loads, then compiles this.
; Needs Inno Setup 6.5 or later (downloads and archive extraction in [Files]).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{7E0B6C8A-3F4D-4B1E-9A52-6D1C8F2E4B73}
AppName=MV Player
AppVersion={#AppVersion}
AppPublisher=brpjerry
AppPublisherURL=https://github.com/brpjerry/mvplayer
AppSupportURL=https://github.com/brpjerry/mvplayer/issues
; Per-user: no administrator rights, and the app can update yt-dlp itself.
PrivilegesRequired=lowest
DefaultDirName={autopf}\MV Player
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=out
OutputBaseFilename=mvplayer-{#AppVersion}-setup
SetupIconFile=..\..\data\mvplayer.ico
UninstallDisplayIcon={app}\mvplayer.exe
UninstallDisplayName=MV Player
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Deno comes as a .zip, which only the full 7-Zip library unpacks.
ArchiveExtraction=full

[Tasks]
Name: "ytdlp"; Description: "Download yt-dlp (finds and downloads the videos)"; GroupDescription: "Helper programs, fetched from their own release pages:"
Name: "deno"; Description: "Download Deno (lets yt-dlp use every YouTube format)"; GroupDescription: "Helper programs, fetched from their own release pages:"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "stage\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion
; The same folder the application's "Update yt-dlp" button writes to
; (toolsDir() in src/core/Util.cpp).
Source: "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"; DestName: "yt-dlp.exe"; DestDir: "{localappdata}\mvplayer\tools"; ExternalSize: 19000000; Flags: external download ignoreversion; Tasks: ytdlp
Source: "https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip"; DestName: "deno.zip"; DestDir: "{localappdata}\mvplayer\tools"; ExternalSize: 130000000; Flags: external download extractarchive ignoreversion; Tasks: deno

[Icons]
Name: "{autoprograms}\MV Player"; Filename: "{app}\mvplayer.exe"
Name: "{autodesktop}\MV Player"; Filename: "{app}\mvplayer.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\mvplayer.exe"; Description: "{cm:LaunchProgram,MV Player}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; The downloaded helper programs and mpv's shader cache. Settings
; (%APPDATA%\mvplayer) and the video library are left alone.
Type: filesandordirs; Name: "{localappdata}\mvplayer"
