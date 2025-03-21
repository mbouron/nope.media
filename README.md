# nope.media

![tests Linux](https://github.com/mbouron/nope.media/workflows/tests%20Linux/badge.svg)
![tests Mac](https://github.com/mbouron/nope.media/workflows/tests%20Mac/badge.svg)
![tests Windows](https://github.com/mbouron/nope.media/workflows/tests%20Windows/badge.svg)
![build Android 🤖](https://github.com/mbouron/nope.media/workflows/build%20Android%20🤖/badge.svg)
![build iOS 🍏](https://github.com/mbouron/nope.media/workflows/build%20iOS%20🍏/badge.svg)
[![coverity](https://scan.coverity.com/projects/29466/badge.svg)](https://scan.coverity.com/projects/nope-media)

## Introduction

`nope.media` is a video playback library for fetching frames at exact times.

It can handle only one stream at a time, generally video. If audio is
requested, the frame returned will be an instant video frame containing both
amplitudes and frequencies information.

## License

LGPL v2.1, see `LICENSE`.

## Compilation, installation

```sh
meson setup builddir
meson compile -C builddir
meson install -C builddir
```

`meson configure` can be used to list the available options. See the [Meson
documentation][meson-doc] for more information.

[meson-doc]: https://mesonbuild.com/Quick-guide.html#compiling-a-meson-project


## API Usage

For API usage, refers to `nopemd.h`.


## Development

### Running tests

Assuming `nope.media` is built within a `builddir` directory (`meson builddir
&& meson compile -C builddir`), the test can be run with meson using a command
such as `meson test -C builddir -v`.

The testing program itself (`test-prog`) can be found in the build directory,
along a sample player tool named `nope-media` (if its dependencies were met at
build time), which can be used for other manual testing purposes.

### Infrastructure overview

```
              .                             .
              .             API             .
              .                             .
              . . . . . . . . . . . . . . . .
              .                             .   .------------.               .-----------------------------------------------------.
              .                             .   | api.c      |               | control                                             |
              .                             .   +------------+               +-----------------------------------------------------+
           ===.===  nmd_create()      ======.==>|            |===  init  ===>|                                                     |
              .                             .   |            |---  start  -->|                    MANAGE THREADS                   |
           ===.===  nmd_get_*frame()      ==.==>|            |---  seek  --->|              _________/  |  \_______                |
  USER        .                             .   |            |---  stop  --->|             /            |          \               |
           ---.---  nmd_start()      -------.-->|            |===  free  ===>|            /             |           \              |
           ---.---  nmd_seek()       -------.-->|            |               |           v              v            v             |
           ---.---  nmd_seek()       -------.-->|            |               |      .----------.  .----------.  .----------.       |
              .                             .   |            |               |      | demuxer  |  | decoder  |  | filterer |       |
           ===.===  nmd_free()       =======.==>|            |               |      +----------+  +----------+  +----------+       |
              .                             .   |            |               |      | init     |  | init     |  | init     |       |
              .                             .   `------------'               |      +----------+  +----------+  +----------+       |
              .                             .                                |      |          |  |          |  |          |       |
              .                             .                                |      |  RUN     |  |  RUN     |  |  RUN     |       |
              .                             .                                |      |          |  |          |  |          |       |
                                                                             |      |          |  |          |  |          |       |
                                                                             |   O==|==O    O==|==|==O    O==|==|==O    O==|==O    |
                                                                             |      |          |  |          |  |          |       |
                                                                             |      +----------+  +----------+  +----------+       |
                                                                             |      | free     |  | free     |  | free     |       |
                                                                             |      `----------'  `----------'  `----------'       |
                                                                             |                                                     |
                                                                             |                                                     |
                                                                             |   O=====O in control queue                          |
                                                                             |                                                     |
                                                                             |   O=====O out control queue                         |
                                                                             |                                                     |
                                                                             `-----------------------------------------------------'

O====O       message queue
-- xxx -->   async operation
== xxx ==>   sync operation
```
