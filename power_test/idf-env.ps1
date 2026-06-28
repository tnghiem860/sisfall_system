$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BaseDir = Split-Path -Parent (Split-Path -Parent $ProjectRoot)
$PythonDir = Join-Path $env:LOCALAPPDATA "Programs\Python\Python312"
$IdfPath = Join-Path $BaseDir "esp-idf-v5.5.1"
$IdfToolsPath = Join-Path $BaseDir ".espressif"

if (-not (Test-Path (Join-Path $PythonDir "python.exe"))) {
    throw "Python 3.12 was not found at $PythonDir"
}

if (-not (Test-Path (Join-Path $IdfPath "export.ps1"))) {
    throw "ESP-IDF was not found at $IdfPath"
}

$env:PATH = "$PythonDir;$PythonDir\Scripts;$env:PATH"
$env:IDF_TOOLS_PATH = $IdfToolsPath
. (Join-Path $IdfPath "export.ps1")
