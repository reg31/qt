$blacklist = @{}
$pipeName = "lld-wrapper-pipe"

while ($true) {
    $pipe = New-Object System.IO.Pipes.NamedPipeServerStream($pipeName, 
        [System.IO.Pipes.PipeDirection]::InOut, 
        [System.IO.Pipes.NamedPipeServerStream+MaxAllowedServerInstances]::MaxAllowed)
    
    $pipe.WaitForConnection()
    
    $reader = New-Object System.IO.StreamReader($pipe)
    $writer = New-Object System.IO.StreamWriter($pipe)
    $writer.AutoFlush = $true
    
    $args = $reader.ReadLine() | ConvertFrom-Json
    
    do {
        $filtered = $args | Where-Object { -not $blacklist.ContainsKey($_) }
        $result = & "$env:LLVM_PATH\bin\ld.lld" @filtered 2>&1
        $exitCode = $LASTEXITCODE
        
        if ($exitCode -ne 0) {
            $newFlag = ($result | Select-String "unknown argument: (\S+)").Matches.Groups[1].Value
            if ($newFlag -and -not $blacklist.ContainsKey($newFlag)) {
                $blacklist[$newFlag] = $true
                continue
            }
        }
        break
    } while ($true)
    
    $writer.WriteLine($exitCode)
    $pipe.Disconnect()
    $pipe.Dispose()
}
