Add-Type -AssemblyName System.Drawing
$src = 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\screenshots\screenshot1_mixer_overview.png'
$bmp = [System.Drawing.Bitmap]::FromFile($src)
$rect = [System.Drawing.Rectangle]::new(350, 985, 900, 35)
$crop = $bmp.Clone($rect, $bmp.PixelFormat)
$big = New-Object System.Drawing.Bitmap(($rect.Width * 3), ($rect.Height * 3))
$g = [System.Drawing.Graphics]::FromImage($big)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.DrawImage($crop, 0, 0, $big.Width, $big.Height)
$g.Dispose()
$big.Save('E:\0qyx\dev\0Idea\audioalgo\showcase_1024\screenshots\diag_status_zoom.png')
$big.Dispose(); $crop.Dispose(); $bmp.Dispose()
'zoomed'
