PixelSandbox uses standard Godot Engine, godot-cpp and Box2D under the MIT License.
Box2D v3.1.1 is pinned to commit `8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3`;
its license is included as `licenses/box2d.txt` in exports.

The local prototype includes selected material settings and 29 material textures
(including rock, sand, earth, wood, ice and TNT), read from the user's Noita installation at their
request. Source: Noita / Nolla Games, `data/data.wak`, `data/materials.xml` and
`data/materials_gfx/`. These references and assets are separate from the MIT
components below. Inherited source attributes, project tuning and SHA-256 hashes
are recorded in `game/assets/prototype_noita/source-materials.json`; the original
stone-only provenance also remains in `source.json`. These are local prototype
assets; the importer never writes into the Noita installation.

- Box2D: https://github.com/erincatto/box2d/blob/v3.1.1/LICENSE
- Godot Engine: https://godotengine.org/license/
- Godot third-party components: https://github.com/godotengine/godot/blob/4.7.2-stable/COPYRIGHT.txt
- godot-cpp: https://github.com/godotengine/godot-cpp/blob/507ed9d840c01a3c5b2a39af8bb4000bfac30bf5/LICENSE.md

Copyright (c) 2014-present Godot Engine contributors.
Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
