@echo off
setlocal
if /I "%~1"=="/?" goto :usage
if /I "%~1"=="-h" goto :usage
if /I "%~1"=="--help" goto :usage

set "T3CONV_RESTORE_SCRIPT=%~f0"
set "T3CONV_RESTORE_NO_START=0"
set "T3CONV_RESTORE_TIMEOUT=45"

:parse_args
if "%~1"=="" goto :run
if /I "%~1"=="-NoStart" (
    set "T3CONV_RESTORE_NO_START=1"
    shift
    goto :parse_args
)
if /I "%~1"=="-StartupTimeoutSeconds" (
    if "%~2"=="" goto :bad_timeout
    set "T3CONV_RESTORE_TIMEOUT=%~2"
    shift
    shift
    goto :parse_args
)
echo Unknown argument: %~1
echo.
call :print_usage
exit /b 2

:bad_timeout
echo Missing value for -StartupTimeoutSeconds.
exit /b 2

:run
powershell.exe -NoProfile -ExecutionPolicy Bypass -EncodedCommand JABFAHIAcgBvAHIAQQBjAHQAaQBvAG4AUAByAGUAZgBlAHIAZQBuAGMAZQAgAD0AIAAnAFMAdABvAHAAJwAKACQAUAByAG8AZwByAGUAcwBzAFAAcgBlAGYAZQByAGUAbgBjAGUAIAA9ACAAJwBTAGkAbABlAG4AdABsAHkAQwBvAG4AdABpAG4AdQBlACcACgB0AHIAeQAgAHsACgAgACAAIAAgACQAYwBvAG4AdABlAG4AdAAgAD0AIABHAGUAdAAtAEMAbwBuAHQAZQBuAHQAIAAtAEwAaQB0AGUAcgBhAGwAUABhAHQAaAAgACQAZQBuAHYAOgBUADMAQwBPAE4AVgBfAFIARQBTAFQATwBSAEUAXwBTAEMAUgBJAFAAVAAgAC0AUgBhAHcACgAgACAAIAAgACQAcABhAHIAdABzACAAPQAgACQAYwBvAG4AdABlAG4AdAAgAC0AcwBwAGwAaQB0ACAAJwAoAD8AbQApAF4AIwAgAFAATwBXAEUAUgBTAEgARQBMAEwALQBCAEUARwBJAE4AXABzACoAJAAnACwAIAAyAAoAIAAgACAAIABpAGYAIAAoACQAcABhAHIAdABzAC4AQwBvAHUAbgB0ACAALQBsAHQAIAAyACkAIAB7AAoAIAAgACAAIAAgACAAIAAgAHQAaAByAG8AdwAgACcARQBtAGIAZABkAGQAZQBkACAAUABvAHcAZQByAFMAaABlAGwAbAAgAGIAbABvAGMAawAgAG4AbwB0ACAAZgBvAHUAbgBkAC4AJwAKACAAIAAgACAAfQAKACAAIAAgACAAJgAgACgAWwBTAGMAcgBpAHAAdABCAGwAbwBjAGsAXQA6ADoAQwByAGUAYQB0AGUAKAAkAHAAYQByAHQAcwBbADEAXQApACkACgB9ACAAYwBhAHQAYwBoACAAewAKACAAIAAgACAAWwBDAG8AbgBzAG8AbABlAF0AOgA6AEUAcgByAG8AcgAuAFcAcgBpAHQAZQBMAGkAbgBlACgAJABfAC4ARQB4AGMAZQBwAHQAaQBvAG4ALgBNAGUAcwBzAGEAZwBlACkACgAgACAAIAAgAGUAeABpAHQAIAAxAAoAfQAKAA==
set "T3CONV_RESTORE_EXIT=%ERRORLEVEL%"
exit /b %T3CONV_RESTORE_EXIT%

:usage
call :print_usage
exit /b 0

:print_usage
echo Usage: restore-autocad-settings.cmd [-NoStart] [-StartupTimeoutSeconds seconds]
echo.
echo Restores common AutoCAD interactive settings after conversion or manual debugging.
echo Re-running it is safe because it writes the same ordinary AutoCAD variables each time.
exit /b 0

# POWERSHELL-BEGIN
$ErrorActionPreference = 'Stop'

function Get-AutoCadApplication {
    try {
        return [Runtime.InteropServices.Marshal]::GetActiveObject('AutoCAD.Application')
    } catch {
        if ($script:NoStart) {
            throw 'AutoCAD is not running. Start AutoCAD and rerun this script, or omit -NoStart.'
        }
        $application = New-Object -ComObject 'AutoCAD.Application'
        $application.Visible = $true
        return $application
    }
}

function Get-AutoCadDocument {
    param(
        [Parameter(Mandatory = $true)]
        $Application
    )

    $deadline = (Get-Date).AddSeconds($script:StartupTimeoutSeconds)
    do {
        try {
            if ($Application.Documents.Count -gt 0) {
                return $Application.ActiveDocument
            }
        } catch {
            Start-Sleep -Milliseconds 500
            continue
        }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)

    return $Application.Documents.Add()
}

function Set-AutoCadVariable {
    param(
        [Parameter(Mandatory = $true)]
        $Document,
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        $Value
    )

    try {
        $Document.SetVariable($Name, $Value)
        Write-Host "$Name=$Value"
    } catch {
        Write-Warning "failed_setvar_$Name=$($_.Exception.Message)"
    }
}

$script:NoStart = $env:T3CONV_RESTORE_NO_START -eq '1'
$script:StartupTimeoutSeconds = 45
if ($env:T3CONV_RESTORE_TIMEOUT -match '^\d+$') {
    $script:StartupTimeoutSeconds = [int]$env:T3CONV_RESTORE_TIMEOUT
}

$application = Get-AutoCadApplication
$document = Get-AutoCadDocument -Application $application

$variables = [ordered]@{
    FILEDIA = 1
    CMDDIA = 1
    CMDECHO = 1
    EXPERT = 0
    PROXYNOTICE = 1
    PROXYSHOW = 1
    PROXYWEBSEARCH = 0
    XREFNOTIFY = 2
    XLOADCTL = 2
    XREFCTL = 0
    ISAVEBAK = 1
    FONTMAP = 'acad.fmp'
    FONTALT = 'simplex.shx'
}

foreach ($entry in $variables.GetEnumerator()) {
    Set-AutoCadVariable -Document $document -Name $entry.Key -Value $entry.Value
}

try {
    $document.SendCommand("_.COMMANDLINE`r")
    Write-Host 'COMMANDLINE=sent'
} catch {
    Write-Warning "failed_commandline=$($_.Exception.Message)"
}

Write-Host 'restore_autocad_settings=done'
