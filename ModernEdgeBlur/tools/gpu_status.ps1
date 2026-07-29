# gpu_status.ps1 - one-shot GPU-status report for an AE plugin.
#
# Run AFTER launching AE and applying the effect. Paste the whole output into
# a session; it is the evidence a fresh session needs to judge whether the
# MediaCore GPUVideoFilter module actually ran.
# See docs/AE_GPU_Effects.md section 4 for how to read the three signals.
#
#   powershell -ExecutionPolicy Bypass -File tools\gpu_status.ps1
#   powershell -ExecutionPolicy Bypass -File tools\gpu_status.ps1 -Plugin MM_Qualify
#
# (ASCII only on purpose: non-ASCII punctuation has broken this file before.)

param(
    [string]$Plugin    = "EdgeRepair",
    [string]$AEVersion = "26.2",
    [string]$LogPath   = "$env:TEMP\EdgeRepair_gpu.log"
)

$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm'
"=== GPU status: $Plugin (AE $AEVersion) - $stamp ==="

"`n--- 1. REGISTRY (decisive: is the GPU module registered?) ---"
$cache = "HKCU:\Software\Adobe\After Effects\$AEVersion\PluginCache\en_US"
if (-not (Test-Path $cache)) {
    "PluginCache not found at $cache - has AE $AEVersion ever run?"
} else {
    $entries = Get-ChildItem $cache | Where-Object PSChildName -match $Plugin
    if (-not $entries) {
        "NO cache entry for '$Plugin' - AE has not loaded it (check install folder)."
    }
    foreach ($e in $entries) {
        "entry: $($e.PSChildName)"
        $gpu = Get-ChildItem $e.PSPath -ErrorAction SilentlyContinue |
               Where-Object PSChildName -match 'GPUVideoFilter'
        if ($gpu) {
            foreach ($g in $gpu) {
                $p = Get-ItemProperty $g.PSPath
                "  GPUVideoFilter record PRESENT:"
                "    MatchName        = " + $p.'GPUVideoFilter.MatchName'
                "    InterfaceVersion = " + $p.'GPUVideoFilter.InterfaceVersion'
                "    Index            = " + $p.'GPUVideoFilter.Index'
            }
        } else {
            "  NO GPUVideoFilter subkey => module not registered."
            "  => fix registration first (docs/AE_GPU_Effects.md 2.1 / 2.2)."
        }
    }
}

"`n--- 2. PLUGIN LOG (did Initialize/Render actually run?) ---"
if (Test-Path $LogPath) {
    $lines = Get-Content $LogPath
    $gpuvf = $lines | Where-Object { $_ -match 'GPUVF' }
    if ($gpuvf) {
        "GPUVF events found (" + $gpuvf.Count + ") - THE MODULE IS RUNNING:"
        $gpuvf | Select-Object -First 15
    } else {
        "NO 'GPUVF' lines => module registered but never instantiated/rendered."
        "=> check GPU pixel-format declaration (docs/AE_GPU_Effects.md 2.3)."
    }
    "`n  (other log lines, deduped:)"
    $lines | Where-Object { $_ -notmatch 'GPUVF' } |
        Group-Object | Sort-Object Count -Descending |
        Select-Object -First 8 |
        ForEach-Object { "    {0,5}x {1}" -f $_.Count, $_.Name.Trim() }
} else {
    "No log at $LogPath - effect not applied since the last build, or"
    "ER_GPU_LOG is compiled out."
}

"`n--- 3. INSTALL STATE (expect exactly ONE copy, in MediaCore) ---"
$dirs = @(
    "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore",
    "C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins"
)
foreach ($dir in $dirs) {
    $f = Join-Path $dir ($Plugin + ".aex")
    if (Test-Path $f) {
        $i = Get-Item $f
        "PRESENT: " + $f
        "         " + $i.Length + " bytes, " + $i.LastWriteTime
    }
}

"`n--- 4. REMINDER: how to read the A/B ---"
"With a passthrough or deliberately-different GPU kernel, Mercury CUDA and"
"Software Only must look DIFFERENT. Identical output = GPU module not running."
