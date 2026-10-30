param(
    [ValidateRange(1,10)][int]$Runs = 3,
    [ValidateRange(1,30)][int]$Seconds = 20,
    [string]$Storage = (Join-Path $PSScriptRoot '..\data\user'),
    [string]$Executable = (Join-Path $PSScriptRoot '..\build\windows\Release\smbd.exe'),
    [string]$Output = (Join-Path $PSScriptRoot ('..\build\checks\composition-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Storage = (Resolve-Path -LiteralPath $Storage).Path
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$Output = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $Output) { throw 'Use a new benchmark output directory.' }
$inputPath = Join-Path $root 'knowledge\composition_input.txt'
$settings = @(Get-Content -LiteralPath (Join-Path $root 'data\local\settings.cfg') |
    Where-Object { $_ -notmatch '^(aspect|vsync|fullscreen|resolution|refresh_rate|muted|show_fps)=' })
$settings += @('aspect=1','vsync=0','fullscreen=0','resolution=1','refresh_rate=360','muted=1','show_fps=0')
New-Item -ItemType Directory -Path $Output | Out-Null
$settingsPath = Join-Path $Output 'settings.cfg'
$settings | Set-Content -LiteralPath $settingsPath -Encoding utf8
$results = @()
Push-Location $root
try {
    for ($run = 1; $run -le $Runs; ++$run) {
        $directory = Join-Path $Output "run-$run"
        $runStorage = Join-Path $directory 'storage'
        New-Item -ItemType Directory -Path $runStorage | Out-Null
        # Each run starts from the same saves; caches and writes remain private.
        foreach ($name in @('UDATA','TDATA','storage.key')) {
            $source = Join-Path $Storage $name
            if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $runStorage -Recurse }
        }
        $performancePath = Join-Path $directory 'performance'
        $consolePath = Join-Path $directory 'console.log'
        $timer = [Diagnostics.Stopwatch]::StartNew()
        & $Executable --hidden --mute --settings $settingsPath --storage $runStorage --input $inputPath `
            --benchmark-ticks ($Seconds * 60) --performance $performancePath --seconds ($Seconds + 120) `
            --capture (Join-Path $directory 'composition.png') > $consolePath 2>&1
        $exitCode = $LASTEXITCODE
        $timer.Stop()
        if ($exitCode -ne 0) { throw "Composition run $run failed ($exitCode); see $consolePath" }
        $window = Get-Content -LiteralPath (Join-Path $performancePath 'benchmark.json') -Raw | ConvertFrom-Json
        $runResult = Get-Content -LiteralPath $consolePath -Tail 1 | ConvertFrom-Json
        if (!$runResult.controller_complete -or !$window.complete -or $window.last_tick - $window.first_tick -ne $Seconds * 60 -or
            $window.mode_changes -ne 0 -or $window.ball_state_changes -ne 0) { throw "Run $run left the benchmark window." }
        foreach ($state in @($window.start, $window.end)) {
            if ($state.mode -ne 51 -or $state.ball_state -ne 32 -or $state.character -ne 0 -or
                $state.practice_category -ne 1 -or $state.practice_world -ne 1 -or $state.practice_stage -ne 2) {
                throw "Run $run did not remain in AIAI / Practice / Story World 2 / Composition."
            }
        }
        $summary = (Get-Content -LiteralPath (Join-Path $performancePath 'performance.json') -Raw | ConvertFrom-Json).summary
        if ($summary.counters.logic_ticks -ne $Seconds * 60 -or $summary.diagnostic_frames_excluded -ne 0 -or
            $summary.source_intervals.samples -lt 2 -or $summary.limits.frames_dropped -ne 0) { throw "Run $run has incomplete timing data." }
        $results += [pscustomobject]@{
            run=$run; total_run_ms=$timer.Elapsed.TotalMilliseconds; setup_ticks=$window.first_tick
            frames=$summary.source_intervals.samples; frame=$summary.source_intervals; work_wall_time=$summary.work_wall_time
            gpu=$summary.measured_gpu_spans; peak_working_set_bytes=$summary.peak_working_set_bytes
            guest_ms_per_frame=($summary.phases | Where-Object name -eq 'guest code').wall_ms / ($window.last_frame - $window.first_frame + 1)
            cpu=$summary.cpu; gpu_name=$summary.gpu; target_met=($summary.source_intervals.p99_ms -le 1000.0 / 360)
        }
    }
    [pscustomobject]@{
        format='smbd-composition-benchmark-v1'; scene='Practice / AIAI / Story World 2 / 3 Composition'; target_hz=360
        budget_ms=1000.0 / 360; warmup_ticks=120; measurement_ticks=$Seconds * 60
        resolution_scale=1; widescreen=$true; vsync=$false; refresh_rate=360; target_sha256=$summary.target_sha256
        executable_sha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
        input_sha256=(Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash
        settings_sha256=(Get-FileHash -LiteralPath $settingsPath -Algorithm SHA256).Hash
        runs=$results
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Output 'benchmark.json') -Encoding utf8
    $results | Select-Object run,frames,@{n='median_ms';e={$_.frame.median_ms}},@{n='p99_ms';e={$_.frame.p99_ms}},target_met | Format-Table -AutoSize
} finally { Pop-Location }
