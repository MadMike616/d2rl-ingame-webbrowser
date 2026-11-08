param(
    [string]$InputPath = (Join-Path $PSScriptRoot '..\assets\open-external-button.png'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\assets\native')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

function Write-SpA1(
    [System.Drawing.Bitmap]$Bitmap,
    [string]$Path,
    [int]$FrameWidth
) {
    if ($FrameWidth -le 0 -or ($Bitmap.Width % $FrameWidth) -ne 0) {
        throw "Sprite atlas width must be evenly divisible by its frame width: $Path"
    }

    $header = [byte[]]::new(40)
    [System.Text.Encoding]::ASCII.GetBytes('SpA1').CopyTo($header, 0)
    [BitConverter]::GetBytes([uint16]31).CopyTo($header, 4)
    [BitConverter]::GetBytes([uint16]$FrameWidth).CopyTo($header, 6)
    [BitConverter]::GetBytes([uint32]$Bitmap.Width).CopyTo($header, 8)
    [BitConverter]::GetBytes([uint32]$Bitmap.Height).CopyTo($header, 12)
    [BitConverter]::GetBytes([uint32]($Bitmap.Width / $FrameWidth)).CopyTo($header, 20)

    $stream = [System.IO.File]::Create($Path)
    try {
        $stream.Write($header, 0, $header.Length)
        for ($y = 0; $y -lt $Bitmap.Height; $y++) {
            for ($x = 0; $x -lt $Bitmap.Width; $x++) {
                $pixel = $Bitmap.GetPixel($x, $y)
                $stream.WriteByte($pixel.R)
                $stream.WriteByte($pixel.G)
                $stream.WriteByte($pixel.B)
                $stream.WriteByte($pixel.A)
            }
        }
    } finally {
        $stream.Dispose()
    }
}

function New-ButtonAtlas(
    [System.Drawing.Bitmap]$Source,
    [int]$FrameWidth,
    [int]$FrameHeight,
    [int]$SourceFrameWidth,
    [int]$SourceGap
) {
    $atlas = [System.Drawing.Bitmap]::new(
        $FrameWidth * 4,
        $FrameHeight,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($atlas)
    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

        for ($frame = 0; $frame -lt 4; $frame++) {
            $sourceX = $frame * ($SourceFrameWidth + $SourceGap)
            $sourceRect = [System.Drawing.Rectangle]::new(
                $sourceX, 0, $SourceFrameWidth, $Source.Height)
            $destinationRect = [System.Drawing.Rectangle]::new(
                $frame * $FrameWidth, 0, $FrameWidth, $FrameHeight)
            $graphics.DrawImage(
                $Source, $destinationRect, $sourceRect,
                [System.Drawing.GraphicsUnit]::Pixel)
        }
        return $atlas
    } catch {
        $atlas.Dispose()
        throw
    } finally {
        $graphics.Dispose()
    }
}

$source = [System.Drawing.Bitmap]::new((Resolve-Path -LiteralPath $InputPath).Path)
try {
    # The supplied strip has four 74px states separated by one transparent column.
    $sourceFrameWidth = 74
    $sourceGap = 1
    if ($source.Width -ne ($sourceFrameWidth * 4 + $sourceGap * 3)) {
        throw "Expected a 299px four-state strip with 1px gutters; found $($source.Width)x$($source.Height)."
    }

    foreach ($size in @(
        @{ Name = 'open-external-button.sprite'; Width = 74; Height = 62 },
        @{ Name = 'open-external-button.lowend.sprite'; Width = 37; Height = 31 }
    )) {
        $atlas = New-ButtonAtlas $source $size.Width $size.Height $sourceFrameWidth $sourceGap
        try {
            Write-SpA1 $atlas (Join-Path $OutputDirectory $size.Name) $size.Width
        } finally {
            $atlas.Dispose()
        }
    }
} finally {
    $source.Dispose()
}
