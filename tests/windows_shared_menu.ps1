# Run with an isolated KYUJO2_DATA_DIR containing Gold.gbc, Silver.gbc and their sibling saves.
$ErrorActionPreference='Stop'
Add-Type -Name Input -Namespace Kyujo2 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr key, IntPtr scan);
'@
$root=$env:KYUJO2_DATA_DIR
$stdoutDir=Split-Path $root
function Tap([int]$vk,[int]$sc) {
    $app.Refresh()
    if($app.HasExited){throw "Game exited while driving menu"}
    $h=$app.MainWindowHandle
    $ext=0
    if($vk -in 37,38,39,40){$ext=16777216}
    $null=[Kyujo2.Input]::PostMessage($h,256,[IntPtr]$vk,[IntPtr](1+$ext+($sc -shl 16)))
    Start-Sleep -Milliseconds 140
    $null=[Kyujo2.Input]::PostMessage($h,257,[IntPtr]$vk,[IntPtr]([long]3221225473+$ext+($sc -shl 16)))
    Start-Sleep -Milliseconds 210
}
function Down {Tap 40 80}
function Up {Tap 38 72}
function Accept {Tap 13 28}
function Back {Tap 27 1}
function InspectFile($pattern,$path,$seconds=22){
    $until=(Get-Date).AddSeconds($seconds)
    do {if(Test-Path -LiteralPath $path){$data=Get-Content -Raw -LiteralPath $path -ErrorAction SilentlyContinue;if($data -match $pattern){return}};Start-Sleep -Milliseconds 220}while((Get-Date) -lt $until -and !$app.HasExited)
    throw "Missing $pattern in $path"
}
function WaitMenu($count) {
    $until=(Get-Date).AddSeconds(20)
    do{if((Get-ChildItem -LiteralPath $env:KYUJO_MENU_DUMP -Filter '*.png').Count -ge $count){return}; Start-Sleep -Milliseconds 240}while((Get-Date) -lt $until -and !$app.HasExited)
    throw "Frame $count missing"
}
$argv='"{0}" "{1}"' -f $env:KYUJO2_TEST_ROM,$root
$app=Start-Process -FilePath $env:KYUJO2_TEST_EXE -ArgumentList $argv -RedirectStandardError (Join-Path $stdoutDir 'guest.log') -PassThru
try {
    WaitMenu 1
    Accept; Accept; Down; Down; Accept
    Back; Down; Accept; Accept; Down; Down; Down; Down; Accept
    Back; Down; Down; Down; Accept
    $cfg=Join-Path $root 'Kyujo2.cfg'
    InspectFile 'pak_setup_done=1' $cfg
    InspectFile 'p1_rom=.*Gold.gbc' $cfg
    InspectFile 'p2_rom=.*Silver.gbc' $cfg
    $app.Refresh()
    $app | Select-Object Id,Path,Responding,MainWindowTitle,WorkingSet64 | Format-List
    $until=(Get-Date).AddSeconds(38);$rendered=$false
    do{
       foreach($f in Get-ChildItem $env:KYUJO2_CAPTURE_DIR -Filter '*.bmp'){
           try{$b=[IO.File]::ReadAllBytes($f.FullName)}catch [IO.IOException]{continue}
           for($i=256;$i -lt $b.Length;$i+=97){if($b[$i]){$rendered=$true;break}}
           if($rendered){break}
       }
       if(!$rendered){Start-Sleep -Milliseconds 350}
    }while(!$rendered -and !$app.HasExited -and (Get-Date) -lt $until)
    if(!$rendered){throw 'Game pixels missing after Game Pak setup'}
    'PASS two Game Paks selected in-window before guest startup; rendered game pixels'
    Tap 112 59; Down; Down; Down; Down; Accept
    Down; Down; Down; Down; Accept
    Tap 112 59
    InspectFile '(?m)^cheats=1' $cfg
    'PASS F1 menu, Options and persisted knockout choice'
} finally {if(!$app.HasExited){$null=$app.CloseMainWindow();$null=$app.WaitForExit(6000)}}
