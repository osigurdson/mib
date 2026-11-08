"""Install mib's desktop entry with the final executable path (without DESTDIR)."""
from pathlib import Path
import sys

binary, destination = sys.argv[1:]
# Exec has its own quoting rules, followed by desktop-entry string escaping.
# Percent signs in the path must not be interpreted as field codes.
quoted = binary.replace('%', '%%')
for character in ('\\', '"', '`', '$'):
    quoted = quoted.replace(character, '\\' + character)
command = ('"' + quoted + '"').replace('\\', '\\\\')
command = command.replace('\n', '\\n').replace('\r', '\\r').replace('\t', '\\t')
source = Path(__file__).resolve().parent.parent / 'packaging/io.github.osigurdson.mib.desktop'
entry = source.read_text().replace('Exec=mib', 'Exec=' + command)
target = Path(destination)
target.write_text(entry)
target.chmod(0o644)
