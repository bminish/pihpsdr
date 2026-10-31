# Settled decisions

Part of the local-change register: [changes.md](../changes.md).

These have been argued through and measured. Changing one needs a
capture that shows the current rule failing, not a new line of
reasoning.

1. **No hang, no timeout, in any reference.**
   - **RADE V1: a new lock replaces an old one.** Nothing else ends a
     lock, short of a retune or other context change. While the pilot is
     absent the lock and the weight are held, and the averages age at
     the Averaging time. The resync search (LC-010) takes a new station
     as soon as it finds one. RADE V1 has no Min coherence either: the
     pilot gates already do that job (LC-016).
   - **Every other reference: the correlation is the arbiter.** The
     averages update every block, the coherence gate decides whether the
     result is applied, and otherwise the last weight is held. Averaging
     decides how fast old data is forgotten. Hang time has no part to
     play.
   - **Why:** measured on `TEST` and scored on decode (LC-014), a short
     timer drops locks that would have recovered: 51 synced frames lost
     on the one marginal capture. A long one does nothing, or costs a
     little. A timer never helped.
2. **Hold good solutions through fades.** When there's nothing new to
   correlate on, keep the weight where it was: it's the best chance of
   being right when the signal returns (LC-012, LC-014).
3. **Don't add mechanisms for sub-1 % cases.** A new RADE station
   landing within 4 samples of the old one's timing (0.9 % of
   changeovers) is taken for the old one returning. If its pilot is
   strong enough the tracker follows it and Averaging corrects the
   weight; otherwise the old weight stays. We accept that. A timeout, or
   a rule to catch it, costs more than the case does.
4. **Averages age at the Averaging time, whether or not a gate accepts
   the block.** Otherwise, under a gate that opens only some of the
   time, the average's real age grows well past the Averaging time just
   when conditions are hardest, and a new signal is averaged into data
   from one that ended long ago. Window, Carrier and FSK/Digital update
   every block anyway. RADE V1 ages through a freeze (LC-010). CW ages
   every bin in its region every block (LC-017).
5. **No empty-band stand-down (2026-10-14).** The combiner does not slew
   the weight to zero when both arms carry nothing; it holds, like every
   other time there is nothing new to correlate on. On `TEST` Level output
   already keeps the output at arm 0's level in dead air, and the flat Sum
   weight shrinks by itself there (LC-012), so the feature branch's 12 dB
   between overs is not found: 18 of 27 captures with an empty stretch are
   within 0.5 dB of arm 0, and four more are below it (T-026). The stand-down needs a 2 s timer
   (decision 1) and assumes arm 0 is the good antenna, which a capture with
   a dead arm 0 showed false (26 to 35 dB below the live antenna).
   **Changing it needs** a capture where the held weight costs several dB in
   dead air with Level output on.

