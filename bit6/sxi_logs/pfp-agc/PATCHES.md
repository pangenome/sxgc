# Reviewable patch artifacts

`series` contains the upstream PFP patch sequence (one complete patch,
`pfp-agc.patch`). Base: `1a5f114ae026c18e7c0049ceace1a5eabc8be44a`.
The patch includes all eleven changed/new files; no commit or staging is needed.
`patch-verification.json` records clean application to a fresh base export,
exact equality with the tested fork, and the patch SHA-256.
`PR-DESCRIPTION.md` is the upstream PR draft; no remote PR was created.

The separate downstream SXI integration is `pipeline.patch`, relative to the
saved pre-lane `sxi_pipeline.before.py`; it includes `tools/build_pfp_agc.sh`.
`pipeline-from-main.patch` is an alternative against the home SXI snapshot.
Apply only the appropriate downstream alternative, not both. The upstream PR
does not include downstream construction, format, endpoint, or query changes.

External reviewer approval remains required before upstream acceptance.
