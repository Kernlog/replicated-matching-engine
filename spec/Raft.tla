-------------------------------- MODULE Raft --------------------------------
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Servers, Values, Nil, MaxTerm, MaxLogLen, MaxInFlight

VARIABLES currentTerm, role, votedFor, log, commitIndex, votes, matchIndex, msgs, elected, committed

vars == <<currentTerm, role, votedFor, log, commitIndex, votes, matchIndex, msgs, elected, committed>>

IsQuorum(S) == Cardinality(S) * 2 > Cardinality(Servers)
LastTerm(l) == IF l = <<>> THEN 0 ELSE l[Len(l)].term
Min(a, b) == IF a < b THEN a ELSE b
Max(a, b) == IF a > b THEN a ELSE b
Symmetry == Permutations(Servers)

Init ==
    /\ currentTerm = [s \in Servers |-> 0]
    /\ role = [s \in Servers |-> "Follower"]
    /\ votedFor = [s \in Servers |-> Nil]
    /\ log = [s \in Servers |-> <<>>]
    /\ commitIndex = [s \in Servers |-> 0]
    /\ votes = [s \in Servers |-> {}]
    /\ matchIndex = [s \in Servers |-> [t \in Servers |-> 0]]
    /\ msgs = {}
    /\ elected = {}
    /\ committed = {}

-----------------------------------------------------------------------------
(* Election *)

Timeout(s) ==
    /\ role[s] # "Leader"
    /\ currentTerm[s] < MaxTerm
    /\ currentTerm' = [currentTerm EXCEPT ![s] = @ + 1]
    /\ role' = [role EXCEPT ![s] = "Candidate"]
    /\ votedFor' = [votedFor EXCEPT ![s] = s]
    /\ votes' = [votes EXCEPT ![s] = {s}]
    /\ msgs' = msgs \cup
         {[type |-> "Vote", term |-> currentTerm[s] + 1, src |-> s, dst |-> t,
           lastTerm |-> LastTerm(log[s]), lastIndex |-> Len(log[s])] : t \in Servers \ {s}}
    /\ UNCHANGED <<log, commitIndex, matchIndex, elected, committed>>

UpdateTerm(m) ==
    /\ m.term > currentTerm[m.dst]
    /\ currentTerm' = [currentTerm EXCEPT ![m.dst] = m.term]
    /\ role' = [role EXCEPT ![m.dst] = "Follower"]
    /\ votedFor' = [votedFor EXCEPT ![m.dst] = Nil]
    /\ UNCHANGED <<log, commitIndex, votes, matchIndex, msgs, elected, committed>>

HandleVoteRequest(m) ==
    LET d == m.dst
        logOk == \/ m.lastTerm > LastTerm(log[d])
                 \/ m.lastTerm = LastTerm(log[d]) /\ m.lastIndex >= Len(log[d])
        grant == logOk /\ votedFor[d] \in {Nil, m.src}
    IN /\ m.type = "Vote"
       /\ m.term = currentTerm[d]
       /\ votedFor' = IF grant THEN [votedFor EXCEPT ![d] = m.src] ELSE votedFor
       /\ msgs' = (msgs \ {m}) \cup
            IF grant THEN {[type |-> "VoteReply", term |-> m.term, src |-> d, dst |-> m.src]}
            ELSE {}
       /\ UNCHANGED <<currentTerm, role, log, commitIndex, votes, matchIndex, elected, committed>>

HandleVoteReply(m) ==
    LET d == m.dst
    IN /\ m.type = "VoteReply"
       /\ m.term = currentTerm[d]
       /\ votes' = IF role[d] = "Candidate" THEN [votes EXCEPT ![d] = @ \cup {m.src}] ELSE votes
       /\ msgs' = msgs \ {m}
       /\ UNCHANGED <<currentTerm, role, votedFor, log, commitIndex, matchIndex, elected, committed>>

BecomeLeader(s) ==
    /\ role[s] = "Candidate"
    /\ IsQuorum(votes[s])
    /\ role' = [role EXCEPT ![s] = "Leader"]
    /\ matchIndex' = [matchIndex EXCEPT ![s] = [t \in Servers |-> IF t = s THEN Len(log[s]) ELSE 0]]
    /\ elected' = elected \cup {<<currentTerm[s], s>>}
    /\ UNCHANGED <<currentTerm, votedFor, log, commitIndex, votes, msgs, committed>>

-----------------------------------------------------------------------------
(* Replication *)

ClientRequest(s, v) ==
    /\ role[s] = "Leader"
    /\ Len(log[s]) < MaxLogLen
    /\ log' = [log EXCEPT ![s] = Append(@, [term |-> currentTerm[s], value |-> v])]
    /\ matchIndex' = [matchIndex EXCEPT ![s][s] = Len(log'[s])]
    /\ UNCHANGED <<currentTerm, role, votedFor, commitIndex, votes, msgs, elected, committed>>

SendAppend(s, t, prev) ==
    /\ role[s] = "Leader"
    /\ t # s
    /\ prev <= Len(log[s])
    /\ msgs' = msgs \cup
         {[type |-> "Append", term |-> currentTerm[s], src |-> s, dst |-> t,
           prevIndex |-> prev,
           prevTerm |-> IF prev = 0 THEN 0 ELSE log[s][prev].term,
           entries |-> SubSeq(log[s], prev + 1, Min(prev + 1, Len(log[s]))),
           commit |-> commitIndex[s]]}
    /\ UNCHANGED <<currentTerm, role, votedFor, log, commitIndex, votes, matchIndex, elected,
                   committed>>

