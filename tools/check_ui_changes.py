"""Source/packaging checks only. Does not claim device or GPU validation."""
from pathlib import Path
import re, struct, hashlib

root=Path(__file__).resolve().parents[1]
before=(root/'backup-v1.0.8-render-fix/main.c').read_text(encoding='utf-8')
after=(root/'source/main.c').read_text(encoding='utf-8')
def section(text,start,end):
    a=text.index(start)
    return text[a:text.index(end,a)]

# The removed names array only supplied display labels; all DSP/state remains.
remove_label=lambda s: re.sub(r'^static const char \*const reverb_names\[\].*\n','',s,flags=re.M)
assert remove_label(section(before,'static u64 last_art_frame_ms;','static u32 ui_color(')) == section(after,'static u64 last_art_frame_ms;','static u32 ui_color(')
assert section(before,'bool lr_both =','bool art_frame_due =') == section(after,'bool lr_both =','bool art_frame_due =')
assert section(before,'static void change_brightness(','int main(void)') == section(after,'static void change_brightness(','static void ui_boot_begin(')
print('PASS: audio/DSP, input handling and brightness implementations unchanged.')

calls=re.findall(r'C2D_Draw(?:RectSolid|CircleSolid|Text|Line|ImageAtRotated|ImageAt|Triangle)\([\s\S]*?\);',after)
assert calls and all('UI_DEPTH' in call for call in calls)
assert after.index('if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return;') < after.index('if (cover_texture_dirty)')
assert 'if(guide_page==3) guide_complete();' in after
assert section(after,'static void guide_load(','static void guide_complete(').count('"wb"') == 0
assert 'if(down & KEY_START) break;' in section(after,'if(guide_page) {','bool lr_both =')
assert '#define COVER_SIZE 128' in after
print(f'PASS: {len(calls)} draw sites share UI_DEPTH; texture upload and guide gates checked.')

def cia(path):
    data=path.read_bytes()
    align=lambda n:(n+63)&~63
    header,_,_,cert,ticket,tmd,meta,content=struct.unpack_from('<IHHIIIIQ',data)
    tmd_at=align(header)+align(cert)+align(ticket)
    payload=tmd_at+align(tmd)
    assert data[tmd_at:tmd_at+4]==bytes.fromhex('00010004')
    body=tmd_at+0x140
    version,count=struct.unpack_from('>HH',data,body+0x9c)
    title=data[body+0x4c:body+0x54]
    assert count==1
    record=body+0x9c4
    size=struct.unpack_from('>Q',data,record+8)[0]
    assert size==content and payload+size<=len(data)
    assert hashlib.sha256(data[payload:payload+size]).digest()==data[record+16:record+48]
    return version,title,len(data),hashlib.sha256(data).hexdigest()

old=cia(root/'backup-v1.0.8-render-fix/ShieldDJStandard.cia')
new=cia(root/'build/ShieldDJStandard.cia')
assert new[0]==1033 and new[0]>old[0] and new[1]==old[1]
print(f'PASS: CIA content SHA-256, unchanged Title ID {new[1].hex()}, version {new[0]}, {new[2]} bytes.')
print('CIA SHA-256:',new[3])
