"""ESP32-S31 FLAC prefetch ring tests: source contracts + executable host model."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "common/audio/aster_audio_prefetch.c").read_text(encoding="utf-8")
DECODER = (ROOT / "common/audio/aster_audio_decoder.c").read_text(encoding="utf-8")
CMAKE = (ROOT / "systems/espidf/components/aster_audio/CMakeLists.txt").read_text(encoding="utf-8")


class S31AudioPrefetch(unittest.TestCase):
    def test_s31_only_flac_and_safe_lifecycle(self):
        self.assertIn("if(CONFIG_IDF_TARGET_ESP32S31)", CMAKE)
        self.assertIn("ASTER_S31_AUDIO_PREFETCH=1", CMAKE)
        self.assertIn("aster_audio_prefetch.c", CMAKE)
        self.assertIn("aster_audio_prefetch_read(d->prefetch", DECODER)
        self.assertIn("aster_audio_prefetch_start(d->prefetch_path", DECODER)
        self.assertIn("aster_audio_prefetch_stop(d->prefetch);", DECODER)
        self.assertIn("if (d->flac &&", DECODER)
        self.assertIn("AUDIO_PREFETCH_RING_BYTES = 128u * 1024u", SOURCE)
        self.assertIn("AUDIO_PREFETCH_READ_BYTES = 8u * 1024u", SOURCE)
        self.assertIn("aster_os_task_join(&p->worker, ASTER_OS_WAIT_FOREVER);", SOURCE)
        self.assertIn("aster_audio_prefetch_window(", SOURCE)
        self.assertIn("if (cancel && cancel(ctx))", SOURCE)
        self.assertNotIn("vTaskDelete(", SOURCE)

    def test_real_ring_wraparound_and_seek_reopen(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if not cc:
            cross = shutil.which("riscv32-esp-elf-gcc")
            if not cross:
                self.skipTest("No host or RISC-V C compiler")
            subprocess.run([
                cross, "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "common/audio"),
                "-I", str(ROOT / "common/port"),
                "-I", str(ROOT / "contract/include"),
                "-fsyntax-only", str(ROOT / "common/audio/aster_audio_prefetch.c")
            ], check=True, capture_output=True, text=True)
            return
        with tempfile.TemporaryDirectory() as folder:
            dest = Path(folder)
            binary = dest / "prefetch_test"
            subprocess.run([
                cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-pthread",
                "-I", str(ROOT / "common/audio"),
                "-I", str(ROOT / "common/port"),
                "-I", str(ROOT / "contract/include"),
                str(ROOT / "tests/s31_audio_prefetch_host.c"),
                str(ROOT / "common/audio/aster_audio_prefetch.c"),
                "-o", str(binary)
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(binary), str(dest / "track.flac")],
                           check=True, capture_output=True, text=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
