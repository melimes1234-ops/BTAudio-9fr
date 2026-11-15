$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$workspace = Split-Path -Parent $root
$clang = Join-Path $workspace 'toolchain/llvm/bin/clang.exe'
$linker = Join-Path $workspace 'toolchain/llvm/bin/ld.lld.exe'
$build = Join-Path $root 'build/bta-app'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$verMatch = [regex]::Match((Get-Content -Raw (Join-Path $root 'src/version.h')), '#define\s+BTA_VERSION\s+"([^"]+)"')
if (-not $verMatch.Success) { throw 'Could not parse BTA_VERSION from src/version.h' }
$ver = $verMatch.Groups[1].Value
$htmlText = [IO.File]::ReadAllText((Join-Path $root 'src/payload/web.html'), [Text.Encoding]::UTF8).Replace('__BTA_VERSION__', $ver)
$html = [Text.Encoding]::UTF8.GetBytes($htmlText)
$array = ($html | ForEach-Object { $_.ToString() }) -join ','
[IO.File]::WriteAllText((Join-Path $build 'web_page.h'), "static const char web_page[] = {$array,0};")
$flags = @('--target=x86_64-pc-freebsd12-elf','-O2','-fPIE','-ffreestanding','-fno-builtin','-fno-stack-protector','-mno-red-zone','-fno-asynchronous-unwind-tables','-Wall','-Wextra','-Werror','-I', $build)
$objects = @()
foreach ($name in @('launcher','worker')) {
    $obj = Join-Path $build "$name.o"
    & $clang @flags -c (Join-Path $root "src/payload/$name.c") -o $obj
    if ($LASTEXITCODE) { throw "Compile failed: $name" }
    $objects += $obj
}
foreach ($item in ,@('bt_accept_patch','bta_apply_patch')) {
    $obj = Join-Path $build "$($item[0]).o"
    & $clang @flags -mgeneral-regs-only "-D_start=$($item[1])" -c (Join-Path $root "src/payload/$($item[0]).c") -o $obj
    if ($LASTEXITCODE) { throw "Compile failed: $($item[0])" }
    $objects += $obj
}
$obj = Join-Path $build 'kresolve.o'
& $clang @flags -mgeneral-regs-only -c (Join-Path $root 'src/payload/kresolve.c') -o $obj
if ($LASTEXITCODE) { throw 'Compile failed: kresolve' }
$objects += $obj
$obj = Join-Path $build 'bt_devices.o'
& $clang @flags -mgeneral-regs-only -c (Join-Path $root 'src/payload/bt_devices.c') -o $obj
if ($LASTEXITCODE) { throw 'Compile failed: bt_devices' }
$objects += $obj
$script = Join-Path $root 'src/payload/payload.ld'
& $linker -T $script @objects -o (Join-Path $build 'BTA-Audio.debug.elf')
if ($LASTEXITCODE) { throw 'ELF link failed' }
& $linker --oformat=binary -T $script @objects -o (Join-Path $root "build/BTAudio4-$ver.bin")
if ($LASTEXITCODE) { throw 'Binary link failed' }
Write-Output "Built build/BTAudio4-$ver.bin"
