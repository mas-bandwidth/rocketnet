-------------------------- MODULE RNRttTreeLayout --------------------------
(***************************************************************************)
(* The rtt history's minimum tree at the code's layout                     *)
(* (sdk/rocketnet_ack.cpp: rocketnet_ack_stats_rtt_node,                   *)
(* rocketnet_ack_stats_rtt_write, rocketnet_ack_endpoint_reset, read by    *)
(* rocketnet_ack_stats_publish_rtt).                                       *)
(*                                                                         *)
(* rtt_history is a ring of L samples (2048 in the code), written at       *)
(* sequence % L by the first block that acknowledges the sequence; an      *)
(* empty or non-positive sample is no sample.  stats_rtt_min_tree is an    *)
(* implicit binary tree in an array of L floats: node k (1 <= k < L) is    *)
(* the minimum of nodes 2k and 2k + 1, a node at or past L is the history  *)
(* entry node - L (NONE when not positive), and node 1 is the minimum      *)
(* positive sample.  A write updates the leaf, then walks k = (index + L)  *)
(* >> 1 up to the root, stopping at the first node whose minimum does not  *)
(* change.  The reset fills the history with -1 and the tree with NONE.    *)
(*                                                                         *)
(* Samples are small integers here; NONE stands for 1e9f.                  *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "nooffset"     the walk starts at index >> 1, without the leaf        *)
(*                  offset L (TreeExact);                                  *)
(*   "decreaseonly" the walk stops at the first node whose minimum does    *)
(*                  not fall, so a replaced minimum is never raised        *)
(*                  (RootIsMinimum).                                       *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS
    L,          \* ROCKETNET_RTT_HISTORY_SIZE, a power of two
    Samples,    \* the sample values a write may carry (non-positive ones too)
    NONE,       \* ROCKETNET_ACK_RTT_NONE, above every sample
    Broken      \* "none", or the reversed witness

ASSUME /\ L \in Nat /\ L >= 2
       /\ Samples \subseteq Int /\ \A v \in Samples : v < NONE
       /\ Broken \in {"none", "nooffset", "decreaseonly"}

Index == 0..L-1
Nodes == 1..L-1

VARIABLES
    history,    \* endpoint->rtt_history
    tree        \* endpoint->stats_rtt_min_tree, nodes 1 .. L-1 (0 unused)

vars == <<history, tree>>

Min(a, b) == IF a < b THEN a ELSE b

\* rocketnet_ack_stats_rtt_node
NodeVal(h, t, n) ==
    IF n < L THEN t[n]
    ELSE LET v == h[n - L] IN IF v > 0 /\ v < NONE THEN v ELSE NONE

\* rocketnet_ack_endpoint_reset
Init ==
    /\ history = [i \in Index |-> -1]
    /\ tree = [k \in 0..L-1 |-> NONE]

\* rocketnet_ack_stats_rtt_write: the leaf, then the walk to the root
Write(i, v) ==
    LET h == [history EXCEPT ![i] = v]
        RECURSIVE Walk(_, _)
        Walk(t, k) ==
            IF k < 1 THEN t
            ELSE LET m == Min(NodeVal(h, t, 2 * k), NodeVal(h, t, 2 * k + 1))
                     stop == IF Broken = "decreaseonly" THEN t[k] <= m ELSE t[k] = m
                 IN IF stop THEN t ELSE Walk([t EXCEPT ![k] = m], k \div 2)
        start == IF Broken = "nooffset" THEN i \div 2 ELSE (i + L) \div 2
    IN /\ history' = h
       /\ tree' = Walk(tree, start)

Next == \E i \in Index, v \in Samples : Write(i, v)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ history \in [Index -> Samples \cup {-1}]
    /\ tree \in [0..L-1 -> Samples \cup {NONE}]

\* every node is the minimum of its two children
TreeExact == \A k \in Nodes : tree[k] = Min(NodeVal(history, tree, 2 * k), NodeVal(history, tree, 2 * k + 1))

\* the root is the minimum positive sample, NONE when there is none
RootIsMinimum ==
    LET pos == {history[i] : i \in {j \in Index : history[j] > 0}}
    IN tree[1] = IF pos = {} THEN NONE ELSE CHOOSE m \in pos : \A x \in pos : m <= x

=============================================================================
