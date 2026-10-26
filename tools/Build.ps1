param(
    [string]$Disc = (Join-Path $PSScriptRoot '..\Super Monkey Ball Deluxe\Super Monkey Ball Deluxe (USA).xiso.iso'),
    [switch]$AllowPartial,
    [ValidateSet('All','Analysis','Game')][string]$Stage = 'All'
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workflowTimer = [Diagnostics.Stopwatch]::StartNew()
$succeeded = $false
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
    $needsConfigure = !(Test-Path -LiteralPath $cache)
    if (Test-Path -LiteralPath $cache) {
        $entries = @{}
        foreach ($line in [IO.File]::ReadLines($cache)) {
            if ($line -match '^(CMAKE_GENERATOR|CMAKE_C_COMPILER|CMAKE_CXX_COMPILER|CMAKE_LINKER_TYPE|SMBD_BATCH|SMBD_SHADER_BANK):[^=]+=(.*)$') {
                $entries[$Matches[1]] = $Matches[2]
            }
        }
        if ($entries.CMAKE_GENERATOR -ne 'Ninja Multi-Config' -or $entries.CMAKE_LINKER_TYPE -ne 'LLD' -or
            [IO.Path]::GetFileNameWithoutExtension($entries.CMAKE_C_COMPILER) -ne 'clang-cl' -or
            [IO.Path]::GetFileNameWithoutExtension($entries.CMAKE_CXX_COMPILER) -ne 'clang-cl') {
            $arguments = @('--fresh') + $arguments
            $needsConfigure = $true
        }
        if ((Get-Item -LiteralPath 'CMakePresets.json').LastWriteTimeUtc -gt (Get-Item -LiteralPath $cache).LastWriteTimeUtc) { $needsConfigure = $true }
        if ($Preset -eq 'tools' -and ($entries.SMBD_BATCH -or $entries.SMBD_SHADER_BANK)) { $needsConfigure = $true }
        foreach ($argument in $Extra) {
            if ($argument -notmatch '^-D(SMBD_BATCH|SMBD_SHADER_BANK)=(.*)$' -or
                ([string]$entries[$Matches[1]]).Replace('\','/') -ne $Matches[2].Replace('\','/')) { $needsConfigure = $true }
        }
    }
    # Ninja tracks CMake sources and knowledge inputs and regenerates as needed.
    if ($needsConfigure) { Invoke-Checked cmake $arguments }
}
Push-Location $root
try {
    . (Join-Path $PSScriptRoot 'Initialize-Toolchain.ps1')
    # Build the small tool first; reuse it for the whole disc and all functions.
    Configure-Native 'tools' 'build/tools'
    Invoke-Checked cmake @('--build', '--preset', 'tools-release')
    $tool = Join-Path $root 'build\tools\Release\smbd-tool.exe'
    New-Item -ItemType Directory -Path 'build\logs', 'build\generated' -Force | Out-Null
    if ($Stage -eq 'All') {
        $Disc = (Resolve-Path -LiteralPath $Disc).Path
        Invoke-Checked $tool @('prepare', $Disc, 'data/local', '--extract-all')
        $ghidraHome = & (Join-Path $PSScriptRoot 'Setup-Tools.ps1')
        & (Join-Path $PSScriptRoot 'Analyze.ps1') -GhidraHome $ghidraHome
        & $tool verify-analysis 'data/local/default.xbe' 'data/local/analysis/instructions.tsv' > 'build/logs/analysis-check.json'
        $analysisResult = $LASTEXITCODE
        if ($analysisResult -ne 0) {
            if ($analysisResult -ne 3 -or !$AllowPartial) { throw 'Ghidra/native instruction comparison failed; see build/logs/analysis-check.json.' }
            Write-Warning 'Ghidra contains rejected instruction rows. Native generation independently decodes original bytes; see the analysis report.'
        }
    } else {
        foreach ($path in @('data/local/default.xbe','data/local/analysis/seeds.txt','data/local/analysis/summary.json')) {
            if (!(Test-Path -LiteralPath $path)) { throw "Prepared input missing: $path. Run the All stage first." }
        }
        $target = (Get-Content -LiteralPath 'knowledge/supported_target.json' -Raw | ConvertFrom-Json).targets[0]
        $analysis = Get-Content -LiteralPath 'data/local/analysis/summary.json' -Raw | ConvertFrom-Json
        if ($analysis.sha256 -ne $target.file_sha256 -or
            (Get-FileHash -LiteralPath 'data/local/default.xbe' -Algorithm SHA256).Hash -ne $target.file_sha256 -or
            $analysis.export_script_sha256 -ne (Get-FileHash -LiteralPath 'tools/ghidra/ExportProgram.java' -Algorithm SHA256).Hash -or
            $analysis.code_evidence_sha256 -ne (Get-FileHash -LiteralPath 'knowledge/non_code_entries.json' -Algorithm SHA256).Hash) {
            throw 'Prepared analysis inputs changed. Run the All stage to refresh them.'
        }
    }
    $batchArguments = @('batch', 'data/local/default.xbe', 'data/local/analysis/seeds.txt', '65536', '8192',
        'build/generated/game.cpp', '--replace', '--shards', '512', '--report', 'build/generated/batch.json')
    if ($AllowPartial) { $batchArguments += '--allow-partial' }
    Invoke-Checked $tool $batchArguments
    $batch = Get-Content -LiteralPath 'build/generated/batch.json' -Raw | ConvertFrom-Json
    $batch.call_recovery | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath 'build/logs/call-recovery.json' -Encoding utf8
    Write-Host "Compiled $($batch.functions_compiled) functions; $($batch.call_recovery.unique_sites) unique unresolved sites."
    $batch.call_recovery.groups | Select-Object -First 12 pattern,count | Format-Table -AutoSize | Out-Host
    if ($Stage -eq 'Analysis') { $succeeded = $true; return }
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
    if ($Stage -eq 'Game') {
        Invoke-Checked cmake @('--build', '--preset', 'release', '--target', 'smbd')
    } else {
        Invoke-Checked cmake @('--build', '--preset', 'release')
        Invoke-Checked (Join-Path $root 'build\windows\Release\smbd-tool.exe') @('check-cpu', 'data/local/default.xbe')
        Invoke-Checked (Join-Path $root 'build\windows\Release\smbd-tool.exe') @('check-input')
        Invoke-Checked (Join-Path $root 'build\windows\Release\smbd-tool.exe') @('check-audio')
    }
    $succeeded = $true
} finally {
    $workflowTimer.Stop()
    if (Test-Path -LiteralPath 'build/logs') {
        [pscustomobject]@{stage=$Stage;seconds=$workflowTimer.Elapsed.TotalSeconds;passed=$succeeded} |
            ConvertTo-Json | Set-Content -LiteralPath "build/logs/workflow-$($Stage.ToLowerInvariant())-timing.json" -Encoding utf8
    }
    Pop-Location
}
