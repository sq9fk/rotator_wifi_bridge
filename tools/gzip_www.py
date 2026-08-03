# Regenerates data/www/*.gz right before every filesystem image build (see
# platformio.ini's extra_scripts + AddPreAction below), so ESPAsyncWebServer's
# static handler finds a pre-compressed variant for every served asset -
# eliminating the "does not exist" VFS error it otherwise logs on every
# uncached request while probing for one - and the panel transfers less data
# over WiFi. Generated fresh on every build rather than committed: a hand-
# maintained .gz the library always prefers over the plain file, forgotten
# after an edit, would silently serve stale content forever (see
# .gitignore - these never go into version control at all).

import gzip
import os

Import("env")


def gzip_www(source, target, env):
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


env.AddPreAction("$BUILD_DIR/littlefs.bin", gzip_www)
