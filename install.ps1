<#
Installs or updates RockWheel on an iPod Video (5th or 5.5th generation) or an
iPod photo from Windows.

    irm https://raw.githubusercontent.com/ssarkisy/RockWheel/main/install.ps1 | iex

Options, after a download of this script, in PowerShell:

    powershell -ExecutionPolicy Bypass -File .\install.ps1 [options]

    -DryRun      look at the iPod and say what would be done, changing nothing
    -From DIR    take the files from DIR instead of downloading them
    -Uninstall   remove the bootloader, so the iPod starts its stock firmware
    -Folders     put your playlist folders from iTunes on the iPod, for them to
                 show there after the next sync
    -Games       add Rockbox's games to Extras > Games without asking
    -NoGames     take those games off the iPod, and don't ask about them again
    -Drive E:    the iPod to use, when more than one is connected

It changes nothing without asking, unless all it has to do is update the firmware
files, and the games if you added them. Windows asks for an administrator's
permission only to reach the iPod's disk directly, for the bootloader. A
Mac-formatted iPod has to be restored in iTunes on this PC first.

Written for Windows PowerShell 5.1, which every Windows 10 and 11 has.
#>
param(
    [switch]$DryRun,
    [string]$From = '',
    [switch]$Uninstall,
    [switch]$Folders,
    [switch]$Games,
    [switch]$NoGames,
    [string]$Drive = ''
)

$Repo = 'ssarkisy/RockWheel'
$ReleaseUrl = "https://github.com/$Repo/releases/latest/download"
# Each model's files in a release, and the bootloader this installer writes on
# it, by its SHA-256 (the app's table, core/src/bootloaders.rs, has the same).
# Apple's last firmware for it is the low byte of the main firmware's version:
# RockWheel's bootloader needs it (on the photo's 1.0 it hung on Apple's logo).
$Models = @{
    # Rockbox's own bootloader, the one Rockbox Utility installs
    Video = [pscustomobject]@{
        Name = 'iPod Video'; Target = 'ipodvideo'; BootName = 'the Rockbox bootloader'
        Firmware = 'rockbox.zip'; Bootloader = 'bootloader-ipodvideo.ipod'; Games = 'rockbox-games.zip'
        BootloaderSha256 = '19dfa0e930689f5afdeaae18f4c56b472cbed9c6c3a7039bb32b646d8040298f'
        Apple = 0x12; AppleName = '1.3'
    }
    # RockWheel's rw-bl2. Rockbox's v4.0 for it can't read a fragmented
    # rockbox.ipod, so an iPod photo with v4.0 gets rw-bl2 in its place.
    Photo = [pscustomobject]@{
        Name = 'iPod photo'; Target = 'ipodcolor'; BootName = "RockWheel's bootloader"
        Firmware = 'rockbox-ipodcolor.zip'; Bootloader = 'bootloader-ipodcolor.ipod'; Games = 'rockbox-games-ipodcolor.zip'
        BootloaderSha256 = 'ff9f7be5a7c85832443ef188e8655de82249c84bf9c1f1202f9bdc8b01f4e1cc'
        Apple = 0x13; AppleName = '1.2.1'
    }
}
# What a bootloader on the photo is, by the SHA-256 of its bytes on the iPod:
# v4.0 is replaced; RockWheel's own and any other are kept. Only one of
# v4.0's length can be v4.0, so only such a one is read.
$PhotoReplaced = '5c47d85d9d0346fcbb17ac2847ca5d97c1ef0cea055b6d1fd9778135be8aba6f'
$PhotoReplacedLength = 49988
# what RockWheel has and plain Rockbox hasn't
$Marker = '.rockbox/wps/iClassic-Square/About Backdrop.bmp'

$script:Work = ''
$script:Files = ''
$script:Patcher = ''
$script:Model = $null

function Say([string]$Text) { Write-Host $Text }
function Step([string]$Text) { Write-Host ''; Write-Host "==> $Text" }
# errors end the run without closing the window, as "irm | iex" runs in the
# user's own PowerShell
function Die([string]$Text) { throw [System.Exception]::new($Text) }

