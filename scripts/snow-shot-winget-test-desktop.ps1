#Requires -Version 7.0
# Desktop preparation for real installation tests, never for normal client provisioning.
function Initialize-SnowShotWingetTestDesktop {
    if ($env:RUNNER_ENVIRONMENT -ne 'github-hosted' -or $env:RUNNER_OS -ne 'Windows') {
        throw 'Desktop preparation requires a disposable GitHub-hosted Windows runner.'
    }

    # Windows ARM64 images can still be in first logon when the job starts, hiding
    # installer launch warnings behind the full-screen privacy experience.
    # https://github.com/actions/runner-images/issues/14069
    # Apply the documented policy before closing the already running experience.
    $policy = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\OOBE'
    if (-not (Test-Path -LiteralPath $policy -ErrorAction Stop)) {
        $null = New-Item -Path $policy -Force -ErrorAction Stop
    }
    $null = New-ItemProperty -LiteralPath $policy -Name DisablePrivacyExperience `
        -Value 1 -PropertyType DWord -Force -ErrorAction Stop

    $sessionId = (Get-Process -Id $PID -ErrorAction Stop).SessionId
    # Stop the broker first so it cannot relaunch its WWAHost privacy screen.
    foreach ($name in @('UserOOBEBroker', 'WWAHost')) {
        $processes = @(Get-Process -Name $name -ErrorAction SilentlyContinue |
            Where-Object { $_.SessionId -eq $sessionId })
        foreach ($process in $processes) {
            if ($process.HasExited) { continue }
            Write-Host "Closing runner first-logon process $name ($($process.Id))."
            Stop-Process -InputObject $process -Force -ErrorAction Stop
            if (-not $process.WaitForExit(5000)) {
                throw "Runner first-logon process $name did not exit."
            }
        }
    }
}
