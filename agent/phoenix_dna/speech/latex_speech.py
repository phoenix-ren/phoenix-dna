# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""LaTeX math made speakable: formulas read as mathematics, not markup.

The window renders the answer's math with LaTeX (theme/latexrenderer);
the daemon cannot call into that C++ pipeline, so it speaks the same
spans instead. Two tables keep the sides aligned:

* ``UNICODE_LATEX`` is the English-side mirror of the renderer's
  ``unicodeTablePreamble()`` — the Unicode symbols the models write
  (∫, α, → …) mapped to the LaTeX commands the parser below already
  knows. The shared fixture ``agent/tests/data/mathspans.json`` pins
  symbol, command and spoken word together for both sides.
* ``_COMMAND_WORDS`` and friends map LaTeX constructs onto the words a
  listener expects ("\\frac{a}{b}" → "a over b").

If a span does not parse confidently it yields "" — the caller drops it
silently, because reading raw LaTeX aloud ("backslash frac curly brace")
is worse than saying nothing: the surrounding prose still carries the
meaning, and the rendered image in the window carries the detail.
"""

from __future__ import annotations

import logging
import re

log = logging.getLogger("phoenix.speech.latex")

#: Spoken instead of a long display span (a derivation, read symbol by
#: symbol it would be noise). The threshold is on the *source* length:
#: a formula anyone can hear is short to write, a derivation is not.
DISPLAY_SOURCE_LIMIT = 80
#: What a long display span is announced as; the image carries the rest.
DISPLAY_SPOKEN = "a displayed formula"

# --------------------------------------------------------------------------
# Unicode math → LaTeX command. Mirrors unicodeTablePreamble() in
# theme/latexrenderer.cpp; the shared fixture pins both sides.
# --------------------------------------------------------------------------

UNICODE_LATEX: dict[str, str] = {
    "×": r"\times",
    "⋅": r"\cdot",
    "·": r"\cdot",
    "≤": r"\leq",
    "≥": r"\geq",
    "≠": r"\neq",
    "≈": r"\approx",
    "≡": r"\equiv",
    "∝": r"\propto",
    "∈": r"\in",
    "∉": r"\notin",
    "∞": r"\infty",
    "∂": r"\partial",
    "∇": r"\nabla",
    "√": r"\sqrt",
    "∫": r"\int",
    "∮": r"\oint",
    "∑": r"\sum",
    "∏": r"\prod",
    "→": r"\rightarrow",
    "←": r"\leftarrow",
    "↔": r"\leftrightarrow",
    "⇒": r"\Rightarrow",
    "⇐": r"\Leftarrow",
    "⇔": r"\Leftrightarrow",
    "⟶": r"\longrightarrow",
    "↦": r"\mapsto",
    "ℏ": r"\hbar",
    "ℓ": r"\ell",
    "ℝ": r"\mathbb{R}",
    "ℕ": r"\mathbb{N}",
    "ℤ": r"\mathbb{Z}",
    "ℚ": r"\mathbb{Q}",
    "ℂ": r"\mathbb{C}",
    "′": r"\prime",
    "″": r"\prime\prime",
    "α": r"\alpha",
    "β": r"\beta",
    "γ": r"\gamma",
    "δ": r"\delta",
    "ε": r"\varepsilon",
    "θ": r"\theta",
    "λ": r"\lambda",
    "μ": r"\mu",
    "π": r"\pi",
    "ρ": r"\rho",
    "σ": r"\sigma",
    "τ": r"\tau",
    "φ": r"\varphi",
    "ω": r"\omega",
    "Δ": r"\Delta",
    "Ω": r"\Omega",
    "Γ": r"\Gamma",
    "Σ": r"\Sigma",
    "Θ": r"\Theta",
}

# --------------------------------------------------------------------------
# LaTeX command → spoken word. Commands absent from every table fail the
# span ("not confidently parseable") rather than being read as markup.
# --------------------------------------------------------------------------

#: Bare symbol commands: one word (or short phrase), no arguments.
_COMMAND_WORDS: dict[str, str] = {
    # relations
    "le": "is less than or equal to",
    "leq": "is less than or equal to",
    "ge": "is greater than or equal to",
    "geq": "is greater than or equal to",
    "ne": "is not equal to",
    "neq": "is not equal to",
    "approx": "is approximately equal to",
    "simeq": "is approximately equal to",
    "equiv": "is identical to",
    "propto": "is proportional to",
    "sim": "is similar to",
    "in": "is in",
    "notin": "is not in",
    "ni": "contains",
    "subset": "is a subset of",
    "subseteq": "is a subset of or equal to",
    "supset": "is a superset of",
    "supseteq": "is a superset of or equal to",
    "emptyset": "the empty set",
    "varnothing": "the empty set",
    "cup": "union",
    "cap": "intersection",
    "setminus": "minus",
    # operators and misc symbols
    "infty": "infinity",
    "partial": "partial",
    "nabla": "nabla",
    "pm": "plus or minus",
    "mp": "minus or plus",
    "times": "times",
    "cdot": "times",
    "div": "divided by",
    "ast": "star",
    "oplus": "direct sum",
    "otimes": "tensor product",
    "perp": "is perpendicular to",
    "parallel": "is parallel to",
    "angle": "angle",
    "degree": "degrees",
    "circ": "degrees",
    "prime": "prime",
    "hbar": "h bar",
    "ell": "ell",
    "forall": "for all",
    "exists": "there exists",
    "nexists": "there is no",
    "neg": "not",
    "lnot": "not",
    "land": "and",
    "lor": "or",
    "cdots": "dot dot dot",
    "ldots": "dot dot dot",
    "dots": "dot dot dot",
    "vdots": "dot dot dot",
    "ddots": "dot dot dot",
    "Re": "the real part of",
    "Im": "the imaginary part of",
    "det": "the determinant of",
    # arrows
    "to": "approaches",
    "rightarrow": "approaches",
    "longrightarrow": "approaches",
    "leftarrow": "from",
    "longleftarrow": "from",
    "gets": "from",
    "leftrightarrow": "if and only if",
    "Leftrightarrow": "if and only if",
    "iff": "if and only if",
    "Rightarrow": "implies",
    "Longrightarrow": "implies",
    "implies": "implies",
    "Leftarrow": "is implied by",
    "mapsto": "maps to",
    "uparrow": "increases to",
    "downarrow": "decreases to",
    # greek
    "alpha": "alpha",
    "beta": "beta",
    "gamma": "gamma",
    "delta": "delta",
    "epsilon": "epsilon",
    "varepsilon": "epsilon",
    "zeta": "zeta",
    "eta": "eta",
    "theta": "theta",
    "vartheta": "theta",
    "iota": "iota",
    "kappa": "kappa",
    "lambda": "lambda",
    "mu": "mu",
    "nu": "nu",
    "xi": "xi",
    "pi": "pi",
    "varpi": "pi",
    "rho": "rho",
    "varrho": "rho",
    "sigma": "sigma",
    "varsigma": "sigma",
    "tau": "tau",
    "upsilon": "upsilon",
    "phi": "phi",
    "varphi": "phi",
    "chi": "chi",
    "psi": "psi",
    "omega": "omega",
    "Gamma": "gamma",
    "Delta": "delta",
    "Theta": "theta",
    "Lambda": "lambda",
    "Xi": "xi",
    "Pi": "pi",
    "Sigma": "sigma",
    "Upsilon": "upsilon",
    "Phi": "phi",
    "Psi": "psi",
    "Omega": "omega",
}

#: Named number sets; \mathbb{R} → "the reals", like the Unicode glyphs.
_BLACKBOARD: dict[str, str] = {
    "R": "the reals",
    "N": "the naturals",
    "Z": "the integers",
    "Q": "the rationals",
    "C": "the complex numbers",
}

#: Function names (read as a word; a following parenthesized argument is
#: spoken as "of …" by the atom parser, giving "sine of x").
_FUNCTIONS: dict[str, str] = {
    "sin": "sine",
    "cos": "cosine",
    "tan": "tangent",
    "cot": "cotangent",
    "sec": "secant",
    "csc": "cosecant",
    "arcsin": "arc sine",
    "arccos": "arc cosine",
    "arctan": "arc tangent",
    "sinh": "hyperbolic sine",
    "cosh": "hyperbolic cosine",
    "tanh": "hyperbolic tangent",
    "log": "log",
    "ln": "the natural log",
    "exp": "exp",
    "max": "the maximum",
    "min": "the minimum",
    "sup": "the supremum",
    "inf": "the infimum",
    "arg": "the argument of",
    "deg": "degrees",
    "gcd": "the greatest common divisor",
    "mod": "mod",
    "bmod": "mod",
}

#: Big operators: prefixes that take bounds and an "of" continuation.
_BIG_OPERATORS: dict[str, str] = {
    "sum": "the sum",
    "prod": "the product",
    "coprod": "the coproduct",
    "int": "the integral",
    "oint": "the contour integral",
    "bigcup": "the union",
    "bigcap": "the intersection",
    "bigoplus": "the direct sum",
    "bigotimes": "the tensor product",
}

#: Commands read literally: their braced content is prose, not math.
_TEXTUAL = {"text", "mathrm", "operatorname", "textbf", "textit", "textrm", "texttt"}

#: Style-only wrappers: speak their argument unchanged.
_STYLE = {
    "mathbf",
    "mathit",
    "mathsf",
    "mathtt",
    "mathcal",
    "mathscr",
    "mathfrak",
    "boldsymbol",
    "ensuremath",
    "left",
    "right",
    "displaystyle",
    "limits",
    "nolimits",
    "big",
    "Big",
    "bigg",
    "Bigg",
    "bigl",
    "bigr",
    "Bigl",
    "Bigr",
    "biggl",
    "biggr",
}

#: Spacing commands. "\," carries the pause the spec's integral reading
#: has ("f of x, d x"); the others are just air.
_SPACING = {",": ",", ";": " ", ":": " ", "!": "", " ": " ", "quad": " ", "qquad": " "}

#: Accents: postfix ("x hat") except the vector, which reads naturally
#: as a prefix ("the vector x").
_ACCENTS_POSTFIX = {"hat": "hat", "bar": "bar", "tilde": "tilde", "dot": "dot", "ddot": "double dot"}
_ACCENTS_PREFIX = {"vec": "the vector"}

#: Single-character symbols spoken by the atom parser.
_CHAR_WORDS = {
    "+": "plus",
    "-": "minus",
    "=": "equals",
    "<": "is less than",
    ">": "is greater than",
    "/": "over",
    "%": "percent",
    "|": "given",
    "!": "factorial",
    "′": "prime",
    "″": "double prime",
    "'": "prime",
}

_ORDINALS = {
    1: "first",
    2: "second",
    3: "third",
    4: "fourth",
    5: "fifth",
    6: "sixth",
    7: "seventh",
    8: "eighth",
    9: "ninth",
    10: "tenth",
    11: "eleventh",
    12: "twelfth",
    13: "thirteenth",
    14: "fourteenth",
    15: "fifteenth",
    16: "sixteenth",
    17: "seventeenth",
    18: "eighteenth",
    19: "nineteenth",
    20: "twentieth",
}

#: Guard against pathological nesting (and runaway recursion).
_MAX_DEPTH = 16


class _Fail(Exception):
    """Internal: this span is not confidently parseable."""


# --------------------------------------------------------------------------
# Tokenizer
# --------------------------------------------------------------------------

_TOKEN_RE = re.compile(
    r"(?P<cmd>\\[a-zA-Z]+|\\[,;:! \\{}])"
    r"|(?P<brace>[{}])"
    r"|(?P<bound>[\^_])"
    r"|(?P<num>\d+(?:\.\d+)?)"
    r"|(?P<word>[A-Za-z]+)"
    r"|(?P<ws>\s+)"
    r"|(?P<char>\S)"
)


def _tokens(source: str) -> list[tuple[str, str]]:
    """LaTeX source → (kind, value) tokens; anything odd is ``char``."""
    out: list[tuple[str, str]] = []
    for match in _TOKEN_RE.finditer(source):
        for kind, value in match.groupdict().items():
            if value is not None:
                out.append((kind, value))
                break
    return out


# --------------------------------------------------------------------------
# Parser
# --------------------------------------------------------------------------


class _Parser:
    """A small recursive-descent reader for the math models actually write.

    It does not try to understand LaTeX; it recognizes the constructs of
    §2(b) of the speech design and speaks them, and refuses (``_Fail``)
    anything else so the caller can drop the span instead of reading
    markup aloud.
    """

    def __init__(self, tokens: list[tuple[str, str]]):
        self._tokens = tokens
        self._pos = 0
        self._depth = 0

    # -- plumbing ---------------------------------------------------------

    def _peek(self) -> tuple[str, str] | None:
        if self._pos < len(self._tokens):
            return self._tokens[self._pos]
        return None

    def _skip_ws(self) -> None:
        token = self._peek()
        if token is not None and token[0] == "ws":
            self._next()

    def _next(self) -> tuple[str, str]:
        token = self._peek()
        if token is None:
            raise _Fail("unexpected end of formula")
        self._pos += 1
        return token

    def parse(self) -> str:
        """The whole token stream as spoken text."""
        return self._sequence(frozenset()).strip()

    def _sequence(self, stop: frozenset[str]) -> str:
        """Spoken text for the tokens ahead, until a closer in ``stop``.

        ``stop`` holds closing delimiter strings ("}", "]", ")") that end
        this level; the closer itself is left for the caller to consume.
        Whitespace between atoms is air, not content.
        """
        if self._depth > _MAX_DEPTH:
            raise _Fail("formula nests too deeply")
        self._depth += 1
        try:
            parts: list[str] = []
            while True:
                token = self._peek()
                if token is None:
                    break
                if token[0] == "ws":
                    self._next()
                    continue
                if token[1] in stop and token[0] != "cmd":
                    break
                parts.append(self._atom())
            return _join(parts)
        finally:
            self._depth -= 1

    # -- grammar ----------------------------------------------------------

    def _atom(self) -> str:
        kind, value = self._next()
        if kind == "brace":
            if value == "}":
                # Unbalanced close: nothing here makes sense as speech.
                raise _Fail("stray closing brace")
            # A bare group is just grouping: speak its content.
            inner = self._sequence({"}"})
            self._expect_close()
            return self._postfix(inner)

        if kind == "bound":
            # A leading ^/_ (no base) — not something we can speak.
            raise _Fail("subscript or superscript without a base")

        if kind == "cmd":
            return self._postfix(self._command(value))

        if kind == "num":
            return self._postfix(value)

        if kind == "word":
            # A bare "sin(x)" (no backslash) still reads as a function.
            if value.lower() in _FUNCTIONS and value not in ("max", "min"):
                return self._postfix(_FUNCTIONS[value.lower()])
            following = self._peek()
            if following is not None and following == ("char", "("):
                # f(x) reads as "f of x"; sin(…) likewise.
                self._next()
                inner = self._sequence({")"})
                closing = self._peek()
                if closing != ("char", ")"):
                    raise _Fail("unbalanced parentheses")
                self._next()
                return self._postfix(f"{_spell(value)} of {inner}")
            return self._postfix(_spell(value))

        # kind == "char"
        return self._postfix(self._char_atom(value))

    def _char_atom(self, value: str) -> str:
        if value == "(":
            inner = self._sequence({")"})
            closing = self._peek()
            if closing != ("char", ")"):
                raise _Fail("unbalanced parentheses")
            self._next()
            return f"the quantity {inner}"
        if value == "[":
            inner = self._sequence({"]"})
            closing = self._peek()
            if closing != ("char", "]"):
                raise _Fail("unbalanced brackets")
            self._next()
            return inner
        if value == "]":
            raise _Fail("stray closing bracket")
        if value == "&":
            # Alignment tabs belong to aligned derivations, which the
            # display heuristic already keeps silent.
            raise _Fail("alignment tab")
        if value in _CHAR_WORDS:
            return _CHAR_WORDS[value]
        if value == ".":
            # Not inside a number (those tokenize whole): a bare dot in
            # math reads as the product, "x dot y".
            return "dot"
        if value in ",;:":
            return value
        return value

    def _expect_close(self) -> None:
        token = self._peek()
        if token != ("brace", "}"):
            raise _Fail("unbalanced braces")
        self._next()

    def _postfix(self, base: str) -> str:
        """Consume ^/_ bounds following an atom ("x sub i squared")."""
        result = base
        while True:
            self._skip_ws()
            token = self._peek()
            if token is None or token[0] != "bound":
                return result
            self._next()
            argument = self._argument()
            if token[1] == "_":
                result = f"{result} sub {argument}"
            else:
                result = self._power(result, argument)

    def _power(self, base: str, exponent: str) -> str:
        if exponent == "2":
            return f"{base} squared"
        if exponent == "3":
            return f"{base} cubed"
        stripped = exponent.replace(" ", "")
        if stripped.isdigit():
            return f"{base} to the {_ordinal(int(stripped))}"
        return f"{base} to the {exponent}"

    def _argument(self) -> str:
        """One ^/_ or command argument: a braced group or a single atom."""
        self._skip_ws()
        token = self._peek()
        if token is None:
            raise _Fail("missing argument")
        if token == ("brace", "{"):
            self._next()
            inner = self._sequence({"}"})
            self._expect_close()
            return inner
        if token[0] == "brace" and token[1] == "}":
            raise _Fail("empty argument")
        # A single token: numbers stay, letters are spelled, commands run.
        kind, value = self._next()
        if kind == "cmd":
            return self._command(value)
        if kind == "num":
            return value
        if kind == "word":
            return _spell(value)
        if kind == "bound":
            raise _Fail("nested bound signs")
        return self._char_atom(value)

    # -- commands ---------------------------------------------------------

    def _command(self, name: str) -> str:
        command = name[1:]
        if command == "\\":  # a derivation's row break
            raise _Fail("row break: derivation territory")
        if command in ("begin", "end"):
            # Environments (aligned, cases, matrices) are derivations.
            raise _Fail("environment: derivation territory")
        if command in _SPACING:
            return _SPACING[command]
        if command in ("frac", "tfrac", "dfrac"):
            numerator = self._argument()
            denominator = self._argument()
            return f"{numerator} over {denominator}"
        if command == "binom":
            upper = self._argument()
            lower = self._argument()
            return f"{upper} choose {lower}"
        if command == "sqrt":
            return self._sqrt()
        if command in _BIG_OPERATORS:
            return self._big_operator(command)
        if command == "lim":
            return self._limit()
        if command in _FUNCTIONS:
            return self._function(command)
        if command in _TEXTUAL:
            return self._literal_group()
        if command == "mathbb":
            return self._blackboard()
        if command in _STYLE:
            # \left, \big, \mathbf…: speak what is inside / what follows.
            self._skip_ws()
            token = self._peek()
            if token == ("brace", "{"):
                self._next()
                inner = self._sequence({"}"})
                self._expect_close()
                return inner
            return ""
        if command in _ACCENTS_POSTFIX:
            return f"{self._argument()} {_ACCENTS_POSTFIX[command]}"
        if command in _ACCENTS_PREFIX:
            return f"{_ACCENTS_PREFIX[command]} {self._argument()}"
        if command == "pmod":
            return f"mod {self._argument()}"
        if command in _COMMAND_WORDS:
            return _COMMAND_WORDS[command]
        # Everything else: rather than guess what "\ grooming" might
        # mean, refuse the span.
        raise _Fail(f"unknown command {command!r}")

    def _sqrt(self) -> str:
        degree = None
        self._skip_ws()
        token = self._peek()
        if token == ("char", "["):
            self._next()
            degree = self._sequence({"]"})
            closing = self._peek()
            if closing != ("char", "]"):
                raise _Fail("unbalanced root degree")
            self._next()
        body = self._argument()
        if degree is None or degree in ("2",):
            return f"the square root of {body}"
        if degree == "3":
            return f"the cube root of {body}"
        if degree.isdigit():
            return f"the {_ordinal(int(degree))} root of {body}"
        return f"the {degree} root of {body}"

    def _big_operator(self, command: str) -> str:
        word = _BIG_OPERATORS[command]
        lower = upper = None
        while True:
            token = self._peek()
            if token is None or token[0] != "bound":
                break
            self._next()
            if token[1] == "_":
                lower = self._argument()
            else:
                upper = self._argument()
        if lower is not None and upper is not None:
            return f"{word} from {lower} to {upper} of"
        if lower is not None:
            return f"{word} over {lower} of"
        if upper is not None:
            return f"{word} to {upper} of"
        return f"{word} of"

    def _limit(self) -> str:
        word = "the limit"
        while True:
            token = self._peek()
            if token is None or token[0] != "bound":
                break
            self._next()
            if token[1] == "_":
                return f"{word} as {self._argument()} of"
            self._argument()  # a superscript on \lim has no speech
        return f"{word} of"

    def _function(self, command: str) -> str:
        word = _FUNCTIONS[command]
        self._skip_ws()
        token = self._peek()
        if token == ("char", "("):
            # sin(x) reads "sine of x", like the bare-word f(x).
            self._next()
            inner = self._sequence({")"})
            closing = self._peek()
            if closing != ("char", ")"):
                raise _Fail("unbalanced parentheses")
            self._next()
            word = f"{word} of {inner}"
            self._skip_ws()
            token = self._peek()
        # sin^{-1} is spoken "arc sine", not "sine to the minus one".
        if token == ("bound", "^"):
            self._next()
            exponent = self._argument()
            if exponent.replace(" ", "") in ("-1", "minus1") and not word.startswith(
                "arc"
            ):
                return f"arc {word}"
            return self._power(word, exponent)
        return word

    def _literal_group(self) -> str:
        """Read a braced group literally (\\text{…} and friends)."""
        token = self._peek()
        if token != ("brace", "{"):
            raise _Fail("text command without a group")
        self._next()
        depth = 1
        pieces: list[str] = []
        while True:
            kind, value = self._next()
            if kind == "brace":
                if value == "{":
                    depth += 1
                else:
                    depth -= 1
                    if depth == 0:
                        break
            pieces.append(value if kind != "cmd" else value[1:])
        return "".join(pieces).strip()

    def _blackboard(self) -> str:
        body = self._literal_group()
        return _BLACKBOARD.get(body.strip(), _spell(body))


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------


def _spell(word: str) -> str:
    """Variables are single letters: "dx" is spoken "d x"."""
    return " ".join(word)


def _ordinal(number: int) -> str:
    if number in _ORDINALS:
        return _ORDINALS[number]
    if number % 100 in (11, 12, 13):
        return f"{number}th"
    if number % 10 == 1:
        return f"{number}st"
    if number % 10 == 2:
        return f"{number}nd"
    if number % 10 == 3:
        return f"{number}rd"
    return f"{number}th"


def _join(parts: list[str]) -> str:
    """Join spoken fragments; punctuation attaches to the previous word."""
    out = ""
    for part in parts:
        if not part:
            continue
        if out and part[0] in ",.;:!?":
            out += part
        elif out:
            out += " " + part
        else:
            out = part
    return out


def _latexify(source: str) -> str:
    """Rewrite Unicode math symbols into their LaTeX commands."""
    for symbol, command in UNICODE_LATEX.items():
        if symbol in source:
            source = source.replace(symbol, f" {command} ")
    return source


# The spoken word for each Unicode symbol (the English direction of the
# renderer's table), derived by speaking the command it maps to. Built
# lazily at first use so importing this module stays side-effect free.
_unicode_spoken: dict[str, str] | None = None


def unicode_spoken() -> dict[str, str]:
    """Symbol → spoken word for every entry of ``UNICODE_LATEX``.

    Derived rather than hand-maintained: each symbol's spoken form is
    what the parser says for its LaTeX command, so the Unicode table and
    the command table can never disagree. Argument-taking commands are
    named directly (a bare "√" has nothing to be the root *of*).
    """
    global _unicode_spoken
    if _unicode_spoken is None:
        table: dict[str, str] = {}
        for symbol, command in UNICODE_LATEX.items():
            body = command[1:]  # without the backslash
            if body.startswith("mathbb"):
                letter = body[-2]  # the X of mathbb{X}
                table[symbol] = _BLACKBOARD.get(letter, _spell(letter))
            elif body == "sqrt":
                table[symbol] = "the square root of"
            elif body in _BIG_OPERATORS:
                table[symbol] = f"{_BIG_OPERATORS[body]} of"
            elif body in _COMMAND_WORDS:
                table[symbol] = _COMMAND_WORDS[body]
            else:
                table[symbol] = spoken_math(command)
        _unicode_spoken = table
    return _unicode_spoken


def spoken_math(source: str) -> str:
    """One math span's content as spoken text; "" when not parseable.

    The empty string is the caller's cue to drop the span silently.
    Unicode symbols the models write directly (∫, α, → …) are accepted
    alongside LaTeX commands.
    """
    try:
        return _Parser(_tokens(_latexify(source))).parse()
    except _Fail:
        log.debug("unparseable math span dropped: %r", source[:80])
        return ""
    except RecursionError:  # pragma: no cover - defensive
        log.debug("runaway math span dropped: %r", source[:80])
        return ""


def spoken_span(source: str, *, display: bool) -> str:
    """A whole span (content only) as speakable text, display heuristic included.

    Display math over :data:`DISPLAY_SOURCE_LIMIT` source characters is a
    derivation, not a formula: it is announced ("a displayed formula")
    and the rendered image carries the detail. Inline math is always
    spoken in full.
    """
    if display and len(source.strip()) > DISPLAY_SOURCE_LIMIT:
        return DISPLAY_SPOKEN
    return spoken_math(source)
