# Run the MediaStream UDP integration test (as documented)
# Server (background) and Client (foreground) with log capture
# Usage: Open PowerShell in repository root and run: .\run_media_stream_test.ps1

$ErrorActionPreference = 'Stop'

# Ensure working directory is the script location
Push-Location -Path (Split-Path -Parent $MyInvocation.MyCommand.Path)

# Remove old logs if present
Remove-Item .\server.log -ErrorAction SilentlyContinue
Remove-Item .\server.err -ErrorAction SilentlyContinue
Remove-Item .\client.log -ErrorAction SilentlyContinue
Remove-Item .\client.err -ErrorAction SilentlyContinue

# Server command (background)
$serverExe = Join-Path -Path '.' -ChildPath 'x64\Debug\ctsTraffic.exe'
$serverArgs = "-listen:* -protocol:udp -bitspersecond:1000000 -FrameRate:1000 -StreamLength:5 -ConsoleVerbosity:1 -verify:connection -serverexitlimit:50"

Write-Output "Starting server: $serverExe $serverArgs"
$serverProc = Start-Process -FilePath $serverExe -ArgumentList $serverArgs -RedirectStandardOutput .\server.log -RedirectStandardError .\server.err -NoNewWindow -PassThru

# Give the server a moment to initialize
Start-Sleep -Seconds 2

# Client command (foreground)
$clientExe = Join-Path -Path '.' -ChildPath 'x64\Debug\ctsTraffic.exe'
$clientArgs = "-target:localhost -protocol:udp -bitspersecond:1000000 -FrameRate:1000 -StreamLength:5 -ConsoleVerbosity:1 -verify:connection -connections:50 -iterations:1"

Write-Output "Running client: $clientExe $clientArgs"
# Run client and wait for completion; capture stdout/stderr
$clientProc = Start-Process -FilePath $clientExe -ArgumentList $clientArgs -RedirectStandardOutput .\client.log -RedirectStandardError .\client.err -NoNewWindow -Wait -PassThru

Write-Output "Client finished. Exit code: $($clientProc.ExitCode)"

# Stop the server process if still running
if ($null -ne $serverProc -and !$serverProc.HasExited) {
    Write-Output "Stopping server (Id=$($serverProc.Id))"
    Stop-Process -Id $serverProc.Id -Force -ErrorAction SilentlyContinue
}

Write-Output "Logs written to: server.log, server.err, client.log, client.err"

Pop-Location
