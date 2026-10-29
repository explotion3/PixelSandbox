"""Read-only import from local Noita. Raw evidence and prototype tuning stay separate.
Movement formulas are not recovered by this importer. Supported reaction
options follow the versioned local analysis; scheduling remains project-owned.
"""
import argparse
import hashlib
import json
import re
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SELECTED = [
    ('stone','rock_static','Stone'), ('sand','sand','Sand'),
    ('sand_static','sand_static','Static sand'), ('water','water','Water'),
    ('water_static','water_static','Static water'), ('oil','oil','Oil'),
    ('wood','wood_static','Wood'), ('steam','steam','Steam'),
    ('smoke','smoke','Smoke'), ('fire','fire','Fire'),
    ('ice','ice_static','Ice'), ('lava','lava','Lava'),
    ('acid','acid','Acid'), ('acid_gas','acid_gas','Acid gas'),
    ('snow','snow','Snow'), ('salt','salt','Salt'),
    ('salt_water','water_salt','Salt water'), ('sludge','radioactive_liquid','Toxic sludge'),
    ('blood','blood','Blood'), ('soil','soil','Soil'), ('mud','mud','Mud'),
    ('coal','coal','Coal'), ('glue','glue','Glue'), ('cement','cement','Cement'),
    ('concrete','concrete_static','Concrete'), ('steel','steel_static','Steel'),
    ('metal_sand','metal_sand','Metal powder'), ('alcohol','alcohol','Alcohol'),
    ('alcohol_gas','alcohol_gas','Alcohol gas'),
    ('ice_blood','ice_blood_static','Frozen blood'),
    ('ice_acid','ice_acid_static','Frozen acid'),
    ('ice_sludge','ice_radioactive_static','Frozen sludge'),
    ('slime','slime','Slime'), ('ice_slime','ice_slime_static','Frozen slime'),
    ('cloud_slime','cloud_slime','Slime vapour'),
    ('gunpowder','gunpowder','Gunpowder'),
    ('explosive_powder','gunpowder_explosive','Explosive powder'),
    ('copper','copper','Copper'), ('brass','brass','Brass'), ('silver','silver','Silver'),
    ('glowrock','rock_static_glow','Glowing rock'),
    ('lavarock','lavarock_static','Lava rock'), ('chilly_water','water_ice','Chilly water'),
    ('copper_molten','copper_molten','Molten copper'), ('brass_molten','brass_molten','Molten brass'),
    ('silver_molten','silver_molten','Molten silver'), ('metal_sand_molten','metal_sand_molten','Molten metal'),
    ('wax','wax','Wax'), ('wax_molten','wax_molten','Molten wax'),
    ('gold','gold','Gold'), ('gold_molten','gold_molten','Molten gold'),
    ('steel_sand','steel_sand','Steel powder'), ('steel_molten','steel_molten','Molten steel'),
    ('liquid_fire','liquid_fire','Burning liquid'),
    ('wood_prop','wood_prop','Rigid wood'),
    ('tnt','tnt','TNT'),
    ('acid_gas_static','acid_gas_static','Static acid gas'),
    ('mana','magic_liquid_mana_regeneration','Mana potion'),
    ('glass','glass','Glass body'), ('glass_molten','glass_molten','Molten glass'),
]

def combustion_fields(attrs, mapping):
    conversion=attrs.get('on_fire_convert_to_material','')
    if conversion and conversion!='air' and conversion not in mapping:
        raise ValueError('Unsupported ignition conversion target: '+conversion)
    return {
        'source_burning':True,'burnable':attrs.get('burnable','0')=='1',
        'fire_hp':int(attrs.get('fire_hp','100')),
        'generates_flames':int(attrs.get('generates_flames','100')),
        'generates_smoke':int(attrs.get('generates_smoke','0')),
        'ignition_output':mapping.get(conversion,conversion),
        'initially_burning':attrs.get('on_fire','0')=='1',
    }

def combustion_reference():
    return {
        'analysis':'docs/Noita原版/材料、地形与物理.md#9',
        'project_fallbacks':{'burnable':False,'on_fire':False,'fire_hp':100,'autoignition_temperature':85,
                            'temperature_of_fire':100,'generates_flames':100,'generates_smoke':0,'requires_oxygen':True,
                            'on_fire_convert_to_material':'','on_fire_flame_material':'fire','on_fire_smoke_material':'smoke'},
        'prototype_notes':'Missing attributes use explicitly project-owned fallbacks. Per-cell/tick deterministic 15-bit LCG streams and fixed-step scheduling differ from the unrecovered world scheduler. Source signed intensity byte, sampled ignition, single upper emission slot, flame-placement feedback and separate liquid/body burnout branches are supported. Body global-counter checks map to fixed tick. Gas countdown and fire lifetime remain project approximations; target-type predicates for scorching are not fully recovered.'
    }

