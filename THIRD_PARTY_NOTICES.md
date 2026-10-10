# Third-party notices and component inventory

Project-owned source uses the [MIT License](LICENSE). That grant does not cover The Witcher 3, Windows, NVIDIA libraries or other mods. No game files, NVIDIA binaries, debugger installation or redistributable installer are bundled by this project.

The source tree vendors toml++ 3.4.0, compiled into the utility. Its source/tag/commit and header/license SHA256 are recorded in [dependency provenance](third_party/dependencies.toml). The full upstream license follows; preserve it in source distributions and future player notices. Windows SDK headers/import libraries are build prerequisites, and the game-owned interposer is inspected at runtime rather than distributed.

Player packaging is pending. Its actual component inventory must be checked against the packager; test fixture executables and debug symbols must not silently become player components.

## toml++ 3.4.0

MIT License

Copyright (c) Mark Gillard <mark.gillard@outlook.com.au>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
