param(
    [Parameter(Position=0)]
    [ValidateSet('bootstrap','configure','build','test','core-test','benchmark','run','editor','export','verify','all')]
    [string]$Action = 'build',
    [ValidateSet('Debug','Release')][string]$Configuration = 'Debug',
    [switch]$Clean,
    [switch]$Rebuild,
    [switch]$MaterialSoak
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -LiteralPath "$PSScriptRoot/versions.json" -Raw | ConvertFrom-Json
$preset = 'windows-' + $Configuration.ToLowerInvariant()
$engineDir = "$PSScriptRoot/_cache/godot-$($lock.godot)"
$engine = "$engineDir/Godot_v$($lock.godot)_win64_console.exe"

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $Program $($Arguments -join ' ')" }
}
function Initialize-Toolchain {
    $env:VSLANG = '1033'
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Missing Visual Studio Installer/vswhere.exe; install MSVC C++ build tools.' }
    $instances = & $vswhere -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
    $instance = $instances | Where-Object { Test-Path -LiteralPath "$($_.installationPath)/VC/Tools/MSVC/$($lock.msvc_tools)/bin/Hostx64/x64/cl.exe" } | Select-Object -First 1
    if (!$instance) { throw "Missing locked MSVC tools $($lock.msvc_tools). Install that version using Visual Studio Installer." }
    Import-Module -Name "$($instance.installationPath)/Common7/Tools/Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $instance.installationPath -SkipAutomaticLocation -Arch amd64 -HostArch amd64 -DevCmdArguments "-no_logo -vcvars_ver=$($lock.msvc_tools) -winsdk=$($lock.windows_sdk)" | Out-Null
    if ((& cmake --version | Select-Object -First 1) -ne "cmake version $($lock.cmake)") { throw "Use CMake $($lock.cmake)." }
    if ((& ninja --version) -ne $lock.ninja) { throw "Use Ninja $($lock.ninja)." }
    if ((& python --version) -ne "Python $($lock.python)") { throw "Use Python $($lock.python)." }
}
function Bootstrap {
    if (!(Test-Path -LiteralPath "$projectRoot/third_party/box2d/.git")) {
        Invoke-Checked 'git' @('clone','--depth','1','--branch',$lock.box2d_tag,'https://github.com/erincatto/box2d.git',"$projectRoot/third_party/box2d")
    }
    $physicsRevision = & git -C "$projectRoot/third_party/box2d" rev-parse HEAD
    if ($physicsRevision -ne $lock.box2d_commit) { throw 'Box2D revision mismatch; preserve local edits and restore the locked commit.' }
    if (!(Test-Path -LiteralPath "$projectRoot/third_party/godot-cpp/.git")) {
        Invoke-Checked 'git' @('clone','--depth','1','--branch',$lock.godot_cpp_tag,'https://github.com/godotengine/godot-cpp.git',"$projectRoot/third_party/godot-cpp")
    }
    $revision = & git -C "$projectRoot/third_party/godot-cpp" rev-parse HEAD
    if ($revision -ne $lock.godot_cpp_commit) { throw 'godot-cpp revision mismatch; preserve local edits and restore the locked commit.' }
    Invoke-Checked 'python' @("$PSScriptRoot/fetch_godot.py")
    Require-Engine
}
function Require-Engine {
    if (!(Test-Path -LiteralPath $engine)) { throw 'Missing pinned standard Godot; run tools/project.ps1 bootstrap.' }
    $version = & $engine --version
    if ($version -notlike '4.7.2.stable.official.*') { throw "Unexpected Godot version: $version" }
}
function Build-Project([string]$Name = $preset) {
    Initialize-Toolchain
    Invoke-Checked 'cmake' @('--preset',$Name)
    $buildArguments = @('--build','--preset',$Name)
    if ($Clean) { $buildArguments += '--clean-first' }
    Invoke-Checked 'cmake' $buildArguments
}
function Initialize-ExtensionList {
    # Preload at engine startup instead of discovering/loading a new extension
    # during the first editor scan (which crashes on first --import on this host).
    $extensionList = "$projectRoot/game/.godot/extension_list.cfg"
    if (!(Test-Path -LiteralPath $extensionList)) {
        New-Item -ItemType Directory -Path "$projectRoot/game/.godot" -Force | Out-Null
        [IO.File]::WriteAllText($extensionList, "res://pixel_sandbox.gdextension`n", [Text.UTF8Encoding]::new($false))
    }
}
function Import-Project {
    Require-Engine
    Initialize-ExtensionList
    Invoke-Checked $engine @('--headless','--path',"$projectRoot/game",'--editor','--import')
}
function Open-Editor {
    Require-Engine
    if ($Rebuild -or $Clean -or !(Test-Path -LiteralPath "$projectRoot/game/bin/pixel_host.debug.dll")) {
        Build-Project 'windows-debug'
    } else {
        Write-Host 'Opening existing development build. After C++ changes, use editor -Rebuild.'
    }
    Initialize-ExtensionList
    # The visible editor performs its own incremental resource scan/import.
    # Avoid loading the project once headlessly and then again interactively.
    Invoke-Checked $engine @('--editor','--path',"$projectRoot/game")
}
function Test-Project {
    # The editor always selects the debug-feature extension, including when
    # native tests use a Release preset. Release host is tested by verify.
    if ($Configuration -eq 'Release') { Build-Project 'windows-debug' }
    Build-Project
    Import-Project
    Invoke-Checked 'ctest' @('--preset',$preset)
}
function Export-Project {
    $script:Configuration = 'Release'
    $script:preset = 'windows-release'
    Build-Project 'windows-debug' # Required for editor import on a fresh checkout.
    Build-Project
    Import-Project
    New-Item -ItemType Directory -Path "$projectRoot/dist/windows" -Force | Out-Null
    Invoke-Checked $engine @('--headless','--path',"$projectRoot/game",'--export-release','Windows Desktop',"$projectRoot/dist/windows/PixelSandbox.exe")
    $licenseDir = "$projectRoot/dist/windows/licenses"
    New-Item -ItemType Directory -Path $licenseDir -Force | Out-Null
    Copy-Item -LiteralPath "$projectRoot/third_party/godot-cpp/LICENSE.md" -Destination "$licenseDir/godot-cpp.txt"
    Copy-Item -LiteralPath "$projectRoot/third_party/box2d/LICENSE" -Destination "$licenseDir/box2d.txt"
    Copy-Item -LiteralPath "$projectRoot/THIRD_PARTY_NOTICES.md" -Destination $licenseDir
    Copy-Item -LiteralPath "$engineDir/COPYRIGHT.txt" -Destination "$licenseDir/godot-COPYRIGHT.txt"
    Copy-Item -LiteralPath "$engineDir/LICENSE.txt" -Destination "$licenseDir/godot-LICENSE.txt"
    Copy-Item -LiteralPath "$projectRoot/tools/versions.json" -Destination "$projectRoot/dist/windows/build-versions.json"
}
function Verify-Export {
    $exe = "$projectRoot/dist/windows/PixelSandbox.exe"
    if (!(Test-Path -LiteralPath $exe)) { throw 'Missing export; run export first.' }
    $logName = if ($MaterialSoak) { 'material-soak' } else { 'export-smoke' }
    $runArguments = if ($MaterialSoak) { @('--headless','--fixed-fps','60','--','--material-soak-test') } else { @('--headless','--','--smoke-test') }
    $expected = if ($MaterialSoak) { 'MATERIAL SOAK PASS' } else { 'SMOKE PASS' }
    $process = Start-Process -FilePath $exe -ArgumentList $runArguments -WindowStyle Hidden -PassThru -Wait -RedirectStandardOutput "$projectRoot/dist/$logName.log" -RedirectStandardError "$projectRoot/dist/$logName-errors.log"
    Get-Content -LiteralPath "$projectRoot/dist/$logName.log"
    $errors = Get-Content -LiteralPath "$projectRoot/dist/$logName-errors.log" -Raw
    if ($errors) { Write-Host $errors }
    if ($process.ExitCode -ne 0 -or $errors -match 'ERROR:|SMOKE FAIL' -or !(Select-String -LiteralPath "$projectRoot/dist/$logName.log" -Pattern $expected -Quiet)) {
        throw "Export verification failed: exit $($process.ExitCode)."
    }
}
Push-Location -LiteralPath $projectRoot
try {
    switch ($Action) {
        bootstrap { Bootstrap }
        configure { Initialize-Toolchain; Invoke-Checked 'cmake' @('--preset',$preset) }
        build { Build-Project }
        test { Test-Project }
        core-test { Build-Project 'core-only'; Invoke-Checked 'ctest' @('--preset','core-only') }
        benchmark {
            Initialize-Toolchain
            Invoke-Checked 'cmake' @('--preset',$preset)
            Invoke-Checked 'cmake' @('--build','--preset',$preset,'--target','pixel_material_benchmark')
            Invoke-Checked "$projectRoot/build/$preset/pixel_material_benchmark.exe" @("$projectRoot/game","$projectRoot/build/benchmark-$Configuration")
        }
        run { Build-Project 'windows-debug'; Import-Project; Invoke-Checked $engine @('--path',"$projectRoot/game") }
        editor { Open-Editor }
        export { Export-Project }
        verify { Verify-Export }
        all { Bootstrap; Test-Project; Export-Project; Verify-Export }
    }
} finally { Pop-Location }
