<# : batch part, the PowerShell part starts under the line that closes this comment
@echo off
setlocal

rem Takes the offsets pasted into offset.txt (the whole header as offsets.imtheo.lol gives it),
rem puts them into Glory\src\Offsets\Offsets.hpp and builds. The header that was in use is kept
rem in offset_backups\. Nothing is changed when offset.txt is not a whole header, or lacks an
rem offset the program uses.
rem Usage: update_offsets.bat [nobuild]

set "OFFSETS_SELF=%~f0"
set "OFFSETS_ROOT=%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -Command "Invoke-Expression (Get-Content -Raw -LiteralPath $env:OFFSETS_SELF)"
set "RESULT=%errorlevel%"

rem 2: these offsets are in use already and the exe was built with them
if "%RESULT%"=="2" (
    set "RESULT=0"
    goto :done
)
if not "%RESULT%"=="0" goto :done
if /i "%~1"=="nobuild" goto :done

tasklist /fi "imagename eq Glory.exe" /nh 2>nul | find /i "Glory.exe" >nul
if not errorlevel 1 (
    echo [offsets] Glory is running, so its exe cannot be replaced. Close it and run build.bat.
    set "RESULT=1"
    goto :done
)

call "%OFFSETS_ROOT%build.bat"
set "RESULT=%errorlevel%"

:done
rem Started with a double click, the window would close before anything could be read
echo %cmdcmdline% | find /i "%~nx0" >nul && pause
exit /b %RESULT%
#>

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$root = $env:OFFSETS_ROOT
$pasted_file = Join-Path $root 'offset.txt'
$header_file = Join-Path $root 'Glory\src\Offsets\Offsets.hpp'
$source_dir = Join-Path $root 'Glory\src'
$backup_dir = Join-Path $root 'offset_backups'
$exe_file = Join-Path $root 'build\Glory.exe'

function Write-Note([string]$text)
{
    Write-Host "[offsets] $text"
}

function Write-Problem([string]$text)
{
    Write-Host "[offsets] WARNING: $text" -ForegroundColor Yellow
}

function Stop-Update([string]$text)
{
    Write-Host "[offsets] $text" -ForegroundColor Red
    Write-Host '[offsets] Nothing was changed.' -ForegroundColor Red
    exit 1
}

# Reads the text of an offsets header: its version, every offset as "Group::Name" with its value, and
# the names of the groups. Throws with the reason when the text is not a whole header.
function Read-Offsets([string]$text)
{
    # Names are told apart by upper and lower case, as the compiler does
    $values = [System.Collections.Generic.Dictionary[string, uint64]]::new()
    $groups = [System.Collections.Generic.HashSet[string]]::new()
    $open = [System.Collections.Generic.List[string]]::new()
    $version = $null
    $saw_offsets = $false

    foreach ($line in ($text -split "`r?`n"))
    {
        if ($line -cmatch '^\s*namespace\s+(\w+)\s*\{\s*$')
        {
            $name = $Matches[1]

            if ($open.Count -eq 0)
            {
                if ($name -cne 'Offsets') { throw "it opens namespace $name where namespace Offsets belongs" }
                $saw_offsets = $true
            }
            elseif ($open.Count -eq 1)
            {
                [void]$groups.Add($name)
            }

            $open.Add($name)
        }
        elseif ($line -cmatch '^\s*\}\s*$')
        {
            if ($open.Count -eq 0) { throw 'it closes more namespaces than it opens' }
            $open.RemoveAt($open.Count - 1)
        }
        elseif ($line -cmatch '\bClientVersion\s*=\s*"([^"]*)"')
        {
            $version = $Matches[1]
        }
        elseif ($line -cmatch '^\s*inline\s+constexpr\s+[\w:]+\s+(\w+)\s*=\s*(0[xX][0-9a-fA-F]+|[0-9]+)\s*;')
        {
            $name = $Matches[1]
            $number = $Matches[2]

            if ($open.Count -eq 0) { throw "the offset $name is outside namespace Offsets" }

            # Without the leading "Offsets"
            $key = (@($open | Select-Object -Skip 1) + $name) -join '::'

            if ($number -cmatch '^0[xX]')
            {
                $values[$key] = [Convert]::ToUInt64($number.Substring(2), 16)
            }
            else
            {
                $values[$key] = [Convert]::ToUInt64($number, 10)
            }
        }
    }

    if (-not $saw_offsets) { throw 'there is no "namespace Offsets {" in it' }
    if ($open.Count -ne 0) { throw "it ends inside namespace $($open[$open.Count - 1]), the end of the file is missing" }
    if (-not $version) { throw 'there is no ClientVersion in it' }
    if ($values.Count -eq 0) { throw 'there is not one offset in it' }

    return [pscustomobject]@{ Version = $version; Values = $values; Groups = $groups }
}

# Every "Group::Name" (or bare name) the program's own source asks namespace Offsets for
function Get-UsedOffsets
{
    $used = [System.Collections.Generic.SortedSet[string]]::new([System.StringComparer]::Ordinal)
    $header = [System.IO.Path]::GetFullPath($header_file)

    foreach ($file in (Get-ChildItem -LiteralPath $source_dir -Recurse -File))
    {
        if (('.cpp', '.h', '.hpp') -notcontains $file.Extension) { continue }
        if ($file.FullName -eq $header) { continue }

        # A name that is only talked about in a comment is not a use
        $code = [System.IO.File]::ReadAllText($file.FullName) -replace '//[^\r\n]*', ''

        foreach ($match in [regex]::Matches($code, '\bOffsets::(\w+)(?:::(\w+))?'))
        {
            if ($match.Groups[2].Success)
            {
                [void]$used.Add($match.Groups[1].Value + '::' + $match.Groups[2].Value)
            }
            else
            {
                [void]$used.Add($match.Groups[1].Value)
            }
        }
    }

    return , $used
}

