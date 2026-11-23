"""S31 native music startup regression checks (not hardware certification)."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PORT = (ROOT / "systems/espidf/adapters/cs43131_i2s_pdm/port.c").read_text(encoding="utf-8")
FADE = ROOT / "common/audio/aster_audio_declick.h"


class S31NativeAudioStartup(unittest.TestCase):
    def test_only_native_output_has_preroll_and_declick(self):
        # native_register() is forward-declared near the top of port.c.
        # Search for its definition *after* native_write(), not that prototype.
        start = PORT.index("static int native_write(")
        stop = PORT.index("static int native_register(", start)
        native = PORT[start:stop]
        self.assertIn("aster_audio_fade_in_s32_stereo(converted, frames, native_frames", native)
        self.assertIn("native_preroll_frames += (uint32_t)(wrote / 8);", native)
        self.assertIn("native_preroll_frames >= NATIVE_PREROLL_TARGET_FRAMES", native)
        self.assertIn("wrote < bytes", native)
        self.assertIn("Preloaded bytes are accepted even before clocks start.", native)
        self.assertIn("if (!r && !tx_on && native_preroll_frames)", native)
        self.assertIn("native_preroll_frames = 0;", PORT)
        self.assertIn("channel.dma_desc_num = 8;", PORT)
        self.assertIn("channel.dma_frame_num = 128;", PORT)
        self.assertIn("codec_volume_locked((uint8_t)aster_plat_audio_get_volume());", PORT)
        self.assertNotIn("aster_audio_fade_in_s32_stereo", PORT[:PORT.index("static int native_write(")])

    def test_pcm_fade_math_compiles_and_runs_where_supported(self):
        host_compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        cross_compiler = None if host_compiler else shutil.which("riscv32-esp-elf-gcc")
        if not host_compiler and not cross_compiler:
            self.skipTest("No host or cross C compiler: source-contract check still runs")
        c = r'''
#include <stdint.h>
#include <limits.h>
#include "aster_audio_declick.h"
int main(void) {
    int32_t a[24];
    for (int i = 0; i < 12; ++i) { a[i*2] = INT32_MAX; a[i*2+1] = INT32_MIN; }
    aster_audio_fade_in_s32_stereo(a, 12, 0, 8);
    if (a[0] || a[1]) return 1;
    if (a[2] != (int32_t)(((int64_t)INT32_MAX)/8)) return 2;
    if (a[3] != (int32_t)(((int64_t)INT32_MIN)/8)) return 3;
    if (a[14] != (int32_t)(((int64_t)INT32_MAX * 7)/8)) return 4;
    if (a[16] != INT32_MAX || a[17] != INT32_MIN) return 5;
    int32_t b[8] = {INT32_MAX, INT32_MIN, INT32_MAX, INT32_MIN,
                    INT32_MAX, INT32_MIN, INT32_MAX, INT32_MIN};
    aster_audio_fade_in_s32_stereo(b, 4, 6, 8);
    if (b[0] != (int32_t)(((int64_t)INT32_MAX * 6)/8)) return 6;
    if (b[2] != (int32_t)(((int64_t)INT32_MAX * 7)/8)) return 7;
    if (b[4] != INT32_MAX || b[6] != INT32_MAX) return 8;
    aster_audio_fade_in_s32_stereo(b, 4, 100, 8);
    if (b[4] != INT32_MAX || b[6] != INT32_MAX) return 9;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            source = Path(d) / "declick.c"
            binary = Path(d) / "declick.exe"
            source.write_text(c, encoding="utf-8")
            common_flags = ["-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(FADE.parent)]
            if host_compiler:
                # Native compiler: run the actual arithmetic assertions.
                subprocess.run([host_compiler, *common_flags, str(source), "-o", str(binary)],
                               check=True, capture_output=True, text=True)
                subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            else:
                # The ESP RISC-V executable cannot run on Windows. Syntax-check
                # the same C header with the configured firmware compiler.
                subprocess.run([cross_compiler, *common_flags, "-fsyntax-only", str(source)],
                               check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
