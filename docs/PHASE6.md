# Phase 6 — explained (Raft leader election)

This is the capstone, and the famous hard one. Phase 5 gave us a leader and
followers — but if the leader's machine died, a **human** had to pick a new leader
and repoint everyone. Phase 6 makes that **automatic** using the **leader-election
half of the Raft consensus algorithm**: the surviving nodes *vote* to elect a new
leader, and they're guaranteed never to disagree about who's in charge.

```
   3 nodes, all Followers
        │  (no heartbeats heard — leader is dead)
        ▼
   one times out first ──► becomes a CANDIDATE (term++), votes for itself,
                           asks the others: "vote for me?"  (RequestVote)
        │
        ▼
   gets a MAJORITY of votes ──► becomes the LEADER, sends heartbeats
                                (everyone else stays Follower)
```

## The core ideas

Raft is built from a few simple, interlocking rules:

| Concept | What it is |
|---|---|
| **term** | a logical clock / "generation number". It only ever goes up. Every leader belongs to exactly one term. |
| **roles** | each node is a **Follower**, **Candidate**, or **Leader** at any moment. |
| **election timeout** | a Follower that hears no leader heartbeat for a *randomized* time (here 400–800 ms) suspects the leader is dead and starts an election. |
| **vote** | each node grants **at most one vote per term**. |
| **quorum (majority)** | a candidate needs votes from **more than half** the cluster to become leader. |
| **heartbeat** | the leader sends empty `AppendEntries` RPCs (~every 100 ms) to stop followers from starting elections. |

Two of these rules together give Raft's headline safety property — **at most one
leader per term**:

> Each node votes at most once per term, and a majority is needed to win. Two
> different candidates can't *both* get a majority in the same term, because the
> two majorities would have to overlap in at least one node — and that node only
> voted once. So no term ever has two leaders.

And **randomized** election timeouts mean the nodes rarely time out at the same
instant, so usually one candidate gets ahead and wins before the others start —
avoiding endless **split votes**.

## The two RPCs

Nodes talk to each other over the *same* line protocol clients use — replication
in Phase 5 reused it, and so does Raft:

```
RequestVote   "__RAFT_VOTE__ <term> <candidate>"   →  "__VOTE__ <term> <0|1>"
AppendEntries "__RAFT_APPEND__ <term> <leader>"     →  "__APPENDED__ <term> <0|1>"
```

`AppendEntries` here carries no log entries — it's purely a heartbeat (this is the
election-only build). The handlers ([raft.cpp](../src/raft.cpp)) implement the
standard term rules:

```cpp
// RequestVote receiver
if (term > current_term_) become_follower(term);          // newer term: step down, clear our vote
bool granted = (term == current_term_ &&
                (voted_for_.empty() || voted_for_ == candidate));  // ≤ 1 vote / term
if (granted) { voted_for_ = candidate; reset_election_deadline(); }
reply (current_term_, granted);

// AppendEntries (heartbeat) receiver
if (term >= current_term_) {                               // a real leader for our term (or newer)
    current_term_ = max(current_term_, term);
    role_ = Follower;                                      // stand down if we were a candidate/leader
    leader_id_ = leader;
    reset_election_deadline();                             // ...and don't start an election
    success = true;
}
reply (current_term_, success);
```

Granting a vote or receiving a valid heartbeat **resets the election timer** — that's
how a healthy leader keeps everyone calm, and how a fresh vote gives the candidate
time to win.

## The election driver ([raft.cpp](../src/raft.cpp))

Each node runs **one timer thread** (`tick_loop`) that wakes every 20 ms:

- **Leader?** If a heartbeat interval has passed, send heartbeats to all peers.
- **Otherwise?** If we're past our (randomized) election deadline, start an election.

Starting an election:
```
become Candidate; current_term_++; vote for self; reset the (random) deadline
for each peer: send RequestVote(term, self)
    if a reply's term > ours: step down to Follower; stop.
    if granted and our votes reach a MAJORITY: become Leader; send heartbeats; stop.
```

## The concurrency rule that matters most

This is the part to really understand, because it's where home-grown Raft usually
goes wrong. The Raft state (term, role, vote) is shared between the **timer
thread** (which sends RPCs) and the **worker threads** (which handle *incoming*
RPCs). Both touch it under one `std::mutex`. The golden rule:

> **Never hold the lock while doing network I/O**, and **re-check your state after
> every RPC reply**.

