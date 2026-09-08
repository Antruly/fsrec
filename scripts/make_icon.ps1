# Convert resources/icon.png into a multi-size Windows .ico for the exe and
# installer. Produces classic 32bpp (BGRA) BMP-DIB entries at 256/128/64/48/32/16
# so it works everywhere (Explorer, Start Menu, taskbar).
# Usage:  powershell -ExecutionPolicy Bypass -File scripts\make_icon.ps1

Add-Type -AssemblyName System.Drawing

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $root 'resources\icon.png'
$out  = Join-Path $root 'resources\fsrec.ico'

if (-not (Test-Path $src)) { throw "icon source not found: $src" }

$sizes = @(256, 128, 64, 48, 32, 16)
$img = [System.Drawing.Image]::FromFile($src)

# --- build one BMP-DIB blob per size ----------------------------------------
$blobs = New-Object System.Collections.Generic.List[byte[]]

foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingMode   = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
    $g.DrawImage($img, 0, 0, $s, $s)
    $g.Dispose()

    $rect = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = [Math]::Abs($data.Stride)
    $raw = New-Object byte[] ($stride * $s)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $raw, 0, $raw.Length)
    $bmp.UnlockBits($data)

    # XOR mask: bottom-up BGRA (GDI+ Format32bppArgb is already BGRA in memory).
    $xor = New-Object byte[] ($s * $s * 4)
    for ($y = 0; $y -lt $s; $y++) {
        [Array]::Copy($raw, $y * $stride, $xor, ($s - 1 - $y) * ($s * 4), $s * 4)
    }

    # AND mask: all-zero (32bpp alpha carries transparency), rows padded to 4 bytes.
    $andRow = [Math]::Ceiling($s / 8.0)
    $andRowPadded = [Math]::Ceiling($andRow / 4.0) * 4
    $and = New-Object byte[] ($andRowPadded * $s)

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([int32]40)                 # biSize
    $bw.Write([int32]$s)                 # biWidth
    $bw.Write([int32]($s * 2))           # biHeight = XOR + AND
    $bw.Write([int16]1)                  # biPlanes
    $bw.Write([int16]32)                 # biBitCount
    $bw.Write([int32]0)                  # biCompression (BI_RGB)
    $bw.Write([int32]($xor.Length + $and.Length))  # biSizeImage
    $bw.Write([int32]0); $bw.Write([int32]0)       # x/y pixels per meter
    $bw.Write([int32]0); $bw.Write([int32]0)       # clrUsed / clrImportant
    $bw.Write($xor)
    $bw.Write($and)
    $bw.Flush()
    $blobs.Add($ms.ToArray())
    $bw.Dispose(); $ms.Dispose(); $bmp.Dispose()
}
$img.Dispose()

# --- assemble ICO container -------------------------------------------------
$count  = $sizes.Count
$offset = 6 + 16 * $count
$ms2 = New-Object System.IO.MemoryStream
$bw2 = New-Object System.IO.BinaryWriter($ms2)
$bw2.Write([int16]0)          # reserved
$bw2.Write([int16]1)          # type = icon
$bw2.Write([int16]$count)

for ($i = 0; $i -lt $count; $i++) {
    $s = $sizes[$i]
    $blob = $blobs[$i]
    $dim = if ($s -ge 256) { 0 } else { $s }   # 0 encodes 256
    $bw2.Write([byte]$dim)
    $bw2.Write([byte]$dim)
    $bw2.Write([byte]0)       # color count
    $bw2.Write([byte]0)       # reserved
    $bw2.Write([int16]1)      # planes
    $bw2.Write([int16]32)     # bit count
    $bw2.Write([int32]$blob.Length)
    $bw2.Write([int32]$offset)
    $offset += $blob.Length
}
foreach ($blob in $blobs) { $bw2.Write($blob) }
$bw2.Flush()
[System.IO.File]::WriteAllBytes($out, $ms2.ToArray())
$bw2.Dispose(); $ms2.Dispose()

Write-Output ("wrote {0}  ({1} bytes, {2} sizes)" -f $out, (Get-Item $out).Length, $count)
