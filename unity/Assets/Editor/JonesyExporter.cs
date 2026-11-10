using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Animations;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Animations;
using UnityEngine.Playables;
using UnityEngine.SceneManagement;

// Bake Unity's humanoid solver, not a custom reconstruction of its muscle curves.
// The native format stores bind geometry and rigid skin transforms, never images.
public static class JonesyExporter
{
    const string ModelPath = "Assets/Jonesy/Jonesy.FBX";
    const string ClipPath = "Assets/Animations/Animation Files/Dance Moves.anim";
    const int Fps = 30;

    sealed class Part
    {
        public SkinnedMeshRenderer Renderer;
        public Mesh Mesh;
        public Matrix4x4 BindWorld;
        public int BoneStart;
        public float[] Albedo;
    }

    [Serializable] sealed class Report
    {
        public string unity_version, model, animation, source, retargeting, coordinates;
        public int vertices, triangles, bones, samples;
        public float fps, seconds, max_rigidity_error, max_bake_vertex_error;
        public string[] bone_names;
        public string[] material_textures;
        public float[] pose_min, pose_max;
    }

    [MenuItem("Dance/Export Jonesy for Terminal")]
    public static void Export()
    {
        try { Bake(); }
        catch (Exception e)
        {
            Debug.LogException(e);
            if (Application.isBatchMode) EditorApplication.Exit(1);
            else EditorUtility.DisplayDialog("Dance export failed", e.Message, "OK");
            throw;
        }
    }

