# Asset provenance

## Current Unity export

`jonesy-unity.dance` is the C renderer's default asset. It was exported with
Unity 6000.6.8f1 from `2223-jonesy-2.unitypackage`:

- Package: [Jonesy #2 by PotatoUnderscoreOX](https://vrcmods.com/item?id=2223).
- Model: `Assets/Jonesy/Jonesy.FBX`, using its supplied humanoid Avatar.
- Animation: `Assets/Animations/Animation Files/Dance Moves.anim`.
- Geometry: 12,038 vertices, 19,239 triangles, 116 skin joints.
- Motion: 6.8 seconds, 205 endpoint-inclusive samples, 30 Hz.
- Textures: supplied head color, hair diffuse, and body color TGA images.

`unity/Assets/Editor/JonesyExporter.cs` evaluates the humanoid clip using Unity's
`AnimationClipPlayable`. It exports the resulting skin transforms, rather than
approximating Unity's muscle solver. Skin vertices are normalized to 1.8 units
high, and X is reflected consistently for geometry and transforms to match
Unity's front camera with the C renderer's projection.

The exporter resolves the supplied materials by name and rejects missing diffuse
textures. Texture UVs repeat and are sampled into vertex brightness.

The sidecar `jonesy-unity.json` records provenance, dimensions, joint names,
texture bindings, and verification results. At four representative frames,
the skin agrees with Unity's independent `SkinnedMeshRenderer.BakeMesh` within
0.0000012 units. Every sampled matrix is rigid within 0.0000014. These checks
establish agreement with Unity playback rather than the original Fortnite
capture. Fortnite, Jonesy, and Dance Moves originate
with Epic Games. The source package is preserved unchanged.

## Runtime format

`jonesy-unity.dance` stores little-endian data, not rendered frames:

1. 8-byte `DANCE3D` magic with a terminating zero.
2. Five uint32 values: version (1), vertices, indices, bones, samples.
3. Two float32 values: sample rate and clip duration.
4. Vertices: position (3 floats), normal (3), brightness (1), four uint16
   joint indices, and four float weights (52 bytes per vertex).
5. Triangle indices as uint32.
6. Motion keys in sample/bone order: quaternion XYZW and translation XYZ.
   These are transforms from the model's bind geometry to each posed joint.

The matching `.json` records dimensions, joint names, provenance, and animation method.
The C player validates the file, interpolates transforms, skins the mesh, and
rasterizes at the current terminal resolution. The asset is about 1.5 MB.
