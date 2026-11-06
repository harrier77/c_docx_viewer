# psbuild.ps1 - compile source to object only if source is newer than object.
# Usage: psbuild.ps1 -Src <src> -Dst <dst> -Compiler <compiler> -Flags <flags>
param(
    [Parameter(Mandatory=$true)][string]$Src,
    [Parameter(Mandatory=$true)][string]$Dst,
    [Parameter(Mandatory=$true)][string]$Compiler,
    [Parameter(Mandatory=$true)][string]$Flags
)

if (-not (Test-Path $Src)) {
    Write-Host "ERRORE: Source non trovato: $Src"
    exit 1
}

$needsCompile = $false
if (-not (Test-Path $Dst)) {
    $needsCompile = $true
} else {
    $srcTime = (Get-Item $Src).LastWriteTime
    $dstTime = (Get-Item $Dst).LastWriteTime
    if ($srcTime -gt $dstTime) {
        $needsCompile = $true
    }
}

if (-not $needsCompile) {
    $base = [System.IO.Path]::GetFileName($Src)
    Write-Host "  $base ... gia aggiornato"
    exit 0
}

$base = [System.IO.Path]::GetFileName($Src)
Write-Host "  Compilazione $base ..."

# Build arguments carefully - avoid quote issues by using argument array
$argsList = @()
if ($Flags) {
    $argsList += $Flags -split '\s+' | Where-Object { $_ -ne '' }
}
$argsList += $Src
$argsList += '-o'
$argsList += $Dst

& $Compiler @argsList
exit $LASTEXITCODE
