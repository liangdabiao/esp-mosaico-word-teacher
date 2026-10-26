# 一次性修复：把覆盖层（设置页/错误页/状态标签/形状/下拉面板）的 z-order
# 移到对象数组末尾（后渲染=置顶）。修复词书/词条/测验页压住下拉面板的问题。
# 运行后由 GSPC 重新编译场景。
import json, io, os

here = os.path.dirname(os.path.abspath(__file__))
path = os.path.join(here, '..', 'ui', 'main.json')
d = json.load(open(path, encoding='utf-8'))
objs = d['objects']

def subtree(root):
    out = [root]
    for i, o in enumerate(objs):
        if o.get('parent') == root:
            out.extend(subtree(i))
    return out

tops = []
for i, o in enumerate(objs):
    if o.get('parent') != -1:
        continue
    name = (o.get('name') or o.get('bind') or '')
    if name in ('settings_panel', 'error_panel', 'status', 'quick_settings'):
        tops.append(i)
    elif o.get('type') == 'shape':
        tops.append(i)

moved = []
for t in tops:
    for j in subtree(t):
        if j not in moved:
            moved.append(j)

print('moving to top:', len(moved), 'objects')
rest = [o for i, o in enumerate(objs) if i not in moved]
layer = [objs[i] for i in moved]

new_objs = rest + layer

# 重建 parent 下标：老数组下标 -> 对象 id -> 新数组下标
id_to_new = {}
for i, o in enumerate(new_objs):
    id_to_new[id(o)] = i
for o in new_objs:
    p = o.get('parent')
    if p is None or p == -1:
        continue
    old_parent_obj = objs[p] if p < len(objs) else None
    # 注意：objs 仍是老数组（已被覆盖？没有——objs 是老列表引用，未被修改）
    if old_parent_obj is None:
        continue
    o['parent'] = id_to_new[id(old_parent_obj)]

d['objects'] = new_objs
io.open(path, 'w', encoding='utf-8').write(json.dumps(d, ensure_ascii=False, indent=1))

# 校验
d2 = json.load(open(path, encoding='utf-8'))
bad = [(i, o.get('parent')) for i, o in enumerate(d2['objects'])
       if o.get('parent') not in (-1, None) and o['parent'] >= i]
print('bad parent refs:', bad[:5] if bad else 'none')
tops2 = [(i, (o.get('name') or o.get('bind'))) for i, o in enumerate(d2['objects']) if o.get('parent') == -1]
print('top order:', tops2)
