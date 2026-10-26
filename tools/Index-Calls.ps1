param(
    [Parameter(Mandatory)][string[]]$Trace,
    [string]$Output = 'build/logs/call-recovery-observed.json'
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location $root
try {
    $index = Get-Content 'build/logs/call-recovery.json' -Raw | ConvertFrom-Json
    $registry = Get-Content 'knowledge/static_calls.json' -Raw | ConvertFrom-Json
    $target = (Get-Content 'knowledge/supported_target.json' -Raw | ConvertFrom-Json).targets[0]
    $layout = & build/tools/Release/smbd-tool.exe inspect data/local/default.xbe | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or $layout.sha256 -ne $target.file_sha256 -or $registry.executable_sha256 -ne $target.file_sha256) {
        throw 'Call indexing requires the verified executable.'
    }
    $sites = @{}
    foreach ($site in $index.sites) { $sites[$site.instruction.address] = $site }
    # Read only explicitly registered interface words, never scan a binary/dump.
    $tables = @{}
    $image = [IO.File]::OpenRead((Join-Path $root 'data/local/default.xbe'))
    try {
        foreach ($table in $registry.entry_tables) {
            $start = [Convert]::ToUInt32($table.start.Substring(2),16)
            $end = [Convert]::ToUInt32($table.end.Substring(2),16)
            $stride = if ($null -eq $table.stride) { 4 } else { [uint32]$table.stride }
            if ($end -le $start -or $end-$start -gt 128KB -or $stride -lt 4 -or $stride%4) { throw 'Invalid interface bounds.' }
            $section = $layout.sections | Where-Object {
                $base = [Convert]::ToUInt32($_.address.Substring(2),16)
                $start -ge $base -and $end -le $base+$_.raw_size
            } | Select-Object -First 1
            if (!$section) { throw "Unbacked interface $($table.start)" }
            if (!$tables.ContainsKey($table.start)) { $tables[$table.start] = [Collections.Generic.HashSet[string]]::new() }
            $raw = $section.raw_offset+$start-[Convert]::ToUInt32($section.address.Substring(2),16)
            for ($offset=0; $offset -lt $end-$start; $offset+=$stride) {
                [void]$image.Seek($raw+$offset,[IO.SeekOrigin]::Begin)
                $word = [byte[]]::new(4)
                if ($image.Read($word,0,4) -ne 4) { throw 'Truncated interface word.' }
                [void]$tables[$table.start].Add(('0x{0:X8}' -f [BitConverter]::ToUInt32($word)))
            }
        }
    } finally { $image.Dispose() }
    $observations = [Collections.Generic.List[object]]::new()
    foreach ($path in $Trace) {
        $traceData = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        if ($traceData.format -ne 'smbd-call-trace-v1' -or $traceData.executable_sha256 -ne $target.file_sha256 -or
            $traceData.exhaustive -ne $false -or $traceData.calls.Count -gt 4096) { throw "Unsupported call trace: $path" }
        foreach ($call in $traceData.calls) {
            if (!$sites.ContainsKey($call.site)) { continue }
            $candidates = [Collections.Generic.HashSet[string]]::new()
            for ($reg=0; $reg -lt 8; ++$reg) {
                foreach ($value in @($call.registers[$reg],$call.pointees[$reg][0])) {
                    if ($tables.ContainsKey($value) -and $tables[$value].Contains($call.target)) { [void]$candidates.Add($value) }
                }
            }
            $observations.Add([pscustomobject]@{site=$call.site;target=$call.target;visits=$call.visits;
                pattern=$sites[$call.site].pattern;owners=$sites[$call.site].owners;
                table_candidates=@($candidates | Sort-Object);trace=$path})
        }
    }
    $groups = @($observations | ForEach-Object {
        $observation = $_
        foreach ($table in $observation.table_candidates) { [pscustomobject]@{table=$table;site=$observation.site} }
    } | Group-Object table | ForEach-Object {
        $unique = @($_.Group.site | Sort-Object -Unique)
        [pscustomobject]@{table=$_.Name;count=$unique.Count;sites=$unique}
    } | Sort-Object -Property @{Expression='count';Descending=$true},table)
    $targetGroups = @($observations | Group-Object target | ForEach-Object {
        $unique = @($_.Group.site | Sort-Object -Unique)
        [pscustomobject]@{target=$_.Name;count=$unique.Count;sites=$unique}
    } | Sort-Object -Property @{Expression='count';Descending=$true},target)
    [pscustomobject]@{executable_sha256=$target.file_sha256;exhaustive=$false;
        evidence='Observed receivers guide static writer/alias proofs; they never close a site by themselves.';
        unresolved_sites=$index.unique_sites;observed_sites=@($observations.site | Sort-Object -Unique).Count;
        groups=$groups;target_groups=$targetGroups;observations=@($observations)} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Output -Encoding utf8
    Write-Host "Observed $(@($observations.site | Sort-Object -Unique).Count) of $($index.unique_sites) unresolved sites."
    $groups | Select-Object table,count | Format-Table -AutoSize
} finally { Pop-Location }
