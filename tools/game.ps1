# game.ps1: drives the running game for testing.
#   game.ps1 shot <out.png>            screenshot of the primary screen, scaled to 960 wide
#   game.ps1 key <Enter|W|C|...> [ms] [out.png] [wait ms]  focuses the game, presses a key (held for ms),
#                                       then optionally screenshots after `wait` ms, in the same process
param([string]$cmd, [string]$arg, [int]$ms = 80, [string]$out = "", [int]$wait = 1500)

Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@

function Focus-Game {
    $p = Get-Process azimuth -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { Write-Output "no game window"; exit 1 }
    [W]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
    # Windows only lets the foreground process hand focus over; a tap of Alt lifts that lock.
    [W]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [W]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
    [W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 150
}

function Shot($path) {
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $full = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($full)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $w = 960; $h = [int]($b.Height * $w / $b.Width)
    (New-Object System.Drawing.Bitmap $full, $w, $h).Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output "saved $path"
}

switch ($cmd) {
    "shot" {
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $full = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $g = [System.Drawing.Graphics]::FromImage($full)
        $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
        $w = 960; $h = [int]($b.Height * $w / $b.Width)
        $small = New-Object System.Drawing.Bitmap $full, $w, $h
        $small.Save($arg, [System.Drawing.Imaging.ImageFormat]::Png)
        Write-Output "saved $arg"
    }
    "key" {
        Focus-Game
        $keys = @{ Enter = 0x0D; Escape = 0x1B; Up = 0x26; Down = 0x28; Left = 0x25; Right = 0x27; Space = 0x20 }
        $vk = if ($keys.ContainsKey($arg)) { $keys[$arg] } else { [int][char]$arg.ToUpper() }
        $scan = [byte][W]::MapVirtualKey([uint32]$vk, 0)
        [W]::keybd_event([byte]$vk, $scan, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds $ms
        [W]::keybd_event([byte]$vk, $scan, 2, [UIntPtr]::Zero)
        Write-Output "pressed $arg for $ms ms"
        if ($out) { Start-Sleep -Milliseconds $wait; Shot $out }
    }
    "focus" { Focus-Game; Write-Output "focused" }
    default { Write-Output "game.ps1 shot <png> | key <name> [ms]" }
}
