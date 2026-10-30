#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_btl_patches.py
====================

Exécute séquentiellement les scripts de fabrication de patchs client de
"buildthehomelab" (un dépôt GitHub par module) et fusionne tous les résultats
dans UN SEUL MPQ (ex. patch-V.MPQ).

Principe
--------
1. Clone (ou met à jour) le dépôt de chaque patch dans <work>/repos/<nom>.
2. Pour chaque étape (build_patch.py, build_glue_patch.py, ...) :
   - résout le MPQ "de base" nécessaire (ex. celui qui contient déjà le
     Spell.dbc le plus récent) : d'abord ce qui a déjà été produit par les
     étapes précédentes, sinon le dernier MPQ du client qui le fournit ;
   - lance le script du dépôt, qui produit un MPQ intermédiaire ;
   - extrait son contenu dans <work>/staging (les étapes suivantes écrasent
     les fichiers identiques des étapes précédentes).
3. Empaquette <work>/staging dans le MPQ final et vérifie qu'il se charge
   bien APRÈS les MPQ du client qui fournissent les mêmes fichiers.

Prérequis
---------
* Python 3.8+, git dans le PATH.
* StormLib (StormLib.dll sur Windows, 64 bits si Python 64 bits). Le script le
  cherche à côté de lui, dans le répertoire courant, via --stormlib ou la
  variable d'environnement STORMLIB (qui est aussi transmise aux scripts du
  dépôt, qui l'utilisent pour empaqueter).

Exemples
--------
    python build_btl_patches.py --list
    python build_btl_patches.py
    python build_btl_patches.py --out patch-V.MPQ --install
    python build_btl_patches.py --repo-path forever-racials=D:\\repo\\azerothcore\\modules\\mod-forever-racials

