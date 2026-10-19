#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate handoff links, current source controls, IDs and offline resources."""
from pathlib import Path
import re,json,csv,xml.etree.ElementTree as ET
from urllib.parse import unquote
root=Path(__file__).resolve().parents[1]
project=root.parents[1]
errors=[];count=0
for p in root.rglob('*.md'):
 for target in re.findall(r'!?\[[^\]]*\]\(([^)]+)\)',p.read_text()):
  if re.match(r'^[a-z]+:',target):continue
  target=unquote(target.split('#')[0].split('?')[0]).strip('<>')
  if target and not (p.parent/target).exists():errors.append(f'Broken link: {p.name} → {target}')
  count+=1
inventory=json.loads((root/'附录/功能控件映射.json').read_text())
controls=inventory['controls']
assert len(controls)==inventory['total']==106
assert len({(c['source'],c['control']) for c in controls})==106
for c in controls:
 p=project/c['source']
 if not p.exists():errors.append(f'Missing source: {c["source"]}');continue
 tree=ET.parse(p)
 if not tree.findall(f'.//*[@name="{c["control"]}"]'):errors.append(f'Missing control: {c["source"]}:{c["control"]}')
 if not re.fullmatch(r'S(0[1-9]|1[0-6])',c['screen_id']):errors.append(f'Invalid screen: {c["screen_id"]}')
for file,expected in [('功能控件映射.csv',106),('需求与验收映射.csv',16),('状态与验收用例.csv',20)]:
 with (root/'附录'/file).open(encoding='utf-8-sig') as f:
  if len(list(csv.DictReader(f)))!=expected:errors.append(f'Row count: {file}')
req=(root/'01_产品需求说明.md').read_text()
at=(root/'08_验收标准与测试计划.md').read_text()
for i in range(1,17):
 rid=f'REQ-{i:02}'
 if rid not in req or rid not in at:errors.append(f'Missing trace: {rid}')
html=(root/'原型/index.html').read_text()
for resource in re.findall(r'(?:src|href)="([^"]+)"',html):
 if '://' in resource:errors.append(f'Remote dependency: {resource}')
 elif not (root/'原型'/resource).exists():errors.append(f'Missing resource: {resource}')
for p in root.rglob('*'):
 if p.suffix in ['.md','.js','.cjs','.json','.css','.html','.py']:
  text=p.read_text()
  if re.search(r'/(?:Users|Volumes)/[A-Za-z][^\n\"\']+/',text):errors.append(f'Host-specific path: {p.relative_to(root)}')
assert not errors,'\n'.join(errors)
print(json.dumps({'result':'PASS','markdown_links':count,'source_controls':106,'requirements':16,'acceptance_cases':20,'offline_assets':'present'},ensure_ascii=False,indent=2))
