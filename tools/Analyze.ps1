param(
    [string]$Image = (Join-Path $PSScriptRoot '..\data\local\default.xbe'),
    [string]$GhidraHome = (Join-Path $PSScriptRoot '..\build\deps\ghidra_12.1.4_PUBLIC'),
    [switch]$Reanalyze
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Image = (Resolve-Path -LiteralPath $Image).Path
$target = (Get-Content -LiteralPath (Join-Path $root 'knowledge\supported_target.json') -Raw | ConvertFrom-Json).targets[0]
if ((Get-FileHash -LiteralPath $Image -Algorithm SHA256).Hash -ne $target.file_sha256) {
    throw 'Executable SHA-256 differs from supported target.'
}
$export = Join-Path $root 'tools\ghidra\ExportProgram.java'
$scriptHash = (Get-FileHash -LiteralPath $export -Algorithm SHA256).Hash
$evidenceHash = (Get-FileHash -LiteralPath (Join-Path $root 'knowledge\non_code_entries.json') -Algorithm SHA256).Hash
$output = Join-Path $root 'data\local\analysis'
$summaryPath = Join-Path $output 'summary.json'
if (!$Reanalyze -and (Test-Path -LiteralPath $summaryPath)) {
    $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
    if ($summary.sha256 -eq $target.file_sha256 -and $summary.export_script_sha256 -eq $scriptHash -and $summary.code_evidence_sha256 -eq $evidenceHash) {
        Write-Host "Reusing verified analysis: $($summary.functions) functions, $($summary.instructions) instructions."
        return
    }
}
$headless = Join-Path ([IO.Path]::GetFullPath($GhidraHome)) 'support\analyzeHeadless.bat'
if (!(Test-Path -LiteralPath $headless)) { throw 'Run tools\Setup-Tools.ps1 first.' }
$project = Join-Path $root 'build\ghidra'
$logs = Join-Path $root 'build\logs'
New-Item -ItemType Directory -Path $project, $logs, $output -Force | Out-Null
$arguments = @($project, 'smbd')
if (Test-Path -LiteralPath (Join-Path $project 'smbd.gpr')) {
    $arguments += @('-process', 'default.xbe')
    if (!$Reanalyze) { $arguments += '-noanalysis' }
} else {
    $arguments += @('-import', $Image, '-loader', 'XbeLoader')
}
$arguments += @('-scriptPath', (Join-Path $root 'tools\ghidra'), '-postScript', 'ExportProgram.java',
    $output, $target.file_sha256, (Join-Path $root 'knowledge\non_code_entries.json'),
    '-analysisTimeoutPerFile', '1800', '-max-cpu', '2',
    '-log', (Join-Path $logs 'ghidra.log'), '-scriptlog', (Join-Path $logs 'ghidra-scripts.log'))
$commandLine = '""' + $headless + '" ' + (($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' ') + '"'
$previousMax = $env:GHIDRA_HEADLESS_MAXMEM
$env:GHIDRA_HEADLESS_MAXMEM = '3G'
$workers = @{}
try {
    # A fresh completion marker is required even when Ghidra returns zero after
    # a script error; stale output must never certify a new export.
    if (Test-Path -LiteralPath $summaryPath) { Remove-Item -LiteralPath $summaryPath }
    Write-Host 'Running one Ghidra analysis/export batch (3 GiB heap, two analysis threads).'
    $process = Start-Process -FilePath $env:ComSpec -ArgumentList @('/d', '/s', '/c', $commandLine) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $logs 'ghidra-stdout.log') `
        -RedirectStandardError (Join-Path $logs 'ghidra-stderr.log')
    $workers[$process.Id] = (Get-CimInstance Win32_Process -Filter "ProcessId = $($process.Id)").CreationDate
    do {
        $all = Get-CimInstance Win32_Process
        $byId = @{}
        foreach ($worker in $all) { $byId[[int]$worker.ProcessId] = $worker }
        do {
            $changed = $false
            foreach ($worker in $all) {
                $parentId = [int]$worker.ParentProcessId
                $parent = $byId[$parentId]
                if ($workers.ContainsKey($parentId) -and $parent -and $parent.CreationDate -eq $workers[$parentId] -and
                    (!$workers.ContainsKey([int]$worker.ProcessId) -or $workers[[int]$worker.ProcessId] -ne $worker.CreationDate)) {
                    $workers[[int]$worker.ProcessId] = $worker.CreationDate
                    $changed = $true
                }
            }
        } while ($changed)
    } while (!$process.WaitForExit(1000))
    if ($process.ExitCode -ne 0) { throw "Ghidra exited with code $($process.ExitCode); see build\logs." }
    if (!(Test-Path -LiteralPath $summaryPath)) { throw 'Ghidra did not finish exporting its analysis.' }
    $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
    if ($summary.sha256 -ne $target.file_sha256 -or $summary.analysis_timed_out) { throw 'Ghidra analysis failed validation.' }
    $summary | Add-Member -NotePropertyName export_script_sha256 -NotePropertyValue $scriptHash -Force
    $summary | Add-Member -NotePropertyName code_evidence_sha256 -NotePropertyValue $evidenceHash -Force
    $summary | ConvertTo-Json | Set-Content -LiteralPath $summaryPath -Encoding utf8
    Write-Host "Exported $($summary.functions) functions and $($summary.instructions) instructions."
} finally {
    foreach ($workerId in @($workers.Keys)) {
        $worker = Get-CimInstance Win32_Process -Filter "ProcessId = $workerId"
        if ($worker -and $worker.CreationDate -eq $workers[$workerId]) { Stop-Process -Id $workerId -ErrorAction SilentlyContinue }
    }
    $env:GHIDRA_HEADLESS_MAXMEM = $previousMax
}