# Whatever writes is skipped in a dry run, and says what it would have done
# a path inside the iPod, from the / of a zip's names
function Get-IPodPath([string]$Root, [string]$Name) {
    $sep = [IO.Path]::DirectorySeparatorChar
    return [IO.Path]::Combine($Root, $Name.Replace('/', $sep).Replace('\', $sep))
}

function Would([string]$Text) {
    if ($DryRun) {
        Say "    Would $Text"
        return $true
    }
    return $false
}

# --- the files ---------------------------------------------------------------

# a file of the release; -Missing is added to the error when the release
# hasn't it (404), not when the download itself failed
function Get-Release([string]$Name, [switch]$Optional, [string]$Missing = '') {
    $out = Join-Path $script:Files $Name
    # Windows PowerShell 5.1 offers older TLS first, and its progress bar
    # makes a download many times slower
    [Net.ServicePointManager]::SecurityProtocol = `
        [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -UseBasicParsing -Uri "$ReleaseUrl/$Name" -OutFile $out
        return $true
    } catch {
        $code = 0
        try { $code = [int]$_.Exception.Response.StatusCode } catch { $code = 0 }
        Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue
        if ($Optional) { return $false }
        if ($code -eq 404) { Die "Couldn't download $Name from $ReleaseUrl$Missing" }
        Die "Couldn't download $Name from $ReleaseUrl"
    }
}

# a list of checksums, as shasum -c reads it: the checksum by the name
function Get-Sums([string]$SumsName) {
    $sums = @{}
    foreach ($line in Get-Content -LiteralPath (Join-Path $script:Files $SumsName)) {
        if ($line -match '^([0-9a-fA-F]{64})\s+\*?(.+)$') {
            $sums[$Matches[2].Trim()] = $Matches[1].ToLowerInvariant()
        }
    }
    return $sums
}

# each of the files against its line in a list of checksums
function Test-Sums([string]$SumsName, [string[]]$Names) {
    $sums = Get-Sums $SumsName
    foreach ($n in $Names) {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $script:Files $n)).Hash.ToLowerInvariant()
        if (-not $sums.ContainsKey($n) -or $sums[$n] -ne $hash) {
            Die "The download is damaged: its checksums don't match."
        }
    }
}

# ipodpatcher.exe first, which reads the iPod's model: the rest of the files
# are that model's (Get-ModelFiles)
function Get-Patcher {
    Step 'Getting the files'
    if ($From) {
        $script:Files = (Resolve-Path -LiteralPath $From).Path
    } else {
        $script:Files = $script:Work
        foreach ($f in @('ipodpatcher.exe', 'SHA256SUMS-windows', 'SHA256SUMS')) {
            Say "    $f"
            Get-Release $f | Out-Null
        }
        Test-Sums 'SHA256SUMS-windows' @('ipodpatcher.exe')
    }
    if (-not (Test-Path -LiteralPath (Join-Path $script:Files 'ipodpatcher.exe'))) { Die "ipodpatcher.exe is missing from $($script:Files)" }
    $script:Patcher = Join-Path $script:Files 'ipodpatcher.exe'
}

# The text of a file in a zip; '' when it hasn't that file or isn't a zip
function Get-ZipText([string]$Zip, [string]$Name) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    try { $z = [IO.Compression.ZipFile]::OpenRead($Zip) } catch { return '' }
    try {
        $e = $z.GetEntry($Name)
        if (-not $e) { return '' }
        $r = New-Object IO.StreamReader ($e.Open())
        try { return $r.ReadToEnd() } finally { $r.Dispose() }
    } catch {
        return ''
    } finally {
        $z.Dispose()
    }
}

# The model's firmware, bootloader and games, each by its line in the
# checksum files: a release lists both models' files, and only this model's
# are downloaded. The games have a checksum file of their own; a release
# without them is only asked nothing about them.
function Get-ModelFiles {
    $m = $script:Model
    if (-not $From) {
        Step "Getting RockWheel for the $($m.Name)"
        $missing = ": this release may have no RockWheel for the $($m.Name)."
        $listed = Get-Sums 'SHA256SUMS'
        foreach ($f in @($m.Firmware, $m.Bootloader)) {
            if (-not $listed.ContainsKey($f)) { Die "$f isn't in this release's SHA256SUMS$missing" }
            Say "    $f"
            Get-Release $f -Missing $missing | Out-Null
            Test-Sums 'SHA256SUMS' @($f)
        }
        if ((Get-Release $m.Games -Optional) -and (Get-Release 'SHA256SUMS-games' -Optional)) {
            Say "    $($m.Games)"
            Test-Sums 'SHA256SUMS-games' @($m.Games)
        } else {
            Remove-Item -LiteralPath (Join-Path $script:Files $m.Games) -ErrorAction SilentlyContinue
        }
    }
    foreach ($f in @($m.Firmware, $m.Bootloader)) {
        if (-not (Test-Path -LiteralPath (Join-Path $script:Files $f))) { Die "$f is missing from $($script:Files)" }
    }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $script:Files $m.Bootloader)).Hash.ToLowerInvariant()
    if ($hash -ne $m.BootloaderSha256) { Die "$($m.Bootloader) isn't the bootloader this installer expects." }
    # the firmware's own word on the player it is for
    $info = Get-ZipText (Join-Path $script:Files $m.Firmware) '.rockbox/rockbox-info.txt'
    if ($info -notmatch "(?m)^Target: $($m.Target)\r?$") { Die "$($m.Firmware) isn't RockWheel for the $($m.Name)." }
}

# --- the iPod ----------------------------------------------------------------

# the iPod's FAT32 partition, as a drive letter, or $null
function Get-IPodVolume($Disk) {
    foreach ($p in Get-CimAssociatedInstance -InputObject $Disk -ResultClassName Win32_DiskPartition) {
        foreach ($v in Get-CimAssociatedInstance -InputObject $p -ResultClassName Win32_LogicalDisk) {
            if ($v.FileSystem -eq 'FAT32') { return $v.DeviceID }
        }
    }
    return $null
}

function Find-IPod {
    while ($true) {
        $disks = @(Get-CimInstance Win32_DiskDrive | Where-Object {
            $_.Model -match 'iPod' -or $_.PNPDeviceID -match 'PROD_IPOD'
        })
        if ($Drive) {
            $letter = $Drive.TrimEnd('\').ToUpperInvariant()
            if ($letter.Length -eq 1) { $letter += ':' }
            $disks = @($disks | Where-Object { (Get-IPodVolume $_) -eq $letter })
            if ($disks.Count -eq 0) { Die "$Drive isn't an iPod." }
        }
        if ($disks.Count -eq 1) { return $disks[0] }
        if ($disks.Count -gt 1) {
            Die 'More than one iPod is connected. Leave only the one to install on, or give it with -Drive E:.'
        }
        Say 'No iPod found. Connect it with its cable. On the stock firmware it should show up'
        Say 'in File Explorer as a drive; if it does not, turn on "Enable disk use" for it in iTunes first.'
        $r = Read-Host 'Press Enter to look again, or type q to stop'
        if ($r -eq 'q') { Die 'Stopped. Nothing was changed.' }
    }
}

# ipodpatcher.exe reaches the disk directly, which takes an administrator: its
# runs go in one command line that Windows asks permission for once, one after
# the other while each succeeds. cmd.exe writes what they print to a file as it
# is: ipodpatcher prints to stderr, which PowerShell 5.1 would wrap as errors,
# and break its lines.
function Invoke-Patcher([int]$DiskNumber, [string[][]]$Runs) {
    $out = Join-Path $script:Work 'ipodpatcher.txt'
    Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue
    $cmds = foreach ($run in $Runs) {
        $a = ($run | ForEach-Object { '"' + $_ + '"' }) -join ' '
        '"' + $script:Patcher + '" ' + $DiskNumber + ' ' + $a + ' >> "' + $out + '" 2>&1'
    }
    # cmd /c takes away the first and the last quote of what follows it
    $line = '/d /c "' + ($cmds -join ' && ') + '"'
    try {
        $p = Start-Process -FilePath 'cmd.exe' -ArgumentList $line -Verb RunAs -Wait -PassThru -WindowStyle Hidden
    } catch {
        Die "Windows didn't give permission to reach the iPod's disk. Nothing was changed."
    }
    $text = ''
    if (Test-Path -LiteralPath $out) { $text = Get-Content -LiteralPath $out -Raw }
    return [pscustomobject]@{ ExitCode = $p.ExitCode; Output = $text }
}

# What ipodpatcher's --list says, -v adding the firmware partition's directory
# as it read it: the main firmware's version (Apple's, in its low byte)
function Read-IPod([int]$DiskNumber) {
    $r = Invoke-Patcher $DiskNumber @(, @('-v', '--list'))
    $model = ''
    $kind = ''
    if ($r.Output -match 'iPod model: (.*) \("([a-z]*pod)"\)') {
        $model = $Matches[1]
        $kind = $Matches[2]
    }
    if (-not $kind) {
        Say ($r.Output -split "`n" | Select-Object -Last 5 | Out-String)
        Die 'ipodpatcher.exe does not recognise this disk as an iPod. Close any File Explorer window on the iPod and run this again.'
    }
    $sector = 512
    if ($r.Output -match 'Sector size is (\d+) bytes') { $sector = [int]$Matches[1] }
    # its fields: id, devOffset, len, addr, entryOffset, chksum, vers, ...
    $version = $null
    if ($r.Output -match '(?m)^\d+ - OSOS((?:\s+0x[0-9a-fA-F]+){7})') {
        $version = [Convert]::ToUInt32(($Matches[1].Trim() -split '\s+')[6], 16)
    }
    $bootBytes = 0
    if ($r.Output -match 'Third-party bootloader - (\d+) bytes') { $bootBytes = [long]$Matches[1] }
    return [pscustomobject]@{
        Model = $model
        Kind = $kind
        HasBootloader = ($r.Output -match 'Third-party bootloader')
        BootloaderBytes = $bootBytes
        Version = $version
        SectorSize = $sector
    }
}

# What went wrong, as ipodpatcher said it: its [ERR] lines, or else its last ones
function Show-PatcherError([string]$Output) {
    $lines = @($Output -split '\r?\n' | Where-Object { $_ -match '^\[ERR\]' })
    if ($lines.Count -eq 0) { $lines = @($Output -split '\r?\n' | Where-Object { $_.Trim() } | Select-Object -Last 5) }
    Say ''
    foreach ($l in $lines) { Say "    $l" }
}

# What ipodpatcher's model is to this installer: its files, its target, its name
function Select-Model([string]$Name) {
    if ($Name -match 'Video') { return $Models.Video }
    if ($Name -match 'Photo/Color') { return $Models.Photo }
    Die "This isn't an iPod Video or an iPod photo, the models RockWheel supports."
}

function Get-Sha256([byte[]]$Bytes, [int]$Offset = 0) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($Bytes, $Offset, $Bytes.Length - $Offset)) -replace '-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