Pour ajouter un autre patch de buildthehomelab : ajouter un objet Patch(...)
dans la liste PATCHES plus bas, dans l'ordre d'exécution voulu.
"""
from __future__ import annotations

import argparse
import ctypes
import os
import re
import shutil
import subprocess
import zlib
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Tuple

# --------------------------------------------------------------------------- #
#  Configuration des patchs (ordre = ordre d'exécution ; le dernier gagne)
# --------------------------------------------------------------------------- #

DEFAULT_CLIENT = r"C:\Jeux\World of Warcraft 3.3.5"
DEFAULT_OUT = "patch-V.MPQ"  # doit se trier APRÈS les MPQ qui fournissent les mêmes fichiers


@dataclass
class Step:
    label: str
    script: str                # chemin relatif à la racine du dépôt
    args: List[str]            # jetons : {out} {outdir} {repo} {base:A|B} (MPQ minimal) {file:A} (fichier extrait)
    kind: str = "py"           # "py" : script python lancé tel quel
                               # "sh-python" : script .sh dont on exécute le python intégré (sans bash)


@dataclass
class Patch:
    name: str
    url: str
    steps: List[Step]
    ref: Optional[str] = None  # branche / tag (défaut : branche par défaut)


DBC = "DBFilesClient\\"
SPELL = DBC + "Spell.dbc"
SKILL = DBC + "SkillLineAbility.dbc"

PATCHES: List[Patch] = [
    Patch(
        name="forever-racials",
        url="https://github.com/buildthehomelab/wow-mod-forever-racials.git",
        steps=[
            # Spell.dbc : 12 nouveaux sorts (90100-90111) + racials modifiés.
            Step("Spell.dbc (racials)", "tools/build_patch.py",
                 ["--from-mpq", "{base:" + SPELL + "}", "--out", "{out}"]),
            # GlueStrings.lua : écran de création de personnage (kits 2 + 2).
            Step("GlueStrings.lua (création de personnage)", "tools/build_glue_patch.py",
                 ["--from-mpq", r"{base:Interface\GlueXML\GlueStrings.lua}", "--out", "{out}"]),
        ],
    ),
    Patch(
        name="forever-paladin",
        url="https://github.com/buildthehomelab/wow-mod-forever-paladin.git",
        steps=[
            # Spell.dbc + SkillLineAbility.dbc + GlyphProperties.dbc + Item.dbc
            Step("DBC paladin (Holy Strike, Seal of Fury, glyphe...)", "tools/build_patch.py",
                 ["--from-mpq",
                  "{base:" + "|".join([SPELL, SKILL, DBC + "GlyphProperties.dbc", DBC + "Item.dbc"]) + "}",
                  "--out", "{out}"]),
        ],
    ),
    Patch(
        name="forever-shaman",
        url="https://github.com/buildthehomelab/wow-mod-forever-shaman.git",
        steps=[
            # Mining en loup fantôme + loup fantôme en intérieur (Spell.dbc)
            Step("Spell.dbc (loup fantôme)", "tools/patch-forever-shaman-dbc.sh",
                 ["{file:" + SPELL + "}", "{outdir}"], kind="sh-python"),
        ],
    ),
    Patch(
        name="forever-druid",
        url="https://github.com/buildthehomelab/wow-mod-forever-druid.git",
        steps=[
            # Balayage, Pulvérisation, consommables/minage en forme, forme de voyage (Spell + SkillLineAbility)
            Step("Spell.dbc + SkillLineAbility.dbc (druide)", "tools/patch-forever-druid-dbc.sh",
                 ["{file:" + SPELL + "}", "{file:" + SKILL + "}", "{outdir}"], kind="sh-python"),
        ],
    ),
    # forever-rogue et forever-hunter : pas de patch client.
]


# --------------------------------------------------------------------------- #
#  Petit wrapper ctypes autour de StormLib
# --------------------------------------------------------------------------- #

MPQ_OPEN_READ_ONLY = 0x00000100
MPQ_CREATE_LISTFILE = 0x00100000
MPQ_CREATE_ATTRIBUTES = 0x00200000
MPQ_CREATE_ARCHIVE_V2 = 0x01000000
MPQ_FILE_COMPRESS = 0x00000200
MPQ_FILE_REPLACEEXISTING = 0x80000000
MPQ_COMPRESSION_ZLIB = 0x02


class StormError(RuntimeError):
    pass


class StormLib:
    def __init__(self, hint: Optional[str], extra_dirs: List[Path]):
        self.path = self._locate(hint, extra_dirs)
        loader = ctypes.WinDLL if os.name == "nt" else ctypes.CDLL
        try:
            self.lib = loader(str(self.path))
        except OSError as exc:
            raise StormError(
                f"Impossible de charger StormLib ({self.path}) : {exc}\n"
                "Vérifie que la DLL a la même architecture que Python (64 bits / 32 bits)."
            )
        L = self.lib
        for fn in (L.SFileOpenArchive, L.SFileCreateArchive, L.SFileCloseArchive, L.SFileHasFile,
                   L.SFileFindNextFile, L.SFileFindClose, L.SFileExtractFile, L.SFileAddFileEx):
            fn.restype = ctypes.c_ubyte
        L.SFileFindFirstFile.restype = ctypes.c_void_p
        L.SFileFindFirstFile.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_char_p]
        L.SFileFindNextFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        L.SFileFindClose.argtypes = [ctypes.c_void_p]
        L.SFileHasFile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        L.SFileCloseArchive.argtypes = [ctypes.c_void_p]
        # Mode des chemins (TCHAR large ou ANSI), détecté à la 1re ouverture réussie.
        self.mode: Optional[str] = None if os.name == "nt" else "ansi"

    # -- localisation de la bibliothèque ---------------------------------- #
    @staticmethod
    def _names() -> List[str]:
        if os.name == "nt":
            return ["StormLib.dll", "stormlib.dll", "storm.dll", "libstorm.dll"]
        if sys.platform == "darwin":
            return ["libstorm.dylib", "libstorm.2.dylib"]
        return ["libstorm.so", "libstorm.so.1", "libstorm.so.9"]

    def _locate(self, hint: Optional[str], extra_dirs: List[Path]) -> Path | str:
        candidates: List[str] = []
        for raw in (hint, os.environ.get("STORMLIB")):
            if raw:
                candidates.append(raw)
        for d in extra_dirs + [Path.cwd()]:
            for n in self._names():
                candidates.append(str(d / n))
        if os.name != "nt":
            for d in ("/usr/local/lib", "/opt/homebrew/lib", "/usr/lib", "/usr/lib/x86_64-linux-gnu"):
                for n in self._names():
                    candidates.append(str(Path(d) / n))
        for c in candidates:
            p = Path(c)
            if p.is_dir():
                for n in self._names():
                    if (p / n).is_file():
                        return p / n
            elif p.is_file():
                return p
        for n in self._names():  # dernier recours : recherche système (PATH)
            try:
                (ctypes.WinDLL if os.name == "nt" else ctypes.CDLL)(n)
                return n
            except OSError:
                pass
        raise StormError(
            "StormLib introuvable. Place StormLib.dll à côté du script, ou utilise "
            "--stormlib <chemin> / la variable d'environnement STORMLIB."
        )

    # -- chemins TCHAR ----------------------------------------------------- #
    def _paths(self, p: Path | str) -> Iterator[object]:
        s = str(p)
        if self.mode in (None, "wide") and os.name == "nt":
            yield ("wide", ctypes.c_wchar_p(s))
        if self.mode in (None, "ansi"):
            enc = "mbcs" if os.name == "nt" else "utf-8"
            yield ("ansi", s.encode(enc))

    # -- opérations -------------------------------------------------------- #
    def open(self, path: Path) -> Optional[ctypes.c_void_p]:
        for mode, pp in self._paths(path):
            h = ctypes.c_void_p()
            if self.lib.SFileOpenArchive(pp, 0, MPQ_OPEN_READ_ONLY, ctypes.byref(h)) and h.value:
                self.mode = mode
                return h
        return None

    def create(self, path: Path, max_files: int) -> ctypes.c_void_p:
        if self.mode is None:
            raise StormError("Mode de chemin StormLib non déterminé (ouvre d'abord un MPQ existant).")
        flags = MPQ_CREATE_ARCHIVE_V2 | MPQ_CREATE_LISTFILE | MPQ_CREATE_ATTRIBUTES
        for _, pp in self._paths(path):
            h = ctypes.c_void_p()
            if self.lib.SFileCreateArchive(pp, flags, max_files, ctypes.byref(h)) and h.value:
                return h
        raise StormError(f"Création du MPQ impossible : {path}")

    def close(self, h: ctypes.c_void_p) -> None:
        self.lib.SFileCloseArchive(h)

    def has_file(self, h: ctypes.c_void_p, archived: str) -> bool:
        return bool(self.lib.SFileHasFile(h, archived.encode("utf-8")))

    def list_files(self, h: ctypes.c_void_p) -> List[str]:
        buf = ctypes.create_string_buffer(4096)
        find = self.lib.SFileFindFirstFile(h, b"*", buf, None)
        names: List[str] = []
        if not find:
            return names
        try:
            while True:
                name = buf.value.decode("utf-8", "replace")
                if name and not name.startswith("("):  # (listfile), (attributes), (signature)
                    names.append(name)
                if not self.lib.SFileFindNextFile(find, buf):
                    break
        finally:
            self.lib.SFileFindClose(find)
        return names

    def extract(self, h: ctypes.c_void_p, archived: str, dest: Path) -> None:
        dest.parent.mkdir(parents=True, exist_ok=True)
        for _, pp in self._paths(dest):
            if self.lib.SFileExtractFile(h, archived.encode("utf-8"), pp, 0):
                return
        raise StormError(f"Extraction impossible : {archived}")

    def add(self, h: ctypes.c_void_p, src: Path, archived: str) -> None:
        for _, pp in self._paths(src):
            ok = self.lib.SFileAddFileEx(
                h, pp, archived.encode("utf-8"),
                ctypes.c_uint(MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING),
                ctypes.c_uint(MPQ_COMPRESSION_ZLIB), ctypes.c_uint(MPQ_COMPRESSION_ZLIB),
            )
            if ok:
                return
        raise StormError(f"Ajout au MPQ impossible : {archived}")


# --------------------------------------------------------------------------- #
#  Ordre de chargement du client 3.3.5
# --------------------------------------------------------------------------- #

ROOT_ORDER = ["common", "common-2", "expansion", "lichking", "patch"]


def load_key(p: Path, data: Path, locale: str) -> Tuple[int, int, str]:
    """Clé de tri approximant l'ordre de chargement (plus grand = chargé plus tard = prioritaire)."""
    stem = p.stem.lower()
    if p.parent.resolve() == data.resolve():
        group, order = 0, ROOT_ORDER
    else:
        loc = locale.lower()
        group = 1
        order = [f"base-{loc}", f"locale-{loc}", f"speech-{loc}",
                 f"expansion-locale-{loc}", f"lichking-locale-{loc}", f"patch-{loc}"]
    rank = order.index(stem) if stem in order else len(order)
    return (group, rank, stem)


