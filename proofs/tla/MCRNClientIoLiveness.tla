----------------------- MODULE MCRNClientIoLiveness -----------------------
\* Split only finite conjunctions of temporal obligations.
\* All checks use the complete RNClientIo state graph, both send rings,
\* every slot, and unchanged SpecFair. Each family of conjuncts below is,
\* together, exactly one original property of RNClientIo, and the ASSUME
\* beside it makes the partition complete: a config change that would add
\* an element no conjunct names fails at parse time instead of silently
\* checking less.
EXTENDS RNClientIo

\* CommittedEventuallyHandedOff == \A r \in Rings, s \in Slots : P(r, s).
\* Split by ring and then by slot: four conjuncts for {"A","B"} x {1,2}.
ASSUME Rings = {"A", "B"}
ASSUME Slots = {1, 2}

CommittedSlot(r, s) ==
    owner[r][s] = "main" ~> owner[r][s] = "io"

\* conjunct r = "A", s = 1 of CommittedEventuallyHandedOff
CommittedA1 == CommittedSlot("A", 1)
\* conjunct r = "A", s = 2 of CommittedEventuallyHandedOff
CommittedA2 == CommittedSlot("A", 2)
\* conjunct r = "B", s = 1 of CommittedEventuallyHandedOff
CommittedB1 == CommittedSlot("B", 1)
\* conjunct r = "B", s = 2 of CommittedEventuallyHandedOff
CommittedB2 == CommittedSlot("B", 2)

\* HandedOffEventuallySent == \A r \in Rings : P(r); split by ring
\* (Rings = {"A", "B"}, assumed above).
HandedOffRing(r) ==
    ring[r] = "HandedOff" ~> ring[r] = "Sent"

\* conjunct r = "A" of HandedOffEventuallySent
HandedOffA == HandedOffRing("A")
\* conjunct r = "B" of HandedOffEventuallySent
HandedOffB == HandedOffRing("B")

\* FilledEventuallyExposed == \A b \in Buffers : P(b); split by buffer.
ASSUME Buffers = {0, 1}

FilledBuffer(b) ==
    (buf[b] = "Fill" /\ count[b] > 0) ~> (buf[b] = "Exposed" \/ phase # "Running")

\* conjunct b = 0 of FilledEventuallyExposed
Filled0 == FilledBuffer(0)
\* conjunct b = 1 of FilledEventuallyExposed
Filled1 == FilledBuffer(1)

\* ArrivedEventuallyDelivered == \A n \in 1..MaxDatagrams : P(n); split by
\* threshold: ArrivedFirst (n = 1), ArrivedSecond (n = 2), ArrivedLast
\* (n = MaxDatagrams = 3).
ASSUME MaxDatagrams = 3

ArrivedThreshold(n) ==
    arrived >= n ~> (delivered >= n \/ phase # "Running")

\* conjunct n = 1 of ArrivedEventuallyDelivered
ArrivedFirst == ArrivedThreshold(1)
\* conjunct n = 2 of ArrivedEventuallyDelivered
ArrivedSecond == ArrivedThreshold(2)
\* conjunct n = MaxDatagrams of ArrivedEventuallyDelivered
ArrivedLast ==
    arrived >= MaxDatagrams ~> (delivered >= MaxDatagrams \/ phase # "Running")
=============================================================================
