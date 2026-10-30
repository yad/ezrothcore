#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
translate.py — exporte les textes Spell.dbc modifiés vers JSON et applique les traductions frFR.

Exemples :
  # Export depuis le MPQ construit, en comparant au Spell.dbc original :
  python translate.py --mpq patch-V.MPQ --base-dbc original-frFR-Spell.dbc --export traductions.json

  # Après avoir rempli les champs "translation" du JSON :
  python translate.py --mpq patch-V.MPQ --apply traductions.json --in-place

  # Variante avec des Spell.dbc extraits :
  python translate.py --dbc Spell.dbc --base-dbc original-Spell.dbc --export traductions.json
  python translate.py --dbc Spell.dbc --apply traductions.json --out-dbc Spell-frFR.dbc

Le script ne modifie que l'index frFR (2) des champs texte. Les autres locales
et les autres champs du DBC sont conservés.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import shutil
import struct
import sys
import tempfile
from collections import Counter
from pathlib import Path

FIELDS = 234
LOCALE_INDEX = {
    "enUS": 0, "koKR": 1, "frFR": 2, "deDE": 3,
    "zhCN": 4, "zhTW": 5, "esES": 6, "esMX": 7,
    "ruRU": 8, "ptBR": 9, "itIT": 10,
}
LOCALE = "frFR"
LOCALE_SLOT = LOCALE_INDEX[LOCALE]

# Début des groupes de 16 chaînes dans Spell.dbc 3.3.5a.
TEXT_FIELDS = {
    "name": 136,
    "subtext": 153,
    "description": 170,
    "aura_description": 187,
}
MPQ_SPELL_PATH = "DBFilesClient\\Spell.dbc"
DBC_MAGIC = b"WDBC"
MPQ_OPEN_READ_ONLY = 0x00000100
MPQ_FILE_COMPRESS = 0x00000200
MPQ_FILE_REPLACEEXISTING = 0x80000000
MPQ_COMPRESSION_ZLIB = 0x02

# Les marqueurs WoW doivent être présents à l'identique dans la traduction.
PLACEHOLDER_RE = re.compile(r"\$[A-Za-z0-9_]+")


def markup_problems(text: str):
    """Détecte les balises de tooltip mal formées qui font planter le client 3.3.5."""
    problems = []
    if text.count("{") != text.count("}"):
        problems.append("accolades { } déséquilibrées")
    if len(re.findall(r"\|c", text)) != len(re.findall(r"\|r", text)):
        problems.append("|c / |r déséquilibrés")
    for m in re.finditer(r"\$[lLgG]", text):
        if text.find(";", m.end()) < 0:
            problems.append("$l/$g sans ';' final")
            break
    return problems


def read_dbc(path: Path):
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"{path}: fichier trop court pour être un DBC")
    magic, count, fields, record_size, string_size = struct.unpack_from("<4s4I", data, 0)
    if magic != DBC_MAGIC or fields != FIELDS or record_size != FIELDS * 4:
        raise ValueError(
            f"{path}: Spell.dbc inattendu (magic={magic!r}, champs={fields}, taille={record_size})"
        )
    expected = 20 + count * record_size + string_size
    if len(data) < expected:
        raise ValueError(f"{path}: fichier tronqué (attendu {expected} octets, reçu {len(data)})")
    rows = [
        list(struct.unpack_from(f"<{FIELDS}I", data, 20 + i * record_size))
        for i in range(count)
    ]
    strings = bytearray(data[20 + count * record_size:expected])
    return rows, strings


def write_dbc(path: Path, rows, strings):
    rows = sorted(rows, key=lambda row: row[0])
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as f:
        f.write(struct.pack("<4s4I", DBC_MAGIC, len(rows), FIELDS, FIELDS * 4, len(strings)))
        for row in rows:
            f.write(struct.pack(f"<{FIELDS}I", *row))
        f.write(strings)


def get_string(strings: bytearray, offset: int) -> str:
    if offset == 0 or offset >= len(strings):
        return ""
    end = strings.find(b"\0", offset)
    if end < 0:
        end = len(strings)
    return bytes(strings[offset:end]).decode("utf-8", "replace")


def add_string(strings: bytearray, value: str) -> int:
    if not value:
        return 0
    encoded = value.encode("utf-8") + b"\0"
    pos = strings.find(encoded)
    while pos > 0 and strings[pos - 1] != 0:
        pos = strings.find(encoded, pos + 1)
    if pos >= 0:
        return pos
    pos = len(strings)
    strings.extend(encoded)
    return pos


