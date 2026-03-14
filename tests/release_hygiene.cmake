get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(required_files
    "${REPO_ROOT}/LICENSE"
    "${REPO_ROOT}/docs/v0.0.1-roadmap.md"
    "${REPO_ROOT}/docs/v0.0.1-release-notes.md"
    "${REPO_ROOT}/docs/zoo-keeper-metal-oom-issue.md"
)

foreach(required_file IN LISTS required_files)
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Missing required release artifact: ${required_file}")
    endif()
endforeach()

file(READ "${REPO_ROOT}/README.md" README_CONTENTS)

set(required_readme_text
    "OpenAI-compatible subset"
    "docs/v0.0.1-roadmap.md"
    "docs/v0.0.1-release-notes.md"
    "docs/zoo-keeper-metal-oom-issue.md"
)

foreach(required_text IN LISTS required_readme_text)
    string(FIND "${README_CONTENTS}" "${required_text}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR "README.md is missing required text: ${required_text}")
    endif()
endforeach()

string(FIND "${README_CONTENTS}" "OpenAI-compatible REST API" stale_claim_at)
if(NOT stale_claim_at EQUAL -1)
    message(FATAL_ERROR "README.md still contains the stale full-compatibility claim")
endif()
