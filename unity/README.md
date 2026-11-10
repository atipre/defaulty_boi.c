# Jonesy dance export

This project uses Unity's humanoid Avatar solver to evaluate the Jonesy model
and Dance Moves clip from `2223-jonesy-2.unitypackage`. The exporter produces
bind geometry and bone transforms for the existing C renderer, rather than
attempting to recreate Unity's muscle solver.

Prepared for Unity **6000.6.8f1**. The original package remains unchanged.
The exporter and preview scene are included here. Model and animation files
are extracted from the package with `make unity-prepare` and ignored by Git.

## Open and export

1. From the repository root, run `make unity-prepare`.
2. In Unity Hub, choose **Projects > Add > Add project from disk** and select
   this `unity` folder. Open it with Unity 6.6 and let the first import finish.
   Open `Assets/DancePreview.unity` to see the saved preview.
3. Choose **Dance > Export Jonesy for Terminal** from Unity's top menu.
4. A successful export writes `../assets/jonesy-unity.dance` and its `.json`
   report, logs `DANCE_EXPORT_OK` to the Console, and opens a preview scene.
5. Press Play to inspect the dance in Unity. In a separate terminal at the
   repository root, run `make run` to inspect the C rendering.

The current default is the verified Unity export.

If no Dance menu appears, let script compilation finish and check the Console
for an error. The exporter has been compiled and run in the installed Unity 6.6
Editor, and its output checked with the native renderer.

## Rebuild from the original download

From the repository root:

```sh
make unity-prepare   # extract source model, textures, materials, clip and metadata
make unity-export    # prepare and run a licensed Unity Editor in batch mode
```

The batch command needs an installed, licensed Unity Editor. Close the project
in the Editor before running it, or use the Hub and menu procedure above.

Only model assets and the Dance Moves clip are copied from the package. Its
VRChat scene, avatar scripts, and other dances are not needed by this project.
Source: [Jonesy #2 by PotatoUnderscoreOX](https://vrcmods.com/item?id=2223).
Fortnite, Jonesy, and Dance Moves originate with Epic Games.

## Export verification

The exporter checks the humanoid Avatar, sample timing, skin weights, and every
sampled skin matrix's rigidity. At four representative poses it compares the
exported skin against Unity's independent `SkinnedMeshRenderer.BakeMesh` result.
It refuses to publish an asset if the maximum difference exceeds 0.002 units
on a character normalized to 1.8 units tall. The `.json` report records the
observed errors and source paths. These checks validate the export against
Unity, not against the original Fortnite gameplay capture.
