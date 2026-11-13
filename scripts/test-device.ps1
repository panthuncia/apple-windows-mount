# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [Parameter(Mandatory=$true)][string]$Udid,
    [string]$Cli = 'build/windows-x64/bin/Release/awm.exe',
    [string]$AppBundleId,
    [string]$Report = 'build/hardware-validation.json'
)
$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
$executable = if ([IO.Path]::IsPathRooted($Cli)) { $Cli } else { [IO.Path]::GetFullPath((Join-Path $repository $Cli)) }
function Invoke-Awm([string[]]$Arguments) {
    $output = & $executable @Arguments --json
    if ($LASTEXITCODE) { throw "awm failed: $output" }
    return ($output | ConvertFrom-Json)
}
$null = Invoke-Awm @('start')
$mounted = Invoke-Awm @('mount', '--udid', $Udid, '--read-write')
$status = Invoke-Awm @('status')
$device = @($status.devices | Where-Object udid -eq $Udid)[0]
if (-not $device -or -not $device.drive) { throw "No mounted drive: $($status | ConvertTo-Json -Depth 8)" }
$drive = $device.drive.TrimEnd('\') + '\'
$results = [ordered]@{
    tested_at = [DateTime]::UtcNow.ToString('o')
    info = (Invoke-Awm @('info', '--udid', $Udid))
    doctor = (Invoke-Awm @('doctor'))
    transfer = (Invoke-Awm @('transfer-probe', '--udid', $Udid))
    sources = @()
}
$sources = @('Media')
if ($AppBundleId) { $sources += 'Apps\' + $AppBundleId }
foreach ($source in $sources) {
    $directory = Join-Path (Join-Path $drive $source) ('awm-validation-' + [Guid]::NewGuid().ToString('N'))
    $original = Join-Path $directory 'Résumé.bin'
    $renamed = Join-Path $directory 'renamed.bin'
    $created = $false
    try {
        $null = New-Item -ItemType Directory -Path $directory
        $created = $true
        $payload = New-Object byte[] (1024 * 1024)
        $random = [Security.Cryptography.RandomNumberGenerator]::Create()
        try { $random.GetBytes($payload) } finally { $random.Dispose() }
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $expected = [BitConverter]::ToString($sha.ComputeHash($payload)).Replace('-', '') } finally { $sha.Dispose() }
        [IO.File]::WriteAllBytes($original, $payload)
        if ((Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash -ne $expected) { throw 'Mounted transfer hash mismatch' }
        Move-Item -LiteralPath $original -Destination $renamed
        $stream = [IO.File]::Open($renamed, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        try { $stream.SetLength(12345); $stream.Flush() } finally { $stream.Dispose() }
        if ((Get-Item -LiteralPath $renamed).Length -ne 12345) { throw 'Truncation length mismatch' }
        $results.sources += [ordered]@{ source=$source; sha256=$expected; rename=$true; truncate=$true }
    } finally {
        # Delete only this invocation's exact files and empty directory.
        if ($created) {
            foreach ($file in @($original, $renamed)) {
                if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }
            }
            Remove-Item -LiteralPath $directory
        }
    }
}
$destination = [IO.Path]::GetFullPath((Join-Path $repository $Report))
$null = New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent)
$results | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $destination -Encoding UTF8
Write-Output "Hardware checks passed. Report: $destination"
