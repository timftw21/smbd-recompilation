#Requires -Version 7.0
<#
.SYNOPSIS
Prepare bounded evidence batches, then check recovery without linking the game.
.EXAMPLE
./tools/Recover-Calls.ps1 Prepare
# Audit build/logs/call-workflow/packet.json, then edit shared knowledge/decoder facts.
./tools/Recover-Calls.ps1 Check
.EXAMPLE
./tools/Recover-Calls.ps1 Inspect
# Continue the active packet's next bounded set of functions. -Entry adds explicit leads.
.EXAMPLE
./tools/Recover-Calls.ps1 Defer -Family slot-0093A334 -Reason 'Need aggregate writer proof at 002D47C0.'
./tools/Recover-Calls.ps1 Resume -Family slot-0093A334
#>
param(
    [ValidateSet('Plan','Prepare','Inspect','Check','Defer','Resume')][string]$Action = 'Plan',
    [string[]]$Family = @(),
    [string[]]$Entry = @(),
    [string]$Reason,
    [ValidateRange(1,16)][int]$FamilyLimit = 3,
    [ValidateRange(1,64)][int]$FunctionLimit = 64,
    [ValidateRange(1,8192)][int]$Instructions = 2048,
    [ValidateRange(1,3)][int]$Workers = 1
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$work = Join-Path $root 'build/logs/call-workflow'
$tool = Join-Path $root 'build/tools/Release/smbd-tool.exe'
$timer = [Diagnostics.Stopwatch]::StartNew()
function Read-Json([string]$Path) { Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable }
function Write-Json([string]$Path, $Value) {
    $Value | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath "$Path.tmp" -Encoding utf8NoBOM
    [IO.File]::Move("$Path.tmp",$Path,$true)
}
function Address([string]$Value) {
    if($Value -match '^0x[0-9a-fA-F]{1,8}$') { return [Convert]::ToUInt32($Value.Substring(2),16) }
    # PowerShell evaluates unquoted hexadecimal arguments before binding them.
    if($Value -match '^\d{1,10}$') { return [Convert]::ToUInt32($Value,10) }
    throw "Invalid address: $Value"
}
function Unique($Values) { @($Values | Sort-Object -Unique) }
function Lost($Before, $After) {
    $set = [Collections.Generic.HashSet[string]]::new([string[]]@($After | Where-Object {$null -ne $_}),[StringComparer]::Ordinal)
    @($Before | Where-Object { $null -ne $_ -and !$set.Contains([string]$_) })
}
function Confirm-GameReachability($Batch) {
    $proof=$Batch.game_reachability
    if(!$proof -or !$proof.closed) {
        if($proof.dormant_sites.Count) {throw 'An incomplete game graph classified a dormant site.'}
        return $null
    }
    $image=& $tool inspect (Join-Path $root 'data/local/default.xbe') | ConvertFrom-Json -AsHashtable
    if($LASTEXITCODE -ne 0 -or $image.sha256 -cne $Batch.sha256 -or $image.entry -cne $proof.entry -or
       $image.non_kernel_imports -ne '0x00000000') {throw 'Game graph does not match the verified image entries.'}
    $nativeHeader=[IO.File]::ReadAllText((Join-Path $root 'src/native_guest_roots.h'))
    $fixedRoots=@([regex]::Matches($nativeHeader,'0x[0-9A-F]{8}U') | ForEach-Object {$_.Value.TrimEnd('U')})
    if(@(Lost $fixedRoots $proof.fixed_native_roots).Count -or @(Lost $proof.fixed_native_roots $fixedRoots).Count -or
       @(Lost $image.tls.callbacks $proof.tls_callbacks).Count -or @(Lost $proof.tls_callbacks $image.tls.callbacks).Count) {
        throw 'Game graph omitted a native or TLS entry.'
    }
    $functions=@{};$roots=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach($entry in @($proof.entry)+$fixedRoots+@($proof.tls_callbacks)){[void]$roots.Add($entry)}
    foreach($function in $Batch.functions) {
        $functions[$function.entry]=$function
        if($function.native_api){[void]$roots.Add($function.entry)}
        foreach($entry in $function.static_callback_targets){[void]$roots.Add($entry)}
    }
    if(@(Lost @($roots) $proof.roots).Count -or @(Lost $proof.roots @($roots)).Count) {throw 'Game graph omitted a verified callback root.'}
    $reached=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $queue=[Collections.Generic.Queue[string]]::new();foreach($entry in $roots){$queue.Enqueue($entry)}
    while($queue.Count) {
        $entry=$queue.Dequeue();if(!$reached.Add($entry)){continue}
        $function=$functions[$entry]
        if(!$function -or !$function.compiled -or $function.unresolved_control_flow -or $function.budget_exhausted -or $function.issues.Count) {
            throw "Game graph reaches an incomplete function: $entry"
        }
        foreach($callee in @($function.calls)+@($function.static_callback_targets)){if(!$reached.Contains($callee)){$queue.Enqueue($callee)}}
    }
    if(@(Lost @($reached) $proof.reachable_functions).Count -or @(Lost $proof.reachable_functions @($reached)).Count -or
       $proof.missing_entries.Count -or $proof.reachable_unresolved_sites.Count) {throw 'Game graph reachability witness differs.'}
    $dormant=@{}
    foreach($site in $proof.dormant_sites) {
        foreach($owner in $site.owners) {
            $function=$functions[$owner]
            $binding=@($function.resolved_indirect_instructions | Where-Object {$_.address -eq $site.instruction.address})
            if($reached.Contains($owner) -or !$function.compiled -or $binding.Count -ne 1 -or
               $binding[0].bytes -cne $site.instruction.bytes -or !$binding[0].game_unreachable -or !$binding[0].diagnostic_dispatch -or
               $site.instruction.address -notin $function.diagnostic_only_indirect_sites) {throw 'Dormant instruction witness differs.'}
        }
        $dormant[$site.instruction.address]=$site
    }
    return [pscustomobject]@{reached=$reached;dormant=$dormant}
}
function Test-ExpandedBindingReopen($Entry, $OldFunctions, $CurrentFunctions, $Visiting) {
    $old = $OldFunctions[$Entry]
    $current = $CurrentFunctions[$Entry]
    if(!$old -or !$current -or !$old.closed -or $current.closed -or
        $current.unresolved_indirect_sites.Count -or !$current.blocked_by.Count -or
        !$Visiting.Add($Entry)) { return $false }
    try {
        $oldBindings = @{}
        foreach($binding in $old.resolved_indirect_instructions) { $oldBindings[$binding.address] = $binding }
        $addedTargets = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach($binding in $current.resolved_indirect_instructions) {
            $previous = $oldBindings[$binding.address]
            if(!$previous) { continue }
            foreach($target in (Lost $binding.jump_targets $previous.jump_targets)) { [void]$addedTargets.Add($target) }
        }
        foreach($dependency in $current.blocked_by) {
            if($addedTargets.Contains($dependency)) { continue }
            if($dependency -notin $old.calls -or
                !(Test-ExpandedBindingReopen $dependency $OldFunctions $CurrentFunctions $Visiting)) { return $false }
        }
        return $true
    } finally { [void]$Visiting.Remove($Entry) }
}
function Test-DiscoveredSite($Site, $OldFunctions, $ExpandedTargets) {
    if(!$Site.owners.Count) { return $false }
    foreach($owner in $Site.owners) {
        if($OldFunctions.ContainsKey($owner) -or !$ExpandedTargets.Contains($owner)) { return $false }
    }
    return $true
}
function Inspect-Packet($Packet) {
    $toolHash = (Get-FileHash -LiteralPath $tool -Algorithm SHA256).Hash
    if($Packet.tool_sha256 -ne $toolHash) { throw 'Tool changed. Check this batch, then Prepare a fresh packet.' }
    $cacheRoot = Join-Path $work 'cache'
    $cache = Join-Path $cacheRoot "$toolHash/$Instructions"
    New-Item -ItemType Directory -Path $cache -Force | Out-Null
    $inspectionKey = "$toolHash/$Instructions"
    if($Packet.inspection_key -ne $inspectionKey) { $Packet.inspected=@{}; $Packet.inspection_key=$inspectionKey }
    $extraEntries = @($Entry | ForEach-Object { '0x{0:X8}' -f (Address $_) })
    $Packet.functions = @(Unique (@($Packet.functions) + $extraEntries))
    # Producers and consumers precede their callers. Explicit leads go first.
    $ordered = $extraEntries + @($Packet.producers) + @($Packet.owners) + @($Packet.functions)
    $seen = [Collections.Generic.HashSet[string]]::new()
    $pending = [Collections.Generic.List[string]]::new()
    $limit = [Math]::Min($FunctionLimit,[Math]::Floor(131072/$Instructions))
    foreach($entryAddress in $ordered) {
        $entryAddress = '0x{0:X8}' -f (Address $entryAddress)
        if(!$seen.Add($entryAddress)) { continue }
        $path = Join-Path $cache "$entryAddress.json"
        if(Test-Path -LiteralPath $path) {
            $report = Read-Json $path
            if($report.sha256 -ne $Packet.executable_sha256 -or $report.entry -ne $entryAddress -or
                $report.format -ne 'smbd-analysis-v1' -or $report.instruction_count -gt $Instructions) {
                throw "Invalid cached analysis: $path"
            }
            $Packet.inspected[$entryAddress]=@{entry=$entryAddress;path=$path;budget_exhausted=$report.budget_exhausted;
                unresolved_control_flow=$report.unresolved_control_flow}
        } elseif(!$Packet.inspected.ContainsKey($entryAddress) -or $entryAddress -in $extraEntries) { $pending.Add($entryAddress) }
    }
    $selected = @($pending | Select-Object -First $limit)
    if($selected.Count) {
        $entriesPath = Join-Path $work 'entries.txt'
        $selected | Set-Content -LiteralPath $entriesPath -Encoding ascii
        & $tool analyse-set data/local/default.xbe $entriesPath $Instructions $cache > (Join-Path $work 'inspection.json')
        if($LASTEXITCODE -notin @(0,3)) { throw 'Native inspection failed; see inspection.json.' }
        foreach($entryAddress in $selected) {
            $path = Join-Path $cache "$entryAddress.json"
            $report = Read-Json $path
            $Packet.inspected[$entryAddress]=@{entry=$entryAddress;path=$path;budget_exhausted=$report.budget_exhausted;
                unresolved_control_flow=$report.unresolved_control_flow}
        }
    }
    $Packet.not_inspected = @(Lost $Packet.functions @($Packet.inspected.Keys))
    $Packet.instruction_limit = $Instructions
    # Keep at most 128 reports / 64 MiB across tool versions. Never cycle through
    # evicted functions again automatically; -Entry explicitly reopens a lead.
    $files = @(Get-ChildItem -LiteralPath $cacheRoot -Recurse -File -Filter '*.json' | Sort-Object LastWriteTimeUtc)
    $bytes = ($files | Measure-Object Length -Sum).Sum
    $count = $files.Count
    $pinned = [Collections.Generic.HashSet[string]]::new([string[]]@($selected | ForEach-Object { Join-Path $cache "$_.json" }),[StringComparer]::OrdinalIgnoreCase)
    foreach($file in $files) {
        if($count -le 128 -and $bytes -le 64MB) { break }
        if($pinned.Contains($file.FullName)) { continue }
        if(!$file.FullName.StartsWith($cacheRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Cache path escaped workspace.' }
        Remove-Item -LiteralPath $file.FullName
        $bytes -= $file.Length; --$count
    }
    if($count -gt 128 -or $bytes -gt 64MB) { throw 'Inspection exceeds cache bounds. Reduce -FunctionLimit.' }
    foreach($report in $Packet.inspected.Values) { $report.cached = Test-Path -LiteralPath $report.path }
    Write-Json (Join-Path $work 'packet.json') $Packet
    $exhausted = @($Packet.inspected.Values | Where-Object { $_.budget_exhausted }).Count
    Write-Host "Inspected $($selected.Count) new functions; $($Packet.inspected.Count) inspected, $($Packet.not_inspected.Count) pending, $exhausted at budget."
    Write-Host "Evidence: $work/packet.json. Inspect continues pending functions; incomplete control flow still needs proof."
}
Push-Location $root
try {
    New-Item -ItemType Directory -Path $work -Force | Out-Null
    $target = (Read-Json 'knowledge/supported_target.json').targets[0].file_sha256
    $statePath = Join-Path $work 'state.json'
    $state = if(Test-Path -LiteralPath $statePath) { Read-Json $statePath } else {
        @{executable_sha256=$target;active=$false;deferred=@{}}
    }
    if($state.executable_sha256 -ne $target) { throw 'Workflow state belongs to another executable.' }
    if($Action -in @('Defer','Resume')) {
        if(!$Family.Count -or ($Action -eq 'Defer' -and [string]::IsNullOrWhiteSpace($Reason))) { throw 'Supply -Family and, for Defer, a concrete -Reason.' }
        $queue = Read-Json (Join-Path $work 'queue.json')
        foreach($id in $Family) {
            if($id -notin $queue.families.id) { throw "Unknown family: $id" }
            if($Action -eq 'Defer') { $state.deferred[$id] = $Reason }
            else { [void]$state.deferred.Remove($id) }
            foreach($row in $queue.families) { if($row.id -eq $id) { $row.deferred_reason=$state.deferred[$id] } }
        }
        Write-Json $statePath $state
        $queue.families = @($queue.families | Sort-Object @{Expression={[bool]$_.deferred_reason}},priority,@{Expression='count';Descending=$true},id)
        $queue.deferred_sites = [int]($queue.families | Where-Object deferred_reason | Measure-Object count -Sum).Sum
        Write-Json (Join-Path $work 'queue.json') $queue
        Write-Host "$Action recorded. Deferred sites remain in the unresolved total."
        return
    }
    if($Action -in @('Inspect','Check') -and !$state.active) { throw 'Prepare a batch first.' }
    if($Action -eq 'Prepare' -and $state.active) { throw 'A batch is active. Use Inspect to continue it or Check before preparing another.' }
    if($Action -eq 'Inspect') {
        $packet = Read-Json (Join-Path $work 'packet.json')
        if(!$PSBoundParameters.ContainsKey('Instructions') -and $packet.instruction_limit) { $Instructions=$packet.instruction_limit }
        Inspect-Packet $packet
        return
    }
    # One sequential analysis cycle, no runtime link, game run, or media extraction.
    & (Join-Path $PSScriptRoot 'Build.ps1') -Stage Analysis -AllowPartial -Workers $Workers
    $batch = Read-Json 'build/generated/batch.json'
    if($batch.sha256 -ne $target -or !$batch.source_written) { throw 'Expected a generated batch for the verified executable.' }
    $index = $batch.call_recovery
    $functions = @{}
    foreach($function in $batch.functions) { $functions[$function.entry] = $function }
    if($Action -eq 'Check') {
        $before = Read-Json (Join-Path $work 'baseline.json')
        if($before.sha256 -ne $target) { throw 'Baseline belongs to another executable.' }
        $failures = [Collections.Generic.List[object]]::new()
        $gameGraph=Confirm-GameReachability $batch
        $expandedReopens = [Collections.Generic.List[object]]::new()
        $expandedTargets = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        $oldFunctions = @{}
        foreach($old in $before.functions) { $oldFunctions[$old.entry] = $old }
        foreach($old in $before.functions) {
            $current = $functions[$old.entry]
            if(!$current -or ($old.compiled -and !$current.compiled)) {
                $failures.Add(@{entry=$old.entry;kind='lost_function'}); continue
            }
            if($current.instruction_count -lt $old.instruction_count) { $failures.Add(@{entry=$old.entry;kind='fewer_instructions'}) }
            if($old.closed -and !$current.closed) {
                $visiting = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
                if(Test-ExpandedBindingReopen $old.entry $oldFunctions $functions $visiting) {
                    $expandedReopens.Add(@{entry=$old.entry;blocked_by=@($current.blocked_by)})
                } else { $failures.Add(@{entry=$old.entry;kind='reopened_function'}) }
            }
            if($old.native_api -ne $current.native_api) { $failures.Add(@{entry=$old.entry;kind='changed_native_binding'}) }
            foreach($field in @('calls','static_callback_targets','kernel_ordinals')) {
                if(($old[$field] -join ',') -ceq ($current[$field] -join ',')) { continue }
                $lost = @(Lost $old[$field] $current[$field])
                if($lost.Count) { $failures.Add(@{entry=$old.entry;kind="lost_$field";values=$lost}) }
            }
            $bindingsBySite = @{}
            foreach($binding in $current.resolved_indirect_instructions) { $bindingsBySite[$binding.address]=$binding }
            $oldBindingsBySite = @{}
            foreach($binding in $old.resolved_indirect_instructions) { $oldBindingsBySite[$binding.address]=$binding }
            foreach($binding in $current.resolved_indirect_instructions) {
                $previous = $oldBindingsBySite[$binding.address]
                $oldTargets = if($previous) { @($previous.jump_targets) } else { @() }
                foreach($expandedTarget in (Lost $binding.jump_targets $oldTargets)) { [void]$expandedTargets.Add($expandedTarget) }
                if($binding.tail_target -and (!$previous -or $binding.tail_target -ne $previous.tail_target)) {
                    [void]$expandedTargets.Add($binding.tail_target)
                }
            }
            foreach($expandedTarget in (Lost $current.static_callback_targets $old.static_callback_targets)) { [void]$expandedTargets.Add($expandedTarget) }
            foreach($binding in $old.resolved_indirect_instructions) {
                $replacement = $bindingsBySite[$binding.address]
                if(!$replacement -or $binding.bytes -cne $replacement.bytes -or
                    $binding.kernel_ordinal -ne $replacement.kernel_ordinal -or $binding.tail_target -ne $replacement.tail_target -or
                    $binding.tail_dispatch -ne $replacement.tail_dispatch -or
                    $binding.proven_fault -ne $replacement.proven_fault -or $binding.fault_target -ne $replacement.fault_target -or
                    $binding.game_unreachable -ne $replacement.game_unreachable -or $binding.diagnostic_dispatch -ne $replacement.diagnostic_dispatch -or
                    @(Lost $binding.zero_sources $replacement.zero_sources).Count -or
                    @(Lost $binding.fault_targets $replacement.fault_targets).Count -or
                    @(Lost $binding.fault_literals $replacement.fault_literals).Count -or
                    @(Lost $binding.jump_targets $replacement.jump_targets).Count) {
                    $failures.Add(@{entry=$old.entry;site=$binding.address;kind='lost_or_changed_binding'})
                }
            }
        }
        foreach($function in $batch.functions) {
            if($function.budget_exhausted -or $function.issues.Count -or $function.error) {
                $failures.Add(@{entry=$function.entry;kind='analysis_or_translation_failure'})
            }
        }
        foreach($field in @('pending_functions','missing_direct_functions')) {
            if($batch[$field].Count) { $failures.Add(@{kind=$field;values=$batch[$field]}) }
        }
        if($batch.unsupported_instructions.Count) { $failures.Add(@{kind='unsupported_instructions';values=$batch.unsupported_instructions}) }
        $lostSeeds = @(Lost $before.seeds $batch.seeds)
        if($lostSeeds.Count) { $failures.Add(@{kind='lost_seeds';values=$lostSeeds}) }
        $oldSites = @($before.call_recovery.sites | ForEach-Object { $_.instruction.address })
        $newSites = @($index.sites | ForEach-Object { $_.instruction.address })
        $closed = @(Lost $oldSites $newSites)
        $introduced = @(Lost $newSites $oldSites)
        $discovered = [Collections.Generic.List[string]]::new()
        $unexpected = [Collections.Generic.List[string]]::new()
        foreach($site in $index.sites) {
            if($site.instruction.address -notin $introduced) { continue }
            if(Test-DiscoveredSite $site $oldFunctions $expandedTargets) { $discovered.Add($site.instruction.address) }
            else { $unexpected.Add($site.instruction.address) }
        }
        if($unexpected.Count) { $failures.Add(@{kind='introduced_unresolved_sites';values=@($unexpected)}) }
        # A site disappearing from the unknown list is not sufficient. Every old
        # owner must still emit those original bytes with a concrete target binding.
        foreach($site in $before.call_recovery.sites) {
            if($site.instruction.address -notin $closed) { continue }
            foreach($owner in $site.owners) {
                $bindings = @($functions[$owner].resolved_indirect_instructions | Where-Object { $_.address -eq $site.instruction.address })
                if($bindings.Count -ne 1 -or $bindings[0].bytes -cne $site.instruction.bytes -or
                    !($bindings[0].jump_targets.Count -or $bindings[0].tail_target -or $bindings[0].ContainsKey('kernel_ordinal') -or
                      ($bindings[0].proven_unreachable -and $bindings[0].zero_guard -and
                       ($bindings[0].source_slot -or $bindings[0].zero_source)) -or
                      ($bindings[0].proven_fault -and $bindings[0].fault_target -eq '0x00000000' -and
                       ($bindings[0].source_slot -or $bindings[0].zero_source)) -or
                      ($bindings[0].game_unreachable -and $bindings[0].diagnostic_dispatch -and $gameGraph -and
                       $gameGraph.dormant.ContainsKey($site.instruction.address) -and !$gameGraph.reached.Contains($owner)))) {
                    $failures.Add(@{entry=$owner;site=$site.instruction.address;kind='closure_without_target_binding'})
                }
            }
        }
        $check = @{executable_sha256=$target;passed=($failures.Count -eq 0);before=$oldSites.Count;after=$newSites.Count;
            closed=$closed;introduced=$introduced;discovered_unresolved_sites=@($discovered);expanded_binding_reopens=@($expandedReopens);failures=@($failures);seconds=$timer.Elapsed.TotalSeconds;
            translated_functions=$batch.translated_functions;native_platform_functions=$batch.native_platform_functions}
        Write-Json (Join-Path $work 'check.json') $check
        if(!$check.passed) { throw 'Recovery check failed; see check.json. Baseline retained so regressions cannot be hidden by another Prepare.' }
    }
    $registry = Read-Json 'knowledge/static_calls.json'
    if($registry.executable_sha256 -ne $target) { throw 'Static registry belongs to another executable.' }
    $observations = @{}
    $observedPath = 'build/logs/call-recovery-observed.json'
    if(Test-Path -LiteralPath $observedPath) {
        $observed = Read-Json $observedPath
        if($observed.executable_sha256 -ne $target -or $observed.exhaustive -ne $false) { throw 'Invalid observation index.' }
        foreach($observation in $observed.observations) {
            if(!$observations.ContainsKey($observation.site)) { $observations[$observation.site] = [Collections.Generic.List[object]]::new() }
            $observations[$observation.site].Add($observation)
        }
    }
    $families = @{}
    $sites = @{}
    $callers = @{}
    foreach($row in $index.direct_callers) { $callers[$row.entry] = $row.callers }
    $indexedWriters = @{}
    foreach($function in $batch.functions) {
        foreach($write in $function.indexed_writes) {
            if(!$write.table_base) { continue }
            if(!$indexedWriters.ContainsKey($write.table_base)) { $indexedWriters[$write.table_base]=@{} }
            $rows=$indexedWriters[$write.table_base]
            if(!$rows.ContainsKey($write.address)) {
                $rows[$write.address]=@{instruction=$write;address=$write.table_base;indexed=$true;owners=@()}
            }
            $rows[$write.address].owners+=@($function.entry)
        }
    }
    foreach($site in $index.sites) {
        $address = $site.instruction.address
        if($sites.ContainsKey($address)) { throw "Duplicate unresolved site: $address" }
        $sites[$address] = $site
        $tables = @(Unique @($observations[$address] | ForEach-Object { $_.table_candidates }))
        $anchor = $null
        if($site.source_slot) { $kind='slot'; $anchor=$site.source_slot; $id="slot-$($anchor.Substring(2))" }
        elseif($site.pattern -match '^indexed table (0x[0-9A-F]{8})$') { $kind='table'; $anchor=$Matches[1]; $id="table-$($anchor.Substring(2))" }
        elseif($tables.Count) { $kind='receiver-candidate'; $id='receiver-'+(($tables | ForEach-Object { $_.Substring(2) }) -join '-') }
        else {
            $kind='owner'
            $anchor = @($site.owners | Sort-Object @{Expression={$functions[$_].instruction_count}},@{Expression={$_}})[0]
            if(!$anchor) { throw "Unowned unresolved site: $address" }
            $id="owner-$($anchor.Substring(2))"
        }
        if(!$families.ContainsKey($id)) { $families[$id] = @{id=$id;kind=$kind;anchor=$anchor;tables=$tables;sites=[Collections.Generic.List[string]]::new()} }
        $families[$id].sites.Add($address)
    }
    if($sites.Count -ne $index.unique_sites) { throw 'Recovery site count does not match index.' }
    foreach($familyRecord in $families.Values) {
        $owners = @(Unique @($familyRecord.sites | ForEach-Object { $sites[$_].owners }))
        $writes = @(); $uses = @(); $stores = @(); $contracts = @()
        if($familyRecord.kind -eq 'slot') {
            $slot = Address $familyRecord.anchor
            $writes = @($index.absolute_slot_writes | Where-Object { (Address $_.address) -lt $slot+4 -and (Address $_.address)+$_.bytes -gt $slot })
            $uses = @($index.nearby_address_uses | Where-Object { $value=Address $_.value; $value -le $slot -and $slot-$value -lt 128 })
        }
        if($familyRecord.kind -eq 'table' -and $indexedWriters.ContainsKey($familyRecord.anchor)) {
            $writes = @($indexedWriters[$familyRecord.anchor].Values | Sort-Object { $_.instruction.address })
        }
        if($familyRecord.tables.Count) {
            $stores = @($index.object_pointer_stores | Where-Object { $_.value -in $familyRecord.tables })
            $contracts = @($registry.pointer_sources | Where-Object {
                @(@($_.table)+@($_.tables) | Where-Object { $_ -in $familyRecord.tables }).Count
            })
        }
        $tableRecords = @($registry.entry_tables | Where-Object { $_.start -in $familyRecord.tables -or $_.start -eq $familyRecord.anchor })
        $returnSources = @($contracts | Where-Object { $_.kind -like 'return-*' } | ForEach-Object { @($_.source)+@($_.sources) } | Where-Object { $_ })
        $producers = @(Unique (@(@($writes)+@($uses)+@($stores) | ForEach-Object { $_.owners }) + $returnSources))
        $producerCallers = @(Unique @($producers | ForEach-Object { $callers[$_] }))
        $ownerCallers = @(Unique @($owners | ForEach-Object { $callers[$_] }))
        $familyRecord.owners=$owners; $familyRecord.producers=$producers
        $familyRecord.caller_entries=@(Unique (@($producerCallers)+@($ownerCallers)))
        $familyRecord.writes=$writes; $familyRecord.address_uses=$uses; $familyRecord.pointer_stores=$stores
        $familyRecord.contracts=$contracts; $familyRecord.table_records=$tableRecords
        $familyRecord.count=$familyRecord.sites.Count
        $familyRecord.priority = if($writes.Count -or $stores.Count -or $familyRecord.kind -eq 'table') { 0 } elseif($uses.Count -or $familyRecord.kind -eq 'receiver-candidate') { 1 } else { 2 }
        $familyRecord.deferred_reason=$state.deferred[$familyRecord.id]
        $familyRecord.proof_needed = switch($familyRecord.kind) {
            'slot' { 'Enumerate initialization, all writers, aggregate aliases and resets; prove every target and call/tail ABI. Missing writers or null observations do not prove unreachability.' }
            'table' { 'Prove index bounds, exact table extent, initialization and all indexed/aliased writers, targets and call/jump/tail ABI.' }
            'receiver-candidate' { 'Trace every receiver producer and field writer, including alternatives; prove aliases and lifetime through calls/writes. Observed tables and matching method offsets are only leads.' }
            default { 'Recover the pointer origin through complete control flow and caller/argument or registration chains; enumerate targets and ABI.' }
        }
    }
    $ranked = @($families.Values | Sort-Object @{Expression={[bool]$_.deferred_reason}},priority,@{Expression='count';Descending=$true},id)
    $queue = @{format='smbd-call-workflow-v1';executable_sha256=$target;unresolved_sites=$sites.Count;
        family_count=$ranked.Count;deferred_sites=(@($ranked | Where-Object deferred_reason | Measure-Object count -Sum).Sum);
        observations_are_proof=$false;families=$ranked}
    Write-Json (Join-Path $work 'queue.json') $queue
    $ranked | Select-Object -First 12 @{n='family';e={$_.id}},count,priority,@{n='deferred';e={[bool]$_.deferred_reason}} | Format-Table -AutoSize | Out-Host
    Write-Host "$($sites.Count) sites in $($ranked.Count) families; every site assigned exactly once."
    if($Action -eq 'Check') {
        $state.active = $false
        Write-Json $statePath $state
        Write-Host "Checked $($closed.Count) closures; $($newSites.Count) remain. No code or target bindings were lost."
        if($discovered.Count) {
            Write-Host "Found $($discovered.Count) unresolved calls in newly verified targets; all remain in the recovery queue."
        }
        if($expandedReopens.Count) {
            Write-Host "Corrected $($expandedReopens.Count) previously closed functions after adding verified targets; their unresolved dependencies remain reported."
        }
    }
    if($Action -ne 'Prepare') { return }
    if($Family.Count) {
        foreach($id in $Family) { if(!$families.ContainsKey($id)) { throw "Unknown family: $id" } }
        $selected = @($Family | Sort-Object -Unique | ForEach-Object { $families[$_] })
    } else {
        $selected = [Collections.Generic.List[object]]::new()
        $available = @($ranked | Where-Object { !$_.deferred_reason })
        if($available.Count) { $selected.Add($available[0]) }
        # Prefer other sites sharing indexed producers or consumer functions.
        while($selected.Count -lt $FamilyLimit) {
            $sharedEntries = @(Unique @($selected | ForEach-Object { @($_.producers)+@($_.owners) }))
            $next = @($available | Where-Object { $_.id -notin $selected.id } | Sort-Object @{Expression={
                @(@($_.producers)+@($_.owners) | Where-Object { $_ -in $sharedEntries }).Count -gt 0
            };Descending=$true},priority,@{Expression='count';Descending=$true},id | Select-Object -First 1)
            if(!$next.Count) { break }; $selected.Add($next[0])
        }
    }
    if(!$selected.Count) { throw 'No available families. Review deferred reasons or the completed queue.' }
    $selectedSites = @(Unique @($selected | ForEach-Object { $_.sites }))
    $owners = @(Unique @($selected | ForEach-Object { $_.owners }))
    $producers = @(Unique @($selected | ForEach-Object { $_.producers }))
    $entries = @(Unique (@($owners)+@($producers)+@($selected | ForEach-Object { $_.caller_entries })))
    $packet = @{executable_sha256=$target;tool_sha256=(Get-FileHash -LiteralPath $tool -Algorithm SHA256).Hash;
        families=@($selected);sites=@($selectedSites | ForEach-Object { $sites[$_] });
        observations=@($selectedSites | ForEach-Object { $observations[$_] });
        owners=$owners;producers=$producers;functions=$entries;
        proof_complete=$false;note='Work packets never certify closure. Audit complete producer/alias coverage; edit shared facts; run Check.'}
    Copy-Item -LiteralPath 'build/generated/batch.json' -Destination (Join-Path $work 'baseline.json')
    Write-Json (Join-Path $work 'packet.json') $packet
    $state.active=$true
    Write-Json $statePath $state
    Inspect-Packet $packet
} finally {
    $timer.Stop()
    Pop-Location
}
