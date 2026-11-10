#!/usr/bin/env python3
"""Prepare the supplied Jonesy package and bake its dance with Unity's Animator.

Only asset preparation needs Unity. The generated .dance file plays in native C.
Use --prepare-only to import/run the project manually through Unity Hub instead.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tarfile
import time

ROOT = Path(__file__).resolve().parent.parent
PROJECT = ROOT / 'unity'


def prepare(package, project=PROJECT):
    (project / 'Assets/Editor').mkdir(parents=True, exist_ok=True)
    (project / 'Packages').mkdir(exist_ok=True)
    (project / 'ProjectSettings').mkdir(exist_ok=True)
    manifest = project / 'Packages/manifest.json'
    if not manifest.exists():
        manifest.write_text(json.dumps({'dependencies': {
            'com.unity.modules.animation': '1.0.0',
            'com.unity.modules.director': '1.0.0',
            'com.unity.modules.imageconversion': '1.0.0',
            'com.unity.modules.imgui': '1.0.0',
            'com.unity.modules.jsonserialize': '1.0.0',
        }}, indent=2) + '\n')
    count = 0
    with tarfile.open(package, 'r:gz') as archive:
        members = {m.name: m for m in archive.getmembers() if m.isfile()}
        for name, member in members.items():
            if not name.endswith('/pathname'):
                continue
            path = Path(archive.extractfile(member).read().decode().strip())
            if path.is_absolute() or '..' in path.parts:
                raise ValueError(f'Unsafe package path: {path}')
            model = (
                str(path) == 'Assets/Jonesy/Jonesy.FBX'
                or (str(path).startswith('Assets/Jonesy/Textures/') and path.suffix.lower() == '.tga')
                or (str(path).startswith('Assets/Jonesy/Materials/') and path.name in {
                    'M_Med_Soldier_04.mat', 'M_Med_Soldier_Head.mat', 'M_SML_Survivor_Hair_02.mat'})
            )
            dance = str(path) == 'Assets/Animations/Animation Files/Dance Moves.anim'
            if not (model or dance):
                continue
            guid = name.rsplit('/', 1)[0]
            if guid + '/asset' not in members:
                continue
            target = project / path
            target.parent.mkdir(parents=True, exist_ok=True)
            for suffix, dest in [('/asset', target), ('/asset.meta', Path(str(target) + '.meta'))]:
                source = members.get(guid + suffix)
                if source is not None:
                    data = archive.extractfile(source).read()
                    if not dest.exists() or dest.read_bytes() != data:
                        dest.write_bytes(data)
            count += 1
    exporter = PROJECT / 'Assets/Editor/JonesyExporter.cs'
    target_exporter = project / 'Assets/Editor/JonesyExporter.cs'
    if exporter.resolve() != target_exporter.resolve():
        target_exporter.write_bytes(exporter.read_bytes())
    print(f'Prepared {count} source assets in {project}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unity', type=Path, help='Path to Unity.app/Contents/MacOS/Unity')
    parser.add_argument('--package', type=Path, default=ROOT / '2223-jonesy-2.unitypackage')
    parser.add_argument('--project', type=Path, default=PROJECT, help='Existing Unity project to prepare')
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    project = args.project.resolve()
    prepare(args.package, project)
    if args.prepare_only:
        return
    editors = sorted(Path('/Applications/Unity/Hub/Editor').glob('*/Unity.app/Contents/MacOS/Unity'))
    editor = args.unity or (editors[-1] if editors else None)
    if editor is None or not editor.is_file():
        parser.error('No Unity Editor found. Install it through Unity Hub or pass --unity PATH.')
    version = editor.parents[3].name
    version_file = project / 'ProjectSettings/ProjectVersion.txt'
    if not version_file.exists():
        version_file.write_text(f'm_EditorVersion: {version}\n')
    log = project / 'Logs/dance-export.log'
    log.parent.mkdir(exist_ok=True)
    print(f'Baking with {editor}\nLog: {log}', flush=True)
    started = time.time()
    result = subprocess.run([str(editor), '-batchmode', '-nographics', '-quit',
                             '-projectPath', str(project), '-executeMethod', 'JonesyExporter.Export',
                             '-logFile', str(log)], cwd=ROOT)
    if result.returncode:
        print(log.read_text(errors='replace')[-9000:] if log.exists() else 'Unity produced no log.')
        raise SystemExit(result.returncode)
    output = ROOT / 'assets/jonesy-unity.dance'
    if not output.exists() or output.stat().st_mtime < started:
        raise SystemExit(f'Unity returned without exporting {output}; inspect {log}')
    print(f'Exported {output} ({output.stat().st_size:,} bytes)')


if __name__ == '__main__':
    main()