def row_text(row, strings, field_name, locale_index):
    return get_string(strings, row[TEXT_FIELDS[field_name] + locale_index])


def export_json(current_dbc: Path, base_dbc: Path, output_json: Path):
    current_rows, current_strings = read_dbc(current_dbc)
    base_rows, base_strings = read_dbc(base_dbc)
    base_by_id = {row[0]: row for row in base_rows}

    spells = []
    for row in current_rows:
        spell_id = row[0]
        base = base_by_id.get(spell_id)
        changed_fields = {}

        for field_name in TEXT_FIELDS:
            source = row_text(row, current_strings, field_name, LOCALE_INDEX["enUS"])
            if not source:
                continue

            current_fr = row_text(row, current_strings, field_name, LOCALE_SLOT)
            if base is None:
                changed = True
            else:
                base_en = row_text(base, base_strings, field_name, LOCALE_INDEX["enUS"])
                base_fr = row_text(base, base_strings, field_name, LOCALE_SLOT)
                # Détecte aussi le cas où build_patch.py a effacé la traduction frFR
                # alors que le texte anglais est resté identique.
                changed = source != base_en or current_fr != base_fr

            if changed:
                changed_fields[field_name] = {
                    "source": source,
                    "translation": "",
                }

        if changed_fields:
            spells.append({"id": spell_id, "fields": changed_fields})

    payload = {
        "format": "wow-spell-dbc-translations-v1",
        "locale": LOCALE,
        "locale_index": LOCALE_SLOT,
        "source_locale": "enUS",
        "source_dbc": current_dbc.name,
        "baseline_dbc": base_dbc.name,
        "instructions": (
            "Remplir chaque champ translation en français. Ne pas modifier les IDs, "
            "les noms de champs ni les marqueurs WoW tels que $s1, $d, $t1 ou $20596s1."
        ),
        "spells": spells,
    }
    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_json.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    total_fields = sum(len(s["fields"]) for s in spells)
    print(f"JSON créé : {output_json}")
    print(f"{len(spells)} sorts, {total_fields} champs texte à traduire.")


def apply_json(dbc_path: Path, json_path: Path, output_dbc: Path):
    rows, strings = read_dbc(dbc_path)
    by_id = {row[0]: row for row in rows}
    payload = json.loads(json_path.read_text(encoding="utf-8"))

    if payload.get("format") != "wow-spell-dbc-translations-v1":
        raise ValueError("Format JSON inconnu ou ancien.")
    if payload.get("locale") != LOCALE or payload.get("locale_index") != LOCALE_SLOT:
        raise ValueError(f"Ce fichier JSON ne cible pas {LOCALE} (index {LOCALE_SLOT}).")

    applied = 0
    skipped = 0
    errors = []

    for spell in payload.get("spells", []):
        spell_id = int(spell["id"])
        row = by_id.get(spell_id)
        if row is None:
            errors.append(f"Sort {spell_id} absent du Spell.dbc")
            continue

        for field_name, item in spell.get("fields", {}).items():
            if field_name not in TEXT_FIELDS:
                errors.append(f"Sort {spell_id}: champ inconnu {field_name!r}")
                continue
            translated = item.get("translation", "")
            if not isinstance(translated, str) or not translated.strip():
                skipped += 1
                continue

            source = item.get("source", "")
            current_source = row_text(row, strings, field_name, LOCALE_INDEX["enUS"])
            # enUS vide = traduction déjà appliquée lors d'un passage précédent
            # (la colonne enUS est vidée à l'application) : on accepte de la réécrire.
            if source and current_source and current_source != source:
                errors.append(
                    f"Sort {spell_id}/{field_name}: le texte enUS ne correspond plus au JSON. "
                    "Refais l'export avec le Spell.dbc actuel."
                )
                continue

            source_tokens = Counter(PLACEHOLDER_RE.findall(source))
            translated_tokens = Counter(PLACEHOLDER_RE.findall(translated))
            if source_tokens != translated_tokens:
                errors.append(
                    f"Sort {spell_id}/{field_name}: marqueurs WoW différents "
                    f"(source={dict(source_tokens)}, traduction={dict(translated_tokens)})"
                )
                continue

            bad = markup_problems(translated)
            if bad:
                errors.append(f"Sort {spell_id}/{field_name}: {', '.join(bad)}")
                continue

            offset = add_string(strings, translated)
            row[TEXT_FIELDS[field_name] + LOCALE_SLOT] = offset
            # Vider la colonne enUS dès que frFR est remplie, sinon le client plante.
            row[TEXT_FIELDS[field_name] + LOCALE_INDEX["enUS"]] = 0
            applied += 1

    if errors:
        raise ValueError("Application annulée :\n - " + "\n - ".join(errors))

    write_dbc(output_dbc, rows, strings)
    print(f"Spell.dbc écrit : {output_dbc}")
    print(f"{applied} traductions appliquées dans la colonne frFR ; {skipped} champs laissés vides.")


