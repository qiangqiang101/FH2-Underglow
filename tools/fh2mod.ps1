# fh2mod.ps1: runs a mod's command in the running game, like the platform's fh2mod.exe (through its command
# pipe \\.\pipe\fh2mod-<process id>, see platform/src/commands.cpp).
#   powershell -ExecutionPolicy Bypass -File tools\fh2mod.ps1 Underglow radio
#   powershell -ExecutionPolicy Bypass -File tools\fh2mod.ps1 list
# The pipe is named after the game's process (Windows PowerShell can't list \\.\pipe\ itself).
$pipes = @(Get-Process azimuth -ErrorAction SilentlyContinue | ForEach-Object { "fh2mod-$($_.Id)" })
if (-not $pipes) { Write-Output "No game with mod commands is running."; exit 1 }
if (@($pipes).Count -gt 1) { Write-Output "More than one game is running: $($pipes -join ', ')"; exit 1 }
if ($args.Count -lt 1) { Write-Output "usage: fh2mod.ps1 list | fh2mod.ps1 <mod> <command> [args ...]"; exit 2 }
$line = ($args | ForEach-Object { if ("$_" -match ' ') { "`"$_`"" } else { "$_" } }) -join ' '

$client = New-Object System.IO.Pipes.NamedPipeClientStream('.', @($pipes)[0], [System.IO.Pipes.PipeDirection]::InOut)
try {
    $client.Connect(5000)
    $client.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($line + "`n")
    $client.Write($bytes, 0, $bytes.Length)
    $client.Flush()
    $reader = New-Object System.IO.StreamReader($client, [System.Text.Encoding]::UTF8)
    $reply = $reader.ReadToEnd()
    Write-Output $reply
    if ($reply -like 'error*') { exit 1 }
} catch {
    Write-Output "couldn't talk to the game: $($_.Exception.Message)"
    exit 1
} finally {
    $client.Dispose()
}
