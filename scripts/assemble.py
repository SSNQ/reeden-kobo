#!/usr/bin/env python3
"""Assemble immutable official core + private runtime; never use symlinks on FAT32.

Developer note: end users should use the repository-root install.py instead; this
script is the developer-oriented assembler that expects fetch_deps.py and
build_font_fallback.py outputs under build/ and deps/ next to the repository.
"""
import hashlib,json,os,pathlib,shutil,sys
ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from install import validate_official_core, create_resource_recovery
CORE=pathlib.Path(os.environ['REEDEN_OFFICIAL_CORE'])
matched_version=validate_official_core(CORE)
P=ROOT/'package'
def copy(src,dst):
    dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src.resolve(),dst)
for d in ['runtime','fonts','certs','checks','licenses','docs','state','logs','userdata','storage/books','storage/documents','storage/screenshots','cache']:(P/d).mkdir(parents=True,exist_ok=True)
for src in (CORE/'lib').glob('*.so*'):copy(src,P/'core/lib'/src.name)
shutil.copytree(CORE/'data',P/'core/data',dirs_exist_ok=True)
# Keep the complete official core byte-for-byte. Only the separate UI asset
# overlay uses the combined CJK/Math fallback; Emoji remains the original.
fallback=ROOT/'build/KoboCJKMath-Regular.ttf'
assert fallback.is_file(),'Build the CJK/Math fallback first'
shutil.copytree(CORE/'data/flutter_assets',P/'ui-assets',dirs_exist_ok=True)
copy(fallback,P/'ui-assets/assets/fonts/KoboCJKMath-Regular.ttf')
manifest=P/'ui-assets/FontManifest.json'
families=json.loads(manifest.read_text())
for family in families:
    if family['family']=='NotoSansMath':family['fonts']=[{'asset':'assets/fonts/KoboCJKMath-Regular.ttf'}]
manifest.write_text(json.dumps(families,separators=(',',':'))+'\n')
for src in (CORE/'lib/glibc').iterdir():
    if src.is_file():copy(src,P/'runtime'/src.name)
shutil.copytree(CORE/'licenses',P/'licenses/official',dirs_exist_ok=True)
copy(CORE/'THIRD_PARTY_NOTICES.txt',P/'licenses/official/THIRD_PARTY_NOTICES.txt')
base=ROOT/'build/debian-runtime'
for tree in [base/'lib/arm-linux-gnueabihf',base/'usr/lib/arm-linux-gnueabihf']:
    for src in tree.glob('*.so*'):copy(src,P/'lib'/src.name)
for stem in ['gio','glib','gobject','gmodule','gthread']:
    copy(P/f'lib/lib{stem}-2.0.so.0',P/f'lib/lib{stem}-2.0.so')
for src in (base/'usr/share/doc').glob('*/copyright'):copy(src,P/'licenses/debian'/f'{src.parent.name}-copyright')
certificates=sorted((base/'usr/share/ca-certificates/mozilla').glob('*.crt'))
assert len(certificates)>100,'CA package missing'
(P/'certs/ca-certificates.crt').write_bytes(b'\n'.join(x.read_bytes() for x in certificates))
for name in ['run.sh','session.sh','recover.sh','guardian.sh','native-session.sh','power.sh','wifi.sh','precheck.sh','reeden-ssh.sh','uninstall.sh','install.sh','verify.sh','integrity.sh']:
    copy(ROOT/'scripts'/name,P/name);(P/name).chmod(0o755)
for name in ['lipc-get-prop','lipc-set-prop']:
    copy(ROOT/'scripts'/name,P/'bin'/name);(P/'bin'/name).chmod(0o755)
for name in ['codec-test-armhf','input-test-armhf','compat-test-armhf','runtime-test-armhf','refresh-policy-test-armhf','power-worker-test-armhf','display-transaction-test-armhf']:
    copy(ROOT/'build'/name,P/'checks'/name);(P/'checks'/name).chmod(0o755)
copy(ROOT/'LICENSE',P/'licenses/adapter-MIT-LICENSE')
copy(ROOT.parent/'deps/NotoSansCJKsc-Regular.otf',P/'fonts/NotoSansCJKsc-Regular.otf')
for src in (ROOT/'vendor').glob('*LICENSE*'):copy(src,P/'licenses'/src.name)
copy(ROOT/'vendor/debian-runtime-manifest.json',P/'licenses/debian-runtime-manifest.json')
for src in (ROOT/'docs').rglob('*'):
    if src.is_file():copy(src,P/'docs'/src.relative_to(ROOT/'docs'))
for src in (P/'runtime').iterdir():src.chmod(0o755)
for src in (P/'bin').iterdir():src.chmod(0o755)
copy(ROOT/'scripts/nickelmenu.conf',P/'nickelmenu.conf')
info={'port_version':'0.7.10-experimental','official_version':matched_version,'model':'Kobo Clara Colour (393)','firmware':'4.46.23836','first_launch':'NOT_PERFORMED','official_core_unmodified':True,'ui_assets_overlay':True,'font_sha256':hashlib.sha256((P/'fonts/NotoSansCJKsc-Regular.otf').read_bytes()).hexdigest()}
(P/'build-info.json').write_text(json.dumps(info,indent=2)+'\n')
# Only resource bytes from this completed assembly enter the recovery archive.
create_resource_recovery(P)
# Empty mutable directories are excluded; checksums cover every shipped file.
files=sorted(x for x in P.rglob('*') if x.is_file() and x.name!='SHA256SUMS')
(P/'SHA256SUMS').write_text(''.join(hashlib.sha256(x.read_bytes()).hexdigest()+'  '+str(x.relative_to(P))+'\n' for x in files))
print('Assembled',len(files),'regular files, no symbolic links')
