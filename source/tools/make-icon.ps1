# Regenerates app.ico (16-256 px). The committed app.ico is what the build uses;
# run this only if you change the artwork:  powershell -File tools\make-icon.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-RoundedPath([float]$size, [float]$radius) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $radius * 2
    $w = $size - 1
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($w - $d, 0, $d, $d, 270, 90)
    $path.AddArc($w - $d, $w - $d, $d, $d, 0, 90)
    $path.AddArc(0, $w - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $s = [float]$size

    # Blue squircle-ish tile
    $tile = New-RoundedPath $s ($s * 0.24)
    $rect = New-Object System.Drawing.RectangleF 0, 0, $s, $s
    $top = [System.Drawing.Color]::FromArgb(255, 0x4A, 0x9B, 0xFF)
    $bottom = [System.Drawing.Color]::FromArgb(255, 0x0A, 0x5E, 0xF0)
    $g.FillPath((New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, $top, $bottom, 90), $tile)

    # Pointer arrow, with a soft shadow for depth on larger sizes
    $arrow = @(@(0.40, 0.28), @(0.40, 0.80), @(0.52, 0.68), @(0.61, 0.86), @(0.69, 0.82), @(0.60, 0.645), @(0.76, 0.645))
    if ($size -ge 32) {
        $shadow = $arrow | ForEach-Object { New-Object System.Drawing.PointF ($_[0] * $s), (($_[1] + 0.02) * $s) }
        $g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(70, 0, 20, 60))), [System.Drawing.PointF[]]$shadow)
    }
    $points = $arrow | ForEach-Object { New-Object System.Drawing.PointF ($_[0] * $s), ($_[1] * $s) }
    $g.FillPolygon([System.Drawing.Brushes]::White, [System.Drawing.PointF[]]$points)

    # Click rays
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([Math]::Max(1.2, $s * 0.055))
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $g.DrawLine($pen, 0.40 * $s, 0.19 * $s, 0.40 * $s, 0.12 * $s)
    $g.DrawLine($pen, 0.31 * $s, 0.225 * $s, 0.255 * $s, 0.17 * $s)
    $g.DrawLine($pen, 0.29 * $s, 0.31 * $s, 0.22 * $s, 0.31 * $s)
    $g.Dispose()
    return $bmp
}

# Classic BITMAPINFOHEADER + BGRA (bottom-up) + AND mask: works with both rc.exe and windres.
function Get-DibBytes([System.Drawing.Bitmap]$bmp) {
    $size = $bmp.Width
    $ms = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter $ms
    $w.Write([UInt32]40); $w.Write([Int32]$size); $w.Write([Int32]($size * 2))
    $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]0)
    $w.Write([UInt32]($size * $size * 4)); $w.Write([Int32]0); $w.Write([Int32]0)
    $w.Write([UInt32]0); $w.Write([UInt32]0)
    for ($y = $size - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $size; $x++) {
            $c = $bmp.GetPixel($x, $y)
            $w.Write([byte]$c.B); $w.Write([byte]$c.G); $w.Write([byte]$c.R); $w.Write([byte]$c.A)
        }
    }
    $maskStride = [int][Math]::Ceiling($size / 32) * 4
    $w.Write((New-Object byte[] ($maskStride * $size)))
    $w.Flush()
    return ,$ms.ToArray()
}

function Get-PngBytes([System.Drawing.Bitmap]$bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    return ,$ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = @()
foreach ($size in $sizes) {
    $bmp = New-IconBitmap $size
    $images += ,($(if ($size -ge 256) { Get-PngBytes $bmp } else { Get-DibBytes $bmp }))
    $bmp.Dispose()
}

$out = Join-Path (Split-Path $PSScriptRoot -Parent) 'app.ico'
$fs = [System.IO.File]::Create($out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$images[$i].Length); $bw.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($bytes in $images) { $bw.Write($bytes) }
$bw.Close()
Write-Host "Wrote $out"