HandleAppend(m) ==
    LET d == m.dst
        i == m.prevIndex + 1
        logOk == IF m.prevIndex = 0 THEN TRUE
                 ELSE m.prevIndex <= Len(log[d]) /\ log[d][m.prevIndex].term = m.prevTerm
        keep == IF m.entries = <<>> THEN TRUE
                ELSE Len(log[d]) >= i /\ log[d][i].term = m.entries[1].term
    IN /\ m.type = "Append"
       /\ m.term = currentTerm[d]
       /\ logOk
       /\ role' = [role EXCEPT ![d] = "Follower"]
       /\ log' = IF keep THEN log
                 ELSE [log EXCEPT ![d] = SubSeq(@, 1, m.prevIndex) \o m.entries]
       /\ commitIndex' = [commitIndex EXCEPT
                            ![d] = Max(@, Min(m.commit, m.prevIndex + Len(m.entries)))]
       /\ msgs' = (msgs \ {m}) \cup
            {[type |-> "AppendReply", term |-> m.term, src |-> d, dst |-> m.src,
              match |-> m.prevIndex + Len(m.entries)]}
       /\ UNCHANGED <<currentTerm, votedFor, votes, matchIndex, elected, committed>>

HandleAppendReply(m) ==
    LET d == m.dst
    IN /\ m.type = "AppendReply"
       /\ m.term = currentTerm[d]
       /\ matchIndex' = IF role[d] = "Leader"
                          THEN [matchIndex EXCEPT ![d][m.src] = Max(@, m.match)]
                          ELSE matchIndex
       /\ msgs' = msgs \ {m}
       /\ UNCHANGED <<currentTerm, role, votedFor, log, commitIndex, votes, elected, committed>>

AdvanceCommit(s, i) ==
    /\ role[s] = "Leader"
    /\ i \in commitIndex[s] + 1..Len(log[s])
    /\ IsQuorum({t \in Servers : matchIndex[s][t] >= i})
    /\ log[s][i].term = currentTerm[s]
    /\ commitIndex' = [commitIndex EXCEPT ![s] = i]
    /\ committed' = committed \cup
         {[index |-> j, entry |-> log[s][j], term |-> currentTerm[s]] : j \in 1..i}
    /\ UNCHANGED <<currentTerm, role, votedFor, log, votes, matchIndex, msgs, elected>>

-----------------------------------------------------------------------------
(* Faults *)

Restart(s) ==
    /\ role' = [role EXCEPT ![s] = "Follower"]
    /\ commitIndex' = [commitIndex EXCEPT ![s] = 0]
    /\ votes' = [votes EXCEPT ![s] = {}]
    /\ matchIndex' = [matchIndex EXCEPT ![s] = [t \in Servers |-> 0]]
    /\ UNCHANGED <<currentTerm, votedFor, log, msgs, elected, committed>>

Drop(m) ==
    /\ msgs' = msgs \ {m}
    /\ UNCHANGED <<currentTerm, role, votedFor, log, commitIndex, votes, matchIndex, elected,
                   committed>>

Next ==
    \/ \E s \in Servers : Timeout(s) \/ BecomeLeader(s) \/ Restart(s)
    \/ \E s \in Servers, i \in 1..MaxLogLen : AdvanceCommit(s, i)
    \/ \E s \in Servers, v \in Values : ClientRequest(s, v)
    \/ \E s, t \in Servers, prev \in 0..MaxLogLen : SendAppend(s, t, prev)
    \/ \E m \in msgs : \/ UpdateTerm(m) \/ HandleVoteRequest(m) \/ HandleVoteReply(m)
                       \/ HandleAppend(m) \/ HandleAppendReply(m) \/ Drop(m)

Spec == Init /\ [][Next]_vars

Bounds == Cardinality(msgs) <= MaxInFlight

-----------------------------------------------------------------------------
(* Invariants *)

TypeOK ==
    /\ currentTerm \in [Servers -> 0..MaxTerm]
    /\ role \in [Servers -> {"Follower", "Candidate", "Leader"}]
    /\ votedFor \in [Servers -> Servers \cup {Nil}]
    /\ \A s \in Servers : Len(log[s]) <= MaxLogLen /\ commitIndex[s] <= Len(log[s])
    /\ votes \in [Servers -> SUBSET Servers]

ElectionSafety == \A a, b \in elected : a[1] = b[1] => a[2] = b[2]

LogMatching ==
    \A s, t \in Servers : \A i \in 1..Min(Len(log[s]), Len(log[t])) :
        log[s][i].term = log[t][i].term => SubSeq(log[s], 1, i) = SubSeq(log[t], 1, i)

LeaderCompleteness ==
    \A s \in Servers : role[s] = "Leader" =>
        \A c \in committed : c.term <= currentTerm[s] =>
            c.index <= Len(log[s]) /\ log[s][c.index] = c.entry

StateMachineSafety ==
    /\ \A a, b \in committed : a.index = b.index => a.entry = b.entry
    /\ \A s \in Servers : \A i \in 1..Min(commitIndex[s], Len(log[s])) :
           \E c \in committed : c.index = i /\ c.entry = log[s][i]

=============================================================================
