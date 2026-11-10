param(
    [string]$Output,
    [ValidateRange(1,3)][int]$Workers=1,
    [switch]$SkipBuild,
    [string]$SigningCertificateThumbprint,
    [uri]$TimestampUrl='http://timestamp.digicert.com',
    [switch]$RequireSignature
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(!$Output) {$Output=Join-Path $root 'build/release'}
$Output=[IO.Path]::GetFullPath($Output)
$SigningCertificateThumbprint=($SigningCertificateThumbprint -replace '\s','').ToUpperInvariant()
if($RequireSignature -and !$SigningCertificateThumbprint) {throw 'A trusted code-signing certificate is required. Set -SigningCertificateThumbprint.'}
if($SigningCertificateThumbprint -and $SigningCertificateThumbprint -notmatch '^[0-9A-F]{40}$') {throw 'The signing certificate thumbprint must contain 40 hexadecimal digits.'}
if($TimestampUrl.Scheme -notin @('http','https')) {throw 'The timestamp server must use HTTP or HTTPS.'}
function Invoke-Checked([string]$Program,[string[]]$Arguments) {
    & $Program @Arguments
    if($LASTEXITCODE -ne 0) {throw "$Program failed with exit code $LASTEXITCODE"}
}
function Sign-ReleaseFile([string]$Path) {
    Invoke-Checked $signTool @('sign','/q','/sha1',$SigningCertificateThumbprint,'/fd','SHA256','/tr',$TimestampUrl.AbsoluteUri,'/td','SHA256','/d','Super Monkey Ball Deluxe recompilation',$Path)
    Invoke-Checked $signTool @('verify','/q','/pa','/all','/tw',$Path)
    $signature=Get-AuthenticodeSignature -LiteralPath $Path
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $SigningCertificateThumbprint -or !$signature.TimeStamperCertificate) {
        throw "Release signature or timestamp verification failed: $Path"
    }
}
Push-Location $root
try {
    . (Join-Path $PSScriptRoot 'Initialize-Toolchain.ps1')
    if($SigningCertificateThumbprint) {
        $certificate=Get-Item -LiteralPath "Cert:\CurrentUser\My\$SigningCertificateThumbprint"
        if(!$certificate.HasPrivateKey) {throw 'The selected code-signing certificate has no private key.'}
        $chain=[Security.Cryptography.X509Certificates.X509Chain]::new()
        try {
            [void]$chain.ChainPolicy.ApplicationPolicy.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
            if(!$chain.Build($certificate)) {throw 'The code-signing certificate must be valid and chain to a trusted authority.'}
        } finally {$chain.Dispose()}
        $signTool=(Get-Command signtool.exe -CommandType Application -ErrorAction Stop).Source
    } else {Write-Warning 'No signing certificate selected. This release will be unsigned; publisher reputation cannot carry across unsigned releases.'}
    if(!$SkipBuild) { & (Join-Path $PSScriptRoot 'Build.ps1') -Stage Game -Workers $Workers }
    if(!$SkipBuild) {Invoke-Checked cmake @('--build','--preset','release','--target','smbd','smbd-tool','--parallel',"$Workers")}
    $package=Join-Path $root 'build/package'
    $stage=Join-Path $package ('stage-'+[Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage,$Output -Force | Out-Null
    # CMake's Runtime component is an explicit whitelist: no media, settings,
    # saves, generated source, profiles, debug maps, or build directories.
    Invoke-Checked cmake @('--install','build/windows','--config','Release','--prefix',$stage,'--component','Runtime')
    $files=@(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName)
    # Sign staged executables before hashing or compressing them. Keep valid
    # third-party signatures, including Microsoft's runtime DLL signatures.
    if($SigningCertificateThumbprint) {
        foreach($file in $files | Where-Object {$_.Extension -in @('.exe','.dll')}) {
            $signature=Get-AuthenticodeSignature -LiteralPath $file.FullName
            if($file.Name -in @('smbd.exe','smbd-tool.exe') -or $signature.Status -eq 'NotSigned') {Sign-ReleaseFile $file.FullName}
            elseif($signature.Status -ne 'Valid') {throw "Payload signature verification failed: $($file.FullName)"}
        }
        $files=@(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName)
    }
    $manifest=@('SMBD-PAYLOAD-V1')
    $cabinet=Join-Path $package 'payload.cab'
    $recipe=Join-Path $package 'payload.ddf'
    $directives=@('.OPTION EXPLICIT','.Set CabinetNameTemplate=payload.cab',('.Set DiskDirectoryTemplate="'+$package+'"'),
        '.Set Cabinet=on','.Set Compress=on','.Set CompressionType=LZX','.Set CompressionMemory=18',
        '.Set MaxDiskSize=0','.Set MaxCabinetSize=0','.Set MaxErrors=1',('.Set RptFileName="'+(Join-Path $package 'payload.rpt')+'"'),
        ('.Set InfFileName="'+(Join-Path $package 'payload.inf')+'"'))
    foreach($file in $files) {
        $relative=[IO.Path]::GetRelativePath($stage,$file.FullName)
        if($relative -match '[^\x20-\x7e]' -or $relative -match '[\r\n\t"]' -or $relative -match '^(data|logs|captures|build)[\\/]') {throw "Unexpected release file: $relative"}
        $manifest+=($relative.Replace('\','/')+"`t"+$file.Length+"`t"+(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)
        $directives+=('"'+$file.FullName+'" "'+$relative+'"')
    }
    $manifestPath=Join-Path $package 'payload.tsv'
    $manifestText=($manifest -join "`n")+"`n"
    $reuse=(Test-Path -LiteralPath $manifestPath) -and [IO.File]::ReadAllText($manifestPath) -ceq $manifestText -and (Test-Path -LiteralPath $cabinet)
    $cachePath=Join-Path $package 'payload-cache.json'
    if($reuse -and (Test-Path -LiteralPath $cachePath)) {
        $cached=Get-Content -LiteralPath $cachePath -Raw | ConvertFrom-Json
        $reuse=$cached.sha256 -eq (Get-FileHash -LiteralPath $cabinet -Algorithm SHA256).Hash
    } else {$reuse=$false}
    [IO.File]::WriteAllText($manifestPath,$manifestText,[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllLines($recipe,$directives,[Text.Encoding]::Default)
    if(!$reuse) {
        Write-Host 'Packing verified release files (one compression worker).'
        Invoke-Checked makecab.exe @('/V0','/F',$recipe)
        @{sha256=(Get-FileHash -LiteralPath $cabinet -Algorithm SHA256).Hash} | ConvertTo-Json | Set-Content -LiteralPath $cachePath -Encoding utf8
    } else {Write-Host 'Reusing the verified release payload.'}
    Invoke-Checked cmake @('--preset','windows',"-DSMBD_BUILD_WORKERS=$Workers","-DSMBD_INSTALLER_PAYLOAD=$($cabinet.Replace('\','/'))","-DSMBD_INSTALLER_MANIFEST=$($manifestPath.Replace('\','/'))")
    Invoke-Checked cmake @('--build','--preset','release','--target','smbd-setup','--parallel',"$Workers")
    $versionLine=Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') | Where-Object {$_ -match '^project\(smbd VERSION ([0-9.]+) '}
    if($versionLine -notmatch 'VERSION ([0-9.]+)') {throw 'Cannot determine the release version'}
    $version=$Matches[1]
    $installer=Join-Path $Output "smbd-$version-windows-x64-setup.exe"
    Copy-Item -LiteralPath (Join-Path $root 'build/windows/Release/smbd-setup.exe') -Destination $installer
    # The final signature covers the embedded, already-signed payload. Byte-for-
    # byte uninstaller copies preserve this same signature and timestamp.
    if($SigningCertificateThumbprint) {Sign-ReleaseFile $installer}
    $signature=Get-AuthenticodeSignature -LiteralPath $installer
    $checksum=(Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash
    [IO.File]::WriteAllText($installer+'.sha256',"$checksum  $([IO.Path]::GetFileName($installer))`n")
    [pscustomobject]@{format='smbd-release-v1';version=$version;platform='windows-x64';installer=[IO.Path]::GetFileName($installer);sha256=$checksum;
        payload_files=$files.Count;payload_bytes=($files | Measure-Object Length -Sum).Sum;source_commit=(& git rev-parse HEAD);
        source_modified=([bool](& git status --porcelain));includes_game_media=$false;
        authenticode_status=$signature.Status.ToString();signing_certificate=$SigningCertificateThumbprint;timestamped=([bool]$signature.TimeStamperCertificate)} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Output 'release.json') -Encoding utf8
    Write-Host "Installer ready: $installer"
    # Only remove the exact staging directory created by this invocation.
    if([IO.Path]::GetFullPath($stage).StartsWith($package+'\',[StringComparison]::OrdinalIgnoreCase) -and
       [IO.Path]::GetFileName($stage) -match '^stage-[0-9a-f]{32}$') {Remove-Item -LiteralPath $stage -Recurse -Force}
} finally {Pop-Location}
