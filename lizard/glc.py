#!/usr/bin/env python3
"""
glc — green lizard → C transpiler for TigxOS userland (V1.0.65)

green lizard is a small C/Python mix:
  - braces { } for blocks (C)
  - fn / let / print / if / while / for (Python-ish)
  - no required semicolons
  - builtins map to user/libc.h syscalls
  - strings: concat (+), index (s[i]), compare (== !=)
  - char literals 'x', break/continue, better errors with line numbers

Usage:
  python3 lizard/glc.py hello.gl -o hello.c
"""
from __future__ import annotations
import argparse
import sys
import re
from dataclasses import dataclass, field
from typing import List, Optional, Any, Tuple

# ---------- Lexer ----------
TOKENS = [
    ("COMMENT", r"//[^\n]*"),
    ("WS",      r"[ \t\r]+"),
    ("NL",      r"\n"),
    ("NUMBER",  r"\d+"),
    ("STRING",  r'"(?:\\.|[^"\\])*"'),
    ("CHAR",    r"'(?:\\.|[^'\\])'"),
    ("IDENT",   r"[A-Za-z_][A-Za-z0-9_]*"),
    ("EQEQ",    r"=="),
    ("NE",      r"!="),
    ("LE",      r"<="),
    ("GE",      r">="),
    ("AND",     r"&&"),
    ("OR",      r"\|\|"),
    ("EQ",      r"="),
    ("PLUS",    r"\+"),
    ("MINUS",   r"-"),
    ("STAR",    r"\*"),
    ("SLASH",   r"/"),
    ("PERCENT", r"%"),
    ("LT",      r"<"),
    ("GT",      r">"),
    ("NOT",     r"!"),
    ("LPAREN",  r"\("),
    ("RPAREN",  r"\)"),
    ("LBRACE",  r"\{"),
    ("RBRACE",  r"\}"),
    ("LBRACK",  r"\["),
    ("RBRACK",  r"\]"),
    ("COMMA",   r","),
    ("SEMI",    r";"),
    ("COLON",   r":"),
]

MASTER = re.compile("|".join(f"(?P<{n}>{p})" for n, p in TOKENS))

KEYWORDS = {
    "fn", "let", "if", "else", "while", "for", "return", "true", "false",
    "int", "str", "void", "break", "continue",
}

@dataclass
class Tok:
    kind: str
    val: str
    line: int

def lex(src: str) -> List[Tok]:
    line = 1
    out: List[Tok] = []
    pos = 0
    while pos < len(src):
        m = MASTER.match(src, pos)
        if not m:
            raise SystemExit(f"lex error at line {line}: {src[pos:pos+20]!r}")
        kind = m.lastgroup
        val = m.group()
        pos = m.end()
        if kind == "NL":
            line += 1
            continue
        if kind in ("WS", "COMMENT"):
            continue
        if kind == "IDENT" and val in KEYWORDS:
            kind = val.upper()
        out.append(Tok(kind, val, line))
    out.append(Tok("EOF", "", line))
    return out

# ---------- AST (every node carries line) ----------
@dataclass
class Node:
    line: int = 0

@dataclass
class Program(Node):
    funcs: List[Any] = field(default_factory=list)

@dataclass
class Func(Node):
    name: str = ""
    params: List[Tuple[str, str]] = field(default_factory=list)  # (name, type)
    body: List[Any] = field(default_factory=list)

@dataclass
class Let(Node):
    name: str = ""
    expr: Any = None
    ty: Optional[str] = None

@dataclass
class Assign(Node):
    name: str = ""
    expr: Any = None

@dataclass
class If(Node):
    cond: Any = None
    then: List[Any] = field(default_factory=list)
    els: List[Any] = field(default_factory=list)

@dataclass
class While(Node):
    cond: Any = None
    body: List[Any] = field(default_factory=list)

@dataclass
class For(Node):
    init: Optional[Any] = None   # Let | Assign | ExprStmt | None
    cond: Optional[Any] = None
    step: Optional[Any] = None   # Assign | ExprStmt | None
    body: List[Any] = field(default_factory=list)

@dataclass
class Return(Node):
    expr: Optional[Any] = None

@dataclass
class Break(Node):
    pass

@dataclass
class Continue(Node):
    pass

@dataclass
class ExprStmt(Node):
    expr: Any = None

