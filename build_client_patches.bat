@echo off

set "STORMLIB=%~dp0StormLib.dll"

python build_client_patches.py --stormlib "%STORMLIB%" --out patch-Z.mpq --install --keep-work --overcast-sql "modules\mod-weather\data\sql\db-world\mod_weather_overcast.sql"
if errorlevel 1 exit /b 1

@REM  python translate.py --stormlib "D:\repo\ezrothcore\StormLib.dll" --mpq "C:\Jeux\World of Warcraft 3.3.5\Data\patch-Z.mpq" --base-dbc "btl-work/bases/files/DBFilesClient/Spell.dbc" --export traductions.json

python translate.py --stormlib "%STORMLIB%" --mpq "C:\Jeux\World of Warcraft 3.3.5\Data\patch-Z.mpq" --apply traductions_frFR.json --in-place
if errorlevel 1 exit /b 1

rmdir /s /q btl-work