def adhesion_fields(attrs):
    return {'source_adhesion':attrs.get('cell_type','liquid')=='liquid',
            'liquid_sticks_to_ceiling':int(attrs.get('liquid_sticks_to_ceiling','0'))}

def liquid_fields(attrs):
    ordinary=(attrs.get('cell_type','liquid')=='liquid' and
              all(attrs.get(k,'0')!='1' for k in ('liquid_sand','liquid_slime','liquid_static')))
    return {'source_liquid':ordinary,
            'liquid_gravity':float(attrs.get('liquid_gravity','0.5')),
            'liquid_damping':float(attrs.get('liquid_damping','0.8')),
            'liquid_flow_speed':float(attrs.get('liquid_flow_speed','0.9'))}

def sync_liquids(root,xml):
    path=ROOT/'game/assets/prototype_noita/source-materials.json'
    manifest=json.loads(path.read_text(encoding='utf-8'))
    if manifest['materials_xml_sha256']!=hashlib.sha256(xml).hexdigest():
        raise ValueError('materials.xml changed; review source references first')
    nodes={e.get('name'):e for e in root if e.get('name')}
    def inherited(name):
        node=nodes[name]
        return (inherited(node.get('_parent')) if node.get('_parent') else {})|node.attrib
    count=0
    for material in manifest['materials']:
        attrs=inherited(material['source_id'])
        if attrs!=material['inherited_attributes']:raise ValueError('Changed source evidence: '+material['id'])
        updates=liquid_fields(attrs)
        if not updates['source_liquid']:continue
        target=ROOT/'game/content/materials'/(material['id'].split('.',1)[1]+'.tres')
        text=target.read_text(encoding='utf-8-sig')
        for key,value in updates.items():
            line=key+' = '+json.dumps(value)
            if re.search(r'^'+key+r'\s*=',text,re.M):text=re.sub(r'^'+key+r'\s*=.*$',line,text,flags=re.M)
            else:text+=line+'\n'
        target.write_text(text,encoding='utf-8');material['prototype_fields'].update(updates);count+=1
    manifest['liquid_motion_reference']={
        'analysis':'docs/Noita原版/材料、地形与物理.md#16',
        'constructor_defaults':{'liquid_gravity':0.5,'liquid_damping':0.8,'liquid_flow_speed':0.9},
        'scope':'Ordinary non-static, non-sand, non-slime liquids only. Recovered fall/lateral accumulators and density gate; fixed-step ordering, extra-fall lateral selection, impulses and rigid displacement remain project adaptations. No pressure projection or global outlet search in this branch. Slime/sand motion is unchanged.'}
    path.write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('Synchronized ordinary liquid motion:',count,'definitions; no textures changed')

