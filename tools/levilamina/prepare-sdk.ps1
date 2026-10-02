[CmdletBinding()]
param(
    [string]$OutputRoot = '',
    [string]$CacheRoot = '',
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$toolRoot = Join-Path $repoRoot 'tools\levilamina'
$sdkLockPath = Join-Path $toolRoot 'sdk-lock.json'
$runtimeLockPath = Join-Path $toolRoot 'runtime-lock.json'

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('spark-ll-' + [Guid]::NewGuid().ToString('N'))
}
if (-not [System.IO.Path]::IsPathRooted($OutputRoot)) {
    throw 'OutputRoot must be an absolute path'
}
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$OutputRoot = (Resolve-Path -LiteralPath $OutputRoot).Path
if (-not [string]::IsNullOrWhiteSpace($CacheRoot)) {
    if (-not [System.IO.Path]::IsPathRooted($CacheRoot)) {
        throw 'CacheRoot must be an absolute path'
    }
    New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null
    $CacheRoot = (Resolve-Path -LiteralPath $CacheRoot).Path
}

$archiveRoot = Join-Path $OutputRoot 'archives'
$sdkRoot = Join-Path $OutputRoot 'sdk'
$extractRoot = Join-Path $OutputRoot 'extract'
$receiptPath = Join-Path $OutputRoot 'setup-receipt.json'
New-Item -ItemType Directory -Force -Path $archiveRoot, $sdkRoot, $extractRoot | Out-Null

