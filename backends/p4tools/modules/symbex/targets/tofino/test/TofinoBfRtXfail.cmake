# XFAILS: tests that *temporarily* fail under the BFRT test backend.
# =================================================================
#
# Populate this list with reasons (substring match against test stderr) for
# tests known to fail with --test-backend BFRT on Tofino/TNA. See the sibling
# TofinoPTFXfail.cmake for example entries.
#
# Entries take the form:
#   p4tools_add_xfail_reason(
#     "symbex-tofino-bfrt"
#     "<substring of failure message>"
#   )
