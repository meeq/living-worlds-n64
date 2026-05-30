"""HTTP download helper for the asset fetchers.

Retries transient failures (timeouts, connection errors, 5xx) and bails out
immediately on 4xx so a typo doesn't hammer the upstream server.
"""

import time
import urllib.error
import urllib.request
from pathlib import Path

USER_AGENT = "Mozilla/5.0 (Living Worlds N64 demo asset fetcher)"
TIMEOUT = 30
RETRY_DELAY = 1
RETRIES = 3


def fetch(url: str, dest: Path) -> bool:
    """GET `url` into `dest`. Returns True on success, False after all retries."""
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    for attempt in range(RETRIES):
        try:
            with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
                dest.write_bytes(r.read())
            return True
        except urllib.error.HTTPError as e:
            if e.code < 500:
                return False
        except (urllib.error.URLError, TimeoutError):
            pass
        if attempt < RETRIES - 1:
            time.sleep(RETRY_DELAY)
    return False
