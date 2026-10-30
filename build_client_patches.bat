@echo off

python build_client_patches.py --out patch-Z.mpq --install --keep-work

@REM  python translate.py --stormlib "D:\repo\ezrothcore\StormLib.dll" --mpq "C:\Jeux\World of Warcraft 3.3.5\Data\patch-Z.mpq" --base-dbc "btl-work/bases/files/DBFilesClient/Spell.dbc" --export traductions.json

python translate.py --stormlib "D:\repo\ezrothcore\StormLib.dll" --mpq "C:\Jeux\World of Warcraft 3.3.5\Data\patch-Z.mpq" --apply traductions_frFR.json --in-place

rmdir /s /q btl-work