def detect_locale(data: Path) -> Optional[str]:
    for d in sorted(data.iterdir()):
        if d.is_dir() and re.fullmatch(r"[a-z]{2}[A-Z]{2}", d.name) and any(d.glob("locale-*.[mM][pP][qQ]")):
            return d.name
    return None


# --------------------------------------------------------------------------- #
#  Orchestrateur
# --------------------------------------------------------------------------- #

TOKEN = re.compile(r"\{(out|outdir|repo|base:[^}]+|file:[^}]+)\}")

# StormLib renvoie un bool sur 1 octet ; l'auteur le lit comme un int de 4 octets (restype par
# défaut), dont les 3 octets hauts sont aléatoires sous Windows -> sa boucle de listing ne
# se termine jamais (mémoire qui explose). On force un restype sur 1 octet.
BOOL_PATCH = (
    "    lib.SFileCloseArchive.argtypes = [handle]\n",
    "    lib.SFileCloseArchive.argtypes = [handle]\n"
    "    for _f in (lib.SFileOpenArchive, lib.SFileCreateArchive, lib.SFileExtractFile,\n"
    "               lib.SFileAddFileEx, lib.SFileCloseArchive, lib.SFileFindNextFile):\n"
    "        _f.restype = ctypes.c_ubyte  # btl: bool 1 octet\n",
)

