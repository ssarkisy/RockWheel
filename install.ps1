<#
Installs or updates RockWheel on an iPod Video (5th or 5.5th generation) from Windows.

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
# Rockbox's own bootloader, the one Rockbox Utility installs
$Bootloader = 'bootloader-ipodvideo.ipod'
$BootloaderSha256 = '19dfa0e930689f5afdeaae18f4c56b472cbed9c6c3a7039bb32b646d8040298f'
# what RockWheel has and plain Rockbox hasn't
$Marker = '.rockbox/wps/iClassic-Square/About Backdrop.bmp'

$script:Work = ''
$script:Files = ''
$script:Patcher = ''

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

function Get-Release([string]$Name, [switch]$Optional) {
    $out = Join-Path $script:Files $Name
    try {
        Invoke-WebRequest -UseBasicParsing -Uri "$ReleaseUrl/$Name" -OutFile $out
        return $true
    } catch {
        Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue
        if ($Optional) { return $false }
        Die "Couldn't download $Name from $ReleaseUrl"
    }
}

# each of the files against its line in a list of checksums, as shasum -c reads it
function Test-Sums([string]$SumsName, [string[]]$Names) {
    $sums = @{}
    foreach ($line in Get-Content -LiteralPath (Join-Path $script:Files $SumsName)) {
        if ($line -match '^([0-9a-fA-F]{64})\s+\*?(.+)$') {
            $sums[$Matches[2].Trim()] = $Matches[1].ToLowerInvariant()
        }
    }
    foreach ($n in $Names) {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $script:Files $n)).Hash.ToLowerInvariant()
        if (-not $sums.ContainsKey($n) -or $sums[$n] -ne $hash) {
            Die "The download is damaged: its checksums don't match."
        }
    }
}

