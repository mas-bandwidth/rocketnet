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