# Les outils de l'auteur passent des chemins en octets (char*) à StormLib, ce qui ne
# marche que sur les builds ANSI (macOS/Linux). Avec une StormLib Windows en UTF-16,
# on corrige une COPIE du dépôt pour passer des chaînes larges.
WIDE_PATCHES = [
    ("lib.SFileOpenArchive.argtypes = [ctypes.c_char_p,",
     "lib.SFileOpenArchive.argtypes = [ctypes.c_wchar_p,"),
    ("lib.SFileCreateArchive.argtypes = [ctypes.c_char_p,",
     "lib.SFileCreateArchive.argtypes = [ctypes.c_wchar_p,"),
    ("lib.SFileExtractFile.argtypes = [handle, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]",
     "lib.SFileExtractFile.argtypes = [handle, ctypes.c_char_p, ctypes.c_wchar_p, ctypes.c_uint]"),
    ("lib.SFileAddFileEx.argtypes = [handle, ctypes.c_char_p, ctypes.c_char_p,",
     "lib.SFileAddFileEx.argtypes = [handle, ctypes.c_wchar_p, ctypes.c_char_p,"),
    ("lib.SFileOpenArchive(mpq.encode(),", "lib.SFileOpenArchive(mpq,"),
    ("lib.SFileCreateArchive(out.encode(),", "lib.SFileCreateArchive(out,"),
    ("lib.SFileExtractFile(h, name.encode(), dst.encode(), 0)",
     "lib.SFileExtractFile(h, name.encode(), dst, 0)"),
    ("lib.SFileAddFileEx(h, src.encode(), name.encode(),",
     "lib.SFileAddFileEx(h, src, name.encode(),"),
]


def log(msg: str = "") -> None:
    print(msg, flush=True)


