# Pinned SDK patch policy regressions

Run `python3 tests/toolchain/test_idf_patches.py`. The isolated Git fixtures
exercise the actual setup/build patch verifier, including exact preimage apply,
postimage verification/idempotence, known partial recovery, check-only behavior,
wrong pin/digest rejection and preservation of unrelated, unknown and staged
work. The unchanged BSP verifier must still reject every tracked modification.
No shared SDK or BSP checkout is changed by this suite.
