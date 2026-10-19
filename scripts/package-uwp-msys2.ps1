param(
    [string]$BuildDir = "build-uwp-msys2",
    [string]$DistDir = "dist-uwp-msys2",
    [string]$PackageName = "xemu.uwp.dev",
    [string]$Publisher = "CN=DanP142",
    [string]$Version = "",
    [string]$EntryPoint = "SDL_app",
    [string]$ProcessorArchitecture = "x64",
    [string]$TargetDeviceFamily = "Windows.Xbox",
    [string]$MinVersion = "10.0.19041.0",
    [string]$MaxVersionTested = "10.0.26100.0",
    [string]$SdkVersion = "10.0.26100.0",
    [string]$MsysRoot = "C:\msys64",
    [string]$SystemFilesDir = "$env:LOCALAPPDATA\xemu\xemu",
    [string]$DxcDir = "",
    [string]$VcRedistDir = "",
    [string]$DvdPath = "",
    [switch]$CopyDvdToLocalState,
    [switch]$SkipDependencyScan,
    [switch]$ConfigureLocalState,
    [switch]$Register,
    [switch]$Launch,
    [string]$CertPath = "",
    [string]$CertPassword = "",
    [string]$CertThumbprint = ""
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# The release version is kept in XEMU_VERSION; -Version overrides it.
if (-not $Version) {
    $VersionFile = Join-Path $Root "XEMU_VERSION"
    if (-not (Test-Path $VersionFile)) {
        throw "Missing $VersionFile. Pass -Version."
    }
    $Version = (Get-Content -Raw $VersionFile).Trim()
}

function Resolve-RepoPath($path) {
    if ([System.IO.Path]::IsPathRooted($path)) {
        return [System.IO.Path]::GetFullPath($path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $Root $path))
}

$BuildDir = Resolve-RepoPath $BuildDir
$DistDir = Resolve-RepoPath $DistDir
$BuildExe = Join-Path $BuildDir "qemu-system-i386w.exe"
$Exe = Join-Path $DistDir "xemu.exe"
$SdlDll = Join-Path $DistDir "SDL3.dll"
$Template = Join-Path $Root "packaging\uwp\AppxManifest.xml.in"
$PackageStamp = (Get-Date).ToString("yyyyMMddHHmmss")
$PackageRoot = Join-Path $DistDir ("appx-" + $PackageStamp)
$AssetsDir = Join-Path $PackageRoot "Assets"
$OutAppx = Join-Path $DistDir ("xemu_" + $Version + ".msix")
$SdkBin = "C:\Program Files (x86)\Windows Kits\10\bin\$SdkVersion\x64"
$SdkD3DRedist = "C:\Program Files (x86)\Windows Kits\10\Redist\D3D\x64"
$MakeAppx = Join-Path $SdkBin "makeappx.exe"
$MakePri = Join-Path $SdkBin "makepri.exe"
$SignTool = Join-Path $SdkBin "signtool.exe"
$MsysBash = Join-Path $MsysRoot "usr\bin\bash.exe"
$Cygpath = Join-Path $MsysRoot "usr\bin\cygpath.exe"

if (Test-Path $BuildExe) {
    New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
    Copy-Item -LiteralPath $BuildExe -Destination $Exe -Force
    Write-Output "Refreshed $Exe from $BuildExe"
}
if (!(Test-Path $Exe)) {
    throw "Missing $Exe. Run scripts/build-uwp-msys2.sh first."
}
if (!(Test-Path $SdlDll)) {
    throw "Missing $SdlDll. Run scripts/build-uwp-msys2.sh to copy the SDL3-UWP DLL into the dist directory."
}
if (!(Test-Path $Template)) {
    throw "Missing $Template."
}
if (!(Test-Path $MakeAppx)) {
    throw "Missing $MakeAppx. Install the Windows SDK or pass -SdkVersion."
}
if (!(Test-Path $MakePri)) {
    throw "Missing $MakePri. Install the Windows SDK or pass -SdkVersion."
}

function Stop-XemuProcess {
    $processes = Get-Process -Name xemu -ErrorAction SilentlyContinue
    if (!$processes) {
        return $true
    }

    $ids = @($processes | ForEach-Object { $_.Id })
    $processes | Stop-Process -Force -ErrorAction SilentlyContinue
    foreach ($id in $ids) {
        Wait-Process -Id $id -Timeout 10 -ErrorAction SilentlyContinue
    }

    $processes = Get-Process -Name xemu -ErrorAction SilentlyContinue
    if ($processes) {
        Write-Warning "xemu.exe is still running; continuing with a fresh UWP staging directory."
        return $false
    }

    return $true
}

function Remove-InstalledPackage($name) {
    Stop-XemuProcess | Out-Null

    $installed = Get-AppxPackage -Name $name -ErrorAction SilentlyContinue
    if ($installed) {
        Remove-AppxPackage -Package $installed.PackageFullName -ErrorAction Stop
    }
}

# Packaging does not require uninstalling a locally registered development
# package. Keep its LocalState intact unless -Register explicitly requests a
# local re-registration below.
Stop-XemuProcess | Out-Null
New-Item -ItemType Directory -Force -Path $AssetsDir | Out-Null

$PackagedExe = Join-Path $PackageRoot "xemu.exe"
Copy-Item -LiteralPath $Exe -Destination $PackagedExe -Force
# The package carries no debug information; the build tree keeps symbols.
$Strip = Join-Path $MsysRoot "ucrt64\bin\strip.exe"
if (!(Test-Path $Strip)) {
    throw "Missing $Strip."
}
& $Strip --strip-debug $PackagedExe
if ($LASTEXITCODE -ne 0) {
    throw "strip failed for $PackagedExe."
}
Copy-Item -LiteralPath $SdlDll -Destination (Join-Path $PackageRoot "SDL3.dll") -Force

function Convert-MsysPathToWindows($path) {
    if ($path -match '^/([a-zA-Z])/(.*)$') {
        return (($Matches[1].ToUpper() + ":\") + $Matches[2].Replace("/", "\"))
    }
    if ($path -match '^/ucrt64/(.*)$') {
        return (Join-Path $MsysRoot ("ucrt64\" + $Matches[1].Replace("/", "\")))
    }
    if ($path -match '^/mingw64/(.*)$') {
        return (Join-Path $MsysRoot ("mingw64\" + $Matches[1].Replace("/", "\")))
    }
    if ($path -match '^/usr/(.*)$') {
        return (Join-Path $MsysRoot ("usr\" + $Matches[1].Replace("/", "\")))
    }
    return $path
}

function Get-AppLocalDependencies($binary) {
    if ($SkipDependencyScan) {
        return @()
    }
    if (!(Test-Path $MsysBash) -or !(Test-Path $Cygpath)) {
        throw "Missing MSYS2 tools under $MsysRoot. Pass -MsysRoot or -SkipDependencyScan."
    }

    $msysBinary = (& $Cygpath -u $binary).Trim()
    $escapedBinary = $msysBinary.Replace("'", "'\''")
    $lddOutput = & $MsysBash -lc "export PATH=/ucrt64/bin:/mingw64/bin:/usr/bin:`$PATH; ldd '$escapedBinary'"
    if ($LASTEXITCODE -ne 0) {
        throw "ldd failed while scanning $binary."
    }

    $deps = New-Object System.Collections.Generic.List[string]
    foreach ($line in $lddOutput) {
        if ($line -notmatch '=>\s+(\S+\.dll)') {
            continue
        }
        $dep = $Matches[1]
        if ($dep -match '^/[cC]/[Ww][Ii][Nn][Dd][Oo][Ww][Ss]/') {
            continue
        }
        if ($dep -match '^/[cC]/Windows/') {
            continue
        }

        $winPath = Convert-MsysPathToWindows $dep
        if ((Test-Path $winPath) -and
            ([System.IO.Path]::GetFullPath($winPath) -ne [System.IO.Path]::GetFullPath($binary))) {
            $deps.Add([System.IO.Path]::GetFullPath($winPath))
        }
    }

    return $deps | Sort-Object -Unique
}

function Get-AppLocalDependencyClosure($binaries) {
    if ($SkipDependencyScan) {
        return @()
    }

    $queue = New-Object System.Collections.Queue
    $scanned = @{}
    $dependencies = @{}

    foreach ($binary in $binaries) {
        if (Test-Path $binary) {
            $queue.Enqueue([System.IO.Path]::GetFullPath($binary))
        }
    }

    while ($queue.Count -gt 0) {
        $current = [string]$queue.Dequeue()
        $current = [System.IO.Path]::GetFullPath($current)
        $currentKey = $current.ToLowerInvariant()
        if ($scanned.ContainsKey($currentKey)) {
            continue
        }
        $scanned[$currentKey] = $true

        foreach ($dep in Get-AppLocalDependencies $current) {
            $dep = [System.IO.Path]::GetFullPath($dep)
            $depKey = $dep.ToLowerInvariant()
            if (!$dependencies.ContainsKey($depKey)) {
                $dependencies[$depKey] = $dep
            }
            if (!$scanned.ContainsKey($depKey)) {
                $queue.Enqueue($dep)
            }
        }
    }

    return $dependencies.Values | Sort-Object -Unique
}

function Copy-IfChanged($source, $dest) {
    if (!(Test-Path $source)) {
        throw "Missing $source."
    }

    $sourceItem = Get-Item -LiteralPath $source
    $destItem = Get-Item -LiteralPath $dest -ErrorAction SilentlyContinue
    if ($destItem -and
        $destItem.Length -eq $sourceItem.Length -and
        $destItem.LastWriteTimeUtc -eq $sourceItem.LastWriteTimeUtc) {
        return
    }

    Copy-Item -LiteralPath $source -Destination $dest -Force
    (Get-Item -LiteralPath $dest).LastWriteTimeUtc = $sourceItem.LastWriteTimeUtc
}

function Find-DxcRuntimeDir {
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($DxcDir -ne "") {
        $candidates.Add((Resolve-RepoPath $DxcDir))
    }
    $candidates.Add($SdkD3DRedist)
    $candidates.Add($SdkBin)

    $pathDxc = Get-Command dxcompiler.dll -ErrorAction SilentlyContinue
    if ($pathDxc) {
        $candidates.Add((Split-Path -Parent $pathDxc.Source))
    }

    foreach ($candidate in ($candidates | Sort-Object -Unique)) {
        if ($candidate -eq "" -or !(Test-Path $candidate)) {
            continue
        }

        $compiler = Join-Path $candidate "dxcompiler.dll"
        $dxil = Join-Path $candidate "dxil.dll"
        if ((Test-Path $compiler) -and (Test-Path $dxil)) {
            return $candidate
        }
    }

    return $null
}

function Find-VcRuntimeDir {
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($VcRedistDir -ne "") {
        $candidates.Add((Resolve-RepoPath $VcRedistDir))
    }
    $vsRoots = @(
        "C:\Program Files\Microsoft Visual Studio",
        "C:\Program Files (x86)\Microsoft Visual Studio"
    )
    foreach ($vsRoot in $vsRoots) {
        if (!(Test-Path $vsRoot)) {
            continue
        }
        foreach ($dir in Get-ChildItem -LiteralPath $vsRoot -Recurse -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue) {
            if ($dir.FullName -match "\\onecore\\x64\\") {
                $candidates.Add($dir.FullName)
            }
        }
    }

    foreach ($candidate in ($candidates | Sort-Object -Unique)) {
        if ($candidate -eq "" -or !(Test-Path $candidate)) {
            continue
        }
        $required = @("msvcp140.dll", "vccorlib140.dll", "vcruntime140.dll", "vcruntime140_1.dll")
        $missing = $false
        foreach ($name in $required) {
            if (!(Test-Path (Join-Path $candidate $name))) {
                $missing = $true
                break
            }
        }
        if (!$missing) {
            return $candidate
        }
    }

    return $null
}

$dependencyInputs = @($Exe, $SdlDll)
$dependencies = Get-AppLocalDependencyClosure $dependencyInputs

foreach ($dep in ($dependencies | Sort-Object -Unique)) {
    $name = Split-Path -Leaf $dep
    $distDep = Join-Path $DistDir $name
    $packageDep = Join-Path $PackageRoot $name
    if ($name -ieq "xemu.exe") {
        continue
    }
    if ([System.IO.Path]::GetFullPath($dep) -ne
        [System.IO.Path]::GetFullPath($distDep)) {
        Copy-Item -LiteralPath $dep -Destination $distDep -Force
    }
    Copy-Item -LiteralPath $dep -Destination $packageDep -Force
    Write-Output "Staged dependency $name"
}

$dxcRuntimeDir = Find-DxcRuntimeDir
if (!$dxcRuntimeDir) {
    throw "Missing DXC runtime. Install the Windows SDK Direct3D redist or pass -DxcDir containing dxcompiler.dll and dxil.dll."
}
foreach ($dxcName in @("dxcompiler.dll", "dxil.dll")) {
    $source = Join-Path $dxcRuntimeDir $dxcName
    Copy-IfChanged $source (Join-Path $DistDir $dxcName)
    Copy-Item -LiteralPath $source -Destination (Join-Path $PackageRoot $dxcName) -Force
    Write-Output "Staged DXC runtime $dxcName from $dxcRuntimeDir"
}

$d3dCompiler = Join-Path $SdkD3DRedist "d3dcompiler_47.dll"
if (!(Test-Path $d3dCompiler)) {
    $d3dCompiler = Join-Path $SdkBin "d3dcompiler_47.dll"
}
if (!(Test-Path $d3dCompiler)) {
    throw "Missing d3dcompiler_47.dll. Install the Windows SDK Direct3D redist or pass -SdkVersion."
}
Copy-IfChanged $d3dCompiler (Join-Path $DistDir "d3dcompiler_47.dll")
Copy-Item -LiteralPath $d3dCompiler -Destination (Join-Path $PackageRoot "d3dcompiler_47.dll") -Force
Write-Output "Staged D3D compiler runtime d3dcompiler_47.dll from $(Split-Path -Parent $d3dCompiler)"

$ucrtRuntimeDir = "C:\Program Files (x86)\Windows Kits\10\Redist\$SdkVersion\ucrt\DLLs\x64"
if (!(Test-Path $ucrtRuntimeDir)) {
    $ucrtRuntimeDir = "C:\Program Files (x86)\Windows Kits\10\Redist\ucrt\DLLs\x64"
}
foreach ($ucrtName in @("ucrtbase.dll", "api-ms-win-crt-private-l1-1-0.dll")) {
    $source = Join-Path $ucrtRuntimeDir $ucrtName
    if (!(Test-Path $source)) {
        throw "Missing UCRT runtime $ucrtName. Install the Windows SDK UCRT redist or pass -SdkVersion."
    }
    Copy-IfChanged $source (Join-Path $DistDir $ucrtName)
    Copy-Item -LiteralPath $source -Destination (Join-Path $PackageRoot $ucrtName) -Force
    Write-Output "Staged UCRT runtime $ucrtName from $ucrtRuntimeDir"
}

$vcRuntimeDir = Find-VcRuntimeDir
if (!$vcRuntimeDir) {
    throw "Missing OneCore x64 VC runtime redist. Install Visual Studio C++ redistributables or pass -VcRedistDir."
}
foreach ($vcName in @("msvcp140.dll", "vccorlib140.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
    $source = Join-Path $vcRuntimeDir $vcName
    Copy-IfChanged $source (Join-Path $DistDir $vcName)
    Copy-Item -LiteralPath $source -Destination (Join-Path $PackageRoot $vcName) -Force
    Write-Output "Staged VC runtime $vcName from $vcRuntimeDir"
}

$manifest = Get-Content -LiteralPath $Template -Raw
$manifest = $manifest.Replace("@PACKAGE_NAME@", $PackageName)
$manifest = $manifest.Replace("@PUBLISHER@", $Publisher)
$manifest = $manifest.Replace("@VERSION@", $Version)
$manifest = $manifest.Replace("@ENTRY_POINT@", $EntryPoint)
$manifest = $manifest.Replace("@PROCESSOR_ARCHITECTURE@", $ProcessorArchitecture)
$manifest = $manifest.Replace("@TARGET_DEVICE_FAMILY@", $TargetDeviceFamily)
$manifest = $manifest.Replace("@MIN_VERSION@", $MinVersion)
$manifest = $manifest.Replace("@MAX_VERSION_TESTED@", $MaxVersionTested)
Set-Content -LiteralPath (Join-Path $PackageRoot "AppxManifest.xml") -Value $manifest -Encoding UTF8

Add-Type -AssemblyName System.Drawing
$SourceIcon = Join-Path $Root "ui\icons\xemu_256x256.png"
$SquareIcon = Join-Path $Root "packaging\uwp\xemu-square.png"
foreach ($icon in @($SourceIcon, $SquareIcon)) {
    if (!(Test-Path $icon)) {
        throw "Missing $icon."
    }
}

function Write-ResizedPng($source, $dest, $width, $height) {
    $src = [System.Drawing.Image]::FromFile($source)
    try {
        $bmp = New-Object System.Drawing.Bitmap $width, $height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        try {
            $g.Clear([System.Drawing.Color]::Transparent)
            $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.DrawImage($src, 0, 0, $width, $height)
            $bmp.Save($dest, [System.Drawing.Imaging.ImageFormat]::Png)
        } finally {
            $g.Dispose()
            $bmp.Dispose()
        }
    } finally {
        $src.Dispose()
    }
}

Write-ResizedPng $SquareIcon (Join-Path $AssetsDir "StoreLogo.png") 50 50
Write-ResizedPng $SquareIcon (Join-Path $AssetsDir "Square44x44Logo.png") 44 44
Write-ResizedPng $SquareIcon (Join-Path $AssetsDir "Square150x150Logo.png") 150 150
Write-ResizedPng $SourceIcon (Join-Path $AssetsDir "Wide310x150Logo.png") 310 150

$PriConfig = Join-Path $DistDir "priconfig.xml"
$PriOutputTemp = Join-Path $DistDir ("resources-" + $PackageStamp + ".pri")
$PriOutput = Join-Path $PackageRoot "resources.pri"
Remove-Item -LiteralPath $PriConfig -ErrorAction SilentlyContinue
& $MakePri createconfig /cf $PriConfig /dq en-US /pv 10.0.0
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
Remove-Item -LiteralPath $PriOutputTemp -ErrorAction SilentlyContinue
& $MakePri new /pr $PackageRoot /cf $PriConfig /of $PriOutputTemp /o
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
Copy-Item -LiteralPath $PriOutputTemp -Destination $PriOutput -Force
Remove-Item -LiteralPath $PriOutputTemp -ErrorAction SilentlyContinue
Write-Output "Created $PriOutput"

& $MakeAppx pack /d $PackageRoot /p $OutAppx /o
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if ($CertPath -ne "" -or $CertThumbprint -ne "") {
    if (!(Test-Path $SignTool)) {
        throw "Missing $SignTool. Install the Windows SDK or pass -SdkVersion."
    }
    if ($CertThumbprint -ne "") {
        & $SignTool sign /fd SHA256 /sha1 $CertThumbprint $OutAppx
    } elseif ($CertPassword -ne "") {
        & $SignTool sign /fd SHA256 /a /f $CertPath /p $CertPassword $OutAppx
    } else {
        & $SignTool sign /fd SHA256 /a /f $CertPath $OutAppx
    }
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

Write-Output "Created $OutAppx"

if ($Register) {
    Remove-InstalledPackage $PackageName
    Add-AppxPackage -Register (Join-Path $PackageRoot "AppxManifest.xml") -ForceApplicationShutdown
    $registered = Get-AppxPackage -Name $PackageName -ErrorAction Stop
    Write-Output "Registered $($registered.PackageFamilyName)"
    if ($ConfigureLocalState) {
        if (!(Test-Path $SystemFilesDir)) {
            throw "Missing system files directory $SystemFilesDir."
        }

        $localRoot = Join-Path $env:LOCALAPPDATA ("Packages\" + $registered.PackageFamilyName + "\LocalState\xemu\xemu")
        New-Item -ItemType Directory -Force -Path $localRoot | Out-Null
        $screenshotsRoot = Join-Path $localRoot "screenshots"
        New-Item -ItemType Directory -Force -Path $screenshotsRoot | Out-Null

        $bootrom = Join-Path $SystemFilesDir "mcpx_1.0.bin"
        $flashrom = Join-Path $SystemFilesDir "Complex_4627v1.03.bin"
        if (!(Test-Path $flashrom)) {
            $flashrom = Join-Path $SystemFilesDir "Complex_4627.bin"
        }
        $eeprom = Join-Path $SystemFilesDir "xbox_eeprom.bin"
        $hdd = Join-Path $SystemFilesDir "xbox_hdd.qcow2"

        foreach ($required in @($bootrom, $flashrom, $eeprom, $hdd)) {
            if (!(Test-Path $required)) {
                throw "Missing required xemu system file $required."
            }
            Copy-IfChanged $required (Join-Path $localRoot (Split-Path -Leaf $required))
        }

        function ConvertTo-TomlString($value) {
            return $value.Replace("\", "\\").Replace('"', '\"')
        }

        $bootromPath = Join-Path $localRoot (Split-Path -Leaf $bootrom)
        $flashromPath = Join-Path $localRoot (Split-Path -Leaf $flashrom)
        $eepromPath = Join-Path $localRoot (Split-Path -Leaf $eeprom)
        $hddPath = Join-Path $localRoot (Split-Path -Leaf $hdd)
        $configuredDvdPath = $DvdPath
        if ($CopyDvdToLocalState -and $DvdPath -ne "") {
            if (!(Test-Path $DvdPath)) {
                throw "Missing DVD image $DvdPath."
            }
            $dvdLocalRoot = Join-Path $localRoot "dvd"
            New-Item -ItemType Directory -Force -Path $dvdLocalRoot | Out-Null
            $configuredDvdPath = Join-Path $dvdLocalRoot (Split-Path -Leaf $DvdPath)
            Copy-IfChanged $DvdPath $configuredDvdPath
        }
        $configPath = Join-Path $localRoot "xemu.toml"

        $config = @"
[general]
show_welcome = false
skip_boot_anim = false
screenshot_dir = "$(ConvertTo-TomlString $screenshotsRoot)"

[general.updates]
check = false

[display]
renderer = "D3D12"

[display.quality]
surface_scale = 1

[display.window]
startup_size = "1280x960"

[sys.files]
bootrom_path = "$(ConvertTo-TomlString $bootromPath)"
flashrom_path = "$(ConvertTo-TomlString $flashromPath)"
eeprom_path = "$(ConvertTo-TomlString $eepromPath)"
hdd_path = "$(ConvertTo-TomlString $hddPath)"
dvd_path = "$(ConvertTo-TomlString $configuredDvdPath)"
"@
        Set-Content -LiteralPath $configPath -Value $config -Encoding UTF8
        Write-Output "Configured LocalState at $localRoot"
    }
    if ($Launch) {
        Start-Process explorer.exe ("shell:AppsFolder\" + $registered.PackageFamilyName + "!App")
        Write-Output "Launched $($registered.PackageFamilyName)!App"
    }
}