function Get-Hash {
    param([string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-OutputPath {
    param(
        [string]$Root,
        [string]$Path
    )

    $rootFull = [System.IO.Path]::GetFullPath($Root)
    $prefix = $rootFull.TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    $target = [System.IO.Path]::GetFullPath($Path)
    if (-not $target.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw ('path is outside the output root: ' + $Path)
    }
    return $target
}

function Assert-VerifiedFile {
    param(
        [string]$Path,
        [pscustomobject]$Item
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw ('archive is missing: ' + $Path)
    }
    $size = (Get-Item -LiteralPath $Path).Length
    $hash = Get-Hash -Path $Path
    $hasDeclaredSize = $Item.PSObject.Properties.Name -contains 'size' -and $null -ne $Item.size
    if (($hasDeclaredSize -and $size -ne [int64]$Item.size) -or $hash -ne $Item.sha256.ToLowerInvariant()) {
        throw ('archive verification failed for ' + $Item.id + ': size=' + $size + ' sha256=' + $hash)
    }
}

function Get-VerifiedGitSource {
    param([pscustomobject]$Item)

    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($null -eq $git) {
        throw 'git.exe is required to prepare the pinned LeviLamina source'
    }
    $target = Get-OutputPath -Root $extractRoot -Path (Join-Path $extractRoot $Item.id)
    if (Test-Path -LiteralPath $target) {
        $head = (& $git.Source -C $target rev-parse HEAD 2>$null).Trim().ToLowerInvariant()
        if ($LASTEXITCODE -ne 0 -or $head -ne $Item.commit.ToLowerInvariant()) {
            throw ('existing Git source does not match the pinned commit: ' + $target)
        }
        return $target
    }
    Write-Host ('cloning ' + $Item.id + ' ' + $Item.ref)
    & $git.Source clone --depth 1 --branch $Item.ref --single-branch $Item.url $target
    if ($LASTEXITCODE -ne 0) {
        throw ('git clone failed for ' + $Item.id)
    }
    $head = (& $git.Source -C $target rev-parse HEAD).Trim().ToLowerInvariant()
    if ($LASTEXITCODE -ne 0 -or $head -ne $Item.commit.ToLowerInvariant()) {
        throw ('Git source verification failed for ' + $Item.id + ': commit=' + $head)
    }
    Write-Host ('verified Git source ' + $Item.id + ' (' + $head + ')')
    return $target
}

function Get-VerifiedArchive {
    param([pscustomobject]$Item)

    $target = Get-OutputPath -Root $archiveRoot -Path (Join-Path $archiveRoot $Item.file)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    if (-not (Test-Path -LiteralPath $target -PathType Leaf) -and -not [string]::IsNullOrWhiteSpace($CacheRoot)) {
        $cacheCandidates = @(
            (Join-Path $CacheRoot $Item.file)
            (Join-Path $CacheRoot ('sdk\' + $Item.file))
            (Join-Path $CacheRoot ('runtime\' + $Item.file))
        )
        foreach ($candidate in $cacheCandidates) {
            $cached = Get-OutputPath -Root $CacheRoot -Path $candidate
            if (Test-Path -LiteralPath $cached -PathType Leaf) {
                Assert-VerifiedFile -Path $cached -Item $Item
                Copy-Item -LiteralPath $cached -Destination $target -Force
                break
            }
        }
    }
    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
        $partial = Get-OutputPath -Root $archiveRoot -Path ($target + '.partial')
        if (Test-Path -LiteralPath $partial -PathType Leaf) {
            Remove-Item -LiteralPath $partial -Force
        }
        Write-Host ('downloading ' + $Item.id + ' ' + $Item.version)
        Invoke-WebRequest -Uri $Item.url -OutFile $partial
        Assert-VerifiedFile -Path $partial -Item $Item
        Move-Item -LiteralPath $partial -Destination $target
    }
    Assert-VerifiedFile -Path $target -Item $Item
    Write-Host ('verified ' + $Item.id + ' (' + $Item.sha256.ToLowerInvariant() + ')')
    return $target
}

function Get-SafeArchivePath {
    param(
        [string]$Root,
        [string]$Entry
    )

    $normalized = $Entry.Replace('\', '/')
    $parts = @($normalized.Split('/', [System.StringSplitOptions]::RemoveEmptyEntries))
    if ([string]::IsNullOrWhiteSpace($normalized) -or $normalized.StartsWith('/') -or $normalized -match '^[A-Za-z]:' -or $parts -contains '..') {
        throw ('unsafe archive entry: ' + $Entry)
    }
    $rootFull = [System.IO.Path]::GetFullPath($Root)
    $prefix = $rootFull.TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    $target = [System.IO.Path]::GetFullPath((Join-Path $rootFull ($parts -join [System.IO.Path]::DirectorySeparatorChar)))
    if (-not $target.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw ('archive entry escapes extraction root: ' + $Entry)
    }
    return $target
}

function Expand-SafeArchive {
    param(
        [string]$ArchivePath,
        [string]$Destination
    )

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    if ($ArchivePath.EndsWith('.tar.gz', [System.StringComparison]::OrdinalIgnoreCase)) {
        $entries = @(tar.exe -tf $ArchivePath)
        if ($LASTEXITCODE -ne 0) {
            throw ('tar could not list ' + $ArchivePath)
        }
        foreach ($entry in $entries) {
            $null = Get-SafeArchivePath -Root $Destination -Entry ([string]$entry)
        }
        tar.exe -xf $ArchivePath -C $Destination
        if ($LASTEXITCODE -ne 0) {
            throw ('tar could not extract ' + $ArchivePath)
        }
        return
    }

    $archive = [System.IO.Compression.ZipFile]::OpenRead($ArchivePath)
    $seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    try {
        foreach ($entry in $archive.Entries) {
            $target = Get-SafeArchivePath -Root $Destination -Entry $entry.FullName
            if ($entry.FullName.EndsWith('/') -or $entry.FullName.EndsWith('\')) {
                New-Item -ItemType Directory -Force -Path $target | Out-Null
                continue
            }
            if (-not $seen.Add($target)) {
                throw ('duplicate archive entry: ' + $entry.FullName)
            }
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            $input = $entry.Open()
            $output = [System.IO.File]::Open($target, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
            try {
                $input.CopyTo($output)
            } finally {
                $output.Dispose()
                $input.Dispose()
            }
        }
    } finally {
        $archive.Dispose()
    }
}

function Get-ArchiveRoot {
    param([string]$Path)

    $roots = @(Get-ChildItem -LiteralPath $Path -Directory)
    if ($roots.Count -ne 1) {
        throw ('expected one outer source directory in ' + $Path)
    }
    return $roots[0].FullName
}

function Copy-FilteredTree {
    param(
        [string]$Source,
        [string]$Destination,
        [string[]]$Extensions = @('.h', '.hpp', '.in'),
        [string[]]$Files = @(),
        [string[]]$FilesGlob = @()
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        throw ('source directory is missing: ' + $Source)
    }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    $sourcePrefix = (Resolve-Path -LiteralPath $Source).Path.TrimEnd('\') + '\'
    if ($Files.Count -gt 0) {
        foreach ($fileName in $Files) {
            $file = Join-Path $Source $fileName
            if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
                throw ('dependency file is missing: ' + $file)
            }
            $target = Join-Path $Destination $fileName
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            Copy-Item -LiteralPath $file -Destination $target -Force
        }
        return
    }
    foreach ($file in Get-ChildItem -LiteralPath $Source -File -Recurse) {
        $include = $Extensions -contains $file.Extension
        if ($FilesGlob.Count -gt 0) {
            $include = $false
            foreach ($pattern in $FilesGlob) {
                if ($file.Name -like $pattern) {
                    $include = $true
                    break
                }
            }
        }
        if (-not $include) {
            continue
        }
        $relative = $file.FullName.Substring($sourcePrefix.Length)
        $target = Join-Path $Destination $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
    }
}

function Copy-WholeTree {
    param(
        [string]$Source,
        [string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        throw ('source directory is missing: ' + $Source)
    }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
        Copy-Item -LiteralPath $item.FullName -Destination (Join-Path $Destination $item.Name) -Recurse -Force
    }
}

function Assert-OutputIdentity {
    param(
        [hashtable]$Paths,
        [string]$Root
    )

    foreach ($property in $Paths.GetEnumerator()) {
        if ([string]::IsNullOrWhiteSpace([string]$property.Value)) {
            throw ('output path is empty: ' + $property.Key)
        }
        $null = Get-OutputPath -Root $Root -Path ([string]$property.Value)
    }
}

function Invoke-SelfTest {
    $selfRoot = Join-Path $OutputRoot 'self-test'
    New-Item -ItemType Directory -Force -Path $selfRoot | Out-Null
    $sample = Join-Path $selfRoot 'sample.bin'
    [System.IO.File]::WriteAllBytes($sample, [byte[]](1, 2, 3))
    $bad = [pscustomobject]@{ id = 'bad-hash'; size = 3; sha256 = ('0' * 64) }
    $badHashCaught = $false
    try {
        Assert-VerifiedFile -Path $sample -Item $bad
    } catch {
        $badHashCaught = $true
    }
    if (-not $badHashCaught) {
        throw 'self-test did not reject an incorrect archive hash'
    }
    $unsafeCaught = $false
    try {
        $null = Get-SafeArchivePath -Root $selfRoot -Entry '../escape'
    } catch {
        $unsafeCaught = $true
    }
    if (-not $unsafeCaught) {
        throw 'self-test did not reject archive traversal'
    }
    $inside = Join-Path $selfRoot 'inside.txt'
    [System.IO.File]::WriteAllText($inside, 'ok')
    $identityCaught = $false
    try {
        $null = Get-OutputPath -Root $selfRoot -Path (Join-Path $selfRoot '..\outside.txt')
    } catch {
        $identityCaught = $true
    }
    if (-not $identityCaught) {
        throw 'self-test did not reject an output path outside the root'
    }
    $safeSelfRoot = Get-OutputPath -Root $OutputRoot -Path $selfRoot
    Remove-Item -LiteralPath $safeSelfRoot -Recurse -Force
    Write-Host 'LeviLamina bootstrap self-tests passed'
}

if ($SelfTest) {
    Invoke-SelfTest
    exit 0
}

if (-not (Test-Path -LiteralPath $sdkLockPath -PathType Leaf) -or -not (Test-Path -LiteralPath $runtimeLockPath -PathType Leaf)) {
    throw 'public LeviLamina lock files are missing'
}
$sdkLock = Get-Content -Raw -LiteralPath $sdkLockPath | ConvertFrom-Json
$runtimeLock = Get-Content -Raw -LiteralPath $runtimeLockPath | ConvertFrom-Json
if ($sdkLock.lock_version -ne 1 -or $sdkLock.locked -ne $true -or $sdkLock.platform -ne 'windows-x64') {
    throw 'unsupported or unlocked SDK lock'
}
if ($runtimeLock.lock_version -ne 1 -or $runtimeLock.locked -ne $true -or $runtimeLock.platform -ne 'windows-x64') {
    throw 'unsupported or unlocked runtime lock'
}
if ($sdkLock.runtime.levilamina_version -ne '26.51.6' -or $sdkLock.runtime.levilamina_commit -ne '32fcaa02baa38371b705358801c7d185c284233e') {
    throw 'LeviLamina source pin is not the accepted public version'
}
if ($sdkLock.runtime.bedrock_runtime_data_version -ne '26.51.1-server.7') {
    throw 'Bedrock runtime data version is not the accepted public version'
}
foreach ($requiredArchiveId in @('levilamina-source', 'entt', 'expected-lite', 'fmt', 'gsl', 'glm', 'leveldb', 'magic_enum', 'nlohmann_json', 'rapidjson', 'type_safe', 'pcg_cpp', 'pfr', 'concurrentqueue', 'stb', 'parallel-hashmap', 'symbolprovider', 'prelink', 'bedrock-runtime-data')) {
    if (@($sdkLock.archives | Where-Object id -eq $requiredArchiveId).Count -ne 1) {
        throw ('SDK lock is missing required archive: ' + $requiredArchiveId)
    }
}
$runtimeItem = @($runtimeLock.artifacts | Where-Object id -eq 'levilamina')
if ($runtimeItem.Count -ne 1) {
    throw 'runtime lock must contain exactly one LeviLamina release artifact'
}
$runtimeItem = $runtimeItem[0]
$allItems = @($sdkLock.archives) + @($runtimeItem)
$archivePaths = @{}
$extractRoots = @{}
foreach ($item in $allItems) {
    if ($archivePaths.ContainsKey($item.id)) {
        throw ('duplicate archive id: ' + $item.id)
    }
    $isGitSource = $item.PSObject.Properties.Name -contains 'kind' -and $item.kind -eq 'git-source'
    if (-not $isGitSource) {
        $archivePaths[$item.id] = Get-VerifiedArchive -Item $item
    }
}
foreach ($item in $sdkLock.archives) {
    $isGitSource = $item.PSObject.Properties.Name -contains 'kind' -and $item.kind -eq 'git-source'
    if ($isGitSource) {
        $extractRoots[$item.id] = Get-VerifiedGitSource -Item $item
        continue
    }
    $destination = Join-Path $extractRoot $item.id
    Expand-SafeArchive -ArchivePath $archivePaths[$item.id] -Destination $destination
    if ($item.id -eq 'prelink' -or $item.id -eq 'bedrock-runtime-data') {
        $extractRoots[$item.id] = $destination
    } else {
        $extractRoots[$item.id] = Get-ArchiveRoot -Path $destination
    }
}
$runtimeExtract = Join-Path $extractRoot 'levilamina-release'
Expand-SafeArchive -ArchivePath $archivePaths['levilamina'] -Destination $runtimeExtract
$runtimeExtract = Get-ArchiveRoot -Path $runtimeExtract

$llRoot = $extractRoots['levilamina-source']
$llTooth = Get-Content -Raw -LiteralPath (Join-Path $llRoot 'tooth.json') | ConvertFrom-Json
if ($llTooth.version -ne $sdkLock.runtime.levilamina_version -or $llTooth.variants.Count -lt 1) {
    throw 'LeviLamina source tooth metadata has an unexpected version'
}
foreach ($layoutName in @('levilamina_common', 'levilamina_server')) {
    $layout = $sdkLock.layouts.PSObject.Properties[$layoutName].Value
    $sourceBase = Join-Path $llRoot $layout.source_root
    $destinationBase = Join-Path $sdkRoot $layout.destination
    foreach ($includeRoot in $layout.include_roots) {
        Copy-FilteredTree -Source (Join-Path $sourceBase $includeRoot) -Destination (Join-Path $destinationBase $includeRoot) -Extensions ([string[]]$layout.include_extensions)
    }
    if (@(Get-ChildItem -LiteralPath $destinationBase -File -Recurse).Count -eq 0) {
        throw ('no staged headers for ' + $layoutName)
    }
}

$dependencyProperties = $sdkLock.dependencies.PSObject.Properties
foreach ($property in $dependencyProperties) {
    $dependency = $property.Value
    $dependencyRoot = $extractRoots[$property.Name]
    $source = Join-Path $dependencyRoot $dependency.source
    $destination = Join-Path $sdkRoot $dependency.destination
    $files = @()
    $glob = @()
    if ($dependency.PSObject.Properties.Name -contains 'files') {
        $files = @($dependency.files)
    }
    if ($dependency.PSObject.Properties.Name -contains 'files_glob') {
        $glob = @($dependency.files_glob)
    }
    $extensions = @('.h', '.hpp', '.in', '.inl', '.c')
    if ($dependency.PSObject.Properties.Name -contains 'extensions') {
        $extensions = @($dependency.extensions)
    }
    Copy-FilteredTree -Source $source -Destination $destination -Extensions $extensions -Files $files -FilesGlob $glob
    if (@(Get-ChildItem -LiteralPath $destination -File -Recurse).Count -eq 0) {
        throw ('no staged public dependency files for ' + $property.Name)
    }
    if ($property.Name -eq 'expected-lite') {
        $expectedPath = Join-Path $destination $dependency.header_path
        $expectedHash = Get-Hash -Path $expectedPath
        if ($expectedHash -ne $dependency.header_sha256.ToLowerInvariant()) {
            throw ('expected-lite header hash mismatch: ' + $expectedHash)
        }
    }
}

$symbolRoot = Join-Path $sdkRoot 'sources\symbolprovider'
Copy-WholeTree -Source $extractRoots['symbolprovider'] -Destination $symbolRoot
$symbolProviderSource = Join-Path $symbolRoot 'src\SymbolProvider.cpp'
if (-not (Test-Path -LiteralPath $symbolProviderSource -PathType Leaf)) {
    throw 'pinned SymbolProvider.cpp is missing'
}
$symbolProviderHash = Get-Hash -Path $symbolProviderSource
if ($symbolProviderHash -ne '7478d26ef21ea417383bf276deba540126410a7abb700402f7cedd79e2bb79fd') {
    throw ('SymbolProvider.cpp hash mismatch: ' + $symbolProviderHash)
}

$prelink = Join-Path $sdkRoot 'tools\prelink\prelink.exe'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $prelink) | Out-Null
$prelinkSource = Join-Path $extractRoots['prelink'] 'bin\prelink.exe'
if (-not (Test-Path -LiteralPath $prelinkSource -PathType Leaf)) {
    throw 'pinned prelink.exe is missing'
}
Copy-Item -LiteralPath $prelinkSource -Destination $prelink -Force
$runtimeData = Join-Path $sdkRoot 'runtime-data\bedrock_runtime_data'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $runtimeData) | Out-Null
$runtimeDataSource = Join-Path $extractRoots['bedrock-runtime-data'] 'bedrock_runtime_data'
if (-not (Test-Path -LiteralPath $runtimeDataSource -PathType Leaf)) {
    throw 'pinned bedrock runtime data is missing'
}
Copy-Item -LiteralPath $runtimeDataSource -Destination $runtimeData -Force

$runtimeRoot = Join-Path $runtimeExtract 'LeviLamina'
if (-not (Test-Path -LiteralPath $runtimeRoot -PathType Container)) {
    $runtimeRoot = $runtimeExtract
}
$runtimeDllSource = Join-Path $runtimeRoot 'LeviLamina.dll'
$runtimePdbSource = Join-Path $runtimeRoot 'LeviLamina.pdb'
if (-not (Test-Path -LiteralPath $runtimeDllSource -PathType Leaf) -or -not (Test-Path -LiteralPath $runtimePdbSource -PathType Leaf)) {
    throw 'public LeviLamina release DLL/PDB is missing'
}
if ((Get-Item -LiteralPath $runtimeDllSource).Length -ne [int64]$runtimeItem.runtime_dll_size -or (Get-Hash -Path $runtimeDllSource) -ne $runtimeItem.runtime_dll_sha256) {
    throw 'public LeviLamina runtime DLL verification failed'
}
if ((Get-Item -LiteralPath $runtimePdbSource).Length -ne [int64]$runtimeItem.runtime_pdb_size -or (Get-Hash -Path $runtimePdbSource) -ne $runtimeItem.runtime_pdb_sha256) {
    throw 'public LeviLamina runtime PDB verification failed'
}
$runtimeDir = Join-Path $sdkRoot 'runtime'
New-Item -ItemType Directory -Force -Path $runtimeDir | Out-Null
$runtimeDll = Join-Path $runtimeDir 'LeviLamina.dll'
$runtimePdb = Join-Path $runtimeDir 'LeviLamina.pdb'
Copy-Item -LiteralPath $runtimeDllSource -Destination $runtimeDll -Force
Copy-Item -LiteralPath $runtimePdbSource -Destination $runtimePdb -Force

$paths = @{
    sdk_root = $sdkRoot
    runtime_dll = $runtimeDll
    runtime_pdb = $runtimePdb
    runtime_data = $runtimeData
    prelink = $prelink
    symbolprovider_source = $symbolProviderSource
}
Assert-OutputIdentity -Paths $paths -Root $OutputRoot
$receipt = [ordered]@{
    schema_version = 1
    task_id = 'levilamina_public_bootstrap'
    status = 'success'
    output_root = $OutputRoot
    versions = [ordered]@{
        levilamina = $sdkLock.runtime.levilamina_version
        levilamina_commit = $sdkLock.runtime.levilamina_commit
        bedrock_runtime_data = $sdkLock.runtime.bedrock_runtime_data_version
        symbolprovider = '6c93ec45c8455992ee726d92df60316c8e731c44'
        prelink = '0.8.6'
    }
    verified_archives = @($allItems | Where-Object { -not ($_.PSObject.Properties.Name -contains 'kind' -and $_.kind -eq 'git-source') } | ForEach-Object { [ordered]@{ id = $_.id; version = $_.version; size = if ($null -eq $_.size) { $null } else { [int64]$_.size }; sha256 = $_.sha256.ToLowerInvariant() } })
    verified_sources = @($sdkLock.archives | Where-Object { $_.PSObject.Properties.Name -contains 'kind' -and $_.kind -eq 'git-source' } | ForEach-Object { [ordered]@{ id = $_.id; ref = $_.ref; commit = $_.commit } })
    paths = [ordered]@{
        sdk_root = $sdkRoot
        runtime_dll = $runtimeDll
        runtime_pdb = $runtimePdb
        runtime_data = $runtimeData
        prelink = $prelink
        symbolprovider_source = $symbolProviderSource
    }
    validation = @(
        'all public archives passed SHA256 checks and declared size checks'
        'the LeviLamina source tag resolved to the pinned commit'
        'archive entries passed path traversal and duplicate-entry checks'
        'LeviLamina source headers and public dependencies were staged'
        'expected-lite header hash matched 4bf6a47f...'
        'SymbolProvider.cpp hash matched 7478d26e...'
        'LeviLamina runtime DLL/PDB hashes matched the public release lock'
        'no BDS server archive was downloaded or staged'
        'all staged build inputs are under the requested output root'
    )
}
$receipt | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $receiptPath -Encoding utf8
Write-Host ('LeviLamina bootstrap completed: ' + $receiptPath)
