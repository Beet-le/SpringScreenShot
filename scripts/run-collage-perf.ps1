[CmdletBinding()]
param(
    [ValidateSet("windows-msvc-performance", "snow-shot-msvc-arm64-performance")]
    [string]$Preset = "windows-msvc-performance",
    [ValidateSet(20, 100)]
    [int[]]$Images = @(20, 100),
    [string]$OutputDirectory = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$previousEnvironment = [Environment]::GetEnvironmentVariables("Process")
Push-Location $repoRoot
try {
    . (Join-Path $PSScriptRoot "snow-build-environment.ps1")
    $windowsTarget = Get-SnowWindowsTarget -Preset $Preset
    if ($windowsTarget.Architecture -cne $windowsTarget.HostArchitecture) {
        throw "Run collage performance measurements on a native $($windowsTarget.Architecture) host."
    }
    Set-SnowBuildEnvironment -Preset $Preset | Out-Null
    if (-not $SkipBuild) {
        & cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw "Collage benchmark configuration failed." }
        & cmake --build --preset "build-$Preset" --target snow-shot-collage-performance-benchmark --parallel
        if ($LASTEXITCODE -ne 0) { throw "Collage benchmark build failed." }
    }

    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
        $stamp = Get-Date -Format "yyyyMMdd-HHmmssfff"
        $OutputDirectory = Join-Path $repoRoot "build/$Preset/collage-performance/$stamp"
    }
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    $null = New-Item -ItemType Directory -Path $OutputDirectory -Force
    $projectPath = Join-Path $repoRoot "build/$Preset/snow_shot/snow-shot-collage-performance-benchmark.vcxproj"
    if (-not (Test-Path -LiteralPath $projectPath -PathType Leaf)) {
        throw "The configured benchmark project was not found. Run without -SkipBuild."
    }
    [xml]$benchmarkProject = Get-Content -Raw -LiteralPath $projectPath
    $releaseOutput = $benchmarkProject.SelectSingleNode(
        "//*[local-name()='OutDir' and contains(@Condition, 'Release')]")
    if (-not $releaseOutput) { throw "The benchmark project does not define a Release output directory." }
    $executable = Join-Path $releaseOutput.InnerText "snow-shot-collage-performance-benchmark.exe"
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "The Release collage benchmark was not found at $executable."
    }

    $env:QT_QPA_PLATFORM = "offscreen"
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $env:QTDIR "plugins/platforms"
    $runtime = Join-Path $windowsTarget.InstalledRoot "bin"
    $env:PATH = "$(Join-Path $env:QTDIR 'bin');$runtime;$env:PATH"

    $computer = Get-CimInstance Win32_ComputerSystem
    $processor = Get-CimInstance Win32_Processor | Select-Object -First 1
    $operatingSystem = Get-CimInstance Win32_OperatingSystem
    $qtManifest = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot "qt-toolchain.json") | ConvertFrom-Json
    $revision = (& git rev-parse HEAD).Trim()
    $metadata = [ordered]@{
        startedUtc = [DateTime]::UtcNow.ToString("o")
        preset = $Preset
        configuration = "Release"
        architecture = $windowsTarget.Architecture
        hostArchitecture = $windowsTarget.HostArchitecture
        operatingSystem = $operatingSystem.Caption
        operatingSystemVersion = $operatingSystem.Version
        processor = $processor.Name
        physicalMemoryBytes = [long]$computer.TotalPhysicalMemory
        qtVersion = $qtManifest.qtVersion
        qtDirectory = $env:QTDIR
        revision = $revision
        dirtyFiles = @(& git status --short).Count
        executableSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash.ToLowerInvariant()
        images = @($Images | Select-Object -Unique)
        imageSize = @(3840, 2160)
        editorSize = @(1200, 700)
        memorySamplingIntervalMs = 5
        clipboardPublished = $false
        fixture = "Opaque patterned RGB; independently decoded image rasters"
        memoryDefinitions = "Resident working set and Windows private committed bytes; clipboard admission estimates are separate from sampled memory."
        scenarioOrder = @("vertical unchanged", "vertical mixed transforms", "horizontal unchanged", "horizontal mixed transforms")
        results = @()
    }
    $metadataPath = Join-Path $OutputDirectory "environment.json"
    $metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $metadataPath -Encoding utf8

    foreach ($count in @($Images | Select-Object -Unique)) {
        $report = Join-Path $OutputDirectory "results-$count.json"
        $log = Join-Path $OutputDirectory "console-$count.log"
        & $executable --images $count.ToString() --output $report 2>&1 | Tee-Object -FilePath $log
        $exitCode = $LASTEXITCODE
        $metadata.results += [ordered]@{ images = $count; report = $report; log = $log; exitCode = $exitCode }
        $metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $metadataPath -Encoding utf8
        if ($exitCode -ne 0) {
            throw "The $count-image collage benchmark failed. Inspect $log."
        }
    }
    $metadata['finishedUtc'] = [DateTime]::UtcNow.ToString("o")
    $metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $metadataPath -Encoding utf8
    Write-Output "Collage benchmark reports: $OutputDirectory"
}
finally {
    Pop-Location
    $currentEnvironment = [Environment]::GetEnvironmentVariables("Process")
    foreach ($key in $currentEnvironment.Keys) {
        if (-not $previousEnvironment.Contains($key)) {
            [Environment]::SetEnvironmentVariable($key, $null, "Process")
        }
    }
    foreach ($key in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($key, $previousEnvironment[$key], "Process")
    }
}