class Builder:
    def __init__(self, sl: StormLib, client: Path, locale: str, work: Path, out: Path,
                 repos_dir: Path, repo_paths: Dict[str, Path], update: bool, in_locale: bool = False,
                 modules_dir: Optional[Path] = None):
        self.sl = sl
        self.client = client
        self.data = client / "Data"
        self.locale = locale
        self.work = work
        self.out = out
        self.staging = work / "staging"
        self.steps_dir = work / "steps"
        self.repos_dir = repos_dir
        self.modules_dir = modules_dir
        self._extracted: Dict[str, Path] = {}
        self.repo_paths = repo_paths
        self.update = update
        self.in_locale = in_locale
        self.target_dir = (client / "Data" / locale) if in_locale else (client / "Data")
        # fichiers fusionnés : clé minuscule -> (nom dans le MPQ, chemin disque, étape d'origine)
        self.staged: Dict[str, Tuple[str, Path, str]] = {}
        self._handles: Dict[Path, Optional[ctypes.c_void_p]] = {}
        self._run_repos: Dict[str, Path] = {}
        self.client_mpqs = self._scan_client()
        self.out_key = load_key(self.target_dir / out.name, self.data, locale)

    # -- MPQ du client ----------------------------------------------------- #
    def _scan_client(self) -> List[Tuple[Tuple[int, int, str], Path]]:
        found: List[Tuple[Tuple[int, int, str], Path]] = []
        for folder in (self.data, self.data / self.locale):
            if not folder.is_dir():
                continue
            for p in folder.iterdir():
                if not (p.is_file() and p.suffix.lower() == ".mpq"):
                    continue
                if folder == self.target_dir and p.name.lower() == self.out.name.lower():
                    continue  # notre propre patch d'un run précédent
                found.append((load_key(p, self.data, self.locale), p))
        found.sort()
        return found

    def _handle(self, p: Path) -> Optional[ctypes.c_void_p]:
        if p not in self._handles:
            h = self.sl.open(p)
            if h is None:
                log(f"  ! MPQ illisible (ignoré) : {p.name}")
            self._handles[p] = h
        return self._handles[p]

    def close_all(self) -> None:
        for h in self._handles.values():
            if h is not None:
                self.sl.close(h)
        self._handles.clear()

    def client_provider(self, archived: str) -> Optional[Tuple[Tuple[int, int, str], Path]]:
        for key, p in reversed(self.client_mpqs):
            h = self._handle(p)
            if h is not None and self.sl.has_file(h, archived):
                return key, p
        return None

    # -- bases pour --from-mpq -------------------------------------------- #
    def resolve_file(self, archived: str) -> Path:
        """Fichier sur disque : la version issue des étapes précédentes, sinon celle du client."""
        key = archived.lower()
        if key in self.staged:
            log(f"  {archived} : résultat des étapes précédentes")
            return self.staged[key][1]
        if key in self._extracted:
            return self._extracted[key]
        prov = self.client_provider(archived)
        if prov is None:
            raise StormError(f"Aucun MPQ du client ne fournit {archived}")
        log(f"  {archived} : extrait de {prov[1].relative_to(self.client)}")
        dest = self.work / "bases" / "files" / Path(*archived.split("\\"))
        self.sl.extract(self._handle(prov[1]), archived, dest)
        self._extracted[key] = dest
        return dest

    def resolve_base(self, spec: str) -> Path:
        # Les scripts de l'auteur extraient TOUT le MPQ de base puis le recompressent :
        # on leur donne un MPQ minimal ne contenant que les fichiers voulus (rapide, et
        # le patch final ne recopie pas le contenu d'autres patchs du client).
        names = spec.split("|")
        slim = self.work / "bases" / f"base_{zlib.crc32(spec.lower().encode()):08x}.MPQ"
        self.pack_files(slim, [(self.resolve_file(n), n) for n in names])
        return slim

    def expand(self, arg: str, repo: Path, out: Path, outdir: Optional[Path] = None) -> str:
        def sub(m: "re.Match[str]") -> str:
            tok = m.group(1)
            if tok == "out":
                return str(out)
            if tok == "outdir":
                return str(outdir)
            if tok == "repo":
                return str(repo)
            kind, _, value = tok.partition(":")
            return str(self.resolve_file(value) if kind == "file" else self.resolve_base(value))
        return TOKEN.sub(sub, arg)

    # -- dépôts ------------------------------------------------------------ #
    def ensure_repo(self, patch: Patch) -> Path:
        if patch.name in self.repo_paths:
            dest = self.repo_paths[patch.name]
            if not dest.is_dir():
                raise StormError(f"--repo-path {patch.name}: dossier introuvable : {dest}")
            return dest
        if self.modules_dir:
            local = self.modules_dir / f"mod-{patch.name}"
            if local.is_dir() and any(local.iterdir()):
                log(f"  dépôt local : {local}")
                return local
        dest = self.repos_dir / patch.name
        if not shutil.which("git"):
            raise StormError("git est introuvable dans le PATH.")
        if not dest.exists():
            dest.parent.mkdir(parents=True, exist_ok=True)
            cmd = ["git", "clone", "--depth", "1"]
            if patch.ref:
                cmd += ["--branch", patch.ref]
            subprocess.run(cmd + [patch.url, str(dest)], check=True)
        elif self.update:
            subprocess.run(["git", "-C", str(dest), "pull", "--ff-only"], check=True)
        return dest

    def prepare_run_repo(self, patch: Patch, repo: Path) -> Path:
        """Dépôt à exécuter : l'original, ou (Windows) une copie corrigée pour StormLib."""
        if os.name != "nt":
            return repo
        if patch.name in self._run_repos:
            return self._run_repos[patch.name]
        run = self.work / "run" / patch.name
        if run.exists():
            shutil.rmtree(run)
        shutil.copytree(repo, run, ignore=shutil.ignore_patterns(".git"))
        target = run / "tools" / "build_patch.py"
        if not target.is_file():
            raise StormError(f"tools/build_patch.py introuvable dans {repo}")
        text = target.read_bytes().decode("utf-8")
        patches = [BOOL_PATCH] + (WIDE_PATCHES if self.sl.mode == "wide" else [])
        for old, new in patches:
            if old not in text:
                raise StormError(
                    "Le script de l'auteur a changé, correctif inapplicable "
                    f"(motif introuvable : {old!r}). Envoie-moi ses lignes StormLib."
                )
            text = text.replace(old, new)
        target.write_bytes(text.encode("utf-8"))
        log("  (copie corrigée du dépôt pour StormLib Windows : bool 1 octet"
            + (" + chemins UTF-16)" if self.sl.mode == "wide" else ")"))
        self._run_repos[patch.name] = run
        return run

    # -- fusion ------------------------------------------------------------ #
    def stage_mpq(self, mpq: Path, origin: str) -> None:
        h = self.sl.open(mpq)
        if h is None:
            raise StormError(f"MPQ intermédiaire illisible : {mpq}")
        try:
            names = self.sl.list_files(h)
            if not names:
                raise StormError(f"MPQ intermédiaire vide (ou sans (listfile)) : {mpq}")
            for name in names:
                key = name.lower()
                dest = self.staging.joinpath(*name.split("\\"))
                if key in self.staged:
                    old = self.staged[key]
                    log(f"    ~ {name} écrase la version de « {old[2]} »")
                    if old[1] != dest and old[1].exists():
                        old[1].unlink()
                self.sl.extract(h, name, dest)
                self.staged[key] = (name, dest, origin)
                log(f"    + {name}")
        finally:
            self.sl.close(h)

    def pack(self, dest: Path) -> None:
        self.pack_files(dest, [(src, name) for name, src, _ in self.staged.values()])

    def pack_files(self, dest: Path, items: List[Tuple[Path, str]]) -> None:
        if dest.exists():
            dest.unlink()
        dest.parent.mkdir(parents=True, exist_ok=True)
        h = self.sl.create(dest, max(16, len(items) * 2))
        try:
            for src, name in items:
                self.sl.add(h, src, name)
        finally:
            self.sl.close(h)

    # -- exécution --------------------------------------------------------- #
    def run(self, patches: List[Patch]) -> None:
        for d in (self.staging, self.steps_dir, self.work / "bases", self.work / "embedded"):
            if d.exists():
                shutil.rmtree(d)
            d.mkdir(parents=True)
        env = os.environ.copy()
        env["STORMLIB"] = str(self.sl.path)
        env["PYTHONUTF8"] = "1"
        total = sum(len(p.steps) for p in patches)
        n = 0
        for patch in patches:
            log(f"\n=== {patch.name} ===")
            repo = self.ensure_repo(patch)
            for step in patch.steps:
                n += 1
                log(f"\n[{n}/{total}] {patch.name} · {step.label}")
                out = self.steps_dir / f"{n:02d}-{patch.name}-{Path(step.script).stem}.MPQ"
                script = repo / step.script
                if not script.is_file():
                    raise StormError(f"Script introuvable : {script}")
                if step.kind == "sh-python":
                    outdir = self.steps_dir / f"{n:02d}-{patch.name}" / "DBFilesClient"
                    step_args = [self.expand(a, repo, out, outdir) for a in step.args]
                    py = self.extract_embedded_python(script, patch.name)
                    cmd = [sys.executable, str(py)] + step_args
                    log("  $ " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
                    subprocess.run(cmd, cwd=repo, env=env, check=True)
                    produced = sorted(outdir.glob("*.dbc")) if outdir.is_dir() else []
                    if not produced:
                        raise StormError(f"Le script n'a produit aucun .dbc dans {outdir}")
                    self.pack_files(out, [(f, DBC + f.name) for f in produced])
                else:
                    step_args = [self.expand(a, repo, out) for a in step.args]  # ouvre les MPQ de base
                    run_repo = self.prepare_run_repo(patch, repo)
                    cmd = [sys.executable, str(run_repo / step.script)] + step_args
                    log("  $ " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
                    subprocess.run(cmd, cwd=run_repo, env=env, check=True)
                if not out.is_file():
                    raise StormError(f"Le script n'a pas produit {out}")
                self.stage_mpq(out, f"{patch.name} · {step.label}")

        if not self.staged:
            raise StormError("Rien à empaqueter.")
        log(f"\n=== Empaquetage : {self.out} ({len(self.staged)} fichiers) ===")
        self.pack(self.out)
        self.check_order()

    def extract_embedded_python(self, sh: Path, name: str) -> Path:
        """Les .sh de l'auteur sont un wrapper bash autour d'un python en heredoc : on exécute le python."""
        text = sh.read_bytes().decode("utf-8").replace("\r\n", "\n")
        m = re.search(r"python3? -[^\n]*<<'PY'\n(.*?)\nPY\n", text, re.S)
        if not m:
            raise StormError(f"Python intégré introuvable dans {sh} (format du script changé ?)")
        dest = self.work / "embedded" / f"{name}-{sh.stem}.py"
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(m.group(1) + "\n", encoding="utf-8")
        return dest

    def check_order(self) -> None:
        problems = []
        for key, (name, _, origin) in sorted(self.staged.items()):
            prov = self.client_provider(name)
            if prov and prov[0] > self.out_key:
                problems.append((name, prov[1].name))
        if problems:
            log("\n  ATTENTION : ces fichiers sont aussi fournis par un MPQ chargé APRÈS "
                f"{self.out.name} (le client utilisera l'autre version) :")
            for name, mpq in problems:
                log(f"    - {name}  (dans {mpq})")
            if not self.in_locale and any(
                    load_key(self.client_provider(n)[1], self.data, self.locale)[0] == 1
                    for n, _ in problems if self.client_provider(n)):
                log("  -> Le client charge les MPQ du dossier de locale APRÈS ceux de Data\\ : aucune lettre dans")
                log(f"     Data\\ ne suffit. Relance avec --in-locale --out patch-{self.locale}-V.mpq")
                log(f"     (le patch sera installé dans Data\\{self.locale}\\).")
            else:
                log("  -> renomme le patch avec une lettre qui se trie plus tard (ex. --out patch-W.MPQ).")
        else:
            log(f"  Ordre de chargement OK : {self.out.name} passe après les MPQ qui fournissent les mêmes fichiers.")


# --------------------------------------------------------------------------- #
#  CLI
# --------------------------------------------------------------------------- #

def cleanup_work(work: Path, marker: Path, out: Path) -> None:
    """Supprime le dossier de travail, avec des garde-fous (jamais le dossier courant, le script ou le MPQ final)."""
    def inside(child: Path, parent: Path) -> bool:
        try:
            child.resolve().relative_to(parent.resolve())
            return True
        except ValueError:
            return False

    if not marker.is_file():
        return
    if inside(Path.cwd(), work) or inside(Path(__file__), work) or inside(out, work):
        log(f"Nettoyage ignoré : {work} contient le dossier courant, le script ou le MPQ final.")
        return

    def force(func, path, _exc):  # fichiers .git en lecture seule sous Windows
        os.chmod(path, 0o700)
        func(path)

    try:
        shutil.rmtree(work, onerror=force)
        log(f"Dossier de travail supprimé : {work}")
    except OSError as exc:
        log(f"Nettoyage incomplet de {work} : {exc}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Fabrique un MPQ unique à partir des patchs client de buildthehomelab.")
    ap.add_argument("--client", default=DEFAULT_CLIENT, help="dossier du client WoW 3.3.5 (défaut : %(default)s)")
    ap.add_argument("--locale", help="locale du client (défaut : détection, ex. enUS)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="MPQ final (défaut : %(default)s)")
    ap.add_argument("--work", default="btl-work", help="dossier de travail (défaut : %(default)s)")
    ap.add_argument("--repos-dir", help="dossier où cloner les dépôts (défaut : <work>/repos)")
    ap.add_argument("--repo-path", action="append", default=[], metavar="NOM=CHEMIN",
                    help="utiliser un dépôt déjà cloné (ex. forever-racials=D:\\...\\modules\\mod-forever-racials)")
    ap.add_argument("--modules-dir", default="modules",
                    help="dossier des modules AzerothCore (submodules mod-forever-*), utilisé s'il existe "
                         "(défaut : %(default)s) ; sinon les dépôts sont clonés")
    ap.add_argument("--only", action="append", default=[], metavar="NOM", help="ne traiter que ce patch (répétable)")
    ap.add_argument("--update", action="store_true", help="git pull des dépôts déjà clonés")
    ap.add_argument("--stormlib", help="chemin de StormLib.dll / libstorm")
    ap.add_argument("--install", action="store_true", help="copier le MPQ final dans <client>\\Data (ou Data\\<locale> avec --in-locale)")
    ap.add_argument("--in-locale", action="store_true",
                    help="le patch est destiné à Data\\<locale>\\ (nom attendu : patch-<locale>-<lettre>.mpq) ; "
                         "nécessaire si un MPQ de locale (ex. patch-frFR-3) fournit déjà Spell.dbc / GlueStrings.lua")
    ap.add_argument("--keep-out", action="store_true",
                    help="avec --install, garder aussi le MPQ du dossier courant (par défaut il est déplacé vers le client)")
    ap.add_argument("--keep-work", action="store_true",
                    help="ne pas supprimer le dossier de travail à la fin (par défaut il est supprimé si tout a réussi)")
    ap.add_argument("--list", action="store_true", help="lister les patchs configurés et quitter")
    args = ap.parse_args()

    if args.list:
        for p in PATCHES:
            log(f"{p.name}  <- {p.url}")
            for s in p.steps:
                log(f"    - {s.label}: {s.script}")
        return 0

    patches = [p for p in PATCHES if not args.only or p.name in args.only]
    if not patches:
        log("Aucun patch sélectionné (voir --list).")
        return 2

    client = Path(args.client)
    data = client / "Data"
    if not data.is_dir():
        log(f"Dossier Data introuvable : {data}  (utilise --client)")
        return 2
    locale = args.locale or detect_locale(data)
    if not locale:
        log("Locale non détectée, précise --locale (ex. enUS).")
        return 2

    work = Path(args.work).resolve()
    out = Path(args.out).resolve()
    repo_paths: Dict[str, Path] = {}
    for item in args.repo_path:
        name, _, path = item.partition("=")
        if not path:
            log(f"--repo-path attend NOM=CHEMIN : {item}")
            return 2
        repo_paths[name] = Path(path).resolve()

    marker = work / ".build_btl_patches"
    work.mkdir(parents=True, exist_ok=True)
    marker.write_text("dossier de travail de build_btl_patches.py : peut être supprimé\n", encoding="utf-8")
    try:
        sl = StormLib(args.stormlib, [Path(__file__).resolve().parent])
        log(f"StormLib : {sl.path}")
        log(f"Client   : {client}  (locale {locale})")
        builder = Builder(sl, client, locale, work, out,
                          Path(args.repos_dir).resolve() if args.repos_dir else work / "repos",
                          repo_paths, args.update, args.in_locale,
                          Path(args.modules_dir).resolve() if args.modules_dir else None)
        try:
            builder.run(patches)
        finally:
            builder.close_all()
    except (StormError, subprocess.CalledProcessError) as exc:
        log(f"\nÉCHEC : {exc}")
        return 1

    log(f"\nMPQ final : {out}")
    if not args.keep_work:
        cleanup_work(work, marker, out)
    if args.install:
        target = builder.target_dir / out.name
        if target.resolve() == out.resolve():
            log(f"Installé  : {target}")
        else:
            shutil.copy2(out, target)
            if target.stat().st_size != out.stat().st_size:
                log(f"ÉCHEC : la copie vers {target} est incomplète ; le MPQ reste dans {out}")
                return 1
            log(f"Installé  : {target}  (client WoW fermé !)")
            if not args.keep_out:
                out.unlink()
                log(f"Supprimé  : {out}")
    else:
        log(f"Pour l'installer : copie-le dans {builder.target_dir} (ou relance avec --install).")
    return 0


if __name__ == "__main__":
    sys.exit(main())