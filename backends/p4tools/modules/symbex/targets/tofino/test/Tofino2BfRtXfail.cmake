# XFAILS: tests that *temporarily* fail under the BFRT test backend on Tofino2/T2NA.
# =================================================================================
#
# Populate this list with reasons (substring match against test stderr) for
# tests known to fail with --test-backend BFRT on Tofino2/T2NA. See the sibling
# Tofino2PTFXfail.cmake for example entries.
#
# Entries take the form:
#   p4tools_add_xfail_reason(
#     "symbex-tofino2-bfrt"
#     "<substring of failure message>"
#   )
