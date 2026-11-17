# D3D11 Impl early-hook

This branch adds an opt-in path for targets that create their D3D11 device before RenderDoc's
ordinary user-mode hooks can be installed. It is intended for controlled testing on a dedicated
Windows machine.

## Components

- `renderdoc/driver/d3d11/d3d11_hooks.cpp` exposes public, Core, and Impl hook entry points.
- `driver/rdoc_earlyhook.*` implements the early kernel detour protocol.
- `driver/rdocctl`, `driver/rdcprobe.c`, and `driver/pdb_syms` provide control and diagnostics.
- `validation/impl_earlyhook` contains self-owned capture and replay fixtures.

`C:\rdoc_kernel_single_layer.flag` selects kernel single-layer mode before the target starts. In
normal mode, upstream D3D11 user-mode hook registration remains enabled.

## Build and offline validation

Build `renderdoc/renderdoc.vcxproj` as Release x64, including its Breakpad prerequisites. Then run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\driver\build_tools.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\validation\impl_earlyhook\verify_symbols.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\validation\impl_earlyhook\run_validation.ps1
```

The symbol gate derives RVAs from the current DLL/PDB. Never reuse RVAs from an older build.

## Driver requirement

The driver project requires the Visual Studio WDK integration and the
`WindowsKernelModeDriver10.0` platform toolset. A Windows SDK installation alone is insufficient.
The driver is unsigned and should only be mapped in an isolated, rebootable test environment.

Do not load the driver, inject a process, or install a detour until the offline validation and
symbol gate both pass. A single boot should contain at most one driver mapping attempt.