# <count> bytes of a file from <at>, or a stop with <why>
function Read-Bytes($Stream, [long]$At, [long]$Count, [string]$Why) {
    $b = New-Object byte[] $Count
    $Stream.Position = $At
    $n = 0
    while ($n -lt $Count) {
        $k = $Stream.Read($b, $n, $Count - $n)
        if ($k -le 0) { Die $Why }
        $n += $k
    }
    return , $b
}

# The firmware partition's directory as ipodpatcher reads it, in the copy of
# the partition its --read-partition makes (deleted here): the bootloader in
# the main firmware, by the SHA-256 of its bytes ('' for none)
function Get-BootloaderHash([string]$Copy, [int]$Sector, [string]$Why) {
    if (-not (Test-Path -LiteralPath $Copy -PathType Leaf)) { Die $Why }
    $s = [IO.File]::OpenRead($Copy)
    try {
        $head = Read-Bytes $s 0 $Sector $Why
        if ([Text.Encoding]::ASCII.GetString($head, 0x100, 4) -ne ']ih[') { Die $Why }
        $dir = Read-Bytes $s ([long][BitConverter]::ToUInt32($head, 0x104) + 0x200) 400 $Why
        for ($i = 0; $i -lt 10; $i++) {
            $e = 40 * $i
            if ([Text.Encoding]::ASCII.GetString($dir, $e, 4) -ne '!ATA') { break }
            if ([Text.Encoding]::ASCII.GetString($dir, $e + 4, 4) -ne 'soso') { continue }
            $dev = [long][BitConverter]::ToUInt32($dir, $e + 12)
            $len = [long][BitConverter]::ToUInt32($dir, $e + 16)
            $entry = [long][BitConverter]::ToUInt32($dir, $e + 24)
            if ($entry -le 0 -or $len -le $entry) { return '' }
            # an image's data is a sector past the partition's start
            if ($Sector + $dev + $len -gt $s.Length) { Die $Why }
            return Get-Sha256 (Read-Bytes $s ($Sector + $dev + $entry) ($len - $entry) $Why)
        }
    } finally {
        $s.Dispose()
        Remove-Item -LiteralPath $Copy -ErrorAction SilentlyContinue
    }
    Die $Why
}

