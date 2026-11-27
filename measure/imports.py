"""Repeats FINDINGS 2026-11-26, "the device byte watched in two visible runs, and the installation's own libraries":
what the executable imports, which of those libraries ship in the installation itself, and what the shipped ones
ask of the console libraries the weave provides.

    python measure/imports.py "<your game folder>\\game_scarlett_master.exe"

Reads the executable and the libraries beside it read-only; nothing is written.
"""
import os
import sys

from pe_image import Checks, Image, open_from_arguments

exe = open_from_arguments(__doc__)
folder = os.path.dirname(os.path.abspath(sys.argv[1]))
system32 = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
c = Checks("imports: the executable's libraries and the installation's own ones")

table = exe.imports()
c.same("libraries the executable imports", len(table), 68)
c.same("functions the executable imports", sum(len(f) for _, f in table), 587)

installed = {name.lower(): name for name in os.listdir(folder)}
shipped = sorted(name for name, _ in table if name.lower() in installed)
c.same("of them, libraries that ship in the installation", sorted(n.lower() for n in shipped),
       sorted(["xboxbvhbuilder.dll", "bink2scarlett.dll", "amd_fidelityfx_loader_xbox.dll",
               "libhttpclient.gdk.dll", "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"]))


def library_imports(file_name):
    lib = Image(os.path.join(folder, installed[file_name.lower()]))
    return {name.lower(): functions for name, functions in lib.imports()}


def console_asks(file_name):
    asks = library_imports(file_name)
    return {name: sorted(functions) for name, functions in asks.items() if name in ("d3d12_xs.dll", "xmem.dll")}


c.same("XboxBvhBuilder.dll asks of the console libraries", console_asks("XboxBvhBuilder.dll"),
       {"d3d12_xs.dll": ["D3D12SerializeVersionedRootSignature"], "xmem.dll": ["XMemVirtualQuery"]})
bink = library_imports("Bink2Scarlett.dll")
c.same("Bink2Scarlett.dll asks of the console libraries", console_asks("Bink2Scarlett.dll"),
       {"xmem.dll": ["XMemAlloc", "XMemFree"]})
c.same("Bink2Scarlett.dll imports XAudio2_9.dll", "xaudio2_9.dll" in bink, True)
c.same("XAudio2_9.dll is a Windows library (in System32)", os.path.exists(os.path.join(system32, "XAudio2_9.dll")),
       True)

upscalers = sorted(n for n in installed if "fidelityfx" in n and "upscaler" in n and n.endswith(".dll"))
c.same("FidelityFX upscaler libraries in the installation", len(upscalers), 1)
if upscalers:
    c.same("the FidelityFX upscaler asks of the console libraries", console_asks(upscalers[0]),
           {"d3d12_xs.dll": ["D3D12CreateRootSignatureDeserializer"],
            "xmem.dll": ["XMemAlloc", "XMemFree", "XMemGetWorkingSetStatistics"]})

# "Everything those shipped libraries import is present": each library they name is in the installation, in
# Windows' System32, or one of the executable's own imports (the console libraries the weave provides).
own = {name.lower() for name, _ in table}
missing = set()
for name in shipped + upscalers:
    for wanted in library_imports(name):
        if wanted not in installed and wanted not in own and not os.path.exists(os.path.join(system32, wanted)):
            if not wanted.startswith(("api-ms-win-", "ext-ms-win-")):
                missing.add(wanted)
c.same("libraries the shipped ones ask for that are nowhere (not counting Windows' api-set names)",
       sorted(missing), [])
c.done()
