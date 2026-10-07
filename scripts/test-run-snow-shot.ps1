# Launcher contracts with a deterministic process fixture; no application launches.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$fixture = Join-Path ([IO.Path]::GetTempPath()) "snow-shot launch-$([guid]::NewGuid().ToString('N'))"
function Require($Value, [string]$Message) { if (-not $Value) { throw $Message } }
Add-Type @'
public class SnowLaunchProcessFixture {
    public bool Waited;
    public int Result;
    public void WaitForExit() { Waited = true; }
    public int ExitCode {
        get {
            if (!Waited) throw new System.InvalidOperationException("Process was not awaited.");
            return Result;
        }
    }
}
'@
try {
    $scripts = Join-Path $fixture 'scripts'
    $null = New-Item -ItemType Directory -Path $scripts
    $policy = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'static-qt-features.json') |
        ConvertFrom-Json
    foreach ($file in @('run-snow-shot.ps1', 'run-snow-shot.bat', 'snow-build-environment.ps1',
            'qt-toolchain.json', 'static-qt-features.json') + @($policy.windowsSourcePatches)) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $scripts
    }
    # Exercise the real architecture check with a minimal x64 PE header.
    $binary = New-Object byte[] 88
    [BitConverter]::GetBytes([uint16]0x5A4D).CopyTo($binary, 0)
    [BitConverter]::GetBytes([uint32]64).CopyTo($binary, 0x3C)
    [BitConverter]::GetBytes([uint32]0x00004550).CopyTo($binary, 64)
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($binary, 68)
    foreach ($target in @('snow_shot', 'snow_shot_mini')) {
        $directory = Join-Path $fixture "build/windows-msvc-debug/$target/Debug"
        $null = New-Item -ItemType Directory -Path $directory -Force
        [IO.File]::WriteAllBytes((Join-Path $directory "$target.exe"), $binary)
    }
    $global:SnowLaunchFixtureExitCode = 0
    $global:SnowLaunchFixtureProcess = $null
    $global:SnowLaunchPath = ''
    function Start-Process {
        param($FilePath, $WorkingDirectory, $WindowStyle, [switch]$PassThru)
        Require ($WindowStyle -eq 'Hidden') 'Launch helpers must not open a console.'
        Require ($WorkingDirectory -eq (Split-Path $FilePath)) 'Use the executable directory.'
        $global:SnowLaunchPath = $FilePath
        if ($PassThru) {
            $global:SnowLaunchFixtureProcess = [SnowLaunchProcessFixture]::new()
            $global:SnowLaunchFixtureProcess.Result = $global:SnowLaunchFixtureExitCode
            return $global:SnowLaunchFixtureProcess
        }
    }
    $launcher = Join-Path $scripts 'run-snow-shot.ps1'
    foreach ($edition in @('Full', 'Mini')) {
        $global:SnowLaunchFixtureProcess = $null
        & $launcher -Edition $edition -NoBuild
        Require $global:SnowLaunchFixtureProcess.Waited 'Foreground launches must wait for the GUI process.'
        $target = if ($edition -eq 'Mini') { 'snow_shot_mini' } else { 'snow_shot' }
        Require ($global:SnowLaunchPath.EndsWith("$target.exe")) 'Select the requested edition.'
    }
    $global:SnowLaunchFixtureExitCode = -1073741515
    $global:SnowLaunchFixtureProcess = $null
    $failure = ''
    try { & $launcher -Edition Mini -NoBuild } catch { $failure = $_.Exception.Message }
    Require ($failure -eq 'Snow Shot exited with code -1073741515.') 'Report the actual loader failure.'
    $global:SnowLaunchFixtureProcess = $null
    & $launcher -Edition Mini -NoBuild -Detached
    Require ($null -eq $global:SnowLaunchFixtureProcess) 'Detached launches must return immediately.'

    # A missing performance build exercises the real batch entry point and argument
    # forwarding without starting an application. Restrict PATH to force its 5.1 fallback.
    $originalPath = $env:Path
    $originalModulePath = $env:PSModulePath
    $originalErrorPreference = $ErrorActionPreference
    try {
        # Let each child host initialize its own built-in module search path.
        Remove-Item Env:PSModulePath -ErrorAction SilentlyContinue
        foreach ($path in @($originalPath, "$env:SystemRoot\System32;$env:SystemRoot\System32\WindowsPowerShell\v1.0")) {
            $env:Path = $path
            $ErrorActionPreference = 'Continue'
            $batch = Join-Path $scripts 'run-snow-shot.bat'
            $output = (& $env:ComSpec /d /c "`"$batch`" -NoBuild -Preset windows-msvc-performance -Edition Mini" 2>&1 |
                Out-String)
            $exitCode = $LASTEXITCODE
            $ErrorActionPreference = $originalErrorPreference
            Require ($exitCode -eq 1) 'The batch launcher must propagate a launch failure.'
            $expectedPath = Join-Path $fixture 'build\windows-msvc-performance\snow_shot_mini\Release\snow_shot_mini.exe'
            # PowerShell 7's concise error view wraps long paths with gutter bars.
            $normalizedOutput = $output -replace '[\s|]', ''
            $expectedOutput = "Snow Shot executable was not found at '$expectedPath'." -replace '\s', ''
            Require ($normalizedOutput.Contains($expectedOutput)) `
                "Both batch PowerShell hosts must load the helper and forward arguments. Output: $output"
        }
    }
    finally {
        $env:Path = $originalPath
        $env:PSModulePath = $originalModulePath
        $ErrorActionPreference = $originalErrorPreference
    }
    Write-Output "PASS: launcher contracts and batch PowerShell fallback (PowerShell $($PSVersionTable.PSVersion))."
}
finally {
    Remove-Variable -Scope Global -Name SnowLaunchFixtureExitCode, SnowLaunchFixtureProcess,
        SnowLaunchPath -ErrorAction SilentlyContinue
    $absolute = [IO.Path]::GetFullPath($fixture)
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $absolute.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unsafe fixture cleanup path.'
    }
    if (Test-Path -LiteralPath $absolute) { Remove-Item -LiteralPath $absolute -Recurse -Force }
}
