# Regenerates data/www/*.gz at the start of every `pio` invocation (see
# platformio.ini's extra_scripts below), so ESPAsyncWebServer's static handler
# finds a pre-compressed variant for every served asset - eliminating the
# "does not exist" VFS error it otherwise logs on every uncached request
# while probing for one - and the panel transfers less data over WiFi.
# Generated fresh every time rather than committed: a hand-maintained .gz the
# library always prefers over the plain file, forgotten after an edit, would
# silently serve stale content forever (see .gitignore - these never go into
# version control at all).
#
# Runs as a bare module-level call, not env.AddPreAction("$BUILD_DIR/
# littlefs.bin", ...) or env.AddPreAction("buildfs", ...) - both were tried
# and both are unreliable on this PlatformIO/espressif32 version: the former
# never fires at all (silently, no error - the target name it expects doesn't
# match whatever node mklittlefs actually registers here), the latter fires
# but AFTER mklittlefs has already packaged littlefs.bin from whatever .gz
# files existed on disk at that point (SCons builds an alias's dependencies,
# here the littlefs.bin file target, before the alias's own actions) - so a
# stale .gz from a previous build gets baked into uploadfs's output even
# though this script "ran". Real incident: an app.js.gz from 2026-08-03
# survived several `uploadfs` runs after `app.js` itself was edited on
# 2026-08-04, silently serving old panel logic - confirmed by fetching
# /app.js from the device and finding the old code in it. Calling this
# directly at import time sidesteps target-name fragility entirely: it always
# runs first, on every single `pio` command (not just buildfs/uploadfs) -
# cheap enough (four small files) that doing it unconditionally costs nothing
# worth guarding against.

import gzip
import os

Import("env")


def gzip_www():
    www_dir = os.path.join(env["PROJECT_DIR"], "data", "www")
    exts = (".html", ".css", ".js", ".ico")
    for name in os.listdir(www_dir):
        if name.endswith(".gz") or not name.endswith(exts):
            continue
        src_path = os.path.join(www_dir, name)
        gz_path = src_path + ".gz"
        with open(src_path, "rb") as f_in:
            data = f_in.read()
        # mtime=0: byte-for-byte identical output for identical input,
        # regardless of when it was generated - not load-bearing since these
        # are never committed, but there is no reason for it to be otherwise.
        with gzip.GzipFile(gz_path, "wb", mtime=0) as f_out:
            f_out.write(data)
        print("gzip_www: %s -> %s" % (src_path, gz_path))


gzip_www()