# The bootloader on the iPod, by the SHA-256 of its bytes
function Read-Bootloader([int]$DiskNumber, [int]$Sector) {
    $copy = Join-Path $script:Work 'firmware-partition.bin'
    Invoke-Patcher $DiskNumber @(, @('--read-partition', $copy)) | Out-Null
    return Get-BootloaderHash $copy $Sector "Couldn't read the iPod's firmware directory."
}

# RockWheel's bootloader needs Apple's last firmware for the iPod. Checked
# before a bootloader is written, not before an update of the files.
function Test-AppleFirmware($Version) {
    $m = $script:Model
    if ($null -eq $Version) { Die "Couldn't read the iPod's firmware directory." }
    $low = $Version -band 0xff
    if ($low -ge $m.Apple) { return }
    $has = '0x{0:x4}' -f $Version
    if ($m.Target -eq 'ipodcolor' -and $low -eq 0) { $has = '1.0' }
    Die "This iPod has Apple's software $has, and RockWheel needs $($m.AppleName). Select the iPod in iTunes and click Update on its Summary page, not Restore: Update keeps your music. Then run this again. Nothing was changed."
}

function Add-Bootloader([int]$DiskNumber, [int]$Sector) {
    $m = $script:Model
    Step 'Installing the bootloader'
    if (Would "add $($m.BootName) to the firmware partition, keeping the stock firmware") { return }
    $file = Join-Path $script:Files $m.Bootloader
    $copy = Join-Path $script:Work 'firmware-partition.bin'
    $r = Invoke-Patcher $DiskNumber @(
        @('--add-bootloader', $file),
        @('--list'),
        @('--read-partition', $copy))
    # ipodpatcher answers 0 also when it couldn't write it; when it couldn't
    # reach the disk, it answers otherwise and goes no further
    if ($r.Output -match '--add-bootloader failed') {
        Show-PatcherError $r.Output
        Die 'Installing the bootloader failed.'
    }
    if ($r.ExitCode -ne 0 -and $r.Output -notmatch 'Third-party bootloader') {
        Show-PatcherError $r.Output
        Die 'Installing the bootloader failed. Close any File Explorer window on the iPod and run this again.'
    }
    if ($r.Output -notmatch 'Third-party bootloader') {
        Show-PatcherError $r.Output
        Die "The bootloader didn't end up on the iPod."
    }
    # the one written, by its bytes on the iPod (the file less its 8-byte header)
    $on = Get-BootloaderHash $copy $Sector ("The bootloader is on the iPod, but it couldn't be read back to check it. " +
        "RockWheel's files aren't on the iPod yet: run the installer again to finish.")
    if ($on -ne (Get-Sha256 ([IO.File]::ReadAllBytes($file)) 8)) { Die "The bootloader on the iPod isn't the one written." }
}

