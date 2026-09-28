Title: Allow an explicit address-space budget for large SXI builds

The 466 build aborts during dictionary-SA allocation because every pipeline
child inherits a hard 149 GB virtual-memory ceiling. Its 18,611,972,670-entry
64-bit SA requires 148,895,781,360 bytes while the dictionary and its auxiliary
structures are already allocated. A copied-input run reproduces `bad_alloc`
at exactly that request; the same executable accepts the request with an
850 GB ceiling. No dictionary-length truncation causes this failure.

Add `xsa build --address-space-gb N`, validate the byte conversion, preserve
the current 149 GB default, and clamp the requested ceiling to any lower
inherited hard limit. Record the effective ceiling in the provenance journal.
Include the option in the packaged pipeline and update its source hashes.

Validation:

- Reproduced the 466 allocation failure under the original 149 GB limit:
  exit 134, 2:06.71 wall, 36,347,904 KiB peak RSS.
- The same 148,895,781,360-byte request succeeds under an 850 GB limit with
  the identical r-pfbwt executable.
- Real tiny-build regression covers default, explicit, and inherited limits,
  rejects invalid values, and compares final index bytes: PASS, chi=1401.
- Packaged release build: PASS.
- Retained yeast235: `.rlebwt`, metadata, `.ssa`, and `.ssa_t` byte-identical;
  10:02.11 wall, 8,820,112 KiB peak RSS. k10 comparison pending.
- Full retained-466 validation (frontend, endpoints, slim, sweep, sampled AGC
  audit, writer, container validation): pending; not the from-zero milestone.

This belongs in SXI's pipeline, not upstream r-pfbwt. The requested 64-bit
upstream patch is unsupported by the reproduced failure and has intentionally
not been invented. r-pfbwt and its PFP-DS/gSACA-K sources are unchanged apart
from the previously existing endpoint tap. No commits or PRs were created.
