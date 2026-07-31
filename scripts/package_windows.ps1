param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDir
)

$ErrorActionPreference = "Stop"
$package = (Resolve-Path -LiteralPath $PackageDir).Path

function Find-VcRuntimeDirectory {
    $candidates = [System.Collections.Generic.List[string]]::new()
    if ($env:VCToolsRedistDir) {
        $candidates.Add((Join-Path $env:VCToolsRedistDir "x64\Microsoft.VC143.CRT"))
    }
    if ($env:VSINSTALLDIR) {
        $root = Join-Path $env:VSINSTALLDIR "VC\Redist\MSVC"
        if (Test-Path -LiteralPath $root) {
            Get-ChildItem -LiteralPath $root -Directory |
                Sort-Object LastWriteTime -Descending |
                ForEach-Object {
                    $candidates.Add((Join-Path $_.FullName "x64\Microsoft.VC143.CRT"))
            }
        }
    }
    $visualStudioRoot = Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022"
    if (Test-Path -LiteralPath $visualStudioRoot) {
        Get-ChildItem -LiteralPath $visualStudioRoot -Directory | ForEach-Object {
            $root = Join-Path $_.FullName "VC\Redist\MSVC"
            if (Test-Path -LiteralPath $root) {
                Get-ChildItem -LiteralPath $root -Directory |
                    Sort-Object LastWriteTime -Descending |
                    ForEach-Object {
                        $candidates.Add((Join-Path $_.FullName "x64\Microsoft.VC143.CRT"))
                    }
            }
        }
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate "vcruntime140.dll")) {
            return $candidate
        }
    }
    throw "Microsoft VC143 x64 runtime directory was not found"
}

$runtimeDirectory = Find-VcRuntimeDirectory
Get-ChildItem -LiteralPath $runtimeDirectory -Filter "*.dll" -File |
    Copy-Item -Destination $package -Force

$required = @(
    "concrt140.dll",
    "msvcp140.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll"
)
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $name))) {
        throw "Required Visual C++ runtime is missing: $name"
    }
}

$generatedLogs = Join-Path $package "logs"
if (Test-Path -LiteralPath $generatedLogs) {
    Remove-Item -LiteralPath $generatedLogs -Recurse -Force
}
$generatedDirectoryNames = @("bin", "obj", "__pycache__")
Get-ChildItem -LiteralPath $package -Directory -Recurse -Force |
    Where-Object { $generatedDirectoryNames -contains $_.Name } |
    Sort-Object { $_.FullName.Length } -Descending |
    Remove-Item -Recurse -Force

$zip = "$package.zip"
$checksum = "$zip.sha256"
if (Test-Path -LiteralPath $zip) {
    Remove-Item -LiteralPath $zip -Force
}
if (Test-Path -LiteralPath $checksum) {
    Remove-Item -LiteralPath $checksum -Force
}

Compress-Archive -Path (Join-Path $package "*") `
    -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($zip))" |
    Set-Content -LiteralPath $checksum -Encoding ascii

Write-Output "Bundled Visual C++ runtime from: $runtimeDirectory"
Write-Output "Created $zip"
Write-Output "Created $checksum"
