"""CHSaveMan: what the shared CHGame tools need to know (`cl run`, `cl gif`, `cl test` and `cl check` read this)."""

# One framebuffer row a DMA chunk: RAM back for the card's buffers.
DEFINES = ["GFX_CHUNK_ROWS=1"]

# The board's card code (sdio_dev.cpp) and FatFs on a RAM disk: mounts, record files, power cuts, the scan,
# the scan's frame-time model.
TESTS = {
    "host_fatfs": dict(sources=["tools/tests/host_fatfs.cpp", "src/sd/sdio_dev.cpp", "src/fatfs/ff.cpp"],
                       args="none"),
}

# The simulator checks (each starts its own simulators, one at a time).
SIM_TESTS = [
    "tools/tests/sim_save.py",
    "tools/tests/extra_safety.py",
    "tools/tests/extra_features.py",
    "tools/tests/sim_round3.py",
]
