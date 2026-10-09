[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$LibdbTool,
    [Parameter(Mandatory = $true)][string]$LLVMBuildBin,
    [Parameter(Mandatory = $true)][string]$BaseDatabase,
    [Parameter(Mandatory = $true)][string]$OutputDatabase,
    [Parameter(Mandatory = $true)][string]$WorkDirectory,
    [string]$MinGWBin = 'C:\Strawberry\c\bin',
    [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
$ScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
    $Source = Join-Path $ScriptDirectory 'gen/stl-instantiations.cpp'
$MergeScript = Join-Path $ScriptDirectory 'merge-databases.py'
$ScratchRoot = Join-Path $HOME 'RustroverProjects\rv-scratch'
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
$OutputDatabase = [IO.Path]::GetFullPath($OutputDatabase)
if (-not $WorkDirectory.StartsWith($ScratchRoot, [StringComparison]::OrdinalIgnoreCase) -or
    -not $OutputDatabase.StartsWith($ScratchRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Work directory and output database must stay under $ScratchRoot"
}
foreach ($Required in @($LibdbTool, $LLVMBuildBin, $BaseDatabase, $Source, $MergeScript)) {
    if (-not (Test-Path $Required)) { throw "Required input not found: $Required" }
}
New-Item -ItemType Directory -Force -Path $WorkDirectory | Out-Null
$ObjectDirectory = Join-Path $WorkDirectory 'objects'
$ArchiveDirectory = Join-Path $WorkDirectory 'archives'
New-Item -ItemType Directory -Force -Path $ObjectDirectory, $ArchiveDirectory | Out-Null

# Keep Strawberry's GCC, but drop its Perl entries before invoking the build.
$env:PATH = (($env:PATH -split ';') | Where-Object {
    $_ -notmatch '\\Strawberry\\perl\\(site\\)?bin$'
}) -join ';'

# revenant-libdb links against the shared LLVM build. Run a scratch copy beside
# the DLLs so the generator never launches a build-tree executable in place.
$ScratchLibdb = Join-Path $WorkDirectory 'revenant-libdb.exe'
Copy-Item -Force $LibdbTool $ScratchLibdb
Get-ChildItem -Path $LLVMBuildBin -Filter '*.dll' | Copy-Item -Destination $WorkDirectory -Force

$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $VsWhere)) { throw "vswhere.exe not found: $VsWhere" }
$VsPath = & $VsWhere -latest -products '*' -property installationPath
if (-not $VsPath) { throw 'vswhere did not return a Visual Studio installation' }
$VcVars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
$ClangCl = Join-Path $VsPath 'VC\Tools\Llvm\x64\bin\clang-cl.exe'
if (-not (Test-Path $VcVars)) { throw "vcvars64.bat not found: $VcVars" }
if (-not (Test-Path $ClangCl)) { throw "clang-cl not found: $ClangCl" }

function Invoke-VcCommand([string]$Name, [string]$Commands) {
    $Batch = Join-Path $WorkDirectory "$Name.cmd"
    @"
@echo off
call "$VcVars" >nul || exit /b 1
cd /d "$WorkDirectory"
$Commands
"@ | Set-Content -Path $Batch -Encoding ASCII
    try {
        $Output = & $env:ComSpec /d /c $Batch 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "$Name failed ($LASTEXITCODE):`n$Commands`n$Output"
        }
    } finally {
        Remove-Item $Batch -ErrorAction SilentlyContinue
    }
}

$Archives = [Collections.Generic.List[string]]::new()
$Modes = @(
    @{ Name = 'cl-O1'; Compiler = 'cl'; Flag = '/O1' },
    @{ Name = 'cl-O2'; Compiler = 'cl'; Flag = '/O2' },
    @{ Name = 'cl-Ox'; Compiler = 'cl'; Flag = '/Ox' },
    @{ Name = 'clang-cl-O1'; Compiler = 'clang-cl'; Flag = '-O1' },
    @{ Name = 'clang-cl-O2'; Compiler = 'clang-cl'; Flag = '-O2' },
    @{ Name = 'clang-cl-O3'; Compiler = 'clang-cl'; Flag = '-O3' },
    @{ Name = 'clang-cl-Oz'; Compiler = 'clang-cl'; Flag = '-Oz' }
)
foreach ($Mode in $Modes) {
    $Object = Join-Path $ObjectDirectory "$($Mode.Name).obj"
    $Archive = Join-Path $ArchiveDirectory "$($Mode.Name).lib"
    $Compiler = if ($Mode.Compiler -eq 'cl') { 'cl.exe' } else { "`"$ClangCl`"" }
    $Command = "$Compiler /nologo /c $($Mode.Flag) /std:c++20 /Zi /EHsc /utf-8 /FAcs /Fo`"$Object`" `"$Source`" && lib.exe /nologo /OUT:`"$Archive`" `"$Object`""
    Invoke-VcCommand $Mode.Name $Command
    $Archives.Add($Archive)
}

$Gxx = Join-Path $MinGWBin 'x86_64-w64-mingw32-g++.exe'
$Ar = Join-Path $MinGWBin 'ar.exe'
foreach ($Required in @($Gxx, $Ar)) {
    if (-not (Test-Path $Required)) { throw "MinGW-w64 GCC 13.2 tool not found: $Required" }
}
$GccVersion = (& $Gxx --version | Select-Object -First 1)
if ($GccVersion -notmatch '13\.2') { throw "Expected MinGW GCC 13.2, found: $GccVersion" }
$env:PATH = "$MinGWBin;$env:PATH"
foreach ($Flag in @('-O2', '-O3', '-Os')) {
    $Name = 'mingw-gcc-' + $Flag.TrimStart('-')
    $Object = Join-Path $ObjectDirectory "$Name.o"
    $Archive = Join-Path $ArchiveDirectory "$Name.a"
    & $Gxx -std=c++20 $Flag -c $Source -o $Object
    if ($LASTEXITCODE -ne 0) { throw "$Name compilation failed ($LASTEXITCODE)" }
    & $Ar rcs $Archive $Object
    if ($LASTEXITCODE -ne 0) { throw "$Name archive failed ($LASTEXITCODE)" }
    $Archives.Add($Archive)
}

$TemplateDatabase = Join-Path $WorkDirectory 'template-signatures.json'
$BuildOutput = & $ScratchLibdb build -o $TemplateDatabase @Archives 2>&1
if ($LASTEXITCODE -ne 0) { throw "revenant-libdb build failed:`n$BuildOutput" }
$BuildOutput | Write-Host
& $Python $MergeScript -o $OutputDatabase $BaseDatabase $TemplateDatabase
if ($LASTEXITCODE -ne 0) { throw "libdb merge failed ($LASTEXITCODE)" }
Write-Host "Merged template signatures into $OutputDatabase"