@dataclass
class Binary(Node):
    op: str = ""
    left: Any = None
    right: Any = None

@dataclass
class Unary(Node):
    op: str = ""
    expr: Any = None

@dataclass
class Call(Node):
    name: str = ""
    args: List[Any] = field(default_factory=list)

@dataclass
class Name(Node):
    name: str = ""

@dataclass
class Index(Node):
    """base[index] — argv[i] or str[i]"""
    base: Any = None
    index: Any = None

@dataclass
class Number(Node):
    value: int = 0

@dataclass
class String(Node):
    value: str = ""

@dataclass
class Bool(Node):
    value: bool = False

# ---------- Parser ----------
class Parser:
    def __init__(self, toks: List[Tok]):
        self.toks = toks
        self.i = 0

    def cur(self) -> Tok:
        return self.toks[self.i]

    def eat(self, kind: Optional[str] = None) -> Tok:
        t = self.cur()
        if kind and t.kind != kind:
            raise SystemExit(f"line {t.line}: expected {kind}, got {t.kind} ({t.val!r})")
        self.i += 1
        return t

    def match(self, *kinds) -> bool:
        if self.cur().kind in kinds:
            self.i += 1
            return True
        return False

    def parse(self) -> Program:
        funcs = []
        while self.cur().kind != "EOF":
            funcs.append(self.parse_func())
        return Program(funcs=funcs, line=1)

    def parse_func(self) -> Func:
        t = self.eat("FN")
        name = self.eat("IDENT").val
        self.eat("LPAREN")
        params = []
        if self.cur().kind != "RPAREN":
            while True:
                if self.cur().kind in ("INT", "STR", "VOID"):
                    ty = self.eat().val
                    pname = self.eat("IDENT").val
                else:
                    pname = self.eat("IDENT").val
                    ty = "int"
                params.append((pname, ty))
                if not self.match("COMMA"):
                    break
        self.eat("RPAREN")
        if self.match("COLON") or self.cur().kind in ("INT", "STR", "VOID"):
            if self.cur().kind in ("INT", "STR", "VOID"):
                self.eat()
        body = self.parse_block()
        return Func(name=name, params=params, body=body, line=t.line)

    def parse_block(self) -> List[Any]:
        self.eat("LBRACE")
        stmts = []
        while self.cur().kind not in ("RBRACE", "EOF"):
            stmts.append(self.parse_stmt())
            self.match("SEMI")
        self.eat("RBRACE")
        return stmts

    def parse_stmt(self) -> Any:
        k = self.cur().kind
        line = self.cur().line
        if k == "LET":
            return self.parse_let()
        if k == "IF":
            self.eat("IF")
            if self.cur().kind == "LPAREN":
                self.eat("LPAREN")
                cond = self.parse_expr()
                self.eat("RPAREN")
            else:
                cond = self.parse_expr()
            then = self.parse_block()
            els: List[Any] = []
            if self.match("ELSE"):
                if self.cur().kind == "IF":
                    els = [self.parse_stmt()]
                else:
                    els = self.parse_block()
            return If(cond=cond, then=then, els=els, line=line)
        if k == "WHILE":
            self.eat("WHILE")
            if self.cur().kind == "LPAREN":
                self.eat("LPAREN")
                cond = self.parse_expr()
                self.eat("RPAREN")
            else:
                cond = self.parse_expr()
            body = self.parse_block()
            return While(cond=cond, body=body, line=line)
        if k == "FOR":
            return self.parse_for()
        if k == "BREAK":
            self.eat("BREAK")
            self.match("SEMI")
            return Break(line=line)
        if k == "CONTINUE":
            self.eat("CONTINUE")
            self.match("SEMI")
            return Continue(line=line)
        if k == "RETURN":
            self.eat("RETURN")
            if self.cur().kind in ("SEMI", "RBRACE"):
                self.match("SEMI")
                return Return(expr=None, line=line)
            expr = self.parse_expr()
            self.match("SEMI")
            return Return(expr=expr, line=line)
        # assignment or expression statement
        expr = self.parse_expr()
        if isinstance(expr, Name) and self.cur().kind == "EQ":
            self.eat("EQ")
            rhs = self.parse_expr()
            self.match("SEMI")
            return Assign(name=expr.name, expr=rhs, line=line)
        self.match("SEMI")
        return ExprStmt(expr=expr, line=line)

    def parse_let(self) -> Let:
        t = self.eat("LET")
        name = self.eat("IDENT").val
        ty = None
        if self.match("COLON"):
            ty = self.eat().val
        self.eat("EQ")
        expr = self.parse_expr()
        # trailing SEMI is optional and left for the caller (stmt or for-init)
        return Let(name=name, expr=expr, ty=ty, line=t.line)

    def parse_for(self) -> For:
        """for init; cond; step { body }   (C-style, parts optional)"""
        t = self.eat("FOR")
        # optional parentheses around the three parts
        paren = self.match("LPAREN")
        init: Optional[Any] = None
        if self.cur().kind not in ("SEMI",):
            if self.cur().kind == "LET":
                init = self.parse_let()
            else:
                # assign or expr
                expr = self.parse_expr()
                if isinstance(expr, Name) and self.cur().kind == "EQ":
                    self.eat("EQ")
                    rhs = self.parse_expr()
                    init = Assign(name=expr.name, expr=rhs, line=expr.line)
                else:
                    init = ExprStmt(expr=expr, line=expr.line)
        self.eat("SEMI")
        cond: Optional[Any] = None
        if self.cur().kind not in ("SEMI",):
            cond = self.parse_expr()
        self.eat("SEMI")
        step: Optional[Any] = None
        if self.cur().kind not in ("RPAREN", "LBRACE"):
            expr = self.parse_expr()
            if isinstance(expr, Name) and self.cur().kind == "EQ":
                self.eat("EQ")
                rhs = self.parse_expr()
                step = Assign(name=expr.name, expr=rhs, line=expr.line)
            else:
                step = ExprStmt(expr=expr, line=expr.line)
        if paren:
            self.eat("RPAREN")
        body = self.parse_block()
        return For(init=init, cond=cond, step=step, body=body, line=t.line)

    # Pratt-ish expression parsing
    def parse_expr(self) -> Any:
        return self.parse_or()

    def parse_or(self) -> Any:
        left = self.parse_and()
        while self.match("OR"):
            line = self.toks[self.i - 1].line
            right = self.parse_and()
            left = Binary(op="||", left=left, right=right, line=line)
        return left

    def parse_and(self) -> Any:
        left = self.parse_cmp()
        while self.match("AND"):
            line = self.toks[self.i - 1].line
            right = self.parse_cmp()
            left = Binary(op="&&", left=left, right=right, line=line)
        return left

    def parse_cmp(self) -> Any:
        left = self.parse_add()
        while self.cur().kind in ("EQEQ", "NE", "LT", "GT", "LE", "GE"):
            op = self.eat().val
            line = self.toks[self.i - 1].line
            right = self.parse_add()
            left = Binary(op=op, left=left, right=right, line=line)
        return left

    def parse_add(self) -> Any:
        left = self.parse_mul()
        while self.cur().kind in ("PLUS", "MINUS"):
            op = self.eat().val
            line = self.toks[self.i - 1].line
            right = self.parse_mul()
            left = Binary(op=op, left=left, right=right, line=line)
        return left

    def parse_mul(self) -> Any:
        left = self.parse_unary()
        while self.cur().kind in ("STAR", "SLASH", "PERCENT"):
            op = self.eat().val
            line = self.toks[self.i - 1].line
            right = self.parse_unary()
            left = Binary(op=op, left=left, right=right, line=line)
        return left

    def parse_unary(self) -> Any:
        if self.cur().kind in ("MINUS", "NOT"):
            op = self.eat().val
            line = self.toks[self.i - 1].line
            return Unary(op=op, expr=self.parse_unary(), line=line)
        return self.parse_primary()

    def parse_primary(self) -> Any:
        t = self.cur()
        if t.kind == "NUMBER":
            self.eat()
            return Number(value=int(t.val), line=t.line)
        if t.kind == "STRING":
            self.eat()
            return String(value=bytes(t.val[1:-1], "utf-8").decode("unicode_escape"), line=t.line)
        if t.kind == "CHAR":
            self.eat()
            raw = t.val[1:-1]
            if raw.startswith("\\"):
                esc = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", "\\": "\\", "'": "'", '"': '"'}.get(raw[1], raw[1])
                return Number(value=ord(esc), line=t.line)
            return Number(value=ord(raw), line=t.line)
        if t.kind == "TRUE":
            self.eat()
            return Bool(value=True, line=t.line)
        if t.kind == "FALSE":
            self.eat()
            return Bool(value=False, line=t.line)
        if t.kind == "IDENT":
            name = self.eat("IDENT").val
            if self.match("LPAREN"):
                args = []
                if self.cur().kind != "RPAREN":
                    while True:
                        args.append(self.parse_expr())
                        if not self.match("COMMA"):
                            break
                self.eat("RPAREN")
                node: Any = Call(name=name, args=args, line=t.line)
            else:
                node = Name(name=name, line=t.line)
            # postfix indexing: name[expr] or call[expr]
            while self.match("LBRACK"):
                idx = self.parse_expr()
                self.eat("RBRACK")
                node = Index(base=node, index=idx, line=t.line)
            return node
        if t.kind == "LPAREN":
            self.eat("LPAREN")
            e = self.parse_expr()
            self.eat("RPAREN")
            return e
        raise SystemExit(f"line {t.line}: unexpected {t.kind} ({t.val!r})")