# a zip into the iPod: each file put in place, replacing the one there
function Expand-Into([string]$Zip, [string]$Root) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        foreach ($e in $z.Entries) {
            if ($e.FullName -match '(^|[\\/])\.\.([\\/]|$)') { continue }
            $target = Get-IPodPath $Root $e.FullName
            if ($e.FullName.EndsWith('/')) {
                New-Item -ItemType Directory -Force -Path $target | Out-Null
                continue
            }
            $dir = Split-Path -Parent $target
            if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $target, $true)
        }
    } finally {
        $z.Dispose()
    }
}

# Stops before anything is changed if the archive's files don't fit: cut
# short, a copy leaves the new firmware with some of the old one's fonts and
# languages. What the archive replaces on the iPod gives its room back, so an
# update needs far less than a first install; a plain Rockbox folder that is
# moved aside gives none. 40 MB are kept spare for the file system's clusters.
function Test-Room([string]$Zip, [string]$Root, [bool]$Moving) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $need = [long]0
    $z = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        foreach ($e in $z.Entries) {
            if ($e.FullName.EndsWith('/')) { continue }
            $need += $e.Length
            $target = Get-IPodPath $Root $e.FullName
            if (-not $Moving -and (Test-Path -LiteralPath $target -PathType Leaf)) { $need -= (Get-Item -LiteralPath $target -Force).Length }
        }
    } finally {
        $z.Dispose()
    }
    $need += 40MB
    $free = (New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($Root))).AvailableFreeSpace
    if ($free -lt $need) {
        $more = [long][Math]::Ceiling(($need - $free) / 1MB)
        Die "The iPod needs $more MB more free space for this update. Free some up (remove a few albums in iTunes) and run the installer again. Nothing has been changed on the iPod."
    }
}

