"""Find installed Sidebar language folders and share their file layout."""

import json
import ntpath
from pathlib import Path
import re
import tempfile


BACKUP = r"C:\ProgramData\TritonSidebarGadgets\original"
GADGETS = {
    "RSSFeeds.Gadget": ("RSSFeeds.html", (("js\\RSSFeeds.js", "rss"),)),
    "Weather.Gadget": ("weather.html", (("js\\weather.js", "weather"), ("js\\settings.js", "weather-settings"))),
    "Currency.Gadget": ("currency.html", (("js\\service.js", "currency"),)),
}


def parse_folders(output):
    folders = []
    for line in output.splitlines():
        if not line.startswith("SIDEBAR_FOLDER|"):
            continue
        _, gadget, folder = line.split("|", 2)
        if gadget not in GADGETS or any(c in folder for c in '\"&|<>^%\r\n'):
            raise RuntimeError("Invalid Sidebar folder")
        parts = folder.split("\\")
        if not re.match(r"^[A-Za-z]:\\", folder) or ".." in parts:
            raise RuntimeError("Invalid Sidebar path")
        if parts[-1] != gadget and (len(parts) < 2 or parts[-2] != gadget or not re.fullmatch(r"[A-Za-z0-9-]+", parts[-1])):
            raise RuntimeError("Invalid Sidebar language folder")
        if (gadget, folder) not in folders:
            folders.append((gadget, folder))
    missing = set(GADGETS) - {gadget for gadget, folder in folders}
    if missing:
        raise RuntimeError("Missing installed gadgets: " + ", ".join(sorted(missing)))
    return folders


def discover_gadgets(control):
    # WSH and FileSystemObject are available on Vista without PowerShell.
    definitions = {name: [page] + [path for path, kind in scripts]
                   for name, (page, scripts) in GADGETS.items()}
    source = "var gadgets = " + json.dumps(definitions) + ";\n" + r'''
var fso = new ActiveXObject("Scripting.FileSystemObject");
var shell = new ActiveXObject("WScript.Shell");
var programFiles = shell.ExpandEnvironmentStrings("%ProgramW6432%");
if (programFiles == "%ProgramW6432%") programFiles = shell.ExpandEnvironmentStrings("%ProgramFiles%");
var base = fso.BuildPath(programFiles, "Windows Sidebar\\Gadgets");
function inspect(name, folder) {
    if (!fso.FileExists(fso.BuildPath(folder, gadgets[name][0]))) return;
    for (var i = 1; i < gadgets[name].length; i++) {
        if (!fso.FileExists(fso.BuildPath(folder, gadgets[name][i]))) {
            WScript.Echo("Missing gadget script: " + folder + "\\" + gadgets[name][i]);
            WScript.Quit(1);
        }
    }
    WScript.Echo("SIDEBAR_FOLDER|" + name + "|" + folder);
}
for (var name in gadgets) {
    var root = fso.BuildPath(base, name);
    if (!fso.FolderExists(root)) continue;
    inspect(name, root);
    for (var folders = new Enumerator(fso.GetFolder(root).SubFolders); !folders.atEnd(); folders.moveNext()) {
        inspect(name, folders.item().Path);
    }
}
'''
    remote = r"C:\Windows\Temp\TritonSidebarFolders.js"
    with tempfile.TemporaryDirectory(prefix="vista-sidebar-folders-") as directory:
        script = Path(directory) / "folders.js"
        script.write_text(source, encoding="ascii")
        control("put", str(script), remote)
        output = control("run", "--timeout", "120", f'cscript //nologo "{remote}"')
    return parse_folders(output)


def gadget_files(folders):
    pages, backends = {}, {}
    for gadget, folder in folders:
        locale = ntpath.basename(folder)
        # Keep existing English backups so reruns never back up patched files.
        prefix = "" if locale.lower() == "en-us" else gadget + "\\" + locale + "\\"
        page, scripts = GADGETS[gadget]
        pages[ntpath.join(folder, page)] = prefix + page
        for relative, kind in scripts:
            backends[ntpath.join(folder, relative)] = (prefix + kind + ".backend-original.js", kind)
    return pages, backends
