# Downloads the pinned build dependencies into .deps/ and verifies their hashes.
# Nothing is installed system-wide.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $root '.deps'
$downloads = Join-Path $deps 'dl'
New-Item -ItemType Directory -Force $downloads | Out-Null

$sdkVersion = '10.0.28000.2526'
$webView2Version = '1.0.2903.40'
$packages = @(
    @{ Name = "microsoft.windows.sdk.cpp.$sdkVersion"; Kind = 'zip'
       Url = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp/$sdkVersion/microsoft.windows.sdk.cpp.$sdkVersion.nupkg"
       Sha256 = 'be1b419491607eae6f7c57844ebab39face9643c51e2af1d9176a3ba0d0b23fc' },
    @{ Name = "microsoft.windows.sdk.cpp.x64.$sdkVersion"; Kind = 'zip'
       Url = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp.x64/$sdkVersion/microsoft.windows.sdk.cpp.x64.$sdkVersion.nupkg"
       Sha256 = 'a9cae2a8c5da7f5dc5838ae6a76d06d0d2e2fdc3d8cfc69ca6c184e4b9193a00' },
    @{ Name = "microsoft.windows.sdk.buildtools.$sdkVersion"; Kind = 'zip'
       Url = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.buildtools/$sdkVersion/microsoft.windows.sdk.buildtools.$sdkVersion.nupkg"
       Sha256 = 'a09a4c9d68160ced4765137a9a7444ea560ea86c45d6a77093dea58c2f7563a0' },
    @{ Name = "microsoft.web.webview2.$webView2Version"; Kind = 'zip'
       Url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$webView2Version/microsoft.web.webview2.$webView2Version.nupkg"
       Sha256 = 'ef128016dd1e51c59178c827ed5b8aa3322c57afa8675d930f8109505542ad74' },
    @{ Name = 'cmake-3.31.9-windows-x86_64'; Kind = 'zip-flat'
       Url = 'https://github.com/Kitware/CMake/releases/download/v3.31.9/cmake-3.31.9-windows-x86_64.zip'
       Sha256 = 'dc16867d0c0ffb35026976aff119335d5b47def44e76ae82ffafdc1f98b3d59d' },
    @{ Name = 'ninja-1.12.1'; Kind = 'zip'
       Url = 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip'
       Sha256 = 'f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a' }
)

foreach ($package in $packages) {
    $target = Join-Path $deps $package.Name
    if (Test-Path $target) { continue }

    $archive = Join-Path $downloads ([IO.Path]::GetFileName($package.Url))
    if (-not (Test-Path $archive)) {
        Write-Host "Downloading $($package.Name)"
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $package.Url -OutFile $archive -UseBasicParsing
    }
    $hash = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant()
    if ($hash -ne $package.Sha256) {
        Remove-Item $archive
        throw "Hash mismatch for $($package.Name): $hash"
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    switch ($package.Kind) {
        'zip' { [IO.Compression.ZipFile]::ExtractToDirectory($archive, $target) }
        # The archive already contains a top-level folder named like the package.
        'zip-flat' { [IO.Compression.ZipFile]::ExtractToDirectory($archive, $deps) }
    }
}
