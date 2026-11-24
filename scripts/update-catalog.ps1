# Writes catalog.json: every Vitool Box app (repositories of VitomDev with the
# vitool-box topic) with its manifest, releases and README, plus the hub's own
# latest release. Vitool Box reads this file from raw.githubusercontent.com,
# which has no API rate limit. Run it after publishing a release.
#   .\scripts\update-catalog.ps1            writes catalog.json
#   .\scripts\update-catalog.ps1 -Publish   also commits and pushes it
# Needs the GitHub CLI (gh), signed in.

param([switch]$Publish)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$owner = 'VitomDev'
$hubRepo = 'Vitool-Box'

# Windows PowerShell re-encodes what native programs print, so gh writes to a
# file through cmd and the UTF-8 is read back untouched.
$scratch = [IO.Path]::GetTempFileName()
function Get-GitHub([string]$path, [string]$accept = 'application/vnd.github+json') {
    cmd /c "gh api -H `"Accept: $accept`" `"$path`" > `"$scratch`" 2>nul"
    if ($LASTEXITCODE -ne 0) { return $null }
    return [IO.File]::ReadAllText($scratch, [Text.Encoding]::UTF8)
}

function Release($release) {
    [ordered]@{
        tag_name     = $release.tag_name
        published_at = $release.published_at
        html_url     = $release.html_url
        body_html    = $release.body_html
        assets       = @($release.assets | ForEach-Object {
            [ordered]@{ name = $_.name; browser_download_url = $_.browser_download_url; digest = $_.digest; size = $_.size }
        })
    }
}

$search = Get-GitHub "search/repositories?q=topic:vitool-box+user:$owner&per_page=100" | ConvertFrom-Json
$apps = @()
foreach ($repo in $search.items | Sort-Object name) {
    $manifestText = Get-GitHub "repos/$owner/$($repo.name)/contents/vitool.json" 'application/vnd.github.raw'
    if (-not $manifestText) { Write-Host "  $($repo.name): no vitool.json, skipped"; continue }
    $releases = @((Get-GitHub "repos/$owner/$($repo.name)/releases?per_page=20" 'application/vnd.github.html+json' | ConvertFrom-Json) |
        Where-Object { -not $_.draft -and -not $_.prerelease })
    if (-not $releases) { Write-Host "  $($repo.name): no release, skipped"; continue }
    $apps += [ordered]@{
        repo       = [ordered]@{
            name = $repo.name; default_branch = $repo.default_branch; description = $repo.description
            html_url = $repo.html_url; created_at = $repo.created_at
        }
        manifest   = $manifestText | ConvertFrom-Json
        releases   = @($releases | ForEach-Object { Release $_ })
        readmeHtml = Get-GitHub "repos/$owner/$($repo.name)/readme" 'application/vnd.github.html'
    }
    Write-Host "  $($repo.name) $($releases[0].tag_name)"
}

$hub = Get-GitHub "repos/$owner/$hubRepo/releases/latest" | ConvertFrom-Json
$catalog = [ordered]@{
    version   = 1
    generated = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    apps      = $apps
    hub       = if ($hub) { Release $hub } else { $null }
}
$file = Join-Path $root 'catalog.json'
[IO.File]::WriteAllText($file, ($catalog | ConvertTo-Json -Depth 20 -Compress), [Text.UTF8Encoding]::new($false))
Remove-Item $scratch
Write-Host "catalog.json: $($apps.Count) apps"

if ($Publish) {
    # git prints its progress on stderr ("To https://github.com/..."). When this
    # script's output is redirected, Windows PowerShell turns those lines into
    # errors and 'Stop' would end the script before the website rebuild. Only
    # git's exit code decides whether it failed.
    function Invoke-Git {
        $ErrorActionPreference = 'Continue'
        & git -C $root @args 2>&1 | ForEach-Object { "$_" }
        if ($LASTEXITCODE -ne 0) { throw "git $($args[0]) failed with exit code $LASTEXITCODE" }
    }

    Invoke-Git add catalog.json
    git -C $root diff --cached --quiet -- catalog.json
    if ($LASTEXITCODE -eq 0) { Write-Host 'catalog.json is unchanged'; return }
    Invoke-Git commit -m 'Update catalog' -- catalog.json
    Invoke-Git push
    # The push starts .github/workflows/deploy-web.yml, which rebuilds the
    # website (vitoolbox.vitom.dev) so it shows the new versions.
    Write-Host 'The website will be rebuilt by GitHub Actions in about 2 minutes'
}
