from pathlib import Path
import re
text = Path('modules/mod-flightmaster-whistle/data/sql/db-world/base_mod_flightmaster_whistle_item.sql').read_text(encoding='utf-8')
print(text)