# Before the bootloader goes on and before an old folder is moved aside, so
# that a refusal leaves the iPod as it was
function Confirm-Room([string]$Root) {
    if ($DryRun) { return }
    $moving = (Test-Path -LiteralPath (Join-Path $Root '.rockbox')) -and -not (Test-Path -LiteralPath (Get-IPodPath $Root $Marker))
    Test-Room (Join-Path $script:Files $script:Model.Firmware) $Root $moving
}

function Copy-Firmware([string]$Root) {
    Step 'Copying the firmware'
    if (Would "copy the files of $($script:Model.Firmware) to the iPod") { return }
    # Plain Rockbox's settings and database don't carry over
    $rockbox = Join-Path $Root '.rockbox'
    if ((Test-Path -LiteralPath $rockbox) -and -not (Test-Path -LiteralPath (Get-IPodPath $Root $Marker))) {
        $old = '.rockbox-old-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
        try { Rename-Item -LiteralPath $rockbox -NewName $old } catch { Die "Couldn't move the old .rockbox folder aside." }
        Say "    Your previous Rockbox folder is now $old on the iPod."
    }
    try { Expand-Into (Join-Path $script:Files $script:Model.Firmware) $Root } catch { Die 'Copying failed. Is the iPod full?' }
}

# Rockbox's games for Extras > Games, in an archive of their own: asked about
# while they aren't on the iPod, then kept up to date with the firmware. A no is
# kept in a file on the iPod, so that an update doesn't ask again.
function Copy-Games([string]$Root) {
    $zip = Join-Path $script:Files $script:Model.Games
    if (-not (Test-Path -LiteralPath $zip)) {    # a release or -From folder without them
        if ($Games -or $NoGames) { Say ''; Say '    There are no games with this release, so they were left as they are.' }
        return
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $files = @($z.Entries | Where-Object { -not $_.FullName.EndsWith('/') } | ForEach-Object { $_.FullName })
        $size = ($z.Entries | Measure-Object -Property Length -Sum).Sum
    } finally {
        $z.Dispose()
    }
    $count = @($files | Where-Object { $_ -like '*.rock' }).Count
    $mb = '{0:0.0}' -f ($size / 1MB)
    $declined = Get-IPodPath $Root '.rockbox/no-extra-games'
    $have = $false
    foreach ($f in $files) {
        if ($f -like '*.rock' -and (Test-Path -LiteralPath (Get-IPodPath $Root $f))) { $have = $true }
    }

    if ($NoGames) {
        Step 'Taking the extra games off the iPod'
        if (Would "remove the $count games of Extras > Games that came with $($script:Model.Games)") { return }
        foreach ($f in $files) { Remove-Item -LiteralPath (Get-IPodPath $Root $f) -ErrorAction SilentlyContinue }
        New-Item -ItemType File -Force -Path $declined | Out-Null
        return
    }
    if (-not $Games -and -not $have) {
        Say ''
        if (Test-Path -LiteralPath $declined) {
            Say '    The extra games are left out, as you chose before. To add them, run the'
            Say '    installer again with -Games, as Script options in docs/advanced.md shows.'
            return
        }
        if ($DryRun) {
            Say "    Would ask whether to add $count games from Rockbox to Extras > Games."
            return
        }
        $r = Read-Host "Add $count games from Rockbox to Extras > Games? About $mb MB. [y/N]"
        if ($r -notmatch '^(y|yes)$') {
            New-Item -ItemType File -Force -Path $declined | Out-Null
            Say '    Not added, and not asked again. To add them later, run the installer'
            Say '    again with -Games, as Script options in docs/advanced.md shows.'
            return
        }
    }
    Step 'Copying the games'
    if (Would "copy the files of $($script:Model.Games) to the iPod") { return }
    try { Expand-Into $zip $Root } catch { Die 'Copying the games failed. Is the iPod full?' }
    Remove-Item -LiteralPath $declined -ErrorAction SilentlyContinue
}

# --- playlist folders ----------------------------------------------------------

