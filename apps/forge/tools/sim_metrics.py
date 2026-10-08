#!/usr/bin/env python3
"""The metric names a stage's sim produces, read from the C++ so the list cannot go stale.

    sim_metrics.py                          every live stage's metric count and any construct it could not resolve
    sim_metrics.py --stage move2_seek       the stage's names, one a line ("family" ones marked)
    sim_metrics.py --check stage.json ...   compare the extraction with the episode_info of real stage.json files

A learner yaml names metrics (status.headline, eval.report, fade.gate_metric ...). Each must be a column the stage's
sim reports (an episode info name, or a reward_<term> column) or one the learner derives itself (animus.evaluation).
Nothing misses loudly at run time: a gate on a name no episode reports reads as "never met" and the ladder never steps.

Where the names come from (src/server/game/Animus/Scenario/Curriculum):
  * Stages/Stages.cpp           each stage's arenas: which Opposition they play against and their flags.
  * StageScenario.cpp           which encounter a stage builds for them (the `add(std::make_unique<...>)` lines and the
                                `AnyArena(...)` conditions before them), AddCoreEpisodeInfo, the constructor's own
                                columns, and the reward terms every stage pays.
  * StandInSeat.cpp             AddStandInEpisodeInfo.
  * Encounters/*.cpp            <Encounter>::AddEpisodeInfo and <Encounter>::RewardTerms.
  * Rewards/CombatReward.cpp    RewardTermName: the term -> name table behind the reward_<term> columns.

A name built at run time is resolved where the source says what it is built from (a loop over a literal list, an
array such as RUNG_NAMES, a switch such as GoalName). One that cannot be is a *family* (a wildcard) and is listed under
`unresolved`; tests/test_metric_names.py fails on any it has not been told about, so a new construct is looked at by a
person rather than silently allowed. The extraction is a superset of one stage's columns where the C++ guards a column
with a condition (`if (_stage.Has(BlockId::Vision))`): for the exact list, point --check at the stage.json the built
binary writes (its `episode_info`), which is what the deploy gate does.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
ANIMUS = REPO / "src" / "server" / "game" / "Animus"
CURRICULUM = ANIMUS / "Scenario" / "Curriculum"
# The runtime half of the curriculum (Stages.cpp, the blocks, the layout) lives under Animus/Runtime.
RUNTIME_CURRICULUM = ANIMUS / "Runtime" / "Scenario" / "Curriculum"
PYTHON_DIR = REPO / "apps" / "forge" / "python"

ANY = "[a-z0-9_]+"
NUMBER = "[0-9]+"
ADD_CALL = re.compile(r"\b(?:table|_info)\.Add\(")


# --------------------------------------------------------------------------------------------- C++ text helpers

def strip_comments(text: str) -> str:
    """The source without // and /* */ comments, every character kept in place (as a space) so offsets and line
    numbers still match the file. String and character literals are left alone."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out[i:j] = " " * (j - i)
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i)
            j = n if j < 0 else j + 2
            out[i:j] = [" " if ch != "\n" else "\n" for ch in text[i:j]]
            i = j
        else:
            i += 1
    return "".join(out)


def match_close(text: str, start: int) -> int:
    """The index of the bracket closing the one at `start` ((, { or [), skipping string literals."""
    pairs = {"(": ")", "{": "}", "[": "]"}
    stack = [pairs[text[start]]]
    i = start + 1
    while i < len(text) and stack:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < len(text) and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j
        elif c in pairs:
            stack.append(pairs[c])
        elif c == stack[-1]:
            stack.pop()
        i += 1
    if stack:
        raise ValueError("unbalanced brackets")
    return i - 1


def split_top(text: str, separator: str) -> list[str]:
    """`text` split at the separator wherever it is not inside brackets or a string."""
    parts, depth, last, i = [], 0, 0, 0
    while i < len(text):
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < len(text) and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j
        elif c in "({[":
            depth += 1
        elif c in ")}]":
            depth -= 1
        elif c == separator and depth == 0:
            parts.append(text[last:i])
            last = i + 1
        i += 1
    parts.append(text[last:])
    return [part.strip() for part in parts]


