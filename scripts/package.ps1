param(
    [string]$BuildDirectory = 'build/windows-x64',
    [string]$StageDirectory = 'build/stage'
)
$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
$build = [IO.Path]::GetFullPath((Join-Path $repository $BuildDirectory))
$stage = [IO.Path]::GetFullPath((Join-Path $repository $StageDirectory))
& cmake --install $build --config Release --prefix $stage
if ($LASTEXITCODE) { throw 'CMake installation staging failed' }
$package = Join-Path $repository '.cache/winfsp-2.1.25156.msi'
if (-not (Test-Path -LiteralPath $package)) {
    New-Item -ItemType Directory -Force (Split-Path $package -Parent) | Out-Null
    Invoke-WebRequest 'https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25156.msi' -OutFile $package
}
if ((Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -ne '073A70E00F77423E34BED98B86E600DEF93393BA5822204FAC57A29324DB9F7A') { throw 'WinFsp installer checksum mismatch' }
if ((Get-AuthenticodeSignature -LiteralPath $package).Status -ne 'Valid') { throw 'WinFsp installer signature is invalid' }
& dotnet build (Join-Path $repository 'installer/app/App.wixproj') -c Release "-p:StageDir=$stage"
if ($LASTEXITCODE) { throw 'Application MSI build failed' }
& dotnet build (Join-Path $repository 'installer/bundle/Bundle.wixproj') -c Release "-p:WinFspMsi=$package"
if ($LASTEXITCODE) { throw 'Setup bundle build failed' }
