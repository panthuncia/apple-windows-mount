param(
    [string]$OutputDirectory = 'build/distribution/sources',
    [string]$VcpkgRoot = $env:VCPKG_ROOT
)
$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
if (-not $VcpkgRoot) { throw 'Set VCPKG_ROOT to the pinned vcpkg checkout used for the build' }
$output = [IO.Path]::GetFullPath((Join-Path $repository $OutputDirectory))
New-Item -ItemType Directory -Force -Path $output | Out-Null
$baseline = '40f3c709db80acf154ac4b17a1f83c564ebd022e'
$vcpkgArchive = Join-Path $output ('vcpkg-' + $baseline + '.tar.gz')
& git -C $VcpkgRoot archive --format=tar.gz "--output=$vcpkgArchive" $baseline
if ($LASTEXITCODE) { throw 'Cannot archive pinned dependency build recipes' }
$archives = @(
    'libusb-libusb-v1.0.30.tar.gz',
    'libimobiledevice-libplist-2.7.0.tar.gz',
    'libimobiledevice-libimobiledevice-glue-214bafdde6a1434ead87357afe6cb41b32318495.tar.gz',
    'libimobiledevice-libusbmuxd-8d30a559cf0585625d9d05dc8ce0dd495b1de4f4.tar.gz',
    'libimobiledevice-libimobiledevice-6fc41f57fc607df9b07446ca45bdf754225c9bd9.tar.gz',
    'openssl-openssl-openssl-3.6.3.tar.gz',
    'nlohmann-json-v3.12.0.tar.gz'
)
foreach ($archive in $archives) {
    $source = Join-Path (Join-Path $VcpkgRoot 'downloads') $archive
    if (-not (Test-Path -LiteralPath $source)) { throw "Build dependencies first. Source archive missing: $archive" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $output $archive)
}
$winfspSource = Join-Path $output 'winfsp-2.1.tar.gz'
if (-not (Test-Path -LiteralPath $winfspSource)) {
    Invoke-WebRequest 'https://github.com/winfsp/winfsp/archive/bcc52225ec7e6a9f5c889b5cdb8051adf41c4b91.tar.gz' -OutFile $winfspSource
}
$staging = Join-Path $repository ('.cache/source-stage-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $staging | Out-Null
try {
    $roots = @('src','tests','ports','third_party','installer','scripts','docs','.github')
    $rootFiles = @('.gitignore','CMakeLists.txt','CMakePresets.json','vcpkg.json','LICENSE','README.md','THIRD_PARTY.md')
    foreach ($file in $rootFiles) { Copy-Item -LiteralPath (Join-Path $repository $file) -Destination $staging }
    foreach ($root in $roots) {
        foreach ($file in Get-ChildItem -LiteralPath (Join-Path $repository $root) -Recurse -File -Force) {
            $relative = $file.FullName.Substring($repository.Length + 1)
            if ($relative -match '[\\/](bin|obj)[\\/]') { continue }
            $target = Join-Path $staging $relative
            New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $target
        }
    }
    Compress-Archive -Path (Join-Path $staging '*') -DestinationPath (Join-Path $output 'apple-windows-mount-source.zip') -Force
} finally {
    $resolved = [IO.Path]::GetFullPath($staging)
    $cache = [IO.Path]::GetFullPath((Join-Path $repository '.cache')) + [IO.Path]::DirectorySeparatorChar
    if ($resolved.StartsWith($cache, [StringComparison]::OrdinalIgnoreCase) -and (Split-Path $resolved -Leaf) -like 'source-stage-*') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
$manifest = @(Get-ChildItem -LiteralPath $output -File | Where-Object Name -ne 'source-manifest.json' | ForEach-Object {
    [ordered]@{file=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'source-manifest.json') -Encoding UTF8
