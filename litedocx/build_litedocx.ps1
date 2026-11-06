param(
    [string]$Root,
    [string]$Litehtml,
    [string]$Obj,
    [string]$Gcc,
    [string]$Gxx,
    [string]$Here,
    [ValidateSet('Release','Debug')]
    [string]$Configuration = 'Release'
)

# Normalize paths - remove trailing backslashes
$Root = $Root.TrimEnd('\')
$Litehtml = $Litehtml.TrimEnd('\')
$Obj = $Obj.TrimEnd('\')
$Here = $Here.TrimEnd('\')

$ErrorActionPreference = 'Stop'

$Dirs = @('gumbo','litehtml','third_party','litedocx')
foreach ($d in $Dirs) {
    $p = Join-Path $Obj $d
    if (-not (Test-Path $p)) { New-Item -ItemType Directory -Path $p -Force | Out-Null }
}

function Compile-File($src, $dst, $compiler, $flags) {
    if (-not (Test-Path $src)) {
        Write-Host "ERRORE: sorgente non trovata: $src"
        return $false
    }
    $needCompile = $false
    if (-not (Test-Path $dst)) {
        $needCompile = $true
    } else {
        $srcTime = (Get-Item $src).LastWriteTime
        $dstTime = (Get-Item $dst).LastWriteTime
        if ($srcTime -gt $dstTime) { $needCompile = $true }
    }
    if (-not $needCompile) {
        Write-Host "  $((Split-Path $src -Leaf)) ... gia aggiornato"
        return $true
    }
    Write-Host "  Compilazione $((Split-Path $src -Leaf)) ..."
    $flagList = @()
    if ($flags) { $flagList += $flags -split '\s+' | Where-Object { $_ -ne '' } }
    # Aggiungi flag di debug se in configurazione Debug
    if ($Configuration -eq 'Debug') {
        $flagList += '-g'
        $flagList += '-O0'
        $flagList += '-DDEBUG'
        $flagList += '-DLITEDOCX_DEBUG=1'
    } else {
        $flagList += '-DLITEDOCX_DEBUG=0'
    }
    $flagList += $src
    $flagList += '-o'
    $flagList += $dst
    & $compiler @flagList
    if ($LASTEXITCODE -ne 0) { return $false }
    return $true
}

function Link-Dll($objDir, $def, $outDll, $outLib) {
    Write-Host "[5/5] Link litedocx.dll..."
    $subdirs = @('gumbo', 'litehtml', 'third_party', 'litedocx')
    $objs = @()
    foreach ($sd in $subdirs) {
        $pattern = Join-Path $objDir "$sd/*.o"
        $objs += Get-ChildItem $pattern -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
    }
    if ($objs.Count -eq 0) { Write-Host "ERRORE: nessun oggetto trovato"; return $false }
    $linkFlags = @('-shared', '-o', $outDll)
    $linkFlags += $objs
    $linkFlags += $def
    $linkFlags += '-static-libgcc', '-static-libstdc++', "-Wl,--out-implib=$outLib"
    $linkFlags += '-lgdi32', '-luser32', '-lole32', '-luuid', '-lwindowscodecs', '-lmsimg32'
    if ($Configuration -eq 'Debug') {
        $linkFlags += '-g'
    }
    & $Gxx @linkFlags
    if ($LASTEXITCODE -ne 0) { return $false }
    Write-Host "[OK] Creato $outDll"
    return $true
}

# Include flags per tutte le compilazioni
$IFLAGS = "-I$Litehtml/include -I$Litehtml/include/litehtml -I$Litehtml/src -I$Litehtml/src/gumbo/src -I$Here/include -I$Here/src -I$Here/third_party"

$optFlags = if ($Configuration -eq 'Debug') { '-O0' } else { '-O2' }

# [1/5] gumbo (C)
Write-Host "[1/5] Compilazione gumbo (C99) con gcc... [$Configuration]"
foreach ($f in Get-ChildItem (Join-Path $Litehtml 'src/gumbo/src/*.c')) {
    $dst = Join-Path $Obj "gumbo/$($f.BaseName).o"
    if (-not (Compile-File $f.FullName $dst $Gcc "$IFLAGS $optFlags -c")) { exit 1 }
}

# [2/5] third_party (C)
Write-Host "[2/5] Compilazione zip/inflate/xmlbuf (C) con gcc... [$Configuration]"
foreach ($f in Get-ChildItem (Join-Path $Here 'third_party/*.c')) {
    $dst = Join-Path $Obj "third_party/$($f.BaseName).o"
    if (-not (Compile-File $f.FullName $dst $Gcc "$IFLAGS $optFlags -c")) { exit 1 }
}

# [3/5] litehtml (C++17)
Write-Host "[3/5] Compilazione litehtml (C++17) con g++... [$Configuration]"
foreach ($f in Get-ChildItem (Join-Path $Litehtml 'src/*.cpp')) {
    $dst = Join-Path $Obj "litehtml/$($f.BaseName).o"
    if (-not (Compile-File $f.FullName $dst $Gxx "$IFLAGS $optFlags -c -std=c++17 -fexceptions")) { exit 1 }
}

# [4/5] litedocx (C++17)
Write-Host "[4/5] Compilazione litedocx (C++17) con g++... [$Configuration]"
foreach ($f in Get-ChildItem (Join-Path $Here 'src/*.cpp')) {
    $dst = Join-Path $Obj "litedocx/$($f.BaseName).o"
    if (-not (Compile-File $f.FullName $dst $Gxx "$IFLAGS $optFlags -c -std=c++17 -fexceptions")) { exit 1 }
}

# [5/5] link
$outDll  = Join-Path $Here 'litedocx.dll'
$outLib  = Join-Path $Here 'liblitedocx.a'
$defFile = Join-Path $Here 'litedocx.def'
if (-not (Test-Path $defFile)) {
    Write-Host "ERRORE: $defFile non trovato"
    exit 1
}
if (-not (Link-Dll $Obj $defFile $outDll $outLib)) { exit 1 }