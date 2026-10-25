#Requires -Version 5.1
<#
.SYNOPSIS
Git bundle로 MAVLink 수집기의 소스와 문서를 가져오거나 갱신합니다.
.DESCRIPTION
처음 실행하면 별도 작업 폴더를 만들고, 이후에는 main에서 이어지는 변경만
적용합니다. 작업 중인 파일이나 로컬 커밋은 자동으로 덮어쓰지 않습니다.
.PARAMETER SharedDirectory
LATEST.json과 bundle이 있는 폴더입니다. 기본값은 이 스크립트의 폴더입니다.
.PARAMETER Destination
소스와 문서를 읽거나 수정할 작업 폴더입니다. OneDrive 밖을 권장합니다.
.PARAMETER CacheDirectory
검증한 bundle과 동시 실행 방지 파일을 보관할 로컬 폴더입니다.
.EXAMPLE
.\Sync-MavlinkRouter.ps1
.EXAMPLE
.\Sync-MavlinkRouter.ps1 -SharedDirectory 'D:\OneDrive\공유 자료' -Destination 'C:\dev\mavlink_telemetry_data_router'
#>
[CmdletBinding()]
param(
    [string]$SharedDirectory = $PSScriptRoot,
    [string]$Destination = (Join-Path $env:USERPROFILE 'dev\mavlink_telemetry_data_router'),
    [string]$CacheDirectory = (Join-Path $env:LOCALAPPDATA 'mavlink-router\bundles')
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$script:GitPath = $null
$destinationLock = $null
$bundleReadLock = $null
$temporaryBundle = $null
$temporaryRef = $null
$temporaryRefRepository = $null
$oldConsoleOutputEncoding = [Console]::OutputEncoding
$oldOutputEncoding = $OutputEncoding
$exitCode = 1

function Get-FullDirectoryPath {
    param([string]$Path, [string]$Label)
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "$Label 경로가 비어 있습니다." }
    $fullPath = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetPathRoot($fullPath)
    while ($fullPath.Length -gt $root.Length -and ($fullPath.EndsWith('\') -or $fullPath.EndsWith('/'))) {
        $fullPath = $fullPath.Substring(0, $fullPath.Length - 1)
    }
    return $fullPath
}

function Test-WithinDirectory {
    param([string]$Path, [string]$Parent)
    if ([string]::Equals($Path, $Parent, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    $prefix = $Parent.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    return $Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}

function Invoke-Git {
    param([string[]]$Arguments, [string]$FailureMessage, [switch]$AllowFailure)
    # Windows PowerShell 5.1은 정상적인 Git의 stderr도 ErrorRecord로 만듭니다.
    # 네이티브 종료 코드를 직접 확인하며 호출자의 오류 정책은 되돌립니다.
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $PSNativeCommandUseErrorActionPreference = $false
    try {
        $nativeOutput = @(& $script:GitPath @Arguments 2>&1)
        $nativeExitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $savedPreference }
    $lines = @($nativeOutput | ForEach-Object { $_.ToString() })
    if ($nativeExitCode -ne 0 -and -not $AllowFailure) {
        $details = $lines -join [Environment]::NewLine
        throw ($FailureMessage + [Environment]::NewLine + $details)
    }
    return [pscustomobject]@{ ExitCode = $nativeExitCode; Lines = $lines }
}

function Assert-BundleFile {
    param([string]$Path, [string]$ExpectedHash, $ExpectedBytes)
    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($item.PSIsContainer) { throw 'bundle 경로가 파일이 아닌 폴더입니다.' }
    if ($null -ne $ExpectedBytes -and $item.Length -ne $ExpectedBytes) {
        throw 'bundle 크기가 배포 목록과 다릅니다. OneDrive 동기화가 끝난 뒤 다시 실행하세요.'
    }
    $hashAlgorithm = [Security.Cryptography.SHA256]::Create()
    $hashStream = [IO.File]::OpenRead($Path)
    try {
        $actualHash = ([BitConverter]::ToString($hashAlgorithm.ComputeHash($hashStream))).Replace('-', '')
    }
    finally {
        $hashStream.Dispose()
        $hashAlgorithm.Dispose()
    }
    if ($actualHash -ine $ExpectedHash) {
        throw 'bundle의 SHA256이 배포 목록과 다릅니다. 파일이 끝까지 복사되었는지 확인하세요.'
    }
}

function Set-BundleRemote {
    param([string]$Repository, [string]$Bundle)
    $remotes = Invoke-Git -Arguments @('-C', $Repository, 'remote') -FailureMessage 'Git 연결 목록을 읽지 못했습니다.'
    if (@($remotes.Lines) -contains 'onedrive') {
        $null = Invoke-Git -Arguments @('-C', $Repository, 'remote', 'set-url', 'onedrive', $Bundle) -FailureMessage 'onedrive 연결 경로를 갱신하지 못했습니다.'
    }
    else {
        $null = Invoke-Git -Arguments @('-C', $Repository, 'remote', 'add', 'onedrive', $Bundle) -FailureMessage 'onedrive 연결을 추가하지 못했습니다.'
    }
}

try {
    # 이 프로세스에서만 UTF-8을 사용합니다. Git이 돌려주는 한글 경로도 보존합니다.
    [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
    $OutputEncoding = New-Object Text.UTF8Encoding($false)
    $gitCommand = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $gitCommand) {
        throw 'Git이 설치되어 있지 않거나 PATH에 없습니다. Git for Windows를 설치한 뒤 새 PowerShell 창에서 다시 실행하세요.'
    }
    $script:GitPath = $gitCommand.Source
    $sharedPath = Get-FullDirectoryPath -Path $SharedDirectory -Label '공유 폴더'
    $destinationPath = Get-FullDirectoryPath -Path $Destination -Label '작업 폴더'
    $cachePath = Get-FullDirectoryPath -Path $CacheDirectory -Label '임시 보관 폴더'
    if ($destinationPath -eq [IO.Path]::GetPathRoot($destinationPath)) {
        throw '드라이브의 최상위 폴더를 작업 폴더로 사용할 수 없습니다. 하위 폴더를 지정하세요.'
    }
    if ((Test-WithinDirectory $cachePath $destinationPath) -or
        (Test-WithinDirectory $destinationPath $cachePath) -or
        (Test-WithinDirectory $cachePath $sharedPath) -or
        (Test-WithinDirectory $sharedPath $cachePath)) {
        throw '임시 보관 폴더는 작업 폴더 및 공유 폴더와 겹치지 않는 별도 경로로 지정하세요.'
    }
    if ((Test-WithinDirectory $destinationPath $sharedPath) -or
        (Test-WithinDirectory $sharedPath $destinationPath)) {
        throw '작업 폴더와 공유 폴더는 서로 겹치지 않는 경로로 지정하세요.'
    }
    $manifestPath = Join-Path $sharedPath 'LATEST.json'
    $manifestItem = Get-Item -LiteralPath $manifestPath -ErrorAction Stop
    if ($manifestItem.PSIsContainer -or $manifestItem.Length -gt 1048576) {
        throw 'LATEST.json이 올바른 배포 목록 파일이 아닙니다.'
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($name in @('format', 'source_branch', 'source_commit', 'bundle')) {
        if ($null -eq $manifest.PSObject.Properties[$name]) { throw "LATEST.json에 $name 항목이 없습니다." }
    }
    if ($manifest.format -cne 'mavlink-router-handoff/v1' -or $manifest.source_branch -cne 'main') {
        throw '지원하지 않는 배포 목록 형식 또는 브랜치입니다.'
    }
    if ($manifest.source_commit -isnot [string] -or $manifest.source_commit -notmatch '\A[0-9a-fA-F]{40}\z') {
        throw '배포 목록의 커밋 번호가 올바르지 않습니다.'
    }
    foreach ($name in @('file', 'sha256')) {
        if ($null -eq $manifest.bundle -or $null -eq $manifest.bundle.PSObject.Properties[$name]) {
            throw "배포 목록의 bundle.$name 항목이 없습니다."
        }
    }
    $bundleName = $manifest.bundle.file
    if ($bundleName -isnot [string] -or $bundleName -notmatch '\A[^<>:"/\\|?*\x00-\x1f]+\.bundle\z' -or
        $bundleName.Trim() -cne $bundleName -or $bundleName.EndsWith('.') -or
        ($bundleName.Split('.')[0] -match '\A(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])\z')) {
        throw '배포 목록의 bundle 파일 이름이 올바르지 않습니다. 폴더 경로는 넣을 수 없습니다.'
    }
    $expectedHash = $manifest.bundle.sha256
    if ($expectedHash -isnot [string] -or $expectedHash -notmatch '\A[0-9a-fA-F]{64}\z') {
        throw '배포 목록의 SHA256이 올바르지 않습니다.'
    }
    $expectedBytes = $null
    if ($null -ne $manifest.bundle.PSObject.Properties['bytes']) {
        $bytesValue = $manifest.bundle.bytes
        if ($null -eq $bytesValue -or $bytesValue -isnot [ValueType] -or $bytesValue -is [bool]) {
            throw '배포 목록의 bundle 크기가 올바르지 않습니다.'
        }
        $byteCount = [decimal]$bytesValue
        if ($byteCount -le 0 -or $byteCount -gt [long]::MaxValue -or [decimal]::Truncate($byteCount) -ne $byteCount) {
            throw '배포 목록의 bundle 크기는 양의 정수여야 합니다.'
        }
        $expectedBytes = [long]$byteCount
    }
    $expectedCommit = $manifest.source_commit.ToLowerInvariant()
    $sourceBundle = Join-Path $sharedPath $bundleName
    $null = New-Item -ItemType Directory -Path $cachePath -Force
    $lockDirectory = Join-Path $cachePath 'locks'
    $null = New-Item -ItemType Directory -Path $lockDirectory -Force
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $lockKey = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($destinationPath.ToUpperInvariant())))).Replace('-', '').ToLowerInvariant()
    }
    finally { $sha.Dispose() }
    try {
        $destinationLock = [IO.File]::Open((Join-Path $lockDirectory ($lockKey + '.lock')), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    }
    catch { throw '같은 작업 폴더를 갱신하는 프로그램이 이미 실행 중입니다. 끝난 뒤 다시 실행하세요.' }

    $initialClone = $true
    if (Test-Path -LiteralPath $destinationPath) {
        $destinationItem = Get-Item -LiteralPath $destinationPath -Force
        if (-not $destinationItem.PSIsContainer) { throw '작업 폴더 경로에 같은 이름의 파일이 있습니다.' }
        $existingFiles = @(Get-ChildItem -LiteralPath $destinationPath -Force | Select-Object -First 1)
        if ($existingFiles.Count -gt 0) {
            $topLevel = Invoke-Git -Arguments @('-C', $destinationPath, 'rev-parse', '--show-toplevel') -AllowFailure
            if ($topLevel.ExitCode -ne 0 -or $topLevel.Lines.Count -ne 1) {
                throw '작업 폴더가 비어 있지 않고 Git 저장소도 아닙니다. 새 폴더를 지정하세요.'
            }
            $actualRoot = Get-FullDirectoryPath -Path $topLevel.Lines[0] -Label 'Git 작업 폴더'
            if (-not [string]::Equals($actualRoot, $destinationPath, [StringComparison]::OrdinalIgnoreCase)) {
                throw '다른 Git 저장소의 하위 폴더입니다. 해당 저장소의 최상위 경로를 지정하거나 새 폴더를 사용하세요.'
            }
            $status = Invoke-Git -Arguments @('-C', $destinationPath, 'status', '--porcelain=v1', '--untracked-files=all') -FailureMessage '작업 파일의 상태를 읽지 못했습니다.'
            if ($status.Lines.Count -gt 0) { throw '수정하거나 새로 만든 파일이 있습니다. 먼저 작업을 커밋하거나 별도로 보관한 뒤 다시 실행하세요.' }
            $branch = Invoke-Git -Arguments @('-C', $destinationPath, 'symbolic-ref', '--quiet', 'HEAD') -AllowFailure
            if ($branch.ExitCode -ne 0 -or ($branch.Lines -join '') -cne 'refs/heads/main') {
                throw '현재 main 브랜치가 아닙니다. 작업을 마친 뒤 main으로 이동하고 다시 실행하세요.'
            }
            $initialClone = $false
        }
    }

    Write-Host '배포 목록과 bundle을 확인하고 있습니다.'
    $cachedBundle = Join-Path $cachePath ($expectedHash.ToLowerInvariant() + '.bundle')
    if (-not (Test-Path -LiteralPath $cachedBundle)) {
        $temporaryBundle = Join-Path $cachePath ('copy-' + [guid]::NewGuid().ToString('N') + '.bundle')
        Copy-Item -LiteralPath $sourceBundle -Destination $temporaryBundle
        Assert-BundleFile -Path $temporaryBundle -ExpectedHash $expectedHash -ExpectedBytes $expectedBytes
        try { [IO.File]::Move($temporaryBundle, $cachedBundle) }
        catch {
            if (-not (Test-Path -LiteralPath $cachedBundle)) { throw }
            # 다른 작업 폴더의 갱신이 같은 내용을 먼저 복사했을 수 있습니다.
        }
    }
    $bundleReadLock = [IO.File]::Open($cachedBundle, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    Assert-BundleFile -Path $cachedBundle -ExpectedHash $expectedHash -ExpectedBytes $expectedBytes
    $heads = Invoke-Git -Arguments @('bundle', 'list-heads', $cachedBundle, 'refs/heads/main') -FailureMessage 'bundle에 담긴 main 이력을 읽지 못했습니다.'
    if ($heads.Lines.Count -ne 1 -or $heads.Lines[0] -notmatch '\A([0-9a-fA-F]{40}) refs/heads/main\z' -or $Matches[1] -ine $expectedCommit) {
        throw 'bundle의 main 커밋과 배포 목록이 일치하지 않습니다.'
    }

    if ($initialClone) {
        Write-Host '처음 가져오는 PC입니다. 작업 폴더를 만들고 있습니다.'
        $parentDirectory = Split-Path -Parent $destinationPath
        $null = New-Item -ItemType Directory -Path $parentDirectory -Force
        $null = Invoke-Git -Arguments @('clone', '--origin', 'onedrive', '--branch', 'main', $cachedBundle, $destinationPath) -FailureMessage '작업 폴더를 만들지 못했습니다. 표시된 Git 오류를 확인하세요.'
        $null = Invoke-Git -Arguments @('-C', $destinationPath, 'bundle', 'verify', $cachedBundle) -FailureMessage '가져온 bundle의 Git 이력을 검증하지 못했습니다.'
    }
    else {
        $null = Invoke-Git -Arguments @('-C', $destinationPath, 'bundle', 'verify', $cachedBundle) -FailureMessage 'bundle의 Git 이력을 검증하지 못했습니다.'
        $head = Invoke-Git -Arguments @('-C', $destinationPath, 'rev-parse', 'HEAD') -FailureMessage '현재 커밋을 읽지 못했습니다.'
        if (($head.Lines -join '') -ine $expectedCommit) {
            $temporaryRef = 'refs/mavlink-router-sync/' + [guid]::NewGuid().ToString('N')
            $temporaryRefRepository = $destinationPath
            $null = Invoke-Git -Arguments @('-C', $destinationPath, 'fetch', '--no-tags', '--no-write-fetch-head', $cachedBundle, ('refs/heads/main:' + $temporaryRef)) -FailureMessage 'bundle에서 새 이력을 가져오지 못했습니다.'
            $ancestry = Invoke-Git -Arguments @('-C', $destinationPath, 'merge-base', '--is-ancestor', 'HEAD', $temporaryRef) -AllowFailure
            if ($ancestry.ExitCode -ne 0) {
                throw '이 bundle은 현재 작업에서 이어지는 버전이 아닙니다. 더 오래된 배포본이거나 로컬 커밋 또는 다른 저장소의 이력이 있으므로 변경을 적용하지 않았습니다.'
            }
            Write-Host '새 변경을 작업 폴더에 적용하고 있습니다.'
            $null = Invoke-Git -Arguments @('-C', $destinationPath, 'merge', '--ff-only', $temporaryRef) -FailureMessage '변경을 적용하지 못했습니다. 작업 폴더의 Git 상태를 확인하세요.'
        }
        Set-BundleRemote -Repository $destinationPath -Bundle $cachedBundle
        $null = Invoke-Git -Arguments @('-C', $destinationPath, 'update-ref', 'refs/remotes/onedrive/main', $expectedCommit) -FailureMessage 'onedrive/main의 커밋 표시를 갱신하지 못했습니다.'
    }
    $finalHead = Invoke-Git -Arguments @('-C', $destinationPath, 'rev-parse', 'HEAD') -FailureMessage '갱신 결과를 확인하지 못했습니다.'
    if (($finalHead.Lines -join '') -ine $expectedCommit) { throw '갱신 후 커밋이 배포 목록과 다릅니다.' }
    Write-Host ''
    Write-Host ('준비되었습니다. 커밋: ' + $expectedCommit.Substring(0, 12)) -ForegroundColor Green
    Write-Host ('작업 폴더: ' + $destinationPath)
    Write-Host ('서버 API 문서: ' + (Join-Path $destinationPath 'docs\SERVER_API.md'))
    $exitCode = 0
}
catch {
    Write-Host ''
    Write-Host ('진행하지 못했습니다: ' + $_.Exception.Message) -ForegroundColor Red
}
finally {
    if ($null -ne $temporaryRef -and $null -ne $temporaryRefRepository) {
        $cleanup = Invoke-Git -Arguments @('-C', $temporaryRefRepository, 'update-ref', '-d', $temporaryRef) -AllowFailure
        if ($cleanup.ExitCode -ne 0) { Write-Warning ('임시 Git 참조를 지우지 못했습니다: ' + $temporaryRef) }
    }
    if ($null -ne $bundleReadLock) { $bundleReadLock.Dispose() }
    if ($null -ne $temporaryBundle -and (Test-Path -LiteralPath $temporaryBundle)) {
        Remove-Item -LiteralPath $temporaryBundle -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $destinationLock) { $destinationLock.Dispose() }
    [Console]::OutputEncoding = $oldConsoleOutputEncoding
    $OutputEncoding = $oldOutputEncoding
}
exit $exitCode