So `start_election` grabs the lock only to *decide* and to *snapshot* (the term,
the peer list), **releases** it, does the (possibly slow) network sends, then
re-acquires the lock per reply — and before acting on a reply it checks "am I
*still* a candidate in *this same* term?". If an incoming RPC bumped our term or
made us a follower while we were waiting on the network, we abandon the stale
election. This is the same "release the lock during slow work" discipline as the
Phase 2 thread pool and the Phase 5 replicator — here it's load-bearing for
*correctness*, not just performance: holding the lock across a network call would
let one slow peer freeze the whole node (and could deadlock against an inbound RPC
trying to take the same lock).

## See it work

```sh
# three nodes, each listing the OTHER two as --raft peers
redon-server 127.0.0.1 6510 4 none 0 0 --raft 127.0.0.1:6511 --raft 127.0.0.1:6512
redon-server 127.0.0.1 6511 4 none 0 0 --raft 127.0.0.1:6510 --raft 127.0.0.1:6512
redon-server 127.0.0.1 6512 4 none 0 0 --raft 127.0.0.1:6510 --raft 127.0.0.1:6511
```
```
# ask each node its role:
redon-cli 6510  # ROLE  ->  role=follower term=1 leader=127.0.0.1:6511
redon-cli 6511  # ROLE  ->  role=leader   term=1 leader=127.0.0.1:6511
# a write to a follower is redirected to the leader:
redon-cli 6510  # SET k v  ->  ERR NOTLEADER 127.0.0.1:6511
# the leader accepts it:
redon-cli 6511  # SET k v  ->  OK
```
Now **kill the leader**. Within about a second the survivors elect a new one (a
higher term), and `ROLE` on a survivor shows `role=leader`. That automatic failover
is the whole point of Raft. (You may briefly see `role=candidate` with the term
ticking up — that's a split vote resolving, which is normal.)

## What this build does and doesn't do (honesty)

- ✅ **Elects exactly one leader per term**, automatically, and re-elects on
  failure — the consensus-on-*who-leads* problem, solved.
- ✅ Writes are routed to the leader (`ERR NOTLEADER <addr>`), the way real clients
  discover the leader.
- ⚠️ **Election only — the data is not replicated through Raft.** True Raft moves
  every write through a replicated **log** with extra safety rules (the
  log-up-to-date check in RequestVote, commit indexes, log matching). That's the
  *other* half of Raft and a substantial amount more code; here, leadership is
  elected but the key/value data isn't carried over the Raft log. Combining this
  election with Phase 5's data replication (the elected leader streams data to the
  others) is the natural next step.
- ⚠️ **Term/vote aren't persisted to disk.** Real Raft writes `currentTerm` and
  `votedFor` to stable storage before replying, so a crashed-and-restarted node
  can't vote twice in a term. We keep them in memory (fine for a learning demo,
  unsafe across a crash-restart).
- ⚠️ **Sequential RPCs** — fine for a handful of nodes on a LAN; a large cluster
  would send RequestVote/heartbeats in parallel.
- ⚠️ **The Raft RPCs share the client port and aren't authenticated.** A client
  that can reach the data port could send a forged `__RAFT_APPEND__` with a huge
  term to demote the leader or redirect clients. This assumes a **trusted private
  network** (as most clustered databases do). Hardening it would mean a separate
  Raft port or a shared cluster secret on the RPCs.

## New ideas this phase introduces

| Concept | Meaning here |
|---|---|
| **consensus** | independent nodes agreeing on one value (here: who is leader) |
| **term** | a monotonically increasing generation number; one leader per term |
| **election timeout** | randomized wait before a follower starts an election |
| **quorum / majority** | > half the nodes; the overlap of any two majorities is what guarantees one leader |
| **split vote** | nobody gets a majority; randomization + a new term resolves it |
| **heartbeat** | the leader's "I'm still alive" that suppresses elections |
| **automatic failover** | the cluster elects a new leader with no human involved |
| **state-under-one-lock + release for I/O** | the concurrency discipline that keeps it correct |

**Recap:** Redon now reaches **consensus** on its leader. Nodes elect a leader by
majority vote using terms and randomized timeouts; the majority rule guarantees at
most one leader per term; heartbeats keep a healthy leader in place; and when the
leader dies, the survivors automatically elect a new one. The deliberate gap —
carrying the *data* through Raft's replicated log — is the boundary between
"leader election" and "full Raft," and a great direction to take the project next.