# iTunes leaves playlist folders out of what it syncs to these iPods. It
# knows them, by the same persistent IDs the iPod's database has for the
# playlists: a line for each folder and for each playlist in one, which
# RockWheel reads at the import after a sync.
function Get-Id($ITunes, $Object) {
    try {
        return '{0:X8}{1:X8}' -f $ITunes.ITObjectPersistentIDHigh($Object), $ITunes.ITObjectPersistentIDLow($Object)
    } catch {
        $high = 0
        $low = 0
        $ITunes.GetITObjectPersistentIDs($Object, [ref]$high, [ref]$low)
        return '{0:X8}{1:X8}' -f $high, $low
    }
}

function Read-Folders {
    try {
        $it = New-Object -ComObject iTunes.Application
    } catch {
        Die "Couldn't reach iTunes. Install iTunes from Apple's website (the one from the Microsoft Store can't be reached by scripts), open it once, and run this again."
    }
    $lines = New-Object System.Collections.Generic.List[string]
    $folders = 0
    try {
        foreach ($p in $it.LibrarySource.Playlists) {
            if ($p.Kind -ne 2) { continue }            # a user playlist
            $parent = $null
            try { $parent = $p.Parent } catch { $parent = $null }
            $parentId = '-'
            if ($parent) { $parentId = Get-Id $it $parent }
            if ($p.SpecialKind -eq 4) {                # a folder
                $name = ($p.Name -replace '[\r\n]', ' ')
                $lines.Add("folder $(Get-Id $it $p) $parentId $name")
                $folders++
            } elseif ($parent) {
                $lines.Add("playlist $(Get-Id $it $p) $parentId")
            }
        }
    } finally {
        # let go of iTunes, or it warns at quitting that a script still uses it
        try { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($it) } catch { }
    }
    return [pscustomobject]@{ Lines = $lines; Folders = $folders }
}

function Put-Folders([string]$Root) {
    Step 'Reading your playlist folders from iTunes'
    $r = Read-Folders
    Say "    $($r.Folders) folders."
    if (-not (Test-Path -LiteralPath (Join-Path $Root '.rockbox'))) { Die "RockWheel isn't on this iPod." }
    if (Would 'write them to .rockbox\playlist_folders.txt on the iPod') { return }
    # whole or not at all: written beside it, then put in its place, in UTF-8
    # without a byte order mark and with LF, as the firmware reads it
    $text = "# Playlist folders from iTunes, written by install.ps1 -Folders`n" + (($r.Lines -join "`n") + "`n")
    $tmp = Get-IPodPath $Root '.rockbox/playlist_folders.tmp'
    $dest = Get-IPodPath $Root '.rockbox/playlist_folders.txt'
    try {
        [IO.File]::WriteAllText($tmp, $text, (New-Object System.Text.UTF8Encoding $false))
        Move-Item -LiteralPath $tmp -Destination $dest -Force
    } catch {
        Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue
        Die "Couldn't write to the iPod."
    }
    Say '    They show on the iPod after your next sync.'
}

function Eject-IPod([string]$Letter) {
    Step 'Ejecting'
    if (Would 'eject the iPod') { return }
    try {
        (New-Object -ComObject Shell.Application).Namespace(17).ParseName($Letter).InvokeVerb('Eject')
    } catch {
        Say '    Eject it from File Explorer before you unplug it.'
    }
}

# --- the run -------------------------------------------------------------------

