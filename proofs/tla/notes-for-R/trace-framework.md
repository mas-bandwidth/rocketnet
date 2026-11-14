# Notes for R from trace-framework (M14)

## Comment re-citations in other packages' Trace modules (MM203)

Seven Trace modules copy the old template's reason for the argument domain.
The claim now holds, but for a different reason: the checker binds each Act arm
to the disjuncts of Next that take its action. The conjunct `Next` does not do it
on its own. Each file has this sentence (the line wrap differs between files):

    The step must be a Next step too, so Next's ranges and guards around an
    action [...] hold for the recorded arguments.

The new text, rewrapped to the file's 77-column comment box:

    The checker binds each arm of Act to the disjuncts of Next that take its
    action (README-TraceCheck.md, "Arguments"), so Next's ranges and guards
    around an action [...] hold for the arguments Next passes as bound
    variables.

The sentence is in the header comment of each of these files:
- proofs/tla/TraceRNAckApplyPhase.tla (the header comment)
- proofs/tla/TraceRNFillPhase.tla (the header comment)
- proofs/tla/TraceRNGsoQueueBankLayout.tla (the header comment)
- proofs/tla/TraceRNRioLaneLayout.tla (the header comment)
- proofs/tla/TraceRNSendReserveCalls.tla (the header comment)
- proofs/tla/TraceRNWriterFinish.tla (the header comment)
- proofs/tla/TraceRNXdpGatewayRefresh.tla (the header comment)

These are comments only. The checker compares only the module text below the
header with the template, so no trace check changes.

## Roadmap entry (MM203, held part)

  (item "trace-check-arity" :group "proof-debt" :area "proofs"
   :title "The trace check does not count a record's arguments"
   :text "MM203's held half. The checker ties each recorded argument Next passes as a bound variable, but does not check how many arguments a record carries: tests/tla_trace_bank.cpp (tla_sequence of the Finish and FinishRefused records) records three for FinishRefused(t, k). Record <<t, k>> for FinishRefused there, then add Len(traceArgs) = n to each bound arm in tools/tla_trace_check.py occurrences() and restore the ArgsTooLong domain control (a Drain record with two arguments, rejected bound)."
   :date "2026-11-13" :origin "MODELS-MERGED.md MM203; models/v1.1.x/trace-framework")

## MA005: the deadlock column of the RNSecurityTables cases (R's)

tools/check_tla_records.py now checks a cfg's CHECK_DEADLOCK against the case's
deadlock column. The 23 MCRNSecurityTables*.cfg cases set CHECK_DEADLOCK FALSE
under deadlock = check (21 at the review's pin; main added two). They are listed
by name in DEADLOCK_OWED, so the check stays green.

When R flips their column in CASES.tsv to ignore-terminal, the same change must
delete those names from DEADLOCK_OWED. The checker refuses an entry whose cfg and
column agree, so the list cannot outlive the debt. Flipping the column changes
the cases' fingerprints, so the rnsecuritytables group's records must be run
again in that change too.

## MA006: witnesses the delivery-assumption check now asks for

The check now follows the operators that a Fairness... definition names. It finds
four more modules with a delivery assumption. Their missing witnesses are listed
in OWED_WITNESSES, each with its owner:
- RNWindow.tla: owed to M3, the MCRNWindow temporal witness.
- Link.tla, ClientConnection.tla, ServerConnection.tla: owed to MM210, a
  reversed witness for each PROPERTY.

When an owner lands its witnesses, the same change deletes that module's entry.
The checker refuses an entry once no case that reaches the module lacks a
witness, and also refuses a module it no longer finds.
