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

The desktop application distributes the FFmpeg 8.1.2 shared libraries used by
the in-engine media layer. It does not distribute or launch the `ffmpeg` or
`ffprobe` command-line programs. The libraries are built without GPL or
nonfree components and are licensed under the GNU Lesser General Public
License, version 2.1 or later. Development builds may link a compatible system
FFmpeg 8 installation; release artifacts use the pinned build described below.

The packaged `share/ffmpeg/` directory contains the corresponding unmodified
source archive, license text, and exact build configuration. The reproducible
platform build commands are:

```text
scripts/build-ffmpeg-linux.sh OUTPUT_SDK_DIRECTORY
scripts/build-ffmpeg-windows.sh OUTPUT_SDK_DIRECTORY
npm run stage:ffmpeg:macos
```

Source: https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz
SHA-256: 464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c

### zlib

The pinned FFmpeg libraries use zlib 1.3.1 for PNG compression and
decompression. Windows builds link it statically; Linux and macOS builds use
the platform zlib. The Windows package includes its unmodified source archive
and license under `share/ffmpeg/zlib/`.

Copyright (C) 1995-2022 Jean-loup Gailly and Mark Adler

This software is provided 'as-is', without any express or implied warranty.
Permission is granted to anyone to use this software for any purpose, including
commercial applications, and to alter it and redistribute it freely, subject
to the restrictions in the included zlib license.

Source: https://zlib.net/fossils/zlib-1.3.1.tar.gz
SHA-256: 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23

## Vulkan Loader

CUDA/Vulkan release packages distribute the Khronos Vulkan Loader 1.4.357.0
beside the engine so capability discovery works without a separately installed
Vulkan SDK. GPU vendors still provide the installable client driver used by the
loader. The loader is licensed under the Apache License 2.0; the complete
license is packaged under `share/getnative/licenses/vulkan-loader/`.

Source: https://github.com/KhronosGroup/Vulkan-Loader/tree/vulkan-sdk-1.4.357.0

## xxHash

The engine vendors xxHash 0.8.3 for plan-store checksums and media-index
compatibility hashes.

Copyright (C) 2012-2023 Yann Collet

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Source: https://github.com/Cyan4973/xxHash/releases/tag/v0.8.3
