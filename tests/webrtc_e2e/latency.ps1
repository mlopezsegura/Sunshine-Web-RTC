<#
.SYNOPSIS
    Measures the glass-to-glass latency of one or more Sunshine builds with driver.mjs.

.DESCRIPTION
    Starts each Sunshine with the given configuration file in this Windows session, waits for its
    Web UI, runs driver.mjs --latency for every stream mode, stops it, and appends one JSON line per
    run to -Results. clock.html is shown full screen on the captured display while a run lasts, so
    nothing else should be on screen. The configuration must enable WebRTC, and its Web UI
    credentials must match -Credentials.

.EXAMPLE
    ./latency.ps1 -Sunshine @{ before = 'C:\old\sunshine.exe'; after = 'C:\new\sunshine.exe' } `
        -Config C:\e2e\sunshine.conf -Results latency.jsonl
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][hashtable]$Sunshine,
    [Parameter(Mandatory = $true)][string]$Config,
    [string]$Results = (Join-Path $PSScriptRoot 'latency.jsonl'),
    [string]$Credentials = 'e2e:e2e-password',
    [int]$WebUiPort = 50001,
    [int]$WebRtcPort = 8010,
    [int]$Seconds = 20,
    [int]$Repeat = 1,
    # codec:width:height:fps:bitrateKbps
    [string[]]$Modes = @('h264:1920:1080:60:20000', 'h264:3840:2160:60:80000'),
    [switch]$WithoutTvTweaks
)

$ErrorActionPreference = 'Stop'
$driver = Join-Path $PSScriptRoot 'driver.mjs'

function Wait-Port([int]$Port) {
    for ($attempt = 0; $attempt -lt 120; $attempt += 1) {
        $client = New-Object System.Net.Sockets.TcpClient
        try {
            $client.Connect('127.0.0.1', $Port)
            return
        } catch {
            Start-Sleep -Milliseconds 500
        } finally {
            $client.Dispose()
        }
    }
    throw "Nothing listens on port $Port"
}

foreach ($label in $Sunshine.Keys) {
    $exe = $Sunshine[$label]
    $process = Start-Process -FilePath $exe -ArgumentList "`"$Config`"" -WorkingDirectory (Split-Path $exe) -PassThru -WindowStyle Hidden
    try {
        Wait-Port $WebUiPort
        Wait-Port $WebRtcPort
        foreach ($mode in $Modes) {
            $codec, $width, $height, $fps, $bitrate = $mode.Split(':')
            foreach ($tweaks in @($true) + @(if ($WithoutTvTweaks) { $false })) {
                for ($run = 1; $run -le $Repeat; $run += 1) {
                    Write-Host "== $label $mode tvTweaks=$tweaks run $run"
                    # Windows PowerShell turns a native command's stderr into terminating errors.
                    $ErrorActionPreference = 'Continue'
                    & node $driver "--creds=$Credentials" "--webui=https://127.0.0.1:$WebUiPort" "--ws=ws://127.0.0.1:$WebRtcPort" `
                        "--codec=$codec" "--width=$width" "--height=$height" "--fps=$fps" "--bitrate=$bitrate" `
                        "--latency=$Seconds" "--tvTweaks=$(if ($tweaks) { 1 } else { 0 })" "--label=$label" "--results=$Results" |
                        Select-String '^LATENCY|timeout|failed'
                    $ErrorActionPreference = 'Stop'
                    Start-Sleep -Seconds 2
                }
            }
        }
    } finally {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    }
}
