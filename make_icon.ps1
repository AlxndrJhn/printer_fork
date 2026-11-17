# Draws the printer_fork logo (a forking filament path ending in two nozzles)
# and writes printer_fork.ico with PNG-compressed entries.
Add-Type -AssemblyName System.Drawing

function Draw-Logo([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    $k = $s / 256.0

    # rounded dark tile
    $r = 56 * $k; $m = 8 * $k; $w = $s - 2 * $m
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($m, $m, $r, $r, 180, 90)
    $path.AddArc($m + $w - $r, $m, $r, $r, 270, 90)
    $path.AddArc($m + $w - $r, $m + $w - $r, $r, $r, 0, 90)
    $path.AddArc($m, $m + $w - $r, $r, $r, 90, 90)
    $path.CloseFigure()
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, 0)), ([System.Drawing.PointF]::new(0, $s)), ([System.Drawing.Color]::FromArgb(255, 44, 50, 62)), ([System.Drawing.Color]::FromArgb(255, 24, 27, 34))
    $g.FillPath($bg, $path)

    $green  = [System.Drawing.Color]::FromArgb(255, 0, 174, 66)    # Bambu side
    $orange = [System.Drawing.Color]::FromArgb(255, 255, 122, 26)  # Snapmaker side
    $white  = [System.Drawing.Color]::FromArgb(255, 236, 240, 245)
    $lw = [Math]::Max(2.0, 22 * $k)

    function New-Pen($c) {
        $p = New-Object System.Drawing.Pen $c, $lw
        $p.StartCap = 'Round'; $p.EndCap = 'Round'; $p.LineJoin = 'Round'
        return $p
    }

    # stem (incoming file / filament)
    $g.DrawLine((New-Pen $white), [float](128 * $k), [float](50 * $k), [float](128 * $k), [float](112 * $k))
    # two branches
    $pl = New-Pen $green
    $g.DrawBezier($pl, [float](128*$k), [float](112*$k), [float](128*$k), [float](140*$k), [float](76*$k), [float](140*$k), [float](76*$k), [float](168*$k))
    $po = New-Pen $orange
    $g.DrawBezier($po, [float](128*$k), [float](112*$k), [float](128*$k), [float](140*$k), [float](180*$k), [float](140*$k), [float](180*$k), [float](168*$k))
    # fork node
    $nr = 15 * $k
    $g.FillEllipse((New-Object System.Drawing.SolidBrush $white), [float](128*$k - $nr), [float](112*$k - $nr), [float](2*$nr), [float](2*$nr))

    # nozzles (trapezoids) at both branch ends
    foreach ($n in @(@(76, $green), @(180, $orange))) {
        $cx = $n[0] * $k
        $pts = [System.Drawing.PointF[]]@(
            [System.Drawing.PointF]::new($cx - 30*$k, 166*$k),
            [System.Drawing.PointF]::new($cx + 30*$k, 166*$k),
            [System.Drawing.PointF]::new($cx + 10*$k, 200*$k),
            [System.Drawing.PointF]::new($cx - 10*$k, 200*$k))
        $g.FillPolygon((New-Object System.Drawing.SolidBrush $n[1]), $pts)
        $g.FillRectangle((New-Object System.Drawing.SolidBrush $n[1]), [float]($cx - 5*$k), [float](198*$k), [float](10*$k), [float](12*$k))
    }
    $g.Dispose()
    return $bmp
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = foreach ($s in $sizes) {
    $b = Draw-Logo $s
    $ms = New-Object System.IO.MemoryStream
    $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($s -eq 256) { $b.Save((Join-Path $PSScriptRoot 'printer_fork.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
    $b.Dispose()
    , $ms.ToArray()
}

$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $out
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $d = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $bw.Write([byte]$d); $bw.Write([byte]$d); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$pngs[$i].Length); $bw.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'printer_fork.ico'), $out.ToArray())
"ok"
