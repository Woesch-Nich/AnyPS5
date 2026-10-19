# UFC6 on Windows: developer setup draft

For the `share/source-review` branch of [Woesch-Nich/AnyPS5](https://github.com/Woesch-Nich/AnyPS5/tree/share/source-review), commit `c9e67d6c0fb6c49338a05d7f0e1fb10bff280773`.

This is a source-reviewed setup recipe, **not an end-to-end verified reproduction on that public commit**. Our private investigation reached AGC driver initialization in one isolated run; that initialization and the ownership fix under review are absent from this public snapshot. Do not expect identical progress or working graphics. A black window alone proves neither useful progress nor a successful setup.

Use your own dump and keep all original files intact. No game executable, module, asset, cache, capture or proprietary DLL accompanies this guide. Keep the runtime outside the Git checkout.

## 1. Build one coherent Windows set

Use x86-64 Windows, Git, Python 3.9+, CMake 3.22.1+, Ninja and the toolchain pinned in `docs/dev/BUILD.md`: WinLibs MinGW-w64 GCC 15.2.0, `x86_64-ucrt-posix-seh`, release `15.2.0posix-14.0.0-ucrt-r7`. Put its `mingw64/bin` first on PATH. Use a compatible Vulkan GPU/driver; see `docs/user/USAGE.md` for device requirements.

Example PowerShell paths; change them for your machine. Run all steps in the **same PowerShell session** so `$Toolchain`, `$Build`, `$InputRoot`, `$Run` and the process environment remain defined. Verify the printed checkout commit against the revision above before building.

```powershell
$Toolchain = 'D:\Tools\mingw64\bin'
$env:PATH = "$Toolchain;$env:PATH"
git clone --branch share/source-review --single-branch https://github.com/Woesch-Nich/AnyPS5.git D:\Dev\AnyPS5
if ($LASTEXITCODE -ne 0) { throw 'Clone failed' }
Set-Location D:\Dev\AnyPS5
git switch --detach c9e67d6c0fb6c49338a05d7f0e1fb10bff280773
if ($LASTEXITCODE -ne 0) { throw 'Pinned source checkout failed' }
git rev-parse HEAD
git submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw 'Submodule initialization failed' }
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DANYPS5_ENABLE_SPIRV_TOOLS=ON -DBUILD_TESTING=OFF
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
cmake --build build-win --target relinker libs --parallel 1
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
```

Use a fresh build directory for this revision/toolchain. `libs` is required; the default build alone can leave system libraries unbuilt. Do not combine old release libraries with the new executable. If FFmpeg download certificate validation fails, use the valid CA-bundle setting described in `BUILD.md`; keep certificate verification and security software enabled.

## 2. Prepare a private input copy

Create `D:\UFC6-work\input\app0` as a **separate working copy** of your own title's complete app0, including its matching update/patch content. Keep the original layout, case and filenames: data, scripts, metadata, manifests and patch resources all matter. Do not mix executable/modules/assets from different title revisions.

The locally investigated layout included an engine module at `app0/Engine.Render.Core2.PlatformPs5.prx`, plus bundled `libc.prx` and `libSceNpCppWebApi.prx` under `app0/sce_module`. Those are **title-owned inputs**, distinct from the host `libs/libc.prx` built by AnyPS5. Your dump may differ.

The converter requires ELF inputs. If an input is already ELF, use it directly. For a decrypted SELF/fSELF container, unwrap **into the working copy**, preserving the original. The script does not decrypt encrypted content and should reject it. For example, from the checkout:

```powershell
python tools/unwrap_self.py D:\UFC6-work\input\app0\eboot.bin D:\UFC6-work\input\eboot.elf
if ($LASTEXITCODE -ne 0) { throw 'Executable unwrap failed' }
```

Likewise unwrap any decrypted bundled containers before conversion. Write each unwrapped ELF to a temporary file, then replace that container **under its original module filename inside the working copy only**. In particular, `input/app0/Engine.Render.Core2.PlatformPs5.prx` must contain ELF bytes; leaving it as SELF makes the root scan skip the engine. Keep the original dump untouched.

Leave `input/app0/eboot.bin` as the original resource copy: the separate `input/eboot.elf` is the conversion input. The root scan will warn that this original SELF `eboot.bin` is skipped; that warning is expected. A skipped required engine or bundled library is a missing conversion, not success.

## 3. Relink and stage the runtime

The historical private command used `--extra-guest-module Engine.Render.Core2.PlatformPs5.prx`. **That switch is not accepted by the published parser.** Its supported `--module-dir .` scans ELF files directly in the selected module parent, including the root engine module, in addition to `sce_module`. This substitution is verified from source, not by a UFC6 run on the public branch.

```powershell
$Build = 'D:\Dev\AnyPS5\build-win'
$InputRoot = 'D:\UFC6-work\input'
$Run = 'D:\UFC6-work\runtime'
if (Test-Path -LiteralPath $Run) { throw 'Choose a new empty runtime directory' }
New-Item -ItemType Directory -Path "$Run\libs", "$Run\shader_cache" | Out-Null
Copy-Item -LiteralPath "$InputRoot\app0" -Destination "$Run\app0" -Recurse
Set-Location -LiteralPath $Run
& "$Build\core\relinker\relinker.exe" --windows --windows-diagnostics --sce-module-path "$InputRoot\app0" --module-dir . unused-filter=0 "$InputRoot\eboot.elf" "$Run\UFC6.exe"
if ($LASTEXITCODE -ne 0) { throw 'Relink failed; do not launch incomplete output' }
Copy-Item "$Build\core\libs\libs\*.prx" "$Run\libs"
Get-ChildItem "$Build\core\libs\libs" -File -Filter '*.dll' | Copy-Item -Destination "$Run\libs"
foreach ($Name in 'libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll') {
    Copy-Item -LiteralPath "$Toolchain\$Name" -Destination "$Run\libs"
}
```

On an Intel host, add the documented `--to-intel` switch at conversion; this cannot translate instructions generated at runtime. Do not add private fixed addresses, import filtering experiments, syscall-check bypasses or library exclusions.

The order above is intentional: copy resources first, then relink. `GuestModuleBuilder.cpp` constructs output paths from the **output executable's parent**, so this command writes the root engine's `.guest.prx` into `runtime/app0` and the bundled modules into `runtime/app0/sce_module`, not into `input/app0`. Do not copy the input tree over these generated outputs afterward. Changing to `$Run` also keeps `windows-diagnostics-imports.txt` outside the source checkout.

Check the converter's `Guest module:` output for all required title modules, including the root engine module and both bundled modules when present. Preserve generated `.guest.prx` files at their printed paths. `libs/` contains AnyPS5's **patched** host libraries from `build/core/libs/libs`, not the `unpatched` subdirectory and not original console libraries.

Expected arrangement:

```text
runtime/
  UFC6.exe
  libs/                 AnyPS5 patched .prx + matching runtime DLLs
  app0/                 complete private title resources and metadata
    eboot.bin
    sce_sys/param.json
    Engine.Render.Core2.PlatformPs5.prx
    Engine.Render.Core2.PlatformPs5.prx.guest.prx
    sce_module/         bundled inputs and converter-generated outputs
  shader_cache/         isolated, initially empty
```

Our private staging also carried `memres`, `download0` and a root `sce_module` directory from its established runtime. Their necessity for a fresh public-branch setup has not been established. Do not invent contents or copy someone else's runtime. One additional loader DLL, `api-ms-win-core-delayload-l1-1-0.dll`, was present locally; its portable source/necessity is unverified, so it is not supplied or prescribed here. If loading fails, record the exact missing dependency and resolve its legitimate origin first; do not download random replacement DLLs.

## 4. Launch one short diagnostic run

Finish compilation first. Launch from the runtime directory with an isolated cache. No experimental pruning flag or shader dumping is needed for this first check. The following example waits at most 60 seconds for normal exit, then stops only the process it created, retaining its original process handle. It has not been executed against the public build.

```powershell
$env:APS5_INDIRECT_DEFAULT_PRUNE = $null
$env:APS5_DUMP_SHADERS = $null
$env:ANYPS5_SHADER_CACHE_DIR = "$Run\shader_cache"
$env:ANYPS5_NO_SHADER_CACHE = $null
$env:PATH = "$Run\libs;$Toolchain;$env:PATH"
if (Get-Process -Name UFC6,kyty_emulator,ninja,cmake,cc1plus,ld -ErrorAction SilentlyContinue) {
    throw 'Another game or build is active; do not launch'
}
$p = $null
try {
    $p = Start-Process -FilePath "$Run\UFC6.exe" -WorkingDirectory $Run -PassThru -RedirectStandardOutput "$Run\startup-out.txt" -RedirectStandardError "$Run\startup-err.txt"
    $heldHandle = $p.Handle
    if (-not $p.WaitForExit(60000)) {
        $p.Kill()
        if (-not $p.WaitForExit(5000)) { throw 'Original process has not confirmed exit' }
    }
} finally {
    if ($null -ne $p) {
        if (-not $p.HasExited) { $p.Kill(); $null = $p.WaitForExit(5000) }
        $p.Dispose()
    }
}
```

Stop earlier if there is a clear loader/runtime failure. Do not leave a black window running overnight or repeatedly retry unchanged inputs.

## 5. Report the first reproducible boundary

Record source commit, compiler, CPU/GPU/driver, conversion flags, whether the three expected guest modules were generated, and the first loader/runtime error. A small reviewed text excerpt is preferable to sharing raw logs. Error 193 can mean an original console module was mistaken for a host DLL; error 126 can mean a host library's dependent DLL is missing even when the `.prx` exists.

The public snapshot deliberately omits private AGC initialization and the unfinished arena-ownership correction. Private AGC table initialization, a black screen and playable rendering are separate milestones. Do not silently import private binaries to claim this public revision reproduces them.

Before sharing evidence, remove personal paths and inspect for game-derived code/data. Do not upload the dump, converted executable/modules, assets, shader cache or captures to the source repository. This document contains setup instructions only and makes no legal-clearance claim.
