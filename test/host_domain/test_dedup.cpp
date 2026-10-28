// Tests DedupCache (docs/08 "Retry envelope": isRetry miss ->
// UNKNOWN_EXECUTION; cache full of active entries -> QUEUE_FULL; in-retention
// duplicate -> same outcome; evict the oldest terminal; active entries
// pinned).
#include "testkit.h"

int main() {
  using AR = DedupCache::AdmitResult;
  char outcome[kDedupOutcomeCap];

  // --- admit miss + isRetry -> UnknownExecution (no entry) --------------------
  {
    DedupCache c;
    CHECK(c.admit("r1", /*isRetry=*/true, outcome, sizeof(outcome))
          == AR::UnknownExecution);
    CHECK(!c.contains("r1"));  // creates no entry
    // First submission (isRetry=false) -> New.
    CHECK(c.admit("r1", false, outcome, sizeof(outcome)) == AR::New);
    CHECK(c.contains("r1"));
    CHECK(c.activeCount() == 1);
  }

  // --- in-retention duplicate -> same outcome ------------------------------------
  {
    DedupCache c;
    CHECK(c.admit("r1", false, outcome, sizeof(outcome)) == AR::New);
    // Duplicate while still ACTIVE -> empty outcome (none yet).
    CHECK(c.admit("r1", true, outcome, sizeof(outcome)) == AR::Duplicate);
    CHECK(outcome[0] == '\0');
    // Complete -> terminal outcome stored.
    CHECK(c.complete("r1", "{\"actionId\":\"act-1\",\"state\":\"SUCCEEDED\"}").ok());
    CHECK(c.terminalCount() == 1);
    // Duplicate (well-behaved retry) -> same outcome, no re-execution.
    CHECK(c.admit("r1", true, outcome, sizeof(outcome)) == AR::Duplicate);
    CHECK(strcmp(outcome, "{\"actionId\":\"act-1\",\"state\":\"SUCCEEDED\"}") == 0);
    CHECK(c.admit("r1", false, outcome, sizeof(outcome)) == AR::Duplicate);
    // complete on non-active / unknown entry -> NotFound.
    CHECK(c.complete("r1", "x").code == Err::NotFound);
    CHECK(c.complete("no-such", "x").code == Err::NotFound);
  }

  // --- cache full of active entries -> Full --------------------------------------------
  {
    DedupCache c;
    char id[16];
    for (int i = 0; i < 32; ++i) {
      snprintf(id, sizeof(id), "req-%d", i);
      CHECK(c.admit(id, false, nullptr, 0) == AR::New);
    }
    CHECK(c.activeCount() == 32);
    CHECK(c.admit("req-x", false, nullptr, 0) == AR::Full);
    // All active entries stay pinned (none evicted).
    for (int i = 0; i < 32; ++i) {
      snprintf(id, sizeof(id), "req-%d", i);
      CHECK(c.contains(id));
    }
  }

  // --- evict oldest terminal first; ACTIVE stays pinned ------------------------------------
  {
    DedupCache c;
    char id[16];
    for (int i = 0; i < 32; ++i) {
      snprintf(id, sizeof(id), "req-%d", i);
      CHECK(c.admit(id, false, nullptr, 0) == AR::New);
    }
    // Make terminal in order: req-0 oldest, then req-1, req-2.
    CHECK(c.complete("req-0", "o0").ok());
    CHECK(c.complete("req-1", "o1").ok());
    CHECK(c.complete("req-2", "o2").ok());
    // New admit -> evict the oldest terminal (req-0), not ACTIVE.
    CHECK(c.admit("new-1", false, nullptr, 0) == AR::New);
    CHECK(!c.contains("req-0"));
    CHECK(c.contains("req-1") && c.contains("req-2"));
    CHECK(c.contains("req-3"));  // ACTIVE stays pinned
    // Next admit -> evict req-1 (next-oldest terminal).
    CHECK(c.admit("new-2", false, nullptr, 0) == AR::New);
    CHECK(!c.contains("req-1"));
    CHECK(c.contains("req-2"));
    // req-2 was completed last -> its stamp is newer than its own
    // earlier stamp; full again: now only req-2 is terminal.
    CHECK(c.activeCount() == 31 && c.terminalCount() == 1);
    // req-2 evicted yet? not yet — check: admit -> evict req-2.
    CHECK(c.admit("new-3", false, nullptr, 0) == AR::New);
    CHECK(!c.contains("req-2"));
    CHECK(c.activeCount() == 32 && c.terminalCount() == 0);
    CHECK(c.admit("new-4", false, nullptr, 0) == AR::Full);
  }

  // --- reset (new boot) -------------------------------------------------------------
  {
    DedupCache c;
    CHECK(c.admit("r1", false, nullptr, 0) == AR::New);
    CHECK(c.complete("r1", "o").ok());
    c.reset();
    CHECK(!c.contains("r1"));
    CHECK(c.activeCount() == 0 && c.terminalCount() == 0);
    CHECK(c.admit("r1", false, nullptr, 0) == AR::New);
  }

  std::printf("PASS test_dedup\n");
  return 0;
}
