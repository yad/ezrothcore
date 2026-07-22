#!/usr/bin/env python3
"""
merge_conf.py - Fusionne les nouveautes d'un fichier *.conf.dist dans un
fichier *.conf modifie, sans ecraser vos valeurs personnalisees.

Principe :
  - Le fichier de sortie suit la STRUCTURE du .dist (ordre, sections,
    commentaires, nouvelles cles) car c'est la version a jour.
  - Pour chaque cle presente dans le .dist, si elle existe deja dans votre
    .conf, on garde VOTRE valeur (pas celle du .dist).
  - Les cles nouvelles dans le .dist (absentes de votre .conf) sont ajoutees
    telles quelles, avec leur valeur par defaut.
  - Les cles presentes dans votre .conf mais absentes du .dist (supprimees /
    renommees en amont, ou faute de frappe) sont listees a la fin en warning
    et ne sont PAS perdues : elles sont ajoutees dans un bloc
    "### CLES NON RECONNUES DANS LE .DIST ###" a la fin du fichier fusionne,
    pour que vous decidiez quoi en faire.

Usage :
  Un seul pair de fichiers :
    python3 merge_conf.py worldserver.conf.dist worldserver.conf

  Dossier entier (cherche tous les *.dist et leur .conf correspondant,
  recursivement, donc y compris etc/modules/) :
    python3 merge_conf.py --dir /home/pi/azerothcore-wotlk/env/dist/etc

  Par defaut le resultat est ecrit a cote sous le nom "<conf>.merged" pour ne
  rien ecraser. Ajoutez --write pour remplacer directement le .conf (une
  sauvegarde .conf.bak est alors creee).
"""

import argparse
import re
import sys
from pathlib import Path

KEY_RE = re.compile(r"^([A-Za-z0-9_.\-]+)\s*=\s*(.*)$")


def parse_conf_values(path: Path) -> dict:
    """Retourne {cle: valeur_brute_apres_le_egal} pour les lignes actives (non commentees)."""
    values = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        m = KEY_RE.match(stripped)
        if m:
            key = m.group(1)
            values[key] = line  # on garde la ligne complete d'origine (indentation incluse)
    return values


def merge(dist_path: Path, conf_path: Path):
    dist_lines = dist_path.read_text(encoding="utf-8", errors="replace").splitlines()
    conf_values = parse_conf_values(conf_path)

    dist_keys_seen = set()
    out_lines = []
    added_keys = []
    kept_keys = []

    for line in dist_lines:
        stripped = line.strip()
        m = None if (not stripped or stripped.startswith("#")) else KEY_RE.match(stripped)
        if m:
            key = m.group(1)
            dist_keys_seen.add(key)
            if key in conf_values:
                out_lines.append(conf_values[key])  # on garde VOTRE ligne/valeur
                kept_keys.append(key)
            else:
                out_lines.append(line)  # nouvelle cle -> valeur par defaut du dist
                added_keys.append(key)
        else:
            out_lines.append(line)  # commentaires / sections / lignes vides -> version a jour du dist

    # Cles presentes chez vous mais absentes du nouveau dist
    orphan_keys = [k for k in conf_values if k not in dist_keys_seen]
    if orphan_keys:
        out_lines.append("")
        out_lines.append("###################################################")
        out_lines.append("#  CLES PRESENTES DANS VOTRE .conf MAIS ABSENTES")
        out_lines.append("#  DU .dist ACTUEL (obsoletes, renommees, ou faute")
        out_lines.append("#  de frappe) - a verifier manuellement")
        out_lines.append("###################################################")
        for k in orphan_keys:
            out_lines.append(conf_values[k])

    return "\n".join(out_lines) + "\n", added_keys, orphan_keys, kept_keys


def process_pair(dist_path: Path, conf_path: Path, write: bool):
    if not conf_path.exists():
        print(f"[SKIP] {conf_path} n'existe pas (rien a fusionner, "
              f"copiez simplement {dist_path.name} si besoin).")
        return

    merged_text, added, orphans, kept = merge(dist_path, conf_path)

    if write:
        backup = conf_path.with_suffix(conf_path.suffix + ".bak")
        backup.write_text(conf_path.read_text(encoding="utf-8", errors="replace"), encoding="utf-8")
        conf_path.write_text(merged_text, encoding="utf-8")
        out_path = conf_path
    else:
        out_path = conf_path.with_name(conf_path.name + ".merged")
        out_path.write_text(merged_text, encoding="utf-8")

    print(f"\n=== {conf_path} ===")
    print(f"  -> resultat : {out_path}" + (f"  (sauvegarde: {backup})" if write else ""))
    print(f"  cles conservees (vos valeurs)   : {len(kept)}")
    print(f"  cles nouvelles ajoutees (dist)  : {len(added)}")
    if added:
        for k in added:
            print(f"      + {k}")
    if orphans:
        print(f"  cles orphelines (a verifier)    : {len(orphans)}")
        for k in orphans:
            print(f"      ? {k}")


def find_pairs(root: Path):
    pairs = []
    for dist_file in root.rglob("*.dist"):
        conf_file = dist_file.with_suffix("")  # enleve le ".dist" final
        pairs.append((dist_file, conf_file))
    return pairs


def main():
    parser = argparse.ArgumentParser(description="Fusionne les .conf.dist dans vos .conf modifies.")
    parser.add_argument("dist", nargs="?", help="fichier .dist (mode paire unique)")
    parser.add_argument("conf", nargs="?", help="fichier .conf modifie (mode paire unique)")
    parser.add_argument("--dir", help="dossier a scanner recursivement pour toutes les paires *.dist / *")
    parser.add_argument("--write", action="store_true",
                         help="ecrase directement le .conf (une sauvegarde .bak est creee)")
    args = parser.parse_args()

    if args.dir:
        root = Path(args.dir)
        pairs = find_pairs(root)
        if not pairs:
            print(f"Aucun fichier *.dist trouve sous {root}")
            sys.exit(1)
        for dist_file, conf_file in pairs:
            process_pair(dist_file, conf_file, args.write)
    elif args.dist and args.conf:
        process_pair(Path(args.dist), Path(args.conf), args.write)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
