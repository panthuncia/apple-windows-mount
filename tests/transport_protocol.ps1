param(
    [Parameter(Mandatory)][string]$Transport,
    [Parameter(Mandatory)][string]$Client
)
$ErrorActionPreference = 'Stop'
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('awm-protocol-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
$eventName = 'Local\awm-test-' + [Guid]::NewGuid().ToString('N')
$stopEvent = New-Object Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $eventName)
$process = $null
$listener = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$listener.Stop()

function Read-Exact([IO.Stream]$Stream, [int]$Count) {
    $buffer = New-Object byte[] $Count
    $offset = 0
    while ($offset -lt $Count) {
        $received = $Stream.Read($buffer, $offset, $Count - $offset)
        if ($received -eq 0) { throw 'Unexpected EOF in mux response' }
        $offset += $received
    }
    return ,$buffer
}

function Invoke-Mux([string]$Message, [int]$Tag) {
    $socket = New-Object Net.Sockets.TcpClient
    try {
        $socket.Connect('127.0.0.1', $port)
        $stream = $socket.GetStream()
        $stream.ReadTimeout = 5000
        $xml = [Text.Encoding]::UTF8.GetBytes('<?xml version="1.0"?><plist version="1.0"><dict><key>MessageType</key><string>' + $Message + '</string></dict></plist>')
        $header = [BitConverter]::GetBytes([uint32]($xml.Length + 16)) + [BitConverter]::GetBytes([uint32]1) + [BitConverter]::GetBytes([uint32]8) + [BitConverter]::GetBytes([uint32]$Tag)
        # Exercise partial protocol headers and payloads across separate sends.
        foreach ($value in $header) { $stream.WriteByte($value) }
        $middle = [int]($xml.Length / 2)
        $stream.Write($xml, 0, $middle)
        $stream.Write($xml, $middle, $xml.Length - $middle)
        $response = Read-Exact $stream 16
        $length = [BitConverter]::ToUInt32($response, 0)
        if ($length -lt 16 -or $length -gt 65536) { throw 'Invalid response size' }
        if ([BitConverter]::ToUInt32($response, 12) -ne $Tag) { throw 'Response tag mismatch' }
        $body = Read-Exact $stream ($length - 16)
        return [xml][Text.Encoding]::UTF8.GetString($body)
    } finally { $socket.Dispose() }
}

try {
    $arguments = @('--no-usb', '--port', $port, '--state-dir', ('"' + $temporary + '"'), '--stop-event', $eventName)
    $process = Start-Process -FilePath $Transport -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $temporary 'stderr.log')
    $null = $process.Handle
    $ready = $false
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($process.HasExited) { throw ('Transport exited early. ' + (Get-Content (Join-Path $temporary 'stderr.log') -Raw)) }
        try { $null = Invoke-Mux 'ListDevices' 11; $ready = $true; break } catch { Start-Sleep -Milliseconds 100 }
    }
    if (-not $ready) { throw 'Transport endpoint did not become ready' }
    $first = Invoke-Mux 'ReadBUID' 12
    $second = Invoke-Mux 'ReadBUID' 13
    if ($first.plist.dict.string -ne $second.plist.dict.string -or $first.plist.dict.string.Length -ne 36) { throw 'BUID did not persist' }
    $output = & $Client devices --port $port --json
    if ($LASTEXITCODE -ne 0 -or $output.Trim() -ne '[]') { throw 'CLI discovery failed against an empty broker' }
    $socket = New-Object Net.Sockets.TcpClient('127.0.0.1', $port)
    try {
        $stream = $socket.GetStream()
        $stream.ReadTimeout = 3000
        $invalid = [BitConverter]::GetBytes([uint32]65537) + (New-Object byte[] 12)
        $stream.Write($invalid, 0, $invalid.Length)
        try { $received = $stream.ReadByte() } catch [IO.IOException] { $received = -1 }
        if ($received -ne -1) { throw 'Oversize packet was accepted' }
    } finally { $socket.Dispose() }
    $null = Invoke-Mux 'ListDevices' 14
    $stopEvent.Set() | Out-Null
    if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) {
        throw ('Transport did not stop cleanly. Exit ' + $process.ExitCode + '. ' + (Get-Content (Join-Path $temporary 'stderr.log') -Raw))
    }
} finally {
    $stopEvent.Set() | Out-Null
    if ($process -and -not $process.HasExited) {
        if (-not $process.WaitForExit(5000)) { $process.Kill() }
    }
    $stopEvent.Dispose()
    # Only remove the exact unique test directory created above.
    $resolved = [IO.Path]::GetFullPath($temporary)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and (Split-Path $resolved -Leaf) -like 'awm-protocol-*') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
