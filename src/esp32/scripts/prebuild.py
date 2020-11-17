import json
import os
import re
import time
from pathlib import Path
from datetime import datetime
import inspect

# with open(os.path.join(os.environ['TEMP'], 'env.json')) as json_file:
#    env = json.load(json_file)
Import("env")
print(os.getcwd())
target = os.path.join(env["PROJECT_BUILD_DIR"], env["PIOENV"], "firmware.elf")
print("Project Target: %s" % target)
mod_path = Path(inspect.getfile(inspect.currentframe())).parent
print("executable folder: %s" % mod_path)
static_files = (mod_path / '../static').resolve()

if os.path.isfile(target):
    compressHtml = False
    updateVersion = False
    targetModified = os.path.getmtime(target)
    with os.scandir(static_files) as files:
        for entry in files:
            if entry.name.endswith(".html") and entry.is_file():
                if os.path.getmtime(entry.path) > targetModified:
                    compressHtml = True
    if compressHtml:
        os.system(
            'powershell.exe -ExecutionPolicy Bypass -Command " Set-Location \'{}\'; Get-childitem ../static -filter *.html | .\compress-file-str.ps1 -target ..\src\web_pages.h"'.format(mod_path)
        )
    else:
        print(
            "static content not modified since {}".format(
                time.strftime("%Y-%m-%d %H:%M:%S",
                              time.localtime(targetModified))
            )
        )
    all_files = []
    for ext in env["CPPSUFFIXES"]:
        all_files.extend(
            Path(env["PROJECTSRC_DIR"]).glob("**/*{}".format(ext)))
        all_files.extend(Path(env["LIBSOURCE_DIRS"][0]
                              ).glob("**/*{}".format(ext)))

else:
    os.system(
        'powershell.exe -ExecutionPolicy Bypass -Command " Set-Location \'{}\'; Get-childitem ../static -filter *.html | .\compress-file-str.ps1 -target ..\src\web_pages.h"'.format(mod_path)
    )