def function_body(text: str, header: str) -> tuple[str, int]:
    """(the text between the braces of the function whose header matches the regex, its offset in `text`)."""
    found = re.search(header, text, re.M)
    if not found:
        raise LookupError(f"no function matching {header!r}")
    paren = text.index("(", found.start())
    brace = text.index("{", match_close(text, paren))
    end = match_close(text, brace)
    return text[brace + 1:end], brace + 1


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


# ------------------------------------------------------------------------------------------------- the names

@dataclass
class Names:
    """A set of column names: exact ones and families (regexes for names built at run time)."""
    exact: set[str] = field(default_factory=set)
    families: list[str] = field(default_factory=list)

    def update(self, other: "Names") -> None:
        self.exact |= other.exact
        for family in other.families:
            if family not in self.families:
                self.families.append(family)

    def match(self, name: str) -> str:
        """"exact", "family" or "" for a name."""
        if name in self.exact:
            return "exact"
        if any(re.fullmatch(family, name) for family in self.families):
            return "family"
        return ""

    def __contains__(self, name: str) -> bool:
        return bool(self.match(name))


@dataclass
class Unresolved:
    where: str
    expression: str

    def __str__(self) -> str:
        return f"{self.where}: {self.expression}"


@dataclass
class _Loop:
    start: int
    header: str


class Source:
    """The curriculum's C++, read once, comments blanked."""

    def __init__(self, root: Path = CURRICULUM, runtime_root: Path | None = RUNTIME_CURRICULUM):
        self.root = root
        self.roots = [root] + ([runtime_root] if runtime_root is not None else [])
        self.files = {path: strip_comments(path.read_text()) for base in self.roots for path in sorted(base.rglob("*"))
                      if path.suffix in (".cpp", ".h")}
        self.unresolved: list[Unresolved] = []

    def path(self, relative: str) -> Path:
        """The file a curriculum-relative path names, in the training tree or under Runtime."""
        for base in self.roots:
            if base / relative in self.files:
                return base / relative
        raise KeyError(relative)

    def relative(self, path: Path) -> str:
        for base in self.roots:
            if path.is_relative_to(base):
                return str(path.relative_to(base))
        return str(path)

    def text(self, relative: str) -> str:
        return self.files[self.path(relative)]

    def where(self, path: Path, offset: int) -> str:
        shown = path.relative_to(REPO) if path.is_relative_to(REPO) else path
        return f"{shown}:{line_of(self.files[path], offset)}"

    def ordered(self, path: Path | None) -> list[str]:
        """Every file's text, the ones `path` includes (and its own) first: a symbol defined in two encounters'
        headers (RUNG_NAMES) means the one the encounter in hand includes."""
        if path is None:
            return list(self.files.values())
        own = self.files[path]
        included = {name for name in re.findall(r'#include "([^"]+)"', own)}
        first = [text for other, text in self.files.items() if other == path or other.name in included]
        return first + [text for text in self.files.values() if not any(text is f for f in first)]

    def definition_literals(self, symbol: str, path: Path | None = None) -> list[str] | None:
        """The string literals a symbol is defined as: `SYMBOL[...] = { "a", "b" }` (an array), or the `return "x";`
        literals of a function `Symbol(...)` with a body (a name switch)."""
        for text in self.ordered(path):
            array = re.search(rf"\b{re.escape(symbol)}\b[^;(){{}}]*=\s*(?:std::array<[^>]*>\s*)?\{{", text)
            if array:
                brace = text.index("{", array.end() - 1)
                return re.findall(r'"([a-z0-9_]+)"', text[brace:match_close(text, brace) + 1])
        for text in self.ordered(path):
            # A definition starts in column 0 (its return type and namespace first); a call site does not.
            function = re.search(rf"^(?!\s)[^\n;=]*\b{re.escape(symbol)}\([^;{{}}]*?\)\s*(?:const\s*)?\n?\{{", text, re.M)
            if function:
                brace = text.index("{", function.end() - 1)
                found = re.findall(r'return\s+"([a-z0-9_]+)"\s*;', text[brace:match_close(text, brace) + 1])
                if found:
                    return found
        return None


