# Patches to the vendored CHGame files

Every file under `chgame/` is a copy of upstream bateske/CHGame at the commit in [VENDORED.md](VENDORED.md),
except these two, which are modified (Apache-2.0 section 4(b); each carries a "MODIFIED for CHSaveMan" notice at
its top):

- `platform/board/arduino/CHGame/libraries/CHGame/src/chgame/Save.h`
- `platform/board/arduino/CHGame/libraries/CHGame/src/chgame/Save.cpp`

What changed:

- `save::rawPage(i)` and `save::rawWrite(i, buf)`: read and program a whole save page (0xF500 / 0xF600) with a
  read-back check. A save manager needs them; games do not use them.
- Flash ownership: a page that holds another game's record is never erased by `save::write()` (the save goes to
  the other page or fails with `BLOCKED`); `save::status()` and `save::allowOverwrite()`.
- Save backends with slots: `save::Backend`, `save::useBackend()`, `save::slots()`, a `slot` argument on
  `read()`, `write()`, `load()` and `store()` (0 for flash).

Nothing else under `chgame/` differs from upstream.
