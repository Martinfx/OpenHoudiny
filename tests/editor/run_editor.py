# The editor driven as a person would drive it -- keys, clicks, a crash --
# by a script played into its window (prototype --script), under xvfb where
# there is no display. What it did is read from the files it leaves: what
# it saved, what it kept to recover, a screenshot taken only if it was
# still running. CMakeLists.txt runs each case as a test of its own.
#
#   python3 tests/editor/run_editor.py --prototype build-editor/prototype --case quit_asks_and_saves
#   python3 tests/editor/run_editor.py --prototype build-editor/prototype --case all
import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))

# A node added through the network's Tab menu: the network panel is at the
# right, below the parameters, in the 1600 x 960 window the runs open.
ADD_NODE = "wait 20\nmove 1200 760\nkey tab\nwait 3\ntype null\nwait 3\nkey enter\nwait 5\n"
STILL_RUNNING = "wait 10\nshot still.png\n"


class Editor:
    def __init__(self, prototype, xvfb):
        self.prototype = prototype
        self.xvfb = xvfb

    def command(self, args, script):
        cmd = [self.prototype] + args + ["--script", script]
        if self.xvfb:
            cmd = [self.xvfb, "-a", "-s", "-screen 0 1600x960x24"] + cmd
        return cmd

    def run(self, folder, args, script, timeout=300):
        """Plays `script` into an editor started in `folder`; its exit code."""
        path = os.path.join(folder, "case.script")
        with open(path, "w") as f:
            f.write(script)
        r = subprocess.run(self.command(args, path), cwd=folder, timeout=timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with open(os.path.join(folder, "editor.log"), "ab") as log:
            log.write(r.stdout)
        return r.returncode

    def crash(self, folder, args, script, until, timeout=120):
        """Starts it on `script`, waits for `until()` to hold, then kills it -- as a crash would."""
        path = os.path.join(folder, "crash.script")
        with open(path, "w") as f:
            f.write(script)
        p = subprocess.Popen(self.command(args, path), cwd=folder, start_new_session=True,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        start = time.time()
        try:
            while not until():
                if time.time() - start > timeout:
                    return False
                if p.poll() is not None:
                    return False
                time.sleep(0.5)
            return True
        finally:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            p.wait()


def network(folder):
    """A network on disk: the subdivision example, its USD file found from anywhere."""
    with open(os.path.join(ROOT, "examples", "sim", "usd_subdivision.pgsim")) as f:
        text = f.read()
    text = text.replace('"../usd/subdivision.usda"', '"%s"' % os.path.join(ROOT, "examples", "usd", "subdivision.usda"))
    path = os.path.join(folder, "net.pgsim")
    with open(path, "w") as f:
        f.write(text)
    return path, text


def read(path):
    with open(path) as f:
        return f.read()


def check(condition, what):
    if not condition:
        raise AssertionError(what)


# --- the cases ---------------------------------------------------------------------------------------

def quit_asks_and_saves(ed, folder):
    path, before = network(folder)
    code = ed.run(folder, ["net.pgsim", "--recovery", "rec"], ADD_NODE + "key ctrl+q\nwait 5\nkey enter\n" + STILL_RUNNING)
    check(code == 0, "the editor exited with %d" % code)
    check("null1" in read(path), "Save on quitting did not save the node added")
    check(not os.path.exists(os.path.join(folder, "still.png")), "the editor did not quit after saving")
    check(not any(f.endswith(".part") for f in os.listdir(folder)), "a .part file was left")


def quit_lets_go(ed, folder):
    path, before = network(folder)
    code = ed.run(folder, ["net.pgsim", "--recovery", "rec"], ADD_NODE + "key ctrl+q\nwait 5\nkey d\n" + STILL_RUNNING)
    check(code == 0, "the editor exited with %d" % code)
    check(read(path) == before, "Don't Save changed the file")
    check(not os.path.exists(os.path.join(folder, "still.png")), "the editor did not quit")
    check(not os.listdir(os.path.join(folder, "rec")), "an autosave was left after letting the changes go")


def quit_cancelled(ed, folder):
    path, before = network(folder)
    code = ed.run(folder, ["net.pgsim", "--recovery", "rec"], ADD_NODE + "key ctrl+q\nwait 5\nkey escape\n" + STILL_RUNNING)
    check(code == 0, "the editor exited with %d" % code)
    check(read(path) == before, "Cancel changed the file")
    check(os.path.exists(os.path.join(folder, "still.png")), "Cancel did not keep the editor running")


def example_saved_as_then_replaced(ed, folder):
    # An example has no file: Save on quitting asks for one first.
    script = ADD_NODE + "key ctrl+q\nwait 5\nkey enter\nwait 5\nkey enter\n" + STILL_RUNNING
    code = ed.run(folder, ["--example", "usd_looks", "--recovery", "rec"], script)
    saved = os.path.join(folder, "usd_looks.pgsim")
    check(code == 0, "the editor exited with %d" % code)
    check(os.path.exists(saved) and "null1" in read(saved), "Save As on quitting did not write the example")
    check(not os.path.exists(os.path.join(folder, "still.png")), "the editor did not quit after Save As")
    # Again: the file is there -- asked first, Enter again replaces it.
    os.utime(saved, (0, 0))
    script = ADD_NODE + "key ctrl+q\nwait 5\nkey enter\nwait 5\nkey enter\nwait 5\nshot asked.png\nkey enter\n" + STILL_RUNNING
    code = ed.run(folder, ["--example", "usd_looks", "--recovery", "rec"], script)
    check(code == 0, "the editor exited with %d" % code)
    check(os.path.exists(os.path.join(folder, "asked.png")), "it did not wait to be asked about replacing the file")
    check(os.path.getmtime(saved) > 0, "Replace did not write the file")
    check(not os.path.exists(os.path.join(folder, "still.png")), "the editor did not quit after replacing")


def crash_recovered(ed, folder):
    path, before = network(folder)
    rec = os.path.join(folder, "rec")
    os.makedirs(rec)
    kept = lambda: any(f.endswith(".pgsim") for f in os.listdir(rec))
    check(ed.crash(folder, ["net.pgsim", "--recovery", rec], ADD_NODE + "wait 3000\n", kept),
          "nothing was kept to recover before the crash")
    check(read(path) == before, "the crash changed the file")
    # Started again elsewhere: offered back; Enter recovers it, Ctrl+S saves it to its file.
    elsewhere = os.path.join(folder, "elsewhere")
    os.makedirs(elsewhere)
    code = ed.run(elsewhere, ["--recovery", rec], "wait 30\nkey enter\nwait 10\nkey ctrl+s\nwait 5\n")
    check(code == 0, "the editor exited with %d" % code)
    check("null1" in read(path), "the network recovered was not saved to its file")
    check(not os.listdir(rec), "autosaves were left: %s" % os.listdir(rec))


def simulate_again(ed, folder):
    # Simulation > Simulate Again, four times while the campfire simulates:
    # the menu at (234, 13), the item at (245, 131).
    again = "click 234 13\nwait 2\nclick 245 131\nwait 1\n"
    code = ed.run(folder, ["--example", "campfire", "--recovery", "rec"], "wait 40\n" + again * 4 + "wait 30\nshot after.png\n")
    check(code == 0, "the editor exited with %d" % code)
    check(os.path.exists(os.path.join(folder, "after.png")), "the editor did not run on after Simulate Again")


CASES = {f.__name__: f for f in (quit_asks_and_saves, quit_lets_go, quit_cancelled, example_saved_as_then_replaced,
                                   crash_recovered, simulate_again)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prototype", required=True)
    parser.add_argument("--xvfb", default=shutil.which("xvfb-run") or "")
    parser.add_argument("--case", default="all")
    parser.add_argument("--keep", action="store_true", help="keep the folders the cases ran in")
    args = parser.parse_args()
    if not args.xvfb and not os.environ.get("DISPLAY"):
        print("no display and no xvfb-run: skipped")
        return 0
    ed = Editor(os.path.abspath(args.prototype), args.xvfb)
    names = list(CASES) if args.case == "all" else [args.case]
    failed = 0
    for name in names:
        folder = tempfile.mkdtemp(prefix="pg_editor_%s_" % name)
        start = time.time()
        try:
            CASES[name](ed, folder)
            print("  ok    %s (%.0f s)" % (name, time.time() - start))
            if not args.keep:
                shutil.rmtree(folder, ignore_errors=True)
        except Exception as e:  # noqa: BLE001 -- every failure reported, the rest run
            failed += 1
            print("  FAIL  %s: %s -- in %s" % (name, e, folder))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
