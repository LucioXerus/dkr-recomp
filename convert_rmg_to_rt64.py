#!/usr/bin/env python3
"""
Convert the RMG/GLideN64-style Diddy Kong Racing PNG texture pack to an
RT64-runnable directory pack compatible with the DKR recomp's RmlUi loader.

Input:
  /home/christopher/dkr-recomp/DKRTexturePack/*.png
    Filename pattern: Diddy Kong Racing#HHHHHHHH#FMT#SIZ_all.png
    HHHHHHHH is the 8-hex Rice CRC.

  /home/christopher/dkr-recomp/DKRDUMP/rt64.json
    Produced by running build-steamdeck/texture_hasher on the TMEM dump.
    Maps each Rice CRC#FMT#SIZ -> 16-hex RT64 hash.

Output:
  /home/christopher/dkr-recomp/DKRTexturePack_RT64/
    rt64.json
    mod.json
    00 Misc/DIDDYKONGRACING#HHHHHHHH#FMT#SIZ_all.png
    ...

The directory is the working input to build-steamdeck/texture_packer.
"""

import json
import re
import shutil
import sys
from collections import defaultdict
from pathlib import Path

REPO = Path('/home/christopher/dkr-recomp')
PACK_IN = REPO / 'DKRTexturePack'
DUMP = REPO / 'DKRDUMP'
PACK_OUT = REPO / 'DKRTexturePack_RT64'

GAME_NAME = 'DIDDYKONGRACING'
APP_ID = 'io.github.cyscott.dkrrecomp'  # matches the .desktop file in AppDir
DISPLAY_NAME = 'DKR HD (RMG pack, RT64-converted)'
DESCRIPTION = ('Diddy Kong Racing HD texture pack, originally authored for the '
               'RMG (Rosalie\'s Mupen GUI) GLideN64 plugin, repackaged for the '
               'DKR recomp\'s RT64 renderer.')
SHORT_DESCRIPTION = 'DKR HD textures'

# Best-effort DKR category layout. We use one flat "00 Misc" category for now
# since the pack's CRCs are not labelled. Users can re-organise after.
CATEGORIES = [
    '00 Misc',
    '01 Adventure Worlds',
    '02 Race Tracks',
    '03 Characters',
    '04 Vehicles',
    '05 HUD',
    '06 Menus',
    '07 Effects',
    '08 Sprites',
    '09 Bosses',
    '10 Skies',
]

PAT = re.compile(r'#([0-9A-Fa-f]{8})#(\d+)#(\d+)')


