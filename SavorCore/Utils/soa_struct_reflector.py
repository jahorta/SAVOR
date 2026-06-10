#!/usr/bin/env python3
import sys, re, pathlib

# Very small, conservative parser tailored to your SoaStructs.h style:
# - Assumes `namespace soa { ... }` wraps the structs
# - Assumes no templates, no unions, no bitfields, no base classes
# - Handles C arrays, nested types, and simple comments
# - Includes ALL declared fields (padding fields are fine: they are byte arrays -> no-op at swap time)

STRUCT_RE = re.compile(r'\bstruct\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{(.*?)\};', re.S)
FIELD_LINE_RE = re.compile(r'^\s*([^;{}/]+?)\s+([A-Za-z_][A-Za-z0-9_]*)(\s*\[[^\]]+\])*\s*;\s*(?://.*|/\*.*\*/\s*)?$', re.S)

def strip_comments(text: str) -> str:
    text = re.sub(r'//.*', '', text)
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return text

def find_namespace_body(src: str, ns: str) -> str:
    # naive but robust for single-namespace file
    m = re.search(r'\bnamespace\s+' + re.escape(ns) + r'\s*\{', src)
    if not m:
        return ""
    depth = 0
    i = m.end()
    start = i
    while i < len(src):
        if src[i] == '{':
            depth += 1
        elif src[i] == '}':
            if depth == 0:
                # end of namespace
                return src[start:i]
            depth -= 1
        i += 1
    return src[start:]

def parse_structs(ns_body: str):
    for sm in STRUCT_RE.finditer(ns_body):
        name = sm.group(1)
        body = sm.group(2)
        # skip static_asserts or forward decls (no body would be empty anyway)
        fields = []
        for line in body.split(';'):
            line = line.strip()
            if not line:
                continue
            line = line + ';'
            # exclude nested types or braces blocks
            if '{' in line or '}' in line:
                continue
            # normalize whitespace
            line = re.sub(r'\s+', ' ', line)
            # match a field
            m = FIELD_LINE_RE.match(line)
            if not m:
                continue
            ftype = m.group(1).strip()
            fname = m.group(2).strip()
            farr  = m.group(3) or ''
            # Keep array suffixes with the declaration via pointer-to-member on the array type.
            # Member pointer to array is valid and our swapper handles arrays element-wise.
            full = f'&type::{fname}'
            fields.append(full)
        yield (name, fields)

def main():
    if len(sys.argv) != 3:
        print("usage: gen_reflect.py <input SoaStructs.h> <output SoaStructs.reflect.h>", file=sys.stderr)
        sys.exit(1)
    inp = pathlib.Path(sys.argv[1])
    outp = pathlib.Path(sys.argv[2])
    src = inp.read_text(encoding='utf-8')
    no_comments = strip_comments(src)
    ns_body = find_namespace_body(no_comments, 'soa')
    if not ns_body:
        print("error: could not find 'namespace soa' in input", file=sys.stderr)
        sys.exit(2)

    structs = list(parse_structs(ns_body))

    out_lines = []
    out_lines.append("// AUTO-GENERATED. DO NOT EDIT.\n#pragma once\n")
    out_lines.append("#include <tuple>\n")
    out_lines.append("#include \"SoaStructs.h\"\n\n")
    out_lines.append("namespace soa_reflect {\n")
    out_lines.append("template <class T, class = void> struct reflect_has : std::false_type {};\n")
    out_lines.append("template <class T> struct reflect_has<T, std::void_t<decltype(T::members)>> : std::true_type {};\n")
    out_lines.append("}\n\n")
    out_lines.append("template <class T> struct reflect; // primary template\n\n")

    for name, fields in structs:
        out_lines.append(f"template <> struct reflect<soa::{name}> {{\n")
        out_lines.append(f"  using type = soa::{name};\n")
        if fields:
            fields_joined = ",\n    ".join(fields)
            out_lines.append("  static constexpr auto members = std::make_tuple(\n    ")
            out_lines.append(fields_joined)
            out_lines.append("\n  );\n")
        else:
            out_lines.append("  static constexpr auto members = std::make_tuple();\n")
        out_lines.append("};\n\n")

    outp.write_text("".join(out_lines), encoding='utf-8')

if __name__ == '__main__':
    main()
