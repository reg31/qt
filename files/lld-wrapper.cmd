@echo off
powershell -Command "
    $args = '%*' -split ' '
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'lld-wrapper-pipe', [System.IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(5000)
    $writer = New-Object System.IO.StreamWriter($pipe)
    $writer.AutoFlush = $true
    $reader = New-Object System.IO.StreamReader($pipe)
    $writer.WriteLine(($args | ConvertTo-Json -Compress))
    $exit = $reader.ReadLine()
    $pipe.Dispose()
    exit [int]$exit
"