class StormLib:
    """Accès minimal StormLib pour extraire/remplacer DBFilesClient\\Spell.dbc."""

    def __init__(self, hint=None):
        candidates = [hint, os.environ.get("STORMLIB")]
        if os.name == "nt":
            names = ["StormLib.dll", "stormlib.dll", "storm.dll"]
            loader = ctypes.WinDLL
        else:
            names = ["libstorm.so", "libstorm.so.1", "libstorm.dylib", "libstorm.2.dylib"]
            loader = ctypes.CDLL

        for candidate in candidates:
            if candidate and Path(candidate).is_file():
                self.path = str(candidate)
                break
        else:
            self.path = None
            for name in names:
                try:
                    self.lib = loader(name)
                    self.path = name
                    break
                except OSError:
                    pass
            if self.path is None:
                raise RuntimeError("StormLib introuvable. Utilise --stormlib ou STORMLIB.")

        if not hasattr(self, "lib"):
            self.lib = loader(self.path)

        self.path_type = ctypes.c_wchar_p if os.name == "nt" else ctypes.c_char_p
        self.encode_path = (lambda p: str(p)) if os.name == "nt" else (lambda p: os.fsencode(p))
        L = self.lib
        L.SFileOpenArchive.argtypes = [
            self.path_type, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)
        ]
        L.SFileOpenArchive.restype = ctypes.c_ubyte
        L.SFileExtractFile.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, self.path_type, ctypes.c_uint
        ]
        L.SFileExtractFile.restype = ctypes.c_ubyte
        L.SFileAddFileEx.argtypes = [
            ctypes.c_void_p, self.path_type, ctypes.c_char_p,
            ctypes.c_uint, ctypes.c_uint, ctypes.c_uint
        ]
        L.SFileAddFileEx.restype = ctypes.c_ubyte
        L.SFileCloseArchive.argtypes = [ctypes.c_void_p]
        L.SFileCloseArchive.restype = ctypes.c_ubyte
        L.SFileRemoveFile.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint]
        L.SFileRemoveFile.restype = ctypes.c_ubyte
        L.SFileSetLocale.argtypes = [ctypes.c_uint]
        L.SFileSetLocale.restype = ctypes.c_uint
        L.SFileFlushArchive.argtypes = [ctypes.c_void_p]
        L.SFileFlushArchive.restype = ctypes.c_ubyte
        L.SFileCompactArchive.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_ubyte]
        L.SFileCompactArchive.restype = ctypes.c_ubyte

    def open(self, mpq: Path, read_only=True):
        handle = ctypes.c_void_p()
        flags = MPQ_OPEN_READ_ONLY if read_only else 0
        if not self.lib.SFileOpenArchive(
            self.encode_path(mpq), 0, flags, ctypes.byref(handle)
        ) or not handle.value:
            raise RuntimeError(f"Impossible d'ouvrir le MPQ : {mpq}")
        return handle

    def extract_spell(self, mpq: Path, destination: Path):
        h = self.open(mpq)
        try:
            destination.parent.mkdir(parents=True, exist_ok=True)
            ok = self.lib.SFileExtractFile(
                h, MPQ_SPELL_PATH.encode("ascii"), self.encode_path(destination), 0
            )
            if not ok:
                raise RuntimeError(f"{MPQ_SPELL_PATH} introuvable dans {mpq}")
        finally:
            self.lib.SFileCloseArchive(h)

    def replace_spell(self, mpq: Path, dbc_path: Path):
        name = MPQ_SPELL_PATH.encode("ascii")
        h = self.open(mpq, read_only=False)
        try:
            # 1) Supprimer toutes les entrées existantes du fichier (toutes locales
            #    confondues). Sinon REPLACEEXISTING ne remplace que l'entrée de la locale
            #    courante et laisse un doublon, ce qui gonfle le MPQ et peut faire
            #    charger au client l'ancienne version.
            for locale in (0, 0x409, 0x40C):  # neutre, enUS, frFR
                self.lib.SFileSetLocale(locale)
                for _ in range(8):
                    if not self.lib.SFileRemoveFile(h, name, 0):
                        break
            self.lib.SFileSetLocale(0)

            # 2) Ajouter le nouveau Spell.dbc, compressé en zlib, locale neutre.
            ok = self.lib.SFileAddFileEx(
                h, self.encode_path(dbc_path), name,
                MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING,
                MPQ_COMPRESSION_ZLIB, MPQ_COMPRESSION_ZLIB
            )
            if not ok:
                raise RuntimeError(f"Impossible de remplacer {MPQ_SPELL_PATH} dans {mpq}")

            # 3) Écrire les tables, puis compacter pour récupérer l'espace des anciennes
            #    versions (5 Mo -> 9 Mo sans compactage).
            self.lib.SFileFlushArchive(h)
            if not self.lib.SFileCompactArchive(h, None, 0):
                print("AVERTISSEMENT : compactage du MPQ impossible, "
                      "l'archive restera plus volumineuse.", file=sys.stderr)
        finally:
            self.lib.SFileCloseArchive(h)

        # 4) Vérification : le Spell.dbc relu depuis le MPQ doit être identique octet
        #    pour octet à celui qu'on vient d'insérer.
        with tempfile.TemporaryDirectory(prefix="wow-verify-") as tmp:
            check = Path(tmp) / "Spell.dbc"
            self.extract_spell(mpq, check)
            if check.read_bytes() != dbc_path.read_bytes():
                raise RuntimeError(f"Vérification échouée : {MPQ_SPELL_PATH} relu dans {mpq} "
                                   "différent du fichier inséré.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = ap.add_mutually_exclusive_group(required=True)
    source.add_argument("--dbc", type=Path, help="Spell.dbc extrait")
    source.add_argument("--mpq", type=Path, help="MPQ contenant DBFilesClient\\Spell.dbc")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--export", type=Path, metavar="JSON", help="exporter les textes à traduire")
    mode.add_argument("--apply", type=Path, metavar="JSON", help="appliquer les traductions du JSON")
    ap.add_argument("--base-dbc", type=Path, help="Spell.dbc original/non patché, requis pour --export")
    ap.add_argument("--out-dbc", type=Path, help="Spell.dbc de sortie, requis avec --dbc --apply")
    ap.add_argument("--out-mpq", type=Path, help="MPQ de sortie, avec --mpq --apply")
    ap.add_argument("--in-place", action="store_true", help="modifier le MPQ fourni directement")
    ap.add_argument("--stormlib", help="chemin vers StormLib.dll / libstorm")
    args = ap.parse_args()

    if args.export and not args.base_dbc:
        ap.error("--export nécessite --base-dbc pour ne récupérer que les textes ajoutés/modifiés.")
    if args.apply and args.dbc and not args.out_dbc:
        ap.error("--dbc --apply nécessite --out-dbc.")
    if args.apply and args.mpq and bool(args.in_place) == bool(args.out_mpq):
        ap.error("Avec --mpq --apply, indique exactement une option : --in-place ou --out-mpq.")
    if args.export and (args.out_dbc or args.out_mpq or args.in_place):
        ap.error("--out-dbc, --out-mpq et --in-place ne servent qu'avec --apply.")
    if args.dbc and (args.out_mpq or args.in_place):
        ap.error("--out-mpq et --in-place nécessitent --mpq.")

    if args.dbc:
        if args.export:
            export_json(args.dbc, args.base_dbc, args.export)
        else:
            apply_json(args.dbc, args.apply, args.out_dbc)
        return 0

    storm = StormLib(args.stormlib)
    with tempfile.TemporaryDirectory(prefix="wow-translate-") as tmp:
        extracted = Path(tmp) / "Spell.dbc"
        storm.extract_spell(args.mpq, extracted)

        if args.export:
            export_json(extracted, args.base_dbc, args.export)
            return 0

        if args.in_place:
            fd, temp_name = tempfile.mkstemp(
                prefix=args.mpq.stem + "-translated-", suffix=args.mpq.suffix,
                dir=str(args.mpq.resolve().parent)
            )
            os.close(fd)
            temp_mpq = Path(temp_name)
            try:
                shutil.copy2(args.mpq, temp_mpq)
                apply_json(extracted, args.apply, extracted)
                storm.replace_spell(temp_mpq, extracted)
                os.replace(temp_mpq, args.mpq)
            finally:
                if temp_mpq.exists():
                    temp_mpq.unlink()
            print(f"MPQ mis à jour : {args.mpq}")
        else:
            output = args.out_mpq
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(args.mpq, output)
            apply_json(extracted, args.apply, extracted)
            storm.replace_spell(output, extracted)
            print(f"MPQ traduit créé : {output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"ERREUR : {exc}", file=sys.stderr)
        raise SystemExit(1)