# ---------- Codegen → C ----------
BUILTINS = {
    "print", "write", "exit", "getpid", "yield", "sleep",
    "open", "close", "spawn", "wait", "len", "unlink", "stat",
    "readch", "writech", "listdir",
}

class Codegen:
    def __init__(self):
        self.lines: List[str] = []
        self.indent = 0
        self.vars: set = set()
        self.vartype: dict = {}  # name -> "int" | "str"

    def emit(self, s: str = ""):
        self.lines.append("    " * self.indent + s)

    def err(self, node: Any, msg: str):
        line = getattr(node, "line", 0) or "?"
        raise SystemExit(f"line {line}: {msg}")

    def gen(self, prog: Program) -> str:
        self.emit("/* Generated by glc — do not edit */")
        self.emit('#include "libc.h"')
        self.emit("int __gl_call_depth;")
        self.emit("")
        for f in prog.funcs:
            self.gen_func(f)
        names = {f.name for f in prog.funcs}
        if "main" not in names:
            raise SystemExit("glc: program must define fn main()")
        return "\n".join(self.lines) + "\n"

    def gen_func(self, f: Func):
        self.vars = {p[0] for p in f.params}
        self.vartype = {p[0]: p[1] for p in f.params}
        params = ", ".join(f"long {n}" for n, _ in f.params)
        if f.name == "main":
            self.emit(f"int main(int argc, char **argv) {{")
            self.indent += 1
            self.vars.add("argc")
            self.vars.add("argv")
            self.vartype["argc"] = "int"
            self.vartype["argv"] = "str"  # special
        else:
            self.emit(f"static long {f.name}({params}) {{")
            self.indent += 1
            # Recursion depth guard (shared process-local counter)
            self.emit("extern int __gl_call_depth;")
            self.emit("if (++__gl_call_depth > 64) { __gl_call_depth = 0; exit(1); }")
        for s in f.body:
            self.gen_stmt(s)
        if f.name == "main":
            self.emit("return 0;")
        else:
            self.emit("__gl_call_depth--;")
        self.indent -= 1
        self.emit("}")
        self.emit("")

    def gen_stmt(self, s: Any):
        if isinstance(s, Let):
            e, ty = self.gen_expr(s.expr)
            if s.ty == "str":
                ty = "str"
            elif s.ty == "int":
                ty = "int"
            if s.name not in self.vars:
                self.vars.add(s.name)
                self.vartype[s.name] = ty
                ctype = "const char *" if ty == "str" else "long"
                self.emit(f"{ctype} {s.name} = {e};")
            else:
                self.emit(f"{s.name} = {e};")
        elif isinstance(s, Assign):
            e, _ = self.gen_expr(s.expr)
            self.emit(f"{s.name} = {e};")
        elif isinstance(s, If):
            c, _ = self.gen_expr(s.cond)
            self.emit(f"if ({c}) {{")
            self.indent += 1
            for x in s.then:
                self.gen_stmt(x)
            self.indent -= 1
            if s.els:
                self.emit("} else {")
                self.indent += 1
                for x in s.els:
                    self.gen_stmt(x)
                self.indent -= 1
            self.emit("}")
        elif isinstance(s, While):
            c, _ = self.gen_expr(s.cond)
            self.emit(f"while ({c}) {{")
            self.indent += 1
            for x in s.body:
                self.gen_stmt(x)
            self.indent -= 1
            self.emit("}")
        elif isinstance(s, For):
            # emit C for(init; cond; step)
            init_code = ""
            if s.init is not None:
                saved = self.lines
                self.lines = []
                old_indent = self.indent
                self.indent = 0
                self.gen_stmt(s.init)
                init_parts = self.lines
                self.lines = saved
                self.indent = old_indent
                init_code = " ".join(p.rstrip(";") for p in init_parts)
            cond_code = "1"
            if s.cond is not None:
                cond_code, _ = self.gen_expr(s.cond)
            step_code = ""
            if s.step is not None:
                saved = self.lines
                self.lines = []
                old_indent = self.indent
                self.indent = 0
                self.gen_stmt(s.step)
                step_parts = self.lines
                self.lines = saved
                self.indent = old_indent
                step_code = " ".join(p.rstrip(";") for p in step_parts)
            self.emit(f"for ({init_code}; {cond_code}; {step_code}) {{")
            self.indent += 1
            for x in s.body:
                self.gen_stmt(x)
            self.indent -= 1
            self.emit("}")
        elif isinstance(s, Break):
            self.emit("break;")
        elif isinstance(s, Continue):
            self.emit("continue;")
        elif isinstance(s, Return):
            if s.expr is None:
                self.emit("return 0;")
            else:
                e, _ = self.gen_expr(s.expr)
                self.emit(f"return (int)({e});")
        elif isinstance(s, ExprStmt):
            e, _ = self.gen_expr(s.expr)
            self.emit(f"(void)({e});")
        else:
            self.err(s, f"unknown statement {type(s)}")

    def gen_expr(self, e: Any) -> Tuple[str, str]:
        """Returns (c_code, type) type in {int,str}"""
        if isinstance(e, Number):
            return str(e.value), "int"
        if isinstance(e, String):
            esc = e.value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")
            return f'"{esc}"', "str"
        if isinstance(e, Bool):
            return ("1" if e.value else "0"), "int"
        if isinstance(e, Name):
            if e.name == "argc":
                return "argc", "int"
            if e.name == "argv":
                self.err(e, "use argv[i], not bare argv")
            _flags = {
                "O_RDONLY": "0", "O_WRONLY": "1", "O_RDWR": "2",
                "O_CREAT": "0x100", "O_TRUNC": "0x200", "O_APPEND": "0x400",
                "STDIN": "0", "STDOUT": "1", "STDERR": "2",
            }
            if e.name in _flags:
                return _flags[e.name], "int"
            vt = self.vartype.get(e.name, "int")
            return e.name, vt
        if isinstance(e, Index):
            idx, ity = self.gen_expr(e.index)
            if ity != "int":
                self.err(e, "index must be int")
            if isinstance(e.base, Name) and e.base.name == "argv":
                # bounds via libc argv_get — OOB → "" (no crash)
                return f"argv_get(argv, argc, (int)({idx}))", "str"
            # string index via libc str_index — OOB → 0
            base, bty = self.gen_expr(e.base)
            if bty != "str":
                self.err(e, "only string or argv indexing is supported")
            return f"str_index({base}, (int)({idx}))", "int"
        if isinstance(e, Unary):
            a, _ = self.gen_expr(e.expr)
            if e.op == "!":
                return f"!({a})", "int"
            return f"(-({a}))", "int"
        if isinstance(e, Binary):
            l, lt = self.gen_expr(e.left)
            r, rt = self.gen_expr(e.right)
            # string concatenation
            if e.op == "+" and (lt == "str" or rt == "str"):
                if lt != "str" or rt != "str":
                    self.err(e, "string + requires both sides to be str")
                return f"str_concat({l}, {r})", "str"
            # string comparison
            if e.op in ("==", "!=") and (lt == "str" or rt == "str"):
                if lt != "str" or rt != "str":
                    self.err(e, f"{e.op} on mixed str/int")
                cmp = f"(strcmp({l}, {r}) == 0)"
                if e.op == "!=":
                    cmp = f"(strcmp({l}, {r}) != 0)"
                return cmp, "int"
            if e.op in ("/", "%"):
                # runtime-safe: divide-by-zero yields 0
                if e.op == "/":
                    return f"((({r}) == 0) ? 0 : (({l}) / ({r})))", "int"
                return f"((({r}) == 0) ? 0 : (({l}) % ({r})))", "int"
            return f"({l} {e.op} {r})", "int"
        if isinstance(e, Call):
            return self.gen_call(e)
        self.err(e, f"unknown expression {type(e)}")

    def gen_call(self, c: Call) -> Tuple[str, str]:
        args = [self.gen_expr(a) for a in c.args]
        n = c.name
        if n == "print":
            if len(args) == 1 and args[0][1] == "str":
                return f"puts({args[0][0]})", "int"
            if len(args) == 1 and args[0][1] == "int":
                return f'printf("%d\\n", (int)({args[0][0]}))', "int"
            parts = []
            for i, (code, ty) in enumerate(args):
                if i > 0:
                    parts.append('printf(" ")')
                if ty == "str":
                    parts.append(f"printf(\"%s\", {code})")
                else:
                    parts.append(f"printf(\"%d\", (int)({code}))")
            parts.append('printf("\\n")')
            return "(" + ", ".join(parts) + ")", "int"
        if n == "write":
            if len(args) != 2:
                self.err(c, "write(fd, str)")
            fd, s = args[0][0], args[1][0]
            return f"write((int)({fd}), {s}, strlen({s}))", "int"
        if n == "exit":
            a = args[0][0] if args else "0"
            return f"(exit((int)({a})), 0)", "int"
        if n == "getpid":
            return "getpid()", "int"
        if n == "yield":
            return "(yield(), 0)", "int"
        if n == "sleep":
            a = args[0][0] if args else "0"
            return f"(sleep_ms((unsigned)({a})), 0)", "int"
        if n == "open":
            if len(args) < 1:
                self.err(c, "open(path, flags?)")
            path = args[0][0]
            flags = args[1][0] if len(args) > 1 else "0"
            return f"open({path}, (int)({flags}))", "int"
        if n == "close":
            return f"close((int)({args[0][0]}))", "int"
        if n == "spawn":
            return f"spawn({args[0][0]}, 0)", "int"
        if n == "wait":
            a = args[0][0] if args else "-1"
            return f"wait((int)({a}))", "int"
        if n == "unlink":
            if len(args) != 1:
                self.err(c, "unlink(path)")
            return f"unlink({args[0][0]})", "int"
        if n == "stat":
            if len(args) != 1:
                self.err(c, "stat(path) -> size")
            return f"({{ struct stat __st; long __r = stat({args[0][0]}, &__st); __r < 0 ? -1L : (long)__st.size; }})", "int"
        if n == "readch":
            if len(args) != 1:
                self.err(c, "readch(fd)")
            return f"readch((int)({args[0][0]}))", "int"
        if n == "writech":
            if len(args) != 2:
                self.err(c, "writech(fd, c)")
            return f"writech((int)({args[0][0]}), (int)({args[1][0]}))", "int"
        if n == "listdir":
            return "(listdir(), 0)", "int"
        if n == "len":
            if len(args) != 1:
                self.err(c, "len(s)")
            return f"((long)strlen({args[0][0]}))", "int"
        # user function
        alist = ", ".join(a[0] for a in args)
        return f"{n}({alist})", "int"


def compile_source(src: str) -> str:
    toks = lex(src)
    prog = Parser(toks).parse()
    return Codegen().gen(prog)


def main():
    ap = argparse.ArgumentParser(description="green lizard → C compiler for TigxOS")
    ap.add_argument("input", help=".gl source file (.tig still accepted, deprecated)")
    ap.add_argument("-o", "--output", help="output .c file")
    args = ap.parse_args()
    if args.input.endswith(".tig"):
        print("glc: warning: .tig is deprecated, rename to .gl", file=sys.stderr)
    src = open(args.input, encoding="utf-8").read()
    try:
        c = compile_source(src)
    except SystemExit as e:
        print(f"glc: {e}", file=sys.stderr)
        sys.exit(1)
    out = args.output
    if not out:
        out = args.input.rsplit(".", 1)[0] + ".c"
    open(out, "w", encoding="utf-8").write(c)
    print(f"glc: {args.input} -> {out}")

if __name__ == "__main__":
    main()