function Test-Has($offsets, [string]$name)
{
    return $offsets.Values.ContainsKey($name) -or $offsets.Groups.Contains($name) -or $name -ceq 'ClientVersion'
}

try
{
    if (-not (Test-Path -LiteralPath $source_dir))
    {
        Stop-Update "Glory\src was not found next to this file ($root)."
    }

    if (-not (Test-Path -LiteralPath $pasted_file))
    {
        Stop-Update "offset.txt is not there. Paste the whole header from offsets.imtheo.lol into $pasted_file"
    }

    $pasted_text = [System.IO.File]::ReadAllText($pasted_file)

    if ($pasted_text -cnotmatch '(?m)^\s*#pragma\s+once\b')
    {
        Stop-Update 'offset.txt has no "#pragma once" line. Paste the whole header, from its first line on.'
    }

    try
    {
        $new = Read-Offsets $pasted_text
    }
    catch
    {
        Stop-Update "offset.txt is not a whole offsets header: $($_.Exception.Message)."
    }

    $old = $null
    if (Test-Path -LiteralPath $header_file)
    {
        try
        {
            $old = Read-Offsets ([System.IO.File]::ReadAllText($header_file))
        }
        catch
        {
            Write-Problem 'the Offsets.hpp in use could not be read, so it cannot be said what changes.'
        }
    }

    # An offset the program uses and the new header lacks would only show as a failed build, with the
    # old header already gone. One the old header lacked as well is not this update's doing.
    $used = Get-UsedOffsets
    $missing = @($used | Where-Object { -not (Test-Has $new $_) -and (-not $old -or (Test-Has $old $_)) })
    if ($missing.Count -ne 0)
    {
        Stop-Update "offset.txt has no value for what Glory uses: $($missing -join ', '). The dumper renamed or dropped it, the source has to follow first."
    }

    # What Roblox hands out right now. Offsets of another version read the wrong places, and nothing
    # in the program notices.
    try
    {
        $live = (Invoke-RestMethod -Uri 'https://clientsettingscdn.roblox.com/v2/client-version/WindowsPlayer' -TimeoutSec 5).clientVersionUpload
        if ($live -and $live -cne $new.Version)
        {
            Write-Problem "Roblox is on $live now, offset.txt is for $($new.Version)."
        }
    }
    catch
    {
        # No answer: the update does not depend on it
    }

    $changed = @()
    $added = 0
    $removed = 0
    if ($old)
    {
        foreach ($key in $new.Values.Keys)
        {
            if (-not $old.Values.ContainsKey($key)) { $added++ }
            elseif ($old.Values[$key] -ne $new.Values[$key]) { $changed += $key }
        }

        foreach ($key in $old.Values.Keys)
        {
            if (-not $new.Values.ContainsKey($key)) { $removed++ }
        }
    }

    if ($old -and $old.Version -ceq $new.Version -and $changed.Count -eq 0 -and $added -eq 0 -and $removed -eq 0)
    {
        Write-Note "offset.txt holds what is in use already ($($new.Version), $($new.Values.Count) offsets)."

        # Rewriting the header would only make every file compile again
        if ((Test-Path -LiteralPath $exe_file) -and (Get-Item -LiteralPath $exe_file).LastWriteTime -gt (Get-Item -LiteralPath $header_file).LastWriteTime)
        {
            exit 2
        }

        Write-Note 'build\Glory.exe is older than them.'
        exit 0
    }

    if ($old)
    {
        Write-Note "$($old.Version) -> $($new.Version): $($changed.Count) of $($new.Values.Count) offsets changed, $added new, $removed gone."

        $used_changed = @($used | Where-Object { $changed -ccontains $_ })
        Write-Note "Glory uses $(@($used | Where-Object { $new.Values.ContainsKey($_) }).Count) of them, $($used_changed.Count) of those changed$(if ($used_changed.Count -ne 0) { ':' } else { '.' })"
        foreach ($key in $used_changed)
        {
            Write-Note ('  {0,-36} 0x{1:x} -> 0x{2:x}' -f $key, $old.Values[$key], $new.Values[$key])
        }
    }
    else
    {
        Write-Note "$($new.Version), $($new.Values.Count) offsets."
    }

    # The dumper writes 0x0 for what it could not find
    $lost = @($used | Where-Object { $new.Values.ContainsKey($_) -and $new.Values[$_] -eq 0 -and (-not $old -or -not $old.Values.ContainsKey($_) -or $old.Values[$_] -ne 0) })
    if ($lost.Count -ne 0)
    {
        Write-Problem "the dumper did not find $($lost -join ', ') (0x0). What reads them will not work."
    }

    if (Test-Path -LiteralPath $header_file)
    {
        New-Item -ItemType Directory -Force -Path $backup_dir | Out-Null

        $label = if ($old) { $old.Version -replace '[^\w.-]', '_' } else { 'unknown' }
        $backup = Join-Path $backup_dir ('Offsets_{0}_{1}.hpp' -f $label, (Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'))
        Copy-Item -LiteralPath $header_file -Destination $backup
        Write-Note "The header that was in use is kept as $backup"
    }

    # As the other source files: no byte order mark, plain line ends
    $text = ($pasted_text -replace "`r`n?", "`n").Trim() + "`n"
    [System.IO.File]::WriteAllText($header_file, $text, [System.Text.UTF8Encoding]::new($false))

    Write-Note "Glory\src\Offsets\Offsets.hpp now holds $($new.Version)."
    exit 0
}
catch
{
    Write-Host "[offsets] $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
