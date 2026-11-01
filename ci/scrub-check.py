#!/usr/bin/env python3
# The seek bar's scrub, checked on the player itself over its --ipc socket
# (ci/smoke.sh starts the player on its first library, offscreen and muted,
# and runs this): a playing track fades out and pauses under the handle,
# follows it, and plays on with its sound fading back in once the handle is
# let go; a paused track shows the same and stays paused; Space under the
# handle turns that round; a track that runs into the next one under the
# handle (gapless) lets the scrub go. The music library, so that no video
# is needed: a track played from its file goes through the same player.
# Usage: scrub-check.py <socket>
import json
import socket
import sys
import time


class Player:
    def __init__(self, path, timeout=60):
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.settimeout(5)  # a reply that never comes fails the check rather than hang it
                self.sock.connect(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.5)
        self.lines = self.sock.makefile("rb")

    def __call__(self, line):
        self.sock.sendall((line + "\n").encode())
        return self.lines.readline().decode().rstrip("\n")

    def state(self):
        return json.loads(self("state"))

    # The state once `pred` holds, or as it stands when `timeout` is up.
    def wait(self, pred, timeout=5):
        deadline = time.time() + timeout
        while True:
            s = self.state()
            if pred(s) or time.time() > deadline:
                return s
            time.sleep(0.05)


KEYS = ("position", "paused", "fade", "scrubbing", "scrubHold", "resuming", "playingNow", "index", "pendingNext")


def main():
    player = Player(sys.argv[1])
    failed = []

    def check(name, s, ok):
        print(("ok    " if ok else "FAIL  ") + name, {k: s.get(k) for k in KEYS})
        if not ok:
            failed.append(name)

    player("mode music")
    s = player.wait(lambda s: s["albumsShown"] >= 1 and not s["scanning"], 60)
    check("the library is up", s, s["albumsShown"] >= 1)
    player("play-album 0")
    s = player.wait(lambda s: s["active"] and s["queueKind"] == "track" and s["position"] > 0.5 and not s["paused"], 30)
    check("a track plays from its file", s, s["active"] and not s["paused"] and s["position"] > 0.5 and s["fade"] == 1)
    duration = s["duration"]
    check("the track is long enough for this", s, duration >= 30)

    # Taken hold of while playing: the sound goes, then the file pauses and
    # the picture follows the handle. Outside, it still counts as playing.
    reply = player("scrub 20")
    s = player.wait(lambda s: s["paused"] and s["fade"] == 0)
    check("held: paused under the handle once the sound has gone (" + reply + ")", s,
          reply == "ok" and s["paused"] and s["fade"] == 0 and s["scrubbing"] and s["scrubHold"])
    s = player.wait(lambda s: abs(s["position"] - 20) < 2)
    check("held: the picture at the handle", s, abs(s["position"] - 20) < 2)
    player("scrub 30")
    s = player.wait(lambda s: abs(s["position"] - 30) < 2)
    check("held: follows the handle", s, abs(s["position"] - 30) < 2 and s["paused"])
    at = s["position"]
    time.sleep(0.8)
    s = player.state()
    check("held: nothing plays", s, s["paused"] and abs(s["position"] - at) < 0.3)
    check("held: no pause to anyone outside", s, s["playingNow"] is True)

    # Let go: there exactly, then the sound back from nothing, and on.
    player("scrub-end 25")
    s = player.wait(lambda s: not s["paused"])
    check("let go: plays on", s, not s["paused"] and not s["scrubbing"])
    s = player.wait(lambda s: s["fade"] == 1)
    check("let go: the sound back", s, s["fade"] == 1 and not s["scrubHold"] and not s["resuming"])
    check("let go: from where it was let go", s, 24.5 <= s["position"] < 28)
    at = s["position"]
    time.sleep(0.6)
    s = player.state()
    check("let go: and on from there", s, s["position"] > at + 0.3)

    # A click: let go before the sound has gone; never paused.
    player("scrub 10")
    player("scrub-end 10")
    s = player.wait(lambda s: s["fade"] == 1 and abs(s["position"] - 10) < 2 and not s["paused"])
    check("a click seeks at once", s, s["fade"] == 1 and abs(s["position"] - 10) < 2 and not s["paused"] and not s["scrubHold"])

    # A paused track: the picture follows the handle; it stays paused.
    player("toggle")
    s = player.wait(lambda s: s["paused"])
    check("paused by Space", s, s["paused"] and s["playingNow"] is False)
    player("scrub 5")
    s = player.wait(lambda s: abs(s["position"] - 5) < 2)
    check("paused: the picture at the handle, no hold", s, abs(s["position"] - 5) < 2 and s["fade"] == 1 and not s["scrubHold"] and s["playingNow"] is False)
    player("scrub-end 7")
    time.sleep(0.6)
    s = player.state()
    check("paused: let go, stays paused at full gain", s, s["paused"] and s["fade"] == 1 and not s["resuming"] and abs(s["position"] - 7) < 1)

    # Space under the handle: a playing track will stay paused instead.
    player("toggle")
    s = player.wait(lambda s: not s["paused"])
    player("scrub 15")
    s = player.wait(lambda s: s["paused"] and s["fade"] == 0)
    check("held again", s, s["paused"] and s["scrubHold"] and s["playingNow"] is True)
    player("toggle")
    s = player.state()
    check("Space under the handle: will stay paused", s, s["scrubbing"] and not s["scrubHold"] and s["playingNow"] is False)
    player("scrub-end 16")
    time.sleep(0.6)
    s = player.state()
    check("let go: stays paused at full gain", s, s["paused"] and s["fade"] == 1 and not s["resuming"])
    player("toggle")
    s = player.wait(lambda s: not s["paused"])
    check("Space again: plays", s, not s["paused"] and s["fade"] == 1)

    # The track runs into the next one under the handle: the scrub lets go,
    # and the next track plays on at full gain, from its start.
    s = player.wait(lambda s: s["pendingNext"] == 1, 10)
    check("the next track is queued", s, s["pendingNext"] == 1 and s["index"] == 0)
    player("seek %.2f" % (duration - 0.15))  # it ends while the scrub's sound is still going
    player("scrub 30")
    s = player.wait(lambda s: s["index"] == 1, 10)
    check("the next track took over", s, s["index"] == 1)
    s = player.wait(lambda s: not s["scrubbing"] and s["fade"] == 1 and not s["paused"])
    check("the scrub let go of it", s, not s["scrubbing"] and not s["scrubHold"] and not s["resuming"] and s["fade"] == 1 and not s["paused"])
    # (Its position is reported a moment after the queue has moved on.)
    s = player.wait(lambda s: s["position"] < 10)
    check("the next track is from its start", s, s["position"] < 10 and not s["paused"])
    player("scrub-end 30")
    time.sleep(0.6)
    s = player.state()
    check("the release of the old hold does nothing", s, not s["paused"] and s["position"] < 12 and s["fade"] == 1)

    player("quit")
    print("scrub check:", "ok" if not failed else "FAILED " + ", ".join(failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
