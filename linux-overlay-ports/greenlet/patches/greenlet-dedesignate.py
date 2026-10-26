# -*- coding: utf-8 -*-
"""降级 greenlet 3.1.x 的 designated initializer 到 v141 可编译的位置初始化。
   原则:绝不重打任何值——只做 (a) 去掉 '.field=' 前缀,(b) 为跳过的字段插入 '0,' 占位。
   这样字符串字面量(尤其 tp_doc 的多行拼接)原封不动。"""
import io, os, re, sys
root = sys.argv[1]

TYPEOBJ = ["tp_name","tp_basicsize","tp_itemsize","tp_dealloc","tp_vectorcall_offset",
 "tp_getattr","tp_setattr","tp_as_async","tp_repr","tp_as_number","tp_as_sequence",
 "tp_as_mapping","tp_hash","tp_call","tp_str","tp_getattro","tp_setattro","tp_as_buffer",
 "tp_flags","tp_doc","tp_traverse","tp_clear","tp_richcompare","tp_weaklistoffset",
 "tp_iter","tp_iternext","tp_methods","tp_members","tp_getset","tp_base","tp_dict",
 "tp_descr_get","tp_descr_set","tp_dictoffset","tp_init","tp_alloc","tp_new","tp_free","tp_is_gc"]
NUMBER = ["nb_add","nb_subtract","nb_multiply","nb_remainder","nb_divmod","nb_power",
 "nb_negative","nb_positive","nb_absolute","nb_bool"]

def rd(p): return io.open(p, encoding='utf-8', newline='').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8', newline='').write(s)

def span(text, pat):
    m = re.search(pat, text)
    if not m: raise SystemExit('NOT FOUND: ' + pat)
    return m.start(), text.index('\n};', m.start()) + 3

def sparse(text, pat, order):
    """按 order 补齐被跳过的字段;逐行处理,非 designator 行原样保留。"""
    s, e = span(text, pat); seg = text[s:e]
    out, idx = [], 0
    for line in seg.split('\n'):
        m = re.match(r'^(\s*)\.\s*(\w+)\s*=\s*(.*)$', line)
        if not m:
            out.append(line); continue
        ind, field, rest = m.groups()
        if field == 'ob_base':                       # PyVarObject_HEAD_INIT,自带结尾
            out.append(ind + rest); continue
        if field not in order:
            raise SystemExit('unknown field ' + field)
        want = order.index(field)
        while idx < want:                            # 补占位
            out.append('%s0, /* %s */' % (ind, order[idx])); idx += 1
        out.append(ind + rest)                       # 原值,一字不改
        idx = want + 1
    return text[:s] + '\n'.join(out) + text[e:]

def prefix(text, pat):
    """按声明顺序的前缀式结构:直接去掉 '.field=' 。"""
    s, e = span(text, pat); seg = text[s:e]
    return text[:s] + re.sub(r'(?m)(^|[{,]\s*)\.\s*\w+\s*=\s*', r'\1', seg) + text[e:]

p = os.path.join(root,'PyGreenlet.cpp'); t = rd(p)
for pat in [r'static PyMethodDef green_methods\[\] = \{',
            r'static PyGetSetDef green_getsets\[\] = \{',
            r'static PyMemberDef green_members\[\] = \{']:
    t = prefix(t, pat)
t = sparse(t, r'static PyNumberMethods green_as_number = \{', NUMBER)
t = sparse(t, r'PyTypeObject PyGreenlet_Type = \{', TYPEOBJ)
wr(p, t); print('PyGreenlet.cpp  ok')

p = os.path.join(root,'PyModule.cpp'); t = rd(p)
t = prefix(t, r'static PyMethodDef GreenMethods\[\] = \{')
t = prefix(t, r'static struct PyModuleDef greenlet_module_def = \{')
wr(p, t); print('PyModule.cpp    ok')

p = os.path.join(root,'PyGreenletUnswitchable.cpp'); t = rd(p)
t = prefix(t, r'static PyGetSetDef green_unswitchable_getsets\[\] = \{')
t = sparse(t, r'PyTypeObject PyGreenletUnswitchable_Type = \{', TYPEOBJ)
wr(p, t); print('PyGreenletUnswitchable.cpp ok')
