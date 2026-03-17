$ws = New-Object -ComObject WScript.Shell
$sc = $ws.CreateShortcut("$env:USERPROFILE\Desktop\Easel.lnk")
$sc.TargetPath = "$env:USERPROFILE\easel\build\Release\Easel.exe"
$sc.WorkingDirectory = "$env:USERPROFILE\easel"
$sc.Save()
