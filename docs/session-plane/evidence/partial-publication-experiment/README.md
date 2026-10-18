# Rejected receipt-publication experiment

Seven complete headed receipts and the exact rejected source delta are retained,
single-box, proof=false. The [index](receipts.json) includes every RED.

The experimental detector rejects a majority-received source while another
playing reader lacks it: RED on `8145319a`. Requiring every reader's receipt
and excluding an incomplete sender makes that detector GREEN on `62210f1e`.
It does not cure the original integrated row: history still differs at 349,
later at 6129 ms, with the opposite held/unheld observations. Existing remote
receipt and neutral deadline controls also fail. Neutral replay and active
phase controls pass. Neither a complete fix nor a gate pass is claimed.

The entire candidate, including its new detector, was reverted. The existing
receipt controls and quorums remain unchanged in the accepted reader-link
correction. The archived patch contains only the rejected experiment.
