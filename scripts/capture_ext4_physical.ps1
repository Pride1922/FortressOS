param([int]$DiskNumber = 1)
$ErrorActionPreference = 'Stop'
$disk = Get-Disk -Number $DiskNumber
$part = Get-Partition -DiskNumber $DiskNumber -PartitionNumber 2
$expectedGuid = 'e68c8e17-8fcd-47fe-b7dd-8f070b5a81d5'
if ($disk.BusType -ne 'USB' -or $disk.Size -ne 4026531840 -or
    $disk.IsBoot -or $disk.IsSystem -or $disk.LogicalSectorSize -ne 512 -or
    ([string]$part.Guid).Trim('{}') -ne $expectedGuid -or
    $part.Offset -ne 68157440 -or $part.Size -ne 33554432) {
    throw 'USB identity or partition geometry does not match the approved fixture.'
}
$repo = Split-Path -Parent $PSScriptRoot
$folder = Join-Path $repo ('.codex-remote-attachments\ext4-phase9\physical-start-capture-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $folder | Out-Null
$path = Join-Path $folder 'after-start.ext4'
$source = $null
$output = $null
try {
    # Physical device handle is strictly read-only; output is a new regular file.
    $source = [System.IO.FileStream]::new("\\.\PhysicalDrive$DiskNumber", [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $output = [System.IO.FileStream]::new($path, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    $source.Seek([long]$part.Offset, [System.IO.SeekOrigin]::Begin) | Out-Null
    $buffer = New-Object byte[] 1048576
    $remaining = [long]$part.Size
    while ($remaining -gt 0) {
        $wanted = [int][Math]::Min($buffer.Length, $remaining)
        $got = $source.Read($buffer, 0, $wanted)
        if ($got -ne $wanted) { throw 'Incomplete device read; capture is not valid.' }
        $output.Write($buffer, 0, $got)
        $remaining -= $got
    }
    $output.Flush($true)
} finally {
    if ($output) { $output.Dispose() }
    if ($source) { $source.Dispose() }
}
$hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
@{ status='captured; integrity audit pending'; captured_utc=[DateTime]::UtcNow.ToString('o');
    disk_number=$DiskNumber; friendly_name=$disk.FriendlyName; serial=$disk.SerialNumber;
    capacity_bytes=$disk.Size; data_partuuid=$expectedGuid; offset_bytes=$part.Offset;
    bytes=$part.Size; sha256=$hash; image=$path; physical_writes=$false
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $folder 'manifest.json') -Encoding UTF8
Write-Output "Captured: $path"
Write-Output "SHA256: $hash"
