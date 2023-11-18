param(
    [string]$Configuration = "Release",
    [string]$BuildDir = "",
    [string]$DistDir = "",
    [string]$ZipName = "t3-conv.zip",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$ProjectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$ProjectParent = Split-Path -Parent $ProjectRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $ProjectParent "_build\t3-conv-release"
}
if ([string]::IsNullOrWhiteSpace($DistDir)) {
    $DistDir = Join-Path $ProjectRoot "release"
}

$PackageName = "t3-conv"
$StageRoot = Join-Path $DistDir ("." + $PackageName + "-stage")
$PackageRoot = Join-Path $StageRoot $PackageName
$ZipPath = Join-Path $DistDir $ZipName
$ExePath = Join-Path $BuildDir "src\t3conv\$Configuration\t3conv.exe"

function Test-PathInside {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Parent
    )

    $FullPath = [System.IO.Path]::GetFullPath($Path)
    $FullParent = [System.IO.Path]::GetFullPath($Parent)
    if (-not $FullParent.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $FullParent = $FullParent + [System.IO.Path]::DirectorySeparatorChar
    }
    return $FullPath.StartsWith($FullParent, [System.StringComparison]::OrdinalIgnoreCase)
}

function Remove-DirectoryIfSafe {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Parent
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    if (-not (Test-PathInside -Path $Path -Parent $Parent)) {
        throw "refusing to remove directory outside expected parent: $Path"
    }
    Remove-Item -LiteralPath $Path -Recurse -Force
}

function Invoke-CmakeChecked {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "cmake failed with exit code ${LASTEXITCODE}: cmake $($Arguments -join ' ')"
    }
}

if (-not $SkipBuild) {
    Invoke-CmakeChecked -Arguments @("-S", $ProjectRoot, "-B", $BuildDir)
    Invoke-CmakeChecked -Arguments @("--build", $BuildDir, "--config", $Configuration)
}

if (-not (Test-Path -LiteralPath $ExePath)) {
    throw "t3conv.exe not found: $ExePath"
}

if (Test-Path -LiteralPath $StageRoot) {
    Remove-DirectoryIfSafe -Path $StageRoot -Parent $DistDir
}
New-Item -ItemType Directory -Path $PackageRoot | Out-Null
New-Item -ItemType Directory -Path (Join-Path $PackageRoot "runtime\tgstart_host") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $PackageRoot "fonts") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $PackageRoot "docs") | Out-Null

Copy-Item -LiteralPath $ExePath -Destination (Join-Path $PackageRoot "t3conv.exe")
Copy-Item -LiteralPath (Join-Path $ProjectRoot "t3conv.ini") -Destination (Join-Path $PackageRoot "t3conv.ini")
Copy-Item -LiteralPath (Join-Path $ProjectRoot "runtime\tgstart_host\tangent_mnl_bridge.lsp") -Destination (Join-Path $PackageRoot "runtime\tgstart_host\tangent_mnl_bridge.lsp")
Copy-Item -LiteralPath (Join-Path $ProjectRoot "runtime\tgstart_host\tbatsave_experimental_trigger.lsp") -Destination (Join-Path $PackageRoot "runtime\tgstart_host\tbatsave_experimental_trigger.lsp")

$FontFiles = Get-ChildItem -LiteralPath (Join-Path $ProjectRoot "fonts") -File -Force
foreach ($FontFile in $FontFiles) {
    Copy-Item -LiteralPath $FontFile.FullName -Destination (Join-Path $PackageRoot "fonts\$($FontFile.Name)")
}

@'
# t3-conv portable package

## Requirements

- Tianzheng T20 V9 must be installed on this machine.
- AutoCAD must be installed on this machine.
- The package does not include Tianzheng or AutoCAD files.

## Usage

Default output is compact. Use `--dry-run` to inspect the launch plan and `-d` for internal diagnostics.

Single file:

````powershell
.\t3conv.exe -s "C:\path\in.dwg" -o "C:\path\out_t3.dwg"
````

Single-file and folder-batch `t3conv.log` files are written next to `t3conv.exe` by default.
When `t3conv.log` exceeds 20 MB, new entries are written under `log\t3conv.log.1`, `log\t3conv.log.2`, and so on. After 20 rolled files are full, old rolled logs are cleared and numbering restarts.
Temporary `_t3conv_work` files are created under this package directory and removed after each conversion attempt.

Folder batch:

````powershell
.\t3conv.exe -s "C:\path\dwgs" -o "C:\path\t3-output"
````

Folder-batch summaries are appended to `t3conv.log` as a `batch_summary` block. The Tianzheng CAD host is reused between files and restarted only for timeouts, launch failures, crashes, or explicit host-side failures.

## Configuration

Default `t3conv.ini` can remain fully commented. If auto-detection fails, set:

````ini
[paths]
;tangent_root=C:\path\to\TArchT20V9
;autocad_root=C:\Program Files\Autodesk\AutoCAD 2020

[fonts]
fontalt=HZTXT.SHX
````

Project fonts go in `fonts`. On startup, `.shx` and `.ttf` files in this package directory are synced into AutoCAD `Fonts` safely.

Generated runtime files are created under `var` after first run and are intentionally not included in this zip.
'@ | Set-Content -LiteralPath (Join-Path $PackageRoot "docs\README-packaged.md") -Encoding UTF8

if (Test-Path -LiteralPath $ZipPath) {
    Remove-Item -LiteralPath $ZipPath -Force
}
Compress-Archive -LiteralPath $PackageRoot -DestinationPath $ZipPath -Force
Remove-DirectoryIfSafe -Path $StageRoot -Parent $DistDir
if (-not $SkipBuild) {
    Remove-DirectoryIfSafe -Path $BuildDir -Parent $ProjectParent
}

Write-Host "package=$ZipPath"