function Get-Files {
    Step 'Getting the files'
    if ($From) {
        $script:Files = (Resolve-Path -LiteralPath $From).Path
    } else {
        $script:Files = $script:Work
        # Windows PowerShell 5.1 offers older TLS first, and its progress bar
        # makes a download many times slower
        [Net.ServicePointManager]::SecurityProtocol = `
            [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $ProgressPreference = 'SilentlyContinue'
        foreach ($f in @('rockbox.zip', 'ipodpatcher.exe', $Bootloader, 'SHA256SUMS', 'SHA256SUMS-windows')) {
            Say "    $f"
            Get-Release $f | Out-Null
        }
        Test-Sums 'SHA256SUMS' @('rockbox.zip', $Bootloader)
        Test-Sums 'SHA256SUMS-windows' @('ipodpatcher.exe')
        # the games, with a checksum of their own; a release without them is
        # only asked nothing about them
        if ((Get-Release 'rockbox-games.zip' -Optional) -and (Get-Release 'SHA256SUMS-games' -Optional)) {
            Say '    rockbox-games.zip'
            Test-Sums 'SHA256SUMS-games' @('rockbox-games.zip')
        } else {
            Remove-Item -LiteralPath (Join-Path $script:Files 'rockbox-games.zip') -ErrorAction SilentlyContinue
        }
    }
    foreach ($f in @('rockbox.zip', 'ipodpatcher.exe', $Bootloader)) {
        if (-not (Test-Path -LiteralPath (Join-Path $script:Files $f))) { Die "$f is missing from $($script:Files)" }
    }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $script:Files $Bootloader)).Hash.ToLowerInvariant()
    if ($hash -ne $BootloaderSha256) { Die "$Bootloader isn't the bootloader this installer expects." }
    $script:Patcher = Join-Path $script:Files 'ipodpatcher.exe'
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

function Read-IPod([int]$DiskNumber) {
    $r = Invoke-Patcher $DiskNumber @(, @('--list'))
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
    return [pscustomobject]@{
        Model = $model
        Kind = $kind
        HasBootloader = ($r.Output -match 'Third-party bootloader')
    }
}

function Add-Bootloader([int]$DiskNumber) {
    Step 'Installing the bootloader'
    if (Would 'add the Rockbox bootloader to the firmware partition, keeping the stock firmware') { return }
    $r = Invoke-Patcher $DiskNumber @(
        @('--add-bootloader', (Join-Path $script:Files $Bootloader)),
        @('--list'))
    if ($r.ExitCode -ne 0) { Die 'Installing the bootloader failed. Close any File Explorer window on the iPod and run this again.' }
    if ($r.Output -notmatch 'Third-party bootloader') { Die "The bootloader didn't end up on the iPod." }
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

function Copy-Firmware([string]$Root) {
    Step 'Copying the firmware'
    if (Would 'copy the files of rockbox.zip to the iPod') { return }
    # Plain Rockbox's settings and database don't carry over
    $rockbox = Join-Path $Root '.rockbox'
    if ((Test-Path -LiteralPath $rockbox) -and -not (Test-Path -LiteralPath (Get-IPodPath $Root $Marker))) {
        $old = '.rockbox-old-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
        try { Rename-Item -LiteralPath $rockbox -NewName $old } catch { Die "Couldn't move the old .rockbox folder aside." }
        Say "    Your previous Rockbox folder is now $old on the iPod."
    }
    try { Expand-Into (Join-Path $script:Files 'rockbox.zip') $Root } catch { Die 'Copying failed. Is the iPod full?' }
}

# Rockbox's games for Extras > Games, in an archive of their own: asked about
# while they aren't on the iPod, then kept up to date with the firmware. A no is
# kept in a file on the iPod, so that an update doesn't ask again.
function Copy-Games([string]$Root) {
    $zip = Join-Path $script:Files 'rockbox-games.zip'
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
        if (Would "remove the $count games of Extras > Games that came with rockbox-games.zip") { return }
        foreach ($f in $files) { Remove-Item -LiteralPath (Get-IPodPath $Root $f) -ErrorAction SilentlyContinue }
        New-Item -ItemType File -Force -Path $declined | Out-Null
        return
    }
    if (-not $Games -and -not $have) {
        if (Test-Path -LiteralPath $declined) { return }
        Say ''
        if ($DryRun) {
            Say "    Would ask whether to add $count games from Rockbox to Extras > Games."
            return
        }
        $r = Read-Host "Add $count games from Rockbox to Extras > Games? About $mb MB. [y/N]"
        if ($r -notmatch '^(y|yes)$') {
            New-Item -ItemType File -Force -Path $declined | Out-Null
            Say '    Not added. Run this with -Games to add them later.'
            return
        }
    }
    Step 'Copying the games'
    if (Would 'copy the files of rockbox-games.zip to the iPod') { return }
    try { Expand-Into $zip $Root } catch { Die 'Copying the games failed. Is the iPod full?' }
    Remove-Item -LiteralPath $declined -ErrorAction SilentlyContinue
}

# --- playlist folders ----------------------------------------------------------

# iTunes leaves playlist folders out of what it syncs to an iPod Video. It
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
    Step 'RockWheel for iPod Video'
    if ($DryRun) { Say 'This is a dry run: nothing on the iPod is changed.' }
    Say 'People run it on iPods with flash cards, SD card adapters and the original hard'
    Say 'drive. Details are in the README.'

    $script:Work = Join-Path ([IO.Path]::GetTempPath()) ('rockwheel-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Path $script:Work | Out-Null

    if ($Folders) {
        Step 'Looking for the iPod'
        $letter = Get-IPodVolume (Find-IPod)
        if (-not $letter) { Die "The iPod's music partition isn't in File Explorer, or RockWheel isn't on it." }
        Put-Folders "$letter\"
        return
    }

    Get-Files

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
    if ($ipod.Model -notmatch 'Video') { Die "This isn't an iPod Video, which is the only model RockWheel supports." }

    if ($Uninstall) {
        if (-not $ipod.HasBootloader) { Die 'No bootloader is installed on this iPod.' }
        Step 'Removing the bootloader'
        $r = Read-Host 'The iPod will start its stock firmware again. Type yes to go on'
        if ($r -ne 'yes') { Die 'Stopped. Nothing was changed.' }
        if (-not (Would 'remove the bootloader')) {
            $res = Invoke-Patcher $disk.Index @(, @('--delete-bootloader'))
            if ($res.ExitCode -ne 0) { Die 'Removing the bootloader failed.' }
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

    $root = "$letter\"
    $fresh = -not $ipod.HasBootloader
    if ($ipod.HasBootloader) {
        Say ''
        Say 'The bootloader is already installed, so this only updates the firmware files.'
        Say 'Settings, the library and play counts on the iPod are kept.'
    } else {
        Add-Bootloader $disk.Index
    }
    Copy-Firmware $root
    Copy-Games $root
    Eject-IPod $letter

    if ($DryRun) {
        Step 'Dry run finished. Nothing was changed.'
        return
    }
    Step 'Done'
    if ($fresh) {
        Say "1. Unplug the iPod. If it doesn't restart by itself, hold Menu and Select until it does."
        Say '2. Connect it again. It appears in iTunes as an iPod: sync your music from there.'
        Say "   Don't choose Restore in iTunes. That erases the iPod and removes RockWheel."
        Say '3. Unplug it. The library comes in while the progress bar runs.'
        Say ''
        Say "To start the stock firmware instead, for video, turn the Hold switch on while it boots."
    } else {
        Say 'Unplug the iPod. It restarts into the new firmware by itself.'
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
