# Firmware attribution and source identity

The project-owned [identity record](../firmware/project_identity.json) defines the
developer (`from1to9`) and canonical HTTPS repository URL. Forks change this
record; builds never derive attribution from a local Git author, workstation
account or possibly credential-bearing remote URL. Values are bounded printable
ASCII for the current English UI. Invalid metadata fails the build.

The supported attribution format is deliberately narrower than arbitrary URLs:
developer names contain 1..31 printable ASCII characters with no outer spaces;
repository URLs contain at most 95 ASCII characters, start with lowercase
`https://`, and use a DNS host with at least two nonempty labels. Each label is
1..63 letters, digits or hyphens, with no leading or trailing hyphen. An optional
1..5 digit decimal port must be in 1..65535. A slash and nonempty repository path
are required. The path accepts RFC 3986 `pchar`, slash and valid two-hex-digit
percent escapes. User information, queries, fragments, raw whitespace,
backslashes, IPv6 literals and single-label hosts are unsupported. The build
generator and About validator enforce the same format; valid explicit ports and
escaped paths remain visible rather than silently becoming Unavailable.

Every firmware build runs [the identity generator](../tools/build_identity.py)
before compiling the application, including incremental builds. It records the
actual 40-digit Git HEAD and tracked, submodule and non-ignored untracked change
state. An unchanged result preserves the generated header's timestamp. Generated
files stay under the build directory and are not committed.

About displays the developer, repository URL and a 12-digit Git abbreviation
with Clean, Modified or Unknown. Startup diagnostics retain the full commit.
Firmware is the project version; Build remains the ELF fingerprint. Neither is
relabeled as a Git commit. The same generated metadata supplies UI and logs.

An archive, an enclosing unrelated repository, an unavailable Git executable or
failed status check cannot establish a clean source state. A valid HEAD may be
retained when only the status check fails, with Unknown state. Modified means
the image includes a locally changed checkout relative to the displayed HEAD;
it does not identify an unpublished final tree. Git state does not describe
ignored SDK configuration, build flags or toolchain patches, so the ELF build
fingerprint and release manifest remain necessary to identify exact artifacts.

Metadata is immutable for the running image. Publishing source after building
does not rewrite its identity; a subsequent build is required for the new
commit and state to appear. The generator records build-time state, not a claim
that GitHub contains the commit or that a physical device is running that image.

Validation covers real temporary Git history transitions, tracked/untracked
changes, detached HEAD, unchanged output, unavailable Git, archive isolation,
status failure and invalid attribution without partial overwrite. Native UI
renders and hit tests must establish readable full canonical repository text,
unknown states, both themes and unchanged main workspaces. Target compilation,
stack/resource checks and actual About inspection remain separate evidence.
