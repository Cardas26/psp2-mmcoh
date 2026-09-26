# Third-party code

Everything under `loader/lib/` is vendored from the projects below. Each keeps
its own licence. Three carry local changes, listed in the last column.

| Directory | Upstream | Licence | Local changes |
|---|---|---|---|
| `loader/lib/vitagl` | [Rinnegatamante/vitaGL](https://github.com/Rinnegatamante/vitaGL) | LGPLv3 (`COPYING`, `COPYING.LESSER`) | one: its debug log is written through a file sink |
| `loader/lib/so_util` | [Rinnegatamante/so_util](https://github.com/Rinnegatamante/so_util) | MIT (`LICENSE`) | three: critical messages mirrored to the port's log, that mirror compiled out of the default build, and `hook_thumb` no longer corrupting a 2-mod-4 entry's first instruction |
| `loader/lib/falso_jni` | [v-atamanenko/FalsoJNI](https://github.com/v-atamanenko/FalsoJNI) | MIT (`LICENSE`) | three: Direct ByteBuffer support (needed for all audio), logging macros that compile out, and methods resolved by name plus signature |
| `loader/lib/fios` | [v-atamanenko/soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate) | MIT (header of `fios.c`) | none |
| `loader/lib/sha1` | Brad Conte, via soloader-boilerplate | "as is, without any guarantees" (header of `sha1.c`) | none |
| `loader/lib/kubridge` | [bythos14/kubridge](https://github.com/bythos14/kubridge) | header and prebuilt stub | none |
| `loader/lib/libc_bridge` | [v-atamanenko/soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate) | NID table | none |
| `loader/lib/libdeflate` | [ebiggers/libdeflate](https://github.com/ebiggers/libdeflate) 1.22 | MIT (`COPYING`) | none |
| `loader/lib/zlib-ng` | [zlib-ng/zlib-ng](https://github.com/zlib-ng/zlib-ng) 2.2.2 | zlib (`LICENSE.md`) | none |

The loader itself started from
[soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate);
its MIT notice is kept as `loader/LICENSE`.

## vitaGL and the LGPL

vitaGL is statically linked into the VPK. LGPLv3 requires that anyone who
receives the VPK can rebuild it against a modified vitaGL. This repository
meets that by shipping vitaGL's complete source and the port's own, and every
release links the tag it was built from.
