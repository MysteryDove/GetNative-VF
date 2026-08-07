# Third-Party Notices

The standalone core independently implements behavior and mathematics studied
from the pinned upstream projects under `upstream/`. Product runtime binaries
do not link those upstream test references. The optional conformance test links
selected standalone sources from descale and zimg.

## descale

Copyright (c) 2017-2022 Frechdachs <frechdachs@rekt.cc>

The MIT License (MIT)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

Source: https://github.com/Irrational-Encoding-Wizardry/descale

## zimg

DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
Version 2, December 2004

Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>

Everyone is permitted to copy and distribute verbatim or modified copies of
this license document, and changing it is allowed as long as the name is
changed.

DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION

0. You just DO WHAT THE FUCK YOU WANT TO.

Source: https://github.com/sekrit-twc/zimg

## FFmpeg

The desktop application may distribute `ffmpeg` and `ffprobe` from FFmpeg
8.1.2 as separate executables. They are built without GPL or nonfree
components and are licensed under the GNU Lesser General Public License,
version 2.1 or later.

The packaged `share/ffmpeg/` directory contains the corresponding unmodified
source archive, license text, and exact build configuration. The reproducible
staging command is:

```text
npm run stage:ffmpeg:macos
```

Source: https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz
SHA-256: 464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c