class Extractor:
    def __init__(self, source: Source | None = None):
        self.source = source or Source()
        self.scenario = self.source.text("StageScenario.cpp")
        self.scenario_path = self.source.path("StageScenario.cpp")

    # ----------------------------------------------------------- dynamic expressions

    def resolve(self, expression: str, text: str, offset: int, path: Path) -> str | None:
        """A regex for what a non-literal piece of a name can be, or None when the source does not say."""
        expression = expression.strip()
        # A call such as std::string(GoalName(SeatGoal(kind))) or AimlessCauseName(...): the innermost name function.
        calls = re.findall(r"\b(\w*Name)\(", expression)
        if calls:
            literals = self.source.definition_literals(calls[-1], path)
            if literals:
                return "(?:" + "|".join(re.escape(literal) for literal in literals) + ")"
        if re.fullmatch(r"std::move\((\w+)\)", expression):
            expression = expression[len("std::move("):-1]
        if re.fullmatch(r"std::string\((\w+)\)", expression):
            expression = expression[len("std::string("):-1]
        if not re.fullmatch(r"[A-Za-z_]\w*", expression):
            return self.member(expression, text, offset)
        name = expression
        before = text[:offset]
        # A loop that counts: `for (uint32 mark = 0; ...)`.
        counter = list(re.finditer(rf"for\s*\(\s*(?:u?int\d*|size_t|std::size_t|auto)\s+{name}\s*=", before))
        # `name = Draw::RUNG_NAMES[rung];` or `std::string const name = "x" + y;`
        assign = [m for m in re.finditer(rf"\b{name}\s*=\s*([^;]+);", before)
                  if not re.search(r"for\s*\([^;]*$", before[max(0, m.start() - 40):m.start()])]
        # A range loop: `for (auto const& [name, drill] : ROLES)` or `: { std::pair{ "won_hold", ... }, ... }`.
        ranged = None
        for opening in reversed(list(re.finditer(r"for\s*\(", before))):
            paren = opening.end() - 1
            try:
                header = text[paren + 1:match_close(text, paren)]
            except ValueError:
                continue
            parts = split_top(re.sub(r"::", "\x00\x00", header), ":")
            parts = [part.replace("\x00\x00", "::") for part in parts]
            if len(parts) == 2 and ";" not in parts[0] and re.search(rf"\b{name}\b", parts[0]):
                ranged = _Loop(opening.start(), header)
                break
        candidates = [(m.start(), "count") for m in counter] + [(m.start(), "assign") for m in assign] + \
                     ([(ranged.start, "range")] if ranged else [])
        if not candidates:
            return None
        _, kind = max(candidates)
        if kind == "count":
            return NUMBER
        if kind == "assign":
            right = assign[-1].group(1)
            array = re.search(r"(\w+)\s*\[", right)
            if array and (literals := self.source.definition_literals(array.group(1), path)):
                return "(?:" + "|".join(re.escape(literal) for literal in literals) + ")"
            literal = re.fullmatch(r'\s*"([a-z0-9_]+)"\s*', right)
            return re.escape(literal.group(1)) if literal else None
        header = ranged.header
        sequence = header.split(":", 1)[1].strip()
        if re.fullmatch(r"\w+", sequence):
            body = self.source.definition_literals(sequence, path)
            if body is None:
                return None
            literals = body
            # A table of pairs ({ "tank", DUNGEON_TANK }): the first literal of each brace group is the name.
            for text_ in self.source.files.values():
                match = re.search(rf"\b{sequence}\b[^;]*=\s*", text_)
                if match:
                    start = text_.index("{", match.end() - 1)
                    group = text_[start:match_close(text_, start) + 1]
                    if re.search(r"\{\s*\{", group):
                        literals = re.findall(r'\{\s*"([a-z0-9_]+)"', group)
                    break
        else:
            literals = re.findall(r'\{\s*"([a-z0-9_]+)"', sequence) or re.findall(r'"([a-z0-9_]+)"', sequence)
        return "(?:" + "|".join(re.escape(literal) for literal in literals) + ")" if literals else None

    def member(self, expression: str, text: str, offset: int) -> str | None:
        """`boss.Name` over `for (auto const& boss : WingBosses())`: the member's literals where the table is."""
        found = re.fullmatch(r"(\w+)\.(\w+)", expression)
        if not found:
            return None
        variable, member = found.groups()
        loop = list(re.finditer(rf"for\s*\([^;:]*\b{variable}\s*:\s*(\w+)\(\)", text[:offset]))
        if not loop:
            return None
        function = loop[-1].group(1)
        for path, body in self.source.files.items():
            if path.suffix != ".cpp" or not re.search(rf"^(?!\s)[^\n;=]*\b{function}\(\)", body, re.M):
                continue
            definition, _ = function_body(body, rf"^(?!\s)[^\n;=]*\b{function}\(\)")
            # Rows written `.Name = "x"`, or positionally with the name last: `{ 389, 11517, "oggleflint" }`.
            literals = re.findall(rf'\.{member} = "([^"]+)"', definition) or \
                re.findall(r'\{[^{}]*?"([^"]+)"\s*\}', definition)
            if literals:
                return "(?:" + "|".join(re.escape(literal) for literal in dict.fromkeys(literals)) + ")"
        return None

    def name_pattern(self, argument: str, text: str, offset: int, path: Path) -> tuple[str, bool]:
        """(regex of the names one Add call adds, whether it is a plain literal)."""
        argument = argument.strip()
        literal = re.fullmatch(r'"([a-z0-9_]+)"', argument)
        if literal:
            return re.escape(literal.group(1)), True
        format_call = re.fullmatch(r'Acore::StringFormat\(\s*"([^"]*)"\s*,(.*)\)', argument, re.S)
        if format_call:
            arguments = split_top(format_call.group(2), ",")
            pieces = format_call.group(1).split("{}")
            if len(pieces) - 1 != len(arguments):
                raise ValueError(f"format with {len(pieces) - 1} fields and {len(arguments)} arguments: {argument}")
            out = re.escape(pieces[0])
            for value, rest in zip(arguments, pieces[1:]):
                part = self.resolve(value, text, offset, path)
                if part is None:
                    part = ANY
                    self.source.unresolved.append(Unresolved(self.source.where(path, offset), value))
                out += part + re.escape(rest)
            return out, False
        out = ""
        for token in split_top(argument, "+"):
            literal = re.fullmatch(r'(?:std::string\(\s*)?"([a-z0-9_]*)"\s*\)?', token)
            if literal:
                out += re.escape(literal.group(1))
                continue
            part = self.resolve(token, text, offset, path)
            if part is None:
                part = ANY
                self.source.unresolved.append(Unresolved(self.source.where(path, offset), token))
            out += part
        return out, False

    # ----------------------------------------------------------- columns of one function

    def adds_in(self, text: str, path: Path, start: int = 0, end: int | None = None) -> Names:
        """The names of the `table.Add(` / `_info.Add(` calls of `text[start:end]`."""
        names = Names()
        end = len(text) if end is None else end
        for call in ADD_CALL.finditer(text, start, end):
            paren = call.end() - 1
            arguments = split_top(text[paren + 1:match_close(text, paren)], ",")
            name_argument = arguments[0]
            # The reward_<term> columns are made by RewardColumns() from the encounters' RewardTerms.
            if re.search(r"\bstd::move\(name\)", name_argument) and '"reward_"' in text[max(0, call.start() - 400):call.start()]:
                continue
            if name_argument.startswith('"reward_" +'):
                continue
            pattern, plain = self.name_pattern(name_argument, text, call.start(), path)
            unescape = lambda text_: re.sub(r"\\(.)", r"\1", text_)  # noqa: E731
            plain_text = r"(?:[^()\\\[\]+*?|]|\\.)*"
            alternation = re.fullmatch(rf"({plain_text})\(\?:((?:[^()|\\]|\\.)*(?:\|(?:[^()|\\]|\\.)*)*)\)({plain_text})",
                                       pattern)
            if plain:
                names.exact.add(unescape(pattern))
            elif alternation:
                # One alternation of literals: its names are known, so they are exact names, not a family.
                names.exact.update(unescape(alternation.group(1)) + unescape(option) + unescape(alternation.group(3))
                                   for option in re.split(r"(?<!\\)\|", alternation.group(2)))
            else:
                names.families.append(pattern)
        return names

    def function_names(self, relative: str, header: str) -> Names:
        path = self.source.path(relative)
        text = self.source.files[path]
        body, offset = function_body(text, header)
        return self.adds_in(text, path, offset, offset + len(body))

    def encounter_file(self, cls: str) -> str:
        for path, text in self.source.files.items():
            if re.search(rf"\b{cls}::AddEpisodeInfo\(", text) and path.suffix == ".cpp":
                return self.source.relative(path)
        raise LookupError(f"no {cls}::AddEpisodeInfo in the curriculum")

    def reward_term_names(self) -> dict[str, str]:
        """RewardTerm enumerator -> its name, from RewardTermName's switch."""
        text = self.source.text("Rewards/CombatReward.cpp")
        body, _ = function_body(text, r"RewardTermName\(RewardTerm term\)")
        return dict(re.findall(r'case RewardTerm::(\w+):\s*return "([a-z0-9_]+)"', body))

    def terms_of(self, cls: str) -> list[str]:
        for text in self.source.files.values():
            if re.search(rf"\b{cls}::RewardTerms\(\) const", text):
                body, _ = function_body(text, rf"{cls}::RewardTerms\(\) const")
                return re.findall(r"RewardTerm::(\w+)", body)
        return []

    def global_terms(self) -> list[str]:
        """The terms every stage pays, listed in the scenario's constructor."""
        found = re.search(r"for \(RewardTerm term : \{([^}]*)\}\)\s*_info\.Add\(\"reward_\"", self.scenario)
        if not found:
            raise LookupError("StageScenario.cpp no longer lists the reward terms every stage pays")
        return re.findall(r"RewardTerm::(\w+)", found.group(1))

    # ----------------------------------------------------------- stages

    def stages(self) -> dict[str, list[dict]]:
        """Stage name -> its arenas as {"against": Opposition, flags...}, from Stages.cpp."""
        text = self.source.text("Stages/Stages.cpp")
        marks = list(re.finditer(r'^\s+\.Name = "(\w+)",\s*$', text, re.M))
        out = {}
        for index, mark in enumerate(marks):
            block = text[mark.end():marks[index + 1].start() if index + 1 < len(marks) else len(text)]
            arenas_at = block.find(".Arenas = {")
            if arenas_at < 0:
                continue
            brace = block.index("{", arenas_at)
            arenas_text = block[brace + 1:match_close(block, brace)]
            arenas = []
            for part in split_top(arenas_text, ","):
                if not part.startswith("{"):
                    continue
                arena = {"against": (re.search(r"\.Against = Opposition::(\w+)", part) or [None, "Combat"])[1],
                         "name": (re.search(r'\.Name = "(\w+)"', part) or [None, ""])[1]}
                arena.update({flag: True for flag in re.findall(r"\.(\w+) = true", part)})
                arenas.append(arena)
            out[mark.group(1)] = arenas
        return out

    def encounter_classes(self) -> list[tuple[str, str]]:
        """[(class, predicate)] in the order the scenario builds them: the arena predicate as source text."""
        out = []
        for built in re.finditer(r"add\(std::make_unique<(\w+)>", self.scenario):
            before = self.scenario[max(0, built.start() - 300):built.start()]
            condition = list(re.finditer(r"AnyArena\(", before))[-1]
            opening = before.index("(", condition.start() + len("AnyArena") - 1)
            argument = before[opening + 1:]
            argument = argument.strip()
            if argument.startswith("["):
                predicate = re.search(r"return ([^;]+);", argument)
            else:
                name = re.match(r"\w+", argument).group(0)
                predicate = re.search(rf"auto const {name} = \[[^\]]*\]\([^)]*\)\s*\{{\s*return ([^;]+);",
                                      self.scenario)
            if not predicate:
                raise LookupError(f"cannot read the arena predicate before add<{built.group(1)}>")
            out.append((built.group(1), predicate.group(1).strip()))
        return out

    @staticmethod
    def satisfies(predicate: str, arena: dict) -> bool:
        against = re.fullmatch(r"arena\.Against == Opposition::(\w+)", predicate)
        if against:
            return arena["against"] == against.group(1)
        flag = re.fullmatch(r"arena\.(\w+)", predicate)
        if flag:
            return bool(arena.get(flag.group(1)))
        raise LookupError(f"an arena predicate this reader does not know: {predicate!r}")

    def classes_of(self, stage: str) -> list[str]:
        arenas = self.stages()[stage]
        return [cls for cls, predicate in self.encounter_classes()
                if any(self.satisfies(predicate, arena) for arena in arenas)]

    # ----------------------------------------------------------- the whole

    def columns(self, stage: str) -> Names:
        """Every episode info column the stage's scenario can report."""
        names = Names()
        names.update(self.function_names("StageScenario.cpp", r"StageScenario::AddCoreEpisodeInfo\(\)"))
        names.update(self.function_names("Encounters/StandInSeat.cpp", r"StageScenario::AddStandInEpisodeInfo\(\)"))
        for cls in self.classes_of(stage):
            names.update(self.function_names(self.encounter_file(cls), rf"{cls}::AddEpisodeInfo\("))
        # What the constructor adds after the encounters: the camera's render width and the outcome score.
        tail_start = self.scenario.index("AddStandInEpisodeInfo();")
        tail_end = self.scenario.index("_spec.EpisodeInfoDim = _info.Size();")
        names.update(self.adds_in(self.scenario, self.scenario_path, tail_start, tail_end))
        # The reward_<term> columns: the encounters' terms, then the ones every stage pays.
        terms = self.reward_term_names()
        for cls in self.classes_of(stage):
            for term in self.terms_of(cls):
                names.exact.add("reward_" + terms[term])
        for term in self.global_terms():
            names.exact.add("reward_" + terms[term])
        return names

    def reward_terms(self) -> set[str]:
        return set(self.reward_term_names().values())


