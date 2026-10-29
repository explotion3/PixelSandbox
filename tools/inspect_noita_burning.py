"""Extract original Noita combustion and material-interaction evidence read-only.

No game execution, injection, material import, or prototype parameter mapping.
Missing XML attributes remain missing. The JSON is evidence, not engine code.
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path('E:/SteamLibrary/steamapps/common/Noita'))
    parser.add_argument('--output', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'build/evidence/noita-burning')
    parser.add_argument('--dumpbin', type=Path, help='Optional installed MSVC dumpbin; disassemble verified local-build ranges')
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if output.is_relative_to(source):
        raise ValueError('Evidence output must be outside the source installation')
    output.mkdir(parents=True, exist_ok=True)
    archive = source / 'data/data.wak'
    with archive.open('rb') as stream:
        _, count, end, _ = struct.unpack('<4I', stream.read(16))
        if count > 100000 or end > archive.stat().st_size:
            raise ValueError('Invalid WAK header')
        entries = {}
        for _ in range(count):
            offset, size, length = struct.unpack('<3I', stream.read(12))
            if length > 1024 or stream.tell() + length > end:
                raise ValueError('Invalid WAK entry')
            name = stream.read(length).decode('utf-8')
            if offset + size > archive.stat().st_size:
                raise ValueError('Invalid WAK file range')
            entries[name] = offset, size

        def read(name):
            offset, size = entries[name]
            stream.seek(offset)
            return stream.read(size)

        materials_raw = read('data/materials.xml')
        root = ET.fromstring(materials_raw)
        nodes = {e.get('name'): e for e in root if e.tag in ('CellData', 'CellDataChild') and e.get('name')}
        combustion = re.compile(r'fire|burn|ignit|oxygen|smoke|flame', re.I)

        def resolve(name, visiting=()):
            if name in visiting:
                raise ValueError('Material inheritance cycle')
            node = nodes[name]
            parent = node.get('_parent')
            attributes, origins = resolve(parent, visiting + (name,)) if parent else ({}, {})
            attributes.update(node.attrib)
            origins.update({key: name for key in node.attrib})
            return attributes, origins

        materials = {}
        for name, node in nodes.items():
            attributes, origins = resolve(name)
            fields = {key: value for key, value in attributes.items() if combustion.search(key)}
            materials[name] = dict(parent=node.get('_parent'), cell_type=attributes.get('cell_type'),
                                   tags=attributes.get('tags'), combustion=fields,
                                   field_origins={key: origins[key] for key in fields},
                                   declared_children=[ET.tostring(e, encoding='unicode') for e in node])
        rules = [dict(index=i, attributes=e.attrib)
                 for i, e in enumerate(root.iter('Reaction'), 1)]
        fire_rules = [r for r in rules if any(re.search(r'fire|burn|lava', value, re.I)
                      for key, value in r['attributes'].items() if key.startswith('input_cell'))]
        fields = sorted({key for m in materials.values() for key in m['combustion']})
        report = dict(materials_xml_sha256=hashlib.sha256(materials_raw).hexdigest(),
                      material_count=len(nodes), combustion_fields=fields,
                      materials=materials, all_reactions=rules,
                      fire_lava_input_reaction_indices=[r['index'] for r in fire_rules])
        (output / 'material-combustion.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        interactions = dict(materials_xml_sha256=report['materials_xml_sha256'],
                            materials={name: dict(attributes=resolve(name)[0],
                                                  field_origins=resolve(name)[1],
                                                  declared_children=materials[name]['declared_children'])
                                       for name in nodes},
                            reactions=[dict(index=i, attributes=e.attrib,
                                            declared_children=[ET.tostring(c, encoding='unicode') for c in e])
                                       for i, e in enumerate(root.iter('Reaction'), 1)],
                            required_reaction_attributes=[dict(index=i, attributes=e.attrib)
                                                          for i, e in enumerate(root.iter('ReqReaction'), 1)],
                            required_reactions=[ET.tostring(e, encoding='unicode') for e in root.iter('ReqReaction')])
        (output / 'material-interactions.json').write_text(json.dumps(interactions, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        corrosion_tags = ('[acid]', '[corrodible]', '[meltable_to_acid]',
                          '[meltable_metal]', '[meltable_metal_generic]')
        tag_members = {tag: [name for name in nodes
                            if tag in resolve(name)[0].get('tags', '').split(',')]
                       for tag in corrosion_tags}
        acid_rules = [r for r in interactions['reactions']
                      if any(re.search(r'acid|corrodible|meltable_to_acid', value)
                             for value in r['attributes'].values())]
        mana_rules = [r for r in interactions['reactions']
                      if any(value == 'magic_liquid_mana_regeneration'
                             for key, value in r['attributes'].items() if key.startswith('input_cell'))]
        corrosion_material_names = ['acid', 'acid_gas', 'acid_gas_static', 'ice_acid_static',
                                    'ice_acid_glass', 'wood', 'wood_prop', 'wood_static', 'rock_static',
                                    'steel_static', 'templebrick_static', 'glass', 'glass_brittle',
                                    'diamond', 'gold', 'gold_box2d', 'water', 'magic_liquid_mana_regeneration']
        corrosion = dict(materials_xml_sha256=report['materials_xml_sha256'],
                         note='Source XML records and inherited declared attributes, not a runtime reaction-table dump.',
                         tag_members_from_resolved_attributes=tag_members,
                         acid_related_reactions=acid_rules, mana_input_reactions=mana_rules,
                         selected_materials={name: interactions['materials'][name]
                                             for name in corrosion_material_names if name in nodes})
        (output / 'corrosion-evidence.json').write_text(json.dumps(corrosion, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        phase_fields = ('cold_freezes_to_material', 'warmth_melts_to_material',
                        'cold_freezes_chance_rev', 'warmth_melts_chance_rev',
                        'cold_freezes_to_dont_do_reverse_reaction')
        phase_materials = {name: dict(attributes={k: m['attributes'][k] for k in phase_fields
                                                   if k in m['attributes']},
                                      field_origins={k: m['field_origins'][k] for k in phase_fields
                                                     if k in m['attributes']})
                           for name, m in interactions['materials'].items()
                           if any(k in m['attributes'] for k in phase_fields)}
        cold_groups = {}
        for name, material in phase_materials.items():
            target = material['attributes'].get('cold_freezes_to_material')
            if target:
                cold_groups.setdefault(target, []).append(name)
        phase_rules = [r for r in interactions['reactions']
                       if any('meltable' in v or v == 'steam' or '_molten' in v
                              for v in r['attributes'].values())]
        phase = dict(materials_xml_sha256=report['materials_xml_sha256'],
                     note='Declared/inherited attributes and keyword-selected recipes; not final runtime targets or a count of thermal algorithms.',
                     phase_materials=phase_materials, cold_target_source_groups=cold_groups,
                     field_material_counts={k: sum(k in m['attributes'] for m in phase_materials.values())
                                            for k in phase_fields},
                     contact_recipe_selector='Any attribute contains meltable, equals steam, or contains _molten.',
                     contact_recipe_matches=phase_rules)
        (output / 'phase-change-evidence.json').write_text(json.dumps(phase, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')

        stain_fields = ('liquid_stains', 'liquid_stains_self', 'liquid_stains_custom_color',
                        'liquid_sprite_stain_shaken_drop_chance',
                        'liquid_sprite_stain_ignited_drop_chance',
                        'liquid_sprite_stains_check_offset', 'liquid_sprite_stains_status_threshold',
                        'status_effects')
        stain_records = {}
        for name, node in nodes.items():
            attributes, origins = resolve(name)
            status = node.find('StatusEffects')
            if status is not None or any(k in attributes for k in stain_fields):
                stain_records[name] = dict(
                    parent=node.get('_parent'),
                    attributes={k: attributes[k] for k in stain_fields if k in attributes},
                    field_origins={k: origins[k] for k in stain_fields if k in attributes},
                    declared_status_effects_xml=(ET.tostring(status, encoding='unicode')
                                                 if status is not None else None))
        stains = dict(materials_xml_sha256=report['materials_xml_sha256'],
                      note='Inherited declared attributes and direct child XML only; missing fields are not runtime defaults. Nested StatusEffects inheritance is not resolved.',
                      materials=stain_records)
        (output / 'stain-evidence.json').write_text(json.dumps(stains, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        adhesion_fields = ('liquid_sticks_to_ceiling', 'liquid_slime', 'liquid_sand', 'liquid_static', 'liquid_sand_never_box2d')
        adhesion = dict(materials_xml_sha256=report['materials_xml_sha256'],
                        note='Inherited declared attributes only; native defaults and represented support states are documented separately.',
                        materials={name: dict(attributes={k: m['attributes'][k] for k in adhesion_fields if k in m['attributes']},
                                              field_origins={k: m['field_origins'][k] for k in adhesion_fields if k in m['attributes']})
                                   for name, m in interactions['materials'].items()
                                   if 'liquid_sticks_to_ceiling' in m['attributes'] or m['attributes'].get('liquid_slime') == '1'})
        (output / 'adhesion-evidence.json').write_text(json.dumps(adhesion, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        motion_fields = {'cell_type', 'density', 'durability', 'crackability',
                         'convert_to_box2d_material', 'liquid_gravity', 'liquid_sand',
                         'liquid_static', 'liquid_slime', 'liquid_viscosity',
                         'liquid_damping', 'liquid_flow_speed', 'liquid_sticks_to_ceiling',
                         'liquid_sand_never_box2d'}
        motion = dict(materials_xml_sha256=report['materials_xml_sha256'],
                      note='Resolved declared XML attributes only. Missing attributes are not native defaults; binary algorithms and unresolved consumers are documented separately.',
                      materials={name: dict(attributes={k: v for k, v in m['attributes'].items()
                                                       if k in motion_fields or k.startswith(('gas_', 'solid_'))},
                                            field_origins={k: v for k, v in m['field_origins'].items()
                                                           if k in motion_fields or k.startswith(('gas_', 'solid_'))})
                                 for name, m in interactions['materials'].items()})
        (output / 'motion-physics-materials.json').write_text(json.dumps(motion, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')

        patterns = re.compile(
            r'fire_probability_of_ignition|fire_how_much_fire_generates|fire_damage_ignited_amount|'
            r'fire_damage_amount|extinguish_fire|fan_the_flames|ignite_materials|'
            r'probability_of_ignition_attempt|frames_suffocated_to_extinguish|suffocation_check_offset_y|'
            r'fire_cells_dont_ignite_damagemodel|protects_from_fire|'
            r'effect\s*=\s*["\'](?:ON_FIRE|PROTECTION_FIRE|WET|OILED)["\']|'
            r'GameGetGameEffectCount\([^\n]*ON_FIRE')
        references = []
        interaction_references = []
        explosion_references = []
        temperature_references = []
        stain_references = []
        stain_pattern = re.compile(
            r'EntityAddRandomStains|EntityRemoveStainStatusEffect|stain_effects|'
            r'stain_effect_cooldowns|stain_shaken_drop_chance_multiplier|SpriteStainsComponent')
        temperature_pattern = re.compile('|'.join(map(re.escape, phase_fields)) +
                                         r'|temperature_reaction_temp|reaction_freeze_chance|reaction_unfreeze_chance')
        explosion_pattern = re.compile(
            r'ExplosionComponent|ExplodeOnDamageComponent|config_explosion|ExplosionConfig|'
            r'ray_energy|max_durability_to_destroy|hole_destroy_liquid|hole_destroy_physics_dynamic|'
            r'cell_explosion_|create_cell_probability|material_sparks_')
        interaction_pattern = re.compile(
            r'temperature_reaction_temp|extinguish_fire|probability_to_heat|hack_is_set_fire|'
            r'do_reactions|reaction_speed|reactions_shaking_speeds_up|reaction_freeze_chance|'
            r'reaction_unfreeze_chance|Electricity(?:Source|Receiver)?Component|ElectricChargeComponent')
        for name in sorted(entries):
            if not name.endswith(('.xml', '.lua')) or name == 'data/materials.xml':
                continue
            raw = read(name)
            text = raw.decode('utf-8', errors='replace').replace('\r\r\n', '\n').replace('\r\n', '\n')
            hits = [dict(line=i, text=line.strip()) for i, line in enumerate(text.split('\n'), 1)
                    if patterns.search(line)]
            if hits:
                references.append(dict(path=name, sha256=hashlib.sha256(raw).hexdigest(), matches=hits))
            interaction_hits = [dict(line=i, text=line.strip()) for i, line in enumerate(text.split('\n'), 1)
                                if interaction_pattern.search(line)]
            if interaction_hits:
                interaction_references.append(dict(path=name, sha256=hashlib.sha256(raw).hexdigest(),
                                                   matches=interaction_hits, text=text))
            explosion_hits = [dict(line=i, text=line.strip()) for i, line in enumerate(text.split('\n'), 1)
                              if explosion_pattern.search(line)]
            if explosion_hits:
                explosion_references.append(dict(path=name, sha256=hashlib.sha256(raw).hexdigest(),
                                                 matches=explosion_hits))
            temperature_hits = [dict(line=i, text=line.strip()) for i, line in enumerate(text.split('\n'), 1)
                                if temperature_pattern.search(line)]
            if temperature_hits:
                temperature_references.append(dict(path=name, sha256=hashlib.sha256(raw).hexdigest(),
                                                   matches=temperature_hits, text=text))
            stain_hits = [dict(line=i, text=line.strip()) for i, line in enumerate(text.split('\n'), 1)
                          if stain_pattern.search(line)]
            if stain_hits:
                stain_references.append(dict(path=name, sha256=hashlib.sha256(raw).hexdigest(),
                                             matches=stain_hits))
        (output / 'entity-script-references.json').write_text(json.dumps(references, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        (output / 'interaction-script-references.json').write_text(json.dumps(interaction_references, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        (output / 'explosion-script-references.json').write_text(json.dumps(explosion_references, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        (output / 'temperature-script-references.json').write_text(json.dumps(temperature_references, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        (output / 'stain-script-references.json').write_text(json.dumps(stain_references, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        selected = ['data/scripts/status_effects/status_list.lua',
                    'data/scripts/status_effects/status_reflect.lua',
                    'data/entities/misc/effect_on_fire.xml', 'data/entities/misc/effect_wet.xml',
                    'data/entities/misc/effect_oiled.xml', 'data/entities/player_base.xml',
                    'data/entities/props/physics_box_harmless.xml',
                    'data/entities/base_item_physics2.xml', 'data/entities/base_item_physics.xml',
                    'data/entities/misc/loose_ground.xml',
                    'data/entities/misc/electricity.xml', 'data/entities/projectiles/bomb.xml',
                    'data/entities/particles/particle_explosion/main_gunpowder_large.xml']
        extracts = []
        for name in selected:
            if name in entries:
                raw = read(name)
                extracts.append(dict(path=name, sha256=hashlib.sha256(raw).hexdigest(),
                                     text=raw.decode('utf-8', errors='replace')))
        (output / 'selected-original-resources.json').write_text(json.dumps(extracts, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    documentation = (source / 'tools_modding/component_documentation.txt').read_text(encoding='utf-8-sig')
    headings = list(re.finditer(r'^([A-Za-z0-9_]+Component)\s*$', documentation, re.M))
    names = {'DamageModelComponent', 'MagicConvertMaterialComponent', 'TorchComponent',
             'ParticleEmitterComponent', 'ElectricityComponent', 'ExplosionComponent',
             'ExplodeOnDamageComponent', 'GameEffectComponent', 'StatusEffectDataComponent',
             'SpriteStainsComponent', 'MaterialInventoryComponent', 'VelocityComponent',
             'LooseGroundComponent', 'PhysicsBodyComponent', 'PhysicsBody2Component',
             'PhysicsImageShapeComponent', 'PhysicsShapeComponent', 'PixelSpriteComponent',
             'CharacterDataComponent', 'CharacterPlatformingComponent', 'PlayerCollisionComponent'}
    sections = {}
    for i, heading in enumerate(headings):
        if heading[1] in names:
            stop = headings[i + 1].start() if i + 1 < len(headings) else len(documentation)
            sections[heading[1]] = dict(line=documentation.count('\n', 0, heading.start()) + 1,
                                       text=documentation[heading.start():stop])
    (output / 'component-sections.json').write_text(json.dumps(sections, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    api_path = source / 'tools_modding/lua_api_documentation.txt'
    api_raw = api_path.read_bytes()
    stain_api = dict(sha256=hashlib.sha256(api_raw).hexdigest(),
                     matches=[dict(line=i, text=line) for i, line in
                              enumerate(api_raw.decode('utf-8-sig').splitlines(), 1)
                              if re.search(r'^Entity(?:AddRandomStains|RemoveStainStatusEffect)\(', line)])
    (output / 'stain-api-documentation.json').write_text(json.dumps(stain_api, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    interaction_names = {'ElectricChargeComponent', 'ElectricityComponent', 'ElectricitySourceComponent',
                         'ElectricityReceiverComponent', 'MagicConvertMaterialComponent',
                         'MaterialInventoryComponent', 'CellEaterComponent', 'CellClumpFreezerComponent',
                         'ExplosionComponent', 'ExplodeOnDamageComponent', 'ProjectileComponent'}
    interaction_sections = {}
    for i, heading in enumerate(headings):
        if heading[1] in interaction_names:
            stop = headings[i + 1].start() if i + 1 < len(headings) else len(documentation)
            interaction_sections[heading[1]] = dict(line=documentation.count('\n', 0, heading.start()) + 1,
                                                    text=documentation[heading.start():stop])
    (output / 'interaction-component-sections.json').write_text(json.dumps(interaction_sections, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    hashes = {name: hashlib.sha256((source / name).read_bytes()).hexdigest()
              for name in ['noita.exe', 'noita_dev.exe', 'data/data.wak', 'tools_modding/component_documentation.txt']}
    (output / 'source-hashes.json').write_text(json.dumps(hashes, indent=2) + '\n', encoding='utf-8')
    if args.dumpbin:
        if hashes['noita.exe'] != '808d2a0ab51ea0b46e9ad2aeb3327a4b0ce3feae04f32ba26326bf585b5779bd':
            raise ValueError('Unrecognized executable hash: do not reuse these version-specific disassembly ranges')
        ranges = {
            'material-fire-parser': (0x6fb010, 0x6fb931),
            'ignition-derived-threshold': (0x6fe541, 0x6fe557),
            'burnable-common': (0x70f430, 0x70f7ff),
            'emissions-spread': (0x70f800, 0x70fddf),
            'liquid-update': (0x70c6e0, 0x70cb3f),
            'liquid-ignite-override': (0x70cdf0, 0x70ce35),
            'solid-update': (0x70e060, 0x70e44f),
            'gas-update': (0x70a670, 0x70aac0),
            'fire-cell-update': (0x709fc0, 0x70a39f),
            'random-neighbor': (0x4abc40, 0x4abcff),
            'color-helper': (0x70e760, 0x70e8bf),
            'liquid-flags-heat': (0x70b110, 0x70b1cf),
            'thermal-field-parser': (0x6fca6c, 0x6fce7b),
            'thermal-id-resolve': (0x707b47, 0x707bc5),
            'reaction-record-parser': (0x7022d8, 0x70376f),
            'reaction-fast-flag-parser': (0x701d60, 0x701f50),
            'reaction-table-build': (0x706b1d, 0x706c9f),
            'reaction-lookup': (0x706cc0, 0x706d96),
            'cell-reaction-dispatch': (0x70e8c0, 0x70ebef),
            'reaction-execute': (0x70ebf0, 0x70ef8f),
            'burning-reaction-dispatch': (0x70f160, 0x70f35f),
            'cell-material-conversion': (0x70f360, 0x70f42f),
            'world-blob-entry': (0x71ab50, 0x71b07f),
            'world-set-material': (0x71b140, 0x71b2cf),
            'world-convert-all-entry': (0x71a970, 0x71a9af),
            'threaded-blob-entry': (0x72b250, 0x72b2ef),
            'threaded-blob-buffer-append': (0x8a6b60, 0x8a6c0f),
            'world-convert-buffer-append': (0x89d220, 0x89d2af),
            'required-reaction-binding': (0x706da0, 0x706eaa),
            'required-reaction-select': (0x709d90, 0x709e0f),
            'required-reaction-execute': (0x70ef90, 0x70f15f),
            'reaction-label-expand': (0x703790, 0x7040e6),
            'reaction-tag-members': (0x705d10, 0x705daf),
            'reaction-tag-registration': (0x705f44, 0x7060a6),
            'material-name-id-lookup': (0x704c20, 0x704d33),
            'string-substitution': (0xddf780, 0xddf87f),
            'trig-imports': (0xdfe242, 0xdfe259),
            'material-conductivity-parser': (0x6fc82f, 0x6fc8d8),
            'material-sand-flag-parser': (0x6fbdcc, 0x6fbe01),
            'electricity-component-layout': (0x6524b1, 0x652b1f),
            'electricity-system-wrapper': (0xc6c790, 0xc6c911),
            'electricity-init': (0xad0ca0, 0xad0ddf),
            'electricity-update': (0xad0de0, 0xad1a8f),
            'electricity-crack-dispatch': (0xad1a90, 0xad1b5f),
            'electric-charge-update': (0xad09b0, 0xad0c6f),
            'electric-source-update': (0xad8920, 0xad8c9f),
            'electric-receiver-update': (0xad6240, 0xad653f),
            'lcg-inclusive-helper': (0xddcc10, 0xddcc6f),
            'vector-normalize-helper': (0x8970f0, 0x89715f),
            'sqrt-wrapper': (0x44a410, 0x44a42f),
            'electric-spark-audio': (0x4b1d90, 0x4b1e6f),
            'entity-end-request': (0x44df60, 0x44dfb2),
            'material-durability-parser': (0x6fb92a, 0x6fb9f7),
            'material-hp-parser': (0x6fdce6, 0x6fdda2),
            'cell-solid-predicate': (0x4adaa0, 0x4adaa2),
            'explosion-config-constructor': (0x4b2780, 0x4b2b2f),
            'explosion-config-layout': (0x4b6060, 0x4b8142),
            'world-explosion-entry': (0x721350, 0x721368),
            'threaded-explosion-entry': (0x72b3d0, 0x72b42c),
            'explosion-dispatch': (0x657410, 0x657661),
            'explosion-execute': (0x657960, 0x65c126),
            'explosion-ray-probe': (0x656b10, 0x656e74),
            'explosion-body-ray-probe': (0x656e80, 0x6572f6),
            'explosion-system-callbacks': (0xae7a30, 0xae7ad0),
            'reaction-record-defaults': (0x4ac6f0, 0x4ac7df),
            'reaction-config-constructor': (0x708f40, 0x708f75),
            'material-lifetime-parser': (0x6fb9f8, 0x6fbac5),
            'gas-cell-constructor': (0x70a4a0, 0x70a566),
            'gas-lifetime-guard': (0x70a670, 0x70a6ca),
            'gas-cell-move': (0x70aad0, 0x70ab79),
            'gas-lifetime-accessors': (0x70aba0, 0x70abb5),
            'gas-reaction-dispatch-tail': (0x70aa3a, 0x70aa7f),
            'material-phase-defaults': (0x4ac7e0, 0x4aca35),
            'biome-modifiers-constructor': (0x4871c0, 0x48722f),
            'biome-temperature-field-parser': (0x4883b9, 0x4884c4),
            'biome-modifiers-copy': (0x488870, 0x4888bc),
            'world-biome-member-init': (0x717ca5, 0x717cb5),
            'world-biome-copy-site': (0x72a51b, 0x72a53e),
            'plain-liquid-predicate': (0x4ac0f0, 0x4ac116),
            'cell-liquid-predicate': (0x5b01b0, 0x5b01b2),
            'cell-freeze-blocker-default': (0x4ac040, 0x4ac042),
            'liquid-freeze-blocker': (0x70b1d0, 0x70b1fe),
            'world-temperature-transform': (0x71aa60, 0x71ab45),
            'temperature-component-layout': (0x748870, 0x7492c7),
            'magic-convert-single-cell': (0xbaac80, 0xbab0ea),
            'magic-convert-circle-sampling': (0xbab0f0, 0xbab496),
            'magic-convert-row-sampling': (0xbab4a0, 0xbab555),
            'magic-convert-column-sampling': (0xbab560, 0xbab615),
            'magic-convert-component-update': (0xbab8c0, 0xbabf8c),
            'magic-convert-update-callsite': (0xc8db60, 0xc8db8b),
            'stain-material-defaults': (0x4aca36, 0x4aca96),
            'stain-material-parser': (0x6fbf9e, 0x6fc48c),
            'stain-status-component-layout': (0x63a3a0, 0x63a451),
            'stain-sprite-component-layout': (0x6a9d26, 0x6a9d77),
            'stain-player-tag-predicate': (0x63e880, 0x63e91b),
            'sprite-stains-component-update': (0xc22080, 0xc23103),
            'stain-system-update-callsite': (0xc8fe00, 0xc8fe2f),
            'stain-clear-status-pixels': (0xc23170, 0xc23289),
            'stain-add-random-pixels': (0xc23290, 0xc235af),
            'stain-lua-remove': (0x7b4ef0, 0x7b52cf),
            'stain-lua-add-random': (0x7b52d0, 0x7b55d8),
            'status-native-registration': (0xc23690, 0xc23c9d),
            'status-effect-component-update': (0xc24670, 0xc24e95),
            'stain-protection-predicate': (0xc24f50, 0xc24fad),
            'stain-drop-percent-helper': (0xddccf0, 0xddcd3b),
            'stain-fire-contact': (0xab6010, 0xab6ccd),
            'stain-fire-counter-update': (0xab7c68, 0xab7cb7),
            'stain-fire-ending': (0xab803a, 0xab8082),
            'stain-damage-model-field-layout': (0x4e978e, 0x4e9837),
            'adhesion-material-default': (0x4aca40, 0x4aca49),
            'adhesion-material-parser': (0x6fc48d, 0x6fc53e),
            'adhesion-coordinate-direction': (0x70acc0, 0x70ad27),
            'adhesion-support-predicates': (0x70ac50, 0x70acbe),
            'adhesion-liquid-accessors': (0x70b110, 0x70b1c7),
            'adhesion-cell-constructor': (0x70ae10, 0x70af12),
            'adhesion-acquisition': (0x70b2d0, 0x70b441),
            'adhesion-retention': (0x70c70b, 0x70c834),
            'motion-liquid-entry': (0x70b200, 0x70b2cf),
            'motion-density-parser': (0x6fbac6, 0x6fbb95),
            'motion-sand-step': (0x70b2d0, 0x70b7bf),
            'motion-slime-step': (0x70b7c0, 0x70bdbf),
            'motion-fluid-step': (0x70bdc0, 0x70c6df),
            'motion-liquid-dispatch': (0x70c9a2, 0x70cb3f),
            'motion-liquid-counter-notify': (0x70d180, 0x70d19f),
            'motion-liquid-swap': (0x70cc00, 0x70cd7f),
            'motion-neighbor-notify': (0x70abf0, 0x70ac4f),
            'motion-gravity-parser': (0x6fbb96, 0x6fbc65),
            'motion-viscosity-parser': (0x6fbe02, 0x6fbecf),
            'motion-material-parser': (0x6fc53f, 0x6fc6a6),
            'motion-material-defaults': (0x4aca97, 0x4acad9),
            'motion-gas-entry': (0x70a5a0, 0x70a66f),
            'motion-gas-locomotion': (0x70a810, 0x70aa7f),
            'motion-gas-parser': (0x6fce82, 0x6fd19a),
            'motion-solid-cell-init': (0x71c017, 0x71c0a8),
            'motion-solid-type-local': (0x4ada80, 0x4adb4f),
            'motion-solid-local-transform': (0x4ad180, 0x4ad1cf),
            'motion-solid-world-position': (0x70e470, 0x70e50f),
            'motion-solid-point-velocity': (0x70e510, 0x70e59f),
            'motion-solid-point-callsite': (0x70d772, 0x70d7a3),
            'motion-world-v4c': (0x71f7d0, 0x71fb9f),
            'motion-world-thread-v4c': (0x72b320, 0x72b38f),
            'motion-world-deferred-append': (0x89d7a0, 0x89d80f),
            'loose-update-callsite': (0xc8d240, 0xc8d26b),
            'loose-component-update': (0xba5ba0, 0xba5f1f),
            'loose-hit-predicate': (0xba58b0, 0xba593f),
            'loose-ray-hit': (0xba5940, 0xba5b6f),
            'loose-ray-helper': (0xba5f20, 0xba632f),
            'loose-chunk-shape': (0xba6330, 0xba698b),
            'loose-border-helper': (0xba6a00, 0xba6b4f),
        }
        for name, (start, stop) in ranges.items():
            result = subprocess.run([str(args.dumpbin.resolve()), '/nologo', '/disasm',
                                     f'/range:0x{start:X},0x{stop:X}', str(source / 'noita.exe')],
                                    capture_output=True, check=True)
            (output / (name + '.asm.txt')).write_bytes(result.stdout)
        (output / 'disassembly-ranges.json').write_text(json.dumps({name: {'start_va': hex(a), 'end_va': hex(b)}
                 for name, (a, b) in ranges.items()}, indent=2) + '\n', encoding='utf-8')
        from inspect_noita_binary import PE32
        pe = PE32(source / 'noita.exe')
        motion_checkpoints = dict(
            executable_sha256=hashes['noita.exe'],
            note='Static instruction/constant evidence for section 16, not runtime verification or recovered original symbols. Constant addresses below are VA.',
            float_constants_by_va={hex(va): struct.unpack_from('<f', pe.data, pe.raw(va - pe.base))[0]
                                   for va in (0x1053558, 0x105361c, 0x1053730, 0x1053780,
                                              0x10539f8, 0x1053b24, 0x1053dc8,
                                              0x1053fa4, 0x1054088, 0x11d2a28)},
            liquid_cell_offsets={'direction': '0x20', 'blocked_counter': '0x21',
                                 'static_or_attached': '0x22', 'fall_history_byte': '0x23',
                                 'fall_accumulator': '0x24', 'lateral_accumulator': '0x28',
                                 'update_epoch': '0x2c'},
            native_entry_ranges={name: dict(start_va=hex(start), end_va=hex(stop))
                                 for name, (start, stop) in ranges.items()
                                 if name.startswith(('motion-', 'loose-'))})
        (output / 'motion-native-checkpoints.json').write_text(json.dumps(motion_checkpoints, indent=2) + '\n', encoding='utf-8')
        adhesion_checkpoints = dict(
            executable_sha256=hashes['noita.exe'],
            note='Static local-build offsets and instruction-derived checkpoints; not original C++ symbols or runtime observation.',
            material_ceiling_threshold_offset='0x164', material_default_write_va='0x4aca40',
            liquid_cell_offsets={'motion_direction':'0x20','sand_counter':'0x21','static_or_attached_flag':'0x22'},
            update_gate=dict(random_modulus=101, strict_threshold=20),
            support_vtable_offsets={'sand_predicate':'0x64','state_classification':'0x6c','liquid_predicate':'0x58','motion_direction':'0x10'},
            coordinate_direction_cases=[dict(x=x,y=y,attached_support_direction=d) for x,y,d in
                                        [(-1,64,7),(-1,63,5),(1,63,6),(-1,-2,7),(-4,-1,5)]])
        (output / 'adhesion-native-checkpoints.json').write_text(json.dumps(adhesion_checkpoints, indent=2) + '\n', encoding='utf-8')
        stain_constants = (0x10539f8, 0x1053780, 0x1053fa4, 0x1053e58,
                           0x105377c, 0x1053520, 0x1053c18, 0x1053e20,
                           0x1053cf8, 0x1053d40, 0x1053dc0, 0x1053558)
        stain_checkpoints = dict(
            executable_sha256=hashes['noita.exe'],
            note='Version-specific static analysis names/offsets; not recovered C++ source or runtime observation.',
            material_offsets={'liquid_stains': '0x170', 'shaken_drop_chance': '0x178',
                              'ignited_drop_chance': '0x17c', 'check_offset': '0x180',
                              'status_threshold': '0x184', 'stain_status_pairs_begin': '0x250'},
            sprite_component_offsets={'mData': '0x5c', 'mState': '0x64',
                                      'stain_shaken_drop_chance_multiplier': '0x50'},
            sprite_stains_data_offsets={'rgba_pixels': '0x18', 'material_id_pixels_u16': '0x58',
                                       'source_sprite_image': '0x64'},
            status_component_offsets={'stain_effects': '0x48', 'stain_effect_cooldowns': '0x54',
                                      'effects_previous': '0x60', 'ingestion_effects': '0x6c',
                                      'smoothed_stain_effects_for_ui': '0x94'},
            status_definition_offsets={'protects_from_fire': '0x60',
                                       'remove_cells_that_cause_when_activated': '0x61'},
            damage_model_offsets={'is_on_fire': '0x204', 'mIsOnFire': '0x218',
                                  'mFireFramesLeft': '0x220', 'mFireDurationFrames': '0x224',
                                  'mFireTriedIgniting': '0x228'},
            float_constants_by_va={hex(va): struct.unpack_from('<f', pe.data, pe.raw(va - pe.base))[0]
                                   for va in stain_constants},
            predicate_tags={'player_unit_va': '0x10023b0',
                            'player_unit': pe.string(pe.raw(0x10023b0 - pe.base))},
            special_status_names_by_va={hex(va): pe.string(pe.raw(va - pe.base))
                                       for va in (0x100c7c4, 0x104029c)},
            component_rtti_names_by_type_descriptor_va={hex(va): pe.string(pe.raw(va - pe.base) + 8)
                                                       for va in (0x11d73cc, 0x11d485c, 0x11d4a58)})
        (output / 'stain-native-checkpoints.json').write_text(json.dumps(stain_checkpoints, indent=2) + '\n', encoding='utf-8')
        stain_rtti = {name: pe.hierarchy(f'.?AV{name}@@') for name in
                      ('SpriteStainsSystem', 'StatusEffectDataSystem',
                       'SpriteStainsComponent', 'StatusEffectDataComponent')}
        (output / 'stain-rtti.json').write_text(json.dumps(stain_rtti, indent=2) + '\n', encoding='utf-8')
        constants = {hex(va): struct.unpack_from('<f', pe.data, pe.raw(va - pe.base))[0]
                     for va in (0x1053c60, 0x105361c, 0x1053c08, 0x1053a74,
                                0x10539f8, 0x10540ec, 0x105353c, 0x10534fc, 0x1053dc0,
                                0x1053ef4, 0x1053780)}
        direction_data = pe.raw(0x11549b0 - pe.base)
        directions = [struct.unpack_from('<ii', pe.data, direction_data + i * 8) for i in range(4)]
        imports = {}
        for va in (0xf07340, 0xf07334):
            thunk = pe.u32(pe.raw(va - pe.base))
            imports[hex(va)] = pe.string(pe.raw(thunk) + 2)
        checkpoints = dict(executable_sha256=hashes['noita.exe'],
                           float_constants_by_va=constants,
                           required_reaction_directions=dict(table_va='0x11549b0',
                                                             image_initial_values=directions),
                           trig_imports_by_iat_va=imports)
        sqrt_thunk_va = pe.u32(pe.raw(0xdfe25a - pe.base) + 2)
        sqrt_name_rva = pe.u32(pe.raw(sqrt_thunk_va - pe.base))
        checkpoints['electricity_distance_math_import'] = dict(
            call_target_va='0xdfe25a', iat_va=hex(sqrt_thunk_va),
            import_name=pe.string(pe.raw(sqrt_name_rva) + 2))
        (output / 'interaction-native-checkpoints.json').write_text(json.dumps(checkpoints, indent=2) + '\n', encoding='utf-8')
        electrical_types = ['ElectricitySystem', 'ElectricityComponent', 'ElectricChargeSystem',
                            'ElectricitySourceSystem', 'ElectricityReceiverSystem']
        electrical_rtti = {name: pe.hierarchy(f'.?AV{name}@@') for name in electrical_types}
        (output / 'electricity-rtti.json').write_text(json.dumps(electrical_rtti, indent=2) + '\n', encoding='utf-8')
        explosion_types = ['ConfigExplosion', 'ExplosionSystem', 'ExplodeOnDamageSystem', 'Message_Explosion']
        explosion_rtti = {name: pe.hierarchy(f'.?AV{name}@@') for name in explosion_types}
        explosion_rtti['grid_cell_types'] = {name: pe.hierarchy(name)
                                            for name in ('ICell', 'CSolidCell', 'CLiquidCell', 'CGasCell', 'CFireCell')}
        (output / 'explosion-rtti.json').write_text(json.dumps(explosion_rtti, indent=2) + '\n', encoding='utf-8')
        # Manually matched XML readers and use sites; not an inferred C++ declaration.
        explosion_fields = {
            'never_cache': 0x4, 'explosion_radius': 0x8, 'load_this_entity': 0x48,
            'hole_enabled': 0x78, 'destroy_non_platform_solid_enabled': 0x79,
            'hole_destroy_liquid': 0x8c, 'hole_destroy_physics_dynamic': 0x8d,
            'create_cell_material': 0x90, 'create_cell_probability': 0xa8,
            'material_sparks_min_hp': 0xc8, 'material_sparks_probability': 0xcc,
            'material_sparks_count_min': 0xd0, 'material_sparks_count_max': 0xd4,
            'material_sparks_enabled': 0xd8, 'material_sparks_real': 0xd9,
            'material_sparks_scale_with_hp': 0xda, 'stains_enabled': 0xe8,
            'ray_energy': 0xf0, 'max_durability_to_destroy': 0xf4,
            'damage_mortals': 0xfd, 'physics_throw_enabled': 0xfe,
            'cell_explosion_power': 0x10c, 'cell_explosion_radius_min': 0x110,
            'cell_explosion_radius_max': 0x114, 'cell_explosion_velocity_min': 0x118,
            'cell_explosion_damage_required': 0x11c, 'cell_explosion_probability': 0x120,
            'cell_explosion_power_ragdoll_coeff': 0x124, 'pixel_sprites_enabled': 0x128,
            'is_digger': 0x129, 'delay': 0x148, 'explosion_delay_id': 0x150,
        }
        explosion_checkpoints = dict(
            executable_sha256=hashes['noita.exe'],
            config_field_offsets={name: hex(at) for name, at in explosion_fields.items()},
            generic_constructor_create_cell_material=pe.string(pe.raw(0xff08ac - pe.base)),
            ray_count=360, ray_step_limit=2048, ray_squared_distance_limit=0x400000,
            material_durability_offset='0x104', material_hp_offset='0x158',
            cell_current_hp_offset='0x4',
            note='Version-specific static checkpoints; constructor defaults may be overridden by owners/XML.')
        (output / 'explosion-native-checkpoints.json').write_text(json.dumps(explosion_checkpoints, indent=2) + '\n', encoding='utf-8')
        # The packed little-endian initializer sets blob radii 0/0 and restrictions 1/1.
        defaults_bytes = pe.data[pe.raw(0x4ac739 - pe.base):pe.raw(0x4ac739 - pe.base) + 7]
        if defaults_bytes != bytes.fromhex('c7 46 2c 00 00 01 01'):
            raise ValueError('Reaction defaults checkpoint no longer matches')
        corrosion_checkpoints = dict(
            executable_sha256=hashes['noita.exe'],
            reaction_blob_default_write_va='0x4ac739', initializer_hex=defaults_bytes.hex(),
            reaction_blob_radius_offsets=['0x2c', '0x2d'],
            reaction_blob_restriction_offsets=['0x2e', '0x2f'],
            material_lifetime_offset='0x154', gas_lifetime_counter_offset='0x34',
            gas_constructor_lifetime_variation_coefficient=struct.unpack_from(
                '<f', pe.data, pe.raw(0x1053578 - pe.base))[0],
            gas_counter_increment_va='0x70ab61',
            note='Static control/data-flow checkpoints; caller scheduling and all counter reset paths are not recovered.')
        (output / 'corrosion-native-checkpoints.json').write_text(json.dumps(corrosion_checkpoints, indent=2) + '\n', encoding='utf-8')
        phase_defaults_at = pe.raw(0x4aca07 - pe.base)
        phase_defaults = struct.unpack_from('<hh', pe.data, phase_defaults_at + 6)
        temperature_checkpoints = dict(
            executable_sha256=hashes['noita.exe'],
            material_constructor_phase_default_write_va='0x4aca07',
            generic_cold_warm_chance_rev_defaults=phase_defaults,
            biome_world_member_offset='0x440',
            biome_modifier_field_offsets={'reaction_freeze_chance': '0x12',
                                         'reaction_unfreeze_chance': '0x13'},
            world_modifier_field_offsets={'reaction_freeze_chance': '0x452',
                                         'reaction_unfreeze_chance': '0x453'},
            magic_convert_field_offsets={'radius': '0x48', 'min_radius': '0x4c',
                                         'is_circle': '0x50', 'steps_per_frame': '0x54',
                                         'fan_the_flames': '0x80', 'temperature_reaction_temp': '0x84',
                                         'ignite_materials': '0x88', 'mRadius': '0xe4'},
            liquid_freeze_predicate_zero_constant=struct.unpack_from(
                '<f', pe.data, pe.raw(0x10533f4 - pe.base))[0],
            world_temperature_transform_rva='0x31aa60',
            note='Version-specific static checkpoints; temperature argument is a signed probability threshold in this function, not an accumulated per-cell temperature.')
        (output / 'temperature-native-checkpoints.json').write_text(json.dumps(temperature_checkpoints, indent=2) + '\n', encoding='utf-8')
        temperature_types = ['BiomeModifiers', 'MagicConvertMaterialSystem', 'MagicConvertMaterialComponent']
        temperature_rtti = {name: pe.hierarchy(f'.?AV{name}@@') for name in temperature_types}
        (output / 'temperature-rtti.json').write_text(json.dumps(temperature_rtti, indent=2) + '\n', encoding='utf-8')
    print('Materials:', len(nodes), 'combustion field names:', len(fields), 'reactions:', len(rules))
    print('Fire/lava input reaction records:', len(fire_rules), 'entity/script files matching explicit controls:', len(references))
    print('Component sections:', ', '.join(sections))
    print('Interaction reference files:', len(interaction_references),
          'interaction component sections:', len(interaction_sections))
    print('Explosion reference files:', len(explosion_references))
    print('Acid-related source reactions:', len(acid_rules), 'mana-input source reactions:', len(mana_rules))
    print('Phase material fields:', phase['field_material_counts'], 'temperature reference files:', len(temperature_references))
    print('Stain material records:', len(stain_records), 'stain reference files:', len(stain_references))
    print('Evidence:', output)


if __name__ == '__main__':
    main()
