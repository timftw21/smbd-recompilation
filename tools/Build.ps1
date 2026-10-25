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
Push-Location $root
try {
    # Build the small tool first; reuse it for the whole disc and all functions.
    Invoke-Checked cmake @('-S', '.', '-B', 'build/tools', '-G', 'Visual Studio 18 2026', '-A', 'x64')
    Invoke-Checked cmake @('--build', 'build/tools', '--config', 'Release', '--target', 'smbd-tool', '--parallel', '1')
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
    Invoke-Checked cmake @('-S', '.', '-B', 'build/windows', '-G', 'Visual Studio 18 2026', '-A', 'x64',
        "-DSMBD_BATCH=$root/build/generated/game.cpp")
    Invoke-Checked cmake @('--build', 'build/windows', '--config', 'Release', '--target', 'smbd-tool', '--parallel', '1')
    Invoke-Checked (Join-Path $root 'build\windows\Release\smbd-tool.exe') @('check-cpu', 'data/local/default.xbe')
} finally {
    Pop-Location
}
