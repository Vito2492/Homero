"""Minimal headless libretro frontend for research: runs a SNES ROM with the
snes9x core, feeds scripted input, grabs frames and memory (WRAM, VRAM)."""
import ctypes as C, os, sys
from PIL import Image

CORE = os.environ.get("CORE", "./snes9x_libretro.so")

# joypad ids
B, Y, SELECT, START, UP, DOWN, LEFT, RIGHT, A, X, L, R = range(12)

class GameInfo(C.Structure):
    _fields_ = [("path", C.c_char_p), ("data", C.c_void_p), ("size", C.c_size_t), ("meta", C.c_char_p)]

ENV = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
VID = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
AUD = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
AUDB = C.CFUNCTYPE(C.c_size_t, C.c_void_p, C.c_size_t)
POLL = C.CFUNCTYPE(None)
STATE = C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)


class Snes:
    def __init__(self, rom):
        self.lib = C.CDLL(CORE)
        self.held = set()
        self.frame = None
        self.fmt = 0
        self.opts = {}
        self.dirty = False
        self._cbs = [ENV(self._env), VID(self._vid), AUD(lambda l, r: None),
                     AUDB(lambda d, n: n), POLL(lambda: None), STATE(self._state)]
        L = self.lib
        L.retro_set_environment(self._cbs[0])
        L.retro_init()
        L.retro_set_video_refresh(self._cbs[1])
        L.retro_set_audio_sample(self._cbs[2])
        L.retro_set_audio_sample_batch(self._cbs[3])
        L.retro_set_input_poll(self._cbs[4])
        L.retro_set_input_state(self._cbs[5])
        self.rom = open(rom, "rb").read()
        self._buf = C.create_string_buffer(self.rom, len(self.rom))
        gi = GameInfo(rom.encode(), C.cast(self._buf, C.c_void_p), len(self.rom), None)
        assert L.retro_load_game(C.byref(gi)), "load failed"
        L.retro_get_memory_data.restype = C.c_void_p
        L.retro_get_memory_size.restype = C.c_size_t
        L.retro_serialize_size.restype = C.c_size_t

    def _env(self, cmd, data):
        cmd &= 0xffff
        if cmd == 10:                        # SET_PIXEL_FORMAT
            self.fmt = C.cast(data, C.POINTER(C.c_int))[0]
            return True
        if cmd in (9, 31):                   # SYSTEM_DIRECTORY, SAVE_DIRECTORY
            C.cast(data, C.POINTER(C.c_char_p))[0] = b"/tmp"
            return True
        if cmd == 15:                        # GET_VARIABLE
            key = C.cast(data, C.POINTER(C.c_char_p))[0]
            val = self.opts.get(key.decode())
            if val is None:
                return False
            self._keep = C.c_char_p(val.encode())
            C.cast(data, C.POINTER(C.c_void_p))[1] = C.cast(self._keep, C.c_void_p)
            return True
        if cmd == 17:                        # GET_VARIABLE_UPDATE
            C.cast(data, C.POINTER(C.c_bool))[0] = self.dirty
            self.dirty = False
            return True
        if cmd == 3:                         # GET_CAN_DUPE
            C.cast(data, C.POINTER(C.c_bool))[0] = True
            return True
        return False

    def _vid(self, data, w, h, pitch):
        if not data:
            return
        raw = C.string_at(data, pitch * h)
        if self.fmt == 2:                    # RGB565
            img = Image.frombuffer("RGB", (w, h), raw, "raw", "BGR;16", pitch, 1)
        else:                                # XRGB8888
            img = Image.frombuffer("RGBX", (w, h), raw, "raw", "BGRX", pitch, 1).convert("RGB")
        self.frame = img.copy()

    def _state(self, port, device, index, id):
        return 1 if port == 0 and device == 1 and id in self.held else 0

    def run(self, n=1, hold=()):
        self.held = set(hold)
        for _ in range(n):
            self.lib.retro_run()
        self.held = set()

    def tap(self, btn, wait=20):
        self.run(4, (btn,))
        self.run(wait)

    def mem(self, kind):                     # 2 = WRAM, 3 = VRAM
        p = self.lib.retro_get_memory_data(kind)
        n = self.lib.retro_get_memory_size(kind)
        return bytearray(C.string_at(p, n)) if p else b""

    def poke(self, addr, val):               # WRAM offset (7E:xxxx -> xxxx)
        p = self.lib.retro_get_memory_data(2)
        C.cast(p, C.POINTER(C.c_uint8))[addr] = val

    def save(self):
        n = self.lib.retro_serialize_size()
        b = C.create_string_buffer(n)
        assert self.lib.retro_serialize(b, n)
        return b.raw

    def load(self, s):
        b = C.create_string_buffer(s, len(s))
        assert self.lib.retro_unserialize(b, len(s))

    def layers(self, on):                    # on: set of 1..5 (BG1-4, sprites)
        for i in range(1, 6):
            self.opts[f"snes9x_layer_{i}"] = "enabled" if i in on else "disabled"
        self.dirty = True

    def shot(self, path, scale=2):
        self.frame.resize((self.frame.width * scale, self.frame.height * scale), Image.NEAREST).save(path)
