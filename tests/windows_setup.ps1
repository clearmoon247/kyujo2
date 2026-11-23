$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
public static class UI {
 [DllImport("user32",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent,IntPtr prev,string cls,string name);
 [DllImport("user32")] public static extern IntPtr GetDlgItem(IntPtr parent,int id);
 [DllImport("user32")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32")] public static extern IntPtr GetMenu(IntPtr h);
 [DllImport("user32")] public static extern int GetMenuItemCount(IntPtr m);
 [DllImport("user32",CharSet=CharSet.Unicode)] public static extern int GetMenuString(IntPtr menu,uint item,StringBuilder text,int max,uint flags);
 [DllImport("user32",CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h,uint msg,IntPtr wp,StringBuilder lp);
 [DllImport("user32",CharSet=CharSet.Unicode,EntryPoint="SendMessageW")] public static extern IntPtr SetText(IntPtr h,uint msg,IntPtr wp,string lp);
 [DllImport("user32",CharSet=CharSet.Unicode,EntryPoint="SendMessageW")] public static extern IntPtr Action(IntPtr h,uint msg,IntPtr wp,IntPtr lp);
 [DllImport("user32")] public static extern bool PostMessage(IntPtr h,uint msg,IntPtr wp,IntPtr lp);
 [DllImport("user32")] public static extern bool SetForegroundWindow(IntPtr h);
 public delegate bool Enum(IntPtr h, IntPtr arg);
 [DllImport("user32")] public static extern bool EnumWindows(Enum cb,IntPtr arg);
 [DllImport("user32")] public static extern bool EnumChildWindows(IntPtr h,Enum cb,IntPtr arg);
 [DllImport("user32")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,StringBuilder b,int max);
 [DllImport("user32")] static extern bool PrintWindow(IntPtr hwnd,IntPtr dc,uint flags);
 [StructLayout(LayoutKind.Sequential)] struct Rect {public int l,t,r,b;}
 [DllImport("user32")] static extern bool GetWindowRect(IntPtr h,out Rect rect);
 public static string Text(IntPtr h){var b=new StringBuilder(2048);SendMessage(h,13,(IntPtr)b.Capacity,b);return b.ToString();}
 public static string Class(IntPtr h){var b=new StringBuilder(100);GetClassName(h,b,100);return b.ToString();}
 public static IntPtr Top(int id,string cls){IntPtr hit=IntPtr.Zero;EnumWindows((h,a)=>{uint pid;GetWindowThreadProcessId(h,out pid);if(pid==id&&Class(h)==cls&&IsWindowVisible(h)){hit=h;return false;}return true;},IntPtr.Zero);return hit;}
 public static IntPtr Canvas(IntPtr parent){
  IntPtr hit=IntPtr.Zero;EnumChildWindows(parent,(h,a)=>{if(Class(h)=="Kyujo2MenuCanvas"){hit=h;return false;}return true;},IntPtr.Zero);return hit;
 }
 public static IntPtr Edit(IntPtr parent){IntPtr hit=IntPtr.Zero;EnumChildWindows(parent,(h,a)=>{if(Class(h)=="Edit"&&IsWindowVisible(h)){hit=h;return false;}return true;},IntPtr.Zero);return hit;}
 public static int[] Size(IntPtr h){Rect r;GetWindowRect(h,out r);return new int[]{r.r-r.l,r.b-r.t};}
 public static void Shot(IntPtr h,string path){Rect r;GetWindowRect(h,out r);using(var bmp=new Bitmap(r.r-r.l,r.b-r.t)){using(var g=Graphics.FromImage(bmp)){var dc=g.GetHdc();PrintWindow(h,dc,2);g.ReleaseHdc(dc);}bmp.Save(path,ImageFormat.Png);}}
}
'@
function WaitFor($label,[scriptblock]$query,$limit=35){
 $end=(Get-Date).AddSeconds($limit)
 do { if($app.HasExited){throw "Game exited while waiting for $label"}; $v=& $query;if($v){return $v};Start-Sleep -Milliseconds 150 } while((Get-Date) -lt $end)
 throw "Timed out waiting for $label"
}

function Row([int]$index){
 $key=1+[int](($index*22+92)*$ph/480)+$py
 $x=[int](220*$pw/640)+$px
 $point=($key -shl 16)+$x
 $null=[UI]::PostMessage($panel,514,0,[IntPtr]$point)
 Start-Sleep -Milliseconds 250
}
function Nav($code){$null=[UI]::PostMessage($main,273,[IntPtr]$code,0)}
function Page($name){WaitFor $name { [UI]::Text($panel) -eq $name -and [UI]::IsWindowVisible($panel) } | Out-Null}
function Shot($name){[UI]::Shot($main,(Join-Path $env:KYUJO2_TEST_DIR ($name+'.png')))}
function Pick($index,$path){
 Row $index
 $dlg=WaitFor 'File dialog' {$h=[UI]::Top($app.Id,'#32770');if($h -ne 0){$h}}
 $edit=WaitFor 'File name' {$h=[UI]::Edit($dlg);if($h -ne 0){$h}}
 $null=[UI]::SetText($edit,12,0,$path)
 $null=[UI]::PostMessage([UI]::GetDlgItem($dlg,1),245,0,0)
 WaitFor 'File dialog closed' {![UI]::IsWindowVisible($dlg)} |Out-Null
}
function Key($code,$scan){
 $null=[UI]::PostMessage($main,256,[IntPtr]$code,[IntPtr](1+($scan -shl 16)))
 Start-Sleep -Milliseconds 90
 $null=[UI]::PostMessage($main,257,[IntPtr]$code,[IntPtr]([long]3221225473+($scan -shl 16)))
 Start-Sleep -Milliseconds 120
}
$app=Start-Process $env:KYUJO2_TEST_EXE -RedirectStandardError (Join-Path $env:KYUJO2_TEST_DIR 'game.log') -PassThru
try{
 $main=WaitFor 'Game window' {$h=[UI]::Top($app.Id,'SDL_app');if($h -ne 0){$h}}
 $panel=WaitFor 'Kyujo canvas' {$h=[UI]::Canvas($main);if($h -ne 0){$h}}
 $sz=[UI]::Size($panel);$pw=[Math]::Min($sz[0],[int]($sz[1]*4/3));$ph=[int]($pw*3/4);$px=[int](($sz[0]-$pw)/2);$py=[int](($sz[1]-$ph)/2)
 Page 'Choose the Stadium 2 ROM';Shot '01-ROM'
 Pick 0 $env:KYUJO2_TEST_WRONG_ROM
 Page 'Choose the Stadium 2 ROM'
 Pick 0 $env:KYUJO2_TEST_ROM
 Page 'Game Pak Setup';Shot '02-Game-Pak-Setup'
 Row 0;Page 'Game Pak - Controller 1'
 Pick 0 $env:KYUJO2_TEST_GB
 Row 3;Page 'Game Pak Setup'
 Row 1;Page 'Game Pak - Controller 2'
 Pick 0 $env:KYUJO2_TEST_GB2
 Row 3;Page 'Game Pak Setup';Shot '03-Two-Game-Paks'
 Nav 405;Page 'Controllers';Shot '04-Controllers'
 Row 0;Page 'Controller 1';Shot '05-Bindings'
 Nav 406;Page 'Game Pak Setup';Row 4
 WaitFor 'Game start' {![UI]::IsWindowVisible($panel)}|Out-Null
 Start-Sleep -Seconds 10
 Key 112 59;Page 'Kyujo2';Shot '06-Main-menu'
 Row 4;Page 'Options';Shot '07-Options'
 Key 40 80;Key 37 75
 Row 2;Page 'Battle cheats';Row 0;Row 1;Shot '08-Cheats'
 Key 27 1;Page 'Options'
 Key 112 59;WaitFor 'Resume gameplay' {![UI]::IsWindowVisible($panel)}|Out-Null
 'PASS: fresh ROM import, rejection, two Game Paks, controllers, in-game menu, sound and cheat settings, keyboard and mouse navigation'
}finally{if(!$app.HasExited){$null=$app.CloseMainWindow();$null=$app.WaitForExit(5000)}}
if(!$app.HasExited){throw 'Game did not close'}
$stored = Get-Content -LiteralPath (Join-Path $env:KYUJO2_DATA_DIR 'Kyujo2.json') -Raw | ConvertFrom-Json
if ($stored.Setup.GamePaksConfigured -ne '1' -or $stored.Options.BattleCheats -ne '3') {
 throw 'Game Pak setup or battle cheat choices were not saved'
}
if ($stored.'Game Pak 1'.ROM -ne $env:KYUJO2_TEST_GB -or $stored.'Game Pak 2'.ROM -ne $env:KYUJO2_TEST_GB2) {
 throw 'Cartridges were not saved on the assigned ports'
}
if ($stored.'Game Pak 1'.Battery -eq $stored.'Game Pak 2'.Battery) { throw 'Both ports share a battery save' }
foreach ($slot in $stored.'Game Pak 1',$stored.'Game Pak 2') {
 if (!(Test-Path -LiteralPath $slot.Battery -PathType Leaf)) { throw 'Private battery save is missing' }
}
'PASS: cheat choices, separate Game Pak ports and private battery saves persisted'
$app=Start-Process $env:KYUJO2_TEST_EXE -RedirectStandardError (Join-Path $env:KYUJO2_TEST_DIR 'relaunch.log') -PassThru
try{Start-Sleep -Seconds 6;$main=[UI]::Top($app.Id,'SDL_app');$panel=[UI]::Canvas($main);if($main -eq 0 -or [UI]::IsWindowVisible($panel)){throw 'Relaunch repeated setup'};'PASS: saved configuration boots directly'}
finally{if(!$app.HasExited){$null=$app.CloseMainWindow();$null=$app.WaitForExit(5000)}}
