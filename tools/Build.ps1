param(
    [string]$Disc = (Join-Path $PSScriptRoot '..\Super Monkey Ball Deluxe\Super Monkey Ball Deluxe (USA).xiso.iso'),
    [switch]$AllowPartial
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Disc = (Resolve-Path -LiteralPath $Disc).Path
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
function Configure-Native([string]$Preset, [string]$Directory, [string[]]$Extra = @()) {
    $directoryPath = [IO.Path]::GetFullPath((Join-Path $root $Directory))
    if (!$directoryPath.StartsWith((Join-Path $root 'build') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Build configuration must stay inside this project build folder.'
    }
    $arguments = @('--preset', $Preset) + $Extra
    $cache = Join-Path $directoryPath 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cache) {
        $entries = @{}
        foreach ($line in [IO.File]::ReadLines($cache)) {
            if ($line -match '^(CMAKE_GENERATOR|CMAKE_C_COMPILER|CMAKE_CXX_COMPILER|CMAKE_LINKER_TYPE):[^=]+=(.*)$') {
                $entries[$Matches[1]] = $Matches[2]
            }
        }
        if ($entries.CMAKE_GENERATOR -ne 'Ninja Multi-Config' -or $entries.CMAKE_LINKER_TYPE -ne 'LLD' -or
            [IO.Path]::GetFileNameWithoutExtension($entries.CMAKE_C_COMPILER) -ne 'clang-cl' -or
            [IO.Path]::GetFileNameWithoutExtension($entries.CMAKE_CXX_COMPILER) -ne 'clang-cl') {
            $arguments = @('--fresh') + $arguments
        }
    }
    Invoke-Checked cmake $arguments
}
Push-Location $root
try {
    . (Join-Path $PSScriptRoot 'Initialize-Toolchain.ps1')
    # Build the small tool first; reuse it for the whole disc and all functions.
    Configure-Native 'tools' 'build/tools'
    Invoke-Checked cmake @('--build', '--preset', 'tools-release')
    $tool = Join-Path $root 'build\tools\Release\smbd-tool.exe'
    New-Item -ItemType Directory -Path 'build\logs', 'build\generated' -Force | Out-Null
    Invoke-Checked $tool @('prepare', $Disc, 'data/local', '--extract-all')
    $ghidraHome = & (Join-Path $PSScriptRoot 'Setup-Tools.ps1')
    & (Join-Path $PSScriptRoot 'Analyze.ps1') -GhidraHome $ghidraHome
    & $tool verify-analysis 'data/local/default.xbe' 'data/local/analysis/instructions.tsv' > 'build/logs/analysis-check.json'
    $analysisResult = $LASTEXITCODE
    if ($analysisResult -ne 0) {
        if ($analysisResult -ne 3 -or !$AllowPartial) { throw 'Ghidra/native instruction comparison failed; see build/logs/analysis-check.json.' }
        Write-Warning 'Ghidra contains rejected instruction rows. Native generation independently decodes original bytes; see the analysis report.'
    }
    $batchArguments = @('batch', 'data/local/default.xbe', 'data/local/analysis/seeds.txt', '65536', '8192',
        'build/generated/game.cpp', '--replace', '--shards', '64', '--report', 'build/generated/batch.json')
    if ($AllowPartial) { $batchArguments += '--allow-partial' }
    Invoke-Checked $tool $batchArguments
    # Shader compilation is also one offline batch. Reuse it only when every
    # shader input, generator source and generated bank still matches.
    $shaderBank = Join-Path $root 'build\generated\shader_bank.cpp'
    $shaderCache = Join-Path $root 'build\generated\shader-cache.json'
    $shaderInputs = @('src\shaders.cpp', 'src\vertex_program.cpp', 'src\shader_compiler.cpp', 'data\local\default.xbe')
    $shaderInputs += @(Get-ChildItem -LiteralPath 'data/local/media' -Filter '*.xvu' -Recurse -File | ForEach-Object FullName)
    $recipe = ($shaderInputs | Sort-Object | ForEach-Object {
        [IO.Path]::GetRelativePath($root, [IO.Path]::GetFullPath($_)) + ':' + (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash
    }) -join "`n"
    $fingerprint = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($recipe)))
    $reuseShaders = $false
    if ((Test-Path -LiteralPath $shaderCache) -and (Test-Path -LiteralPath $shaderBank)) {
        $cachedShaders = Get-Content -LiteralPath $shaderCache -Raw | ConvertFrom-Json
        $reuseShaders = $cachedShaders.inputs -eq $fingerprint -and
            $cachedShaders.output -eq (Get-FileHash -LiteralPath $shaderBank -Algorithm SHA256).Hash
    }
    if (!$reuseShaders) {
        & $tool compile-shaders 'data/local/default.xbe' $shaderBank > 'build/logs/shaders.json'
        if ($LASTEXITCODE -ne 0) { throw 'Offline shader compilation failed; see build/logs/shaders.json.' }
        @{inputs=$fingerprint;output=(Get-FileHash -LiteralPath $shaderBank -Algorithm SHA256).Hash} |
            ConvertTo-Json | Set-Content -LiteralPath $shaderCache -Encoding utf8
    } else { Write-Host 'Reusing verified offline shader bank.' }
    Configure-Native 'windows' 'build/windows' @(
        "-DSMBD_BATCH=$root/build/generated/game.cpp", "-DSMBD_SHADER_BANK=$shaderBank")
    Invoke-Checked cmake @('--build', '--preset', 'release')
    Invoke-Checked (Join-Path $root 'build\windows\Release\smbd-tool.exe') @('check-cpu', 'data/local/default.xbe')
    Invoke-Checked (Join-Path $root 'build\windows\Release\smbd-tool.exe') @('check-input')
    Invoke-Checked (Join-Path $root 'build\windows\Release\smbd-tool.exe') @('check-audio')
} finally {
    Pop-Location
}
