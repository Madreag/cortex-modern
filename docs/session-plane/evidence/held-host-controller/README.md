# Held host controller divergence

The native Linux control is a two-peer headed match on a measured 30 ms fake
link, with the official runner stalling the host simulation for 2000 ms at tick
600. The agreed hold begins at 606 and the AI handoff is 727. All simdump rows
agree through 726. At 727 the host's Brain Robot 1048577 and Green Dummy 1048615
are enabled on the host and disabled on the client. At 728 the Brain Robot's
front arm 1048586, rifle 1048587, magazine 1048588 and muzzle 1048589 differ.
Both controllers have AI mode, player -1 and neutral command state at the
handoff. The first physical difference follows the disabled-state difference;
it is not evidence of human input applying on just one peer.

`native-red.json` retains the complete actor/controller records for those two
actors at 726–728, each controller phase, the first two complete differing
simdump ticks, source/binary provenance and the runner manifest. Every simdump
field was compared, with no exclusions. The original compressed full dumps and
logs remain in its named scratch path. The controller checksum hashes mode,
player and input but not the quick-disabled flag, so a controller disable
divergence can first appear in the attachable checksum.

`IsPeerGoneAtFrame` handles a historical held host as a live hub, but its latest
held state falls through to `m_PeerLeaveFrames` and reads gone. A local host's
shortcut can instead read live. `ApplyLockstepLeaveHandoffsImpl` hands the actor
to AI and then uses that gone reading to disable it. A future return makes the
held state historical and changes the answer before the return's frame.
The new detector drives the actual actor handoff with and without that future
return, preserves both initial disable states, and retains negatives for an
explicit host departure and a held client.

The field logs independently retain the distinguishing arrival order. In the
first host hold, AI starts at 42286; the host learns return 42346 while displaying
42306 and the joiner while displaying 42320. For the second, AI starts at 49617;
return 49668 is learned at 49630/49642. For the third, AI starts at 57175; the
host already knows return 57209 at 57170, while the joiner learns it at 57183.
The recorded-hold path keeps a newer return and stages only historical Held
metadata on that host, while the joiner still has the current held metadata.
The first field physical difference is attachables at 57176. These exact
network events, line numbers and log digests are in
`field-hold-arrival-order.json`.

The field did not record per-object state or controller-production/application
traces. Its one-way attachable checksum cannot identify the exact limb or UID.
The local Brain Robot UIDs above belong only to the reproducing control. The
field's exact object remains a lead-owned evidence request, not an inferred UID.