    static void Bake()
    {
        AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);
        var importer = AssetImporter.GetAtPath(ModelPath) as ModelImporter;
        if (importer == null) throw new Exception("Run tools/export_unity.py --prepare-only first.");
        if (!importer.isReadable || importer.optimizeGameObjects)
        {
            importer.isReadable = true;
            importer.optimizeGameObjects = false;
            importer.SaveAndReimport();
        }
        foreach (string guid in AssetDatabase.FindAssets("t:Texture2D", new[] { "Assets/Jonesy" }))
        {
            var textureImporter = AssetImporter.GetAtPath(AssetDatabase.GUIDToAssetPath(guid)) as TextureImporter;
            if (textureImporter != null && !textureImporter.isReadable)
            {
                textureImporter.isReadable = true;
                textureImporter.SaveAndReimport();
            }
        }
        var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(ModelPath);
        var clip = AssetDatabase.LoadAssetAtPath<AnimationClip>(ClipPath);
        if (prefab == null || clip == null) throw new Exception("Jonesy model or Dance Moves clip did not import.");
        var instance = UnityEngine.Object.Instantiate(prefab);
        instance.name = "Jonesy Export";
        ResolveMaterials(instance);
        var graph = PlayableGraph.Create("Jonesy dance bake");
        try
        {
            var animator = instance.GetComponent<Animator>();
            if (animator == null || animator.avatar == null || !animator.avatar.isValid || !animator.avatar.isHuman)
                throw new Exception("The imported Jonesy humanoid Avatar is missing or invalid.");
            if (!clip.humanMotion) throw new Exception("Dance Moves did not import as a humanoid clip.");
            animator.cullingMode = AnimatorCullingMode.AlwaysAnimate;
            animator.applyRootMotion = false;

            var parts = new List<Part>();
            var names = new List<string>();
            var bounds = new Bounds();
            bool first = true;
            foreach (var renderer in instance.GetComponentsInChildren<SkinnedMeshRenderer>())
            {
                var mesh = renderer.sharedMesh;
                if (mesh == null || mesh.vertexCount == 0) continue;
                if (mesh.boneWeights.Length != mesh.vertexCount || mesh.bindposes.Length != renderer.bones.Length)
                    throw new Exception("Unexpected skin layout on " + renderer.name);
                var part = new Part { Renderer = renderer, Mesh = mesh,
                    BindWorld = renderer.localToWorldMatrix, BoneStart = names.Count };
                for (int b = 0; b < renderer.bones.Length; ++b)
                    names.Add(renderer.name + "/" + renderer.bones[b].name);
                foreach (var vertex in mesh.vertices)
                {
                    Vector3 point = part.BindWorld.MultiplyPoint3x4(vertex);
                    if (first) { bounds = new Bounds(point, Vector3.zero); first = false; }
                    else bounds.Encapsulate(point);
                }
                part.Albedo = SampleAlbedo(renderer, mesh);
                parts.Add(part);
            }
            if (first || names.Count > 256) throw new Exception("Empty model or more than 256 skin joints.");
            float scale = 1.8f / bounds.size.y;
            Vector3 origin = new Vector3(bounds.center.x, bounds.min.y, bounds.center.z);
            // Unity's front view has screen-right = -X. Match that view in C,
            // which looks toward -Z with screen-right = +X.
            Matrix4x4 canonical = Matrix4x4.Scale(new Vector3(-scale, scale, scale)) * Matrix4x4.Translate(-origin);
            Matrix4x4 undoCanonical = canonical.inverse;
            int frames = Mathf.RoundToInt(clip.length * Fps) + 1;
            float duration = (frames - 1f) / Fps;
            var vertices = new List<byte>();
            var indices = new List<uint>();
            using (var stream = new MemoryStream())
            using (var writer = new BinaryWriter(stream))
            {
                uint vertexStart = 0;
                foreach (var part in parts)
                {
                    var mesh = part.Mesh;
                    var positions = mesh.vertices;
                    var normals = mesh.normals;
                    var weights = mesh.boneWeights;
                    var toCanonical = canonical * part.BindWorld;
                    var normalMatrix = toCanonical.inverse.transpose;
                    for (int v = 0; v < mesh.vertexCount; ++v)
                    {
                        Write(writer, toCanonical.MultiplyPoint3x4(positions[v]));
                        Write(writer, normalMatrix.MultiplyVector(normals[v]).normalized);
                        writer.Write(part.Albedo[v]);
                        BoneWeight w = weights[v];
                        writer.Write((ushort)(part.BoneStart + w.boneIndex0));
                        writer.Write((ushort)(part.BoneStart + w.boneIndex1));
                        writer.Write((ushort)(part.BoneStart + w.boneIndex2));
                        writer.Write((ushort)(part.BoneStart + w.boneIndex3));
                        float total = w.weight0 + w.weight1 + w.weight2 + w.weight3;
                        if (total < .999f || total > 1.001f) throw new Exception("Skin weights do not sum to one.");
                        writer.Write(w.weight0 / total); writer.Write(w.weight1 / total);
                        writer.Write(w.weight2 / total); writer.Write(w.weight3 / total);
                    }
                    foreach (int index in mesh.triangles) indices.Add(vertexStart + (uint)index);
                    vertexStart += (uint)mesh.vertexCount;
                }
                vertices.AddRange(stream.ToArray());
            }
            var report = new Report {
                unity_version = Application.unityVersion, model = ModelPath, animation = ClipPath,
                source = "2223-jonesy-2.unitypackage; PotatoUnderscoreOX, https://vrcmods.com/item?id=2223; Fortnite / Epic Games",
                retargeting = "Unity humanoid Avatar evaluated by AnimationClipPlayable; exported bone skin transforms",
                coordinates = "Normalized to 1.8 units high; Unity front view, X reflected for the C camera",
                vertices = vertices.Count / 52, triangles = indices.Count / 3, bones = names.Count,
                samples = frames, fps = Fps, seconds = duration, bone_names = names.ToArray()
            };
            report.material_textures = parts.SelectMany(part => part.Renderer.sharedMaterials)
                .Select(material => material.name + ": " + AssetDatabase.GetAssetPath(material.mainTexture))
                .Distinct().ToArray();
            graph.SetTimeUpdateMode(DirectorUpdateMode.Manual);
            var playable = AnimationClipPlayable.Create(graph, clip);
            playable.SetApplyFootIK(false);
            playable.SetApplyPlayableIK(false);
            var output = AnimationPlayableOutput.Create(graph, "Jonesy", animator);
            output.SetSourcePlayable(playable);
            graph.Play();
            var poseBounds = new Bounds();
            bool firstPose = true;
            var previous = new Quaternion[names.Count];
            string directory = Path.Combine(WorkspaceRoot(), "assets");
            Directory.CreateDirectory(directory);
            string destination = Path.Combine(directory, "jonesy-unity.dance");
            string temporary = destination + ".tmp";
            using (var writer = new BinaryWriter(File.Create(temporary)))
            {
                writer.Write(new byte[] { 68, 65, 78, 67, 69, 51, 68, 0 });
                writer.Write((uint)1); writer.Write((uint)report.vertices); writer.Write((uint)indices.Count);
                writer.Write((uint)names.Count); writer.Write((uint)frames);
                writer.Write((float)Fps); writer.Write(duration);
                writer.Write(vertices.ToArray());
                foreach (uint index in indices) writer.Write(index);
                for (int frame = 0; frame < frames; ++frame)
                {
                    double time = frame / (double)Fps;
                    playable.SetTime(time);
                    graph.Evaluate(0);
                    foreach (var part in parts)
                    {
                        var skin = new Matrix4x4[part.Renderer.bones.Length];
                        var bindposes = part.Mesh.bindposes;
                        for (int b = 0; b < skin.Length; ++b)
                        {
                            skin[b] = canonical * part.Renderer.bones[b].localToWorldMatrix * bindposes[b]
                                * part.BindWorld.inverse * undoCanonical;
                            Quaternion q = skin[b].rotation.normalized;
                            Vector3 translation = skin[b].GetColumn(3);
                            var rigid = Matrix4x4.TRS(translation, q, Vector3.one);
                            for (int row = 0; row < 3; ++row)
                                for (int col = 0; col < 3; ++col)
                                    report.max_rigidity_error = Mathf.Max(report.max_rigidity_error,
                                        Mathf.Abs(skin[b][row, col] - rigid[row, col]));
                            if (report.max_rigidity_error > .002f)
                                throw new Exception("The skin contains scaling; cannot store it as rigid quaternion keys: "
                                    + report.max_rigidity_error + " at " + names[part.BoneStart + b]);
                            int joint = part.BoneStart + b;
                            if (frame > 0 && Quaternion.Dot(previous[joint], q) < 0)
                                q = new Quaternion(-q.x, -q.y, -q.z, -q.w);
                            previous[joint] = q;
                            Write(writer, q); Write(writer, translation);
                            skin[b] = rigid;
                        }
                        // An independent CPU skin in Unity verifies the exported transform
                        // convention against SkinnedMeshRenderer at representative poses.
                        if (frame == 0 || frame == 64 || frame == 150 || frame == frames - 1)
                        {
                            var baked = new Mesh();
                            part.Renderer.BakeMesh(baked);
                            var actual = baked.vertices;
                            var bind = part.Mesh.vertices;
                            var weights = part.Mesh.boneWeights;
                            var bindToCanonical = canonical * part.BindWorld;
                            for (int v = 0; v < actual.Length; ++v)
                            {
                                Vector3 p = bindToCanonical.MultiplyPoint3x4(bind[v]);
                                BoneWeight w = weights[v];
                                Vector3 predicted = skin[w.boneIndex0].MultiplyPoint3x4(p) * w.weight0
                                    + skin[w.boneIndex1].MultiplyPoint3x4(p) * w.weight1
                                    + skin[w.boneIndex2].MultiplyPoint3x4(p) * w.weight2
                                    + skin[w.boneIndex3].MultiplyPoint3x4(p) * w.weight3;
                                Vector3 observed = (canonical * part.Renderer.localToWorldMatrix).MultiplyPoint3x4(actual[v]);
                                report.max_bake_vertex_error = Mathf.Max(report.max_bake_vertex_error,
                                    Vector3.Distance(predicted, observed));
                                if (firstPose) { poseBounds = new Bounds(observed, Vector3.zero); firstPose = false; }
                                else poseBounds.Encapsulate(observed);
                            }
                            UnityEngine.Object.DestroyImmediate(baked);
                        }
                    }
                }
            }
            if (report.max_bake_vertex_error > .002f)
                throw new Exception("Exported skin disagrees with Unity BakeMesh: " + report.max_bake_vertex_error);
            report.pose_min = new[] { poseBounds.min.x, poseBounds.min.y, poseBounds.min.z };
            report.pose_max = new[] { poseBounds.max.x, poseBounds.max.y, poseBounds.max.z };
            File.Copy(temporary, destination, true);
            File.Delete(temporary);
            File.WriteAllText(Path.ChangeExtension(destination, ".json"), JsonUtility.ToJson(report, true) + "\n");
            Debug.Log("DANCE_EXPORT_OK " + destination + "\n" + JsonUtility.ToJson(report, true));
        }
        finally
        {
            if (graph.IsValid()) graph.Destroy();
            UnityEngine.Object.DestroyImmediate(instance);
        }
        CreatePreview(prefab, clip);
    }

    static float[] SampleAlbedo(SkinnedMeshRenderer renderer, Mesh mesh)
    {
        var result = Enumerable.Repeat(.5f, mesh.vertexCount).ToArray();
        var uv = mesh.uv;
        var materials = renderer.sharedMaterials;
        for (int submesh = 0; submesh < mesh.subMeshCount; ++submesh)
        {
            var material = submesh < materials.Length ? materials[submesh] : null;
            var texture = material != null ? material.mainTexture as Texture2D : null;
            if (texture == null)
                throw new Exception("Missing diffuse texture on " + renderer.name + "/" + (material != null ? material.name : "no material"));
            var color = material != null && material.HasProperty("_Color") ? material.color : Color.white;
            foreach (int v in mesh.GetIndices(submesh))
            {
                Color sample = color;
                if (texture != null && uv.Length == mesh.vertexCount)
                {
                    Vector2 texcoord = Vector2.Scale(uv[v], material.mainTextureScale) + material.mainTextureOffset;
                    sample *= texture.GetPixelBilinear(texcoord.x - Mathf.Floor(texcoord.x), texcoord.y - Mathf.Floor(texcoord.y));
                }
                result[v] = Mathf.Clamp01(.2126f * sample.r + .7152f * sample.g + .0722f * sample.b);
            }
        }
        return result;
    }

    static void CreatePreview(GameObject prefab, AnimationClip clip)
    {
        var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
        var character = UnityEngine.Object.Instantiate(prefab);
        character.name = "Jonesy - Dance Moves";
        ResolveMaterials(character);
        // A Hub-created project can use URP. Adapt preview materials to its
        // installed shader while keeping the package's source materials intact.
        if (UnityEngine.Rendering.GraphicsSettings.currentRenderPipeline != null)
        {
            var shader = Shader.Find("Universal Render Pipeline/Lit");
            if (shader != null)
                foreach (var renderer in character.GetComponentsInChildren<SkinnedMeshRenderer>())
                {
                    var materials = renderer.sharedMaterials;
                    for (int i = 0; i < materials.Length; ++i)
                    {
                        var source = materials[i];
                        if (source == null) continue;
                        string materialPath = "Assets/DancePreview-" + source.name + ".mat";
                        var material = AssetDatabase.LoadAssetAtPath<Material>(materialPath);
                        if (material == null)
                        {
                            material = new Material(shader);
                            AssetDatabase.CreateAsset(material, materialPath);
                        }
                        material.SetTexture("_BaseMap", source.mainTexture);
                        material.SetColor("_BaseColor", source.HasProperty("_Color") ? source.color : Color.white);
                        EditorUtility.SetDirty(material);
                        materials[i] = material;
                    }
                    renderer.sharedMaterials = materials;
                }
        }
        const string controllerPath = "Assets/DancePreview.controller";
        var controller = AssetDatabase.LoadAssetAtPath<AnimatorController>(controllerPath);
        if (controller == null)
        {
            controller = AnimatorController.CreateAnimatorControllerAtPath(controllerPath);
            var state = controller.layers[0].stateMachine.AddState("Dance Moves");
            state.motion = clip;
            controller.layers[0].stateMachine.defaultState = state;
        }
        character.GetComponent<Animator>().runtimeAnimatorController = controller;
        character.GetComponent<Animator>().applyRootMotion = false;
        var cameraObject = new GameObject("Main Camera", typeof(Camera));
        var camera = cameraObject.GetComponent<Camera>();
        cameraObject.tag = "MainCamera";
        camera.transform.position = new Vector3(0, 1, 4);
        camera.transform.LookAt(new Vector3(0, .9f, 0));
        camera.clearFlags = CameraClearFlags.SolidColor;
        camera.backgroundColor = Color.black;
        camera.fieldOfView = 32;
        var lightObject = new GameObject("Key Light", typeof(Light));
        var light = lightObject.GetComponent<Light>();
        light.type = LightType.Directional;
        light.intensity = 1.3f;
        light.transform.rotation = Quaternion.Euler(35, 150, 0);
        RenderSettings.ambientLight = new Color(.35f, .35f, .35f);
        EditorSceneManager.SaveScene(scene, "Assets/DancePreview.unity");
        AssetDatabase.SaveAssets();
    }

    static string WorkspaceRoot()
    {
        var directory = new DirectoryInfo(Application.dataPath);
        while (directory != null)
        {
            if (File.Exists(Path.Combine(directory.FullName, "defaulty_boi.c"))
                && File.Exists(Path.Combine(directory.FullName, "2223-jonesy-2.unitypackage")))
                return directory.FullName;
            directory = directory.Parent;
        }
        throw new Exception("Cannot find the default_dance workspace above this Unity project.");
    }

    static void ResolveMaterials(GameObject character)
    {
        foreach (var renderer in character.GetComponentsInChildren<SkinnedMeshRenderer>())
        {
            var materials = renderer.sharedMaterials;
            for (int i = 0; i < materials.Length; ++i)
            {
                var source = materials[i];
                if (source == null) continue;
                var supplied = AssetDatabase.LoadAssetAtPath<Material>("Assets/Jonesy/Materials/" + source.name + ".mat");
                if (supplied != null) materials[i] = supplied;
            }
            renderer.sharedMaterials = materials;
        }
    }

    static void Write(BinaryWriter writer, Vector3 value)
    { writer.Write(value.x); writer.Write(value.y); writer.Write(value.z); }
    static void Write(BinaryWriter writer, Quaternion value)
    { writer.Write(value.x); writer.Write(value.y); writer.Write(value.z); writer.Write(value.w); }
}
