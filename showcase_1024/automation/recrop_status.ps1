Add-Type -AssemblyName System.Drawing
$src = 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\screenshots\screenshot1_mixer_overview.png'
$bmp = [System.Drawing.Bitmap]::FromFile($src)
$strip = $bmp.Clone([System.Drawing.Rectangle]::new(0, 980, 1920, 45), $bmp.PixelFormat)
$strip.Save('E:\0qyx\dev\0Idea\audioalgo\showcase_1024\screenshots\screenshot3_status_sha.png')
$strip.Dispose(); $bmp.Dispose()
'recropped'
