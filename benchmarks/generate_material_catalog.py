"""Compile the project's scalar material resources into a native benchmark fixture.

This is deliberately not a Godot Resource loader. Unsupported resource syntax
fails rather than silently benchmarking a different catalog.
"""
import json
import re
import sys
from pathlib import Path

game, destination = map(Path, sys.argv[1:])
lines = ['std::vector<pixel::MaterialDefinition> benchmark_materials() {',
         'std::vector<pixel::MaterialDefinition> result;']
for resource in (game / 'content/materials/catalog.txt').read_text(encoding='utf-8-sig').splitlines():
    resource = resource.strip()
    if not resource or resource.startswith('#'):
        continue
    if not resource.startswith('res://'):
        raise ValueError(f'Unsupported resource path: {resource}')
    lines.append('{ pixel::MaterialDefinition d;')
    in_resource = False
    for line in (game / resource[6:]).read_text(encoding='utf-8-sig').splitlines():
        line = line.strip()
        if line == '[resource]':
            in_resource = True
            continue
        if not in_resource or not line or line.startswith(';'):
            continue
        match = re.fullmatch(r'(\w+)\s*=\s*(.+)', line)
        if not match:
            raise ValueError(f'Unsupported resource line: {resource}: {line}')
        field, literal = match.groups()
        value = json.loads(literal)
        if field == 'tags':
            literal = '{' + ','.join(json.dumps(t.strip()) for t in value.split(',') if t.strip()) + '}'
        elif field in ('density', 'solid_friction', 'liquid_gravity', 'liquid_damping', 'liquid_flow_speed'):
            literal = f'static_cast<float>({literal})'
        if field == 'content_id':
            field = 'id'
        elif field.startswith('explosion_'):
            field = 'explosion.' + field[len('explosion_'):]
        lines.append(f'd.{field} = {literal};')
    lines.append('result.push_back(std::move(d)); }')
lines.append('return result; }')
destination.write_text('\n'.join(lines) + '\n', encoding='utf-8')
