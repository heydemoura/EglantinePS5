# SPDX-License-Identifier: GPL-3.0-or-later
# Writes .eglantine-index in the folder of a game: the name, size and kind of every file and folder in it, one per line,
# so that EglantinePS5 reads the game's folders from this one file instead of listing them over NFS (which for a game
# with hundreds of thousands of files takes minutes, the first time).
#
#   .\make-nfs-index.ps1 E:\Roms\917504
#
# Run it again whenever the game's files change: the index is trusted as it is.

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Folder
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $Folder).Path.TrimEnd('\')
if (-not (Test-Path -LiteralPath (Join-Path $root 'exefs\main'))) {
    Write-Warning "There is no exefs\main in ${root}: is this the folder of a game?"
}
$final = Join-Path $root '.eglantine-index'
$staged = "$final.tmp"
$utf8 = New-Object System.Text.UTF8Encoding($false)
$writer = New-Object System.IO.StreamWriter($staged, $false, $utf8)
$writer.NewLine = "`n"
$writer.WriteLine('EGLANTINE-INDEX 1')
$count = 0
$start = Get-Date
try {
    $top = New-Object System.IO.DirectoryInfo($root)
    foreach ($entry in $top.EnumerateFileSystemInfos('*', [System.IO.SearchOption]::AllDirectories)) {
        $relative = $entry.FullName.Substring($root.Length + 1).Replace('\', '/')
        if ($relative -eq '.eglantine-index' -or $relative -eq '.eglantine-index.tmp') { continue }
        $isFolder = ($entry.Attributes -band [System.IO.FileAttributes]::Directory) -ne 0
        $size = if ($isFolder) { 0 } else { $entry.Length }
        $modified = [DateTimeOffset]::new($entry.LastWriteTimeUtc).ToUnixTimeSeconds()
        $kind = if ($isFolder) { 'D' } else { 'F' }
        $writer.WriteLine("$kind`t$size`t$modified`t$relative")
        $count++
        if ($count % 20000 -eq 0) { Write-Host "$count entries..." }
    }
}
finally {
    $writer.Dispose()
}
Move-Item -LiteralPath $staged -Destination $final -Force
$seconds = [int]((Get-Date) - $start).TotalSeconds
Write-Host "Done: $count entries in $seconds s -> $final"