def stage_json_names(path: Path) -> set[str]:
    return set(json.loads(path.read_text()).get("episode_info", ()))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--stage", help="print this stage's names")
    parser.add_argument("--check", nargs="+", type=Path, metavar="STAGE_JSON",
                        help="compare the extraction with these stage.json files' episode_info (exit 1 on a name "
                             "the extraction lacks)")
    args = parser.parse_args()
    extractor = Extractor()
    stages = extractor.stages()
    if args.check:
        bad = 0
        for path in args.check:
            stage = json.loads(path.read_text())["stage"]
            have = stage_json_names(path)
            names = extractor.columns(stage)
            missing = sorted(name for name in have if name not in names)
            extra = sorted(names.exact - have)
            print(f"{path}: {len(have)} names in the file, {len(names.exact)} exact + {len(names.families)} families "
                  f"extracted; the file has {len(missing)} the extraction lacks; the extraction has {len(extra)} "
                  f"exact the file lacks (conditional columns)")
            for name in missing:
                print(f"  MISSING {name}")
            bad += len(missing)
        return 1 if bad else 0
    if args.stage:
        names = extractor.columns(args.stage)
        for name in sorted(names.exact):
            print(name)
        for family in names.families:
            print(f"family {family}")
        return 0
    for stage in stages:
        names = extractor.columns(stage)
        print(f"{stage}: {len(names.exact)} exact, {len(names.families)} families; encounters "
              f"{', '.join(extractor.classes_of(stage)) or '-'}")
    for item in dict.fromkeys(str(u) for u in extractor.source.unresolved):
        print(f"unresolved: {item}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