def sync_adhesion(root, xml):
    """Synchronize only the recovered adhesion mode and raw source threshold."""
    path=ROOT/'game/assets/prototype_noita/source-materials.json'
    manifest=json.loads(path.read_text(encoding='utf-8'))
    if manifest['materials_xml_sha256']!=hashlib.sha256(xml).hexdigest():
        raise ValueError('materials.xml changed; review source references first')
    nodes={e.get('name'):e for e in root if e.get('name')}
    def inherited(name):
        node=nodes[name]
        attrs=inherited(node.get('_parent')) if node.get('_parent') else {}
        return attrs|node.attrib
    for material in manifest['materials']:
        attrs=inherited(material['source_id'])
        if attrs!=material['inherited_attributes']:
            raise ValueError('Preserve/review changed material evidence: '+material['id'])
        updates=adhesion_fields(attrs)
        target=ROOT/'game/content/materials'/(material['id'].split('.',1)[1]+'.tres')
        text=target.read_text(encoding='utf-8-sig')
        for key,value in updates.items():
            line=key+' = '+json.dumps(value)
            if re.search(r'^'+key+r'\s*=',text,re.M):text=re.sub(r'^'+key+r'\s*=.*$',line,text,flags=re.M)
            else:text+=line+'\n'
        target.write_text(text,encoding='utf-8')
        material['prototype_fields'].update(updates)
    manifest['adhesion_reference']={
        'analysis':'docs/Noita原版/材料、地形与物理.md#15',
        'known_default_liquid_sticks_to_ceiling':0,
        'notes':'Raw source threshold, coordinate-selected top/side attachment, Random15%101<20 maintenance gate, support loss and empty-below release. Existing strength/span/duration fields are retained for old authored/save compatibility but ignored in source mode. Fixed-step schedule, per-cell random stream and mapping of represented support states are project integration; full sand movement states remain unrecovered.'}
    path.write_text(json.dumps(manifest,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    print('Synchronized source adhesion for',len(manifest['materials']),'material definitions')

def sync_reactions(root, xml):
    """Upgrade exact source matches in the existing curated table only."""
    manifest_path=ROOT/'game/assets/prototype_noita/source-materials.json'
    manifest=json.loads(manifest_path.read_text(encoding='utf-8'))
    digest=hashlib.sha256(xml).hexdigest()
    if manifest['materials_xml_sha256']!=digest:
        raise ValueError('materials.xml changed; current material references must be reviewed first')
    names={m['id']:m['source_id'] for m in manifest['materials']}
    def source(value): return names.get(value,value)
    records=list(root.iter('Reaction'))
    path=ROOT/'game/content/reactions.txt';updated=[];matched=[];project=[]
    for line_number,line in enumerate(path.read_text(encoding='utf-8-sig').splitlines(),1):
        if not line.strip() or line.lstrip().startswith('#'): updated.append(line);continue
        fields=[v.strip() for v in line.split(';')];end=next((i for i,v in enumerate(fields) if '=' in v),len(fields))
        inputs=[source(fields[0]),source(fields[1]),source(fields[8]) if end==10 else '']
        outputs=[source(fields[2]),source(fields[3]),source(fields[9]) if end==10 else 'keep']
        matches=[]
        for index,node in enumerate(records,1):
            attrs=node.attrib
            for reverse in (False,True):
                order=[1,0,2] if reverse else [0,1,2]
                ins=[inputs[i] for i in order];outs=[outputs[i] for i in order]
                if any(ins[i]!=attrs.get('input_cell'+str(i+1),'') for i in range(3)):continue
                if all((outs[i]=='keep' and (attrs.get('output_cell'+str(i+1)) in (None,ins[i]))) or
                       outs[i]==attrs.get('output_cell'+str(i+1)) for i in range(3)):
                    matches.append((index,node,reverse));break
        if len(matches)!=1: updated.append(line);project.append(line_number);continue
        index,node,reverse=matches[0];attrs=node.attrib
        if reverse:fields[0],fields[1],fields[2],fields[3]=fields[1],fields[0],fields[3],fields[2]
        if 'req_lifetime' in attrs:
            while end<6:fields.insert(end,'0');end+=1
            fields[5]=attrs['req_lifetime']
        # Rebuild supported options from source data; absent restriction flags
        # keep the explicitly documented project fallback, not an inferred 0.
        options=['probability='+attrs['probability'],'fast='+attrs.get('fast_reaction','0')]
        if 'direction' in attrs:options.append('direction='+attrs['direction'])
        if 'destroy_horizontally_lonely_pixels' in attrs:options.append('destroy_lonely='+attrs['destroy_horizontally_lonely_pixels'])
        for slot,suffix in ((1,'a'),(2,'b')):
            radius=attrs.get('blob_radius'+str(slot))
            if radius is not None:
                options.extend(['blob_radius_'+suffix+'='+radius,'restrict_'+suffix+'='+attrs.get('blob_restrict_to_input_material'+str(slot),'1')])
        updated.append(';'.join(fields[:end]+options))
        matched.append({'line':line_number,'source_index':index,'source_attributes':attrs})
    path.write_text('\n'.join(updated)+'\n',encoding='utf-8')
    manifest['reaction_dispatch_reference']={
        'materials_xml_sha256':digest,'source_matches':matched,'project_rule_lines':project,
        'prototype_notes':'Exact supported source matches use shared 15-bit samples, probability*100 gates, oriented cardinal/third-neighbor selection and direct-world radial/strict-disk geometry. Missing blob restriction flags retain recovered constructor true. One ordinary and one fast attempt per active cell/fixed tick, source A anchoring, dirty wake scheduling and atomic C/B/A staged conflict handling are project choices. Unmatched curated recipes retain project sampling. ReqReaction is supported without importing new vegetation; threaded request consumption, environment bytes, convert_all and entity/audio effects remain unrecovered.'
    }
    manifest['reaction_dispatch_reference']['project_fallbacks']={'fast_reaction':0,'direction':'any','req_lifetime':0}
    manifest['reaction_dispatch_reference']['known_defaults']={'blob_radius':[0,0],'blob_restrict_to_input_material':[True,True]}
    manifest['reaction_notes']='Curated subset: exact matches use source probability*100 gates; unmatched rules keep project denominators. Invocation frequency, source-A ownership and commit semantics remain project choices.'
    manifest['restricted_blob_reference']['prototype_notes']='Source records use restricted radial sampling or an unrestricted strict disk; missing restriction flags retain recovered constructor true. Authored legacy records retain connected disks. Float trigonometry, runtime precision, scheduling and atomic multi-output conflict handling are not claimed to reproduce the entire original world pipeline.'
    manifest_path.write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(f'Synchronized {len(matched)} exact source recipes; retained {len(project)} project recipes. No material or texture import.')

def sync_interactions(root,xml,read):
    """Update only newly supported fields; preserve existing author tuning and textures."""
    manifest_path=ROOT/'game/assets/prototype_noita/source-materials.json'
    manifest=json.loads(manifest_path.read_text(encoding='utf-8'))
    if manifest['materials_xml_sha256']!=hashlib.sha256(xml).hexdigest():
        raise ValueError('materials.xml changed; review provenance before syncing interactions')
    target=ROOT/'game/content/materials';nodes={n.get('name'):n for n in root if n.get('name')}
    def inherited(name):
        chain=[];node=nodes[name]
        while node is not None:chain.append(node);node=nodes.get(node.get('_parent'))
        attrs={};gfx=None
        for node in reversed(chain):
            attrs.update(node.attrib)
            if node.find('Graphics') is not None:gfx=node.find('Graphics')
        return attrs,gfx
    additions=[('acid_gas_static','acid_gas_static','Static acid gas','material.acid_gas'),
               ('mana','magic_liquid_mana_regeneration','Mana potion','material.water'),
               ('glass','glass','Glass body','material.stone'),
               ('glass_molten','glass_molten','Molten glass','material.copper_molten')]
    mapping={m['source_id']:m['id'] for m in manifest['materials']}
    mapping.update({source:'material.'+short for short,source,_,_ in additions})
    for short,source,display,base in additions:
        if source in {m['source_id'] for m in manifest['materials']}:continue
        attrs,gfx=inherited(source);fields=dict(next(m['prototype_fields'] for m in manifest['materials'] if m['id']==base))
        fields.update({'content_id':'material.'+short,'source_id':source,'display_name':display,
            'cell_type':attrs.get('cell_type','liquid'),'sand':attrs.get('liquid_sand','0')=='1','is_static':attrs.get('liquid_static','0')=='1',
            'density':float(attrs.get('density','1')),'tags':','.join(re.findall(r'\[([^\]]+)\]',attrs.get('tags',''))),
            'color':int(gfx.get('color',attrs.get('wang_color','ff888888')) if gfx is not None else attrs.get('wang_color','ff888888'),16),
            'collectible':False,'can_loosen':False,'slime':False,'adhesion_strength_q8':0,'adhesion_span':1,'adhesion_hold_ticks':0,
            'lifetime':int(attrs.get('lifetime','0')),'fuel':max(0,int(attrs.get('fire_hp','100'))) if attrs.get('burnable')=='1' else 0,
            'ignition':int(attrs.get('autoignition_temperature','85')),'requires_oxygen':attrs.get('requires_oxygen','1')=='1',
            'burn_heat':int(attrs.get('temperature_of_fire','100')),'heat':0,'cooling':0,
            'warmth_output':mapping.get(attrs.get('warmth_melts_to_material',''),''),'cold_output':mapping.get(attrs.get('cold_freezes_to_material',''),''),
            'durability':int(attrs.get('durability','0')),'hit_points':int(attrs.get('hp','100')),
            'electrical_conductivity':attrs.get('electrical_conductivity','1' if attrs.get('cell_type','liquid')=='liquid' and attrs.get('liquid_sand','0')!='1' else '0')=='1',
            'gravity_q8':256 if attrs.get('cell_type')=='gas' else round(float(attrs.get('liquid_gravity','1'))*256),
            'explosion_radius':0,'explosion_source_rules':True})
        fields.update(combustion_fields(attrs,mapping));fields.update(adhesion_fields(attrs));fields.update(liquid_fields(attrs));fields['texture_path']=''
        texture=gfx.get('texture_file','') if gfx is not None else ''
        if texture:
            data=read(texture);filename=Path(texture).name;out=ROOT/'game/assets/prototype_noita'/filename
            if out.exists() and out.read_bytes()!=data:raise ValueError('Preserve modified texture: '+str(out))
            out.write_bytes(data);fields['texture_path']='res://assets/prototype_noita/'+filename
            if not any(a['entry']==texture for a in manifest['assets']):manifest['assets'].append({'entry':texture,'output':filename,'sha256':hashlib.sha256(data).hexdigest()})
        path=target/(short+'.tres')
        if path.exists():raise ValueError('Preserve unregistered user material: '+str(path))
        path.write_text('[gd_resource type="SandboxMaterialDefinition" format=3]\n\n[resource]\n'+'\n'.join(k+' = '+json.dumps(v,ensure_ascii=False) for k,v in fields.items())+'\n',encoding='utf-8')
        manifest['materials'].append({'id':'material.'+short,'source_id':source,'inherited_attributes':attrs,'inherited_explosion':{},'prototype_fields':fields})
        catalog=target/'catalog.txt';lines=catalog.read_text(encoding='utf-8-sig').splitlines();lines.append('res://content/materials/'+short+'.tres');catalog.write_text('\n'.join(lines)+'\n',encoding='utf-8')
    for material in manifest['materials']:
        attrs=material['inherited_attributes'];updates={'source_phase':True,'source_gas':attrs.get('cell_type')=='gas','source_electricity':True,
            'cold_chance_rev':int(attrs.get('cold_freezes_chance_rev','100')),'warmth_chance_rev':int(attrs.get('warmth_melts_chance_rev','100')),
            'explosion_source_rules':True,'explosion_create_probability':int(material['inherited_explosion'].get('create_cell_probability','5'))}
        path=target/(material['id'].split('.',1)[1]+'.tres');text=path.read_text(encoding='utf-8-sig')
        for key,value in updates.items():
            line=key+' = '+json.dumps(value)
            if re.search(r'^'+key+r'\s*=',text,re.M):text=re.sub(r'^'+key+r'\s*=.*$',line,text,flags=re.M)
            else:text+=line+'\n'
        path.write_text(text,encoding='utf-8');material['prototype_fields'].update(updates)
    # Add only concrete, supported recipes from the evidence; no unrelated materials.
    wanted={8,14,54,55,100,101,190,208,209,212,280};path=ROOT/'game/content/reactions.txt';lines=path.read_text(encoding='utf-8-sig').splitlines()
    added=[]
    for i,node in enumerate(root.iter('Reaction'),1):
        if i not in wanted:continue
        a=node.attrib
        def translated(value):return mapping.get(value,value)
        fields=[translated(a['input_cell1']),translated(a['input_cell2']),translated(a.get('output_cell1','keep')),translated(a.get('output_cell2','keep')),'1']
        # Matching source indices are retained for idempotent selective synchronization.
        marker='# source interaction '+str(i)
        if marker in lines:continue
        lines.extend([marker,';'.join(fields)]);added.append(i)
    path.write_text('\n'.join(lines)+'\n',encoding='utf-8')
    manifest['phase_reference']={'default_reverse_chances':[100,100],'environment_defaults':[0,0],
        'notes':'Source T is a signed random threshold, separate from project stored heat. Liquid blocker maps to project motion state; original state byte/float writers remain unknown. Existing stable cold/warm target mappings are retained; full original reverse-target overwrite order is not claimed.'}
    manifest['source_interaction_reference']={'added_source_recipes':sorted(wanted),'gas_notes':'Signed +/-30% initial counter and increment on actual swaps; no unconditional tick increment. Other original writers remain unknown. Existing fire lifetime and gas movement remain project models.',
        'electric_notes':'Brush parameters: energy1000 speed32 heat0.1 splitting count2 threshold500 glow10; tick maps source time, unknown cell marker false, no environment exception. Sources/receivers and crack components not instantiated.',
        'explosion_notes':'360 grid probes read persistent HP, strict radius and source creation gate. Angle binning, body geometry/pressure and fuse aggregation remain project rules; unknown sine edge modifiers, sparks and delay queues are not imported.'}
    manifest_path.write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    sync_reactions(root,xml)
    manifest=json.loads(manifest_path.read_text(encoding='utf-8'));ref=manifest['reaction_dispatch_reference']
    ref['known_defaults']={'blob_radius':[0,0],'blob_restrict_to_input_material':[True,True]}
    ref['project_fallbacks'].pop('blob_restrict_to_input_material',None)
    manifest_path.write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('Synchronized source interactions for',len(manifest['materials']),'materials; added recipes',added)

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--source',type=Path,default=Path(r'E:\SteamLibrary\steamapps\common\Noita'))
    parser.add_argument('--sync-reactions-only',action='store_true',help='Update exact existing recipe matches without rewriting material resources or textures')
    parser.add_argument('--sync-interactions-only',action='store_true',help='Sync supported phase/electric/blast fields and acid/mana definitions without replacing existing tuning')
    parser.add_argument('--sync-liquids-only',action='store_true',help='Sync ordinary liquid motion parameters without replacing other fields or textures')
    parser.add_argument('--sync-adhesion-only',action='store_true',help='Sync recovered adhesion threshold without rewriting textures or other fields')
    args=parser.parse_args()
    archive=args.source/'data/data.wak'
    destination=ROOT/'game/assets/prototype_noita'
    target=ROOT/'game/content/materials'
    destination.mkdir(parents=True,exist_ok=True)
    mapping={source:'material.'+short for short,source,_ in SELECTED}
    with archive.open('rb') as stream:
        _,count,table_end,_=struct.unpack('<4I',stream.read(16))
        archive_size=archive.stat().st_size
        if count>100000 or table_end>archive_size: raise ValueError('Invalid WAK header')
        entries={}
        for _ in range(count):
            offset,size,length=struct.unpack('<3I',stream.read(12))
            if length>1024 or stream.tell()+length>table_end: raise ValueError('Invalid WAK name')
            name=stream.read(length).decode()
            if offset<table_end or offset+size>archive_size: raise ValueError('Invalid WAK range')
            entries[name]=(offset,size)
        def read(name):
            offset,size=entries[name]
            stream.seek(offset)
            return stream.read(size)
        xml=read('data/materials.xml')
        root=ET.fromstring(xml)
        if args.sync_liquids_only:
            sync_liquids(root,xml)
            return
        if args.sync_adhesion_only:
            sync_adhesion(root,xml)
            return
        if args.sync_interactions_only:
            sync_interactions(root,xml,read)
            return
        if args.sync_reactions_only:
            sync_reactions(root,xml)
            return
        nodes={e.get('name'):e for e in root if e.get('name')}
        manifest={
            'archive_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),
            'materials_xml_sha256':hashlib.sha256(xml).hexdigest(),
            'source_install':str(args.source),'purpose':'Local prototype reference / Noita, Nolla Games',
            'materials':[],'assets':[],
            'reaction_notes':'reactions.txt is a curated supported subset; exact source matches and project scheduling are distinguished by sync_reactions.'
        }
        electric_xml=read('data/entities/misc/electricity.xml')
        manifest['electricity_reference']={
            'entry':'data/entities/misc/electricity.xml','sha256':hashlib.sha256(electric_xml).hexdigest(),
            'declared_attributes':ET.fromstring(electric_xml).find('ElectricityComponent').attrib,
            'component_documentation':'tools_modding/component_documentation.txt',
            'documented_defaults':{'energy':1000,'speed':32},
            'prototype_notes':'Pulse branching, conductor defaults, costs, afterglow and heat conversion are project rules; spark_electric is not treated as an electricity source.'
        }
        manifest['restricted_blob_reference']={
            'source_fields':['blob_radius1','blob_radius2','blob_restrict_to_input_material1','blob_restrict_to_input_material2'],
            'prototype_notes':'Explicitly restricted source blobs are supported. Each anchor expands through its exact material inside a bounded disk; geometry, connectivity, overlap ordering and sampling remain project rules. Unrestricted source blobs are not yet reproduced.'
        }
        manifest['phase_reference']={
            'selectors':'[meltable]_molten and [meltable_metal]_molten bind source_id families, not project display names.',
            'prototype_notes':'Source fire_hp is a signed definition threshold, not a shared fuel countdown: imported liquid-class cells use sampled burnout into flame output and rigid pixels count burning updates. on_fire_convert_to_material acts at ignition entry. Gas budget, lifetime, update frequency and thermal bridges remain project rules. Cold/water quench recipes retain source outputs.'
        }
        manifest['combustion_reference']=combustion_reference()
        manifest['body_explosion_reference']={
            'source_material':'tnt',
            'prototype_notes':'Source solid type, density, durability, HP, fuel, minimum radius and ray energy are retained. Four-tick fuse, smoke residue, heat, area impulse, inertia response, ray sampling, cluster growth and pressure-unit mapping remain project rules; this does not import an explosive-crate entity or recover private engine formulas.'
        }
        manifest['body_contact_reference']={
            'source_field':'solid_friction',
            'prototype_fallback':0.6,
            'prototype_notes':'Explicit inherited solid_friction is retained; omitted values use the project fallback, not a recovered original default. Mapping to Box2D shape friction and its default geometric-mean mixing is a project integration; original contact/float formulas remain unverified.'
        }
        manifest['liquid_flow_reference']={
            'prototype_field':'flow_search',
            'prototype_notes':'Local probe budget, not physical reach. Connected own-liquid columns and finite-row fallback share nearest downward outlet queries. Movement remains cell-by-cell; cache, row wake scope and velocity units are project rules, not recovered Noita formulas.'
        }
        paths=[]
        textures=set()
        for short,source,display in SELECTED:
            chain=[]
            node=nodes[source]
            while node is not None:
                chain.append(node)
                node=nodes.get(node.get('_parent'))
            attrs={}
            gfx=None
            explosion={}
            for node in reversed(chain):
                attrs.update(node.attrib)
                if node.find('Graphics') is not None: gfx=node.find('Graphics')
                if node.find('ExplosionConfig') is not None: explosion.update(node.find('ExplosionConfig').attrib)
            texture=gfx.get('texture_file','') if gfx is not None else ''
            texture_path=''
            if texture and texture in entries:
                filename=Path(texture).name
                data=read(texture)
                if not data.startswith(b'\x89PNG\r\n\x1a\n'): raise ValueError('Invalid PNG '+texture)
                (destination/filename).write_bytes(data)
                texture_path='res://assets/prototype_noita/'+filename
                if texture not in textures:
                    manifest['assets'].append({'entry':texture,'output':filename,'sha256':hashlib.sha256(data).hexdigest()})
                    textures.add(texture)
            tags=re.findall(r'\[([^\]]+)\]',attrs.get('tags',''))
            if short=='water_static': tags+=['water','liquid']
            color=int(gfx.get('color',attrs.get('wang_color','ff888888')) if gfx is not None else attrs.get('wang_color','ff888888'),16)
            viscosity=float(attrs.get('liquid_viscosity','0'))
            interval=max(1,min(20,1+int(viscosity)//10))
            gas=attrs.get('cell_type') in ['gas','fire']
            slime=attrs.get('liquid_slime','0')=='1'
            ceiling=int(attrs.get('liquid_sticks_to_ceiling','0'))
            warm=mapping.get(attrs.get('warmth_melts_to_material',''),'')
            cold=mapping.get(attrs.get('cold_freezes_to_material',''),'')
            if short=='water_static': warm='material.steam'
            fields={
                'content_id':'material.'+short,'display_name':display,'hardness':8 if short in ['stone','steel'] else 1,
                'source_id':source,'cell_type':attrs.get('cell_type','liquid'),
                'sand':attrs.get('liquid_sand','0')=='1','is_static':attrs.get('liquid_static','0')=='1',
                'density':float(attrs.get('density','1')),'solid_friction':float(attrs.get('solid_friction','0.6')),'color':color,'texture_path':texture_path,
                'tags':','.join(dict.fromkeys(tags)),'collectible':short=='stone',
                'cooling':40 if short in ['water','water_static','salt_water'] else 0,
                'warmth_threshold':60 if warm=='material.steam' else 30 if warm else 0,
                'warmth_output':warm,'cold_output':cold,'cold_threshold':-10 if cold else -100,
                'lifetime':90 if source=='fire' else min(60000,int(attrs.get('lifetime','0'))),
                'fuel':max(0,int(attrs.get('fire_hp','100'))) if attrs.get('burnable')=='1' else 0,
                'initially_burning':attrs.get('on_fire')=='1' and attrs.get('burnable')=='1',
                'ignition':max(0,min(100,int(float(attrs.get('autoignition_temperature','85'))))),
                'heat':int(attrs.get('temperature_of_fire','0')) if source in ['fire','lava'] else 0,
                'flow_interval':interval,
                'gravity_q8':256 if gas else max(0,min(2048,round(float(attrs.get('liquid_gravity','1'))*256))),
                'damping_milli':max(0,min(1000,round(float(attrs.get('liquid_damping','0.9'))*1000))),
                'lateral_speed_q8':max(1,min(2048,round(float(attrs.get('liquid_flow_speed','0.25' if slime else '1'))*256))),
                'flow_search':64,'terminal_speed_q8':2048,
                'powder_liquid_speed_q8':102, # Project tuning, not a recovered Noita field.
                # Direction caps are prototype tuning. Selected source definitions omit gas_* speeds.
                'gas_upward_speed_q8':256,'gas_horizontal_speed_q8':256,'gas_downward_speed_q8':2048,
                # Mechanical phases, force thresholds, spans and hold times are project rules.
                'can_loosen':attrs.get('liquid_static','0')=='1' and attrs.get('liquid_sand','0')=='1' and attrs.get('cell_type','liquid')=='liquid',
                'settle_ticks':30,'slime':slime,
                'adhesion_strength_q8':min(8192,ceiling*8),
                'adhesion_span':max(1,min(16,(ceiling+9)//10)) if slime else 1,
                'adhesion_hold_ticks':0 if ceiling>=100 else ceiling*6,
                'durability':int(attrs.get('durability','0')),'hit_points':int(attrs.get('hp','100')),
                'explosion_radius':min(48,int(explosion.get('explosion_radius',explosion.get('cell_explosion_radius_min','8')))) if explosion else 0,
                'explosion_ray_energy':int(explosion.get('ray_energy','400000')),
                'explosion_max_durability':int(explosion.get('max_durability_to_destroy','11')),
                'explosion_heat':120,'explosion_impulse_q8':1536,'explosion_delay_ticks':4,
                'explosion_trigger_energy':int(explosion.get('cell_explosion_damage_required','20'))*1000,
                'explosion_hole_enabled':explosion.get('hole_enabled','1')=='1',
                'explosion_destroy_liquids':explosion.get('hole_destroy_liquid','0')=='1',
                'explosion_output':'material.smoke','explosion_fire_material':'material.fire',
                'electrical_conductivity':attrs.get('electrical_conductivity','1' if attrs.get('cell_type','liquid')=='liquid' and attrs.get('liquid_sand','0')!='1' else '0')=='1',
                'electric_cost':1 if attrs.get('liquid_sand','0')=='1' else 4,
                'requires_oxygen':attrs.get('requires_oxygen','1')=='1',
                'flame_output':mapping.get(attrs.get('on_fire_flame_material',''),'material.fire'),
                'smoke_output':mapping.get(attrs.get('on_fire_smoke_material',''),'material.smoke'),
                'burn_output':mapping.get(attrs.get('on_fire_convert_to_material',''),'material.'+short if 'molten' in source else 'material.smoke'),
                'burn_heat':int(attrs.get('temperature_of_fire','100')) if attrs.get('burnable')=='1' else 100,
            }
            fields.update(combustion_fields(attrs,mapping))
            fields.update(adhesion_fields(attrs))
            def value(v):
                if isinstance(v,bool): return 'true' if v else 'false'
                return json.dumps(v,ensure_ascii=False)
            (target/(short+'.tres')).write_text('[gd_resource type="SandboxMaterialDefinition" format=3]\n\n[resource]\n'+'\n'.join(k+' = '+value(v) for k,v in fields.items())+'\n',encoding='utf-8')
            paths.append('res://content/materials/'+short+'.tres')
            manifest['materials'].append({'id':'material.'+short,'source_id':source,'inherited_attributes':attrs,'inherited_explosion':explosion,'prototype_fields':fields})
        # Keep separately authored extensions already present in the catalog.
        catalog=target/'catalog.txt'
        if catalog.exists():
            for path in catalog.read_text(encoding='utf-8-sig').splitlines():
                if path.strip() and path.strip() not in paths: paths.append(path.strip())
        catalog.write_text('\n'.join(paths)+'\n',encoding='utf-8')
        manifest['source_contact_reactions']=[e.attrib for e in root.iter('Reaction') if any(e.get(k) in mapping for k in ['input_cell1','input_cell2'])]
        (destination/'source-materials.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        sync_interactions(root,xml,read)
    print(f'Imported {len(SELECTED)} original definitions, {len(textures)} textures; retained {len(paths)-len(SELECTED)} custom catalog entries.')
if __name__=='__main__': main()