def main():
    # 1. Load the dump's rt64.json.
    with open(DUMP / 'rt64.json') as f:
        dump_db = json.load(f)

    # Build: 8-hex CRC -> list of (fmt, siz, rt64_hash, original_rice_str)
    by_crc: dict[str, list[tuple[str, str, str, str]]] = defaultdict(list)
    for t in dump_db['textures']:
        r = t['hashes'].get('rice')
        if not r:
            continue
        parts = r.split('#')
        if len(parts) < 3:
            continue
        crc, fmt, siz = parts[0].lower(), parts[1], parts[2]
        rt64 = t['hashes'].get('rt64')
        if not rt64:
            continue
        # Normalize to lowercase to match RT64's canonical hash format.
        by_crc[crc].append((fmt, siz, rt64.lower(), r))

    # 2. Walk the PNG pack.
    pngs = sorted(PACK_IN.glob('*.png'))
    print(f'PNGs in pack: {len(pngs)}')

    # 3. Build join plan: for each PNG, decide the canonical RT64 hash.
    # Rule: if a CRC exists in dump, use the dump's (fmt, siz) and rt64 hash.
    # If not, mark as "orphan" (we'll copy it but warn it won't load).
    plan = []  # list of (src_png, dst_relpath, rt64_hash_or_None, original_rice, dump_rice)
    orphans = 0
    multi_crc_chosen_fmt_siz = 0
    for p in pngs:
        m = PAT.search(p.name)
        if not m:
            print(f'  WARN skipping {p.name}: filename does not match #CRC#FMT#SIZ pattern')
            continue
        crc, fmt, siz = m.group(1).lower(), m.group(2), m.group(3)
        if crc not in by_crc:
            plan.append((p, None, None, f'{crc}#{fmt}#{siz}', None))
            orphans += 1
            continue
        # Pick the dump entry whose (fmt, siz) matches the pack's. If none match,
        # fall back to the only entry (shouldn't happen given the dedup analysis).
        candidates = by_crc[crc]
        match = None
        for df, ds, rt64, orig in candidates:
            if df == fmt and ds == siz:
                match = (df, ds, rt64, orig)
                break
        if match is None:
            # No fmt/siz match; use the first candidate. The pack's (fmt, siz)
            # was wrong relative to RT64's view, so trust the dump.
            if len(candidates) > 1:
                multi_crc_chosen_fmt_siz += 1
            df, ds, rt64, orig = candidates[0]
        else:
            df, ds, rt64, orig = match
        # All dumps go to "00 Misc" for now. (CRC-based classification would
        # require a separate map we don't have.)
        dst = Path(CATEGORIES[0]) / f'{GAME_NAME}#{crc}#{df}#{ds}_all.png'
        plan.append((p, dst, rt64, f'{crc}#{fmt}#{siz}', f'{crc}#{df}#{ds}'))

    print(f'  joinable: {len(plan) - orphans}')
    print(f'  orphans:  {orphans}')

    # 4. Create output directory structure.
    if PACK_OUT.exists():
        shutil.rmtree(PACK_OUT)
    PACK_OUT.mkdir()
    for cat in CATEGORIES:
        (PACK_OUT / cat).mkdir()

    # 5. Copy PNGs to their destinations and build the new rt64.json.
    new_textures = []
    seen_rt64 = set()
    dup_count = 0
    for src, dst_rel, rt64, orig_rice, dump_rice in plan:
        if dst_rel is None:
            # Orphan: drop the rice record. We can still copy the file under
            # "00 Misc" with its pack-supplied fmt/siz for inspection, but we
            # don't put it in rt64.json because it won't match anything.
            dst_rel = Path(CATEGORIES[0]) / f'{GAME_NAME}#__{src.stem.split("#", 1)[1]}'
            shutil.copy2(src, PACK_OUT / dst_rel)
            continue
        shutil.copy2(src, PACK_OUT / dst_rel)
        if rt64 in seen_rt64:
            # Same RT64 hash in dump (one CRC -> one RT64 hash, so this only
            # happens if the pack had a CRC collision across different
            # (fmt,siz) PNGs that all joined). Use empty path on the second.
            dup_count += 1
            continue
        seen_rt64.add(rt64)
        new_textures.append({
            'hashes': {
                'rice': dump_rice.lower(),
                'rt64': rt64,
            },
            'path': '',
        })

    print(f'  unique RT64-hashed entries: {len(new_textures)}')
    if dup_count:
        print(f'  duplicate-RT64 PNGs skipped: {dup_count}')

    # 6. Write the new rt64.json. Use the same configurationVersion/hashVersion
    #    as the dump.
    cfg = dump_db.get('configuration', {})
    new_rt64 = {
        'configuration': {
            'autoPath': cfg.get('autoPath', 'rice'),
            'configurationVersion': cfg.get('configurationVersion', 3),
            'defaultOperation': cfg.get('defaultOperation', 'stream'),
            'defaultShift': cfg.get('defaultShift', 'none'),
            'hashVersion': cfg.get('hashVersion', 5),
        },
        'extraFiles': [
            'mod.json',
            'thumb.dds',
        ],
        'operationFilters': [],
        'shiftFilters': [],
        'textures': new_textures,
    }
    with open(PACK_OUT / 'rt64.json', 'w') as f:
        json.dump(new_rt64, f, indent=2)
    print(f'Wrote {PACK_OUT / "rt64.json"}')

    # 7. Write mod.json. game_id is read by the RmlUi launcher. The DKR
    #    recomp uses the .desktop id "io.github.cyscott.dkrrecomp"; check
    #    the AppDir for the exact value.
    mod = {
        'authors': ['Unknown RMG pack author', 'RT64 conversion script'],
        'description': DESCRIPTION,
        'display_name': DISPLAY_NAME,
        'game_id': 'diddy-kong-racing',  # matches mod_game_id in src/main.cpp:62
        'id': 'DKR-RMG-HD-RT64',
        'minimum_recomp_version': '0.1.18',
        'short_description': SHORT_DESCRIPTION,
        'version': '2026.07.28',
    }
    with open(PACK_OUT / 'mod.json', 'w') as f:
        json.dump(mod, f, indent=2)
    print(f'Wrote {PACK_OUT / "mod.json"}')

    # 8. Copy the MK64 thumb.dds as a placeholder thumbnail. (It's just a
    #    preview image; any DDS will do.)
    src_thumb = REPO / 'MK64TexturePack' / 'thumb.dds'
    if src_thumb.exists():
        shutil.copy2(src_thumb, PACK_OUT / 'thumb.dds')
        print(f'Copied placeholder thumb.dds')

    # 9. Save a report of orphans + dedup collisions for the user.
    report = PACK_OUT / 'CONVERSION_REPORT.txt'
    with open(report, 'w') as f:
        f.write('DKR RMG -> RT64 conversion report\n')
        f.write(f'Generated: {__file__}\n\n')
        f.write(f'PNGs in input pack: {len(pngs)}\n')
        f.write(f'Joinable: {len(plan) - orphans}\n')
        f.write(f'Orphans (not in dump, will not load): {orphans}\n')
        f.write(f'Unique RT64 entries written: {len(new_textures)}\n')
        f.write(f'PNG dupes (same RT64 hash, skipped): {dup_count}\n')
        f.write(f'Multi-fmt-siz CRCs where pack fmt/siz mismatched dump: {multi_crc_chosen_fmt_siz}\n\n')
        f.write('Orphan CRC list (walk the relevant area in-game to capture them):\n')
        for src, dst_rel, rt64, orig_rice, dump_rice in plan:
            if dst_rel is None:
                f.write(f'  {orig_rice}  {src.name}\n')
    print(f'Wrote {report}')

    print('\nDone. Next step: run texture_packer.')


if __name__ == '__main__':
    main()
