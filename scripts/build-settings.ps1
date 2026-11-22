[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('x64', 'ARM64')][string]$Architecture = 'x64',
    [string]$PlatformToolset = '',
    [string]$Prefix = '',
    [string]$BuildRoot = '',
    [string]$BoostSource = '',
    [string]$BoostHeaders = '',
    [switch]$UiTest
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio with MSBuild and C++/WinRT tools is required.' }
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild/**/Bin/MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'MSBuild was not found.' }
$vcToolsComponent = if ($Architecture -eq 'ARM64') {
    'Microsoft.VisualStudio.Component.VC.Tools.ARM64'
} else {
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
}
$visualStudio = & $vswhere -latest -products * -requires $vcToolsComponent -property installationPath
if ($LASTEXITCODE -ne 0 -or !$visualStudio) { throw "Visual Studio C++ tools for $Architecture are required." }
$developerCommand = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
if (!$BuildRoot) { $BuildRoot = "$root/win32/build/settings" }
$architectureDirectory = if ($Architecture -eq 'x64') { '' } else { $Architecture }
$configurationRoot = if ($architectureDirectory) {
    [IO.Path]::GetFullPath("$BuildRoot/$architectureDirectory/$Configuration")
} else {
    [IO.Path]::GetFullPath("$BuildRoot/$Configuration")
}
$intermediateRoot = if ($architectureDirectory) {
    [IO.Path]::GetFullPath("$BuildRoot/obj/$architectureDirectory/$Configuration")
} else {
    [IO.Path]::GetFullPath("$BuildRoot/obj/$Configuration")
}
$i18nBuild = if ($architectureDirectory) {
    [IO.Path]::GetFullPath("$BuildRoot/i18n/$architectureDirectory/$Configuration")
} else {
    [IO.Path]::GetFullPath("$BuildRoot/i18n/$Configuration")
}
$before = @{}
Get-ChildItem Env: | ForEach-Object { $before[$_.Name] = $_.Value }
try {
    $targetArchitecture = if ($Architecture -eq 'ARM64') { 'arm64' } else { 'amd64' }
    $hostArchitecture = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'amd64' }
    $developerEnvironment = & cmd.exe /d /c "`"$developerCommand`" -arch=$targetArchitecture -host_arch=$hostArchitecture >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment initialization failed.' }
    foreach ($line in $developerEnvironment) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
    }
    $i18nOptions = @('-S', "$root/win32/i18n", '-B', $i18nBuild, '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Configuration", '-DCMAKE_CXX_COMPILER=cl.exe',
        "-DCMAKE_MAKE_PROGRAM=$((Get-Command ninja.exe -CommandType Application | Select-Object -First 1).Source)")
    if ($BoostSource) { $i18nOptions += "-DFETCHCONTENT_SOURCE_DIR_NATIVE_BOOST=$([IO.Path]::GetFullPath($BoostSource))" }
    if ($BoostHeaders) { $i18nOptions += "-DNATIVE_BOOST_HEADERS=$([IO.Path]::GetFullPath($BoostHeaders))" }
    & cmake @i18nOptions
    if ($LASTEXITCODE -ne 0) { throw 'Settings translation library configuration failed.' }
    & cmake --build $i18nBuild -j 6
    if ($LASTEXITCODE -ne 0) { throw 'Settings translation library build failed.' }
} finally {
    foreach ($variable in @(Get-ChildItem Env:)) {
        if (!$before.ContainsKey($variable.Name)) { [Environment]::SetEnvironmentVariable($variable.Name, $null, 'Process') }
    }
    foreach ($name in $before.Keys) { [Environment]::SetEnvironmentVariable($name, $before[$name], 'Process') }
}
$kits = (Get-ItemProperty 'HKLM:/SOFTWARE/Microsoft/Windows Kits/Installed Roots').KitsRoot10
$sdk = Get-ChildItem "$kits/References" -Directory | Where-Object { $_.Name -match '^10\.0\.' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty Name
if (!$sdk -or [version]$sdk -lt [version]'10.0.22000.0') { throw 'Windows SDK 10.0.22000.0 or later is required.' }
$msbuildArguments = @(
    "$root/win32/settings/Fcitx5Settings.vcxproj",
    '/restore', '/m', '/nologo', '/verbosity:minimal',
    "/p:Configuration=$Configuration",
    "/p:Platform=$Architecture",
    "/p:WindowsTargetPlatformVersion=$sdk",
    "/p:TargetPlatformVersion=$sdk",
    "/p:I18nBuildDir=$i18nBuild",
    "/p:BaseIntermediateOutputPath=$([IO.Path]::GetFullPath("$BuildRoot/nuget/$Architecture"))/",
    "/p:OutDir=$configurationRoot/",
    "/p:IntDir=$intermediateRoot/",
    "/p:UiTest=$($UiTest.IsPresent.ToString().ToLowerInvariant())"
)
if ($PlatformToolset) { $msbuildArguments += "/p:PlatformToolset=$PlatformToolset" }
& $msbuild @msbuildArguments
if ($LASTEXITCODE -ne 0) { throw 'WinUI settings build failed.' }
. "$root/win32/scripts/pe-machine.ps1"
Assert-PeMachine "$configurationRoot/Fcitx5Settings.exe" $Architecture
if ($Prefix) {
    $destination = [IO.Path]::GetFullPath($Prefix)
    $repository = [IO.Path]::GetFullPath($root).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (!$destination.StartsWith($repository, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The isolated deployment prefix must be inside this repository.'
    }
    New-Item -ItemType Directory -Force "$destination/settings" | Out-Null
    Get-ChildItem "$configurationRoot" -File |
        Where-Object { $_.Extension -in '.exe', '.dll', '.pri', '.winmd' } |
        Copy-Item -Destination "$destination/settings" -Force
    New-Item -ItemType Directory -Force "$destination/share/locale" | Out-Null
    Copy-Item "$i18nBuild/share/locale/*" "$destination/share/locale" -Recurse -Force
    & cmake --install $i18nBuild --prefix $destination
    if ($LASTEXITCODE -ne 0) { throw 'Translation resource deployment failed.' }
}
Write-Output "Framework-dependent deployment requires the matching $Architecture Windows App SDK 1.8 runtime and Visual C++ runtime."
