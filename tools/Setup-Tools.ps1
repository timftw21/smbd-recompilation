param([string]$Destination = (Join-Path $PSScriptRoot '..\build\deps'))
$ErrorActionPreference = 'Stop'
$Destination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Path $Destination -Force | Out-Null

function Get-VerifiedArchive($Name, $Url, $Sha256) {
    $archive = Join-Path $Destination $Name
    if (!(Test-Path -LiteralPath $archive)) {
        Write-Host "Downloading $Name"
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $Sha256) {
        throw "Archive hash mismatch: $archive"
    }
    return $archive
}

$ghidraArchive = Get-VerifiedArchive 'ghidra_12.1.4_PUBLIC_20260921.zip' `
    'https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip' `
    'DDAC49F903DA9D5BAC833E5CC79395098B9C33CFD3279BE5F31BD00387D2D4DB'
$extensionArchive = Get-VerifiedArchive 'ghidra_12.1.4_PUBLIC_20261016_ghidra-xbe.zip' `
    'https://github.com/XboxDev/ghidra-xbe/releases/download/build-202610161023/ghidra_12.1.4_PUBLIC_20261016_ghidra-xbe.zip' `
    '339B2F67563BBB8B5217740319B7F3EAE9E9BBACF1F3A8AA29151F7ECB7D5152'
$ghidraHome = Join-Path $Destination 'ghidra_12.1.4_PUBLIC'
if (!(Test-Path -LiteralPath (Join-Path $ghidraHome 'support\analyzeHeadless.bat'))) {
    Expand-Archive -LiteralPath $ghidraArchive -DestinationPath $Destination
}
$extensions = Join-Path $ghidraHome 'Ghidra\Extensions'
if (!(Test-Path -LiteralPath (Join-Path $extensions 'ghidra-xbe\extension.properties'))) {
    New-Item -ItemType Directory -Path $extensions -Force | Out-Null
    Expand-Archive -LiteralPath $extensionArchive -DestinationPath $extensions
}
$support = Join-Path $ghidraHome 'support'
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if (($userPath -split ';') -notcontains $support) {
    [Environment]::SetEnvironmentVariable('Path', ($userPath.TrimEnd(';') + ';' + $support), 'User')
}
if (($env:Path -split ';') -notcontains $support) { $env:Path += ';' + $support }
Write-Host 'Verified Ghidra 12.1.4 and the matching Xbox extension.'
Write-Output $ghidraHome
