# Restore the original auto-load demo project and stop nothing else.
$backup = 'E:\0qyx\dev\0Idea\audioalgo\showcase_1024\automation\gain_demo.aaproj.original'
$userProj = "$env:USERPROFILE\Documents\AudioAlgo\Examples\gain_demo\gain_demo.aaproj"
Copy-Item $backup $userProj -Force
'restored original gain_demo.aaproj'