function Invoke-Install {
    $ErrorActionPreference = 'Stop'
    # the names of folders in any alphabet; a console that can't, as the ISE, keeps its own
    try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }
    Step 'RockWheel for iPod Video and iPod photo'
    if ($DryRun) { Say 'This is a dry run: nothing on the iPod is changed.' }
    Say 'People run it on iPods with flash cards, SD card adapters and the original hard'
    Say 'drive. Details are in the advanced guide, docs/advanced.md.'

    $script:Work = Join-Path ([IO.Path]::GetTempPath()) ('rockwheel-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Path $script:Work | Out-Null

    if ($Folders) {
        Step 'Looking for the iPod'
        $letter = Get-IPodVolume (Find-IPod)
        if (-not $letter) { Die "The iPod's music partition isn't in File Explorer, or RockWheel isn't on it." }
        Put-Folders "$letter\"
        return
    }

    Get-Patcher

    Step 'Looking for the iPod'
    $disk = Find-IPod
    $letter = Get-IPodVolume $disk
    $where = 'PhysicalDrive' + $disk.Index
    if ($letter) { $where = "$letter ($where)" }
    Say "Found $where."
    Say 'Close any File Explorer window on the iPod: Windows will ask for permission to'
    Say "reach the iPod's disk directly."
    $ipod = Read-IPod $disk.Index
    Say "Model: $($ipod.Model), $($ipod.Kind)."
    $script:Model = Select-Model $ipod.Model

    if ($Uninstall) {
        if (-not $ipod.HasBootloader) { Die 'No bootloader is installed on this iPod.' }
        Step 'Removing the bootloader'
        $r = Read-Host 'The iPod will start its stock firmware again. Type yes to go on'
        if ($r -ne 'yes') { Die 'Stopped. Nothing was changed.' }
        if (-not (Would 'remove the bootloader')) {
            # ipodpatcher answers 0 also when it couldn't remove it
            $res = Invoke-Patcher $disk.Index @(@('--delete-bootloader'), @('--list'))
            if ($res.ExitCode -ne 0 -or $res.Output -match 'Third-party bootloader') {
                Show-PatcherError $res.Output
                Die 'Removing the bootloader failed.'
            }
        }
        if ($letter) { Eject-IPod $letter }
        Say 'Done. The .rockbox folder is still on the iPod and can be deleted from File Explorer.'
        return
    }

    # Windows can't write the Mac's format, and iTunes on Windows formats an
    # iPod as FAT32, which is what RockWheel needs
    if ($ipod.Kind -eq 'macpod' -or -not $letter) {
        Say ''
        if ($ipod.Kind -eq 'macpod') {
            Say "This iPod is Mac-formatted, and RockWheel needs FAT32, which Windows can't make of it here."
        } else {
            Say "The iPod's music partition has no FAT32 file system."
        }
        Say 'Restore it in iTunes on this PC: that formats it as FAT32. RESTORING ERASES'
        Say 'EVERYTHING ON THE IPOD: music, photos and anything else you keep on it. What is'
        Say 'in your iTunes library comes back with a sync; plays and ratings not yet synced'
        Say 'to iTunes are lost, so sync once first. Then run this again.'
        Die 'Stopped. Nothing was changed.'
    }

    Get-ModelFiles

    # The bootloader to write: none there yet, or (the photo) Rockbox's v4.0 in
    # place of RockWheel's. Apple's last firmware is checked first, before
    # anything changes.
    $write = -not $ipod.HasBootloader
    if (-not $write -and $script:Model.Target -eq 'ipodcolor' -and $ipod.BootloaderBytes -eq $PhotoReplacedLength) {
        Say ''
        Say 'Windows will ask again, to read the bootloader on the iPod.'
        $write = ((Read-Bootloader $disk.Index $ipod.SectorSize) -eq $PhotoReplaced)
    }
    if ($write) { Test-AppleFirmware $ipod.Version }

    $root = "$letter\"
    Confirm-Room $root
    if (-not $write) {
        Say ''
        Say 'The bootloader is already installed, so this only updates the firmware files.'
        Say 'Settings, the library and play counts on the iPod are kept.'
    } else {
        if ($ipod.HasBootloader) {
            Say ''
            Say "Rockbox's bootloader v4.0 is on this iPod photo: RockWheel's takes its place."
        }
        Add-Bootloader $disk.Index $ipod.SectorSize
    }
    Copy-Firmware $root
    Copy-Games $root
    Eject-IPod $letter

    if ($DryRun) {
        Step 'Dry run finished. Nothing was changed.'
        return
    }
    Step 'Done'
    if ($write) {
        Say "1. Unplug the iPod. If it doesn't restart by itself, hold Menu and Select until it does."
        Say '2. Connect it again. It appears in iTunes as an iPod: sync your music from there.'
        Say "   Don't choose Restore in iTunes. That erases the iPod and removes RockWheel."
        Say '3. Unplug it. The library comes in while the progress bar runs.'
        Say ''
        if ($script:Model.Target -eq 'ipodcolor') {
            Say 'To start the stock firmware instead, turn the Hold switch on while it boots.'
        } else {
            Say "To start the stock firmware instead, for video, turn the Hold switch on while it boots."
        }
    } else {
        Say "Unplug the iPod. If it doesn't restart by itself, hold Menu and Select until it does."
    }
}

try {
    Invoke-Install
} catch {
    Write-Host ''
    Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($script:Work -and (Test-Path -LiteralPath $script:Work)) {
        Remove-Item -LiteralPath $script:Work -Recurse -Force -ErrorAction SilentlyContinue
    }
}
