#!/usr/bin/env python3
"""Checks the Doxygen comments in include/tpw/*.h against three rules:

1. Every function/callback's actual parameter order has every @param[out]
   (and @param[in,out]) after every @param[in] (params with no direction
   tag are implicit @param[in]) — EXCEPT that an out-array may be
   immediately followed by its own `<name>_len` capacity in-param. That
   specific adjacent pair is allowed in either order, keeping an array
   and its capacity next to each other: this project's
   tpw_stream_get_dmabuf_planes()/tpw_filter_port_get_dmabuf_planes() use it for
   their `planes`/`planes_len` pair.
2. Every function/callback/type has a doc block with @brief; every
   non-void function/callback also has @return; the @param name list
   matches the declaration's parameter list exactly (same names, same
   order, none missing or extra).
3. Every @see target names a symbol (function, typedef, or enumerator)
   actually declared somewhere in the checked headers.

Usage: check_doxygen_conventions.py <header.h> [<header.h> ...]
Exits 1 and prints every violation found, across all given headers,
combined (so e.g. a tpw_filter.h @see can resolve to a tpw_stream.h
symbol). Exits 0 with a one-line summary otherwise.
"""
import re
import sys

COMMENT_RE = re.compile(r'/\*.*?\*/', re.DOTALL)
EXTERN_C_OPEN_RE = re.compile(r'#ifdef __cplusplus\s*\nextern "C" \{\s*\n#endif\n?')
EXTERN_C_CLOSE_RE = re.compile(r'#ifdef __cplusplus\s*\n\}\s*\n#endif\n?')
PARAM_TAG_RE = re.compile(r'@param(?:\[(in|out|in,out)\])?\s+(\w+)')
SEE_RE = re.compile(r'@see\s+(\w+)')
TPW_IDENT_RE = re.compile(r'\btpw_[A-Za-z0-9_]+\b')
ENUM_IDENT_RE = re.compile(r'\bTPW_[A-Z0-9_]+\b')


def strip_comments(s):
    return COMMENT_RE.sub('', s)


def tokenize(text):
    """Splits a header into ('comment', text) and ('decl', text) blocks,
    in source order, treating a decl as everything up to the next ';'
    at brace/paren depth 0 (nested comments don't affect depth)."""
    text = EXTERN_C_OPEN_RE.sub('', text)
    text = EXTERN_C_CLOSE_RE.sub('', text)
    i, n = 0, len(text)
    blocks = []
    while i < n:
        while i < n and text[i] in ' \t\r\n':
            i += 1
        if i >= n:
            break
        if text[i:i + 2] == '/*':
            end = text.index('*/', i + 2) + 2
            blocks.append(('comment', text[i:end]))
            i = end
            continue
        if text[i:i + 2] == '//':
            end = text.find('\n', i)
            i = n if end == -1 else end
            continue
        if text[i] == '#':
            end = text.find('\n', i)
            i = n if end == -1 else end
            continue
        start = i
        depth = 0
        while i < n:
            if text[i:i + 2] == '/*':
                i = text.index('*/', i + 2) + 2
                continue
            c = text[i]
            if c in '({[':
                depth += 1
            elif c in ')}]':
                depth -= 1
            elif c == ';' and depth <= 0:
                i += 1
                break
            i += 1
        blocks.append(('decl', text[start:i]))
    return blocks


def analyze_decl(decl_text):
    """Returns (name, is_void, params) for a function/callback decl, or
    (name, None, None) for a struct/enum/opaque-handle typedef."""
    code = re.sub(r'\bTPW_API\b', '', strip_comments(decl_text))
    if '(' not in code:
        m = re.search(r'(\w+)\s*;\s*$', code)
        return (m.group(1) if m else None), None, None

    open_idx = code.rindex('(')
    close_idx = code.rindex(')')
    params_text = code[open_idx + 1:close_idx]
    before = code[:open_idx]
    idents = list(TPW_IDENT_RE.finditer(before))
    if not idents:
        return None, None, None
    name = idents[-1].group(0)
    return_type = before[:idents[-1].start()].replace('typedef', '')
    return_type = re.sub(r'[(*\s]+$', '', return_type).strip()
    is_void = (return_type == 'void')

    params = []
    ptext = params_text.strip()
    if ptext and ptext != 'void':
        for p in ptext.split(','):
            pm = re.search(r'(\w+)\s*$', p.strip())
            if pm:
                params.append(pm.group(1))
    return name, is_void, params


def check_param_order(path, name, tags, errors):
    """tags: [(direction_or_'', param_name), ...] in declaration order."""
    seen_out = None
    for direction, pname in tags:
        is_out = direction in ('out', 'in,out')
        if seen_out is not None and not is_out:
            if pname == seen_out + '_len':
                seen_out = None  # the array's own length: allowed, pair closed
                continue
            errors.append(
                f"{path}: {name}: in-parameter `{pname}` comes after "
                f"out-parameter `{seen_out}` without being its `_len` "
                f"companion — out-params must be last (an out-array may be "
                f"immediately followed by its own `{seen_out}_len`)")
        if is_out:
            seen_out = pname


def check_header(path, errors, known_symbols, see_refs):
    text = open(path).read()
    blocks = tokenize(text)

    pending_comment = None
    for kind, block_text in blocks:
        if kind == 'comment':
            pending_comment = block_text
            for m in SEE_RE.finditer(block_text):
                see_refs.append((path, m.group(1)))
            continue

        # kind == 'decl'
        name, is_void, params = analyze_decl(block_text)
        if name is None:
            pending_comment = None
            continue
        known_symbols.add(name)
        for m in ENUM_IDENT_RE.finditer(strip_comments(block_text)):
            known_symbols.add(m.group(0))

        doc = pending_comment or ''
        pending_comment = None

        if '@brief' not in doc:
            errors.append(f"{path}: {name}: missing @brief")

        if params is None:
            continue  # struct/enum/opaque handle: no @param/@return to check

        if not doc:
            errors.append(f"{path}: {name}: no doc block found for a function/callback")
            continue

        if not is_void and '@return' not in doc:
            errors.append(f"{path}: {name}: non-void, missing @return")

        tags = PARAM_TAG_RE.findall(doc)  # [(direction_or_'', name), ...]
        doc_names = [n for _, n in tags]
        if doc_names != params:
            errors.append(
                f"{path}: {name}: @param names {doc_names} don't match the "
                f"declaration's parameter order {params}")
            continue  # order check below assumes they line up 1:1

        check_param_order(path, name, tags, errors)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1

    errors = []
    known_symbols = set()
    see_refs = []
    for path in argv[1:]:
        check_header(path, errors, known_symbols, see_refs)

    for path, target in see_refs:
        if target not in known_symbols:
            errors.append(f"{path}: @see references unknown symbol `{target}`")

    if errors:
        print(f"check_doxygen_conventions: {len(errors)} violation(s):")
        for e in errors:
            print(f"  - {e}")
        return 1

    print(f"check_doxygen_conventions: OK ({len(known_symbols)} symbols checked "
          f"across {len(argv) - 1} header(s))")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
