[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    # Self-contained package: the obs-videopress folder (bin/64bit + data) plus instructions,
    # to copy into C:\ProgramData\obs-studio\plugins by hand.
    $PortableRoot = "${ProjectRoot}/release/${OutputName}-portable"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -Path $PortableRoot
    New-Item -ItemType Directory -Path $PortableRoot | Out-Null
    Copy-Item -Recurse -Path (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}").FullName -Destination $PortableRoot
    Copy-Item -Path "${ProjectRoot}/.github/scripts/portable/INSTALL-windows.txt" -Destination "${PortableRoot}/INSTALL.txt"

    $CompressArgs = @{
        Path = (Get-ChildItem -Path $PortableRoot).FullName
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}-portable.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Remove-Item -Recurse -Force -Path $PortableRoot
    Log-Group
}

Package
